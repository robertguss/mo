#include "check/check.h"
#include "check/checked.h"
#include "check/data.h"
#include "check/patterns.h"

#include "base/codes.h"
#include "base/limits.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum host_operation {
#define HOSTOP(id, module, name, signature, params, bugs, spec_only) HOST_##id,
#include "base/hostops.def"
};

static const struct host_signature {
    const char *module, *name, *qualified, *type;
} host_signatures[] = {
#define HOSTOP(id, module, name, signature, params, bugs, spec_only)                               \
    {module, name, module "." name, signature},
#include "base/hostops.def"
};

enum numeric { INT = 1, BIG = 2, BYTE = 4, DECIMAL = 8, FLOAT = 16 };

struct term {
    struct mo_source_type shape;
    struct term **arguments;
    struct term *parent;
    const struct mo_source_type *resolved;
    uint32_t rank;
    uint32_t seen;
    uint32_t height; /* longest path to a leaf in a resolved type DAG */
    unsigned numeric;
    bool variable;
    bool partial_default;
};

struct local;
struct name_slot {
    const struct mo_syntax *name;
    const struct mo_syntax *module_use;
    const struct mo_syntax *local_name;
    struct name_slot *left, *right;
    struct local *active;
    struct local *expired;
    uint32_t height;
};

struct local {
    struct name_slot *slot;
    const struct mo_syntax *name;
    struct term *type;
    struct local *previous;
    struct local *next;
    struct note *binding;
    uint32_t function;
    uint32_t pattern_group, pattern_alternative;
    bool variable;
    bool used;
};

struct emitted_lambda {
    struct mo_checked_lambda output;
    struct note **parameters;
};

struct emitted {
    struct mo_checked_expression output;
    struct note **operands;
    struct emitted_lambda *lambda;
};

struct note {
    struct mo_inferred_expression output;
    struct term *type;
    struct note *next;
    struct emitted *graph;
    struct note *binding;
    uint32_t index;
    uint32_t binding_index;
    bool variable;
    bool discarded;
    bool negative;
};

struct note_range {
    struct note *first, *last;
};

struct typed_pattern {
    struct mo_match_pattern match;
    struct term *type;
    const struct mo_syntax *syntax;
    struct typed_pattern *next;
};

struct match_check {
    struct mo_match_pattern **patterns;
    uint32_t count;
    uint32_t end;
    struct mo_span span;
    const struct mo_syntax *scrutinee;
    struct term *type;
    const struct mo_syntax *catch_all;
    struct match_check *next;
};

struct inference {
    struct mo_arena *arena;
    struct mo_diags *diags;
    const struct mo_module_interfaces *interfaces;
    const struct mo_signatures *signatures;
    const struct mo_function_signature *function;
    struct name_slot *names;
    struct local *locals, *all_locals;
    struct note *notes, *last;
    struct typed_pattern *patterns;
    struct match_check *matches;
    struct term *result;
    struct mo_span span;
    struct mo_diag error;
    struct mo_label secondary;
    struct mo_edit edit;
    struct mo_fix fix;
    struct local *unused, *error_unused;
    const char *type_label;
    uint16_t expectation, node_expectation;
    uint32_t count, binding_count, depth, loops, lambda, work, epoch;
    uint32_t pattern_group, pattern_depth, lambda_count;
    bool checked;
    bool keep_patterns;
    bool failed, fatal, partial;
};

struct value {
    struct term *type;
    const struct mo_syntax *binding;
    const struct mo_symbol *symbol;
    const struct mo_field_signature *field;
    const char *builtin_case;
    struct emitted *graph;
    struct note *note;
    struct note *bound;
    bool never;
};

static MO_MUST_USE bool problem(struct inference *p, uint16_t code, struct mo_span span,
                                const char *message, const char *reason,
                                const struct mo_syntax *previous) {
    if (p->fatal || (p->partial && code == MO_CODE_CANNOT_INFER))
        return false;
    p->failed = true;
    p->fatal = code >= 1000 && code != MO_CODE_CHECK_UNFINISHED;
    if (!p->fatal && p->error.code != 0 && p->error.primary.span.start <= span.start)
        return false;
    const char *label =
        code == MO_CODE_NAME_UNKNOWN    ? "not visible in this block"
        : code == MO_CODE_BIND_REPEATED ? "this name already has a meaning"
        : code == MO_CODE_UNUSED_NAME   ? "never read"
        : code == MO_CODE_TYPE_MISMATCH || code == MO_CODE_CONDITION_TYPE ||
                code == MO_CODE_NUMBER_ABILITY || code == MO_CODE_BYTE_ARITHMETIC ||
                code == MO_CODE_ARGUMENT_TYPE || code == MO_CODE_RESULT_TYPE ||
                code == MO_CODE_NOT_FUNCTION
            ? p->type_label
        : code == MO_CODE_CANNOT_INFER         ? "no use decides the type"
        : code == MO_CODE_BOOL_NAME            ? "must be Bool or a function returning Bool"
        : code == MO_CODE_CALL_ARGUMENTS       ? "pass one positional argument per parameter"
        : code == MO_CODE_UNUSED_VALUE         ? "not Unit and not used"
        : code == MO_CODE_NOT_VARIABLE         ? "no assignable var here"
        : code == MO_CODE_EXIT_OUTSIDE_LOOP    ? "no enclosing loop in this function"
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
    p->secondary = (struct mo_label){
        previous == NULL ? span : previous->span,
        code == MO_CODE_BIND_REPEATED ? "the other meaning of this name" : "declared here"};
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
static void defer(struct inference *p, uint16_t code, struct mo_span span, const char *message,
                  const char *reason) {
    bool reported = problem(p, code, span, message, reason, NULL);
    (void)reported;
    p->failed = p->fatal;
}

static void emit_problem(struct inference *p) {
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
            bool reported = problem(p, MO_CODE_CHECK_NO_MEMORY, span,
                                    "the checking arena has no room", NULL, NULL);
            (void)reported;
            emit_problem(p);
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

static MO_MUST_USE bool unfinished(struct inference *p) {
    return problem(p, MO_CODE_CHECK_UNFINISHED, p->span,
                   "this body rule is not implemented by the staged checker", NULL, NULL);
}

static MO_MUST_USE bool work(struct inference *p) {
    if (p->failed)
        return false;
    if (p->work == MO_LIMIT_SOURCE_CHECK_WORK)
        return problem(p, MO_CODE_CHECK_WORK, p->span,
                       "the function needs too much type-checking work", NULL, NULL);
    if (*p->signatures->work == MO_LIMIT_PROGRAM_CHECK_WORK)
        return problem(p, MO_CODE_CHECK_PROGRAM_WORK, p->span,
                       "the program needs too much type-checking work", NULL, NULL);
    p->work++;
    (*p->signatures->work)++;
    return true;
}

static MO_MUST_USE void *allocate(struct inference *p, size_t count, size_t size,
                                  size_t alignment) {
    if (p->failed)
        return NULL;
    void *out = mo_arena_allocate_array(p->arena, count, size, alignment);
    if (out == NULL) {
        bool reported = problem(p, MO_CODE_CHECK_NO_MEMORY, p->span,
                                "the checking arena has no room", NULL, NULL);
        (void)reported;
    }
    return out;
}

static MO_MUST_USE struct term *make_type(struct inference *p, struct mo_source_type shape) {
    if (!work(p))
        return NULL;
    struct term *t = allocate(p, 1, sizeof(*t), _Alignof(struct term));
    if (t == NULL)
        return NULL;
    *t = (struct term){.shape = shape};
    if (shape.count != 0) {
        t->arguments = allocate(p, shape.count, sizeof(*t->arguments), _Alignof(struct term *));
        if (t->arguments == NULL)
            return NULL;
    }
    return t;
}

static MO_MUST_USE struct term *fresh(struct inference *p, unsigned numeric) {
    struct term *t = make_type(p, (struct mo_source_type){0});
    if (t != NULL) {
        t->variable = true;
        t->numeric = numeric;
    }
    return t;
}

static MO_MUST_USE struct term *native(struct inference *p, const char *name, uint32_t count) {
    return make_type(
        p, (struct mo_source_type){
               .kind = MO_SOURCE_NAMED, .name = name, .module = UINT32_MAX, .count = count});
}

static MO_MUST_USE struct term *root(struct term *t) {
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

static MO_MUST_USE bool named(struct term *t, const char *name) {
    t = root(t);
    return !t->variable && t->shape.kind == MO_SOURCE_NAMED && t->shape.module == UINT32_MAX &&
           strcmp(t->shape.name, name) == 0;
}

static MO_MUST_USE unsigned number_type(struct term *t) {
    static const char *const names[] = {"Int", "BigInt", "Byte", "Decimal", "Float"};
    for (unsigned i = 0; i < 5; i++)
        if (named(t, names[i]))
            return 1u << i;
    return 0;
}

static MO_MUST_USE bool type_depth(struct inference *p, uint32_t depth) {
    if (depth < MO_LIMIT_SOURCE_TYPE_DEPTH)
        return true;
    return problem(p, MO_CODE_CHECK_TYPE_DEPTH, p->span, "the inferred type is nested too deeply",
                   NULL, NULL);
}

static MO_MUST_USE struct term *annotation(struct inference *p, const struct mo_source_type *type,
                                           struct term **parameters, uint32_t depth);

/* Frozen annotations borrow their prepared DAG. Materialize only the child a
 * constraint or projection actually reads, not a wide signature on every use. */
static MO_MUST_USE struct term *argument(struct inference *p, struct term *t, uint32_t index) {
    t = root(t);
    MO_ASSERT(index < t->shape.count);
    if (t->arguments != NULL)
        return t->arguments[index];
    MO_ASSERT(t->resolved != NULL);
    return annotation(p, t->resolved->arguments[index], NULL, 0);
}

struct type_preview {
    char text[512];
    size_t length;
};

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

static MO_MUST_USE bool preview_type(struct inference *p, struct type_preview *out, struct term *t,
                                     uint32_t depth) {
    if (out->length + 4 >= sizeof(out->text))
        return true;
    if (!work(p))
        return false;
    t = root(t);
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
        struct term *part = argument(p, t, i);
        if (part == NULL || !preview_type(p, out, part, depth + 1))
            return false;
    }
    preview_text(out, ")");
    if (function && out->length + 4 < sizeof(out->text)) {
        preview_text(out, " : ");
        struct term *result = argument(p, t, count);
        if (result == NULL || !preview_type(p, out, result, depth + 1))
            return false;
    }
    return true;
}

static MO_MUST_USE bool mismatch(struct inference *p, struct term *found, struct term *expected) {
    struct type_preview label = {0};
    preview_text(&label, "expected ");
    if (!preview_type(p, &label, expected, 0))
        return false;
    preview_text(&label, ", found ");
    if (!preview_type(p, &label, found, 0))
        return false;
    char *text = allocate(p, label.length + 1, 1, 1);
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
        reason = "Only Int, BigInt, Decimal and Float have Number; times and durations also add "
                 "and subtract (reference 9.2).";
    } else if (code == MO_CODE_BYTE_ARITHMETIC) {
        message = "Byte is used in arithmetic";
        reason = "Byte has no arithmetic; convert it with to_int first (reference 3.2).";
    } else if (code == MO_CODE_ARGUMENT_TYPE) {
        message = "an argument's type doesn't match its parameter";
        reason = "A call passes each argument to its corresponding parameter (reference 5.2).";
    } else if (code == MO_CODE_RESULT_TYPE) {
        message = "a function's result doesn't match its signature";
        reason = "A function's signature gives its types (reference 5.1).";
    } else if (code == MO_CODE_NOT_FUNCTION) {
        message = "a value that isn't a function is called";
        reason = "A call applies a function to its arguments (reference 5.2).";
    }
    return problem(p, code, p->span, message, reason, NULL);
}

/* Each occurs walk visits a shared type once, not once per path through its DAG. */
static MO_MUST_USE bool occurs(struct inference *p, struct term *needle, struct term *t,
                               uint32_t depth) {
    if (!work(p) || !type_depth(p, depth))
        return true;
    t = root(t);
    if (needle == t)
        return true;
    if (t->resolved != NULL)
        return !type_depth(p, depth + t->height);
    if (t->seen == p->epoch)
        return false;
    t->seen = p->epoch;
    for (uint32_t i = 0; i < t->shape.count; i++)
        if (occurs(p, needle, t->arguments[i], depth + 1))
            return true;
    return false;
}

static MO_MUST_USE bool unify(struct inference *p, struct term *a, struct term *b, uint32_t depth) {
    if (!work(p) || !type_depth(p, depth))
        return false;
    a = root(a);
    b = root(b);
    if (a == b || (a->resolved != NULL && a->resolved == b->resolved))
        return true;
    unsigned numeric = a->numeric == 0   ? b->numeric
                       : b->numeric == 0 ? a->numeric
                                         : a->numeric & b->numeric;
    if (a->numeric != 0 && b->numeric != 0 && numeric == 0)
        return mismatch(p, a, b);
    if ((!a->variable && numeric != 0 && (number_type(a) & numeric) == 0) ||
        (!b->variable && numeric != 0 && (number_type(b) & numeric) == 0))
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
            struct term *x = argument(p, a, i), *y = argument(p, b, i);
            if (x == NULL || y == NULL || !unify(p, x, y, depth + 1))
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
    if (a->rank == b->rank)
        a->rank++;
    return true;
}

static MO_MUST_USE bool unify_as(struct inference *p, struct term *found, struct term *expected,
                                 uint16_t code) {
    uint16_t saved = p->expectation;
    p->expectation = code;
    bool valid = unify(p, found, expected, 0);
    p->expectation = saved;
    return valid;
}

static MO_MUST_USE struct term *annotation(struct inference *p, const struct mo_source_type *type,
                                           struct term **parameters, uint32_t depth) {
    if (!type_depth(p, depth))
        return NULL;
    if (parameters == NULL || !type->parameters) {
        if (!work(p) || !type_depth(p, depth + type->height))
            return NULL;
        struct term *t = allocate(p, 1, sizeof(*t), _Alignof(struct term));
        if (t != NULL)
            *t = (struct term){.shape = *type, .resolved = type, .height = type->height};
        return t;
    }
    if (parameters != NULL && type->kind == MO_SOURCE_PARAMETER) {
        unsigned index = (unsigned)(type->name[0] - 'A');
        MO_ASSERT(index < 26);
        if (parameters[index] == NULL)
            parameters[index] = fresh(p, 0);
        return parameters[index];
    }
    struct term *t = make_type(p, *type);
    if (t == NULL)
        return NULL;
    for (uint32_t i = 0; i < type->count; i++) {
        t->arguments[i] = annotation(p, type->arguments[i], parameters, depth + 1);
        if (t->arguments[i] == NULL)
            return NULL;
    }
    return t;
}

static MO_MUST_USE int compare_name(const struct mo_syntax *a, const struct mo_syntax *b) {
    size_t x = a->span.end - a->span.start, y = b->span.end - b->span.start;
    int order = memcmp(a->text, b->text, x < y ? x : y);
    return order != 0 ? order : x < y ? -1 : x > y ? 1 : 0;
}

static MO_MUST_USE struct value graph(struct inference *p, struct value v, uint32_t count) {
    if (!p->checked || v.type == NULL)
        return v;
    if (!work(p))
        return (struct value){0};
    struct emitted *out = allocate(p, 1, sizeof(*out), _Alignof(struct emitted));
    struct note **operands = allocate(p, count, sizeof(*operands), _Alignof(struct note *));
    if (p->failed)
        return (struct value){0};
    *out = (struct emitted){
        .output = {.operand_count = count, .field = UINT32_MAX, .receiver = UINT32_MAX},
        .operands = operands};
    v.graph = out;
    return v;
}

static MO_MUST_USE bool record(struct inference *p, const struct mo_syntax *node, struct value v) {
    struct note *n = allocate(p, 1, sizeof(*n), _Alignof(struct note));
    if (n == NULL)
        return false;
    *n = (struct note){.output = {.syntax = node,
                                  .binding = v.binding,
                                  .symbol = v.symbol,
                                  .field = v.field,
                                  .builtin_case = v.builtin_case,
                                  .never = v.never},
                       .type = v.type,
                       .graph = v.graph,
                       .binding = v.bound,
                       .index = UINT32_MAX,
                       .binding_index = UINT32_MAX};
    if (p->last == NULL)
        p->notes = n;
    else
        p->last->next = n;
    p->last = n;
    p->count++;
    return true;
}

static MO_MUST_USE int source_note_order(const void *a, const void *b) {
    const struct note *x = *(const struct note *const *)a;
    const struct note *y = *(const struct note *const *)b;
    uint32_t left = x->output.syntax->span.start, right = y->output.syntax->span.start;
    return left < right ? -1 : left > right ? 1 : 0;
}

static MO_MUST_USE int compare_text(const struct mo_syntax *a, const char *b) {
    size_t x = a->span.end - a->span.start, y = strlen(b);
    int order = memcmp(a->text, b, x < y ? x : y);
    return order != 0 ? order : x < y ? -1 : x > y ? 1 : 0;
}

static MO_MUST_USE uint32_t height(const struct name_slot *s) {
    return s == NULL ? 0 : s->height;
}

static void refresh(struct name_slot *s) {
    uint32_t a = height(s->left), b = height(s->right);
    s->height = 1 + (a > b ? a : b);
}

static MO_MUST_USE struct name_slot *rotate(struct name_slot *s, bool right) {
    struct name_slot *next = right ? s->left : s->right;
    if (right) {
        s->left = next->right;
        next->right = s;
    } else {
        s->right = next->left;
        next->left = s;
    }
    refresh(s);
    refresh(next);
    return next;
}

static MO_MUST_USE struct name_slot *insert(struct name_slot *s, struct name_slot *entry) {
    if (s == NULL)
        return entry;
    int order = compare_name(entry->name, s->name);
    MO_ASSERT(order != 0);
    if (order < 0)
        s->left = insert(s->left, entry);
    else
        s->right = insert(s->right, entry);
    refresh(s);
    if (height(s->left) > height(s->right) + 1) {
        if (height(s->left->right) > height(s->left->left))
            s->left = rotate(s->left, false);
        return rotate(s, true);
    }
    if (height(s->right) > height(s->left) + 1) {
        if (height(s->right->left) > height(s->right->right))
            s->right = rotate(s->right, true);
        return rotate(s, false);
    }
    return s;
}

static MO_MUST_USE struct name_slot *slot(struct inference *p, const struct mo_syntax *name,
                                          bool create) {
    struct name_slot *at = p->names;
    while (at != NULL) {
        int order = compare_name(name, at->name);
        if (order == 0)
            return at;
        at = order < 0 ? at->left : at->right;
    }
    if (!create)
        return NULL;
    struct name_slot *entry = allocate(p, 1, sizeof(*entry), _Alignof(struct name_slot));
    if (entry != NULL) {
        *entry = (struct name_slot){.name = name, .height = 1};
        p->names = insert(p->names, entry);
    }
    return entry;
}

static MO_MUST_USE bool record_binding(struct inference *p, const struct mo_syntax *name,
                                       struct term *type, bool variable) {
    if (!record(p, name, (struct value){.type = type, .binding = name}))
        return false;
    if (p->checked) {
        p->last->binding = p->last;
        p->last->variable = variable;
        p->binding_count++;
    }
    return true;
}

static MO_MUST_USE bool bind(struct inference *p, const struct mo_syntax *name, struct term *type,
                             bool variable) {
    if (name->kind == MO_S_UNDERSCORE)
        return record_binding(p, name, type, variable);
    struct name_slot *s = slot(p, name, true);
    if (s == NULL)
        return false;
    if (s->active != NULL)
        return problem(p, MO_CODE_BIND_REPEATED, name->span, "a visible name is bound again",
                       "While a local name or parameter is visible it can't be bound again, "
                       "even in an inner block (reference 6.1).",
                       s->active->name);
    if (s->module_use != NULL)
        return problem(p, MO_CODE_BIND_REPEATED, name->span,
                       "a name means both a local and a module function",
                       "A function can't refer to its module's function by a name it uses for "
                       "a local (reference 6.1).",
                       s->module_use);
    struct local *l = allocate(p, 1, sizeof(*l), _Alignof(struct local));
    if (l == NULL)
        return false;
    *l = (struct local){.slot = s,
                        .name = name,
                        .type = type,
                        .variable = variable,
                        .previous = p->locals,
                        .next = p->all_locals,
                        .function = p->lambda};
    s->local_name = name;
    s->active = l;
    p->locals = l;
    p->all_locals = l;
    if (!record_binding(p, name, type, variable))
        return false;
    l->binding = p->last;
    return true;
}

static MO_MUST_USE bool leave(struct inference *p, struct local *mark) {
    while (p->locals != mark) {
        struct local *l = p->locals;
        if (!l->used && l->name->text[0] != '_') {
            p->unused = l;
            defer(p, MO_CODE_UNUSED_NAME, l->name->span, "a name or parameter is unused",
                  "A name starting with _ is unused on purpose (reference 6.1).");
        }
        l->slot->active = NULL;
        l->slot->expired = l;
        p->locals = l->previous;
    }
    return true;
}

static MO_MUST_USE const struct mo_symbol *
find_symbol(const struct mo_module_scope *scope, const struct mo_syntax *name, uint32_t *count) {
    const struct mo_symbol *s =
        mo_find_symbol(scope, name->text, name->span.end - name->span.start);
    if (s != NULL)
        *count = s->overload_count;
    return s;
}

static MO_MUST_USE const struct mo_data_signature *data_type(struct inference *p,
                                                             const struct mo_symbol *symbol) {
    uint32_t lo = 0, hi = p->signatures->data_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const struct mo_symbol *at = p->signatures->data[mid].symbol;
        if (at->module < symbol->module ||
            (at->module == symbol->module && (uintptr_t)at < (uintptr_t)symbol))
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < p->signatures->data_count && p->signatures->data[lo].symbol == symbol
               ? &p->signatures->data[lo]
               : NULL;
}

static MO_MUST_USE bool visible_data(struct inference *p, const struct mo_symbol *owner) {
    if (owner->module == p->function->function->module || owner->insides)
        return true;
    return problem(p, MO_CODE_DATA_PRIVATE, p->span, "the type's fields or cases are hidden",
                   "Outside its module a type exposed without (..) can't be built, taken apart "
                   "or have its fields read (reference 10.2).",
                   owner->declaration->children[0]);
}

static MO_MUST_USE const struct mo_field_signature *find_field(const struct mo_data_signature *data,
                                                               const struct mo_syntax *name) {
    if (data->unnamed)
        return NULL;
    uint32_t lo = 0, hi = data->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (compare_name(data->fields[mid].name, name) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < data->count && compare_name(data->fields[lo].name, name) == 0 ? &data->fields[lo]
                                                                              : NULL;
}

static MO_MUST_USE const struct mo_symbol *definition(struct inference *p,
                                                      const struct mo_syntax *name,
                                                      const struct mo_syntax *prefix,
                                                      uint32_t *count) {
    const struct mo_module_scope *scope = &p->interfaces->scopes[p->function->function->module];
    if (name->kind == MO_S_QUALIFIED) {
        prefix = name->children[0];
        name = name->children[1];
    }
    if (prefix != NULL) {
        uint32_t lo = 0, hi = scope->alias_count;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (compare_text(prefix, scope->aliases[mid].name) > 0)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo == scope->alias_count || compare_text(prefix, scope->aliases[lo].name) != 0) {
            uint32_t meanings = 0;
            const struct mo_symbol *type = definition(p, prefix, NULL, &meanings);
            size_t length = prefix->span.end - prefix->span.start;
            bool parameter =
                length == 1 && (p->function->type->parameters &
                                (UINT32_C(1) << (unsigned)(prefix->text[0] - 'A'))) != 0;
            if (type != NULL || mo_source_builtin(prefix->text, length) || parameter)
                return NULL; /* static type/builtin dispatch is a later stage */
            bool reported =
                problem(p, MO_CODE_IMPORT_MISSING, prefix->span, "a module prefix isn't in scope",
                        "Use names the modules available to this file (reference 10.3).", NULL);
            (void)reported;
            return NULL;
        }
        const struct mo_symbol *s =
            find_symbol(&p->interfaces->scopes[scope->aliases[lo].module], name, count);
        if (s != NULL && s->owner != UINT32_MAX) {
            const struct mo_symbol *owner = &p->interfaces->scopes[s->module].symbols[s->owner];
            if (owner->exported && !visible_data(p, owner))
                return NULL;
        }
        if (s != NULL && !s->exported) {
            bool reported = problem(p, MO_CODE_IMPORT_PRIVATE, name->span,
                                    "everything not on expose is private to the module",
                                    "Everything not on expose is private to the module "
                                    "(reference 10.1).",
                                    s->declaration->children[0]);
            (void)reported;
            return NULL;
        }
        return s;
    }
    const struct mo_symbol *s = find_symbol(scope, name, count);
    if (s != NULL)
        return s;
    uint32_t lo = 0, hi = scope->imported_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (compare_text(name, scope->imported[mid].name) > 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == scope->imported_count || compare_text(name, scope->imported[lo].name) != 0)
        return NULL;
    *count = scope->imported[lo].count;
    return scope->imported[lo].symbols;
}

static MO_MUST_USE struct value builtin_construct(struct inference *p,
                                                  const struct mo_builtin_case *data,
                                                  const struct mo_syntax *args,
                                                  struct term *expected);

static MO_MUST_USE bool missing_field(struct inference *p, struct mo_span span, const uint8_t *name,
                                      size_t length, const struct mo_syntax *declaration) {
    char *message = allocate(p, 256, 1, 1);
    if (message == NULL)
        return false;
    int n = name == NULL ? snprintf(message, 256, "the case is missing its unnamed value")
                         : snprintf(message, 256, "the record or case is missing field '%.*s%s'",
                                    (int)(length > 128 ? 128 : length), (const char *)name,
                                    length > 128 ? "..." : "");
    MO_ASSERT(n > 0 && n < 256);
    return problem(p, MO_CODE_DATA_ARGUMENTS, span, message,
                   "There are no optional fields; a record or case is built with "
                   "every field (reference 4).",
                   declaration);
}

static MO_MUST_USE struct value identifier(struct inference *p, const struct mo_syntax *node,
                                           const struct mo_syntax *prefix) {
    if (prefix == NULL && node->kind == MO_S_NAME) {
        struct name_slot *s = slot(p, node, false);
        if (s != NULL && s->active != NULL) {
            if (p->checked && s->active->function != p->lambda) {
                bool reported = unfinished(p);
                (void)reported;
                return (struct value){0};
            }
            s->active->used = true;
            return (struct value){
                .type = s->active->type, .binding = s->active->name, .bound = s->active->binding};
        }
    }
    const struct mo_builtin_case *builtin = prefix == NULL ? mo_builtin_case_by_name(node) : NULL;
    if (builtin != NULL)
        return builtin_construct(p, builtin, NULL, NULL);
    uint32_t count = 0;
    const struct mo_symbol *s = definition(p, node, prefix, &count);
    if (p->failed)
        return (struct value){0};
    if (s == NULL) {
        if (prefix != NULL || node->kind == MO_S_CAPNAME || node->kind == MO_S_QUALIFIED) {
            bool reported = unfinished(p);
            (void)reported;
        } else {
            struct name_slot *entry = slot(p, node, false);
            if (entry != NULL && entry->expired != NULL && p->error.code == MO_CODE_UNUSED_NAME &&
                p->error_unused == entry->expired) {
                /* An attempted read outside the binding's scope explains why
                 * it looked unused. Report that use, not a destructive rename. */
                p->error.code = 0;
            }
            bool reported =
                problem(p, MO_CODE_NAME_UNKNOWN, node->span, "a name isn't visible in this block",
                        "A local name is visible from its binding to the end of its "
                        "block (reference 6.1).",
                        NULL);
            (void)reported;
        }
        return (struct value){0};
    }
    if (s->declaration->kind == MO_S_STRUCT || s->declaration->kind == MO_S_CASE_DECL) {
        const struct mo_data_signature *data = data_type(p, s);
        MO_ASSERT(data != NULL);
        if (!visible_data(p, data->type->declaration))
            return (struct value){0};
        if (data->count == 0) {
            struct term *parameters[26] = {NULL};
            return (struct value){.type = annotation(p, data->type, parameters, 0), .symbol = s};
        }
        const struct mo_syntax *field = data->by_index[0]->name;
        bool reported =
            missing_field(p, node->span, field == NULL ? NULL : field->text,
                          field == NULL ? 0 : field->span.end - field->span.start, field);
        (void)reported;
        return (struct value){0};
    }
    if (s->declaration->kind != MO_S_FN || count != 1) {
        bool reported = unfinished(p);
        (void)reported;
        return (struct value){0};
    }
    if (prefix == NULL && node->kind == MO_S_NAME) {
        struct name_slot *entry = slot(p, node, true);
        if (entry == NULL)
            return (struct value){0};
        if (entry->local_name != NULL) {
            bool reported = problem(p, MO_CODE_BIND_REPEATED, node->span,
                                    "a name means both a local and a module function",
                                    "A function can't refer to its module's function by a name "
                                    "it uses for a local (reference 6.1).",
                                    entry->local_name);
            (void)reported;
            return (struct value){0};
        }
        entry->module_use = node;
    }
    uint32_t lo = 0, hi = p->signatures->count;
    uintptr_t wanted = (uintptr_t)s;
    /* Signatures follow the interface arrays: module, then symbol address. */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const struct mo_symbol *at = p->signatures->items[mid].function;
        if (at->module < s->module || (at->module == s->module && (uintptr_t)at < wanted))
            lo = mid + 1;
        else
            hi = mid;
    }
    MO_ASSERT(lo < p->signatures->count && p->signatures->items[lo].function == s);
    if (p->checked && p->signatures->items[lo].type->parameters != 0) {
        bool reported = unfinished(p);
        (void)reported;
        return (struct value){0};
    }
    struct term *parameters[26] = {NULL};
    return (struct value){.type = annotation(p, p->signatures->items[lo].type, parameters, 0),
                          .symbol = s};
}

static MO_MUST_USE struct value infer(struct inference *p, const struct mo_syntax *node,
                                      struct term *expected);

static MO_MUST_USE struct value infer_as(struct inference *p, const struct mo_syntax *node,
                                         struct term *expected, uint16_t code) {
    uint16_t saved = p->expectation;
    p->expectation = code;
    struct value v = infer(p, node, expected);
    p->expectation = saved;
    return v;
}

static MO_MUST_USE struct value discard(struct inference *p, const struct mo_syntax *node) {
    struct value v = infer(p, node, NULL);
    if (v.type != NULL) {
        p->last->discarded = !v.never;
        v.type = native(p, "Unit", 0);
    }
    return v;
}

static MO_MUST_USE bool lambda_argument(const struct mo_syntax *node) {
    while (node->kind == MO_S_GROUP)
        node = node->children[0];
    return node->kind == MO_S_LAMBDA;
}

/* Constraint order must never become evaluation order in output metadata. */
static void order_notes(struct inference *p, struct note *before, const struct note_range *ranges,
                        uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (before == NULL)
            p->notes = ranges[i].first;
        else
            before->next = ranges[i].first;
        before = ranges[i].last;
    }
    if (before != NULL)
        before->next = NULL;
    p->last = before;
}

static MO_MUST_USE struct term *
builtin_type(struct inference *p, const struct mo_builtin_case *data, struct term *expected) {
    struct term *type = native(p, data->type, data->parameters);
    if (type == NULL)
        return NULL;
    for (uint32_t i = 0; i < data->parameters; i++) {
        type->arguments[i] = fresh(p, 0);
        if (type->arguments[i] == NULL)
            return NULL;
    }
    if (expected != NULL && !unify(p, type, expected, 0))
        return NULL;
    return type;
}

static MO_MUST_USE uint32_t builtin_field(const struct mo_builtin_case *data,
                                          const struct mo_syntax *arg) {
    if (data->fields[0] == NULL)
        return arg->kind == MO_S_NAMED_ARG ? UINT32_MAX : 0;
    if (arg->kind == MO_S_NAMED_ARG)
        for (uint32_t i = 0; i < data->count; i++)
            if (compare_text(arg->children[0], data->fields[i]) == 0)
                return i;
    return UINT32_MAX;
}

static MO_MUST_USE struct value builtin_construct(struct inference *p,
                                                  const struct mo_builtin_case *data,
                                                  const struct mo_syntax *args,
                                                  struct term *expected) {
    struct term *type = builtin_type(p, data, expected);
    if (type == NULL)
        return (struct value){0};
    if ((args == NULL ? 0 : args->count) < data->count) {
        bool provided[2] = {false, false};
        for (uint32_t i = 0; args != NULL && i < args->count; i++) {
            uint32_t index = builtin_field(data, args->children[i]);
            if (index < data->count)
                provided[index] = true;
        }
        for (uint32_t i = 0; i < data->count; i++) {
            if (provided[i])
                continue;
            const char *name = data->fields[i];
            bool reported =
                missing_field(p, args == NULL ? p->span : args->span, (const uint8_t *)name,
                              name == NULL ? 0 : strlen(name), NULL);
            (void)reported;
            return (struct value){0};
        }
        MO_ASSERT(false);
    }
    if ((args == NULL ? 0 : args->count) != data->count) {
        bool reported = problem(p, MO_CODE_DATA_ARGUMENTS, args == NULL ? p->span : args->span,
                                "the case hasn't been given every field",
                                "A case is built with every named field or its one unnamed "
                                "value (reference 4).",
                                NULL);
        (void)reported;
        return (struct value){0};
    }
    bool seen[2] = {false, false};
    bool never = false;
    for (uint32_t i = 0; args != NULL && i < args->count; i++) {
        const struct mo_syntax *arg = args->children[i];
        uint32_t index = builtin_field(data, arg);
        if (index == UINT32_MAX || seen[index]) {
            bool reported = problem(p, MO_CODE_DATA_ARGUMENTS, arg->span,
                                    "a supplied field doesn't match the case",
                                    "A case is built with every named field once or its "
                                    "one unnamed value (reference 4).",
                                    NULL);
            (void)reported;
            return (struct value){0};
        }
        seen[index] = true;
        struct term *field =
            data->fields[0] == NULL ? type->arguments[data->parameter] : native(p, "Text", 0);
        if (field == NULL)
            return (struct value){0};
        struct value v = infer(p, arg->kind == MO_S_NAMED_ARG ? arg->children[1] : arg, field);
        if (v.type == NULL)
            return (struct value){0};
        never = never || v.never;
    }
    return (struct value){.type = type, .builtin_case = data->name, .never = never};
}

static MO_MUST_USE struct value construct(struct inference *p, const struct mo_data_signature *data,
                                          const struct mo_syntax *args, struct term *expected) {
    if (!visible_data(p, data->type->declaration))
        return (struct value){0};
    struct term *parameters[26] = {NULL};
    struct term *type = annotation(p, data->type, parameters, 0);
    if (type == NULL || (expected != NULL && !unify(p, type, expected, 0)))
        return (struct value){0};
    bool *seen = allocate(p, data->count, sizeof(*seen), _Alignof(bool));
    const struct mo_field_signature **selected =
        allocate(p, args->count, sizeof(*selected), _Alignof(const struct mo_field_signature *));
    struct note_range *ranges =
        allocate(p, args->count, sizeof(*ranges), _Alignof(struct note_range));
    if (p->failed)
        return (struct value){0};
    memset(seen, 0, data->count * sizeof(*seen));
    for (uint32_t i = 0; i < args->count; i++) {
        const struct mo_syntax *arg = args->children[i];
        const struct mo_field_signature *field =
            data->unnamed && arg->kind != MO_S_NAMED_ARG ? &data->fields[0]
            : arg->kind == MO_S_NAMED_ARG                ? find_field(data, arg->children[0])
                                                         : NULL;
        if (field == NULL || seen[field->index]) {
            bool reported =
                problem(p, MO_CODE_DATA_ARGUMENTS, arg->span,
                        "a supplied field doesn't match the record or case",
                        "A record or case is built by naming every field once, or "
                        "from its one unnamed value (reference 4).",
                        field == NULL ? data->symbol->declaration->children[0] : field->name);
            (void)reported;
            return (struct value){0};
        }
        seen[field->index] = true;
        selected[i] = field;
    }
    for (uint32_t i = 0; i < data->count; i++) {
        if (seen[i])
            continue;
        const struct mo_syntax *name = data->by_index[i]->name;
        bool reported = missing_field(p, args->span, name == NULL ? NULL : name->text,
                                      name == NULL ? 0 : name->span.end - name->span.start, name);
        (void)reported;
        return (struct value){0};
    }
    bool never = false;
    struct note *before = p->last;
    for (unsigned pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < args->count; i++) {
            const struct mo_syntax *arg = args->children[i];
            const struct mo_syntax *value = data->unnamed ? arg : arg->children[1];
            if (lambda_argument(value) != (pass != 0))
                continue;
            struct term *wanted = annotation(p, selected[i]->type, parameters, 0);
            if (wanted == NULL)
                return (struct value){0};
            struct note *previous = p->last;
            struct value v = infer_as(p, value, wanted, MO_CODE_ARGUMENT_TYPE);
            if (v.type == NULL)
                return (struct value){0};
            p->last->output.field = selected[i];
            ranges[i] = (struct note_range){previous == NULL ? p->notes : previous->next, p->last};
            never = never || v.never;
        }
    }
    order_notes(p, before, ranges, args->count);
    return (struct value){.type = type, .symbol = data->symbol, .never = never};
}

static MO_MUST_USE struct value field_value(struct inference *p, struct value v,
                                            const struct mo_syntax *name) {
    if (v.type == NULL)
        return v;
    p->span = name->span;
    struct term *type = root(v.type);
    if (type->variable || type->shape.kind != MO_SOURCE_NAMED || type->shape.declaration == NULL) {
        bool reported = unfinished(p);
        (void)reported;
        return (struct value){0};
    }
    const struct mo_data_signature *data = data_type(p, type->shape.declaration);
    const struct mo_field_signature *field = data == NULL ? NULL : find_field(data, name);
    bool visible =
        type->shape.module == p->function->function->module || type->shape.declaration->insides;
    if (field == NULL || !visible) {
        uint32_t count = 0;
        const struct mo_symbol *method =
            find_symbol(&p->interfaces->scopes[type->shape.module], name, &count);
        bool plausible = method != NULL && method->declaration->kind == MO_S_FN &&
                         (method->module == p->function->function->module || method->exported);
        if (!plausible) {
            method = definition(p, name, NULL, &count);
            plausible = method != NULL && method->declaration->kind == MO_S_FN;
        }
        if (plausible || compare_text(name, "to_text") == 0 || compare_text(name, "compare") == 0) {
            bool reported = unfinished(p);
            (void)reported;
            return (struct value){0};
        }
    }
    if (!visible_data(p, type->shape.declaration))
        return (struct value){0};
    if (field == NULL) {
        bool reported =
            problem(p, MO_CODE_FIELD_MISSING, name->span, "the record has no such field",
                    "A struct declares named fields, each with a type (reference 4.1).",
                    type->shape.declaration->declaration->children[0]);
        (void)reported;
        return (struct value){0};
    }
    struct term *parameters[26] = {NULL};
    for (uint32_t i = 0; i < type->shape.count; i++) {
        MO_ASSERT(data->parameters != NULL && i < data->parameters->count);
        parameters[data->parameters->children[i]->text[0] - 'A'] = argument(p, type, i);
        if (p->failed)
            return (struct value){0};
    }
    return (struct value){
        .type = annotation(p, field->type, parameters, 0), .field = field, .never = v.never};
}

static MO_MUST_USE struct value sequence(struct inference *p, const struct mo_syntax *node,
                                         struct term *expected) {
    struct value last = {.type = native(p, "Unit", 0)};
    struct value emitted = graph(p, last, node->count);
    if (emitted.type == NULL)
        return (struct value){0};
    bool never = false;
    for (uint32_t i = 0; i < node->count; i++) {
        last = infer_as(p, node->children[i], i + 1 == node->count && !never ? expected : NULL,
                        p->node_expectation);
        if (last.type == NULL)
            return (struct value){0};
        if (p->checked)
            emitted.graph->operands[i] = last.note;
        never = never || last.never;
        /* The last note belongs to this statement, not its nested expressions. */
        if (i + 1 != node->count)
            p->last->discarded = !last.never;
    }
    last.never = never;
    last.graph = emitted.graph;
    return last;
}

static MO_MUST_USE struct value call(struct inference *p, struct value callee,
                                     const struct mo_syntax *args) {
    if (callee.type == NULL)
        return (struct value){0};
    struct term *t = root(callee.type);
    if (t->variable) {
        struct term *fn = make_type(p, (struct mo_source_type){.kind = MO_SOURCE_FUNCTION,
                                                               .name = "fn",
                                                               .module = UINT32_MAX,
                                                               .count = args->count + 1});
        if (fn == NULL)
            return (struct value){0};
        for (uint32_t i = 0; i < fn->shape.count; i++) {
            fn->arguments[i] = fresh(p, 0);
            if (fn->arguments[i] == NULL)
                return (struct value){0};
        }
        if (!unify(p, t, fn, 0))
            return (struct value){0};
        t = root(fn);
    }
    if (t->shape.kind != MO_SOURCE_FUNCTION) {
        if (callee.binding != NULL) {
            uint32_t count = 0;
            const struct mo_symbol *symbol = definition(p, callee.binding, NULL, &count);
            if (symbol != NULL && symbol->declaration->kind == MO_S_FN) {
                bool reported = problem(p, MO_CODE_BIND_REPEATED, p->span,
                                        "a name means both a local and a module function",
                                        "A function can't refer to a module function by a name it "
                                        "uses for a local (reference 6.1).",
                                        callee.binding);
                (void)reported;
                return (struct value){0};
            }
        }
        struct term function = {.shape = {.kind = MO_SOURCE_FUNCTION, .name = "fn"}};
        bool valid = unify_as(p, t, &function, MO_CODE_NOT_FUNCTION);
        (void)valid;
        return (struct value){0};
    }
    if (t->shape.count != args->count + 1) {
        bool reported = problem(p, MO_CODE_CALL_ARGUMENTS,
                                args->span.start == args->span.end ? p->span : args->span,
                                "a call's arguments don't match its parameters",
                                "A function has no default or optional parameters; pass every "
                                "argument by position (reference 5.2).",
                                NULL);
        (void)reported;
        return (struct value){0};
    }
    bool never = callee.never;
    for (uint32_t i = 0; i < args->count; i++)
        if (args->children[i]->kind == MO_S_NAMED_ARG) {
            bool reported = problem(p, MO_CODE_CALL_ARGUMENTS, args->children[i]->span,
                                    "a function is called with named fields",
                                    "Arguments are passed by position; only records and cases "
                                    "are built with named fields (reference 5.2).",
                                    NULL);
            (void)reported;
            return (struct value){0};
        }
    struct note_range *ranges =
        allocate(p, args->count, sizeof(*ranges), _Alignof(struct note_range));
    if (ranges == NULL)
        return (struct value){0};
    struct value result = {0};
    if (p->checked) {
        result =
            graph(p, (struct value){.type = argument(p, t, t->shape.count - 1)}, args->count + 1);
        if (result.type == NULL)
            return result;
        result.graph->operands[0] = callee.note;
        result.graph->output.symbol = callee.symbol;
    }
    struct note *before = p->last;
    for (unsigned pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < args->count; i++) {
            if (lambda_argument(args->children[i]) != (pass != 0))
                continue;
            struct note *previous = p->last;
            struct value arg =
                infer_as(p, args->children[i], argument(p, t, i), MO_CODE_ARGUMENT_TYPE);
            if (arg.type == NULL)
                return (struct value){0};
            ranges[i] = (struct note_range){previous == NULL ? p->notes : previous->next, p->last};
            if (p->checked)
                result.graph->operands[i + 1] = arg.note;
            never = never || arg.never;
        }
    }
    /* Constraint solving may visit lambdas last; metadata retains written order
     * so this never suggests reordering evaluation to downstream consumers. */
    order_notes(p, before, ranges, args->count);
    if (!p->checked)
        result.type = argument(p, t, t->shape.count - 1);
    result.never = never;
    return result;
}

/* This first host slice accepts a direct Console.print call, not a bound
 * method value. Its named parameter types come from the shared host table. */
static MO_MUST_USE struct value console_print(struct inference *p, const struct mo_syntax *node) {
    const struct host_signature *host = &host_signatures[HOST_CONSOLE_PRINT];
    if (node->count != 3 || node->children[1]->kind != MO_S_FIELD_ACCESS ||
        node->children[2]->kind != MO_S_CALL ||
        compare_text(node->children[1]->children[0], host->name) != 0) {
        bool reported = unfinished(p);
        (void)reported;
        return (struct value){0};
    }
    struct value receiver = infer(p, node->children[0], NULL);
    if (receiver.type == NULL)
        return receiver;
    if (!named(receiver.type, host->module)) {
        bool reported = unfinished(p);
        (void)reported;
        return (struct value){0};
    }
    const struct mo_syntax *args = node->children[2]->children[0];
    if (args->count != 1 || args->children[0]->kind == MO_S_NAMED_ARG) {
        bool reported = problem(
            p, MO_CODE_CALL_ARGUMENTS, node->span, "a call's arguments don't match its parameters",
            "A function takes every argument by position (reference 5.2).", NULL);
        (void)reported;
        return (struct value){0};
    }
    /* The selected table row has two plain named inputs and no written result.
     * Composite signatures are left to the complete native-interface stage. */
    const char *start = strchr(host->type, ',');
    MO_ASSERT(start != NULL && start[1] == ' ');
    start += 2;
    const char *end = strchr(start, ')');
    MO_ASSERT(end != NULL && end[1] == '\0');
    size_t length = (size_t)(end - start);
    char *name = allocate(p, length + 1, 1, 1);
    if (name == NULL)
        return (struct value){0};
    memcpy(name, start, length);
    name[length] = '\0';
    struct term *text = native(p, name, 0);
    struct value argument = text == NULL
                                ? (struct value){0}
                                : infer_as(p, args->children[0], text, MO_CODE_ARGUMENT_TYPE);
    if (argument.type == NULL)
        return argument;
    struct value out = graph(
        p, (struct value){.type = native(p, "Unit", 0), .never = receiver.never || argument.never},
        2);
    if (out.type != NULL) {
        out.graph->output.native = host->qualified;
        out.graph->output.receiver = 0; /* operand slot until graph finalization */
        out.graph->operands[0] = receiver.note;
        out.graph->operands[1] = argument.note;
    }
    return out;
}

static MO_MUST_USE struct value condition(struct inference *p, const struct mo_syntax *node,
                                          bool keep);

static MO_MUST_USE struct value binary(struct inference *p, enum mo_syntax_token op, struct value a,
                                       const struct mo_syntax *right, struct term *expected) {
    if (a.type == NULL)
        return (struct value){0};
    bool logical = op == MO_T_AND || op == MO_T_OR || op == MO_T_IMPLIES;
    bool comparison = op == MO_T_EQ || op == MO_T_NE || op == MO_T_LT || op == MO_T_LE ||
                      op == MO_T_GT || op == MO_T_GE;
    if (logical && op == MO_T_OR && named(a.type, "Option")) {
        bool reported = unfinished(p);
        (void)reported;
        return (struct value){0};
    }
    struct term *operand = logical             ? native(p, "Bool", 0)
                           : op == MO_T_DOTDOT ? native(p, "Int", 0)
                                               : a.type;
    if (operand == NULL || !unify_as(p, a.type, operand, logical ? MO_CODE_CONDITION_TYPE : 0))
        return (struct value){0};
    if (!logical && !comparison && op != MO_T_DOTDOT) {
        if (root(operand)->shape.kind == MO_SOURCE_PARAMETER || named(operand, "Time") ||
            named(operand, "Duration")) {
            bool reported = unfinished(p);
            (void)reported;
            return (struct value){0};
        }
        struct term *numeric = fresh(p, INT | BIG | DECIMAL | FLOAT);
        if (numeric == NULL ||
            !unify_as(p, operand, numeric,
                      named(operand, "Byte") ? MO_CODE_BYTE_ARITHMETIC : MO_CODE_NUMBER_ABILITY))
            return (struct value){0};
        if (expected != NULL && !unify_as(p, operand, expected, p->node_expectation))
            return (struct value){0};
    }
    struct value b = logical ? condition(p, right, op == MO_T_AND) : infer(p, right, operand);
    if (b.type == NULL)
        return (struct value){0};
    struct term *result = logical || comparison ? native(p, "Bool", 0)
                          : op == MO_T_DOTDOT   ? native(p, "Range", 0)
                                                : operand;
    struct value out =
        graph(p, (struct value){.type = result, .never = a.never || (!logical && b.never)}, 2);
    if (p->checked && out.type != NULL) {
        out.graph->operands[0] = a.note;
        out.graph->operands[1] = b.note;
        out.graph->output.operation = op;
        out.graph->output.or_kind = op == MO_T_OR ? MO_CHECK_OR_BOOL : MO_CHECK_OR_ABSENT;
    }
    return out;
}

static MO_MUST_USE struct mo_match_pattern *
match_pattern(struct inference *p, const struct mo_syntax *node, struct term *type, uintptr_t key,
              uint32_t signature, uint32_t count, const char *name) {
    if (!work(p))
        return NULL;
    if (count > MO_LIMIT_SOURCE_CHECK_WORK - p->work) {
        bool reported = problem(p, MO_CODE_CHECK_WORK, p->span,
                                "the function needs too much pattern-checking work", NULL, NULL);
        (void)reported;
        return NULL;
    }
    if (count > MO_LIMIT_PROGRAM_CHECK_WORK - *p->signatures->work) {
        bool reported = problem(p, MO_CODE_CHECK_PROGRAM_WORK, p->span,
                                "the program needs too much pattern-checking work", NULL, NULL);
        (void)reported;
        return NULL;
    }
    p->work += count;
    *p->signatures->work += count;
    struct typed_pattern *out = allocate(p, 1, sizeof(*out), _Alignof(struct typed_pattern));
    struct mo_match_pattern **items =
        allocate(p, count, sizeof(*items), _Alignof(struct mo_match_pattern *));
    if (p->failed)
        return NULL;
    memset(items, 0, count * sizeof(*items));
    *out = (struct typed_pattern){
        .match = {.key = key, .signature = signature, .count = count, .name = name, .items = items},
        .type = type,
        .syntax = node,
        .next = p->patterns};
    p->patterns = out;
    return &out->match;
}

static MO_MUST_USE bool pattern_bind(struct inference *p, const struct mo_syntax *node,
                                     struct term *type, uint32_t group, uint32_t alternative) {
    struct name_slot *entry = node->kind == MO_S_NAME ? slot(p, node, false) : NULL;
    struct local *local = entry == NULL ? NULL : entry->active;
    if (alternative > 1 && node->kind != MO_S_UNDERSCORE &&
        (local == NULL || local->pattern_group != group))
        return problem(p, MO_CODE_PATTERN_ALTERNATIVES, node->span,
                       "the alternatives don't bind the same names",
                       "Arms with the same body share a line; their names must be visible "
                       "throughout that body (reference 7.2).",
                       NULL);
    if (alternative > 1 && local != NULL && local->pattern_group == group &&
        local->pattern_alternative != alternative) {
        if (!unify(p, local->type, type, 0))
            return false;
        local->pattern_alternative = alternative;
        return record(p, node, (struct value){.type = type, .binding = local->name});
    } else {
        if (!bind(p, node, type, false))
            return false;
        if (node->kind != MO_S_UNDERSCORE) {
            local = p->locals;
            local->pattern_group = group;
            local->pattern_alternative = alternative;
        }
    }
    return true;
}

static MO_MUST_USE bool pattern(struct inference *p, const struct mo_syntax *node,
                                struct term *type, uint32_t group, uint32_t alternative,
                                struct mo_match_pattern **out);

static MO_MUST_USE bool pattern_node(struct inference *p, const struct mo_syntax *node,
                                     struct term *type, uint32_t group, uint32_t alternative,
                                     struct mo_match_pattern **out) {
    *out = NULL;
    if (node->kind == MO_S_GROUP)
        return pattern(p, node->children[0], type, group, alternative, out);
    if (node->kind == MO_S_UNDERSCORE || node->kind == MO_S_NAME)
        return pattern_bind(p, node, type, group, alternative);
    if (node->kind == MO_S_TUPLE) {
        struct term *tuple = make_type(p, (struct mo_source_type){.kind = MO_SOURCE_TUPLE,
                                                                  .name = "()",
                                                                  .module = UINT32_MAX,
                                                                  .count = node->count});
        if (tuple == NULL)
            return false;
        for (uint32_t i = 0; i < node->count; i++) {
            tuple->arguments[i] = fresh(p, 0);
            if (tuple->arguments[i] == NULL)
                return false;
        }
        if (!unify(p, type, tuple, 0))
            return false;
        *out = match_pattern(p, node, type, 5, 1, node->count, "tuple");
        if (*out == NULL)
            return false;
        for (uint32_t i = 0; i < node->count; i++)
            if (!pattern(p, node->children[i], tuple->arguments[i], group, alternative,
                         &(*out)->items[i]))
                return false;
        return record(p, node, (struct value){.type = type});
    }
    if (node->kind == MO_S_LIST) {
        struct term *list = native(p, "List", 1);
        if (list == NULL)
            return false;
        list->arguments[0] = fresh(p, 0);
        if (list->arguments[0] == NULL || !unify(p, type, list, 0))
            return false;
        struct mo_match_pattern **parts =
            allocate(p, node->count, sizeof(*parts), _Alignof(struct mo_match_pattern *));
        if (parts == NULL)
            return false;
        uint32_t length = node->count;
        bool rest = length != 0 && node->children[length - 1]->kind == MO_S_REST;
        if (rest)
            length--;
        for (uint32_t i = 0; i < length; i++)
            if (!pattern(p, node->children[i], list->arguments[0], group, alternative, &parts[i]))
                return false;
        struct mo_match_pattern *tail = NULL;
        if (rest) {
            if (!pattern(p, node->children[length]->children[0], type, group, alternative, &tail))
                return false;
        } else {
            tail = match_pattern(p, node, type, 3, 2, 0, "[]");
            if (tail == NULL)
                return false;
        }
        /* Flat list prefixes do not consume C stack. */
        for (uint32_t i = length; i != 0; i--) {
            struct mo_match_pattern *cons = match_pattern(p, node, type, 4, 2, 2, "list cell");
            if (cons == NULL)
                return false;
            cons->items[0] = parts[i - 1];
            cons->items[1] = tail;
            tail = cons;
        }
        *out = tail;
        return record(p, node, (struct value){.type = type});
    }
    bool qualified_call =
        node->kind == MO_S_POSTFIX && node->count == 3 && node->children[0]->kind == MO_S_CAPNAME &&
        node->children[1]->kind == MO_S_QUALIFIED && node->children[2]->kind == MO_S_CALL;
    if (node->kind == MO_S_CAPNAME || node->kind == MO_S_QUALIFIED || node->kind == MO_S_CALL ||
        qualified_call) {
        const struct mo_syntax *name = node->kind == MO_S_CALL ? node->children[0] : node;
        const struct mo_syntax *args = node->kind == MO_S_CALL ? node->children[1] : NULL;
        const struct mo_syntax *prefix = NULL;
        if (qualified_call) {
            prefix = node->children[0];
            name = node->children[1]->children[0];
            args = node->children[2]->children[0];
        }
        const struct mo_builtin_case *builtin = mo_builtin_case_by_name(name);
        if (prefix != NULL)
            builtin = NULL;
        if (builtin != NULL) {
            struct term *nominal = builtin_type(p, builtin, type);
            if (nominal == NULL)
                return false;
            bool unnamed = builtin->count != 0 && builtin->fields[0] == NULL;
            if ((unnamed &&
                 (args == NULL || args->count != 1 || args->children[0]->kind == MO_S_NAMED_ARG)) ||
                (!unnamed && builtin->count != 0 && args == NULL))
                return problem(p, MO_CODE_PATTERN_FIELDS, node->span,
                               "the pattern doesn't match the case's fields",
                               "A case with named fields is matched by naming them; Some(p) "
                               "matches one unnamed value (reference 7.3).",
                               NULL);
            *out = match_pattern(p, node, type, builtin->key, builtin->cases, builtin->count,
                                 builtin->name);
            if (*out == NULL)
                return false;
            bool seen[2] = {false, false};
            for (uint32_t i = 0; args != NULL && i < args->count; i++) {
                const struct mo_syntax *arg = args->children[i];
                uint32_t index = builtin_field(builtin, arg);
                if (index == UINT32_MAX || index >= builtin->count || seen[index])
                    return problem(p, MO_CODE_PATTERN_FIELDS, arg->span,
                                   "a matched field is absent or repeated",
                                   "A case is matched with its named fields; fields not named "
                                   "are ignored (reference 7.3).",
                                   NULL);
                seen[index] = true;
                struct term *field =
                    unnamed ? nominal->arguments[builtin->parameter] : native(p, "Text", 0);
                if (field == NULL || !pattern(p, unnamed ? arg : arg->children[1], field, group,
                                              alternative, &(*out)->items[index]))
                    return false;
            }
            return record(p, node, (struct value){.type = type, .builtin_case = builtin->name});
        }
        uint32_t count = 0;
        const struct mo_symbol *symbol = definition(p, name, prefix, &count);
        if (p->failed)
            return false;
        const struct mo_data_signature *data = symbol == NULL ? NULL : data_type(p, symbol);
        if (data == NULL && prefix == NULL && name->kind == MO_S_CAPNAME &&
            compare_text(name, "Entry") == 0)
            return unfinished(p);
        if (data == NULL)
            return problem(p, MO_CODE_PATTERN_CONSTRUCTOR, name->span,
                           "a pattern doesn't name a record or case",
                           "A pattern names a case or record, a literal, a tuple or a list "
                           "(reference 7.3).",
                           NULL);
        if (!visible_data(p, data->type->declaration))
            return false;
        struct term *parameters[26] = {NULL};
        struct term *nominal = annotation(p, data->type, parameters, 0);
        if (nominal == NULL || !unify(p, type, nominal, 0))
            return false;
        if ((data->unnamed &&
             (args == NULL || args->count != 1 || args->children[0]->kind == MO_S_NAMED_ARG)) ||
            (!data->unnamed && args == NULL && data->count != 0))
            return problem(p, MO_CODE_PATTERN_FIELDS, node->span,
                           "the pattern doesn't match the case's fields",
                           "A case with named fields is matched by naming them; Circle(p) is "
                           "only for one unnamed value (reference 7.3).",
                           symbol->declaration->children[0]);
        const struct mo_symbol *owner = data->type->declaration;
        *out = match_pattern(p, node, type, (uintptr_t)symbol,
                             symbol->owner == UINT32_MAX ? 1u : owner->case_count, data->count,
                             symbol->name);
        if (*out == NULL)
            return false;
        (*out)->choice = symbol->owner == UINT32_MAX ? NULL : owner;
        (*out)->data = data;
        bool *seen = allocate(p, data->count, sizeof(*seen), _Alignof(bool));
        if (seen == NULL)
            return false;
        memset(seen, 0, data->count * sizeof(*seen));
        for (uint32_t i = 0; args != NULL && i < args->count; i++) {
            const struct mo_syntax *arg = args->children[i];
            const struct mo_field_signature *field = data->unnamed ? &data->fields[0]
                                                     : arg->kind == MO_S_NAMED_ARG
                                                         ? find_field(data, arg->children[0])
                                                         : NULL;
            if (!data->unnamed && data->count == 1 && args->count == 1 &&
                arg->kind != MO_S_NAMED_ARG) {
                const struct mo_syntax *field_name = data->fields[0].name;
                size_t length = field_name->span.end - field_name->span.start;
                char *replacement = allocate(p, length + 3, 1, 1);
                if (replacement == NULL)
                    return false;
                memcpy(replacement, field_name->text, length);
                memcpy(replacement + length, ": ", 3);
                bool reported = problem(p, MO_CODE_PATTERN_FIELDS, arg->span,
                                        "a matched field hasn't been named",
                                        "A case with named fields is matched by naming them "
                                        "(reference 7.3).",
                                        field_name);
                (void)reported;
                if (p->error.code == MO_CODE_PATTERN_FIELDS &&
                    p->error.primary.span.start == arg->span.start) {
                    p->edit = (struct mo_edit){{arg->span.file, arg->span.start, arg->span.start},
                                               replacement};
                    p->fix = (struct mo_fix){"name the matched field", &p->edit, 1};
                    p->error.fixes = &p->fix;
                    p->error.fix_count = 1;
                }
                return false;
            }
            if (field == NULL || seen[field->index])
                return problem(p, MO_CODE_PATTERN_FIELDS, arg->span,
                               "a matched field is absent or repeated",
                               "A record or case is matched with its named fields; fields "
                               "not named are ignored (reference 7.3).",
                               symbol->declaration->children[0]);
            seen[field->index] = true;
            struct term *field_type = annotation(p, field->type, parameters, 0);
            if (field_type == NULL ||
                !pattern(p, data->unnamed ? arg : arg->children[1], field_type, group, alternative,
                         &(*out)->items[field->index]))
                return false;
            p->last->output.field = field;
        }
        return record(p, node, (struct value){.type = type, .symbol = symbol});
    }
    bool literal =
        node->kind == MO_S_INT || node->kind == MO_S_DECIMAL || node->kind == MO_S_TEXT ||
        node->kind == MO_S_CHAR || node->kind == MO_S_TRUE || node->kind == MO_S_FALSE ||
        (node->kind == MO_S_NEGATE &&
         (node->children[0]->kind == MO_S_INT || node->children[0]->kind == MO_S_DECIMAL));
    if (!literal)
        return problem(p, MO_CODE_PATTERN_CONSTRUCTOR, node->span,
                       "a pattern doesn't name a record or case",
                       "A pattern names a case or record, a literal, a tuple or a list "
                       "(reference 7.3).",
                       NULL);
    struct value literal_value = infer(p, node, type);
    if (literal_value.type == NULL)
        return false;
    bool boolean = node->kind == MO_S_TRUE || node->kind == MO_S_FALSE;
    *out =
        match_pattern(p, node, type, node->kind == MO_S_TRUE ? 1u : 2u, boolean ? 2u : UINT32_MAX,
                      0, boolean ? (node->kind == MO_S_TRUE ? "true" : "false") : "literal");
    return *out != NULL;
}

static MO_MUST_USE bool pattern(struct inference *p, const struct mo_syntax *node,
                                struct term *type, uint32_t group, uint32_t alternative,
                                struct mo_match_pattern **out) {
    if (!work(p))
        return false;
    MO_ASSERT(p->pattern_depth < MO_LIMIT_NESTING_DEPTH);
    p->span = node->span;
    p->pattern_depth++;
    bool ok = pattern_node(p, node, type, group, alternative, out);
    p->pattern_depth--;
    return ok;
}

static MO_MUST_USE struct value condition(struct inference *p, const struct mo_syntax *node,
                                          bool keep) {
    bool previous = p->keep_patterns;
    p->keep_patterns = keep;
    struct term *boolean = native(p, "Bool", 0);
    struct value v =
        boolean == NULL ? (struct value){0} : infer_as(p, node, boolean, MO_CODE_CONDITION_TYPE);
    p->keep_patterns = previous;
    return v;
}

static MO_MUST_USE struct value match(struct inference *p, const struct mo_syntax *node,
                                      struct term *expected) {
    struct value input = infer(p, node->children[0], NULL);
    struct term *result = expected == NULL ? fresh(p, 0) : expected;
    if (input.type == NULL || result == NULL)
        return (struct value){0};
    const struct mo_syntax *arms = node->children[1];
    uint32_t count = 0;
    for (uint32_t i = 0; i < arms->count; i++)
        count += arms->children[i]->children[0]->count;
    struct match_check *check = allocate(p, 1, sizeof(*check), _Alignof(struct match_check));
    struct mo_match_pattern **patterns =
        allocate(p, count, sizeof(*patterns), _Alignof(struct mo_match_pattern *));
    if (p->failed)
        return (struct value){0};
    struct mo_span header = node->span;
    header.end = node->children[0]->span.end;
    *check = (struct match_check){.patterns = patterns,
                                  .span = header,
                                  .end = node->span.end,
                                  .scrutinee = node->children[0],
                                  .type = input.type,
                                  .next = p->matches};
    p->matches = check;
    bool never = true;
    for (uint32_t i = 0; i < arms->count; i++) {
        struct local *mark = p->locals;
        const struct mo_syntax *arm = arms->children[i], *alternatives = arm->children[0];
        uint32_t group = ++p->pattern_group;
        for (uint32_t j = 0; j < alternatives->count; j++) {
            struct mo_match_pattern *normalized = NULL;
            if (!pattern(p, alternatives->children[j], input.type, group, j + 1, &normalized))
                return (struct value){0};
            if (normalized == NULL && check->catch_all == NULL)
                check->catch_all = alternatives->children[j];
            if (arm->children[1] == NULL)
                check->patterns[check->count++] = normalized;
            for (struct local *local = p->locals; local != mark; local = local->previous) {
                if (local->pattern_alternative != j + 1) {
                    bool reported =
                        problem(p, MO_CODE_PATTERN_ALTERNATIVES, alternatives->children[j]->span,
                                "the alternatives don't bind the same names",
                                "Arms with the same body share a line; their names must be "
                                "visible throughout that body (reference 7.2).",
                                local->name);
                    (void)reported;
                    return (struct value){0};
                }
            }
        }
        if (arm->children[1] != NULL && condition(p, arm->children[1], true).type == NULL)
            return (struct value){0};
        struct value body = infer(p, arm->children[2], result);
        if (body.type == NULL || !leave(p, mark))
            return (struct value){0};
        never = never && body.never;
    }
    return (struct value){.type = result, .never = never};
}

static MO_MUST_USE struct value conditional(struct inference *p, const struct mo_syntax *node,
                                            struct term *expected) {
    const struct mo_syntax *parts = node->children[2];
    bool has_else = parts->count != 0 && parts->children[parts->count - 1]->kind == MO_S_ELSE;
    struct term *result =
        has_else ? (expected == NULL ? fresh(p, 0) : expected) : native(p, "Unit", 0);
    struct term *condition = native(p, "Bool", 0);
    if (result == NULL || condition == NULL)
        return (struct value){0};
    struct local *mark = p->locals;
    bool previous = p->keep_patterns;
    p->keep_patterns = true;
    struct value test = infer_as(p, node->children[0], condition, MO_CODE_CONDITION_TYPE);
    p->keep_patterns = previous;
    if (test.type == NULL)
        return (struct value){0};
    struct value branch = has_else ? infer_as(p, node->children[1], result, p->node_expectation)
                                   : discard(p, node->children[1]);
    if (branch.type == NULL || !leave(p, mark))
        return (struct value){0};
    bool never = has_else && branch.never;
    for (uint32_t i = 0; i < parts->count; i++) {
        const struct mo_syntax *part = parts->children[i];
        if (part->kind == MO_S_IF_PART) {
            p->keep_patterns = true;
            test = infer_as(p, part->children[0], condition, MO_CODE_CONDITION_TYPE);
            p->keep_patterns = previous;
            if (test.type == NULL)
                return (struct value){0};
        }
        const struct mo_syntax *body = part->children[part->kind == MO_S_ELSE ? 0 : 1];
        branch = has_else ? infer_as(p, body, result, p->node_expectation) : discard(p, body);
        if (branch.type == NULL || !leave(p, mark))
            return (struct value){0};
        never = never && branch.never;
    }
    if (never)
        result = expected == NULL ? native(p, "Unit", 0) : expected;
    return (struct value){.type = result, .never = never};
}

static MO_MUST_USE bool binder(struct inference *p, const struct mo_syntax *node,
                               struct term *type) {
    if (node->kind == MO_S_BINDER)
        return binder(p, node->children[0], type);
    if (node->kind != MO_S_TUPLE)
        return bind(p, node, type, false);
    struct term *tuple = make_type(
        p, (struct mo_source_type){
               .kind = MO_SOURCE_TUPLE, .name = "()", .module = UINT32_MAX, .count = node->count});
    if (tuple == NULL)
        return false;
    for (uint32_t i = 0; i < node->count; i++) {
        tuple->arguments[i] = fresh(p, 0);
        if (tuple->arguments[i] == NULL)
            return false;
    }
    if (!unify(p, tuple, type, 0))
        return false;
    for (uint32_t i = 0; i < node->count; i++)
        if (!binder(p, node->children[i], tuple->arguments[i]))
            return false;
    return true;
}

static MO_MUST_USE struct value position(struct inference *p, struct value v,
                                         const struct mo_syntax *index) {
    if (v.type == NULL)
        return v;
    struct term *t = root(v.type);
    if (t->variable) {
        bool reported = unfinished(p);
        (void)reported;
        return (struct value){0};
    }
    uint32_t at = 0;
    bool fits = true;
    for (uint32_t i = 0; i < index->span.end - index->span.start; i++) {
        uint8_t c = index->text[i];
        if (c == '_')
            continue;
        uint32_t digit = (uint32_t)(c - '0');
        if (digit > 9 || at > (UINT32_MAX - digit) / 10) {
            fits = false;
            break;
        }
        at = at * 10 + digit;
    }
    if (!fits || t->shape.kind != MO_SOURCE_TUPLE || at >= t->shape.count) {
        bool reported =
            problem(p, MO_CODE_TUPLE_POSITION, index->span, "the tuple has no such part",
                    "Tuple parts are read with .0, .1 and so on (reference 3.6).", NULL);
        (void)reported;
        return (struct value){0};
    }
    return (struct value){.type = argument(p, t, at), .never = v.never};
}

static MO_MUST_USE struct value infer_node(struct inference *p, const struct mo_syntax *node,
                                           struct term *expected) {
    switch (node->kind) {
    case MO_S_SEQUENCE:
        return sequence(p, node, expected);
    case MO_S_EXPR_STMT:
    case MO_S_GROUP:
        return infer_as(p, node->children[0], expected, p->node_expectation);
    case MO_S_NAME:
    case MO_S_CAPNAME:
    case MO_S_QUALIFIED:
        return identifier(p, node, NULL);
    case MO_S_FIELD_ACCESS:
        if (node->children[0]->kind == MO_S_CAPNAME)
            return identifier(p, node->children[1], node->children[0]);
        return field_value(p, infer(p, node->children[0], NULL), node->children[1]);
    case MO_S_INT:
        return (struct value){.type = fresh(p, INT | BIG | BYTE | DECIMAL | FLOAT)};
    case MO_S_DECIMAL:
        return (struct value){.type = fresh(p, DECIMAL | FLOAT)};
    case MO_S_CHAR:
        return (struct value){.type = native(p, "Char", 0)};
    case MO_S_TEXT:
        return (struct value){.type = native(p, "Text", 0)};
    case MO_S_TRUE:
    case MO_S_FALSE:
        return (struct value){.type = native(p, "Bool", 0)};
    case MO_S_BIND:
    case MO_S_VAR:
    case MO_S_ADD_ASSIGN:
    case MO_S_SUB_ASSIGN: {
        const struct mo_syntax *name = node->children[0];
        if (name->kind == MO_S_FIELD_ACCESS || name->kind == MO_S_POSTFIX) {
            const struct mo_syntax *base = name;
            bool fields = true;
            while (base->kind == MO_S_FIELD_ACCESS || base->kind == MO_S_POSTFIX) {
                if (base->kind == MO_S_POSTFIX)
                    for (uint32_t i = 1; i < base->count; i++)
                        fields = fields && base->children[i]->kind == MO_S_FIELD_ACCESS;
                base = base->children[0];
            }
            struct name_slot *entry = base->kind == MO_S_NAME ? slot(p, base, false) : NULL;
            struct local *variable = entry == NULL ? NULL : entry->active;
            if (!fields || variable == NULL || !variable->variable ||
                variable->function != p->lambda) {
                bool reported = problem(p, MO_CODE_NOT_VARIABLE, name->span,
                                        "a field is assigned without an assignable var",
                                        "A var or its fields can be assigned within the function "
                                        "that declares it (reference 6.2).",
                                        variable == NULL ? NULL : variable->name);
                (void)reported;
                return (struct value){0};
            }
            struct value target = infer(p, name, NULL);
            if (target.type == NULL)
                return (struct value){0};
            p->last->output.binding = variable->name;
            struct value rhs =
                node->kind == MO_S_BIND
                    ? infer(p, node->children[1], target.type)
                    : binary(p, node->kind == MO_S_ADD_ASSIGN ? MO_T_PLUS : MO_T_MINUS, target,
                             node->children[1], target.type);
            if (rhs.type == NULL)
                return (struct value){0};
            return (struct value){.type = native(p, "Unit", 0),
                                  .binding = variable->name,
                                  .field = target.field,
                                  .never = rhs.never};
        }
        if (name->kind != MO_S_NAME && name->kind != MO_S_TUPLE && name->kind != MO_S_UNDERSCORE) {
            bool reported = unfinished(p);
            (void)reported;
            return (struct value){0};
        }
        if (p->checked && name->kind == MO_S_TUPLE) {
            bool reported = unfinished(p);
            (void)reported;
            return (struct value){0};
        }
        struct name_slot *s = name->kind == MO_S_NAME ? slot(p, name, false) : NULL;
        struct local *l = s == NULL ? NULL : s->active;
        bool assignment = l != NULL && l->variable && node->kind != MO_S_VAR;
        if (l != NULL && !assignment) {
            bool reported =
                problem(p, MO_CODE_BIND_REPEATED, name->span, "a visible name is bound again",
                        "While a local name or parameter is visible it can't be "
                        "bound again (reference 6.1).",
                        l->name);
            (void)reported;
            return (struct value){0};
        }
        if ((node->kind == MO_S_ADD_ASSIGN || node->kind == MO_S_SUB_ASSIGN) && !assignment) {
            bool reported =
                problem(p, MO_CODE_NOT_VARIABLE, name->span, "a name that isn't var is assigned",
                        "+= and -= assign a var or its fields (reference 6.2).", NULL);
            (void)reported;
            return (struct value){0};
        }
        if (assignment && l->function != p->lambda) {
            bool reported = problem(p, MO_CODE_NOT_VARIABLE, name->span,
                                    "a var is assigned outside the function that declares it",
                                    "A var can be assigned again within the function that declares "
                                    "it (reference 6.2).",
                                    l->name);
            (void)reported;
            return (struct value){0};
        }
        if (assignment && !record(p, name, (struct value){.type = l->type, .binding = l->name}))
            return (struct value){0};
        struct value rhs;
        if (assignment && node->kind != MO_S_BIND) {
            l->used = true;
            rhs = binary(p, node->kind == MO_S_ADD_ASSIGN ? MO_T_PLUS : MO_T_MINUS,
                         (struct value){.type = l->type}, node->children[1], l->type);
        } else
            rhs = infer(p, node->children[1], assignment ? l->type : NULL);
        if (rhs.type == NULL)
            return (struct value){0};
        if (!assignment &&
            !(node->kind == MO_S_VAR ? bind(p, name, rhs.type, true) : binder(p, name, rhs.type)))
            return (struct value){0};
        struct value result = graph(p,
                                    (struct value){.type = native(p, "Unit", 0),
                                                   .binding = assignment ? l->name : NULL,
                                                   .bound = assignment ? l->binding : p->last,
                                                   .never = rhs.never},
                                    1);
        if (p->checked && result.type != NULL) {
            result.graph->operands[0] = rhs.note;
            result.graph->output.assignment = assignment;
        }
        return result;
    }
    case MO_S_BINARY:
    case MO_S_RANGE: {
        bool logical =
            node->token == MO_T_AND || node->token == MO_T_OR || node->token == MO_T_IMPLIES;
        struct local *mark = p->locals;
        struct value left = logical ? condition(p, node->children[0], node->token != MO_T_OR)
                                    : infer(p, node->children[0], NULL);
        struct value result = binary(p, node->token, left, node->children[1], expected);
        if (result.type == NULL || (logical && node->token != MO_T_AND && !leave(p, mark)))
            return (struct value){0};
        return result;
    }
    case MO_S_BINARY_CHAIN: {
        enum mo_syntax_token op = node->children[1]->token;
        bool logical = op == MO_T_AND || op == MO_T_OR;
        struct value result = logical ? condition(p, node->children[0], op == MO_T_AND)
                                      : infer(p, node->children[0], NULL);
        for (uint32_t i = 1; i < node->count && result.type != NULL; i += 2) {
            result = binary(p, node->children[i]->token, result, node->children[i + 1], expected);
            if (p->checked && result.type != NULL && i + 2 < node->count) {
                if (!record(p, node->children[i], result))
                    return (struct value){0};
                result.note = p->last;
            }
        }
        return result;
    }
    case MO_S_CALL: {
        const struct mo_syntax *callee = node->children[0];
        if (p->checked && (callee->kind == MO_S_CAPNAME || callee->kind == MO_S_QUALIFIED)) {
            bool reported = unfinished(p);
            (void)reported;
            return (struct value){0};
        }
        const struct mo_builtin_case *builtin = mo_builtin_case_by_name(callee);
        if (builtin != NULL)
            return builtin_construct(p, builtin, node->children[1], expected);
        if (callee->kind == MO_S_CAPNAME || callee->kind == MO_S_QUALIFIED) {
            uint32_t count = 0;
            const struct mo_symbol *symbol = definition(p, callee, NULL, &count);
            if (p->failed)
                return (struct value){0};
            const struct mo_data_signature *data = symbol == NULL ? NULL : data_type(p, symbol);
            if (data != NULL)
                return construct(p, data, node->children[1], expected);
        }
        return call(p, infer(p, node->children[0], NULL), node->children[1]);
    }
    case MO_S_POSITION:
        return position(p, infer(p, node->children[0], NULL), node->children[1]);
    case MO_S_POSTFIX: {
        if (p->checked)
            return console_print(p, node);
        bool qualified = node->children[0]->kind == MO_S_CAPNAME &&
                         (node->children[1]->kind == MO_S_FIELD_ACCESS ||
                          node->children[1]->kind == MO_S_QUALIFIED);
        uint32_t start = qualified ? 2u : 1u;
        const struct mo_syntax *name =
            qualified ? node->children[1]->children[0] : node->children[0];
        const struct mo_data_signature *data = NULL;
        const struct mo_builtin_case *builtin =
            !qualified && start < node->count && node->children[start]->kind == MO_S_CALL
                ? mo_builtin_case_by_name(name)
                : NULL;
        if ((name->kind == MO_S_CAPNAME || qualified) && start < node->count &&
            node->children[start]->kind == MO_S_CALL) {
            uint32_t count = 0;
            const struct mo_symbol *symbol =
                definition(p, name, qualified ? node->children[0] : NULL, &count);
            if (p->failed)
                return (struct value){0};
            data = symbol == NULL ? NULL : data_type(p, symbol);
        }
        struct value v =
            builtin != NULL ? builtin_construct(p, builtin, node->children[start]->children[0],
                                                start + 1 == node->count ? expected : NULL)
            : data != NULL  ? construct(p, data, node->children[start]->children[0],
                                       start + 1 == node->count ? expected : NULL)
            : qualified     ? identifier(p, node->children[1]->children[0], node->children[0])
                            : infer(p, node->children[0], NULL);
        if (data != NULL || builtin != NULL)
            start++;
        for (uint32_t i = start; i < node->count && v.type != NULL; i++) {
            const struct mo_syntax *step = node->children[i];
            if (step->kind == MO_S_POSITION)
                v = position(p, v, step->children[0]);
            else if (step->kind == MO_S_CALL)
                v = call(p, v, step->children[0]);
            else if (step->kind == MO_S_FIELD_ACCESS)
                v = field_value(p, v, step->children[0]);
            else {
                bool reported = unfinished(p);
                (void)reported;
                return (struct value){0};
            }
        }
        return v;
    }
    case MO_S_LAMBDA: {
        const struct mo_syntax *parameters = node->children[0];
        struct term *fn = make_type(p, (struct mo_source_type){.kind = MO_SOURCE_FUNCTION,
                                                               .name = "fn",
                                                               .module = UINT32_MAX,
                                                               .count = parameters->count + 1});
        if (fn == NULL)
            return (struct value){0};
        for (uint32_t i = 0; i < fn->shape.count; i++) {
            fn->arguments[i] = fresh(p, 0);
            if (fn->arguments[i] == NULL)
                return (struct value){0};
        }
        if (expected != NULL && !unify_as(p, fn, expected, p->node_expectation))
            return (struct value){0};
        struct value emitted = graph(p, (struct value){.type = fn}, 1);
        if (emitted.type == NULL)
            return emitted;
        if (p->checked) {
            struct emitted_lambda *lambda =
                allocate(p, 1, sizeof(*lambda), _Alignof(struct emitted_lambda));
            struct note **notes =
                allocate(p, parameters->count, sizeof(*notes), _Alignof(struct note *));
            if (p->failed)
                return (struct value){0};
            *lambda = (struct emitted_lambda){.output = {.parameter_count = parameters->count},
                                              .parameters = notes};
            emitted.graph->lambda = lambda;
            p->lambda_count++;
        }
        struct local *mark = p->locals;
        struct term *old_result = p->result;
        uint32_t old_loops = p->loops;
        p->loops = 0;
        p->lambda++;
        p->result = fn->arguments[parameters->count];
        for (uint32_t i = 0; i < parameters->count; i++) {
            if (!bind(p, parameters->children[i], fn->arguments[i], false))
                return (struct value){0};
            if (p->checked)
                emitted.graph->lambda->parameters[i] = p->last;
        }
        struct value body = named(p->result, "Unit")
                                ? discard(p, node->children[1])
                                : infer_as(p, node->children[1], p->result, MO_CODE_RESULT_TYPE);
        if (body.type == NULL || !leave(p, mark))
            return (struct value){0};
        p->result = old_result;
        p->loops = old_loops;
        p->lambda--;
        if (p->checked)
            emitted.graph->operands[0] = body.note;
        return emitted;
    }
    case MO_S_IF:
    case MO_S_LINE_IF:
        return conditional(p, node, expected);
    case MO_S_CASE:
        return match(p, node, expected);
    case MO_S_IS: {
        struct value input = infer(p, node->children[0], NULL);
        struct mo_match_pattern *normalized = NULL;
        if (input.type == NULL || !pattern(p, node->children[1], input.type, 0, 1, &normalized))
            return (struct value){0};
        return (struct value){.type = native(p, "Bool", 0), .never = input.never};
    }
    case MO_S_RETURN: {
        struct local *mark = p->locals;
        if (node->children[1] != NULL && condition(p, node->children[1], true).type == NULL)
            return (struct value){0};
        struct value value = node->children[0] == NULL
                                 ? (struct value){.type = native(p, "Unit", 0)}
                                 : infer_as(p, node->children[0], p->result, MO_CODE_RESULT_TYPE);
        if (value.type == NULL || !unify_as(p, value.type, p->result, MO_CODE_RESULT_TYPE) ||
            !leave(p, mark))
            return (struct value){0};
        return (struct value){.type = native(p, "Unit", 0), .never = node->children[1] == NULL};
    }
    case MO_S_STOP: {
        struct term *text = native(p, "Text", 0);
        if (text == NULL || infer(p, node->children[0], text).type == NULL)
            return (struct value){0};
        return (struct value){.type = expected == NULL ? native(p, "Unit", 0) : expected,
                              .never = true};
    }
    case MO_S_ASSERT: {
        if (condition(p, node->children[0], true).type == NULL)
            return (struct value){0};
        return (struct value){.type = native(p, "Unit", 0)};
    }
    case MO_S_FOR:
    case MO_S_WHILE: {
        struct local *mark = p->locals;
        uint32_t body_index = node->kind == MO_S_FOR ? 2 : 1;
        struct value test = node->kind == MO_S_WHILE ? condition(p, node->children[0], true)
                                                     : infer(p, node->children[1], NULL);
        if (test.type == NULL)
            return (struct value){0};
        if (node->kind == MO_S_WHILE) {
            struct term *boolean = native(p, "Bool", 0);
            if (boolean == NULL || !unify_as(p, test.type, boolean, MO_CODE_CONDITION_TYPE))
                return (struct value){0};
        } else {
            struct term *element = NULL, *container = root(test.type);
            if (named(container, "Range"))
                element = native(p, "Int", 0);
            else if (named(container, "List") || named(container, "Set"))
                element = argument(p, container, 0);
            else if (named(container, "Map")) {
                element = make_type(p, (struct mo_source_type){.kind = MO_SOURCE_TUPLE,
                                                               .name = "()",
                                                               .module = UINT32_MAX,
                                                               .count = 2});
                if (element != NULL) {
                    element->arguments[0] = argument(p, container, 0);
                    element->arguments[1] = argument(p, container, 1);
                    if (p->failed)
                        return (struct value){0};
                }
            } else {
                bool reported =
                    container->variable && container->numeric == 0
                        ? unfinished(p)
                        : problem(p, MO_CODE_NOT_ITERABLE, node->children[body_index - 1]->span,
                                  "for is given a value that can't be iterated",
                                  "For walks a list, map, set or range (reference 7.5).", NULL);
                (void)reported;
            }
            if (element == NULL || !binder(p, node->children[0], element))
                return (struct value){0};
        }
        p->loops++;
        struct term *unit = native(p, "Unit", 0);
        struct value body =
            unit == NULL ? (struct value){0} : discard(p, node->children[body_index]);
        p->loops--;
        if (body.type == NULL || !leave(p, mark))
            return (struct value){0};
        return (struct value){.type = unit};
    }
    case MO_S_BREAK:
    case MO_S_CONTINUE: {
        if (p->loops == 0) {
            bool reported = problem(p, MO_CODE_EXIT_OUTSIDE_LOOP, node->span,
                                    "break or continue is outside a loop",
                                    "Break leaves the loop and continue starts its next round "
                                    "(reference 7.5).",
                                    NULL);
            (void)reported;
            return (struct value){0};
        }
        if (node->children[0] != NULL) {
            struct term *condition = native(p, "Bool", 0);
            if (condition == NULL ||
                infer_as(p, node->children[0], condition, MO_CODE_CONDITION_TYPE).type == NULL)
                return (struct value){0};
        }
        return (struct value){.type = native(p, "Unit", 0), .never = node->children[0] == NULL};
    }
    case MO_S_LIST:
    case MO_S_TUPLE:
    case MO_S_MAP: {
        uint32_t arity = node->kind == MO_S_TUPLE ? node->count : node->kind == MO_S_MAP ? 2u : 1u;
        struct term *t =
            make_type(p, (struct mo_source_type){.kind = node->kind == MO_S_TUPLE ? MO_SOURCE_TUPLE
                                                                                  : MO_SOURCE_NAMED,
                                                 .name = node->kind == MO_S_TUPLE ? "()"
                                                         : node->kind == MO_S_MAP ? "Map"
                                                                                  : "List",
                                                 .module = UINT32_MAX,
                                                 .count = arity});
        if (t == NULL)
            return (struct value){0};
        for (uint32_t i = 0; i < arity; i++) {
            t->arguments[i] = fresh(p, 0);
            if (t->arguments[i] == NULL)
                return (struct value){0};
        }
        if (expected != NULL && !unify_as(p, t, expected, p->node_expectation))
            return (struct value){0};
        bool never = false;
        for (uint32_t i = 0; i < node->count; i++) {
            const struct mo_syntax *part = node->children[i];
            if (node->kind == MO_S_MAP) {
                for (uint32_t j = 0; j < 2; j++) {
                    struct value v = infer(p, part->children[j], t->arguments[j]);
                    if (v.type == NULL)
                        return (struct value){0};
                    never = never || v.never;
                }
            } else {
                struct value v = infer(p, part, t->arguments[node->kind == MO_S_TUPLE ? i : 0]);
                if (v.type == NULL)
                    return (struct value){0};
                never = never || v.never;
            }
        }
        return (struct value){.type = t, .never = never};
    }
    case MO_S_NEGATE:
    case MO_S_NOT:
    case MO_S_PREFIX: {
        const struct mo_syntax *operand = node->children[node->count - 1];
        struct value v = infer(p, operand, NULL);
        if (v.type == NULL)
            return v;
        bool negative =
            node->kind == MO_S_NEGATE ||
            (node->kind == MO_S_PREFIX && node->children[node->count - 2]->token == MO_T_MINUS);
        if (negative && (operand->kind == MO_S_INT || operand->kind == MO_S_DECIMAL)) {
            p->last->negative = true;
        }
        uint32_t count = node->kind == MO_S_PREFIX ? node->count - 1 : 1;
        for (uint32_t i = count; i != 0; i--) {
            enum mo_syntax_token op = node->kind == MO_S_PREFIX ? node->children[i - 1]->token
                                      : node->kind == MO_S_NOT  ? MO_T_NOT
                                                                : MO_T_MINUS;
            struct term *wanted = op == MO_T_NOT     ? native(p, "Bool", 0)
                                  : op == MO_T_MINUS ? fresh(p, INT | BIG | DECIMAL | FLOAT)
                                                     : NULL;
            if (wanted == NULL) {
                bool reported = unfinished(p);
                (void)reported;
                return (struct value){0};
            }
            uint16_t code = op == MO_T_NOT          ? MO_CODE_CONDITION_TYPE
                            : named(v.type, "Byte") ? MO_CODE_BYTE_ARITHMETIC
                                                    : MO_CODE_NUMBER_ABILITY;
            if (!unify_as(p, v.type, wanted, code))
                return (struct value){0};
            if (p->checked) {
                struct note *input = v.note;
                v = graph(p, (struct value){.type = v.type, .never = v.never}, 1);
                if (v.type == NULL)
                    return v;
                v.graph->operands[0] = input;
                v.graph->output.operation = op;
                if (i != 1) {
                    if (!record(p, node->children[i - 1], v))
                        return (struct value){0};
                    v.note = p->last;
                }
            }
        }
        return v;
    }
    case MO_S_FILE:
    case MO_S_MODULE:
    case MO_S_EXPOSE:
    case MO_S_EXPOSE_ALL:
    case MO_S_USE:
    case MO_S_FN:
    case MO_S_PARAM:
    case MO_S_WHERE:
    case MO_S_STRUCT:
    case MO_S_FIELD:
    case MO_S_ENUM:
    case MO_S_CASE_DECL:
    case MO_S_TEST:
    case MO_S_CONST:
    case MO_S_PROMISE:
    case MO_S_ANCHOR:
    case MO_S_EXAMPLES:
    case MO_S_TYPE:
    case MO_S_BINDER:
    case MO_S_ELSE:
    case MO_S_ARM:
    case MO_S_TRY:
    case MO_S_NAMED_ARG:
    case MO_S_TEXT_INTERPOLATED:
    case MO_S_UNDERSCORE:
    case MO_S_REST:
    case MO_S_MAP_ENTRY:
    case MO_S_TOKEN:
    case MO_S_IF_PART:
        break;
    }
    bool reported = unfinished(p);
    (void)reported;
    return (struct value){0};
}

static MO_MUST_USE struct value infer(struct inference *p, const struct mo_syntax *node,
                                      struct term *expected) {
    if (!work(p))
        return (struct value){0};
    MO_ASSERT(node != NULL && !node->incomplete);
    struct local *mark = p->locals;
    bool keep = p->keep_patterns;
    bool propagates = node->kind == MO_S_IS || node->kind == MO_S_GROUP ||
                      (node->kind == MO_S_BINARY && node->token == MO_T_AND) ||
                      (node->kind == MO_S_BINARY_CHAIN && node->children[1]->token == MO_T_AND);
    p->keep_patterns = node->kind == MO_S_GROUP && keep;
    p->span = node->span;
    if (p->depth == MO_LIMIT_SOURCE_INFERENCE_DEPTH) {
        bool reported = problem(p, MO_CODE_CHECK_SYNTAX_DEPTH, node->span,
                                "the body needs too many inference frames", NULL, NULL);
        (void)reported;
        return (struct value){0};
    }
    uint16_t code = p->expectation, saved = p->node_expectation;
    p->node_expectation = code;
    p->expectation = 0;
    p->depth++;
    if (p->checked && node->kind != MO_S_SEQUENCE && node->kind != MO_S_EXPR_STMT &&
        node->kind != MO_S_GROUP && node->kind != MO_S_INT && node->kind != MO_S_DECIMAL &&
        node->kind != MO_S_CHAR && node->kind != MO_S_TEXT && node->kind != MO_S_TRUE &&
        node->kind != MO_S_FALSE && node->kind != MO_S_NAME && node->kind != MO_S_VAR &&
        node->kind != MO_S_BIND && node->kind != MO_S_BINARY && node->kind != MO_S_BINARY_CHAIN &&
        node->kind != MO_S_RANGE && node->kind != MO_S_NEGATE && node->kind != MO_S_NOT &&
        node->kind != MO_S_PREFIX && node->kind != MO_S_CALL && node->kind != MO_S_POSTFIX &&
        node->kind != MO_S_LAMBDA) {
        bool reported = unfinished(p);
        (void)reported;
        p->depth--;
        return (struct value){0};
    }
    struct value v = infer_node(p, node, expected);
    p->depth--;
    p->expectation = code;
    p->node_expectation = saved;
    p->keep_patterns = keep;
    p->span = node->span;
    if (v.type == NULL || (propagates && !keep && !leave(p, mark)) ||
        (expected != NULL && !v.never && !unify(p, v.type, expected, 0)))
        return (struct value){0};
    if (p->checked && (node->kind == MO_S_EXPR_STMT || node->kind == MO_S_GROUP)) {
        struct note *operand = v.note;
        v = graph(p, v, 1);
        if (v.type == NULL)
            return v;
        v.graph->operands[0] = operand;
    } else if (p->checked && v.graph == NULL)
        v = graph(p, v, 0);
    if (v.type == NULL)
        return v;
    if (!record(p, node, v))
        return (struct value){0};
    v.note = p->last;
    return v;
}

static MO_MUST_USE const struct mo_source_type *finish_type(struct inference *p, struct term *t,
                                                            uint32_t depth) {
    if (!work(p) || !type_depth(p, depth))
        return NULL;
    t = root(t);
    if (t->resolved != NULL) {
        if (!type_depth(p, depth + t->height))
            return NULL;
        return t->resolved;
    }
    if (t->variable) {
        if (t->numeric == 0) {
            bool reported =
                problem(p, MO_CODE_CANNOT_INFER, p->span, "no use decides this expression's type",
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
        t->shape.count == 0
            ? NULL
            : allocate(p, t->shape.count, sizeof(*args), _Alignof(const struct mo_source_type *));
    if (p->failed)
        return NULL;
    for (uint32_t i = 0; i < t->shape.count; i++) {
        args[i] = finish_type(p, t->arguments[i], depth + 1);
        if (args[i] == NULL)
            return NULL;
        t->partial_default = t->partial_default || root(t->arguments[i])->partial_default;
        uint32_t height = root(t->arguments[i])->height + 1;
        if (height > t->height)
            t->height = height;
    }
    struct mo_source_type *out = allocate(p, 1, sizeof(*out), _Alignof(struct mo_source_type));
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
    if (strcmp(name, "Int") == 0)
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
    return fits ||
           problem(p, MO_CODE_LITERAL_RANGE, node->span, "a number literal doesn't fit its type",
                   "A number literal takes its context's numeric type and must fit "
                   "that type's range (reference 3.3).",
                   NULL);
}

static MO_MUST_USE bool finish_notes(struct inference *p, struct mo_inferred_expression *items) {
    uint32_t i = 0;
    for (struct note *n = p->notes; n != NULL; n = n->next) {
        p->span = n->output.syntax->span;
        if (p->partial && p->span.start >= p->error.primary.span.start)
            continue;
        n->output.type = finish_type(p, n->type, 0);
        if (n->output.type != NULL) {
            bool valid = literal(p, n);
            (void)valid;
            if (!p->fatal && n->discarded && !named(n->type, "Unit"))
                defer(p, named(n->type, "Result") ? MO_CODE_FAILURE_IGNORED : MO_CODE_UNUSED_VALUE,
                      p->span,
                      named(n->type, "Result") ? "a failure is ignored" : "a result isn't used",
                      named(n->type, "Result")
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
        const struct mo_source_type *type = finish_type(p, l->type, 0);
        if (p->fatal)
            return false;
        p->failed = false;
        if (type == NULL)
            continue;
        if (type->kind == MO_SOURCE_FUNCTION)
            type = type->arguments[type->count - 1];
        if (type->kind != MO_SOURCE_NAMED || type->module != UINT32_MAX ||
            strcmp(type->name, "Bool") != 0)
            defer(p, MO_CODE_BOOL_NAME, p->span, "a name ending in ? isn't a yes-or-no answer",
                  "A name ending in ? is a Bool, or a function that returns one (reference 2.3).");
    }
    MO_ASSERT(p->partial || i == p->count);
    return p->error.code == 0;
}

/* Check only complete, independently typed matches before any retained error.
 * Later failures inside an arm must not invent a coverage error for that match.
 * False means coverage already emitted a resource diagnostic. */
static MO_MUST_USE bool finish_patterns(struct inference *p) {
    /* Byte is a small finite literal domain. Normalize spelling only after all
     * uses and literal range checks have fixed the contextual numeric type. */
    for (struct typed_pattern *at = p->patterns; at != NULL; at = at->next) {
        const struct mo_syntax *node = at->syntax;
        if (p->error.code != 0 && node->span.end > p->error.primary.span.start)
            continue;
        if (node->kind != MO_S_INT || !named(at->type, "Byte"))
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
        char *name = allocate(p, 16, 1, 1);
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
        struct term *type = root(check->type);
        /* Defaulting during a failed walk cannot certify a literal domain:
         * the later use deciding Byte versus Int may never have been reached. */
        if (type->variable || type->resolved == NULL || type->partial_default)
            continue;
        bool choice = type->shape.declaration != NULL &&
                      type->shape.declaration->declaration->kind == MO_S_ENUM;
        choice = choice || named(type, "Option") || named(type, "Result") ||
                 named(type, "Ordering") || named(type, "EntryKind") || named(type, "FileError");
        if (check->catch_all != NULL && choice) {
            defer(p, MO_CODE_CASE_CATCH_ALL, check->catch_all->span, "a choice arm is a catch-all",
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
            char *message = allocate(p, 8192, 1, 1);
            if (message == NULL)
                return true;
            int length =
                snprintf(message, 8192, "case doesn't cover every possible value; %smissing: %s",
                         check->count == 0 ? "guards don't count toward coverage; " : "", missing);
            MO_ASSERT(length > 0 && length < 8192);
            defer(p, MO_CODE_CASE_INCOMPLETE, check->span, message,
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

static MO_MUST_USE bool finish_operator(struct inference *p, struct note *note) {
    struct emitted *graph = note->graph;
    enum mo_syntax_token op = graph->output.operation;
    if (op == MO_T_EOF || op == MO_T_AND || op == MO_T_OR || op == MO_T_IMPLIES || op == MO_T_NOT ||
        op == MO_T_DOTDOT)
        return true;
    enum mo_source_ability ability =
        op == MO_T_EQ || op == MO_T_NE                                     ? MO_SOURCE_EQUAL
        : op == MO_T_LT || op == MO_T_LE || op == MO_T_GT || op == MO_T_GE ? MO_SOURCE_ORDER
                                                                           : MO_SOURCE_NUMBER;
    MO_ASSERT(graph->output.operand_count != 0 && graph->operands[0] != NULL);
    const struct mo_source_type *type = graph->operands[0]->output.type;
    bool numeric = strcmp(type->name, "Int") == 0 || strcmp(type->name, "BigInt") == 0 ||
                   strcmp(type->name, "Decimal") == 0 || strcmp(type->name, "Float") == 0;
    bool scalar = numeric || strcmp(type->name, "Bool") == 0 || strcmp(type->name, "Byte") == 0 ||
                  strcmp(type->name, "Text") == 0 || strcmp(type->name, "Char") == 0 ||
                  strcmp(type->name, "Unit") == 0 || strcmp(type->name, "Bytes") == 0 ||
                  strcmp(type->name, "Time") == 0 || strcmp(type->name, "Duration") == 0;
    bool supported = ability == MO_SOURCE_NUMBER  ? numeric
                     : ability == MO_SOURCE_EQUAL ? scalar || strcmp(type->name, "Range") == 0
                                                  : scalar;
    p->span = note->output.syntax->span;
    if (type->kind != MO_SOURCE_NAMED || type->module != UINT32_MAX || !supported)
        return unfinished(p);
    struct mo_ability_implementation *implementation =
        allocate(p, 1, sizeof(*implementation), _Alignof(struct mo_ability_implementation));
    struct mo_checked_ability_use *use =
        allocate(p, 1, sizeof(*use), _Alignof(struct mo_checked_ability_use));
    if (p->failed)
        return false;
    *implementation = (struct mo_ability_implementation){
        .ability = ability, .kind = MO_ABILITY_NATIVE, .type = type};
    *use = (struct mo_checked_ability_use){graph->operands[0]->index, implementation};
    graph->output.ability_count = 1;
    graph->output.abilities = use;
    return true;
}

static MO_MUST_USE struct mo_inferred_body *
infer_function(struct mo_arena *arena, const struct mo_module_interfaces *interfaces,
               const struct mo_signatures *signatures, uint32_t function, struct mo_diags *diags,
               struct mo_checked_function *checked) {
    MO_ASSERT(arena != NULL && interfaces != NULL && signatures != NULL && diags != NULL);
    MO_ASSERT(function < signatures->count && signatures->work != NULL);
    MO_ASSERT(*signatures->work <= MO_LIMIT_PROGRAM_CHECK_WORK);
    const struct mo_function_signature *signature = &signatures->items[function];
    const struct mo_syntax *syntax = signature->function->declaration;
    struct inference p = {.arena = arena,
                          .diags = diags,
                          .interfaces = interfaces,
                          .signatures = signatures,
                          .function = signature,
                          .checked = checked != NULL,
                          .span = syntax->span};
    bool finalized = false;
    if (syntax->children[4]->count != 0) {
        p.span = syntax->children[4]->children[0]->span;
        bool reported = unfinished(&p);
        (void)reported;
        goto failed;
    }
    struct term *fn = annotation(&p, signature->type, NULL, 0);
    if (fn == NULL)
        goto failed;
    p.result = argument(&p, fn, fn->shape.count - 1);
    if (p.result == NULL)
        goto failed;
    const struct mo_syntax *parameters = syntax->children[1];
    struct note **parameter_notes =
        checked == NULL
            ? NULL
            : allocate(&p, parameters->count, sizeof(*parameter_notes), _Alignof(struct note *));
    if (checked != NULL && parameter_notes == NULL)
        goto failed;
    for (uint32_t i = 0; i < parameters->count; i++) {
        struct term *type = argument(&p, fn, i);
        if (type == NULL || !bind(&p, parameters->children[i]->children[0], type, false))
            goto failed;
        if (checked != NULL)
            parameter_notes[i] = p.last;
    }
    struct value v = named(p.result, "Unit")
                         ? discard(&p, syntax->children[5])
                         : infer_as(&p, syntax->children[5], p.result, MO_CODE_RESULT_TYPE);
    if (v.type == NULL || !leave(&p, NULL))
        goto failed;
    struct mo_inferred_body *out = allocate(&p, 1, sizeof(*out), _Alignof(struct mo_inferred_body));
    struct mo_inferred_expression *items =
        allocate(&p, p.count, sizeof(*items), _Alignof(struct mo_inferred_expression));
    if (p.failed)
        goto failed;
    *out = (struct mo_inferred_body){.function = signature, .count = p.count, .items = items};
    finalized = true;
    bool valid = finish_notes(&p, items);
    (void)valid;
    if (!p.fatal && !finish_patterns(&p)) {
        mo_diags_sort(diags);
        return NULL;
    }
    if (p.error.code != 0)
        goto failed;
    if (checked != NULL) {
        struct mo_checked_expression *rows =
            allocate(&p, p.count, sizeof(*rows), _Alignof(struct mo_checked_expression));
        struct mo_checked_binding *bindings =
            allocate(&p, p.binding_count, sizeof(*bindings), _Alignof(struct mo_checked_binding));
        const struct mo_checked_binding **parameter_bindings =
            allocate(&p, parameters->count, sizeof(*parameter_bindings),
                     _Alignof(const struct mo_checked_binding *));
        struct note **lambdas =
            allocate(&p, p.lambda_count, sizeof(*lambdas), _Alignof(struct note *));
        if (p.failed)
            goto failed;
        uint32_t count = 0, binding_count = 0, lambda_count = 0;
        for (struct note *n = p.notes; n != NULL; n = n->next) {
            if (n->graph != NULL) {
                n->index = count++;
                if (n->graph->lambda != NULL)
                    lambdas[lambda_count++] = n;
            }
            if (n->binding != n)
                continue;
            const struct mo_syntax *name = n->output.syntax;
            uint32_t length = name->span.end - name->span.start;
            char *text = allocate(&p, (size_t)length + 1, 1, 1);
            if (text == NULL)
                goto failed;
            memcpy(text, name->text, length);
            text[length] = '\0';
            n->binding_index = binding_count++;
            bindings[n->binding_index] = (struct mo_checked_binding){.syntax = name,
                                                                     .span = name->span,
                                                                     .name = text,
                                                                     .type = n->output.type,
                                                                     .variable = n->variable};
        }
        MO_ASSERT(binding_count == p.binding_count);
        MO_ASSERT(lambda_count == p.lambda_count);
        qsort(lambdas, lambda_count, sizeof(*lambdas), source_note_order);
        for (uint32_t i = 0; i < lambda_count; i++)
            lambdas[i]->graph->lambda->output.number = i + 1;
        for (uint32_t i = 0; i < parameters->count; i++) {
            MO_ASSERT(parameter_notes[i]->binding_index < binding_count);
            parameter_bindings[i] = &bindings[parameter_notes[i]->binding_index];
        }
        for (struct note *n = p.notes; n != NULL; n = n->next) {
            if (n->graph == NULL)
                continue;
            struct emitted *g = n->graph;
            uint32_t *operands =
                allocate(&p, g->output.operand_count, sizeof(*operands), _Alignof(uint32_t));
            if (operands == NULL)
                goto failed;
            for (uint32_t i = 0; i < g->output.operand_count; i++) {
                MO_ASSERT(g->operands[i] != NULL && g->operands[i]->index < n->index);
                operands[i] = g->operands[i]->index;
            }
            if (!finish_operator(&p, n))
                goto failed;
            if (g->lambda != NULL) {
                struct emitted_lambda *lambda = g->lambda;
                const struct mo_checked_binding **bound =
                    allocate(&p, lambda->output.parameter_count, sizeof(*bound),
                             _Alignof(const struct mo_checked_binding *));
                if (bound == NULL)
                    goto failed;
                for (uint32_t i = 0; i < lambda->output.parameter_count; i++) {
                    MO_ASSERT(lambda->parameters[i]->binding_index < binding_count);
                    bound[i] = &bindings[lambda->parameters[i]->binding_index];
                }
                lambda->output.parameters = bound;
                lambda->output.body = operands[0];
                g->output.lambda = &lambda->output;
            }
            rows[n->index] = g->output;
            rows[n->index].syntax = n->output.syntax;
            rows[n->index].type = n->output.type;
            rows[n->index].operands = operands;
            rows[n->index].never = n->output.never;
            if (g->output.receiver != UINT32_MAX) {
                MO_ASSERT(g->output.receiver < g->output.operand_count);
                rows[n->index].receiver = operands[g->output.receiver];
            }
            enum mo_syntax_kind kind = n->output.syntax->kind;
            if (kind == MO_S_NAME || kind == MO_S_VAR || kind == MO_S_BIND) {
                rows[n->index].symbol = n->output.symbol;
                if (n->binding != NULL) {
                    MO_ASSERT(n->binding->binding_index < binding_count);
                    rows[n->index].binding = &bindings[n->binding->binding_index];
                }
            }
        }
        *checked = (struct mo_checked_function){.signature = signature,
                                                .parameter_count = parameters->count,
                                                .parameters = parameter_bindings,
                                                .body = {.count = count,
                                                         .items = rows,
                                                         .root = v.note->index,
                                                         .binding_count = binding_count,
                                                         .bindings = bindings}};
    }
    return out;
failed:
    if (!p.fatal && !finalized) {
        p.failed = false;
        p.partial = true;
        bool partial_valid = finish_notes(&p, NULL);
        (void)partial_valid;
        if (!p.fatal && !finish_patterns(&p)) {
            mo_diags_sort(diags);
            return NULL;
        }
    }
    emit_problem(&p);
    mo_diags_sort(diags);
    return NULL;
}

struct mo_inferred_body *mo_infer_function(struct mo_arena *arena,
                                           const struct mo_module_interfaces *interfaces,
                                           const struct mo_signatures *signatures,
                                           uint32_t function, struct mo_diags *diags) {
    return infer_function(arena, interfaces, signatures, function, diags, NULL);
}

bool mo_check_function(struct mo_arena *arena, const struct mo_module_interfaces *interfaces,
                       const struct mo_signatures *signatures, uint32_t function,
                       struct mo_diags *diags, struct mo_checked_function *out) {
    MO_ASSERT(out != NULL);
    return infer_function(arena, interfaces, signatures, function, diags, out) != NULL;
}
