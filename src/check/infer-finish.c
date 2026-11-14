#include "check/infer.h"
#include "check/signatures.h"

#include "base/capabilities.h"
#include "base/codes.h"

#include <stdio.h>
#include <string.h>

MO_MUST_USE const struct mo_source_type *infer_finish_type(struct inference *p, struct term *t,
                                                           uint32_t depth) {
    if (!infer_work(p) || !infer_type_depth(p, depth))
        return NULL;
    t = infer_root(t);
    if (t->resolved != NULL) {
        if (!infer_type_depth(p, depth + t->height))
            return NULL;
        return t->resolved;
    }
    if (t->variable) {
        if (t->numeric == 0) {
            bool reported = infer_problem(
                p, MO_CODE_CANNOT_INFER, p->span, "no use decides this expression's type",
                "A function's signature gives its types; inside a body types "
                "are inferred from all their uses (reference 5.1).",
                NULL);
            (void)reported;
            return NULL;
        }
        t->variable = false;
        t->partial_default = p->partial;
        t->shape = (struct mo_source_type){.kind = MO_SOURCE_NAMED,
                                           .module = UINT32_MAX,
                                           .name = (t->numeric & INT) != 0 ? "Int" : "Decimal"};
    }
    const struct mo_source_type **args =
        t->shape.count == 0 ? NULL
                            : infer_allocate(p, t->shape.count, sizeof(*args),
                                             _Alignof(const struct mo_source_type *));
    if (p->failed)
        return NULL;
    for (uint32_t i = 0; i < t->shape.count; i++) {
        args[i] = infer_finish_type(p, t->arguments[i], depth + 1);
        if (args[i] == NULL)
            return NULL;
        t->partial_default = t->partial_default || infer_root(t->arguments[i])->partial_default;
        uint32_t height = infer_root(t->arguments[i])->height + 1;
        if (height > t->height)
            t->height = height;
    }
    struct mo_source_type *out =
        infer_allocate(p, 1, sizeof(*out), _Alignof(struct mo_source_type));
    if (out != NULL) {
        *out = t->shape;
        out->arguments = args;
        out->height = t->height;
        out->parameters =
            out->kind == MO_SOURCE_PARAMETER ? UINT32_C(1) << (unsigned)(out->name[0] - 'A') : 0;
        for (uint32_t i = 0; i < out->count; i++)
            out->parameters |= args[i]->parameters;
        t->resolved = out;
    }
    return out;
}

static MO_MUST_USE bool finite_integer(const struct mo_syntax *node, unsigned base) {
    uint32_t bits = 0;
    bool smaller = false;
    for (uint32_t i = 2; i < node->span.end - node->span.start; i++) {
        uint8_t c = node->text[i];
        if (c == '_')
            continue;
        unsigned digit = c >= 'a'   ? (unsigned)(c - 'a') + 10
                         : c >= 'A' ? (unsigned)(c - 'A') + 10
                                    : (unsigned)(c - '0');
        for (unsigned j = base == 16 ? 4u : 1u; j != 0; j--) {
            unsigned bit = (digit >> (j - 1)) & 1u;
            if (bits == 0 && bit == 0)
                continue;
            bits++;
            if (bits <= 54 && bit == 0)
                smaller = true;
            if (bits > 1024)
                return false;
        }
    }
    /* Nearest-even overflows at 2^1024 - 2^970: 54 leading ones,
     * then 970 zeros. No floating arithmetic is needed to test this boundary. */
    return bits < 1024 || smaller;
}

static MO_MUST_USE bool finite_decimal(const struct mo_syntax *node) {
    /* Exact nearest-even overflow boundary, 2^1024 - 2^970. It is an
     * integer: fractions cannot bridge the gap from an integer below it.
     * Comparing digits avoids both the host locale and floating rounding mode. */
    static const char boundary[] =
        "179769313486231580793728971405303415079934132710037826936173778980444968292764750946649"
        "017977587207096330286416692887910946555547851940402630657488671505820681908902000708383"
        "676273854845817711531764475730270069855571366959622842914819860834936475292719074168444"
        "365510704342711559699508093042880177904174497792";
    size_t length = node->span.end - node->span.start, digits = 0, first = 0;
    for (size_t i = 0; i < length && node->text[i] != '.'; i++) {
        uint8_t c = node->text[i];
        if (c == '_' || (digits == 0 && c == '0'))
            continue;
        if (digits == 0)
            first = i;
        digits++;
    }
    if (digits != sizeof(boundary) - 1)
        return digits < sizeof(boundary) - 1;
    size_t at = 0;
    for (size_t i = first; i < length && node->text[i] != '.'; i++) {
        uint8_t c = node->text[i];
        if (c == '_')
            continue;
        MO_ASSERT(at < sizeof(boundary) - 1);
        if (c != (uint8_t)boundary[at])
            return c < (uint8_t)boundary[at];
        at++;
    }
    return false;
}

/* A Number parameter is instantiated as Int, BigInt, Decimal and Float.
 * The generic body is checked once, so a literal in that context must fit Int. */
static MO_MUST_USE bool number_parameter(const struct inference *p,
                                         const struct mo_source_type *type) {
    if (type->kind != MO_SOURCE_PARAMETER || type->declaration != p->function->function ||
        type->name[0] < 'A' || type->name[0] > 'Z' || type->name[1] != '\0')
        return false;
    return (p->function->requirements[(unsigned)(type->name[0] - 'A')] &
            (1u << MO_SOURCE_NUMBER)) != 0;
}

static MO_MUST_USE bool literal(struct inference *p, const struct note *n) {
    const struct mo_syntax *node = n->output.syntax;
    if (node->kind != MO_S_INT && node->kind != MO_S_DECIMAL)
        return true;
    const char *name = n->output.type->name;
    if (strcmp(name, "BigInt") == 0)
        return true;
    size_t length = node->span.end - node->span.start;
    mo_u128 magnitude = 0;
    unsigned base = length > 2 && node->text[1] == 'x'   ? 16
                    : length > 2 && node->text[1] == 'b' ? 2
                                                         : 10;
    uint32_t places = 0;
    bool fraction = false, overflow = false;
    for (size_t i = base == 10 ? 0 : 2; i < length; i++) {
        uint8_t c = node->text[i];
        if (c == '_')
            continue;
        if (c == '.') {
            fraction = true;
            continue;
        }
        unsigned digit = c >= 'a'   ? (unsigned)(c - 'a') + 10
                         : c >= 'A' ? (unsigned)(c - 'A') + 10
                                    : (unsigned)(c - '0');
        MO_ASSERT(digit < base);
        if (magnitude > (~(mo_u128)0 - digit) / base)
            overflow = true;
        else if (!overflow)
            magnitude = magnitude * base + digit;
        if (fraction)
            places++;
    }
    bool fits = !overflow;
    if (strcmp(name, "Int") == 0 || number_parameter(p, n->output.type))
        fits = fits && magnitude <= (mo_u128)INT64_MAX + (n->negative ? 1u : 0u);
    else if (strcmp(name, "Byte") == 0)
        fits = fits && !n->negative && magnitude <= 255;
    else if (strcmp(name, "Decimal") == 0) {
        mo_u128 maximum = ((mo_u128)1 << 127) - 1u;
        fits = fits && places <= 18;
        for (uint32_t i = places; fits && i < 18; i++) {
            if (magnitude > maximum / 10)
                fits = false;
            else
                magnitude *= 10;
        }
        fits = fits && magnitude <= maximum;
    } else if (strcmp(name, "Float") == 0)
        fits = base == 10 ? finite_decimal(node) : finite_integer(node, base);
    return fits || infer_problem(p, MO_CODE_LITERAL_RANGE, node->span,
                                 "a number literal doesn't fit its type",
                                 "A number literal takes its context's numeric type and must fit "
                                 "that type's range (reference 3.3).",
                                 NULL);
}

MO_MUST_USE bool infer_finish_notes(struct inference *p, struct mo_inferred_expression *items) {
    uint32_t i = 0;
    for (struct note *n = p->notes; n != NULL; n = n->next) {
        p->span = n->output.syntax->span;
        if (n->callee != NULL) {
            n->output.symbol = n->callee->output.symbol;
            if (n->graph != NULL && n->callee->graph != NULL) {
                struct emitted *callee = n->callee->graph;
                n->graph->output.symbol = callee->output.symbol;
                n->graph->function_type = callee->function_type;
                n->graph->requirements = callee->requirements;
                n->graph->parameters = callee->parameters;
            }
        }
        if (p->partial && p->span.start >= p->error.primary.span.start)
            continue;
        n->output.type = infer_finish_type(p, n->type, 0);
        if (n->output.type != NULL) {
            bool valid = literal(p, n);
            (void)valid;
            if (!p->fatal && n->discarded && !infer_named(n->type, "Unit"))
                infer_defer(
                    p,
                    infer_named(n->type, "Result") ? MO_CODE_FAILURE_IGNORED : MO_CODE_UNUSED_VALUE,
                    p->span,
                    infer_named(n->type, "Result") ? "a failure is ignored" : "a result isn't used",
                    infer_named(n->type, "Result")
                        ? "Mo never drops a failure silently (reference 8.3)."
                        : "A result that isn't Unit and isn't used is an error "
                          "(reference 6.1).");
        }
        if (p->fatal)
            return false;
        p->failed = false;
        if (items != NULL)
            items[i++] = n->output;
    }
    for (struct local *l = p->all_locals; l != NULL; l = l->next) {
        p->span = l->name->span;
        if ((p->partial && p->span.start >= p->error.primary.span.start) ||
            l->name->text[l->name->span.end - l->name->span.start - 1] != '?')
            continue;
        const struct mo_source_type *type = infer_finish_type(p, l->type, 0);
        if (p->fatal)
            return false;
        p->failed = false;
        if (type == NULL)
            continue;
        if (type->kind == MO_SOURCE_FUNCTION)
            type = type->arguments[type->count - 1];
        if (type->kind != MO_SOURCE_NAMED || type->module != UINT32_MAX ||
            strcmp(type->name, "Bool") != 0)
            infer_defer(
                p, MO_CODE_BOOL_NAME, p->span, "a name ending in ? isn't a yes-or-no answer",
                "A name ending in ? is a Bool, or a function that returns one (reference 2.3).");
    }
    MO_ASSERT(p->partial || i == p->count);
    return p->error.code == 0;
}

static MO_MUST_USE bool capture_type(struct inference *p, const struct mo_source_type *type,
                                     bool *capability, uint32_t *unknown, uint32_t depth) {
    if (!infer_work(p) || !infer_type_depth(p, depth))
        return false;
    if (type->kind == MO_SOURCE_FUNCTION)
        return true;
    if (type->kind == MO_SOURCE_PARAMETER) {
        const struct mo_function_signature *fn =
            mo_function_signature_by_symbol(p->signatures, type->declaration);
        unsigned letter = (unsigned)(type->name[0] - 'A');
        MO_ASSERT(letter < 26);
        /* Number is native-only. Custom Order/Text (including implied Equal)
         * do not establish that a nominal value contains no capabilities. */
        if (fn == NULL || (fn->requirements[letter] & (1u << MO_SOURCE_NUMBER)) == 0)
            *unknown |= UINT32_C(1) << letter;
        return true;
    }
    if (type->kind == MO_SOURCE_NAMED && type->declaration != NULL) {
        const struct mo_nominal_signature *owner =
            mo_nominal_signature_by_symbol(p->signatures, type->declaration);
        MO_ASSERT(owner != NULL);
        *capability = *capability || owner->holds_capability;
        for (uint32_t i = 0; i < type->count; i++)
            if ((owner->capture_parameters & owner->type->arguments[i]->parameters) != 0 &&
                !capture_type(p, type->arguments[i], capability, unknown, depth + 1))
                return false;
        return true;
    }
    for (uint32_t i = 0; i < MO_CAPABILITY_COUNT; i++)
        if (type->kind == MO_SOURCE_NAMED && type->module == UINT32_MAX &&
            strcmp(type->name, mo_capability_names[i]) == 0)
            *capability = true;
    for (uint32_t i = 0; i < type->count; i++)
        if (!capture_type(p, type->arguments[i], capability, unknown, depth + 1))
            return false;
    return true;
}

static MO_MUST_USE bool capture_assignment(struct inference *p, const struct emitted_lambda *lambda,
                                           const struct capture *capture) {
    if (!capture->local->variable)
        return true;
    for (const struct capture_write *w = capture->local->writes; w != NULL; w = w->next) {
        if (!infer_work(p))
            return false;
        bool repeated = false;
        for (const struct capture_loop *loop = lambda->loop; loop != NULL; loop = loop->parent) {
            if (!infer_work(p))
                return false;
            struct mo_span span = loop->syntax->span;
            repeated = repeated ||
                       (capture->local->name->span.start < span.start &&
                        w->syntax->span.start >= span.start && w->syntax->span.start < span.end);
        }
        /* Assignment takes effect after its RHS, which may make this lambda. */
        if (!repeated && w->syntax->span.end <= lambda->syntax->span.start)
            continue;
        bool exclusive = false;
        if (!repeated && w->path != NULL)
            for (const struct capture_path *a = lambda->path; a != NULL; a = a->parent)
                for (const struct capture_path *b = w->path; b != NULL; b = b->parent) {
                    if (!infer_work(p))
                        return false;
                    exclusive = exclusive || (a->group == b->group && a->arm != b->arm);
                }
        if (exclusive)
            continue;
        return infer_problem(p, MO_CODE_CAPTURE_ASSIGNMENT, capture->use,
                             "an anonymous function reads a var assigned after it is made",
                             "Copy the value into a name first; later rounds of an enclosing loop "
                             "also count as later assignments (reference 5.6).",
                             w->target);
    }
    return true;
}

MO_MUST_USE bool infer_finish_captures(struct inference *p) {
    for (struct emitted_lambda *lambda = p->lambdas; lambda != NULL; lambda = lambda->next) {
        for (struct capture *c = lambda->captures; c != NULL; c = c->next) {
            p->span = c->use;
            if (p->partial && c->use.start >= p->error.primary.span.start)
                continue;
            const struct mo_source_type *type = c->local->binding->output.type;
            if (type == NULL) {
                MO_ASSERT(p->error.code != 0);
                continue;
            }
            bool capability = false;
            uint32_t unknown = 0;
            if (!capture_type(p, type, &capability, &unknown, 0))
                return false;
            if (capability) {
                bool reported =
                    infer_problem(p, MO_CODE_CAPABILITY_CAPTURE, c->use,
                                  "an anonymous function can't capture a capability",
                                  "Anonymous functions can't capture capabilities, or values "
                                  "holding them (reference 11.4).",
                                  c->local->name);
                (void)reported;
            } else {
                /* An abstract captured value holds a capability only where the generic function
                 * is used; mo_check_capture_arguments checks every use. */
                p->capture_free |= unknown;
                if (!capture_assignment(p, lambda, c) && p->fatal)
                    return false;
            }
            if (p->fatal)
                return false;
            p->failed = false;
        }
    }
    return p->error.code == 0;
}

/* Check only complete, independently typed matches before any retained error.
 * Later failures inside an arm must not invent a coverage error for that match.
 * False means coverage already emitted a resource diagnostic. */
MO_MUST_USE bool infer_finish_patterns(struct inference *p) {
    /* Byte is a small finite literal domain. Normalize spelling only after all
     * uses and literal range checks have fixed the contextual numeric type. */
    for (struct typed_pattern *at = p->patterns; at != NULL; at = at->next) {
        const struct mo_syntax *node = at->syntax;
        if (p->error.code != 0 && node->span.end > p->error.primary.span.start)
            continue;
        if (node->kind != MO_S_INT || !infer_named(at->type, "Byte"))
            continue;
        uint32_t length = node->span.end - node->span.start;
        uint32_t base = length > 2 && node->text[1] == 'x'   ? 16u
                        : length > 2 && node->text[1] == 'b' ? 2u
                                                             : 10u;
        uint32_t value = 0;
        for (uint32_t j = base == 10 ? 0u : 2u; j < length; j++) {
            uint8_t c = node->text[j];
            if (c == '_')
                continue;
            uint32_t digit = c >= 'a'   ? (uint32_t)(c - 'a') + 10u
                             : c >= 'A' ? (uint32_t)(c - 'A') + 10u
                                        : (uint32_t)(c - '0');
            MO_ASSERT(digit < base && value <= (255u - digit) / base);
            value = value * base + digit;
        }
        char *name = infer_allocate(p, 16, 1, 1);
        if (name == NULL)
            return true;
        int length_written = snprintf(name, 16, "%u", value);
        MO_ASSERT(length_written > 0 && length_written < 16);
        at->match.key = 256u + value;
        at->match.signature = 256;
        at->match.name = name;
    }
    for (struct match_check *check = p->matches; check != NULL; check = check->next) {
        if (p->error.code != 0 && check->end > p->error.primary.span.start)
            continue;
        p->span = check->span;
        struct term *type = infer_root(check->type);
        /* Defaulting during a failed walk cannot certify a literal domain:
         * the later use deciding Byte versus Int may never have been reached. */
        if (type->variable || type->resolved == NULL || type->partial_default)
            continue;
        bool choice = type->shape.declaration != NULL &&
                      type->shape.declaration->declaration->kind == MO_S_ENUM;
        choice = choice || infer_named(type, "Option") || infer_named(type, "Result") ||
                 infer_named(type, "Ordering") || infer_named(type, "EntryKind") ||
                 infer_named(type, "FileError") || infer_named(type, "SimulationError");
        if (check->catch_all != NULL && choice) {
            infer_defer(p, MO_CODE_CASE_CATCH_ALL, check->catch_all->span,
                        "a choice arm is a catch-all",
                        "A whole arm over a choice type can't be _ or a name; "
                        "list the cases (reference 7.2).");
            continue;
        }
        const char *missing = NULL;
        if (!mo_patterns_cover(p->arena, p->interfaces, check->patterns, check->count,
                               type->resolved, &p->work, p->signatures->work, p->diags, check->span,
                               &missing))
            return false;
        if (missing != NULL) {
            char *message = infer_allocate(p, 8192, 1, 1);
            if (message == NULL)
                return true;
            int length =
                snprintf(message, 8192, "case doesn't cover every possible value; %smissing: %s",
                         check->count == 0 ? "guards don't count toward coverage; " : "", missing);
            MO_ASSERT(length > 0 && length < 8192);
            infer_defer(p, MO_CODE_CASE_INCOMPLETE, check->span, message,
                        "The arms must cover every possible value (reference 7.2).");
            if (p->error.code == MO_CODE_CASE_INCOMPLETE &&
                p->error.primary.span.start == check->span.start) {
                p->secondary = (struct mo_label){check->scrutinee->span, "matched value"};
                p->error.secondary = &p->secondary;
                p->error.secondary_count = 1;
            }
        }
    }
    return true;
}

MO_MUST_USE bool infer_finish_operator(struct inference *p, struct note *note) {
    struct emitted *graph = note->graph;
    enum mo_syntax_token op = graph->output.operation;
    const char *native = graph->output.native;
    bool text = native != NULL && strcmp(native, "to_text") == 0;
    bool compare = native != NULL && strcmp(native, "compare") == 0;
    if ((op == MO_T_EOF && !text && !compare) || op == MO_T_AND || op == MO_T_OR ||
        op == MO_T_IMPLIES || op == MO_T_NOT || op == MO_T_DOTDOT)
        return true;
    enum mo_source_ability ability =
        text                             ? MO_SOURCE_TEXT
        : op == MO_T_EQ || op == MO_T_NE ? MO_SOURCE_EQUAL
        : compare || op == MO_T_LT || op == MO_T_LE || op == MO_T_GT || op == MO_T_GE
            ? MO_SOURCE_ORDER
            : MO_SOURCE_NUMBER;
    /* A qualified ability function, as a value or called, proves its
     * ability on its first parameter; a dot call on its receiver. */
    bool qualified = (text || compare) &&
                     (graph->output.operand_count == 0 || graph->output.receiver == UINT32_MAX);
    MO_ASSERT(qualified || (graph->output.operand_count != 0 && graph->operands[0] != NULL));
    struct note *operand = graph->output.operand_count == 0 ? note : graph->operands[0];
    const struct mo_source_type *type = operand->output.type;
    if (qualified) {
        MO_ASSERT(type->kind == MO_SOURCE_FUNCTION && type->count != 0);
        type = type->arguments[0];
    }
    p->span = note->output.syntax->span;
    const struct mo_module_scope *module = NULL;
    for (const struct ability_module *at = p->ability_modules; at != NULL; at = at->next) {
        if (!infer_work(p))
            return false;
        if (at->graph == graph)
            module = at->module;
    }
    if (module != NULL && (type->kind != MO_SOURCE_NAMED || type->declaration == NULL ||
                           &p->interfaces->scopes[type->module] != module)) {
        char *label = infer_allocate(p, 256, 1, 1);
        if (label == NULL)
            return false;
        int length = snprintf(label, 256, "%.100s doesn't declare %.100s", module->name,
                              type->kind == MO_SOURCE_FUNCTION ? "functions"
                              : type->kind == MO_SOURCE_TUPLE  ? "tuples"
                                                               : type->name);
        MO_ASSERT(length > 0 && length < 256);
        p->type_label = label;
        return infer_problem(
            p, MO_CODE_ARGUMENT_TYPE, p->span,
            "a module's to_text or compare is given a type it doesn't declare",
            "M.to_text(x) and M.compare(a, b) take a value of a type that the module M "
            "declares (reference 5.4).",
            NULL);
    }
    if (ability == MO_SOURCE_NUMBER && type->kind == MO_SOURCE_NAMED &&
        type->module == UINT32_MAX &&
        (strcmp(type->name, "Time") == 0 || strcmp(type->name, "Duration") == 0))
        return true;
    const struct mo_ability_implementation *implementation =
        mo_prove_ability(&p->abilities, type, ability);
    if (implementation == NULL)
        return infer_problem(p, p->abilities.error, p->span, p->abilities.message,
                             p->abilities.reason, NULL);
    struct mo_checked_ability_use *use =
        infer_allocate(p, 1, sizeof(*use), _Alignof(struct mo_checked_ability_use));
    if (p->failed)
        return false;
    *use = (struct mo_checked_ability_use){.operand = operand->index,
                                           .implementation = implementation};
    graph->output.ability_count = 1;
    graph->output.abilities = use;
    return true;
}

MO_MUST_USE bool infer_finish_interpolation(struct inference *p, struct note *note) {
    if (note->output.syntax->kind != MO_S_TEXT_INTERPOLATED)
        return true;
    struct emitted *graph = note->graph;
    uint32_t count = graph->output.operand_count;
    struct mo_checked_ability_use *uses =
        infer_allocate(p, count, sizeof(*uses), _Alignof(struct mo_checked_ability_use));
    if (uses == NULL)
        return false;
    for (uint32_t i = 0; i < count; i++) {
        struct note *hole = graph->operands[i];
        const struct mo_ability_implementation *proof =
            mo_prove_ability(&p->abilities, hole->output.type, MO_SOURCE_TEXT);
        if (proof == NULL)
            return infer_problem(p, p->abilities.error, hole->output.syntax->span,
                                 p->abilities.message, p->abilities.reason, NULL);
        uses[i] = (struct mo_checked_ability_use){.operand = hole->index, .implementation = proof};
    }
    graph->output.ability_count = count;
    graph->output.abilities = uses;
    return true;
}

MO_MUST_USE bool infer_finish_function_use(struct inference *p, struct emitted *graph) {
    const struct mo_source_type *type = graph->function_type;
    if (type == NULL)
        return true;
    uint32_t count = 0, proof_count = 0;
    for (unsigned i = 0; i < 26; i++) {
        count += (type->parameters & (UINT32_C(1) << i)) != 0 ? 1u : 0u;
        for (unsigned j = 0; j < 5; j++)
            proof_count += (graph->requirements[i] & (1u << j)) != 0 ? 1u : 0u;
    }
    struct mo_type_argument *arguments =
        infer_allocate(p, count, sizeof(*arguments), _Alignof(struct mo_type_argument));
    struct mo_checked_ability_use *proofs =
        infer_allocate(p, proof_count, sizeof(*proofs), _Alignof(struct mo_checked_ability_use));
    if (p->failed)
        return false;
    uint32_t at = 0, proof = 0;
    for (unsigned i = 0; i < 26; i++) {
        uint32_t bit = UINT32_C(1) << i;
        if ((type->parameters & bit) == 0)
            continue;
        const struct mo_source_type *actual = infer_finish_type(p, graph->parameters[at], 0);
        if (actual == NULL)
            return false;
        arguments[at++] = (struct mo_type_argument){infer_parameter_type(type, bit), actual};
        for (unsigned j = 0; j < 5; j++) {
            if ((graph->requirements[i] & (1u << j)) == 0)
                continue;
            const struct mo_ability_implementation *implementation =
                mo_prove_ability(&p->abilities, actual, (enum mo_source_ability)j);
            if (implementation == NULL)
                return infer_problem(p, p->abilities.error, p->span, p->abilities.message,
                                     p->abilities.reason, NULL);
            proofs[proof++] =
                (struct mo_checked_ability_use){.operand = UINT32_MAX,
                                                .implementation = implementation,
                                                .parameter = arguments[at - 1].parameter};
        }
    }
    graph->output.arguments = (struct mo_type_arguments){count, arguments};
    graph->output.ability_count = proof_count;
    graph->output.abilities = proofs;
    return true;
}

MO_MUST_USE bool infer_type_arguments(struct inference *p, const struct mo_source_type *generic,
                                      const struct mo_source_type *actual,
                                      struct mo_type_arguments *out) {
    if (generic->count == 0)
        return true;
    MO_ASSERT(actual->count == generic->count);
    struct mo_type_argument *items =
        infer_allocate(p, generic->count, sizeof(*items), _Alignof(struct mo_type_argument));
    if (items == NULL)
        return false;
    for (uint32_t i = 0; i < actual->count; i++)
        items[i] = (struct mo_type_argument){generic->arguments[i], actual->arguments[i]};
    *out = (struct mo_type_arguments){.count = actual->count, .items = items};
    return true;
}

MO_MUST_USE bool infer_data_requirements(struct inference *p,
                                         const struct mo_type_arguments *arguments, uint32_t *count,
                                         const struct mo_checked_ability_use **out) {
    uint32_t needed = 0;
    if (arguments->count == 0)
        return true;
    const struct mo_symbol *owner = arguments->items[0].parameter->declaration;
    const struct mo_nominal_signature *data = mo_nominal_signature_by_symbol(p->signatures, owner);
    if (data != NULL)
        needed = data->key_parameters;
    if (needed == 0)
        return true;
    uint32_t length = 0;
    for (uint32_t i = 0; i < arguments->count; i++)
        length += (needed & arguments->items[i].parameter->parameters) != 0 ? 2u : 0u;
    struct mo_checked_ability_use *proofs =
        infer_allocate(p, length, sizeof(*proofs), _Alignof(struct mo_checked_ability_use));
    if (proofs == NULL)
        return false;
    uint32_t at = 0;
    for (uint32_t i = 0; i < arguments->count; i++) {
        const struct mo_type_argument *argument = &arguments->items[i];
        if ((needed & argument->parameter->parameters) == 0)
            continue;
        for (unsigned j = MO_SOURCE_EQUAL; j <= MO_SOURCE_HASH; j++) {
            const struct mo_ability_implementation *proof =
                mo_prove_ability(&p->abilities, argument->type, (enum mo_source_ability)j);
            if (proof == NULL)
                return infer_problem(p, p->abilities.error, p->span, p->abilities.message,
                                     p->abilities.reason, NULL);
            proofs[at++] = (struct mo_checked_ability_use){
                .operand = UINT32_MAX, .implementation = proof, .parameter = argument->parameter};
        }
    }
    MO_ASSERT(at == length);
    *count = length;
    *out = proofs;
    return true;
}

static MO_MUST_USE const struct note *pattern_note(struct note *const *notes, uint32_t count,
                                                   const struct mo_syntax *syntax) {
    while (syntax->kind == MO_S_GROUP || syntax->kind == MO_S_BINDER)
        syntax = syntax->children[0];
    uintptr_t key = (uintptr_t)syntax;
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if ((uintptr_t)notes[mid]->output.syntax < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    MO_ASSERT(lo < count && notes[lo]->output.syntax == syntax);
    return notes[lo];
}

static MO_MUST_USE struct mo_checked_pattern *pattern_output(struct inference *p,
                                                             const struct mo_syntax *syntax,
                                                             const struct mo_source_type *type,
                                                             enum mo_checked_pattern_kind kind,
                                                             uint32_t count) {
    p->span = syntax->span;
    /* Checked output has ordinary node/arena budgets. Only the coverage
     * matrix charges bytes, to bound its retained combinatorial scratch. */
    if (!infer_work(p))
        return NULL;
    struct mo_checked_pattern *out =
        infer_allocate(p, 1, sizeof(*out), _Alignof(struct mo_checked_pattern));
    if (out != NULL)
        *out = (struct mo_checked_pattern){
            .kind = kind, .syntax = syntax, .type = type, .part_count = count};
    return out;
}

static MO_MUST_USE const struct mo_source_type *
pattern_field_type(struct inference *p, const struct mo_data_signature *data,
                   const struct mo_source_type *actual, uint32_t index) {
    struct term *parameters[26] = {NULL};
    for (uint32_t i = 0; i < actual->count; i++) {
        unsigned letter = (unsigned)(data->type->arguments[i]->name[0] - 'A');
        parameters[letter] = infer_annotation(p, actual->arguments[i], NULL, 0);
        if (parameters[letter] == NULL)
            return NULL;
    }
    struct term *type = infer_annotation(p, data->by_index[index]->type, parameters, 0);
    return type == NULL ? NULL : infer_finish_type(p, type, 0);
}

static MO_MUST_USE bool same_failure_type(struct inference *p, const struct mo_source_type *a,
                                          const struct mo_source_type *b, uint32_t depth) {
    if (!infer_work(p) || !infer_type_depth(p, depth))
        return false;
    if (a == b)
        return true;
    if (a->kind != b->kind || a->module != b->module || a->declaration != b->declaration ||
        a->count != b->count || strcmp(a->name, b->name) != 0)
        return false;
    for (struct type_pair *pair = p->equal_types; pair != NULL; pair = pair->next) {
        if (!infer_work(p))
            return false;
        if (pair->a == a && pair->b == b)
            return true;
    }
    for (uint32_t i = 0; i < a->count; i++)
        if (!same_failure_type(p, a->arguments[i], b->arguments[i], depth + 1))
            return false;
    struct type_pair *pair = infer_allocate(p, 1, sizeof(*pair), _Alignof(struct type_pair));
    if (pair == NULL)
        return false;
    *pair = (struct type_pair){a, b, p->equal_types};
    p->equal_types = pair;
    return true;
}

/* Candidates do not constrain unknown payloads until exactly one matches.
 * Repeated variables must agree, and shared type paths are visited once. */
static MO_MUST_USE bool failure_matches(struct inference *p, struct term *input,
                                        const struct mo_source_type *type,
                                        struct failure_guess **guesses, uint32_t depth) {
    if (!infer_work(p) || !infer_type_depth(p, depth))
        return false;
    input = infer_root(input);
    if (input->variable && input->numeric != 0 && infer_finish_type(p, input, 0) == NULL)
        return false;
    if (input->resolved != NULL)
        return same_failure_type(p, input->resolved, type, depth);
    for (struct failure_guess *at = *guesses; at != NULL; at = at->next) {
        if (!infer_work(p))
            return false;
        if (at->term == input)
            return same_failure_type(p, at->type, type, depth);
    }
    if (!input->variable) {
        const struct mo_source_type *shape = &input->shape;
        if (shape->kind != type->kind || shape->module != type->module ||
            shape->declaration != type->declaration || shape->count != type->count ||
            strcmp(shape->name, type->name) != 0)
            return false;
        for (uint32_t i = 0; i < type->count; i++) {
            struct term *part = infer_argument(p, input, i);
            if (part == NULL || !failure_matches(p, part, type->arguments[i], guesses, depth + 1))
                return false;
        }
    }
    struct failure_guess *guess =
        infer_allocate(p, 1, sizeof(*guess), _Alignof(struct failure_guess));
    if (guess == NULL)
        return false;
    *guess = (struct failure_guess){input, type, *guesses};
    *guesses = guess;
    return true;
}

MO_MUST_USE bool infer_finish_wrapping(struct inference *p) {
    for (struct failure_check *at = p->failures; at != NULL; at = at->next) {
        p->span = at->span;
        if (!infer_work(p))
            return false;
        if (at->op != MO_T_EOF || at->fallback != NULL || at->context == NULL ||
            infer_named(at->input, "Option"))
            continue;
        struct term *from = infer_argument(p, at->input, 1),
                    *to = infer_argument(p, at->context, 1);
        if (from == NULL || to == NULL)
            return false;
        if ((infer_root(from)->variable && infer_root(from)->numeric == 0) ||
            infer_root(to)->variable) {
            if (!infer_unify(p, from, to, 0))
                return false;
        }
        const struct mo_source_type *target = infer_finish_type(p, to, 0);
        if (target == NULL)
            return false;
        struct failure_guess *guesses = NULL;
        if (failure_matches(p, from, target, &guesses, 0)) {
            struct term *actual = infer_annotation(p, target, NULL, 0);
            if (actual == NULL || !infer_unify(p, from, actual, 0))
                return false;
            continue;
        }
        if (p->failed)
            return false;
        const struct mo_source_type *selected = NULL;
        uint32_t count = 0;
        if (target->declaration != NULL && target->declaration->declaration->kind == MO_S_ENUM) {
            const struct mo_symbol *owner = target->declaration;
            if (!infer_visible_data(p, owner))
                return false;
            const struct mo_module_scope *scope = &p->interfaces->scopes[owner->module];
            for (uint32_t i = 0; i < owner->case_count; i++) {
                if (!infer_work(p))
                    return false;
                const struct mo_data_signature *data =
                    infer_data_type(p, &scope->symbols[owner->cases[i]]);
                MO_ASSERT(data != NULL);
                if (!data->unnamed || data->count != 1)
                    continue;
                const struct mo_source_type *field = pattern_field_type(p, data, target, 0);
                if (field == NULL)
                    return false;
                guesses = NULL;
                if (failure_matches(p, from, field, &guesses, 0)) {
                    count++;
                    selected = field;
                    at->wrapper.symbol = data->symbol;
                    at->generic = data->type;
                }
                if (p->failed)
                    return false;
            }
        } else if (target->declaration == NULL) {
            const struct mo_builtin_case *first = mo_builtin_case_by_type(target->name);
            for (uint32_t i = 0; first != NULL && i < first->cases; i++) {
                if (!infer_work(p))
                    return false;
                const struct mo_builtin_case *data = mo_builtin_case_by_key(first->first_key + i);
                MO_ASSERT(data != NULL);
                if (data->count != 1 || data->fields[0] != NULL)
                    continue;
                MO_ASSERT(data->parameter < target->count);
                guesses = NULL;
                if (failure_matches(p, from, target->arguments[data->parameter], &guesses, 0)) {
                    count++;
                    selected = target->arguments[data->parameter];
                    at->wrapper.builtin = data->name;
                    at->generic = mo_builtin_template(data);
                }
                if (p->failed)
                    return false;
            }
        }
        if (count != 1) {
            struct type_preview preview = {0};
            if (!infer_preview_type(p, &preview, from, 0))
                return false;
            char *message = infer_allocate(p, 768, 1, 1);
            if (message == NULL)
                return false;
            int length = snprintf(message, 768,
                                  "try needs exactly one failure case holding %.511s as its "
                                  "unnamed value; found %u",
                                  preview.text, (unsigned)count);
            MO_ASSERT(length > 0 && length < 768);
            return infer_problem(
                p, MO_CODE_TRY_WRAPPER, p->span, message,
                count == 0 ? "Add one case Wrap(E) to the function's failure type, where E "
                             "is the source failure type; named fields don't wrap "
                             "(reference 8.3)."
                           : "Keep exactly one case with this unnamed value; make the "
                             "other matching cases use named fields, or handle the failure "
                             "with case (reference 8.3).",
                NULL);
        }
        MO_ASSERT(selected != NULL);
        struct term *actual = infer_annotation(p, selected, NULL, 0);
        if (actual == NULL || !infer_unify(p, from, actual, 0))
            return false;
    }
    return true;
}

/* One immutable typed ANY per field and nominal instantiation. Omitted parts
 * borrow these nodes, whose syntax points at the declaring field. */
static MO_MUST_USE const struct mo_checked_pattern *const *
pattern_defaults(struct inference *p, const struct mo_data_signature *data,
                 const struct mo_source_type *type) {
    for (struct pattern_defaults *at = p->defaults; at != NULL; at = at->next) {
        if (!infer_work(p))
            return NULL;
        if (at->data == data && at->type == type)
            return at->fields;
    }
    struct pattern_defaults *out =
        infer_allocate(p, 1, sizeof(*out), _Alignof(struct pattern_defaults));
    const struct mo_checked_pattern **fields = infer_allocate(
        p, data->count, sizeof(*fields), _Alignof(const struct mo_checked_pattern *));
    if (p->failed)
        return NULL;
    for (uint32_t i = 0; i < data->count; i++) {
        const struct mo_field_signature *field = data->by_index[i];
        const struct mo_source_type *field_type = pattern_field_type(p, data, type, i);
        fields[i] =
            field_type == NULL
                ? NULL
                : pattern_output(p, field->name == NULL ? data->symbol->declaration : field->name,
                                 field_type, MO_PATTERN_ANY, 0);
        if (fields[i] == NULL)
            return NULL;
    }
    *out = (struct pattern_defaults){data, type, fields, p->defaults};
    p->defaults = out;
    return fields;
}

/* Normalize only decisions already retained by inference. The syntax-pointer
 * index supplies resolved binders/constructors/fields without a second lexical
 * walk; omitted fields acquire explicit typed ANY parts. */
MO_MUST_USE struct mo_checked_pattern *
infer_checked_pattern(struct inference *p, const struct mo_syntax *syntax,
                      const struct mo_source_type *type, struct note *const *notes,
                      uint32_t note_count, const struct mo_checked_binding *bindings,
                      uint32_t binding_count, uint32_t depth) {
    if (!infer_work(p) || !infer_type_depth(p, depth))
        return NULL;
    if (syntax->kind == MO_S_GROUP || syntax->kind == MO_S_BINDER)
        return infer_checked_pattern(p, syntax->children[0], type, notes, note_count, bindings,
                                     binding_count, depth + 1);
    if (syntax->kind == MO_S_UNDERSCORE)
        return pattern_output(p, syntax, type, MO_PATTERN_ANY, 0);
    if (syntax->kind == MO_S_NAME) {
        const struct note *note = pattern_note(notes, note_count, syntax);
        MO_ASSERT(note->binding != NULL && note->binding->binding_index < binding_count);
        struct mo_checked_pattern *out = pattern_output(p, syntax, type, MO_PATTERN_BIND, 0);
        if (out != NULL)
            out->binding = &bindings[note->binding->binding_index];
        return out;
    }
    if (syntax->kind == MO_S_TUPLE) {
        struct mo_checked_pattern *out =
            pattern_output(p, syntax, type, MO_PATTERN_TUPLE, syntax->count);
        struct mo_checked_pattern_part *parts = infer_allocate(
            p, syntax->count, sizeof(*parts), _Alignof(struct mo_checked_pattern_part));
        if (p->failed)
            return NULL;
        MO_ASSERT(type->count == syntax->count);
        out->parts = parts;
        for (uint32_t i = 0; i < syntax->count; i++) {
            parts[i] = (struct mo_checked_pattern_part){
                i, infer_checked_pattern(p, syntax->children[i], type->arguments[i], notes,
                                         note_count, bindings, binding_count, depth + 1)};
            if (parts[i].pattern == NULL)
                return NULL;
        }
        return out;
    }
    if (syntax->kind == MO_S_LIST) {
        uint32_t length = syntax->count;
        bool rest = length != 0 && syntax->children[length - 1]->kind == MO_S_REST;
        if (rest)
            length--;
        const struct mo_checked_pattern **heads =
            infer_allocate(p, length, sizeof(*heads), _Alignof(const struct mo_checked_pattern *));
        if (heads == NULL)
            return NULL;
        for (uint32_t i = 0; i < length; i++) {
            heads[i] = infer_checked_pattern(p, syntax->children[i], type->arguments[0], notes,
                                             note_count, bindings, binding_count, depth + 1);
            if (heads[i] == NULL)
                return NULL;
        }
        struct mo_checked_pattern *tail =
            rest ? infer_checked_pattern(p, syntax->children[length]->children[0], type, notes,
                                         note_count, bindings, binding_count, depth + 1)
                 : pattern_output(p, syntax, type, MO_PATTERN_LIST, 0);
        for (uint32_t i = length; tail != NULL && i != 0; i--) {
            struct mo_checked_pattern *cons = pattern_output(p, syntax, type, MO_PATTERN_LIST, 2);
            struct mo_checked_pattern_part *parts =
                infer_allocate(p, 2, sizeof(*parts), _Alignof(struct mo_checked_pattern_part));
            if (p->failed)
                return NULL;
            parts[0] = (struct mo_checked_pattern_part){0, heads[i - 1]};
            parts[1] = (struct mo_checked_pattern_part){1, tail};
            cons->parts = parts;
            tail = cons;
        }
        return tail;
    }
    bool constructor = syntax->kind == MO_S_CAPNAME || syntax->kind == MO_S_QUALIFIED ||
                       syntax->kind == MO_S_CALL || syntax->kind == MO_S_POSTFIX;
    if (!constructor)
        return pattern_output(p, syntax, type, MO_PATTERN_LITERAL, 0);
    const struct note *note = pattern_note(notes, note_count, syntax);
    if (note->output.builtin_case != NULL) {
        const struct mo_syntax *name = syntax->kind == MO_S_CALL ? syntax->children[0] : syntax;
        const struct mo_builtin_case *data = mo_builtin_case_by_name(name);
        MO_ASSERT(data != NULL);
        struct mo_checked_pattern *out =
            pattern_output(p, syntax, type, MO_PATTERN_CONSTRUCTOR, data->count);
        struct mo_checked_pattern_part *parts = infer_allocate(
            p, data->count, sizeof(*parts), _Alignof(struct mo_checked_pattern_part));
        if (p->failed)
            return NULL;
        out->constructor.builtin = data->name;
        out->parts = parts;
        const struct mo_source_type *generic = mo_builtin_template(data);
        if (generic != NULL && !infer_type_arguments(p, generic, type, &out->arguments))
            return NULL;
        struct term *nominal = infer_annotation(p, type, NULL, 0);
        if (nominal == NULL)
            return NULL;
        for (uint32_t i = 0; i < data->count; i++) {
            struct term *part = infer_builtin_part(p, data, nominal, i);
            const struct mo_source_type *field =
                part == NULL ? NULL : infer_finish_type(p, part, 0);
            if (field == NULL)
                return NULL;
            parts[i] = (struct mo_checked_pattern_part){
                i, pattern_output(p, syntax, field, MO_PATTERN_ANY, 0)};
            if (parts[i].pattern == NULL)
                return NULL;
        }
        const struct mo_syntax *args = syntax->kind == MO_S_CALL ? syntax->children[1] : NULL;
        for (uint32_t i = 0; args != NULL && i < args->count; i++) {
            uint32_t index = infer_builtin_field(data, args->children[i]);
            MO_ASSERT(index < data->count);
            const struct mo_syntax *value =
                data->fields[0] == NULL ? args->children[i] : args->children[i]->children[1];
            parts[index].pattern =
                infer_checked_pattern(p, value, parts[index].pattern->type, notes, note_count,
                                      bindings, binding_count, depth + 1);
            if (parts[index].pattern == NULL)
                return NULL;
        }
        return out;
    }
    MO_ASSERT(note->output.symbol != NULL);
    const struct mo_data_signature *data = infer_data_type(p, note->output.symbol);
    MO_ASSERT(data != NULL);
    struct mo_checked_pattern *out =
        pattern_output(p, syntax, type, MO_PATTERN_CONSTRUCTOR, data->count);
    struct mo_checked_pattern_part *parts =
        infer_allocate(p, data->count, sizeof(*parts), _Alignof(struct mo_checked_pattern_part));
    if (p->failed)
        return NULL;
    out->constructor.symbol = data->symbol;
    out->parts = parts;
    if (!infer_type_arguments(p, data->type, type, &out->arguments) ||
        !infer_data_requirements(p, &out->arguments, &out->ability_count, &out->abilities))
        return NULL;
    const struct mo_checked_pattern *const *defaults = pattern_defaults(p, data, type);
    if (defaults == NULL)
        return NULL;
    for (uint32_t i = 0; i < data->count; i++) {
        parts[i] = (struct mo_checked_pattern_part){i, defaults[i]};
    }
    const struct mo_syntax *args = syntax->kind == MO_S_CALL      ? syntax->children[1]
                                   : syntax->kind == MO_S_POSTFIX ? syntax->children[2]->children[0]
                                                                  : NULL;
    for (uint32_t i = 0; args != NULL && i < args->count; i++) {
        const struct mo_syntax *value =
            data->unnamed ? args->children[i] : args->children[i]->children[1];
        const struct note *selected = pattern_note(notes, note_count, value);
        MO_ASSERT(selected->output.field != NULL);
        uint32_t index = selected->output.field->index;
        parts[index].pattern =
            infer_checked_pattern(p, value, parts[index].pattern->type, notes, note_count, bindings,
                                  binding_count, depth + 1);
        if (parts[index].pattern == NULL)
            return NULL;
    }
    return out;
}
