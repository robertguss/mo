#include "check/infer.h"
#include "check/map_keys.h"
#include "check/signatures.h"

#include "base/codes.h"
#include "base/limits.h"

#include <string.h>

const char infer_unknown_member[] = "the module or type has no function with this name";

MO_MUST_USE bool infer_problem(struct inference *p, uint16_t code, struct mo_span span,
                               const char *message, const char *reason,
                               const struct mo_syntax *previous) {
    if (p->fatal || (p->partial && code == MO_CODE_CANNOT_INFER))
        return false;
    p->failed = true;
    p->fatal = code >= 1000 && code != MO_CODE_CHECK_UNFINISHED;
    if (!p->fatal && p->error.code != 0 && p->error.primary.span.start <= span.start)
        return false;
    const char *label =
        code == MO_CODE_NAME_UNKNOWN
            ? (message == infer_unknown_member ? "no function of this name"
                                               : "not visible in this block")
        : code == MO_CODE_BIND_REPEATED ? "this name already has a meaning"
        : code == MO_CODE_UNUSED_NAME   ? "never read"
        : code == MO_CODE_TYPE_MISMATCH || code == MO_CODE_CONDITION_TYPE ||
                code == MO_CODE_NUMBER_ABILITY || code == MO_CODE_BYTE_ARITHMETIC ||
                code == MO_CODE_ARGUMENT_TYPE || code == MO_CODE_RESULT_TYPE ||
                code == MO_CODE_SERVICE_VALUE || code == MO_CODE_NOT_FUNCTION
            ? p->type_label
        : code == MO_CODE_CANNOT_INFER         ? "no use decides the type"
        : code == MO_CODE_BOOL_NAME            ? "must be Bool or a function returning Bool"
        : code == MO_CODE_CALL_ARGUMENTS       ? "pass one positional argument per parameter"
        : code == MO_CODE_UNUSED_VALUE         ? "not Unit and not used"
        : code == MO_CODE_NOT_VARIABLE         ? "no assignable var here"
        : code == MO_CODE_LAMBDA_WRITE         ? "an anonymous function can't assign a var"
        : code == MO_CODE_CAPABILITY_CAPTURE   ? "pass capabilities as parameters instead"
        : code == MO_CODE_CAPTURE_ASSIGNMENT   ? "copy this value into a name first"
        : code == MO_CODE_EXIT_OUTSIDE_LOOP    ? "no enclosing loop in this function"
        : code == MO_CODE_MAP_KEY_REPEATED     ? "this key is already in the literal"
        : code == MO_CODE_DOT_CALL_PARENTHESES ? "the function takes only the receiver"
        : code == MO_CODE_FAKE_OUTSIDE_SPEC    ? "outside any test, example or promise"
        : code == MO_CODE_LITERAL_RANGE        ? "outside its inferred numeric range"
        : code == MO_CODE_TUPLE_POSITION       ? "part index is outside this tuple"
        : code == MO_CODE_FAILURE_IGNORED      ? "handle this Result or pass its failure upward"
        : code == MO_CODE_DATA_ARGUMENTS       ? "supplied fields don't match the declaration"
        : code == MO_CODE_DATA_PRIVATE         ? "the type's insides aren't exposed"
        : code == MO_CODE_FIELD_MISSING        ? "no field with this name"
        : code == MO_CODE_PATTERN_FIELDS       ? "matched fields don't fit this constructor"
        : code == MO_CODE_CASE_CATCH_ALL       ? "covers cases without naming them"
        : code == MO_CODE_PATTERN_ALTERNATIVES ? "the alternatives bind different names"
        : code == MO_CODE_CASE_INCOMPLETE      ? "a possible value isn't covered"
        : code == MO_CODE_PATTERN_CONSTRUCTOR  ? "not a record or case constructor"
                                               : message;
    p->secondary =
        (struct mo_label){previous == NULL ? span : previous->span,
                          code == MO_CODE_BIND_REPEATED        ? "the other meaning of this name"
                          : code == MO_CODE_CAPTURE_ASSIGNMENT ? "assigned here"
                          : code == MO_CODE_MAP_KEY_REPEATED   ? "the first equal key"
                                                               : "declared here"};
    p->error = (struct mo_diag){.code = code,
                                .primary = {span, label},
                                .message = message,
                                .reason = reason,
                                .secondary = previous == NULL ? NULL : &p->secondary,
                                .secondary_count = previous == NULL ? 0 : 1};
    p->error_unused = p->unused;
    return false;
}

/* End-of-scope and end-of-inference errors are certain but need not stop
 * constraints. Keep only the earliest independent error, without a corpus-sized
 * temporary list. Resource failure still stops immediately and takes priority. */
void infer_defer(struct inference *p, uint16_t code, struct mo_span span, const char *message,
                 const char *reason) {
    bool reported = infer_problem(p, code, span, message, reason, NULL);
    (void)reported;
    p->failed = p->fatal;
}

void infer_emit_problem(struct inference *p) {
    if (p->unavailable && p->error.code == 0)
        return; /* The declaration already has its own diagnostic. */
    MO_ASSERT(p->error.code != 0);
    struct mo_span span = p->error.primary.span;
    struct mo_edit edit = {{span.file, span.start, span.start}, "_"};
    struct mo_fix fix = {"mark the name unused on purpose", &edit, 1};
    bool offer_fix = false;
    if (p->error.code == MO_CODE_UNUSED_NAME && !p->error_unused->variable &&
        p->error_unused->pattern_alternative <= 1) {
        const struct mo_syntax *name = p->error_unused->name;
        size_t length = name->span.end - name->span.start;
        uint8_t *candidate = mo_arena_allocate(p->arena, length + 1, 1);
        if (candidate == NULL) {
            bool reported = infer_problem(p, MO_CODE_CHECK_NO_MEMORY, span,
                                          "the checking arena has no room", NULL, NULL);
            (void)reported;
            infer_emit_problem(p);
            return;
        }
        candidate[0] = '_';
        memcpy(candidate + 1, name->text, length);
        struct name_slot *at = p->names;
        while (at != NULL) {
            size_t other = at->name->span.end - at->name->span.start;
            size_t shared = length + 1 < other ? length + 1 : other;
            int order = memcmp(candidate, at->name->text, shared);
            if (order == 0)
                order = length + 1 < other ? -1 : length + 1 > other ? 1 : 0;
            if (order == 0)
                break;
            at = order < 0 ? at->left : at->right;
        }
        const struct mo_module_scope *scope = &p->interfaces->scopes[p->function->function->module];
        offer_fix = at == NULL && scope->imported_count == 0 &&
                    mo_find_symbol(scope, candidate, length + 1) == NULL;
    }
    if (p->error.code == MO_CODE_UNUSED_NAME) {
        p->error.fixes = offer_fix ? &fix : NULL;
        p->error.fix_count = offer_fix ? 1u : 0u;
    }
    bool kept = mo_diags_add(p->diags, &p->error);
    (void)kept;
}

MO_MUST_USE bool infer_unfinished(struct inference *p) {
    return infer_problem(p, MO_CODE_CHECK_UNFINISHED, p->span,
                         "this body rule is not implemented by the staged checker", NULL, NULL);
}

MO_MUST_USE bool infer_work(struct inference *p) {
    if (p->failed)
        return false;
    if (p->work == MO_LIMIT_SOURCE_CHECK_WORK)
        return infer_problem(p, MO_CODE_CHECK_WORK, p->span,
                             "the function needs too much type-checking work", NULL, NULL);
    if (*p->signatures->work == MO_LIMIT_PROGRAM_CHECK_WORK)
        return infer_problem(p, MO_CODE_CHECK_PROGRAM_WORK, p->span,
                             "the program needs too much type-checking work", NULL, NULL);
    p->work++;
    (*p->signatures->work)++;
    return true;
}

MO_MUST_USE void *infer_allocate(struct inference *p, size_t count, size_t size, size_t alignment) {
    if (p->failed)
        return NULL;
    void *out = mo_arena_allocate_array(p->arena, count, size, alignment);
    if (out == NULL) {
        bool reported = infer_problem(p, MO_CODE_CHECK_NO_MEMORY, p->span,
                                      "the checking arena has no room", NULL, NULL);
        (void)reported;
    }
    return out;
}

static MO_MUST_USE bool map_key_work(void *context, size_t amount) {
    struct inference *p = context;
    for (size_t i = 0; i < amount; i++)
        if (!infer_work(p))
            return false;
    return true;
}

static MO_MUST_USE void *map_key_allocate(void *context, size_t bytes) {
    return infer_allocate(context, bytes, 1, 1);
}

struct literal_key {
    struct mo_map_literal_key value;
    const struct mo_syntax *syntax;
    uint32_t position, version;
};

static MO_MUST_USE bool map_key_order(struct inference *p, const struct literal_key *a,
                                      const struct literal_key *b, int *order) {
    const struct mo_map_literal_key *x = &a->value, *y = &b->value;
    if (!infer_work(p))
        return false;
    if (x->category != y->category)
        *order = x->category < y->category ? -1 : 1;
    else if (a->version != b->version)
        *order = a->version < b->version ? -1 : 1;
    else if (x->negative != y->negative)
        *order = x->negative ? -1 : 1;
    else if (x->length != y->length)
        *order = x->length < y->length ? -1 : 1;
    else {
        if (!map_key_work(p, x->length))
            return false;
        *order = memcmp(x->bytes, y->bytes, x->length);
    }
    return true;
}

MO_MUST_USE bool infer_map_keys(struct inference *p, const struct mo_syntax *node,
                                const uint32_t *versions) {
    if (node->count < 2)
        return true;
    struct literal_key *keys =
        infer_allocate(p, node->count, sizeof(*keys), _Alignof(struct literal_key));
    struct literal_key *scratch =
        infer_allocate(p, node->count, sizeof(*scratch), _Alignof(struct literal_key));
    if (keys == NULL || scratch == NULL)
        return false;
    struct mo_map_key_context context = {p, map_key_work, map_key_allocate};
    uint32_t count = 0;
    for (uint32_t i = 0; i < node->count; i++) {
        const struct mo_syntax *key = node->children[i]->children[0];
        while (key->kind == MO_S_GROUP)
            key = key->children[0];
        struct mo_map_literal_key value = {0};
        p->span = key->span;
        if (!mo_map_literal_key(&context, key, &value))
            return false;
        if (key->kind == MO_S_NAME) {
            value.category = MO_MAP_KEY_NAME;
            value.bytes = key->text;
            value.length = key->span.end - key->span.start;
        }
        if (value.category != MO_MAP_KEY_UNKNOWN)
            keys[count++] = (struct literal_key){value, key, i, versions[i]};
    }
    /* Stable merges retain the first source key and bound equal-prefix work. */
    for (uint32_t width = 1; width < count; width *= 2) {
        for (uint32_t begin = 0; begin < count; begin += 2 * width) {
            uint32_t mid = count - begin < width ? count : begin + width;
            uint32_t end = count - mid < width ? count : mid + width;
            uint32_t left = begin, right = mid;
            for (uint32_t at = begin; at < end; at++) {
                int order = 0;
                if (left < mid && right < end &&
                    !map_key_order(p, &keys[left], &keys[right], &order))
                    return false;
                scratch[at] =
                    right == end || (left < mid && order <= 0) ? keys[left++] : keys[right++];
            }
        }
        struct literal_key *swap = keys;
        keys = scratch;
        scratch = swap;
    }
    const struct literal_key *first = NULL, *repeated = NULL;
    for (uint32_t i = 1; i < count; i++) {
        int order = 0;
        if (!map_key_order(p, &keys[i - 1], &keys[i], &order))
            return false;
        if (order == 0 && (repeated == NULL || keys[i].position < repeated->position)) {
            first = &keys[i - 1];
            repeated = &keys[i];
        }
    }
    return repeated == NULL ||
           infer_problem(p, MO_CODE_MAP_KEY_REPEATED, repeated->syntax->span,
                         "a map literal gives the same key twice",
                         "Each key in a map literal must be different (reference 3.8).",
                         first->syntax);
}

MO_MUST_USE struct term *infer_make_type(struct inference *p, struct mo_source_type shape) {
    if (!infer_work(p))
        return NULL;
    struct term *t = infer_allocate(p, 1, sizeof(*t), _Alignof(struct term));
    if (t == NULL)
        return NULL;
    *t = (struct term){.shape = shape};
    if (shape.count != 0) {
        t->arguments =
            infer_allocate(p, shape.count, sizeof(*t->arguments), _Alignof(struct term *));
        if (t->arguments == NULL)
            return NULL;
    }
    return t;
}

MO_MUST_USE struct term *infer_fresh(struct inference *p, unsigned numeric) {
    struct term *t = infer_make_type(p, (struct mo_source_type){0});
    if (t != NULL) {
        t->variable = true;
        t->numeric = numeric;
    }
    return t;
}

MO_MUST_USE struct term *infer_native(struct inference *p, const char *name, uint32_t count) {
    return infer_make_type(
        p, (struct mo_source_type){
               .kind = MO_SOURCE_NAMED, .name = name, .module = UINT32_MAX, .count = count});
}

MO_MUST_USE struct term *infer_root(struct term *t) {
    MO_ASSERT(t != NULL);
    struct term *at = t;
    while (at->parent != NULL)
        at = at->parent;
    while (t->parent != NULL) {
        struct term *next = t->parent;
        t->parent = at;
        t = next;
    }
    return at;
}

MO_MUST_USE bool infer_named(struct term *t, const char *name) {
    t = infer_root(t);
    return !t->variable && t->shape.kind == MO_SOURCE_NAMED && t->shape.module == UINT32_MAX &&
           strcmp(t->shape.name, name) == 0;
}

static MO_MUST_USE unsigned number_type(struct term *t) {
    static const char *const names[] = {"Int", "BigInt", "Byte", "Decimal", "Float"};
    for (unsigned i = 0; i < 5; i++)
        if (infer_named(t, names[i]))
            return 1u << i;
    return 0;
}

MO_MUST_USE bool infer_type_depth(struct inference *p, uint32_t depth) {
    if (depth < MO_LIMIT_SOURCE_TYPE_DEPTH)
        return true;
    return infer_problem(p, MO_CODE_CHECK_TYPE_DEPTH, p->span,
                         "the inferred type is nested too deeply", NULL, NULL);
}

/* Frozen annotations borrow their prepared DAG. Materialize only the child a
 * constraint or projection actually reads, not a wide signature on every use. */
MO_MUST_USE struct term *infer_argument(struct inference *p, struct term *t, uint32_t index) {
    t = infer_root(t);
    MO_ASSERT(index < t->shape.count);
    if (t->arguments != NULL)
        return t->arguments[index];
    MO_ASSERT(t->resolved != NULL);
    return infer_annotation(p, t->resolved->arguments[index], NULL, 0);
}

static void preview_text(struct type_preview *out, const char *text) {
    if (out->length + 4 >= sizeof(out->text))
        return;
    while (*text != '\0' && out->length + 4 < sizeof(out->text))
        out->text[out->length++] = *text++;
    if (*text != '\0') {
        memcpy(out->text + out->length, "...", 3);
        out->length += 3;
    }
    out->text[out->length] = '\0';
}

MO_MUST_USE bool infer_preview_type(struct inference *p, struct type_preview *out, struct term *t,
                                    uint32_t depth) {
    if (out->length + 4 >= sizeof(out->text))
        return true;
    if (!infer_work(p))
        return false;
    t = infer_root(t);
    if (depth == MO_LIMIT_SOURCE_TYPE_DEPTH) {
        preview_text(out, "...");
        return true;
    }
    if (t->variable) {
        if (t->numeric == 0)
            preview_text(out, "?");
        static const char *const numbers[] = {"Int", "BigInt", "Byte", "Decimal", "Float"};
        bool first = true;
        for (unsigned i = 0; i < 5; i++)
            if ((t->numeric & (1u << i)) != 0) {
                if (!first)
                    preview_text(out, " or ");
                preview_text(out, numbers[i]);
                first = false;
            }
        return true;
    }
    bool tuple = t->shape.kind == MO_SOURCE_TUPLE, function = t->shape.kind == MO_SOURCE_FUNCTION;
    if (!tuple)
        preview_text(out, t->shape.name);
    if (t->shape.count == 0)
        return true;
    preview_text(out, "(");
    uint32_t count = t->shape.count - (function ? 1u : 0u);
    for (uint32_t i = 0; i < count && out->length + 4 < sizeof(out->text); i++) {
        if (i != 0)
            preview_text(out, ", ");
        struct term *part = infer_argument(p, t, i);
        if (part == NULL || !infer_preview_type(p, out, part, depth + 1))
            return false;
    }
    preview_text(out, ")");
    if (function && out->length + 4 < sizeof(out->text)) {
        preview_text(out, " : ");
        struct term *result = infer_argument(p, t, count);
        if (result == NULL || !infer_preview_type(p, out, result, depth + 1))
            return false;
    }
    return true;
}

static MO_MUST_USE bool mismatch(struct inference *p, struct term *found, struct term *expected) {
    struct type_preview label = {0};
    preview_text(&label, "expected ");
    if (!infer_preview_type(p, &label, expected, 0))
        return false;
    preview_text(&label, ", found ");
    if (!infer_preview_type(p, &label, found, 0))
        return false;
    char *text = infer_allocate(p, label.length + 1, 1, 1);
    if (text == NULL)
        return false;
    memcpy(text, label.text, label.length + 1);
    p->type_label = text;
    uint16_t code = p->expectation == 0 ? MO_CODE_TYPE_MISMATCH : p->expectation;
    const char *message = "values of different types are combined";
    const char *reason =
        "Comparing or combining values of different types is an error (reference 3).";
    if (code == MO_CODE_CONDITION_TYPE) {
        message = "a condition isn't a yes-or-no answer";
        reason = "Bool is a yes-or-no answer; conditions need Bool (reference 3.1).";
    } else if (code == MO_CODE_NUMBER_ABILITY) {
        message = "arithmetic is used on a type without Number";
        reason = infer_named(found, "Text")
                     ? "Text has no Number, so + doesn't join texts; insert them into one text "
                       "instead, as in \"#{a}#{b}\" (reference 3.4)."
                     : "Only Int, BigInt, Decimal and Float have Number; times and durations "
                       "also add and subtract (reference 9.2).";
    } else if (code == MO_CODE_BYTE_ARITHMETIC) {
        message = "Byte is used in arithmetic";
        reason = "Byte has no arithmetic; convert it with to_int first (reference 3.2).";
    } else if (code == MO_CODE_ARGUMENT_TYPE) {
        message = "an argument's type doesn't match its parameter";
        reason = "A call passes each argument to its corresponding parameter (reference 5.2).";
    } else if (code == MO_CODE_RESULT_TYPE) {
        message = "a function's result doesn't match its signature";
        reason = "A function's signature gives its types (reference 5.1).";
    } else if (code == MO_CODE_SERVICE_VALUE) {
        message = "a service value has the wrong type";
        reason = "The start value has the declared state type (reference 11.5).";
    } else if (code == MO_CODE_NOT_FUNCTION) {
        message = "a value that isn't a function is called";
        reason = "A call applies a function to its arguments (reference 5.2).";
    }
    return infer_problem(p, code, p->span, message, reason, NULL);
}

/* Each occurs walk visits a shared type once, not once per path through its DAG. */
static MO_MUST_USE bool occurs(struct inference *p, struct term *needle, struct term *t,
                               uint32_t depth) {
    if (!infer_work(p) || !infer_type_depth(p, depth))
        return true;
    t = infer_root(t);
    if (needle == t)
        return true;
    if (t->resolved != NULL)
        return !infer_type_depth(p, depth + t->height);
    if (t->seen == p->epoch)
        return false;
    t->seen = p->epoch;
    for (uint32_t i = 0; i < t->shape.count; i++)
        if (occurs(p, needle, t->arguments[i], depth + 1))
            return true;
    return false;
}

MO_MUST_USE bool infer_unify(struct inference *p, struct term *a, struct term *b, uint32_t depth) {
    if (!infer_work(p) || !infer_type_depth(p, depth))
        return false;
    a = infer_root(a);
    b = infer_root(b);
    if (a == b || (a->resolved != NULL && a->resolved == b->resolved))
        return true;
    unsigned numeric = a->numeric == 0   ? b->numeric
                       : b->numeric == 0 ? a->numeric
                                         : a->numeric & b->numeric;
    if (a->numeric != 0 && b->numeric != 0 && numeric == 0)
        return mismatch(p, a, b);
    bool a_number = (numeric & (INT | BIG | DECIMAL | FLOAT)) == (INT | BIG | DECIMAL | FLOAT) &&
                    a->shape.kind == MO_SOURCE_PARAMETER &&
                    a->shape.declaration == p->function->function &&
                    (p->function->requirements[(unsigned)(a->shape.name[0] - 'A')] &
                     (1u << MO_SOURCE_NUMBER)) != 0;
    bool b_number = (numeric & (INT | BIG | DECIMAL | FLOAT)) == (INT | BIG | DECIMAL | FLOAT) &&
                    b->shape.kind == MO_SOURCE_PARAMETER &&
                    b->shape.declaration == p->function->function &&
                    (p->function->requirements[(unsigned)(b->shape.name[0] - 'A')] &
                     (1u << MO_SOURCE_NUMBER)) != 0;
    if ((!a->variable && numeric != 0 && !a_number && (number_type(a) & numeric) == 0) ||
        (!b->variable && numeric != 0 && !b_number && (number_type(b) & numeric) == 0))
        return mismatch(p, a, b);
    if (a->variable && !b->variable) {
        struct term *swap = a;
        a = b;
        b = swap;
    }
    if (b->variable) {
        MO_ASSERT(p->epoch < UINT32_MAX);
        p->epoch++;
        if (occurs(p, b, a, 0))
            return p->failed ? false : mismatch(p, a, b);
    } else {
        if (a->shape.kind != b->shape.kind || a->shape.module != b->shape.module ||
            a->shape.declaration != b->shape.declaration || a->shape.count != b->shape.count ||
            strcmp(a->shape.name, b->shape.name) != 0)
            return mismatch(p, a, b);
        for (uint32_t i = 0; i < a->shape.count; i++) {
            struct term *x = infer_argument(p, a, i), *y = infer_argument(p, b, i);
            if (x == NULL || y == NULL || !infer_unify(p, x, y, depth + 1))
                return false;
        }
    }
    if (a->variable == b->variable && a->rank < b->rank) {
        struct term *swap = a;
        a = b;
        b = swap;
    }
    b->parent = a;
    a->numeric = numeric;
    a->deferred = a->deferred || b->deferred;
    if (a->rank == b->rank)
        a->rank++;
    return true;
}

MO_MUST_USE bool infer_unify_as(struct inference *p, struct term *found, struct term *expected,
                                uint16_t code) {
    uint16_t saved = p->expectation;
    p->expectation = code;
    bool valid = infer_unify(p, found, expected, 0);
    p->expectation = saved;
    return valid;
}

/* Whether a and b can be one type, without unifying them: an unsolved type matches any type its
 * numeric range allows. A diagnostic's choice of fix needs only this, so it is approximate. */
MO_MUST_USE bool infer_same_type(struct inference *p, struct term *a, struct term *b,
                                 uint32_t depth) {
    a = infer_root(a);
    b = infer_root(b);
    if (a == b || (a->resolved != NULL && a->resolved == b->resolved))
        return true;
    if (depth >= MO_LIMIT_SOURCE_TYPE_DEPTH)
        return false;
    if (a->variable && !b->variable) {
        struct term *swap = a;
        a = b;
        b = swap;
    }
    if (b->variable)
        return b->numeric == 0 || (a->variable && a->numeric == 0) ||
               ((a->variable ? a->numeric : number_type(a)) & b->numeric) != 0;
    if (a->shape.kind != b->shape.kind || a->shape.module != b->shape.module ||
        a->shape.declaration != b->shape.declaration || a->shape.count != b->shape.count ||
        strcmp(a->shape.name, b->shape.name) != 0)
        return false;
    for (uint32_t i = 0; i < a->shape.count; i++) {
        struct term *x = infer_argument(p, a, i), *y = infer_argument(p, b, i);
        if (x == NULL || y == NULL || !infer_same_type(p, x, y, depth + 1))
            return false;
    }
    return true;
}

MO_MUST_USE struct term *infer_annotation(struct inference *p, const struct mo_source_type *type,
                                          struct term **parameters, uint32_t depth) {
    if (depth == 0 && !mo_signature_type_available(p->signatures, type)) {
        p->failed = p->fatal = p->unavailable = true;
        return NULL;
    }
    if (!infer_type_depth(p, depth))
        return NULL;
    if (parameters == NULL || !type->parameters) {
        if (!infer_work(p) || !infer_type_depth(p, depth + type->height))
            return NULL;
        struct term *t = infer_allocate(p, 1, sizeof(*t), _Alignof(struct term));
        if (t != NULL)
            *t = (struct term){.shape = *type, .resolved = type, .height = type->height};
        return t;
    }
    if (parameters != NULL && type->kind == MO_SOURCE_PARAMETER) {
        unsigned index = (unsigned)(type->name[0] - 'A');
        MO_ASSERT(index < 26);
        if (parameters[index] == NULL)
            parameters[index] = infer_fresh(p, 0);
        return parameters[index];
    }
    struct term *t = infer_make_type(p, *type);
    if (t == NULL)
        return NULL;
    for (uint32_t i = 0; i < type->count; i++) {
        t->arguments[i] = infer_annotation(p, type->arguments[i], parameters, depth + 1);
        if (t->arguments[i] == NULL)
            return NULL;
    }
    return t;
}

MO_MUST_USE int infer_compare_name(const struct mo_syntax *a, const struct mo_syntax *b) {
    size_t x = a->span.end - a->span.start, y = b->span.end - b->span.start;
    int order = memcmp(a->text, b->text, x < y ? x : y);
    return order != 0 ? order : x < y ? -1 : x > y ? 1 : 0;
}

MO_MUST_USE struct value infer_graph(struct inference *p, struct value v, uint32_t count) {
    if (!p->checked || v.type == NULL)
        return v;
    if (!infer_work(p))
        return (struct value){0};
    struct emitted *out = infer_allocate(p, 1, sizeof(*out), _Alignof(struct emitted));
    struct note **operands = infer_allocate(p, count, sizeof(*operands), _Alignof(struct note *));
    if (p->failed)
        return (struct value){0};
    *out = (struct emitted){
        .output = {.operand_count = count, .field = UINT32_MAX, .receiver = UINT32_MAX},
        .operands = operands};
    v.graph = out;
    return v;
}

MO_MUST_USE bool infer_record(struct inference *p, const struct mo_syntax *node, struct value v) {
    struct note *n = infer_allocate(p, 1, sizeof(*n), _Alignof(struct note));
    if (n == NULL)
        return false;
    if (v.graph != NULL) {
        if (v.symbol != NULL)
            v.graph->output.symbol = v.symbol;
        if (v.builtin_case != NULL)
            v.graph->output.native = v.builtin_case;
    }
    *n = (struct note){.output = {.syntax = node,
                                  .binding = v.binding,
                                  .symbol = v.symbol,
                                  .field = v.field,
                                  .builtin_case = v.builtin_case,
                                  .never = v.never},
                       .type = v.type,
                       .graph = v.graph,
                       .binding = v.bound,
                       .callee = v.callee,
                       .index = UINT32_MAX,
                       .binding_index = UINT32_MAX};
    if (v.dispatch != NULL && v.dispatch->note == NULL)
        v.dispatch->note = n;
    if (p->last == NULL)
        p->notes = n;
    else
        p->last->next = n;
    p->last = n;
    p->count++;
    return true;
}

MO_MUST_USE int infer_source_note_order(const void *a, const void *b) {
    const struct note *x = *(const struct note *const *)a;
    const struct note *y = *(const struct note *const *)b;
    uint32_t left = x->output.syntax->span.start, right = y->output.syntax->span.start;
    return left < right ? -1 : left > right ? 1 : 0;
}

MO_MUST_USE int infer_syntax_note_order(const void *a, const void *b) {
    const struct note *x = *(const struct note *const *)a;
    const struct note *y = *(const struct note *const *)b;
    uintptr_t left = (uintptr_t)x->output.syntax, right = (uintptr_t)y->output.syntax;
    return left < right ? -1 : left > right ? 1 : 0;
}

MO_MUST_USE int infer_compare_text(const struct mo_syntax *a, const char *b) {
    size_t x = a->span.end - a->span.start, y = strlen(b);
    int order = memcmp(a->text, b, x < y ? x : y);
    return order != 0 ? order : x < y ? -1 : x > y ? 1 : 0;
}

MO_MUST_USE const struct mo_source_type *infer_parameter_type(const struct mo_source_type *type,
                                                              uint32_t bit) {
    if ((type->parameters & bit) == 0)
        return NULL;
    if (type->kind == MO_SOURCE_PARAMETER)
        return type;
    for (uint32_t i = 0; i < type->count; i++) {
        const struct mo_source_type *out = infer_parameter_type(type->arguments[i], bit);
        if (out != NULL)
            return out;
    }
    return NULL;
}
