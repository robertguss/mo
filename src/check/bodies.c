#include "check/abilities.h"
#include "check/check.h"
#include "check/checked.h"
#include "check/data.h"
#include "check/internal.h"
#include "check/map_keys.h"
#include "check/native.h"
#include "check/patterns.h"
#include "check/signatures.h"

#include "base/codes.h"
#include "base/limits.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    bool deferred;
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
    struct capture_write *writes;
    uint32_t function, version;
    uint32_t pattern_group, pattern_alternative;
    bool variable;
    bool used;
};

struct emitted_lambda {
    struct mo_checked_lambda output;
    struct note **parameters;
    struct emitted_lambda *parent;
    struct emitted_lambda *next;
    struct capture *captures;
    const struct mo_syntax *syntax;
    struct capture_loop *loop;
    struct capture_path *path;
    uint32_t depth;
};

struct capture {
    struct local *local;
    struct mo_span use;
    struct capture *next;
};

struct capture_loop {
    const struct mo_syntax *syntax;
    struct capture_loop *parent;
};

struct capture_path {
    const struct mo_syntax *group;
    uint32_t arm;
    struct capture_path *parent;
};

struct capture_write {
    const struct mo_syntax *syntax, *target;
    struct capture_path *path;
    struct capture_write *next;
};

struct emitted_match {
    struct mo_checked_match output;
    struct mo_checked_arm *arms;
};

struct emitted {
    struct mo_checked_expression output;
    struct note **operands;
    struct emitted_lambda *lambda;
    struct emitted_match *match;
    const struct mo_source_type *generic;
    struct term *instantiated;
    struct term *pattern_type;
    struct term *try_result;
    struct failure_check *failure;
    const struct mo_source_type *function_type;
    const uint8_t *requirements;
    struct term **parameters; /* compact A-Z substitution terms */
};

struct note {
    struct mo_inferred_expression output;
    struct term *type;
    struct note *next;
    struct emitted *graph;
    struct note *binding;
    struct note *callee;
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

struct pattern_defaults {
    const struct mo_data_signature *data;
    const struct mo_source_type *type;
    const struct mo_checked_pattern **fields;
    struct pattern_defaults *next;
};

struct failure_check {
    struct failure_check *next;
    struct term *input, *value, *context, *fallback;
    struct note *input_note;
    struct emitted *graph;
    struct mo_span span;
    struct mo_constructor_ref wrapper;
    const struct mo_source_type *generic;
    enum mo_syntax_token op;
    uint16_t code;
    bool solved, never;
};

struct type_pair {
    const struct mo_source_type *a, *b;
    struct type_pair *next;
};

struct failure_guess {
    struct term *term;
    const struct mo_source_type *type;
    struct failure_guess *next;
};

struct inference {
    struct mo_arena *arena;
    struct mo_diags *diags;
    const struct mo_module_interfaces *interfaces;
    const struct mo_signatures *signatures;
    const struct mo_function_signature *function;
    struct mo_constant_context *constants;
    struct mo_ability_context abilities;
    struct name_slot *names;
    struct local *locals, *all_locals;
    struct note *notes, *last;
    struct typed_pattern *patterns;
    struct match_check *matches;
    struct pattern_defaults *defaults;
    struct dispatch *dispatches, *last_dispatch;
    struct failure_check *failures;
    struct type_pair *equal_types;
    struct emitted_lambda *lambda_frame, *lambdas;
    struct capture_loop *capture_loop;
    uint32_t capture_free; /* type parameters that lambdas capture, so can't hold capabilities */
    struct capture_path *capture_path;
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
    bool checked, spec;
    bool keep_patterns;
    bool failed, fatal, partial, unavailable;
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
    struct note *callee;
    struct dispatch *dispatch;
    bool never;
};

struct dispatch {
    struct dispatch *next;
    const struct mo_syntax *name;
    const struct mo_symbol *group;
    uint32_t count;
    struct term *type;
    struct emitted *graph;
    struct note *note;
    const struct mo_syntax *syntax, *call, *arguments;
    struct value receiver;
    struct value *inputs;
    bool position; /* `.0` on a tuple: `name` is the index */
    bool solved;
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
        : code == MO_CODE_LAMBDA_WRITE         ? "an anonymous function can't assign a var"
        : code == MO_CODE_CAPABILITY_CAPTURE   ? "pass capabilities as parameters instead"
        : code == MO_CODE_CAPTURE_ASSIGNMENT   ? "copy this value into a name first"
        : code == MO_CODE_EXIT_OUTSIDE_LOOP    ? "no enclosing loop in this function"
        : code == MO_CODE_MAP_KEY_REPEATED     ? "this key is already in the literal"
        : code == MO_CODE_DOT_CALL_PARENTHESES ? "the function takes only the receiver"
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
static void defer(struct inference *p, uint16_t code, struct mo_span span, const char *message,
                  const char *reason) {
    bool reported = problem(p, code, span, message, reason, NULL);
    (void)reported;
    p->failed = p->fatal;
}

static void emit_problem(struct inference *p) {
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

static MO_MUST_USE bool map_key_work(void *context, size_t amount) {
    struct inference *p = context;
    for (size_t i = 0; i < amount; i++)
        if (!work(p))
            return false;
    return true;
}

static MO_MUST_USE void *map_key_allocate(void *context, size_t bytes) {
    return allocate(context, bytes, 1, 1);
}

struct literal_key {
    struct mo_map_literal_key value;
    const struct mo_syntax *syntax;
    uint32_t position, version;
};

static MO_MUST_USE bool map_key_order(struct inference *p, const struct literal_key *a,
                                      const struct literal_key *b, int *order) {
    const struct mo_map_literal_key *x = &a->value, *y = &b->value;
    if (!work(p))
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

static MO_MUST_USE bool map_keys(struct inference *p, const struct mo_syntax *node,
                                 const uint32_t *versions) {
    if (node->count < 2)
        return true;
    struct literal_key *keys =
        allocate(p, node->count, sizeof(*keys), _Alignof(struct literal_key));
    struct literal_key *scratch =
        allocate(p, node->count, sizeof(*scratch), _Alignof(struct literal_key));
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
        if (value.category != 0)
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
           problem(p, MO_CODE_MAP_KEY_REPEATED, repeated->syntax->span,
                   "a map literal gives the same key twice",
                   "Each key in a map literal must be different (reference 3.8).", first->syntax);
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
    a->deferred = a->deferred || b->deferred;
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
    if (depth == 0 && !mo_signature_type_available(p->signatures, type)) {
        p->failed = p->fatal = p->unavailable = true;
        return NULL;
    }
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

static MO_MUST_USE int source_note_order(const void *a, const void *b) {
    const struct note *x = *(const struct note *const *)a;
    const struct note *y = *(const struct note *const *)b;
    uint32_t left = x->output.syntax->span.start, right = y->output.syntax->span.start;
    return left < right ? -1 : left > right ? 1 : 0;
}

static MO_MUST_USE int syntax_note_order(const void *a, const void *b) {
    const struct note *x = *(const struct note *const *)a;
    const struct note *y = *(const struct note *const *)b;
    uintptr_t left = (uintptr_t)x->output.syntax, right = (uintptr_t)y->output.syntax;
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
    if (p->checked && name->kind != MO_S_UNDERSCORE) {
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

static MO_MUST_USE bool capture_local(struct inference *p, struct local *local,
                                      struct mo_span use) {
    for (struct emitted_lambda *lambda = p->lambda_frame;
         lambda != NULL && local->function < lambda->depth; lambda = lambda->parent) {
        if (!work(p))
            return false;
        struct capture **at = &lambda->captures;
        while (*at != NULL && (*at)->local != local) {
            if (!work(p))
                return false;
            at = &(*at)->next;
        }
        struct capture *value = *at;
        if (value != NULL) {
            if (value->use.start <= use.start)
                continue;
            *at = value->next;
        } else {
            if (lambda->output.capture_count == MO_LIMIT_FIELDS)
                return problem(p, MO_CODE_TOO_MANY_FIELDS, use,
                               "an anonymous function captures too many values", NULL, NULL);
            value = allocate(p, 1, sizeof(*value), _Alignof(struct capture));
            if (value == NULL)
                return false;
            lambda->output.capture_count++;
        }
        *value = (struct capture){.local = local, .use = use};
        at = &lambda->captures;
        while (*at != NULL && (*at)->use.start < use.start) {
            if (!work(p))
                return false;
            at = &(*at)->next;
        }
        value->next = *at;
        *at = value;
    }
    return true;
}

static MO_MUST_USE bool capture_write(struct inference *p, struct local *local,
                                      const struct mo_syntax *syntax,
                                      const struct mo_syntax *target) {
    if (!p->checked)
        return true;
    struct capture_write *write = allocate(p, 1, sizeof(*write), _Alignof(struct capture_write));
    if (write == NULL)
        return false;
    *write = (struct capture_write){syntax, target, p->capture_path, local->writes};
    local->writes = write;
    return true;
}

static MO_MUST_USE bool capture_path(struct inference *p, const struct mo_syntax *group,
                                     uint32_t arm) {
    if (!p->checked)
        return true;
    struct capture_path *path = allocate(p, 1, sizeof(*path), _Alignof(struct capture_path));
    if (path == NULL)
        return false;
    *path = (struct capture_path){group, arm, p->capture_path};
    p->capture_path = path;
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
    if (p->signatures->recovering &&
        (symbol->declaration->kind == MO_S_STRUCT || symbol->declaration->kind == MO_S_CASE_DECL ||
         symbol->declaration->kind == MO_S_ENUM)) {
        const struct mo_symbol *owner =
            symbol->owner == UINT32_MAX
                ? symbol
                : &p->interfaces->scopes[symbol->module].symbols[symbol->owner];
        const struct mo_nominal_signature *nominal =
            mo_nominal_signature_by_symbol(p->signatures, owner);
        if (nominal == NULL || nominal->invalid) {
            p->failed = p->fatal = p->unavailable = true;
            return NULL;
        }
    }
    return mo_data_signature_by_symbol(p->signatures, symbol);
}

static MO_MUST_USE bool visible_data(struct inference *p, const struct mo_symbol *owner) {
    if (owner->module == p->function->function->module)
        return true;
    if (!owner->insides)
        return problem(p, MO_CODE_DATA_PRIVATE, p->span, "the type's fields or cases are hidden",
                       "Outside its module a type exposed without (..) can't be built, taken apart "
                       "or have its fields read (reference 10.2).",
                       owner->declaration->children[0]);
    return true;
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
        const struct mo_module_scope *target =
            lo < scope->alias_count && compare_text(prefix, scope->aliases[lo].name) == 0
                ? &p->interfaces->scopes[scope->aliases[lo].module]
                : NULL;
        bool implicit = mo_source_builtin(prefix->text, prefix->span.end - prefix->span.start) ||
                        (scope->library && compare_text(prefix, scope->name) == 0);
        for (uint32_t i = 0; implicit && target == NULL && i < p->interfaces->count; i++) {
            if (!work(p))
                return NULL;
            const struct mo_module_scope *library = &p->interfaces->scopes[i];
            if (library->library && compare_text(prefix, library->name) == 0)
                target = library;
        }
        if (target == NULL) {
            uint32_t meanings = 0;
            const struct mo_symbol *type = definition(p, prefix, NULL, &meanings);
            size_t length = prefix->span.end - prefix->span.start;
            bool parameter = length == 1 && prefix->text[0] >= 'A' && prefix->text[0] <= 'Z' &&
                             (p->function->type->parameters &
                              (UINT32_C(1) << (unsigned)(prefix->text[0] - 'A'))) != 0;
            if (type != NULL || mo_source_builtin(prefix->text, length) || parameter)
                return NULL; /* native dispatch follows in identifier */
            bool reported =
                problem(p, MO_CODE_IMPORT_MISSING, prefix->span, "a module prefix isn't in scope",
                        "Use names the modules available to this file (reference 10.3).", NULL);
            (void)reported;
            return NULL;
        }
        const struct mo_symbol *s = find_symbol(target, name, count);
        if (s != NULL && s->owner != UINT32_MAX) {
            const struct mo_symbol *owner = &p->interfaces->scopes[s->module].symbols[s->owner];
            if (owner->exported && !visible_data(p, owner))
                return NULL;
        }
        if (s != NULL && !s->exported && s->module != p->function->function->module) {
            bool reported = problem(p, MO_CODE_IMPORT_PRIVATE, name->span,
                                    "everything not on expose is private to the module",
                                    "Everything not on expose is private to the module "
                                    "(reference 10.1).",
                                    s->declaration->children[0]);
            (void)reported;
            return NULL;
        }
        if (lo < scope->alias_count && compare_text(prefix, scope->aliases[lo].name) == 0 &&
            !mo_use_import(p->signatures, p->function->function->module,
                           scope->aliases[lo].origin)) {
            bool reported = problem(p, MO_CODE_CHECK_PROGRAM_WORK, name->span,
                                    "the program needs too much import-checking work", NULL, NULL);
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
    if (!mo_use_import(p->signatures, p->function->function->module, scope->imported[lo].origin)) {
        bool reported = problem(p, MO_CODE_CHECK_PROGRAM_WORK, name->span,
                                "the program needs too much import-checking work", NULL, NULL);
        (void)reported;
        return NULL;
    }
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

static MO_MUST_USE const struct mo_native_signature *
native_definition(struct inference *p, const char *module, const struct mo_syntax *name) {
    for (uint32_t i = 0; i < p->interfaces->count; i++) {
        if (!work(p))
            return NULL;
        const struct mo_module_scope *scope = &p->interfaces->scopes[i];
        if (!scope->library || scope->name == NULL || strcmp(scope->name, module) != 0)
            continue;
        uint32_t count = 0;
        const struct mo_symbol *s = find_symbol(scope, name, &count);
        if (s != NULL && s->declaration->kind == MO_S_FN &&
            (s->module == p->function->function->module || s->exported))
            return NULL;
    }
    for (uint32_t i = 0; i < p->signatures->native_count; i++) {
        const struct mo_native_signature *native = &p->signatures->native[i];
        if (strcmp(native->module, module) == 0 && compare_text(name, native->name) == 0)
            return native;
    }
    return NULL;
}

static MO_MUST_USE struct value function_value(struct inference *p,
                                               const struct mo_source_type *type,
                                               const uint8_t requirements[26]) {
    struct term *parameters[26] = {NULL};
    struct value out = graph(p, (struct value){.type = annotation(p, type, parameters, 0)}, 0);
    if (!p->checked || out.type == NULL || type->parameters == 0)
        return out;
    uint32_t count = 0;
    for (unsigned i = 0; i < 26; i++)
        count += (type->parameters & (UINT32_C(1) << i)) != 0 ? 1u : 0u;
    out.graph->parameters =
        allocate(p, count, sizeof(*out.graph->parameters), _Alignof(struct term *));
    if (p->failed)
        return (struct value){0};
    out.graph->function_type = type;
    out.graph->requirements = requirements;
    uint32_t at = 0;
    for (unsigned i = 0; i < 26; i++)
        if ((type->parameters & (UINT32_C(1) << i)) != 0)
            out.graph->parameters[at++] = parameters[i];
    return out;
}

static MO_MUST_USE struct value native_identifier(struct inference *p,
                                                  const struct mo_native_signature *signature) {
    if (signature->spec_only && !p->spec) {
        bool reported = unfinished(p);
        (void)reported;
        return (struct value){0};
    }
    struct value out = function_value(p, signature->type, signature->requirements);
    if (p->checked && out.type != NULL)
        out.graph->output.native = signature->qualified;
    return out;
}

static MO_MUST_USE struct value declared_function(struct inference *p, const struct mo_symbol *s) {
    uint32_t lo = 0, hi = p->signatures->count;
    uintptr_t wanted = (uintptr_t)s;
    while (lo < hi) {
        if (!work(p))
            return (struct value){0};
        uint32_t mid = lo + (hi - lo) / 2;
        const struct mo_symbol *at = p->signatures->items[mid].function;
        if (at->module < s->module || (at->module == s->module && (uintptr_t)at < wanted))
            lo = mid + 1;
        else
            hi = mid;
    }
    MO_ASSERT(lo < p->signatures->count && p->signatures->items[lo].function == s);
    const struct mo_function_signature *signature = &p->signatures->items[lo];
    if (signature->type == NULL) {
        MO_ASSERT(p->signatures->recovering);
        p->failed = p->fatal = p->unavailable = true;
        return (struct value){0};
    }
    struct value out = function_value(p, signature->type, signature->requirements);
    out.symbol = s;
    return out;
}

static MO_MUST_USE const struct mo_symbol *select_overload(struct inference *p,
                                                           const struct mo_symbol *group,
                                                           uint32_t count, struct term *receiver) {
    if (count == 1)
        return group;
    receiver = root(receiver);
    if (receiver->variable)
        return NULL;
    const struct mo_module_scope *scope = &p->interfaces->scopes[group->module];
    for (uint32_t i = 0; i < count; i++) {
        if (!work(p))
            return NULL;
        MO_ASSERT(group[i].receiver < scope->count);
        if (receiver->shape.declaration == &scope->symbols[group[i].receiver])
            return &group[i];
    }
    bool reported =
        problem(p, MO_CODE_ARGUMENT_TYPE, p->span, "no overload takes this first argument's type",
                "A call picks the function whose first parameter matches its first "
                "argument (reference 5.1).",
                NULL);
    (void)reported;
    return NULL;
}

static MO_MUST_USE struct value deferred_function(struct inference *p, const struct mo_syntax *name,
                                                  const struct mo_symbol *group, uint32_t count) {
    struct dispatch *pending = allocate(p, 1, sizeof(*pending), _Alignof(struct dispatch));
    struct value out = graph(p, (struct value){.type = fresh(p, 0)}, 0);
    if (pending == NULL || out.type == NULL)
        return (struct value){0};
    out.type->deferred = group == NULL;
    *pending = (struct dispatch){
        .name = name, .group = group, .count = count, .type = out.type, .graph = out.graph};
    if (p->last_dispatch == NULL)
        p->dispatches = pending;
    else
        p->last_dispatch->next = pending;
    p->last_dispatch = pending;
    out.dispatch = pending;
    return out;
}

static MO_MUST_USE const struct mo_module_scope *type_scope(struct inference *p,
                                                            struct term *type) {
    if (type->shape.declaration != NULL)
        return &p->interfaces->scopes[type->shape.module];
    if (type->shape.kind != MO_SOURCE_NAMED)
        return NULL;
    for (uint32_t i = 0; i < p->interfaces->count; i++) {
        if (!work(p))
            return NULL;
        const struct mo_module_scope *scope = &p->interfaces->scopes[i];
        if (scope->library && scope->name != NULL && strcmp(scope->name, type->shape.name) == 0)
            return scope;
    }
    return NULL;
}

static MO_MUST_USE struct value identifier(struct inference *p, const struct mo_syntax *node,
                                           const struct mo_syntax *prefix) {
    if (node->kind == MO_S_QUALIFIED) {
        prefix = node->children[0];
        node = node->children[1];
    }
    if (prefix == NULL && node->kind == MO_S_NAME) {
        struct name_slot *s = slot(p, node, false);
        if (s != NULL && s->active != NULL) {
            if (p->checked && s->active->function != p->lambda) {
                if (!capture_local(p, s->active, node->span))
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
        if (prefix != NULL) {
            for (uint32_t i = 0; i < p->signatures->native_count; i++) {
                const struct mo_native_signature *native = &p->signatures->native[i];
                if (compare_text(prefix, native->module) == 0 &&
                    compare_text(node, native->name) == 0)
                    return native_identifier(p, native);
            }
        }
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
        if (data == NULL) {
            MO_ASSERT(p->unavailable);
            return (struct value){0};
        }
        if (!visible_data(p, data->type->declaration))
            return (struct value){0};
        if (data->count == 0) {
            struct term *parameters[26] = {NULL};
            struct value out = graph(
                p, (struct value){.type = annotation(p, data->type, parameters, 0), .symbol = s},
                0);
            if (p->checked && out.type != NULL) {
                out.graph->generic = data->type;
                out.graph->instantiated = out.type;
            }
            return out;
        }
        const struct mo_syntax *field = data->by_index[0]->name;
        bool reported =
            missing_field(p, node->span, field == NULL ? NULL : field->text,
                          field == NULL ? 0 : field->span.end - field->span.start, field);
        (void)reported;
        return (struct value){0};
    }
    bool constant = s->declaration->kind == MO_S_CONST;
    if (s->declaration->kind != MO_S_FN && !constant) {
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
    if (constant) {
        if (p->constants == NULL) {
            bool reported = unfinished(p);
            (void)reported;
            return (struct value){0};
        }
        struct mo_checked_constant *value = mo_constant_by_symbol(p->constants, s);
        if (value->type == NULL) {
            uint32_t index = (uint32_t)(value - p->constants->items);
            p->failed = p->fatal = p->unavailable = true;
            if (!p->constants->done[index]) {
                p->constants->pending = value;
                p->constants->pending_span = node->span;
            }
            return (struct value){0};
        }
        return graph(p, (struct value){.type = annotation(p, value->type, NULL, 0), .symbol = s},
                     0);
    }
    if (count > 1)
        return deferred_function(p, node, s, count);
    return declared_function(p, s);
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

static MO_MUST_USE struct term *builtin_part(struct inference *p,
                                             const struct mo_builtin_case *data, struct term *type,
                                             uint32_t index) {
    MO_ASSERT(index < data->count);
    if (data->fields[0] == NULL)
        return argument(p, type, data->parameter);
    return native(p, strcmp(data->name, "Entry") == 0 && index == 1 ? "EntryKind" : "Text", 0);
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
    struct value out =
        graph(p, (struct value){.type = type, .builtin_case = data->name}, data->count);
    if (out.type == NULL)
        return out;
    if (p->checked) {
        out.graph->generic = mo_builtin_template(data);
        out.graph->instantiated = type;
    }
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
        struct term *field = builtin_part(p, data, type, index);
        if (field == NULL)
            return (struct value){0};
        struct value v = infer(p, arg->kind == MO_S_NAMED_ARG ? arg->children[1] : arg, field);
        if (v.type == NULL)
            return (struct value){0};
        if (p->checked) {
            if (arg->kind == MO_S_NAMED_ARG) {
                struct value supplied =
                    graph(p, (struct value){.type = v.type, .never = v.never}, 1);
                if (supplied.type == NULL)
                    return supplied;
                supplied.graph->output.field = index;
                supplied.graph->operands[0] = v.note;
                if (!record(p, arg, supplied))
                    return (struct value){0};
                v.note = p->last;
            }
            out.graph->operands[i] = v.note;
        }
        out.never = out.never || v.never;
    }
    return out;
}

static MO_MUST_USE struct value construct(struct inference *p, const struct mo_data_signature *data,
                                          const struct mo_syntax *args, struct term *expected) {
    if (!visible_data(p, data->type->declaration))
        return (struct value){0};
    struct term *parameters[26] = {NULL};
    struct term *type = annotation(p, data->type, parameters, 0);
    if (type == NULL || (expected != NULL && !unify(p, type, expected, 0)))
        return (struct value){0};
    struct value out = graph(p, (struct value){.type = type, .symbol = data->symbol}, args->count);
    if (out.type == NULL)
        return out;
    if (p->checked) {
        out.graph->generic = data->type;
        out.graph->instantiated = type;
    }
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
            if (p->checked) {
                if (!data->unnamed) {
                    struct value supplied =
                        graph(p, (struct value){.type = v.type, .never = v.never}, 1);
                    if (supplied.type == NULL)
                        return supplied;
                    supplied.graph->output.field = selected[i]->index;
                    supplied.graph->operands[0] = v.note;
                    if (!record(p, arg, supplied))
                        return (struct value){0};
                    v.note = p->last;
                }
                out.graph->operands[i] = v.note;
            }
            ranges[i] = (struct note_range){previous == NULL ? p->notes : previous->next, p->last};
            never = never || v.never;
        }
    }
    order_notes(p, before, ranges, args->count);
    out.never = never;
    return out;
}

static MO_MUST_USE struct value call(struct inference *p, struct value callee,
                                     const struct mo_syntax *args, const struct value *receiver);

static MO_MUST_USE struct value ability_function(struct inference *p, struct value receiver,
                                                 const struct mo_syntax *name) {
    bool text = compare_text(name, "to_text") == 0;
    struct term *type = make_type(p, (struct mo_source_type){.kind = MO_SOURCE_FUNCTION,
                                                             .name = "fn",
                                                             .module = UINT32_MAX,
                                                             .count = text ? 2u : 3u});
    if (type == NULL)
        return (struct value){0};
    type->arguments[0] = receiver.type;
    if (!text)
        type->arguments[1] = receiver.type;
    type->arguments[type->shape.count - 1] = native(p, text ? "Text" : "Ordering", 0);
    if (p->failed)
        return (struct value){0};
    struct value out = graph(p, (struct value){.type = type}, 0);
    if (p->checked && out.type != NULL)
        out.graph->output.native = text ? "to_text" : "compare";
    return out;
}

/* Resolve a member without walking its explicit arguments. A method returns
 * its function type; a visible field returns its projection. */
static MO_MUST_USE struct value member(struct inference *p, struct value v,
                                       const struct mo_syntax *name, bool *called) {
    *called = true;
    if (v.type == NULL)
        return v;
    p->span = name->span;
    struct term *type = root(v.type);
    MO_ASSERT(!type->variable);
    if (!type->variable && type->shape.kind == MO_SOURCE_NAMED &&
        type->shape.module == UINT32_MAX) {
        if (named(type, "Entry") &&
            (compare_text(name, "name") == 0 || compare_text(name, "kind") == 0)) {
            *called = false;
            uint32_t index = compare_text(name, "name") == 0 ? 0u : 1u;
            const struct mo_builtin_case *data = mo_builtin_case_by_type("Entry");
            struct value out = graph(
                p, (struct value){.type = builtin_part(p, data, type, index), .never = v.never}, 1);
            if (p->checked && out.type != NULL) {
                out.graph->output.field = index;
                out.graph->output.native = index == 0 ? "Entry.name" : "Entry.kind";
                out.graph->operands[0] = v.note;
            }
            return out;
        }
        const struct mo_native_signature *signature = native_definition(p, type->shape.name, name);
        if (signature != NULL)
            return native_identifier(p, signature);
    }
    const struct mo_data_signature *data = type->variable || type->shape.declaration == NULL
                                               ? NULL
                                               : data_type(p, type->shape.declaration);
    const struct mo_field_signature *field = data == NULL ? NULL : find_field(data, name);
    bool visible = data != NULL && (type->shape.module == p->function->function->module ||
                                    type->shape.declaration->insides);
    if ((field == NULL || !visible) &&
        (compare_text(name, "to_text") == 0 || compare_text(name, "compare") == 0))
        return ability_function(p, v, name);
    if (field == NULL || !visible) {
        uint32_t count = 0;
        const struct mo_module_scope *owner = type_scope(p, type);
        if (p->failed)
            return (struct value){0};
        const struct mo_symbol *method = owner == NULL ? NULL : find_symbol(owner, name, &count);
        bool plausible = method != NULL && method->declaration->kind == MO_S_FN &&
                         (method->module == p->function->function->module || method->exported);
        if (!plausible) {
            method = definition(p, name, NULL, &count);
            plausible = method != NULL && method->declaration->kind == MO_S_FN;
        }
        if (plausible) {
            method = select_overload(p, method, count, type);
            if (method == NULL)
                return (struct value){0};
            return declared_function(p, method);
        }
    }
    if (type->shape.declaration == NULL) {
        bool reported = problem(p, MO_CODE_NAME_UNKNOWN, name->span,
                                "no visible function has this dot-call name",
                                "A dot call looks in the type's module, then among the names in "
                                "scope (reference 5.4).",
                                NULL);
        (void)reported;
        return (struct value){0};
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
    *called = false;
    struct term *parameters[26] = {NULL};
    for (uint32_t i = 0; i < type->shape.count; i++) {
        MO_ASSERT(data->parameters != NULL && i < data->parameters->count);
        parameters[data->parameters->children[i]->text[0] - 'A'] = argument(p, type, i);
        if (p->failed)
            return (struct value){0};
    }
    struct value out = graph(p,
                             (struct value){.type = annotation(p, field->type, parameters, 0),
                                            .field = field,
                                            .never = v.never},
                             1);
    if (p->checked && out.type != NULL) {
        out.graph->output.field = field->index;
        out.graph->operands[0] = v.note;
        out.graph->generic = data->type;
        out.graph->instantiated = type;
    }
    return out;
}

static bool receiver_only(struct term *type) {
    type = root(type);
    return !type->variable && type->shape.kind == MO_SOURCE_FUNCTION && type->shape.count == 2;
}

/* `x.f()` where `f` takes only the receiver: reference 5.4 spells it `x.f`. A
 * field holding a function is called with `()` and never reaches here. */
static MO_MUST_USE bool empty_dot_call(struct inference *p, const struct mo_syntax *site) {
    struct mo_span span = site->span;
    bool reported = problem(p, MO_CODE_DOT_CALL_PARENTHESES, span, "write x.f, not x.f()",
                            "x.f, with no other arguments, calls f(x) (reference 5.4).", NULL);
    (void)reported;
    if (p->error.code == MO_CODE_DOT_CALL_PARENTHESES &&
        p->error.primary.span.start == span.start) {
        p->edit = (struct mo_edit){span, ""};
        p->fix = (struct mo_fix){"remove the parentheses", &p->edit, 1};
        p->error.fixes = &p->fix;
        p->error.fix_count = 1;
    }
    return false;
}

static MO_MUST_USE struct value field_value(struct inference *p, struct value receiver,
                                            const struct mo_syntax *name,
                                            const struct mo_syntax *syntax,
                                            const struct mo_syntax *site, bool *called) {
    if (receiver.type == NULL)
        return receiver;
    const struct mo_syntax *args = site == NULL ? NULL : site->children[0];
    if (root(receiver.type)->variable) {
        *called = true;
        uint32_t count = args == NULL ? 0u : args->count;
        struct value out = deferred_function(p, name, NULL, count);
        if (out.type == NULL)
            return out;
        struct dispatch *pending = out.dispatch;
        pending->syntax = syntax;
        pending->call = site;
        pending->arguments = args;
        pending->receiver = receiver;
        pending->inputs = allocate(p, count, sizeof(*pending->inputs), _Alignof(struct value));
        if (pending->inputs == NULL)
            return (struct value){0};
        out.never = receiver.never;
        for (uint32_t i = 0; i < count; i++) {
            if (args->children[i]->kind == MO_S_NAMED_ARG) {
                bool reported = problem(p, MO_CODE_CALL_ARGUMENTS, args->children[i]->span,
                                        "a function is called with named fields",
                                        "Arguments are passed by position (reference 5.2).", NULL);
                (void)reported;
                return (struct value){0};
            }
            pending->inputs[i] = infer(p, args->children[i], NULL);
            if (pending->inputs[i].type == NULL)
                return (struct value){0};
            out.never = out.never || pending->inputs[i].never;
        }
        return out;
    }
    struct value resolved = member(p, receiver, name, called);
    if (resolved.type == NULL || !*called)
        return resolved;
    if (args != NULL && args->count == 0 && receiver_only(resolved.type) &&
        !empty_dot_call(p, site))
        return (struct value){0};
    if (p->checked && resolved.symbol != NULL) {
        if (!record(p, name, resolved))
            return (struct value){0};
        resolved.note = p->last;
    }
    return call(p, resolved, args, &receiver);
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
                                     const struct mo_syntax *args, const struct value *receiver) {
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
    uint32_t offset = receiver == NULL ? 0u : 1u;
    uint32_t count = args == NULL ? 0u : args->count;
    if (t->shape.count != count + offset + 1) {
        bool reported =
            problem(p, MO_CODE_CALL_ARGUMENTS,
                    args == NULL || args->span.start == args->span.end ? p->span : args->span,
                    "a call's arguments don't match its parameters",
                    "A function has no default or optional parameters; pass every "
                    "argument by position (reference 5.2).",
                    NULL);
        (void)reported;
        return (struct value){0};
    }
    bool never = callee.never;
    if (receiver != NULL) {
        if (!unify_as(p, receiver->type, argument(p, t, 0), MO_CODE_ARGUMENT_TYPE))
            return (struct value){0};
        never = never || receiver->never;
    }
    for (uint32_t i = 0; i < count; i++)
        if (args->children[i]->kind == MO_S_NAMED_ARG) {
            bool reported = problem(p, MO_CODE_CALL_ARGUMENTS, args->children[i]->span,
                                    "a function is called with named fields",
                                    "Arguments are passed by position; only records and cases "
                                    "are built with named fields (reference 5.2).",
                                    NULL);
            (void)reported;
            return (struct value){0};
        }
    struct note_range *ranges = allocate(p, count, sizeof(*ranges), _Alignof(struct note_range));
    if (ranges == NULL)
        return (struct value){0};
    struct value result = {0};
    if (p->checked) {
        bool declared_dot = receiver != NULL && callee.note != NULL;
        uint32_t extra = declared_dot ? 1u : 0u;
        result =
            graph(p, (struct value){.type = argument(p, t, t->shape.count - 1)}, count + 1 + extra);
        if (result.type == NULL)
            return result;
        result.graph->operands[0] = receiver == NULL || declared_dot ? callee.note : receiver->note;
        if (declared_dot)
            result.graph->operands[1] = receiver->note;
        result.graph->output.symbol = callee.symbol;
        if (callee.graph != NULL) {
            result.graph->output.native = callee.graph->output.native;
            result.graph->function_type = callee.graph->function_type;
            result.graph->requirements = callee.graph->requirements;
            result.graph->parameters = callee.graph->parameters;
        }
        if (receiver != NULL)
            result.graph->output.receiver = extra;
    }
    struct note *before = p->last;
    for (unsigned pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < count; i++) {
            if (lambda_argument(args->children[i]) != (pass != 0))
                continue;
            struct note *previous = p->last;
            struct value arg =
                infer_as(p, args->children[i], argument(p, t, i + offset), MO_CODE_ARGUMENT_TYPE);
            if (arg.type == NULL)
                return (struct value){0};
            ranges[i] = (struct note_range){previous == NULL ? p->notes : previous->next, p->last};
            if (p->checked)
                result.graph->operands[i + (receiver != NULL && callee.note != NULL ? 2u : 1u)] =
                    arg.note;
            never = never || arg.never;
        }
    }
    /* Constraint solving may visit lambdas last; metadata retains written order
     * so this never suggests reordering evaluation to downstream consumers. */
    order_notes(p, before, ranges, count);
    if (!p->checked)
        result.type = argument(p, t, t->shape.count - 1);
    result.callee = callee.note;
    result.never = never;
    return result;
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
    if (op == MO_T_OR && root(a.type)->variable) {
        struct mo_span span = p->span;
        struct value fallback = infer(p, right, expected);
        struct value out = graph(p, (struct value){.type = fresh(p, 0), .never = a.never}, 2);
        struct failure_check *pending =
            allocate(p, 1, sizeof(*pending), _Alignof(struct failure_check));
        if (fallback.type == NULL || out.type == NULL || pending == NULL)
            return (struct value){0};
        *pending = (struct failure_check){.next = p->failures,
                                          .input = a.type,
                                          .value = out.type,
                                          .fallback = fallback.type,
                                          .graph = out.graph,
                                          .span = span,
                                          .never = fallback.never};
        p->failures = pending;
        if (p->checked) {
            out.graph->operands[0] = a.note;
            out.graph->operands[1] = fallback.note;
            out.graph->output.operation = MO_T_OR;
        }
        return out;
    }
    if (logical && op == MO_T_OR && named(a.type, "Option")) {
        struct term *payload = argument(p, a.type, 0);
        if (payload == NULL ||
            (expected != NULL && !unify_as(p, payload, expected, p->node_expectation)))
            return (struct value){0};
        struct value fallback = infer(p, right, payload);
        if (fallback.type == NULL)
            return fallback;
        struct value out = graph(p, (struct value){.type = payload, .never = a.never}, 2);
        if (p->checked && out.type != NULL) {
            out.graph->operands[0] = a.note;
            out.graph->operands[1] = fallback.note;
            out.graph->output.operation = MO_T_OR;
            out.graph->output.or_kind = MO_CHECK_OR_OPTION;
        }
        return out;
    }
    struct term *operand = logical             ? native(p, "Bool", 0)
                           : op == MO_T_DOTDOT ? native(p, "Int", 0)
                                               : a.type;
    if (operand == NULL || !unify_as(p, a.type, operand, logical ? MO_CODE_CONDITION_TYPE : 0))
        return (struct value){0};
    struct term *right_type = operand;
    struct term *result = logical || comparison ? native(p, "Bool", 0)
                          : op == MO_T_DOTDOT   ? native(p, "Range", 0)
                                                : operand;
    struct value b = {0};
    if (!logical && !comparison && op != MO_T_DOTDOT) {
        if (!p->checked && (named(operand, "Time") || named(operand, "Duration") ||
                            root(operand)->shape.kind == MO_SOURCE_PARAMETER)) {
            bool reported = unfinished(p);
            (void)reported;
            return (struct value){0};
        }
        if (named(operand, "Time") && (op == MO_T_PLUS || op == MO_T_MINUS)) {
            struct term *duration = native(p, "Duration", 0);
            if (duration == NULL)
                return (struct value){0};
            if (op == MO_T_PLUS) {
                if (expected != NULL && !unify_as(p, operand, expected, p->node_expectation))
                    return (struct value){0};
                right_type = duration;
            } else {
                struct mo_span span = p->span;
                b = infer(p, right, NULL);
                if (b.type == NULL)
                    return (struct value){0};
                if (!root(b.type)->variable || root(b.type)->numeric != 0) {
                    if (named(b.type, "Time")) {
                        result = duration;
                    } else {
                        if (!unify(p, b.type, duration, 0))
                            return (struct value){0};
                        result = operand;
                    }
                    if (expected != NULL && !unify_as(p, result, expected, p->node_expectation))
                        return (struct value){0};
                } else {
                    result = fresh(p, 0);
                    struct failure_check *pending =
                        allocate(p, 1, sizeof(*pending), _Alignof(struct failure_check));
                    if (result == NULL || pending == NULL ||
                        (expected != NULL && !unify(p, result, expected, 0)))
                        return (struct value){0};
                    root(result)->deferred = true;
                    *pending = (struct failure_check){.next = p->failures,
                                                      .input = operand,
                                                      .fallback = b.type,
                                                      .value = result,
                                                      .op = MO_T_MINUS,
                                                      .code = p->node_expectation,
                                                      .span = span};
                    p->failures = pending;
                }
            }
        } else if (named(operand, "Duration") &&
                   (op == MO_T_PLUS || op == MO_T_MINUS || op == MO_T_STAR)) {
            if (expected != NULL && !unify_as(p, operand, expected, p->node_expectation))
                return (struct value){0};
            if (op == MO_T_STAR) {
                right_type = native(p, "Int", 0);
                if (right_type == NULL)
                    return (struct value){0};
            }
        } else if (p->checked && (op == MO_T_PLUS || op == MO_T_MINUS || op == MO_T_STAR) &&
                   root(operand)->variable && root(operand)->numeric == 0 &&
                   root(operand)->deferred) {
            struct mo_span span = p->span;
            b = infer(p, right, NULL);
            result = fresh(p, 0);
            struct failure_check *pending =
                allocate(p, 1, sizeof(*pending), _Alignof(struct failure_check));
            if (b.type == NULL || result == NULL || pending == NULL ||
                (expected != NULL && !unify(p, result, expected, 0)))
                return (struct value){0};
            root(result)->deferred = true;
            *pending = (struct failure_check){.next = p->failures,
                                              .input = operand,
                                              .fallback = b.type,
                                              .value = result,
                                              .op = op,
                                              .code = p->node_expectation,
                                              .span = span};
            p->failures = pending;
        } else {
            struct term *numeric = root(operand)->shape.kind == MO_SOURCE_PARAMETER
                                       ? operand
                                       : fresh(p, INT | BIG | DECIMAL | FLOAT);
            if (numeric == NULL || !unify_as(p, operand, numeric,
                                             named(operand, "Byte") ? MO_CODE_BYTE_ARITHMETIC
                                                                    : MO_CODE_NUMBER_ABILITY))
                return (struct value){0};
            if (expected != NULL && !unify_as(p, operand, expected, p->node_expectation))
                return (struct value){0};
        }
    }
    if (b.type == NULL)
        b = logical ? condition(p, right, op == MO_T_AND) : infer(p, right, right_type);
    if (b.type == NULL)
        return (struct value){0};
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

static MO_MUST_USE bool solve_failure(struct inference *p, struct failure_check *at, bool defaults);

static MO_MUST_USE struct value propagate(struct inference *p, struct value input,
                                          struct term *expected) {
    if (input.type == NULL)
        return input;
    if (p->function->function->declaration->kind == MO_S_CONST && p->lambda == 0) {
        bool reported = problem(p, MO_CODE_TRY_RESULT, p->span,
                                "try has no enclosing function returning Option or Result",
                                "try passes a failure upwards from the nearest function or "
                                "anonymous function (reference 8.3).",
                                NULL);
        (void)reported;
        return (struct value){0};
    }
    struct value out = graph(
        p, (struct value){.type = expected == NULL ? fresh(p, 0) : expected, .never = input.never},
        1);
    struct failure_check *pending =
        allocate(p, 1, sizeof(*pending), _Alignof(struct failure_check));
    if (out.type == NULL || pending == NULL)
        return (struct value){0};
    *pending = (struct failure_check){.next = p->failures,
                                      .input = input.type,
                                      .value = out.type,
                                      .context = p->result,
                                      .input_note = input.note,
                                      .graph = out.graph,
                                      .span = p->span,
                                      .never = input.never};
    p->failures = pending;
    if (p->checked && out.type != NULL) {
        out.graph->operands[0] = input.note;
        out.graph->try_result = p->result;
        out.graph->failure = pending;
    }
    if (!root(p->result)->variable &&
        (named(input.type, "Option") || named(input.type, "Result")) &&
        !solve_failure(p, pending, false))
        return (struct value){0};
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
        return record(
            p, node, (struct value){.type = type, .binding = local->name, .bound = local->binding});
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
                struct term *field = builtin_part(p, builtin, nominal, index);
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
    /* Retain inference notes for contextual range checking, but no executable
     * rows: the normalized LITERAL already carries this spelling and type. */
    bool checked = p->checked;
    p->checked = false;
    struct value literal_value = infer(p, node, type);
    p->checked = checked;
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
    struct value emitted = graph(p, (struct value){.type = result}, 1 + 2 * arms->count);
    if (emitted.type == NULL)
        return emitted;
    uint32_t operand = 1;
    if (p->checked) {
        struct emitted_match *match =
            allocate(p, 1, sizeof(*match), _Alignof(struct emitted_match));
        struct mo_checked_arm *rows =
            allocate(p, arms->count, sizeof(*rows), _Alignof(struct mo_checked_arm));
        if (p->failed)
            return (struct value){0};
        *match = (struct emitted_match){.output = {.arm_count = arms->count, .arms = rows},
                                        .arms = rows};
        emitted.graph->match = match;
        emitted.graph->operands[0] = input.note;
    }
    bool never = true;
    for (uint32_t i = 0; i < arms->count; i++) {
        struct local *mark = p->locals;
        struct capture_path *old_path = p->capture_path;
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
        if (p->checked)
            emitted.graph->match->arms[i] = (struct mo_checked_arm){
                .syntax = arm, .pattern_count = alternatives->count, .guard = UINT32_MAX};
        if (arm->children[1] != NULL) {
            struct value guard = condition(p, arm->children[1], true);
            if (guard.type == NULL)
                return guard;
            if (p->checked) {
                emitted.graph->match->arms[i].guard = operand;
                emitted.graph->operands[operand++] = guard.note;
            }
        }
        if (!capture_path(p, node, i))
            return (struct value){0};
        struct value body = infer(p, arm->children[2], result);
        if (body.type == NULL || !leave(p, mark))
            return (struct value){0};
        p->capture_path = old_path;
        if (p->checked) {
            emitted.graph->match->arms[i].body = operand;
            emitted.graph->operands[operand++] = body.note;
        }
        never = never && body.never;
    }
    if (p->checked)
        emitted.graph->output.operand_count = operand;
    emitted.never = never;
    return emitted;
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
    struct value emitted =
        graph(p, (struct value){.type = result}, 2 + 2 * parts->count - (has_else ? 1u : 0u));
    if (emitted.type == NULL)
        return emitted;
    uint32_t operand = 0;
    struct local *mark = p->locals;
    bool previous = p->keep_patterns;
    p->keep_patterns = true;
    struct value test = infer_as(p, node->children[0], condition, MO_CODE_CONDITION_TYPE);
    p->keep_patterns = previous;
    if (test.type == NULL)
        return (struct value){0};
    if (p->checked)
        emitted.graph->operands[operand++] = test.note;
    struct capture_path *old_path = p->capture_path;
    if (!capture_path(p, node, 0))
        return (struct value){0};
    struct value branch = has_else ? infer_as(p, node->children[1], result, p->node_expectation)
                                   : discard(p, node->children[1]);
    if (branch.type == NULL || !leave(p, mark))
        return (struct value){0};
    p->capture_path = old_path;
    if (p->checked)
        emitted.graph->operands[operand++] = branch.note;
    bool never = has_else && branch.never;
    for (uint32_t i = 0; i < parts->count; i++) {
        const struct mo_syntax *part = parts->children[i];
        if (part->kind == MO_S_IF_PART) {
            p->keep_patterns = true;
            test = infer_as(p, part->children[0], condition, MO_CODE_CONDITION_TYPE);
            p->keep_patterns = previous;
            if (test.type == NULL)
                return (struct value){0};
            if (p->checked)
                emitted.graph->operands[operand++] = test.note;
        }
        const struct mo_syntax *body = part->children[part->kind == MO_S_ELSE ? 0 : 1];
        if (!capture_path(p, node, i + 1))
            return (struct value){0};
        branch = has_else ? infer_as(p, body, result, p->node_expectation) : discard(p, body);
        if (branch.type == NULL || !leave(p, mark))
            return (struct value){0};
        p->capture_path = old_path;
        if (p->checked)
            emitted.graph->operands[operand++] = branch.note;
        never = never && branch.never;
    }
    if (never)
        result = expected == NULL ? native(p, "Unit", 0) : expected;
    emitted.type = result;
    emitted.never = never;
    return emitted;
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

/* The part of tuple type `t` that `index` reads, or NULL after reporting. */
static MO_MUST_USE struct term *tuple_part(struct inference *p, struct term *t,
                                           const struct mo_syntax *index, uint32_t *part) {
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
        return NULL;
    }
    *part = at;
    return argument(p, t, at);
}

static MO_MUST_USE struct value position(struct inference *p, struct value v,
                                         const struct mo_syntax *index) {
    if (v.type == NULL)
        return v;
    struct term *t = root(v.type);
    if (t->variable) {
        /* A later use may settle the tuple's type (reference 5.1); solved with the dispatches. */
        struct value out = deferred_function(p, index, NULL, 0);
        if (out.type == NULL)
            return out;
        out.dispatch->receiver = v;
        out.dispatch->position = true;
        out.never = v.never;
        return out;
    }
    uint32_t at;
    struct term *part = tuple_part(p, t, index, &at);
    if (part == NULL)
        return (struct value){0};
    struct value out = graph(p, (struct value){.type = part, .never = v.never}, 1);
    if (p->checked && out.type != NULL) {
        out.graph->operands[0] = v.note;
        out.graph->output.field = at;
    }
    return out;
}

static MO_MUST_USE struct value infer_node(struct inference *p, const struct mo_syntax *node,
                                           struct term *expected) {
    switch (node->kind) {
    case MO_S_SEQUENCE:
        return sequence(p, node, expected);
    case MO_S_EXPR_STMT:
    case MO_S_GROUP:
        return infer_as(p, node->children[0], expected, p->node_expectation);
    case MO_S_TEXT_INTERPOLATED: {
        uint32_t count = node->count / 2;
        struct value out = graph(p, (struct value){.type = native(p, "Text", 0)}, count);
        if (out.type == NULL)
            return out;
        for (uint32_t i = 0; i < count; i++) {
            struct value hole = infer(p, node->children[2 * i + 1], NULL);
            if (hole.type == NULL)
                return (struct value){0};
            if (p->checked)
                out.graph->operands[i] = hole.note;
            out.never = out.never || hole.never;
        }
        return out;
    }
    case MO_S_NAME:
    case MO_S_CAPNAME:
    case MO_S_QUALIFIED:
        return identifier(p, node, NULL);
    case MO_S_FIELD_ACCESS: {
        if (node->children[0]->kind == MO_S_CAPNAME)
            return identifier(p, node->children[1], node->children[0]);
        bool called;
        return field_value(p, infer(p, node->children[0], NULL), node->children[1], node, NULL,
                           &called);
    }
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
            if (p->lambda != 0) {
                bool reported = problem(p, MO_CODE_LAMBDA_WRITE, name->span,
                                        "an anonymous function can't assign a var",
                                        "An anonymous function can read names in scope, but can't "
                                        "assign a var (reference 5.6).",
                                        variable->name);
                (void)reported;
                return (struct value){0};
            }
            if (!capture_write(p, variable, node, name))
                return (struct value){0};
            variable->version++;
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
            if (p->checked && node->kind != MO_S_BIND) {
                if (!record(p, node, rhs))
                    return (struct value){0};
                rhs.note = p->last;
            }
            struct value out = graph(p,
                                     (struct value){.type = native(p, "Unit", 0),
                                                    .binding = variable->name,
                                                    .bound = variable->binding,
                                                    .field = target.field,
                                                    .never = rhs.never},
                                     2);
            if (p->checked && out.type != NULL) {
                out.graph->output.assignment = true;
                out.graph->operands[0] = target.note;
                out.graph->operands[1] = rhs.note;
            }
            return out;
        }
        if (name->kind != MO_S_NAME && name->kind != MO_S_TUPLE && name->kind != MO_S_UNDERSCORE) {
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
        if (assignment && p->lambda != 0) {
            bool reported = problem(p, MO_CODE_LAMBDA_WRITE, name->span,
                                    "an anonymous function can't assign a var",
                                    "An anonymous function can read names in scope, but can't "
                                    "assign a var (reference 5.6).",
                                    l->name);
            (void)reported;
            return (struct value){0};
        }
        if (assignment && !capture_write(p, l, node, name))
            return (struct value){0};
        if (assignment)
            l->version++;
        struct value target = {0};
        if (assignment) {
            target = (struct value){.type = l->type, .binding = l->name, .bound = l->binding};
            if (node->kind != MO_S_BIND)
                target = graph(p, target, 0);
            if (target.type == NULL || !record(p, name, target))
                return (struct value){0};
            target.note = p->last;
        }
        struct value rhs;
        if (assignment && node->kind != MO_S_BIND) {
            l->used = true;
            rhs = binary(p, node->kind == MO_S_ADD_ASSIGN ? MO_T_PLUS : MO_T_MINUS, target,
                         node->children[1], l->type);
        } else
            rhs = infer(p, node->children[1], assignment ? l->type : NULL);
        if (rhs.type == NULL)
            return (struct value){0};
        if (p->checked && assignment && node->kind != MO_S_BIND) {
            if (!record(p, node, rhs))
                return (struct value){0};
            rhs.note = p->last;
        }
        if (!assignment &&
            !(node->kind == MO_S_VAR ? bind(p, name, rhs.type, true) : binder(p, name, rhs.type)))
            return (struct value){0};
        struct value result =
            graph(p,
                  (struct value){.type = native(p, "Unit", 0),
                                 .binding = assignment ? l->name : NULL,
                                 .bound = assignment                 ? l->binding
                                          : name->kind == MO_S_TUPLE ? NULL
                                                                     : p->last->binding,
                                 .never = rhs.never},
                  1);
        if (p->checked && result.type != NULL) {
            result.graph->operands[0] = rhs.note;
            result.graph->output.assignment = assignment;
            if (name->kind == MO_S_TUPLE)
                result.graph->pattern_type = rhs.type;
        }
        return result;
    }
    case MO_S_BINARY:
    case MO_S_RANGE: {
        bool logical =
            node->token == MO_T_AND || node->token == MO_T_OR || node->token == MO_T_IMPLIES;
        struct local *mark = p->locals;
        struct value left = logical && node->token != MO_T_OR
                                ? condition(p, node->children[0], true)
                                : infer(p, node->children[0], NULL);
        struct value result = binary(p, node->token, left, node->children[1], expected);
        if (result.type == NULL || (logical && node->token != MO_T_AND && !leave(p, mark)))
            return (struct value){0};
        return result;
    }
    case MO_S_BINARY_CHAIN: {
        enum mo_syntax_token op = node->children[1]->token;
        struct value result = op == MO_T_AND ? condition(p, node->children[0], true)
                                             : infer(p, node->children[0], NULL);
        for (uint32_t i = 1; i < node->count && result.type != NULL; i += 2) {
            result = binary(p, node->children[i]->token, result, node->children[i + 1],
                            i + 2 == node->count ? expected : NULL);
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
        return call(p, infer(p, node->children[0], NULL), node->children[1], NULL);
    }
    case MO_S_POSITION:
        return position(p, infer(p, node->children[0], NULL), node->children[1]);
    case MO_S_POSTFIX: {
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
        if (p->checked && v.type != NULL && v.note == NULL && start < node->count) {
            if (v.graph == NULL)
                v = graph(p, v, 0);
            if (v.type == NULL || !record(p, node->children[start - 1], v))
                return (struct value){0};
            v.note = p->last;
        }
        for (uint32_t i = start; i < node->count && v.type != NULL; i++) {
            const struct mo_syntax *step = node->children[i];
            if (step->kind == MO_S_POSITION)
                v = position(p, v, step->children[0]);
            else if (step->kind == MO_S_CALL)
                v = call(p, v, step->children[0], NULL);
            else if (step->kind == MO_S_FIELD_ACCESS) {
                const struct mo_syntax *site = NULL;
                if (i + 1 < node->count && node->children[i + 1]->kind == MO_S_CALL)
                    site = node->children[i + 1];
                bool called;
                v = field_value(p, v, step->children[0], step, site, &called);
                if (called && site != NULL)
                    i++;
            } else {
                bool reported = unfinished(p);
                (void)reported;
                return (struct value){0};
            }
            if (p->checked && v.type != NULL && i + 1 < node->count) {
                if (!record(p, step, v))
                    return (struct value){0};
                v.note = p->last;
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
                                              .parameters = notes,
                                              .parent = p->lambda_frame,
                                              .next = p->lambdas,
                                              .syntax = node,
                                              .loop = p->capture_loop,
                                              .path = p->capture_path,
                                              .depth = p->lambda + 1};
            p->lambdas = lambda;
            emitted.graph->lambda = lambda;
            p->lambda_count++;
        }
        struct local *mark = p->locals;
        struct term *old_result = p->result;
        struct emitted_lambda *old_frame = p->lambda_frame;
        if (p->checked)
            p->lambda_frame = emitted.graph->lambda;
        struct capture_loop *old_loop = p->capture_loop;
        p->capture_loop = NULL;
        uint32_t old_loops = p->loops;
        p->loops = 0;
        p->lambda++;
        p->result = fn->arguments[parameters->count];
        for (uint32_t i = 0; i < parameters->count; i++) {
            if (!bind(p, parameters->children[i], fn->arguments[i], false))
                return (struct value){0};
            if (p->checked)
                emitted.graph->lambda->parameters[i] = p->last->binding;
        }
        struct value body = named(p->result, "Unit")
                                ? discard(p, node->children[1])
                                : infer_as(p, node->children[1], p->result, MO_CODE_RESULT_TYPE);
        if (body.type == NULL || !leave(p, mark))
            return (struct value){0};
        p->result = old_result;
        p->lambda_frame = old_frame;
        p->capture_loop = old_loop;
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
        struct value out =
            graph(p, (struct value){.type = native(p, "Bool", 0), .never = input.never}, 1);
        if (p->checked && out.type != NULL)
            out.graph->operands[0] = input.note;
        return out;
    }
    case MO_S_RETURN: {
        struct local *mark = p->locals;
        struct value guard = {0};
        if (node->children[1] != NULL) {
            guard = condition(p, node->children[1], true);
            if (guard.type == NULL)
                return guard;
        }
        struct value value = node->children[0] == NULL
                                 ? (struct value){.type = native(p, "Unit", 0)}
                                 : infer_as(p, node->children[0], p->result, MO_CODE_RESULT_TYPE);
        if (value.type == NULL || !unify_as(p, value.type, p->result, MO_CODE_RESULT_TYPE) ||
            !leave(p, mark))
            return (struct value){0};
        uint32_t count =
            (node->children[0] == NULL ? 0u : 1u) + (node->children[1] == NULL ? 0u : 1u);
        struct value out = graph(
            p, (struct value){.type = native(p, "Unit", 0), .never = node->children[1] == NULL},
            count);
        if (p->checked && out.type != NULL) {
            if (guard.note != NULL)
                out.graph->operands[0] = guard.note;
            if (value.note != NULL)
                out.graph->operands[count - 1] = value.note;
        }
        return out;
    }
    case MO_S_STOP: {
        struct term *text = native(p, "Text", 0);
        struct value reason = text == NULL ? (struct value){0} : infer(p, node->children[0], text);
        if (reason.type == NULL)
            return (struct value){0};
        struct value out =
            graph(p,
                  (struct value){.type = expected == NULL ? native(p, "Unit", 0) : expected,
                                 .never = true},
                  1);
        if (p->checked && out.type != NULL)
            out.graph->operands[0] = reason.note;
        return out;
    }
    case MO_S_ASSERT: {
        struct value test = condition(p, node->children[0], true);
        if (test.type == NULL)
            return (struct value){0};
        struct value out = graph(p, (struct value){.type = native(p, "Unit", 0)}, 1);
        if (p->checked && out.type != NULL)
            out.graph->operands[0] = test.note;
        return out;
    }
    case MO_S_FOR:
    case MO_S_WHILE: {
        struct local *mark = p->locals;
        struct note *binding = NULL;
        struct term *element = NULL;
        uint32_t body_index = node->kind == MO_S_FOR ? 2 : 1;
        struct capture_loop *old_loop = p->capture_loop;
        struct capture_loop *loop = NULL;
        if (p->checked) {
            loop = allocate(p, 1, sizeof(*loop), _Alignof(struct capture_loop));
            if (loop == NULL)
                return (struct value){0};
            *loop = (struct capture_loop){node, old_loop};
            if (node->kind == MO_S_WHILE)
                p->capture_loop = loop;
        }
        struct value test = node->kind == MO_S_WHILE ? condition(p, node->children[0], true)
                                                     : infer(p, node->children[1], NULL);
        if (test.type == NULL)
            return (struct value){0};
        if (node->kind == MO_S_WHILE) {
            struct term *boolean = native(p, "Bool", 0);
            if (boolean == NULL || !unify_as(p, test.type, boolean, MO_CODE_CONDITION_TYPE))
                return (struct value){0};
        } else {
            struct term *container = root(test.type);
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
            const struct mo_syntax *name = node->children[0];
            while (name->kind == MO_S_BINDER)
                name = name->children[0];
            if (name->kind != MO_S_TUPLE)
                binding = p->last->binding;
        }
        p->capture_loop = loop;
        p->loops++;
        struct term *unit = native(p, "Unit", 0);
        struct value body =
            unit == NULL ? (struct value){0} : discard(p, node->children[body_index]);
        p->loops--;
        p->capture_loop = old_loop;
        if (body.type == NULL || !leave(p, mark))
            return (struct value){0};
        struct value out = graph(p, (struct value){.type = unit, .bound = binding}, 2);
        if (p->checked && out.type != NULL) {
            out.graph->operands[0] = test.note;
            out.graph->operands[1] = body.note;
            out.graph->pattern_type = element;
        }
        return out;
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
        struct value guard = {0};
        if (node->children[0] != NULL) {
            struct term *condition = native(p, "Bool", 0);
            guard = condition == NULL
                        ? (struct value){0}
                        : infer_as(p, node->children[0], condition, MO_CODE_CONDITION_TYPE);
            if (guard.type == NULL)
                return (struct value){0};
        }
        struct value out = graph(
            p, (struct value){.type = native(p, "Unit", 0), .never = node->children[0] == NULL},
            guard.type == NULL ? 0u : 1u);
        if (p->checked && guard.type != NULL && out.type != NULL)
            out.graph->operands[0] = guard.note;
        return out;
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
        struct value out = graph(p, (struct value){.type = t},
                                 node->kind == MO_S_MAP ? 2 * node->count : node->count);
        if (out.type == NULL)
            return out;
        uint32_t *versions = NULL;
        if (node->kind == MO_S_MAP && node->count > 1) {
            versions = allocate(p, node->count, sizeof(*versions), _Alignof(uint32_t));
            if (versions == NULL)
                return (struct value){0};
        }
        for (uint32_t i = 0; i < node->count; i++) {
            const struct mo_syntax *part = node->children[i];
            if (node->kind == MO_S_MAP) {
                const struct mo_syntax *key = part->children[0];
                while (key->kind == MO_S_GROUP)
                    key = key->children[0];
                struct name_slot *name = key->kind == MO_S_NAME ? slot(p, key, false) : NULL;
                if (versions != NULL)
                    versions[i] = name != NULL && name->active != NULL && name->active->variable
                                      ? name->active->version
                                      : 0;
                for (uint32_t j = 0; j < 2; j++) {
                    struct value v = infer(p, part->children[j], t->arguments[j]);
                    if (v.type == NULL)
                        return (struct value){0};
                    if (p->checked)
                        out.graph->operands[2 * i + j] = v.note;
                    out.never = out.never || v.never;
                }
            } else {
                struct value v = infer(p, part, t->arguments[node->kind == MO_S_TUPLE ? i : 0]);
                if (v.type == NULL)
                    return (struct value){0};
                if (p->checked)
                    out.graph->operands[i] = v.note;
                out.never = out.never || v.never;
            }
        }
        if (node->kind == MO_S_MAP && !map_keys(p, node, versions))
            return (struct value){0};
        return out;
    }
    case MO_S_TRY: {
        struct value input = infer(p, node->children[0], NULL);
        p->span = (struct mo_span){node->span.file, node->span.start, node->span.start + 3};
        return propagate(p, input, expected);
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
            if (op == MO_T_TRY) {
                p->span = node->children[i - 1]->span;
                v = propagate(p, v, i == 1 ? expected : NULL);
                if (v.type == NULL)
                    return v;
                if (p->checked && i != 1) {
                    if (!record(p, node->children[i - 1], v))
                        return (struct value){0};
                    v.note = p->last;
                }
                continue;
            }
            if (op == MO_T_MINUS && !p->checked && named(v.type, "Duration")) {
                bool reported = unfinished(p);
                (void)reported;
                return (struct value){0};
            }
            if (op == MO_T_MINUS && p->checked && root(v.type)->variable &&
                root(v.type)->numeric == 0 && root(v.type)->deferred) {
                struct term *result = fresh(p, 0);
                struct failure_check *pending =
                    allocate(p, 1, sizeof(*pending), _Alignof(struct failure_check));
                if (result == NULL || pending == NULL)
                    return (struct value){0};
                result->deferred = true;
                *pending = (struct failure_check){.next = p->failures,
                                                  .input = v.type,
                                                  .value = result,
                                                  .op = MO_T_MINUS,
                                                  .code = i == 1 ? p->node_expectation : 0,
                                                  .span = node->span};
                p->failures = pending;
                v.type = result;
            } else {
                struct term *wanted = op == MO_T_NOT ? native(p, "Bool", 0)
                                      : op == MO_T_MINUS
                                          ? ((p->checked && named(v.type, "Duration")) ||
                                                     root(v.type)->shape.kind == MO_SOURCE_PARAMETER
                                                 ? v.type
                                                 : fresh(p, INT | BIG | DECIMAL | FLOAT))
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
            }
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
    case MO_S_NAMED_ARG:
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
        node->kind != MO_S_LAMBDA && node->kind != MO_S_IF && node->kind != MO_S_LINE_IF &&
        node->kind != MO_S_CAPNAME && node->kind != MO_S_QUALIFIED &&
        node->kind != MO_S_FIELD_ACCESS && node->kind != MO_S_RETURN && node->kind != MO_S_STOP &&
        node->kind != MO_S_ASSERT && node->kind != MO_S_CASE && node->kind != MO_S_IS &&
        node->kind != MO_S_FOR && node->kind != MO_S_WHILE && node->kind != MO_S_BREAK &&
        node->kind != MO_S_CONTINUE && node->kind != MO_S_LIST && node->kind != MO_S_MAP &&
        node->kind != MO_S_TUPLE && node->kind != MO_S_POSITION && node->kind != MO_S_ADD_ASSIGN &&
        node->kind != MO_S_SUB_ASSIGN && node->kind != MO_S_TEXT_INTERPOLATED &&
        node->kind != MO_S_TRY) {
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
        v = graph(p, (struct value){.type = v.type, .never = v.never}, 1);
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

static MO_MUST_USE bool capture_type(struct inference *p, const struct mo_source_type *type,
                                     bool *capability, uint32_t *unknown, uint32_t depth) {
    if (!work(p) || !type_depth(p, depth))
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
    static const char *const capabilities[] = {"Args",      "Env",   "Console", "Files",
                                               "ReadFiles", "Clock", "Random"};
    for (uint32_t i = 0; i < sizeof(capabilities) / sizeof(capabilities[0]); i++)
        if (type->kind == MO_SOURCE_NAMED && type->module == UINT32_MAX &&
            strcmp(type->name, capabilities[i]) == 0)
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
        if (!work(p))
            return false;
        bool repeated = false;
        for (const struct capture_loop *loop = lambda->loop; loop != NULL; loop = loop->parent) {
            if (!work(p))
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
                    if (!work(p))
                        return false;
                    exclusive = exclusive || (a->group == b->group && a->arm != b->arm);
                }
        if (exclusive)
            continue;
        return problem(p, MO_CODE_CAPTURE_ASSIGNMENT, capture->use,
                       "an anonymous function reads a var assigned after it is made",
                       "Copy the value into a name first; later rounds of an enclosing loop "
                       "also count as later assignments (reference 5.6).",
                       w->target);
    }
    return true;
}

static MO_MUST_USE bool finish_captures(struct inference *p) {
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
                bool reported = problem(p, MO_CODE_CAPABILITY_CAPTURE, c->use,
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
    MO_ASSERT(graph->output.operand_count != 0 && graph->operands[0] != NULL);
    const struct mo_source_type *type = graph->operands[0]->output.type;
    if (ability == MO_SOURCE_NUMBER && type->kind == MO_SOURCE_NAMED &&
        type->module == UINT32_MAX &&
        (strcmp(type->name, "Time") == 0 || strcmp(type->name, "Duration") == 0))
        return true;
    p->span = note->output.syntax->span;
    const struct mo_ability_implementation *implementation =
        mo_prove_ability(&p->abilities, type, ability);
    if (implementation == NULL)
        return problem(p, p->abilities.error, p->span, p->abilities.message, p->abilities.reason,
                       NULL);
    struct mo_checked_ability_use *use =
        allocate(p, 1, sizeof(*use), _Alignof(struct mo_checked_ability_use));
    if (p->failed)
        return false;
    *use = (struct mo_checked_ability_use){.operand = graph->operands[0]->index,
                                           .implementation = implementation};
    graph->output.ability_count = 1;
    graph->output.abilities = use;
    return true;
}

static MO_MUST_USE bool finish_interpolation(struct inference *p, struct note *note) {
    if (note->output.syntax->kind != MO_S_TEXT_INTERPOLATED)
        return true;
    struct emitted *graph = note->graph;
    uint32_t count = graph->output.operand_count;
    struct mo_checked_ability_use *uses =
        allocate(p, count, sizeof(*uses), _Alignof(struct mo_checked_ability_use));
    if (uses == NULL)
        return false;
    for (uint32_t i = 0; i < count; i++) {
        struct note *hole = graph->operands[i];
        const struct mo_ability_implementation *proof =
            mo_prove_ability(&p->abilities, hole->output.type, MO_SOURCE_TEXT);
        if (proof == NULL)
            return problem(p, p->abilities.error, hole->output.syntax->span, p->abilities.message,
                           p->abilities.reason, NULL);
        uses[i] = (struct mo_checked_ability_use){.operand = hole->index, .implementation = proof};
    }
    graph->output.ability_count = count;
    graph->output.abilities = uses;
    return true;
}

static MO_MUST_USE const struct mo_source_type *parameter_type(const struct mo_source_type *type,
                                                               uint32_t bit) {
    if ((type->parameters & bit) == 0)
        return NULL;
    if (type->kind == MO_SOURCE_PARAMETER)
        return type;
    for (uint32_t i = 0; i < type->count; i++) {
        const struct mo_source_type *out = parameter_type(type->arguments[i], bit);
        if (out != NULL)
            return out;
    }
    return NULL;
}

static MO_MUST_USE bool finish_function_use(struct inference *p, struct emitted *graph) {
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
        allocate(p, count, sizeof(*arguments), _Alignof(struct mo_type_argument));
    struct mo_checked_ability_use *proofs =
        allocate(p, proof_count, sizeof(*proofs), _Alignof(struct mo_checked_ability_use));
    if (p->failed)
        return false;
    uint32_t at = 0, proof = 0;
    for (unsigned i = 0; i < 26; i++) {
        uint32_t bit = UINT32_C(1) << i;
        if ((type->parameters & bit) == 0)
            continue;
        const struct mo_source_type *actual = finish_type(p, graph->parameters[at], 0);
        if (actual == NULL)
            return false;
        arguments[at++] = (struct mo_type_argument){parameter_type(type, bit), actual};
        for (unsigned j = 0; j < 5; j++) {
            if ((graph->requirements[i] & (1u << j)) == 0)
                continue;
            const struct mo_ability_implementation *implementation =
                mo_prove_ability(&p->abilities, actual, (enum mo_source_ability)j);
            if (implementation == NULL)
                return problem(p, p->abilities.error, p->span, p->abilities.message,
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

static MO_MUST_USE bool type_arguments(struct inference *p, const struct mo_source_type *generic,
                                       const struct mo_source_type *actual,
                                       struct mo_type_arguments *out) {
    if (generic->count == 0)
        return true;
    MO_ASSERT(actual->count == generic->count);
    struct mo_type_argument *items =
        allocate(p, generic->count, sizeof(*items), _Alignof(struct mo_type_argument));
    if (items == NULL)
        return false;
    for (uint32_t i = 0; i < actual->count; i++)
        items[i] = (struct mo_type_argument){generic->arguments[i], actual->arguments[i]};
    *out = (struct mo_type_arguments){.count = actual->count, .items = items};
    return true;
}

static MO_MUST_USE bool data_requirements(struct inference *p,
                                          const struct mo_type_arguments *arguments,
                                          uint32_t *count,
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
        allocate(p, length, sizeof(*proofs), _Alignof(struct mo_checked_ability_use));
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
                return problem(p, p->abilities.error, p->span, p->abilities.message,
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
    if (!work(p))
        return NULL;
    struct mo_checked_pattern *out =
        allocate(p, 1, sizeof(*out), _Alignof(struct mo_checked_pattern));
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
        parameters[letter] = annotation(p, actual->arguments[i], NULL, 0);
        if (parameters[letter] == NULL)
            return NULL;
    }
    struct term *type = annotation(p, data->by_index[index]->type, parameters, 0);
    return type == NULL ? NULL : finish_type(p, type, 0);
}

static MO_MUST_USE bool same_failure_type(struct inference *p, const struct mo_source_type *a,
                                          const struct mo_source_type *b, uint32_t depth) {
    if (!work(p) || !type_depth(p, depth))
        return false;
    if (a == b)
        return true;
    if (a->kind != b->kind || a->module != b->module || a->declaration != b->declaration ||
        a->count != b->count || strcmp(a->name, b->name) != 0)
        return false;
    for (struct type_pair *pair = p->equal_types; pair != NULL; pair = pair->next) {
        if (!work(p))
            return false;
        if (pair->a == a && pair->b == b)
            return true;
    }
    for (uint32_t i = 0; i < a->count; i++)
        if (!same_failure_type(p, a->arguments[i], b->arguments[i], depth + 1))
            return false;
    struct type_pair *pair = allocate(p, 1, sizeof(*pair), _Alignof(struct type_pair));
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
    if (!work(p) || !type_depth(p, depth))
        return false;
    input = root(input);
    if (input->variable && input->numeric != 0 && finish_type(p, input, 0) == NULL)
        return false;
    if (input->resolved != NULL)
        return same_failure_type(p, input->resolved, type, depth);
    for (struct failure_guess *at = *guesses; at != NULL; at = at->next) {
        if (!work(p))
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
            struct term *part = argument(p, input, i);
            if (part == NULL || !failure_matches(p, part, type->arguments[i], guesses, depth + 1))
                return false;
        }
    }
    struct failure_guess *guess = allocate(p, 1, sizeof(*guess), _Alignof(struct failure_guess));
    if (guess == NULL)
        return false;
    *guess = (struct failure_guess){input, type, *guesses};
    *guesses = guess;
    return true;
}

static MO_MUST_USE bool finish_wrapping(struct inference *p) {
    for (struct failure_check *at = p->failures; at != NULL; at = at->next) {
        p->span = at->span;
        if (!work(p))
            return false;
        if (at->op != MO_T_EOF || at->fallback != NULL || at->context == NULL ||
            named(at->input, "Option"))
            continue;
        struct term *from = argument(p, at->input, 1), *to = argument(p, at->context, 1);
        if (from == NULL || to == NULL)
            return false;
        if ((root(from)->variable && root(from)->numeric == 0) || root(to)->variable) {
            if (!unify(p, from, to, 0))
                return false;
        }
        const struct mo_source_type *target = finish_type(p, to, 0);
        if (target == NULL)
            return false;
        struct failure_guess *guesses = NULL;
        if (failure_matches(p, from, target, &guesses, 0)) {
            struct term *actual = annotation(p, target, NULL, 0);
            if (actual == NULL || !unify(p, from, actual, 0))
                return false;
            continue;
        }
        if (p->failed)
            return false;
        const struct mo_source_type *selected = NULL;
        uint32_t count = 0;
        if (target->declaration != NULL && target->declaration->declaration->kind == MO_S_ENUM) {
            const struct mo_symbol *owner = target->declaration;
            if (!visible_data(p, owner))
                return false;
            const struct mo_module_scope *scope = &p->interfaces->scopes[owner->module];
            for (uint32_t i = 0; i < owner->case_count; i++) {
                if (!work(p))
                    return false;
                const struct mo_data_signature *data =
                    data_type(p, &scope->symbols[owner->cases[i]]);
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
                if (!work(p))
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
            if (!preview_type(p, &preview, from, 0))
                return false;
            char *message = allocate(p, 768, 1, 1);
            if (message == NULL)
                return false;
            int length = snprintf(message, 768,
                                  "try needs exactly one failure case holding %.511s as its "
                                  "unnamed value; found %u",
                                  preview.text, (unsigned)count);
            MO_ASSERT(length > 0 && length < 768);
            return problem(p, MO_CODE_TRY_WRAPPER, p->span, message,
                           count == 0
                               ? "Add one case Wrap(E) to the function's failure type, where E "
                                 "is the source failure type; named fields don't wrap "
                                 "(reference 8.3)."
                               : "Keep exactly one case with this unnamed value; make the "
                                 "other matching cases use named fields, or handle the failure "
                                 "with case (reference 8.3).",
                           NULL);
        }
        MO_ASSERT(selected != NULL);
        struct term *actual = annotation(p, selected, NULL, 0);
        if (actual == NULL || !unify(p, from, actual, 0))
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
        if (!work(p))
            return NULL;
        if (at->data == data && at->type == type)
            return at->fields;
    }
    struct pattern_defaults *out = allocate(p, 1, sizeof(*out), _Alignof(struct pattern_defaults));
    const struct mo_checked_pattern **fields =
        allocate(p, data->count, sizeof(*fields), _Alignof(const struct mo_checked_pattern *));
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
static MO_MUST_USE struct mo_checked_pattern *
checked_pattern(struct inference *p, const struct mo_syntax *syntax,
                const struct mo_source_type *type, struct note *const *notes, uint32_t note_count,
                const struct mo_checked_binding *bindings, uint32_t binding_count, uint32_t depth) {
    if (!work(p) || !type_depth(p, depth))
        return NULL;
    if (syntax->kind == MO_S_GROUP || syntax->kind == MO_S_BINDER)
        return checked_pattern(p, syntax->children[0], type, notes, note_count, bindings,
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
        struct mo_checked_pattern_part *parts =
            allocate(p, syntax->count, sizeof(*parts), _Alignof(struct mo_checked_pattern_part));
        if (p->failed)
            return NULL;
        MO_ASSERT(type->count == syntax->count);
        out->parts = parts;
        for (uint32_t i = 0; i < syntax->count; i++) {
            parts[i] = (struct mo_checked_pattern_part){
                i, checked_pattern(p, syntax->children[i], type->arguments[i], notes, note_count,
                                   bindings, binding_count, depth + 1)};
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
            allocate(p, length, sizeof(*heads), _Alignof(const struct mo_checked_pattern *));
        if (heads == NULL)
            return NULL;
        for (uint32_t i = 0; i < length; i++) {
            heads[i] = checked_pattern(p, syntax->children[i], type->arguments[0], notes,
                                       note_count, bindings, binding_count, depth + 1);
            if (heads[i] == NULL)
                return NULL;
        }
        struct mo_checked_pattern *tail =
            rest ? checked_pattern(p, syntax->children[length]->children[0], type, notes,
                                   note_count, bindings, binding_count, depth + 1)
                 : pattern_output(p, syntax, type, MO_PATTERN_LIST, 0);
        for (uint32_t i = length; tail != NULL && i != 0; i--) {
            struct mo_checked_pattern *cons = pattern_output(p, syntax, type, MO_PATTERN_LIST, 2);
            struct mo_checked_pattern_part *parts =
                allocate(p, 2, sizeof(*parts), _Alignof(struct mo_checked_pattern_part));
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
        struct mo_checked_pattern_part *parts =
            allocate(p, data->count, sizeof(*parts), _Alignof(struct mo_checked_pattern_part));
        if (p->failed)
            return NULL;
        out->constructor.builtin = data->name;
        out->parts = parts;
        const struct mo_source_type *generic = mo_builtin_template(data);
        if (generic != NULL && !type_arguments(p, generic, type, &out->arguments))
            return NULL;
        struct term *nominal = annotation(p, type, NULL, 0);
        if (nominal == NULL)
            return NULL;
        for (uint32_t i = 0; i < data->count; i++) {
            struct term *part = builtin_part(p, data, nominal, i);
            const struct mo_source_type *field = part == NULL ? NULL : finish_type(p, part, 0);
            if (field == NULL)
                return NULL;
            parts[i] = (struct mo_checked_pattern_part){
                i, pattern_output(p, syntax, field, MO_PATTERN_ANY, 0)};
            if (parts[i].pattern == NULL)
                return NULL;
        }
        const struct mo_syntax *args = syntax->kind == MO_S_CALL ? syntax->children[1] : NULL;
        for (uint32_t i = 0; args != NULL && i < args->count; i++) {
            uint32_t index = builtin_field(data, args->children[i]);
            MO_ASSERT(index < data->count);
            const struct mo_syntax *value =
                data->fields[0] == NULL ? args->children[i] : args->children[i]->children[1];
            parts[index].pattern = checked_pattern(p, value, parts[index].pattern->type, notes,
                                                   note_count, bindings, binding_count, depth + 1);
            if (parts[index].pattern == NULL)
                return NULL;
        }
        return out;
    }
    MO_ASSERT(note->output.symbol != NULL);
    const struct mo_data_signature *data = data_type(p, note->output.symbol);
    MO_ASSERT(data != NULL);
    struct mo_checked_pattern *out =
        pattern_output(p, syntax, type, MO_PATTERN_CONSTRUCTOR, data->count);
    struct mo_checked_pattern_part *parts =
        allocate(p, data->count, sizeof(*parts), _Alignof(struct mo_checked_pattern_part));
    if (p->failed)
        return NULL;
    out->constructor.symbol = data->symbol;
    out->parts = parts;
    if (!type_arguments(p, data->type, type, &out->arguments) ||
        !data_requirements(p, &out->arguments, &out->ability_count, &out->abilities))
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
        parts[index].pattern = checked_pattern(p, value, parts[index].pattern->type, notes,
                                               note_count, bindings, binding_count, depth + 1);
        if (parts[index].pattern == NULL)
            return NULL;
    }
    return out;
}

/* The receiver was walked in its live lexical scope. Insert the selected
 * function or callable-field projection after it, before the saved arguments. */
static MO_MUST_USE struct note *dispatch_callee(struct inference *p, struct dispatch *at,
                                                struct value selected, bool method) {
    struct note *last = p->last;
    if (!record(p, method ? at->name : at->syntax, selected))
        return NULL;
    struct note *callee = p->last;
    last->next = NULL;
    p->last = last;
    callee->next = at->receiver.note->next;
    at->receiver.note->next = callee;
    return callee;
}

static MO_MUST_USE bool finish_dot(struct inference *p, struct dispatch *at) {
    bool method;
    struct value selected = member(p, at->receiver, at->name, &method);
    if (selected.type == NULL)
        return false;
    if (method && at->arguments != NULL && at->count == 0 && receiver_only(selected.type))
        return empty_dot_call(p, at->call);
    if (!method && at->arguments == NULL) {
        if (!unify(p, at->type, selected.type, 0))
            return false;
        at->note->output.field = selected.field;
        if (p->checked)
            *at->graph = *selected.graph;
        return true;
    }
    struct term *fn = root(selected.type);
    if (fn->shape.kind != MO_SOURCE_FUNCTION) {
        struct term function = {.shape = {.kind = MO_SOURCE_FUNCTION, .name = "fn"}};
        return unify_as(p, fn, &function, MO_CODE_NOT_FUNCTION);
    }
    uint32_t offset = method ? 1u : 0u;
    if (fn->shape.count != at->count + offset + 1)
        return problem(p, MO_CODE_CALL_ARGUMENTS, p->span,
                       "a call's arguments don't match its parameters",
                       "Pass every argument by position (reference 5.2).", NULL);
    if (method && !unify_as(p, at->receiver.type, argument(p, fn, 0), MO_CODE_ARGUMENT_TYPE))
        return false;
    for (uint32_t i = 0; i < at->count; i++) {
        p->span = at->arguments->children[i]->span;
        if (!at->inputs[i].never &&
            !unify_as(p, at->inputs[i].type, argument(p, fn, i + offset), MO_CODE_ARGUMENT_TYPE))
            return false;
    }
    p->span = at->name->span;
    if (!unify(p, at->type, argument(p, fn, fn->shape.count - 1), 0))
        return false;
    if (!p->checked)
        return true;
    if (!method && at->note->output.syntax == at->syntax) {
        MO_ASSERT(at->call != NULL);
        at->note->output.syntax = at->call;
    }
    bool callee = !method || selected.symbol != NULL;
    struct note *function = callee ? dispatch_callee(p, at, selected, method) : NULL;
    if (callee && function == NULL)
        return false;
    uint32_t receiver = method && callee ? 1u : 0u;
    struct note **operands =
        allocate(p, at->count + 1 + receiver, sizeof(*operands), _Alignof(struct note *));
    if (operands == NULL)
        return false;
    operands[0] = callee ? function : at->receiver.note;
    if (receiver != 0)
        operands[1] = at->receiver.note;
    for (uint32_t i = 0; i < at->count; i++)
        operands[i + 1 + receiver] = at->inputs[i].note;
    *at->graph =
        (struct emitted){.output = {.operand_count = at->count + 1 + receiver,
                                    .symbol = selected.symbol,
                                    .native = method ? selected.graph->output.native : NULL,
                                    .field = UINT32_MAX,
                                    .receiver = method ? receiver : UINT32_MAX},
                         .operands = operands,
                         .function_type = method ? selected.graph->function_type : NULL,
                         .requirements = method ? selected.graph->requirements : NULL,
                         .parameters = method ? selected.graph->parameters : NULL};
    at->note->callee = method ? function : NULL;
    at->note->output.symbol = selected.symbol;
    return true;
}

static MO_MUST_USE bool finish_position(struct inference *p, struct dispatch *at) {
    uint32_t part;
    struct term *type = tuple_part(p, root(at->receiver.type), at->name, &part);
    if (type == NULL || !unify(p, at->type, type, 0))
        return false;
    if (!p->checked)
        return true;
    struct note **operands = allocate(p, 1, sizeof(*operands), _Alignof(struct note *));
    if (operands == NULL)
        return false;
    operands[0] = at->receiver.note;
    *at->graph =
        (struct emitted){.output = {.operand_count = 1, .field = part, .receiver = UINT32_MAX},
                         .operands = operands};
    return true;
}

static MO_MUST_USE struct term *failure_container(struct inference *p, bool option) {
    struct term *out = native(p, option ? "Option" : "Result", option ? 1u : 2u);
    if (out == NULL)
        return NULL;
    for (uint32_t i = 0; i < out->shape.count; i++) {
        out->arguments[i] = fresh(p, 0);
        if (out->arguments[i] == NULL)
            return NULL;
    }
    return out;
}

static MO_MUST_USE bool solve_failure(struct inference *p, struct failure_check *at,
                                      bool defaults) {
    p->span = at->span;
    struct term *input = root(at->input);
    if (at->op != MO_T_EOF) {
        if (input->variable && defaults && input->numeric != 0 && finish_type(p, input, 0) == NULL)
            return false;
        input = root(at->input);
        struct term *right = at->fallback == NULL ? NULL : root(at->fallback);
        if (at->fallback == NULL) {
            if (input->variable)
                return true;
            if (!named(input, "Duration")) {
                struct term *numeric = input->shape.kind == MO_SOURCE_PARAMETER
                                           ? input
                                           : fresh(p, INT | BIG | DECIMAL | FLOAT);
                if (numeric == NULL || !unify_as(p, input, numeric,
                                                 named(input, "Byte") ? MO_CODE_BYTE_ARITHMETIC
                                                                      : MO_CODE_NUMBER_ABILITY))
                    return false;
            }
            if (!unify_as(p, input, at->value, at->code))
                return false;
            at->solved = true;
            return true;
        }
        if (named(input, "Time") && (at->op == MO_T_PLUS || at->op == MO_T_MINUS)) {
            struct term *duration = native(p, "Duration", 0);
            if (duration == NULL)
                return false;
            if (at->op == MO_T_PLUS) {
                if (!unify(p, at->fallback, duration, 0) ||
                    !unify_as(p, input, at->value, at->code))
                    return false;
                at->solved = true;
                return true;
            }
            if (right->variable && right->numeric == 0)
                return true;
            bool diff = named(right, "Time");
            if (!unify(p, at->fallback, diff ? input : duration, 0) ||
                !unify_as(p, diff ? duration : input, at->value, at->code))
                return false;
            at->solved = true;
            return true;
        }
        if (named(input, "Duration") &&
            (at->op == MO_T_PLUS || at->op == MO_T_MINUS || at->op == MO_T_STAR)) {
            struct term *wanted = at->op == MO_T_STAR ? native(p, "Int", 0) : input;
            if (wanted == NULL || !unify(p, at->fallback, wanted, 0) ||
                !unify_as(p, input, at->value, at->code))
                return false;
            at->solved = true;
            return true;
        }
        if (input->variable)
            return true;
        struct term *numeric = input->shape.kind == MO_SOURCE_PARAMETER
                                   ? input
                                   : fresh(p, INT | BIG | DECIMAL | FLOAT);
        if (numeric == NULL ||
            !unify_as(p, input, numeric,
                      named(input, "Byte") ? MO_CODE_BYTE_ARITHMETIC : MO_CODE_NUMBER_ABILITY) ||
            !unify(p, at->fallback, input, 0) || !unify_as(p, input, at->value, at->code))
            return false;
        at->solved = true;
        return true;
    }
    if (at->fallback != NULL) {
        if (input->variable && defaults && input->numeric != 0 && finish_type(p, input, 0) == NULL)
            return false;
        if (root(input)->variable)
            return true;
        bool option = named(input, "Option");
        struct term *value = option ? argument(p, input, 0) : native(p, "Bool", 0);
        if (value == NULL || (!option && !unify_as(p, input, value, MO_CODE_CONDITION_TYPE)) ||
            !unify(p, at->value, value, 0) || (!at->never && !unify(p, at->fallback, value, 0)))
            return false;
        if (at->graph != NULL)
            at->graph->output.or_kind = option ? MO_CHECK_OR_OPTION : MO_CHECK_OR_BOOL;
        at->solved = true;
        return true;
    }
    if (input->variable && input->numeric != 0) {
        if (!defaults)
            return true;
        if (finish_type(p, input, 0) == NULL)
            return false;
    }
    struct term *context = root(at->context);
    bool option = named(input, "Option"), result = named(input, "Result");
    if (at->never && (named(context, "Option") || named(context, "Result")) && !option && !result) {
        input = failure_container(p, named(context, "Option"));
        if (input == NULL)
            return false;
        at->input = input;
        if (at->input_note != NULL)
            at->input_note->type = input;
        option = named(input, "Option");
        result = !option;
    }
    if (context->variable && (option || result)) {
        struct term *wanted = failure_container(p, option);
        if (wanted == NULL || !unify(p, context, wanted, 0))
            return false;
        context = root(context);
    }
    if (input->variable && (named(context, "Option") || named(context, "Result"))) {
        struct term *wanted = failure_container(p, named(context, "Option"));
        if (wanted == NULL || !unify(p, input, wanted, 0))
            return false;
        input = root(input);
        option = named(input, "Option");
        result = !option;
    }
    if (input->variable || context->variable)
        return true;
    if (!option && !result)
        return problem(p, MO_CODE_TRY_INPUT, p->span, "try needs a Result or an Option",
                       "try takes the value out of an Ok or Some (reference 8.3).", NULL);
    if (!named(context, option ? "Option" : "Result"))
        return problem(p, MO_CODE_TRY_RESULT, p->span,
                       "try needs a function returning the same failure kind",
                       "try works on a Result in a function returning Result, or on an "
                       "Option in a function returning Option; otherwise handle the "
                       "failure with case (reference 8.3).",
                       NULL);
    struct term *value = argument(p, input, 0);
    if (value == NULL || !unify(p, at->value, value, 0))
        return false;
    at->solved = true;
    return true;
}

static MO_MUST_USE bool finish_dispatches(struct inference *p) {
    bool progress;
    bool defaults = false;
    do {
        progress = false;
        for (struct dispatch *at = p->dispatches; at != NULL; at = at->next) {
            if (!work(p))
                return false;
            if (at->solved)
                continue;
            p->span = at->name->span;
            if (at->group == NULL) {
                struct term *receiver = root(at->receiver.type);
                if (receiver->variable && defaults && receiver->numeric != 0 &&
                    finish_type(p, receiver, 0) == NULL)
                    return false;
                if (receiver->variable)
                    continue;
                if (at->position ? !finish_position(p, at) : !finish_dot(p, at))
                    return false;
                at->solved = true;
                progress = true;
                continue;
            }
            struct term *type = root(at->type);
            if (type->variable)
                continue;
            if (type->shape.kind != MO_SOURCE_FUNCTION || type->shape.count < 2)
                return problem(p, MO_CODE_CALL_ARGUMENTS, p->span,
                               "an overloaded function needs a first argument",
                               "Overloads differ in their first parameter's type (reference 5.1).",
                               NULL);
            struct term *receiver = argument(p, type, 0);
            if (receiver == NULL)
                return false;
            if (root(receiver)->variable && root(receiver)->numeric == 0)
                continue;
            const struct mo_symbol *symbol = select_overload(p, at->group, at->count, receiver);
            if (symbol == NULL) {
                if (!p->failed)
                    return problem(p, MO_CODE_ARGUMENT_TYPE, p->span,
                                   "no overload takes this first argument's type",
                                   "Overloads take local record or choice types (reference 5.1).",
                                   NULL);
                return false;
            }
            struct value selected = declared_function(p, symbol);
            if (selected.type == NULL || !unify_as(p, selected.type, type, MO_CODE_ARGUMENT_TYPE))
                return false;
            at->note->output.symbol = symbol;
            if (p->checked) {
                *at->graph = *selected.graph;
                at->graph->output.symbol = symbol;
            }
            at->solved = true;
            progress = true;
        }
        for (struct failure_check *at = p->failures; at != NULL; at = at->next) {
            if (!work(p))
                return false;
            if (!at->solved) {
                if (!solve_failure(p, at, defaults))
                    return false;
                progress = progress || at->solved;
            }
        }
        if (!progress && !defaults) {
            defaults = true;
            progress = true;
        } else if (!progress) {
            for (struct failure_check *at = p->failures; at != NULL; at = at->next) {
                if (at->solved || at->op == MO_T_EOF)
                    continue;
                p->span = at->span;
                if (at->op == MO_T_MINUS && named(at->input, "Time")) {
                    struct term *value = root(at->value);
                    if (!named(value, "Duration") && !named(value, "Time"))
                        continue;
                    struct term *duration = native(p, "Duration", 0);
                    if (duration == NULL)
                        return false;
                    bool diff = named(value, "Duration");
                    if (!unify(p, at->fallback, diff ? at->input : duration, 0) ||
                        !unify_as(p, diff ? duration : at->input, at->value, at->code))
                        return false;
                    at->solved = true;
                    progress = true;
                    continue;
                }
                if (!root(at->input)->variable)
                    continue;
                struct term *numeric = fresh(p, INT | BIG | DECIMAL | FLOAT);
                if (numeric == NULL || !unify_as(p, at->input, numeric, MO_CODE_NUMBER_ABILITY) ||
                    (at->fallback != NULL && !unify(p, at->fallback, at->input, 0)) ||
                    !unify_as(p, at->input, at->value, at->code))
                    return false;
                at->solved = true;
                progress = true;
            }
        }
    } while (progress);
    for (struct dispatch *at = p->dispatches; at != NULL; at = at->next)
        if (!at->solved)
            return at->position
                       ? problem(p, MO_CODE_CANNOT_INFER, at->name->span,
                                 "no use decides this expression's type",
                                 "A function's signature gives its types; inside a body types "
                                 "are inferred from all their uses (reference 5.1).",
                                 NULL)
                       : problem(p, MO_CODE_CANNOT_INFER, at->name->span,
                                 "no use decides this dispatch's receiver type",
                                 "Give the value a type, or write the call another way "
                                 "(reference 5.4).",
                                 NULL);
    for (struct failure_check *at = p->failures; at != NULL; at = at->next)
        if (!at->solved)
            return problem(p, MO_CODE_CANNOT_INFER, at->span,
                           at->op != MO_T_EOF ? "no use decides this expression's type"
                                              : "no use decides this failure expression's type",
                           at->op != MO_T_EOF
                               ? "A function's signature gives its types; inside a body types "
                                 "are inferred from all their uses (reference 5.1)."
                               : "or needs Bool or Option, and try needs Option or Result in the "
                                 "enclosing function (reference 8).",
                           NULL);
    return true;
}

static MO_MUST_USE bool anchor_inputs(struct inference *p, const struct mo_syntax *anchor,
                                      const struct mo_syntax *parameters,
                                      const struct mo_syntax **inputs) {
    const struct mo_syntax *values = anchor->children[0];
    uint32_t count = parameters->count + 1;
    memset(inputs, 0, (size_t)count * sizeof(*inputs));
    for (uint32_t i = 0; i < values->count; i++) {
        const struct mo_syntax *item = values->children[i];
        const struct mo_syntax *name = item->children[0];
        uint32_t at = parameters->count;
        if (compare_text(name, "result") != 0) {
            for (at = 0; at < parameters->count; at++) {
                if (!work(p))
                    return false;
                if (compare_name(name, parameters->children[at]->children[0]) == 0)
                    break;
            }
            if (at == parameters->count)
                return problem(p, MO_CODE_ANCHOR_ARGUMENTS, name->span,
                               "an anchor names a parameter or result",
                               "Anchors give every parameter and result a value (reference 12.2).",
                               NULL);
        }
        if (inputs[at] != NULL)
            return problem(p, MO_CODE_ANCHOR_ARGUMENTS, name->span,
                           "an anchor gives a name more than one value",
                           "Anchors give every parameter and result one value (reference 12.2).",
                           inputs[at]->children[0]);
        inputs[at] = item;
    }
    for (uint32_t i = 0; i < count; i++)
        if (inputs[i] == NULL)
            return problem(p, MO_CODE_ANCHOR_ARGUMENTS, anchor->span,
                           "an anchor gives every parameter and result a value",
                           "No parameter or result can be omitted (reference 12.2).", NULL);
    return true;
}

static MO_MUST_USE struct mo_inferred_body *
infer_function(struct mo_arena *arena, const struct mo_module_interfaces *interfaces,
               const struct mo_signatures *signatures, struct mo_constant_context *constants,
               const struct mo_function_signature *signature, struct mo_diags *diags,
               struct mo_checked_function *checked, bool *halt, struct mo_checked_spec *spec,
               struct mo_checked_condition *conditions) {
    MO_ASSERT(arena != NULL && interfaces != NULL && signatures != NULL && diags != NULL);
    MO_ASSERT(halt != NULL && signature != NULL && signatures->work != NULL);
    MO_ASSERT(*signatures->work <= MO_LIMIT_PROGRAM_CHECK_WORK);
    *halt = false;
    const struct mo_syntax *syntax = signature->function->declaration;
    const struct mo_syntax *written = spec == NULL ? syntax : spec->syntax;
    bool promise = spec != NULL && spec->kind == MO_SPEC_PROMISE;
    bool anchor = spec != NULL && spec->kind == MO_SPEC_ANCHOR;
    bool test = spec != NULL && spec->kind == MO_SPEC_TEST;
    bool constant = syntax->kind == MO_S_CONST;
    struct inference p = {.arena = arena,
                          .diags = diags,
                          .interfaces = interfaces,
                          .signatures = signatures,
                          .function = signature,
                          .constants = constants,
                          .checked = checked != NULL,
                          .spec = spec != NULL,
                          .span = written->span};
    p.abilities = (struct mo_ability_context){
        .arena = arena, .interfaces = interfaces, .signatures = signatures, .work = &p.work};
    bool finalized = false;
    if (checked == NULL && syntax->children[4]->count != 0) {
        p.span = syntax->children[4]->children[0]->span;
        bool reported = unfinished(&p);
        (void)reported;
        goto failed;
    }
    struct value instance = anchor
                                ? function_value(&p, signature->type, signature->requirements)
                                : (struct value){.type = annotation(&p, signature->type, NULL, 0)};
    struct term *fn = instance.type;
    if (fn == NULL)
        goto failed;
    p.result = constant       ? fresh(&p, 0)
               : spec == NULL ? argument(&p, fn, fn->shape.count - 1)
                              : native(&p, test ? "Unit" : "Bool", 0);
    if (p.result == NULL)
        goto failed;
    const struct mo_syntax *parameters = test || constant ? NULL : syntax->children[1];
    uint32_t parameter_count = constant       ? 0
                               : spec == NULL ? parameters->count
                               : promise      ? parameters->count + 1
                                              : 0;
    const struct mo_syntax **inputs = NULL;
    struct note **spec_notes = NULL;
    if (promise || anchor) {
        inputs = allocate(&p, fn->shape.count, sizeof(*inputs), _Alignof(const struct mo_syntax *));
        if (inputs == NULL ||
            !anchor_inputs(&p, anchor ? written : written->children[2]->children[0], parameters,
                           inputs))
            goto failed;
        uint32_t count = anchor ? fn->shape.count : written->children[1]->count;
        spec_notes = allocate(&p, count, sizeof(*spec_notes), _Alignof(struct note *));
        if (spec_notes == NULL)
            goto failed;
    }
    struct note **parameter_notes =
        checked == NULL
            ? NULL
            : allocate(&p, parameter_count, sizeof(*parameter_notes), _Alignof(struct note *));
    if (checked != NULL && parameter_notes == NULL)
        goto failed;
    for (uint32_t i = 0; i < parameter_count; i++) {
        struct term *type = argument(&p, fn, i);
        const struct mo_syntax *name =
            i < parameters->count ? parameters->children[i]->children[0] : inputs[i]->children[0];
        if (type == NULL || !bind(&p, name, type, false))
            goto failed;
        if (promise && p.locals != NULL)
            p.locals->used = true; /* A promise need not mention every subject parameter. */
        if (checked != NULL)
            parameter_notes[i] = p.last;
    }
    struct value v = {0};
    if (anchor) {
        const struct mo_syntax *values = written->children[0];
        for (uint32_t i = 0; i < values->count; i++) {
            uint32_t slot = 0;
            while (inputs[slot] != values->children[i]) {
                if (!work(&p))
                    goto failed;
                slot++;
                MO_ASSERT(slot < fn->shape.count);
            }
            v = infer_as(&p, inputs[slot]->children[1], argument(&p, fn, slot),
                         MO_CODE_ARGUMENT_TYPE);
            if (v.type == NULL)
                goto failed;
            spec_notes[slot] = v.note;
        }
    } else if (promise) {
        const struct mo_syntax *lines = written->children[1];
        for (uint32_t i = 0; i < lines->count; i++) {
            struct value line = infer_as(&p, lines->children[i], p.result, MO_CODE_CONDITION_TYPE);
            if (line.type == NULL)
                goto failed;
            spec_notes[i] = line.note;
            if (i == 0)
                v = line;
            else {
                struct value both =
                    graph(&p, (struct value){.type = p.result, .never = v.never}, 2);
                if (both.type == NULL)
                    goto failed;
                both.graph->output.operation = MO_T_AND;
                both.graph->operands[0] = v.note;
                both.graph->operands[1] = line.note;
                if (!record(&p, written, both))
                    goto failed;
                both.note = p.last;
                v = both;
            }
        }
    } else if (spec != NULL)
        v = test ? discard(&p, written->children[1])
                 : infer_as(&p, written, p.result, MO_CODE_CONDITION_TYPE);
    else if (constant)
        v = infer_as(&p, syntax->children[1], p.result, MO_CODE_RESULT_TYPE);
    else
        v = named(p.result, "Unit")
                ? discard(&p, syntax->children[5])
                : infer_as(&p, syntax->children[5], p.result, MO_CODE_RESULT_TYPE);
    if (v.type == NULL || !leave(&p, NULL))
        goto failed;
    if (!finish_dispatches(&p) || !finish_wrapping(&p))
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
        *halt = true;
        mo_diags_sort(diags);
        return NULL;
    }
    if (!finish_captures(&p))
        goto failed;
    if (p.error.code != 0)
        goto failed;
    if (checked != NULL) {
        struct mo_checked_expression *rows =
            allocate(&p, p.count, sizeof(*rows), _Alignof(struct mo_checked_expression));
        struct mo_checked_binding *bindings =
            allocate(&p, p.binding_count, sizeof(*bindings), _Alignof(struct mo_checked_binding));
        const struct mo_checked_binding **parameter_bindings =
            allocate(&p, parameter_count, sizeof(*parameter_bindings),
                     _Alignof(const struct mo_checked_binding *));
        struct note **lambdas =
            allocate(&p, p.lambda_count, sizeof(*lambdas), _Alignof(struct note *));
        struct note **notes = allocate(&p, p.count, sizeof(*notes), _Alignof(struct note *));
        if (p.failed)
            goto failed;
        uint32_t count = 0, binding_count = 0, lambda_count = 0, note_count = 0;
        for (struct note *n = p.notes; n != NULL; n = n->next) {
            notes[note_count++] = n;
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
        MO_ASSERT(note_count == p.count);
        qsort(notes, note_count, sizeof(*notes), syntax_note_order);
        qsort(lambdas, lambda_count, sizeof(*lambdas), source_note_order);
        for (uint32_t i = 0; i < lambda_count; i++)
            lambdas[i]->graph->lambda->output.number = i + 1;
        for (uint32_t i = 0; i < parameter_count; i++) {
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
            p.span = n->output.syntax->span;
            if (!mo_check_type_abilities(&p.abilities, n->output.type)) {
                bool reported = problem(&p, p.abilities.error, p.span, p.abilities.message,
                                        p.abilities.reason, NULL);
                (void)reported;
                goto failed;
            }
            if (!finish_operator(&p, n) || !finish_function_use(&p, g) ||
                !finish_interpolation(&p, n))
                goto failed;
            if (g->try_result != NULL) {
                struct mo_checked_try *try_ =
                    allocate(&p, 1, sizeof(*try_), _Alignof(struct mo_checked_try));
                if (try_ == NULL)
                    goto failed;
                const struct mo_source_type *result = finish_type(&p, g->try_result, 0);
                if (result == NULL)
                    goto failed;
                *try_ = (struct mo_checked_try){
                    .result = result,
                    .failure = {.builtin = named(g->try_result, "Option") ? "None" : "Error"},
                    .wrapper = g->failure->wrapper};
                if (g->failure->generic != NULL &&
                    (!type_arguments(&p, g->failure->generic, result->arguments[1],
                                     &try_->arguments) ||
                     !data_requirements(&p, &try_->arguments, &try_->ability_count,
                                        &try_->abilities)))
                    goto failed;
                g->output.try_ = try_;
            }
            if (g->generic != NULL && g->generic->count != 0) {
                const struct mo_source_type *actual = finish_type(&p, g->instantiated, 0);
                if (actual == NULL ||
                    !type_arguments(&p, g->generic, actual, &g->output.arguments) ||
                    !data_requirements(&p, &g->output.arguments, &g->output.ability_count,
                                       &g->output.abilities))
                    goto failed;
            }
            if (g->match != NULL) {
                struct emitted_match *match = g->match;
                match->output.scrutinee = operands[0];
                for (uint32_t i = 0; i < match->output.arm_count; i++) {
                    struct mo_checked_arm *arm = &match->arms[i];
                    if (arm->guard != UINT32_MAX)
                        arm->guard = operands[arm->guard];
                    arm->body = operands[arm->body];
                    const struct mo_checked_pattern **patterns =
                        allocate(&p, arm->pattern_count, sizeof(*patterns),
                                 _Alignof(const struct mo_checked_pattern *));
                    if (patterns == NULL)
                        goto failed;
                    arm->patterns = patterns;
                    for (uint32_t j = 0; j < arm->pattern_count; j++) {
                        patterns[j] = checked_pattern(&p, arm->syntax->children[0]->children[j],
                                                      g->operands[0]->output.type, notes,
                                                      note_count, bindings, binding_count, 0);
                        if (patterns[j] == NULL)
                            goto failed;
                    }
                }
                g->output.match = &match->output;
            } else if (n->output.syntax->kind == MO_S_IS) {
                g->output.pattern =
                    checked_pattern(&p, n->output.syntax->children[1], g->operands[0]->output.type,
                                    notes, note_count, bindings, binding_count, 0);
                if (g->output.pattern == NULL)
                    goto failed;
            }
            if (g->pattern_type != NULL) {
                const struct mo_source_type *type = finish_type(&p, g->pattern_type, 0);
                if (type == NULL)
                    goto failed;
                g->output.pattern = checked_pattern(&p, n->output.syntax->children[0], type, notes,
                                                    note_count, bindings, binding_count, 0);
                if (g->output.pattern == NULL)
                    goto failed;
            }
            if (g->lambda != NULL) {
                struct emitted_lambda *lambda = g->lambda;
                const struct mo_checked_binding **bound =
                    allocate(&p, lambda->output.parameter_count, sizeof(*bound),
                             _Alignof(const struct mo_checked_binding *));
                if (bound == NULL)
                    goto failed;
                for (uint32_t i = 0; i < lambda->output.parameter_count; i++) {
                    const struct note *parameter = lambda->parameters[i];
                    MO_ASSERT(parameter == NULL || parameter->binding_index < binding_count);
                    bound[i] = parameter == NULL ? NULL : &bindings[parameter->binding_index];
                }
                lambda->output.parameters = bound;
                const struct mo_checked_binding **captures =
                    allocate(&p, lambda->output.capture_count, sizeof(*captures),
                             _Alignof(const struct mo_checked_binding *));
                if (captures == NULL)
                    goto failed;
                uint32_t at = 0;
                for (const struct capture *c = lambda->captures; c != NULL; c = c->next) {
                    MO_ASSERT(c->local->binding->binding_index < binding_count);
                    captures[at++] = &bindings[c->local->binding->binding_index];
                }
                MO_ASSERT(at == lambda->output.capture_count);
                lambda->output.captures = captures;
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
            if (n->binding != NULL) {
                MO_ASSERT(n->binding->binding_index < binding_count);
                rows[n->index].binding = &bindings[n->binding->binding_index];
            }
        }
        if (promise) {
            MO_ASSERT(conditions != NULL);
            for (uint32_t i = 0; i < spec->condition_count; i++)
                conditions[i].expression = spec_notes[i]->index;
        }
        if (anchor) {
            uint32_t *values = allocate(&p, fn->shape.count, sizeof(*values), _Alignof(uint32_t));
            if (values == NULL || finish_type(&p, fn, 0) == NULL ||
                !finish_function_use(&p, instance.graph))
                goto failed;
            for (uint32_t i = 0; i < fn->shape.count; i++)
                values[i] = spec_notes[i]->index;
            spec->value_count = fn->shape.count;
            spec->values = values;
            spec->arguments = instance.graph->output.arguments;
            spec->ability_count = instance.graph->output.ability_count;
            spec->abilities = instance.graph->output.abilities;
        }
        *checked =
            (struct mo_checked_function){.signature = signature,
                                         .parameter_count = parameter_count,
                                         .parameters = parameter_bindings,
                                         .body = {.count = count,
                                                  .items = rows,
                                                  .root = anchor ? UINT32_MAX : v.note->index,
                                                  .capture_free_parameters = p.capture_free,
                                                  .binding_count = binding_count,
                                                  .bindings = bindings}};
    }
    return out;
failed:
    if (constants != NULL && constants->pending != NULL)
        return NULL; /* The explicit constant driver resumes after its dependency. */
    if (!p.fatal && !finalized) {
        p.failed = false;
        p.partial = true;
        bool partial_valid = finish_notes(&p, NULL);
        (void)partial_valid;
        if (!p.fatal && !finish_patterns(&p)) {
            *halt = true;
            mo_diags_sort(diags);
            return NULL;
        }
        if (!p.fatal) {
            bool captures_valid = finish_captures(&p);
            (void)captures_valid;
        }
    }
    emit_problem(&p);
    *halt = p.error.code >= 1000 || (diags->dropped != 0 && diags->count < MO_LIMIT_DIAGNOSTICS);
    mo_diags_sort(diags);
    return NULL;
}

struct mo_inferred_body *mo_infer_function(struct mo_arena *arena,
                                           const struct mo_module_interfaces *interfaces,
                                           const struct mo_signatures *signatures,
                                           uint32_t function, struct mo_diags *diags) {
    bool halt;
    MO_ASSERT(function < signatures->count);
    return infer_function(arena, interfaces, signatures, NULL, &signatures->items[function], diags,
                          NULL, &halt, NULL, NULL);
}

bool mo_check_function(struct mo_arena *arena, const struct mo_module_interfaces *interfaces,
                       const struct mo_signatures *signatures,
                       struct mo_constant_context *constants, uint32_t function,
                       struct mo_diags *diags, struct mo_checked_function *out, bool *halt) {
    MO_ASSERT(out != NULL && function < signatures->count);
    return infer_function(arena, interfaces, signatures, constants, &signatures->items[function],
                          diags, out, halt, NULL, NULL) != NULL;
}

bool mo_check_spec_body(struct mo_arena *arena, const struct mo_module_interfaces *interfaces,
                        const struct mo_signatures *signatures,
                        struct mo_constant_context *constants,
                        const struct mo_function_signature *signature, struct mo_checked_spec *spec,
                        struct mo_checked_condition *conditions, struct mo_diags *diags,
                        bool *halt) {
    MO_ASSERT(spec != NULL && spec->syntax != NULL);
    struct mo_checked_function checked;
    if (infer_function(arena, interfaces, signatures, constants, signature, diags, &checked, halt,
                       spec, conditions) == NULL)
        return false;
    spec->body = checked.body;
    spec->parameter_count = checked.parameter_count;
    spec->parameters = checked.parameters;
    return true;
}

bool mo_check_constant(struct mo_arena *arena, const struct mo_module_interfaces *interfaces,
                       const struct mo_signatures *signatures,
                       struct mo_constant_context *constants, struct mo_checked_constant *constant,
                       struct mo_diags *diags, bool *halt) {
    struct mo_source_type unit = {.kind = MO_SOURCE_NAMED, .name = "Unit", .module = UINT32_MAX};
    const struct mo_source_type *result = &unit;
    struct mo_source_type type = {.kind = MO_SOURCE_FUNCTION,
                                  .name = "fn",
                                  .count = 1,
                                  .module = UINT32_MAX,
                                  .arguments = &result,
                                  .height = 1};
    struct mo_function_signature context = {.function = constant->symbol, .type = &type};
    struct mo_checked_function checked;
    if (infer_function(arena, interfaces, signatures, constants, &context, diags, &checked, halt,
                       NULL, NULL) == NULL)
        return false;
    const struct mo_source_type *value = checked.body.items[checked.body.root].type;
    if (constant->symbol->name[constant->symbol->name_length - 1] == '?') {
        const struct mo_source_type *answer =
            value->kind == MO_SOURCE_FUNCTION ? value->arguments[value->count - 1] : value;
        if (answer->kind != MO_SOURCE_NAMED || answer->module != UINT32_MAX ||
            strcmp(answer->name, "Bool") != 0) {
            bool kept = mo_diags_add(
                diags, &(struct mo_diag){
                           .code = MO_CODE_BOOL_NAME,
                           .primary = {constant->symbol->declaration->children[0]->span,
                                       "must be Bool or a function returning Bool"},
                           .message = "a name ending in ? isn't a yes-or-no answer",
                           .reason = "A name ending in ? is a Bool, or a function that returns "
                                     "one (reference 2.3)."});
            (void)kept;
            *halt = diags->dropped != 0 && diags->count < MO_LIMIT_DIAGNOSTICS;
            return false;
        }
    }
    constant->initializer = checked.body;
    constant->type = value;
    return true;
}
