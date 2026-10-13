#include "check/check.h"

#include "base/codes.h"
#include "base/limits.h"

#include <math.h>
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
};

struct local;
struct name_slot {
    const struct mo_syntax *name;
    const struct mo_syntax *module_use;
    const struct mo_syntax *local_name;
    struct name_slot *left, *right;
    struct local *active;
    uint32_t height;
};

struct local {
    struct name_slot *slot;
    const struct mo_syntax *name;
    struct term *type;
    struct local *previous;
    struct local *next;
    uint32_t function;
    bool variable;
    bool used;
};

struct note {
    struct mo_inferred_expression output;
    struct term *type;
    struct note *next;
    bool discarded;
    bool negative;
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
    struct term *result;
    struct mo_span span;
    uint32_t count, depth, loops, lambda, work, epoch;
    bool failed;
};

struct value {
    struct term *type;
    const struct mo_syntax *binding;
    const struct mo_symbol *symbol;
    bool never;
};

static MO_MUST_USE bool problem(struct inference *p, uint16_t code, struct mo_span span,
                                const char *message, const char *reason,
                                const struct mo_syntax *previous) {
    if (p->failed)
        return false;
    struct mo_label secondary = {previous == NULL ? span : previous->span, "declared here"};
    struct mo_edit edit = {{span.file, span.start, span.start}, "_"};
    struct mo_fix fix = {"mark the name unused on purpose", &edit, 1};
    bool offer_fix = false;
    if (code == MO_CODE_UNUSED_NAME) {
        const struct mo_syntax *name = p->locals->name;
        size_t length = name->span.end - name->span.start;
        uint8_t *candidate = mo_arena_allocate(p->arena, length + 1, 1);
        if (candidate == NULL)
            return problem(p, MO_CODE_CHECK_NO_MEMORY, span, "the checking arena has no room", NULL,
                           NULL);
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
    const char *label =
        code == MO_CODE_NAME_UNKNOWN        ? "not visible in this block"
        : code == MO_CODE_BIND_REPEATED     ? "this name already has a meaning"
        : code == MO_CODE_UNUSED_NAME       ? "never read"
        : code == MO_CODE_TYPE_MISMATCH     ? "the types don't match"
        : code == MO_CODE_CANNOT_INFER      ? "no use decides the type"
        : code == MO_CODE_BOOL_NAME         ? "must be Bool or a function returning Bool"
        : code == MO_CODE_CALL_ARGUMENTS    ? "pass one positional argument per parameter"
        : code == MO_CODE_UNUSED_VALUE      ? "not Unit and not used"
        : code == MO_CODE_NOT_VARIABLE      ? "no assignable var here"
        : code == MO_CODE_EXIT_OUTSIDE_LOOP ? "no enclosing loop in this function"
        : code == MO_CODE_LITERAL_RANGE     ? "outside its inferred numeric range"
        : code == MO_CODE_TUPLE_POSITION    ? "part index is outside this tuple"
        : code == MO_CODE_FAILURE_IGNORED   ? "handle this Result or pass its failure upward"
                                            : message;
    bool kept =
        mo_diags_add(p->diags, &(struct mo_diag){.code = code,
                                                 .primary = {span, label},
                                                 .message = message,
                                                 .reason = reason,
                                                 .secondary = previous == NULL ? NULL : &secondary,
                                                 .secondary_count = previous == NULL ? 0 : 1,
                                                 .fixes = offer_fix ? &fix : NULL,
                                                 .fix_count = offer_fix ? 1u : 0u});
    (void)kept;
    p->failed = true;
    return false;
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
    p->work++;
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

static MO_MUST_USE bool mismatch(struct inference *p) {
    return problem(p, MO_CODE_TYPE_MISMATCH, p->span, "values of different types are combined",
                   "Comparing or combining values of different types is an error (reference 3).",
                   NULL);
}

static MO_MUST_USE bool type_depth(struct inference *p, uint32_t depth) {
    if (depth < MO_LIMIT_SOURCE_TYPE_DEPTH)
        return true;
    return problem(p, MO_CODE_CHECK_TYPE_DEPTH, p->span, "the inferred type is nested too deeply",
                   NULL, NULL);
}

/* Each occurs walk visits a shared type once, not once per path through its DAG. */
static MO_MUST_USE bool occurs(struct inference *p, struct term *needle, struct term *t,
                               uint32_t depth) {
    if (!work(p) || !type_depth(p, depth))
        return true;
    t = root(t);
    if (needle == t)
        return true;
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
    if (a == b)
        return true;
    unsigned numeric = a->numeric == 0   ? b->numeric
                       : b->numeric == 0 ? a->numeric
                                         : a->numeric & b->numeric;
    if (a->numeric != 0 && b->numeric != 0 && numeric == 0)
        return mismatch(p);
    if ((!a->variable && numeric != 0 && (number_type(a) & numeric) == 0) ||
        (!b->variable && numeric != 0 && (number_type(b) & numeric) == 0))
        return mismatch(p);
    if (a->variable && !b->variable) {
        struct term *swap = a;
        a = b;
        b = swap;
    }
    if (b->variable) {
        MO_ASSERT(p->epoch < UINT32_MAX);
        p->epoch++;
        if (occurs(p, b, a, 0))
            return p->failed ? false : mismatch(p);
    } else {
        if (a->shape.kind != b->shape.kind || a->shape.module != b->shape.module ||
            a->shape.declaration != b->shape.declaration || a->shape.count != b->shape.count ||
            strcmp(a->shape.name, b->shape.name) != 0)
            return mismatch(p);
        for (uint32_t i = 0; i < a->shape.count; i++)
            if (!unify(p, a->arguments[i], b->arguments[i], depth + 1))
                return false;
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

static MO_MUST_USE struct term *annotation(struct inference *p, const struct mo_source_type *type,
                                           struct term **parameters, uint32_t depth) {
    if (!type_depth(p, depth))
        return NULL;
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

static MO_MUST_USE bool bind(struct inference *p, const struct mo_syntax *name, struct term *type,
                             bool variable) {
    if (name->kind == MO_S_UNDERSCORE)
        return true;
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
    return true;
}

static MO_MUST_USE bool leave(struct inference *p, struct local *mark) {
    while (p->locals != mark) {
        struct local *l = p->locals;
        if (!l->used && l->name->text[0] != '_')
            return problem(p, MO_CODE_UNUSED_NAME, l->name->span, "a name or parameter is unused",
                           "A name starting with _ is unused on purpose (reference 6.1).", NULL);
        l->slot->active = NULL;
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
        if (lo == scope->alias_count || compare_text(prefix, scope->aliases[lo].name) != 0)
            return NULL;
        const struct mo_symbol *s =
            find_symbol(&p->interfaces->scopes[scope->aliases[lo].module], name, count);
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

static MO_MUST_USE struct value identifier(struct inference *p, const struct mo_syntax *node,
                                           const struct mo_syntax *prefix) {
    if (prefix == NULL && node->kind == MO_S_NAME) {
        struct name_slot *s = slot(p, node, false);
        if (s != NULL && s->active != NULL) {
            s->active->used = true;
            return (struct value){.type = s->active->type, .binding = s->active->name};
        }
    }
    if (node->kind == MO_S_CAPNAME && compare_text(node, "Unit") == 0)
        return (struct value){.type = native(p, "Unit", 0)};
    uint32_t count = 0;
    const struct mo_symbol *s = definition(p, node, prefix, &count);
    if (p->failed)
        return (struct value){0};
    if (s == NULL) {
        if (prefix != NULL || node->kind == MO_S_CAPNAME || node->kind == MO_S_QUALIFIED) {
            bool reported = unfinished(p);
            (void)reported;
        } else {
            bool reported =
                problem(p, MO_CODE_NAME_UNKNOWN, node->span, "a name isn't visible in this block",
                        "A local name is visible from its binding to the end of its "
                        "block (reference 6.1).",
                        NULL);
            (void)reported;
        }
        return (struct value){0};
    }
    if (s->declaration->kind != MO_S_FN || count != 1) {
        bool reported = unfinished(p);
        (void)reported;
        return (struct value){0};
    }
    if (prefix == NULL && node->kind == MO_S_NAME && s->module == p->function->function->module) {
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
    struct term *parameters[26] = {NULL};
    return (struct value){.type = annotation(p, p->signatures->items[lo].type, parameters, 0),
                          .symbol = s};
}

static MO_MUST_USE struct value infer(struct inference *p, const struct mo_syntax *node,
                                      struct term *expected);

static MO_MUST_USE struct value scoped(struct inference *p, const struct mo_syntax *node,
                                       struct term *expected) {
    struct local *mark = p->locals;
    struct value v = infer(p, node, expected);
    if (v.type == NULL || !leave(p, mark))
        return (struct value){0};
    return v;
}

static MO_MUST_USE struct value sequence(struct inference *p, const struct mo_syntax *node,
                                         struct term *expected) {
    struct value last = {.type = native(p, "Unit", 0)};
    bool never = false;
    for (uint32_t i = 0; i < node->count; i++) {
        last = infer(p, node->children[i], i + 1 == node->count && !never ? expected : NULL);
        if (last.type == NULL)
            return (struct value){0};
        never = never || last.never;
        /* The last note belongs to this statement, not its nested expressions. */
        if (i + 1 != node->count)
            p->last->discarded = !last.never;
    }
    last.never = never;
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
    if (t->shape.kind != MO_SOURCE_FUNCTION || t->shape.count != args->count + 1) {
        bool reported = problem(p, MO_CODE_CALL_ARGUMENTS, args->span,
                                "a call's arguments don't match its parameters",
                                "A function has no default or optional parameters; pass every "
                                "argument by position (reference 5.2).",
                                NULL);
        (void)reported;
        return (struct value){0};
    }
    bool never = callee.never;
    for (uint32_t i = 0; i < args->count; i++) {
        if (args->children[i]->kind == MO_S_NAMED_ARG) {
            bool reported = problem(p, MO_CODE_CALL_ARGUMENTS, args->children[i]->span,
                                    "a function is called with named fields",
                                    "Arguments are passed by position; only records and cases "
                                    "are built with named fields (reference 5.2).",
                                    NULL);
            (void)reported;
            return (struct value){0};
        }
        struct value arg = infer(p, args->children[i], t->arguments[i]);
        if (arg.type == NULL)
            return (struct value){0};
        never = never || arg.never;
    }
    return (struct value){.type = t->arguments[t->shape.count - 1], .never = never};
}

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
    if (operand == NULL || !unify(p, a.type, operand, 0))
        return (struct value){0};
    if (!logical && !comparison && op != MO_T_DOTDOT) {
        if (root(operand)->shape.kind == MO_SOURCE_PARAMETER || named(operand, "Time") ||
            named(operand, "Duration")) {
            bool reported = unfinished(p);
            (void)reported;
            return (struct value){0};
        }
        struct term *numeric = fresh(p, INT | BIG | DECIMAL | FLOAT);
        if (numeric == NULL || !unify(p, operand, numeric, 0))
            return (struct value){0};
        if (expected != NULL && !unify(p, operand, expected, 0))
            return (struct value){0};
    }
    struct value b = infer(p, right, operand);
    if (b.type == NULL)
        return (struct value){0};
    struct term *result = logical || comparison ? native(p, "Bool", 0)
                          : op == MO_T_DOTDOT   ? native(p, "Range", 0)
                                                : operand;
    return (struct value){.type = result, .never = a.never || (!logical && b.never)};
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
    struct value test = infer(p, node->children[0], condition);
    if (test.type == NULL)
        return (struct value){0};
    struct value branch = scoped(p, node->children[1], result);
    if (branch.type == NULL)
        return (struct value){0};
    bool never = has_else && branch.never;
    for (uint32_t i = 0; i < parts->count; i++) {
        const struct mo_syntax *part = parts->children[i];
        if (part->kind == MO_S_IF_PART) {
            test = infer(p, part->children[0], condition);
            if (test.type == NULL)
                return (struct value){0};
        }
        branch = scoped(p, part->children[part->kind == MO_S_ELSE ? 0 : 1], result);
        if (branch.type == NULL)
            return (struct value){0};
        never = never && branch.never;
    }
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
    return (struct value){.type = t->arguments[at], .never = v.never};
}

static MO_MUST_USE struct value infer_node(struct inference *p, const struct mo_syntax *node,
                                           struct term *expected) {
    switch (node->kind) {
    case MO_S_SEQUENCE:
        return sequence(p, node, expected);
    case MO_S_EXPR_STMT:
    case MO_S_GROUP:
        return infer(p, node->children[0], expected);
    case MO_S_NAME:
    case MO_S_CAPNAME:
    case MO_S_QUALIFIED:
        return identifier(p, node, NULL);
    case MO_S_FIELD_ACCESS:
        if (node->children[0]->kind == MO_S_CAPNAME)
            return identifier(p, node->children[1], node->children[0]);
        break;
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
        if (assignment && node->kind != MO_S_BIND) {
            struct term *numeric = fresh(p, INT | BIG | DECIMAL | FLOAT);
            if (numeric == NULL || !unify(p, numeric, l->type, 0))
                return (struct value){0};
            l->used = true;
        }
        struct value rhs = infer(p, node->children[1], assignment ? l->type : NULL);
        if (rhs.type == NULL)
            return (struct value){0};
        if (!assignment &&
            !(node->kind == MO_S_VAR ? bind(p, name, rhs.type, true) : binder(p, name, rhs.type)))
            return (struct value){0};
        return (struct value){.type = native(p, "Unit", 0), .never = rhs.never};
    }
    case MO_S_BINARY:
    case MO_S_RANGE:
        return binary(p, node->token, infer(p, node->children[0], NULL), node->children[1],
                      expected);
    case MO_S_BINARY_CHAIN: {
        struct value result = infer(p, node->children[0], NULL);
        for (uint32_t i = 1; i < node->count && result.type != NULL; i += 2)
            result = binary(p, node->children[i]->token, result, node->children[i + 1], expected);
        return result;
    }
    case MO_S_CALL:
        return call(p, infer(p, node->children[0], NULL), node->children[1]);
    case MO_S_POSITION:
        return position(p, infer(p, node->children[0], NULL), node->children[1]);
    case MO_S_POSTFIX: {
        bool qualified =
            node->children[0]->kind == MO_S_CAPNAME && node->children[1]->kind == MO_S_FIELD_ACCESS;
        struct value v = qualified
                             ? identifier(p, node->children[1]->children[0], node->children[0])
                             : infer(p, node->children[0], NULL);
        for (uint32_t i = qualified ? 2u : 1u; i < node->count && v.type != NULL; i++) {
            const struct mo_syntax *step = node->children[i];
            if (step->kind == MO_S_POSITION)
                v = position(p, v, step->children[0]);
            else if (step->kind == MO_S_CALL)
                v = call(p, v, step->children[0]);
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
        if (expected != NULL && !unify(p, fn, expected, 0))
            return (struct value){0};
        struct local *mark = p->locals;
        struct term *old_result = p->result;
        uint32_t old_loops = p->loops;
        p->loops = 0;
        p->lambda++;
        p->result = fn->arguments[parameters->count];
        for (uint32_t i = 0; i < parameters->count; i++)
            if (!bind(p, parameters->children[i], fn->arguments[i], false))
                return (struct value){0};
        struct value body = infer(p, node->children[1], p->result);
        if (body.type == NULL || !leave(p, mark))
            return (struct value){0};
        p->result = old_result;
        p->loops = old_loops;
        p->lambda--;
        return (struct value){.type = fn};
    }
    case MO_S_IF:
    case MO_S_LINE_IF:
        return conditional(p, node, expected);
    case MO_S_RETURN: {
        struct value value = node->children[0] == NULL
                                 ? (struct value){.type = native(p, "Unit", 0)}
                                 : infer(p, node->children[0], p->result);
        if (value.type == NULL || !unify(p, value.type, p->result, 0))
            return (struct value){0};
        if (node->children[1] != NULL) {
            struct term *condition = native(p, "Bool", 0);
            if (condition == NULL || infer(p, node->children[1], condition).type == NULL)
                return (struct value){0};
        }
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
        struct term *condition = native(p, "Bool", 0);
        if (condition == NULL || infer(p, node->children[0], condition).type == NULL)
            return (struct value){0};
        return (struct value){.type = native(p, "Unit", 0)};
    }
    case MO_S_FOR:
    case MO_S_WHILE: {
        struct local *mark = p->locals;
        uint32_t body_index = node->kind == MO_S_FOR ? 2 : 1;
        struct value condition = infer(p, node->children[body_index - 1], NULL);
        if (condition.type == NULL)
            return (struct value){0};
        if (node->kind == MO_S_WHILE) {
            struct term *boolean = native(p, "Bool", 0);
            if (boolean == NULL || !unify(p, condition.type, boolean, 0))
                return (struct value){0};
        } else {
            struct term *element = NULL, *container = root(condition.type);
            if (named(container, "Range"))
                element = native(p, "Int", 0);
            else if (named(container, "List") || named(container, "Set"))
                element = container->arguments[0];
            else if (named(container, "Map")) {
                element = make_type(p, (struct mo_source_type){.kind = MO_SOURCE_TUPLE,
                                                               .name = "()",
                                                               .module = UINT32_MAX,
                                                               .count = 2});
                if (element != NULL) {
                    element->arguments[0] = container->arguments[0];
                    element->arguments[1] = container->arguments[1];
                }
            } else {
                bool reported = unfinished(p);
                (void)reported;
            }
            if (element == NULL || !binder(p, node->children[0], element))
                return (struct value){0};
        }
        p->loops++;
        struct term *unit = native(p, "Unit", 0);
        struct value body =
            unit == NULL ? (struct value){0} : infer(p, node->children[body_index], unit);
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
            if (condition == NULL || infer(p, node->children[0], condition).type == NULL)
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
        if (expected != NULL && !unify(p, t, expected, 0))
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
            if (!unify(p, v.type, wanted, 0))
                return (struct value){0};
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
    case MO_S_CASE:
    case MO_S_ARM:
    case MO_S_TRY:
    case MO_S_IS:
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
    MO_ASSERT(p->depth < 4 * MO_LIMIT_NESTING_DEPTH + 8);
    p->span = node->span;
    p->depth++;
    struct value v = infer_node(p, node, expected);
    p->depth--;
    p->span = node->span;
    if (v.type == NULL || (expected != NULL && !v.never && !unify(p, v.type, expected, 0)))
        return (struct value){0};
    struct note *n = allocate(p, 1, sizeof(*n), _Alignof(struct note));
    if (n == NULL)
        return (struct value){0};
    *n = (struct note){
        .output = {.syntax = node, .binding = v.binding, .symbol = v.symbol, .never = v.never},
        .type = v.type};
    if (p->last == NULL)
        p->notes = n;
    else
        p->last->next = n;
    p->last = n;
    p->count++;
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
        uint32_t height = root(t->arguments[i])->height + 1;
        if (height > t->height)
            t->height = height;
    }
    struct mo_source_type *out = allocate(p, 1, sizeof(*out), _Alignof(struct mo_source_type));
    if (out != NULL) {
        *out = t->shape;
        out->arguments = args;
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
        mo_u128 maximum = ((mo_u128)1 << 127) - (n->negative ? 0u : 1u);
        fits = fits && places <= 18;
        for (uint32_t i = places; fits && i < 18; i++) {
            if (magnitude > maximum / 10)
                fits = false;
            else
                magnitude *= 10;
        }
        fits = fits && magnitude <= maximum;
    } else if (strcmp(name, "Float") == 0) {
        if (base != 10)
            fits = finite_integer(node, base);
        else {
            char *text = allocate(p, length + 1, 1, 1);
            if (text == NULL)
                return false;
            size_t at = 0;
            for (size_t i = 0; i < length; i++)
                if (node->text[i] != '_')
                    text[at++] = (char)node->text[i];
            text[at] = '\0';
            char *end;
            double value = strtod(text, &end);
            MO_ASSERT(end == text + at);
            fits = isfinite(value);
        }
    }
    return fits ||
           problem(p, MO_CODE_LITERAL_RANGE, node->span, "a number literal doesn't fit its type",
                   "A number literal takes its context's numeric type and must fit "
                   "that type's range (reference 3.3).",
                   NULL);
}

struct mo_inferred_body *mo_infer_function(struct mo_arena *arena,
                                           const struct mo_module_interfaces *interfaces,
                                           const struct mo_signatures *signatures,
                                           uint32_t function, struct mo_diags *diags) {
    MO_ASSERT(arena != NULL && interfaces != NULL && signatures != NULL && diags != NULL);
    MO_ASSERT(function < signatures->count);
    const struct mo_function_signature *signature = &signatures->items[function];
    const struct mo_syntax *syntax = signature->function->declaration;
    struct inference p = {.arena = arena,
                          .diags = diags,
                          .interfaces = interfaces,
                          .signatures = signatures,
                          .function = signature,
                          .span = syntax->span};
    struct term *fn = annotation(&p, signature->type, NULL, 0);
    if (fn == NULL)
        goto failed;
    p.result = fn->arguments[fn->shape.count - 1];
    const struct mo_syntax *parameters = syntax->children[1];
    for (uint32_t i = 0; i < parameters->count; i++)
        if (!bind(&p, parameters->children[i]->children[0], fn->arguments[i], false))
            goto failed;
    struct value v = infer(&p, syntax->children[5], p.result);
    if (v.type == NULL || !leave(&p, NULL))
        goto failed;
    struct mo_inferred_body *out = allocate(&p, 1, sizeof(*out), _Alignof(struct mo_inferred_body));
    struct mo_inferred_expression *items =
        allocate(&p, p.count, sizeof(*items), _Alignof(struct mo_inferred_expression));
    if (p.failed)
        goto failed;
    *out = (struct mo_inferred_body){.function = signature, .count = p.count, .items = items};
    uint32_t i = 0;
    for (struct note *n = p.notes; n != NULL; n = n->next) {
        p.span = n->output.syntax->span;
        n->output.type = finish_type(&p, n->type, 0);
        if (n->output.type == NULL || !literal(&p, n))
            goto failed;
        if (n->discarded && !named(n->type, "Unit")) {
            bool reported = problem(
                &p, named(n->type, "Result") ? MO_CODE_FAILURE_IGNORED : MO_CODE_UNUSED_VALUE,
                p.span, named(n->type, "Result") ? "a failure is ignored" : "a result isn't used",
                named(n->type, "Result") ? "Mo never drops a failure silently (reference 8.3)."
                                         : "A result that isn't Unit and isn't used is an error "
                                           "(reference 6.1).",
                NULL);
            (void)reported;
            goto failed;
        }
        items[i++] = n->output;
    }
    for (struct local *l = p.all_locals; l != NULL; l = l->next) {
        p.span = l->name->span;
        const struct mo_source_type *type = finish_type(&p, l->type, 0);
        if (type == NULL)
            goto failed;
        if (l->name->text[l->name->span.end - l->name->span.start - 1] != '?')
            continue;
        if (type->kind == MO_SOURCE_FUNCTION)
            type = type->arguments[type->count - 1];
        if (type->kind != MO_SOURCE_NAMED || type->module != UINT32_MAX ||
            strcmp(type->name, "Bool") != 0) {
            bool reported = problem(&p, MO_CODE_BOOL_NAME, p.span,
                                    "a name ending in ? isn't a yes-or-no answer",
                                    "A name ending in ? is a Bool, or a function that returns "
                                    "one (reference 2.3).",
                                    NULL);
            (void)reported;
            goto failed;
        }
    }
    MO_ASSERT(i == p.count);
    return out;
failed:
    mo_diags_sort(diags);
    return NULL;
}
