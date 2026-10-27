#include "core/core.h"

#include <string.h>

#include "base/codes.h"
#include "base/limits.h"

/* Ordered indices preserve Lean's first-match lookup without scans of wide declarations or
 * parameter lists. Types are interned by tag and child identities, never by recursive equality.
 * No hash table depends on friendly names. AVL recursion is logarithmic and bounded by nodes. */
struct index {
    struct index *left, *right;
    mo_core_string key;
    void *value;
    unsigned height;
};
struct type {
    enum mo_core_ty_tag tag;
    size_t id, count;
    struct type **parts;
    mo_core_string name;
    enum mo_core_cap_tag cap;
    struct data *constructors;
    bool resolved;
};
struct expr;
struct answer {
    struct type *ty, *expected;
    uint16_t code;
    const struct expr *at;
};
struct memo {
    size_t expected;
    struct answer answer;
    struct memo *next;
};
struct expr {
    mo_core_expr source;
    struct expr **children;
    size_t count, node;
    struct type *annotation;
    struct type **params;
    struct signature *signature;
    struct expr *parent;
    struct index *bindings, *arm_bindings, *joins;
    bool bound, arm_bound;
    unsigned memo_count;
    struct memo memo[2];
    struct memo *extra;
};
struct function {
    const struct mo_core_fun *source;
    struct type **params, *ret;
    size_t scope;
};
struct constructor {
    struct type **fields;
    size_t count;
    mo_core_string name;
};
struct data {
    struct constructor *ctors;
    size_t count;
    struct index *names;
};
struct constant {
    struct type *ty;
    size_t index;
};
struct checker {
    struct mo_arena arena, body;
    const struct mo_core *core;
    const struct mo_services *services;
    struct index *types, *nominals, *annotations, *funs, *data, *consts;
    size_t nodes, type_count, scope;
    bool spec;
    uint16_t fatal;
    mo_core_string declaration;
    struct mo_core_span declaration_span;
};
struct context {
    struct index *names;
    const struct context *outer;
};

static MO_MUST_USE void *allocate(struct checker *c, struct mo_arena *a, size_t n, size_t size) {
    if (n > MO_LIMIT_CORE_NODES) {
        c->fatal = MO_CODE_CORE_CHECK_LIMIT;
        return NULL;
    }
    void *p = mo_arena_allocate_array(a, n, size, MO_ARENA_ALIGNMENT);
    if (p == NULL)
        c->fatal = MO_CODE_CORE_CHECK_MEMORY;
    else
        memset(p, 0, n * size);
    return p;
}
static MO_MUST_USE int compare(mo_core_string a, mo_core_string b) {
    size_t n = a.length < b.length ? a.length : b.length;
    int order = n == 0 ? 0 : memcmp(a.bytes, b.bytes, n);
    return order != 0 ? order : (a.length > b.length) - (a.length < b.length);
}
static MO_MUST_USE unsigned height(const struct index *p) {
    return p == NULL ? 0 : p->height;
}
static void update_height(struct index *p) {
    unsigned l = height(p->left), r = height(p->right);
    p->height = 1 + (l > r ? l : r);
    MO_ASSERT(p->height < MO_LIMIT_CORE_DEPTH);
}
static MO_MUST_USE struct index *rotate_left(struct index *p) {
    struct index *r = p->right;
    MO_ASSERT(r != NULL);
    p->right = r->left;
    r->left = p;
    update_height(p);
    update_height(r);
    return r;
}
static MO_MUST_USE struct index *rotate_right(struct index *p) {
    struct index *l = p->left;
    MO_ASSERT(l != NULL);
    p->left = l->right;
    l->right = p;
    update_height(p);
    update_height(l);
    return l;
}
static MO_MUST_USE struct index *insert(struct index *p, struct index *item) {
    if (p == NULL)
        return item;
    int order = compare(item->key, p->key);
    MO_ASSERT(order != 0);
    if (order < 0)
        p->left = insert(p->left, item);
    else
        p->right = insert(p->right, item);
    update_height(p);
    if (height(p->left) > height(p->right) + 1) {
        if (compare(item->key, p->left->key) > 0)
            p->left = rotate_left(p->left);
        return rotate_right(p);
    }
    if (height(p->right) > height(p->left) + 1) {
        if (compare(item->key, p->right->key) < 0)
            p->right = rotate_right(p->right);
        return rotate_left(p);
    }
    return p;
}
static MO_MUST_USE void *find(const struct index *p, mo_core_string key) {
    while (p != NULL) {
        int order = compare(key, p->key);
        if (order == 0)
            return p->value;
        p = order < 0 ? p->left : p->right;
    }
    return NULL;
}
static MO_MUST_USE bool put(struct checker *c, struct mo_arena *a, struct index **root,
                            mo_core_string key, void *value, bool copy) {
    MO_ASSERT(value != NULL);
    if (find(*root, key) != NULL)
        return true;
    struct index *item = allocate(c, a, 1, sizeof *item);
    if (item == NULL)
        return false;
    if (copy) {
        char *bytes = mo_arena_allocate(a, key.length, 1);
        if (bytes == NULL) {
            c->fatal = MO_CODE_CORE_CHECK_MEMORY;
            return false;
        }
        if (key.length != 0)
            memcpy(bytes, key.bytes, key.length);
        key.bytes = bytes;
    }
    *item = (struct index){.key = key, .value = value, .height = 1};
    *root = insert(*root, item);
    return true;
}
static MO_MUST_USE mo_core_string bytes_key(const void *p, size_t n) {
    return (mo_core_string){p, n};
}
static MO_MUST_USE struct type *intern(struct checker *c, enum mo_core_ty_tag tag,
                                       struct type **parts, size_t count, mo_core_string name,
                                       enum mo_core_cap_tag cap) {
    /* Child IDs have no padding; byte order is private to this invocation. */
    size_t small[5];
    size_t *key = count <= 3 ? small : allocate(c, &c->body, count + 2, sizeof *key);
    if (key == NULL)
        return NULL;
    key[0] = (size_t)tag;
    key[1] = tag == MO_CORE_TY_CAP ? (size_t)cap : 0;
    for (size_t i = 0; i < count; i++) {
        if (parts[i] == NULL)
            return NULL;
        key[i + 2] = parts[i]->id;
    }
    struct index **table = tag == MO_CORE_TY_DATA ? &c->nominals : &c->types;
    mo_core_string k = tag == MO_CORE_TY_DATA ? name : bytes_key(key, (count + 2) * sizeof *key);
    struct type *t = find(*table, k);
    if (t != NULL)
        return t;
    t = allocate(c, &c->arena, 1, sizeof *t);
    if (t == NULL)
        return NULL;
    struct type **saved = count == 0 ? NULL : allocate(c, &c->arena, count, sizeof *saved);
    if (count != 0 && saved == NULL)
        return NULL;
    if (count != 0)
        memcpy(saved, parts, count * sizeof *saved);
    *t = (struct type){.tag = tag,
                       .id = ++c->type_count,
                       .count = count,
                       .parts = saved,
                       .cap = cap,
                       .name = name};
    if (!put(c, &c->arena, table, k, t, tag != MO_CORE_TY_DATA))
        return NULL;
    return t;
}
static MO_MUST_USE struct type *scalar(struct checker *c, enum mo_core_ty_tag tag) {
    return intern(c, tag, NULL, 0, (mo_core_string){0}, MO_CORE_CAP_CONSOLE);
}
static MO_MUST_USE struct type *unary(struct checker *c, enum mo_core_ty_tag tag, struct type *t) {
    return intern(c, tag, &t, 1, (mo_core_string){0}, MO_CORE_CAP_CONSOLE);
}
static MO_MUST_USE struct type *binary(struct checker *c, enum mo_core_ty_tag tag, struct type *a,
                                       struct type *b) {
    struct type *parts[] = {a, b};
    return intern(c, tag, parts, 2, (mo_core_string){0}, MO_CORE_CAP_CONSOLE);
}
static MO_MUST_USE struct type *capability(struct checker *c, enum mo_core_cap_tag cap) {
    return intern(c, MO_CORE_TY_CAP, NULL, 0, (mo_core_string){0}, cap);
}
static MO_MUST_USE struct type *annotation(struct checker *c, mo_core_ty t, size_t depth) {
    if (t == NULL || depth >= MO_LIMIT_CORE_DEPTH) {
        c->fatal = MO_CODE_CORE_CHECK_LIMIT;
        return NULL;
    }
    mo_core_string key = bytes_key(&t, sizeof t);
    struct type *cached = find(c->annotations, key);
    if (cached != NULL)
        return cached;
    if (++c->nodes > MO_LIMIT_CORE_NODES) {
        c->fatal = MO_CODE_CORE_CHECK_LIMIT;
        return NULL;
    }
    struct type *r = NULL;
    switch (t->tag) {
    case MO_CORE_TY_TUPLE:
    case MO_CORE_TY_FN: {
        mo_core_ty_list ts = t->tag == MO_CORE_TY_TUPLE ? t->as.tuple : t->as.fn.params;
        size_t n = ts.count + (t->tag == MO_CORE_TY_FN ? 1u : 0u);
        struct type **parts = allocate(c, &c->body, n, sizeof *parts);
        if (parts == NULL)
            return NULL;
        for (size_t i = 0; i < ts.count; i++)
            parts[i] = annotation(c, ts.items[i], depth + 1);
        if (t->tag == MO_CORE_TY_FN)
            parts[ts.count] = annotation(c, t->as.fn.ret, depth + 1);
        r = intern(c, t->tag, parts, n, (mo_core_string){0}, MO_CORE_CAP_CONSOLE);
        break;
    }
    case MO_CORE_TY_LIST:
        r = unary(c, t->tag, annotation(c, t->as.list, depth + 1));
        break;
    case MO_CORE_TY_SET:
        r = unary(c, t->tag, annotation(c, t->as.set, depth + 1));
        break;
    case MO_CORE_TY_OPTION:
        r = unary(c, t->tag, annotation(c, t->as.option, depth + 1));
        break;
    case MO_CORE_TY_MAP:
        r = binary(c, t->tag, annotation(c, t->as.map.k, depth + 1),
                   annotation(c, t->as.map.v, depth + 1));
        break;
    case MO_CORE_TY_RESULT:
        r = binary(c, t->tag, annotation(c, t->as.result.t, depth + 1),
                   annotation(c, t->as.result.e, depth + 1));
        break;
    case MO_CORE_TY_DATA:
        r = intern(c, t->tag, NULL, 0, t->as.data, MO_CORE_CAP_CONSOLE);
        break;
    case MO_CORE_TY_CAP:
        r = capability(c, t->as.cap->tag);
        break;
    case MO_CORE_TY_INT:
    case MO_CORE_TY_BIG_INT:
    case MO_CORE_TY_BYTE:
    case MO_CORE_TY_DECIMAL:
    case MO_CORE_TY_FLOAT:
    case MO_CORE_TY_BOOL:
    case MO_CORE_TY_CHAR:
    case MO_CORE_TY_TEXT:
    case MO_CORE_TY_BYTES:
    case MO_CORE_TY_UNIT:
    case MO_CORE_TY_RANGE:
    case MO_CORE_TY_TIME:
    case MO_CORE_TY_DURATION:
    case MO_CORE_TY_ORDERING:
    case MO_CORE_TY_FILE_ERROR:
    case MO_CORE_TY_SIMULATION:
    case MO_CORE_TY_SIMULATION_ERROR:
    case MO_CORE_TY_ENTRY:
    case MO_CORE_TY_ENTRY_KIND:
        r = scalar(c, t->tag);
        break;
    }
    if (r == NULL || !put(c, &c->arena, &c->annotations, key, r, true))
        return NULL;
    return r;
}
static MO_MUST_USE struct type **parameter_types(struct checker *c, struct mo_arena *arena,
                                                 mo_core_params ps) {
    struct type **ts = allocate(c, arena, ps.count, sizeof *ts);
    if (ts == NULL)
        return NULL;
    for (size_t i = 0; i < ps.count; i++) {
        ts[i] = annotation(c, ps.items[i].ty, 0);
        if (ts[i] == NULL)
            return NULL;
    }
    return ts;
}
static MO_MUST_USE bool natural(mo_core_natural n, size_t limit, size_t *out) {
    size_t value = 0;
    for (size_t i = 0; i < n.digits.length; i++) {
        size_t digit = (size_t)(n.digits.bytes[i] - '0');
        if (digit > limit || value > (limit - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    *out = value;
    return !n.negative;
}
static MO_MUST_USE bool magnitude(mo_core_integer n, const char *bound) {
    size_t length = strlen(bound);
    return n.digits.length < length ||
           (n.digits.length == length && memcmp(n.digits.bytes, bound, length) <= 0);
}
static MO_MUST_USE struct type *literal(struct checker *c, mo_core_lit l) {
    bool valid = true;
    enum mo_core_ty_tag tag = MO_CORE_TY_UNIT;
    switch (l->tag) {
    case MO_CORE_LIT_INT:
        tag = MO_CORE_TY_INT;
        valid = magnitude(l->as.int_,
                          l->as.int_.negative ? "9223372036854775808" : "9223372036854775807");
        break;
    case MO_CORE_LIT_BIG_INT:
        tag = MO_CORE_TY_BIG_INT;
        break;
    case MO_CORE_LIT_BYTE:
        tag = MO_CORE_TY_BYTE;
        valid = !l->as.byte.negative && magnitude(l->as.byte, "255");
        break;
    case MO_CORE_LIT_DECIMAL:
        tag = MO_CORE_TY_DECIMAL;
        valid = magnitude(l->as.decimal, "170141183460469231731687303715884105727");
        break;
    case MO_CORE_LIT_FLOAT:
        tag = MO_CORE_TY_FLOAT;
        break;
    case MO_CORE_LIT_BOOL:
        tag = MO_CORE_TY_BOOL;
        break;
    case MO_CORE_LIT_CHAR:
        tag = MO_CORE_TY_CHAR;
        break;
    case MO_CORE_LIT_TEXT:
        tag = MO_CORE_TY_TEXT;
        break;
    case MO_CORE_LIT_BYTES:
        tag = MO_CORE_TY_BYTES;
        for (size_t i = 0; i < l->as.bytes.count; i++)
            if (l->as.bytes.items[i].negative || !magnitude(l->as.bytes.items[i], "255"))
                valid = false;
        break;
    case MO_CORE_LIT_UNIT:
        break;
    case MO_CORE_LIT_TIME:
        tag = MO_CORE_TY_TIME;
        valid =
            magnitude(l->as.time, l->as.time.negative ? "62135596800000000" : "253402300799999999");
        break;
    case MO_CORE_LIT_DURATION:
        tag = MO_CORE_TY_DURATION;
        valid = magnitude(l->as.duration,
                          l->as.duration.negative ? "9223372036854775808" : "9223372036854775807");
        break;
    }
    return valid ? scalar(c, tag) : NULL;
}

/* Primitive signatures mirror Syntax.lean. Literal counts are compared to supplied arguments
 * before allocation: a hostile arbitrary-precision Nat cannot request a giant signature. */
struct signature {
    struct type *ret, *args[3];
    struct type **selected;
    size_t count;
    bool spec, repeated;
};
static MO_MUST_USE struct signature sig(struct type *r, size_t n, struct type *a, struct type *b,
                                        struct type *d) {
    return (struct signature){.ret = r, .args = {a, b, d}, .count = n};
}

static MO_MUST_USE struct signature simulation(struct checker *c, const struct mo_core_sim_op *op) {
    struct signature invalid = {0};
    struct type *handle = scalar(c, MO_CORE_TY_SIMULATION);
    if (op->tag == MO_SIM_ADVANCE)
        return sig(scalar(c, MO_CORE_TY_UNIT), 2, handle, scalar(c, MO_CORE_TY_DURATION), NULL);
    if (op->tag == MO_SIM_CRASH || op->tag == MO_SIM_RESTART)
        return sig(scalar(c, MO_CORE_TY_UNIT), 1, handle, NULL, NULL);
    if (c->services == NULL || c->services->count == 0)
        goto refuse;
    size_t descriptor, tool;
    if (!natural(op->descriptor, c->services->count - 1, &descriptor))
        goto refuse;
    const struct mo_service *s = &c->services->items[descriptor];
    if (s->start >= c->core->funs.count)
        goto refuse;
    const struct mo_core_fun *start = &c->core->funs.items[s->start];
    if (start->kind != MO_CORE_FUNCTION || start->params.count != 0)
        goto refuse;
    if (op->tag == MO_SIM_START) {
        struct signature result = sig(handle, 1, scalar(c, MO_CORE_TY_TIME), NULL, NULL);
        result.spec = true;
        return result;
    }
    if (op->tag != MO_SIM_SEND || s->tool_count == 0 ||
        !natural(op->tool, s->tool_count - 1, &tool) ||
        s->tools[tool].function >= c->core->funs.count)
        goto refuse;
    const struct mo_core_fun *fn = &c->core->funs.items[s->tools[tool].function];
    if (fn->kind != MO_CORE_FUNCTION || fn->params.count < 2 ||
        op->arguments.count != fn->params.count - 2 ||
        annotation(c, start->ret, 0) != annotation(c, fn->params.items[0].ty, 0) ||
        fn->params.items[1].ty->tag != MO_CORE_TY_TIME ||
        annotation(c, op->result, 0) != annotation(c, fn->ret, 0))
        goto refuse;
    struct signature result = {.ret = binary(c, MO_CORE_TY_RESULT, annotation(c, op->result, 0),
                                             scalar(c, MO_CORE_TY_SIMULATION_ERROR)),
                               .count = op->arguments.count + 1};
    result.selected = allocate(c, &c->body, result.count, sizeof *result.selected);
    if (result.selected == NULL)
        return invalid;
    result.selected[0] = handle;
    for (size_t i = 0; i < op->arguments.count; i++) {
        result.selected[i + 1] = annotation(c, op->arguments.items[i], 0);
        if (result.selected[i + 1] != annotation(c, fn->params.items[i + 2].ty, 0))
            goto refuse;
    }
    return result;
refuse:
    if (c->fatal == 0)
        c->fatal = MO_CODE_CORE_CHECK_ARGUMENTS;
    return invalid;
}

#define S(tag) scalar(c, MO_CORE_TY_##tag)
#define O(t) unary(c, MO_CORE_TY_OPTION, (t))
#define L(t) unary(c, MO_CORE_TY_LIST, (t))
#define C(tag) capability(c, MO_CORE_CAP_##tag)
static MO_MUST_USE struct signature primitive(struct checker *c, const struct mo_core_prim *p,
                                              size_t argc) {
    struct type *t = NULL, *k = NULL, *v = NULL, *collection = NULL;
    switch (p->tag) {
    case MO_CORE_PRIM_SIM:
        return simulation(c, &p->as.sim);
    case MO_CORE_PRIM_INT:
        switch (p->as.int_) {
        case MO_CORE_INT_OP_ADD:
        case MO_CORE_INT_OP_SUB:
        case MO_CORE_INT_OP_MUL:
        case MO_CORE_INT_OP_DIV:
        case MO_CORE_INT_OP_REM:
        case MO_CORE_INT_OP_MIN:
        case MO_CORE_INT_OP_MAX:
        case MO_CORE_INT_OP_POW:
        case MO_CORE_INT_OP_FLOOR_DIV:
        case MO_CORE_INT_OP_FLOOR_MOD:
        case MO_CORE_INT_OP_WRAPPING_ADD:
        case MO_CORE_INT_OP_WRAPPING_SUB:
        case MO_CORE_INT_OP_WRAPPING_MUL:
        case MO_CORE_INT_OP_SATURATING_ADD:
        case MO_CORE_INT_OP_SATURATING_SUB:
        case MO_CORE_INT_OP_SATURATING_MUL:
            return sig(S(INT), 2, S(INT), S(INT), NULL);
        case MO_CORE_INT_OP_NEG:
        case MO_CORE_INT_OP_ABS:
            return sig(S(INT), 1, S(INT), NULL, NULL);
        case MO_CORE_INT_OP_CHECKED_ADD:
        case MO_CORE_INT_OP_CHECKED_SUB:
        case MO_CORE_INT_OP_CHECKED_MUL:
            return sig(O(S(INT)), 2, S(INT), S(INT), NULL);
        case MO_CORE_INT_OP_TO_TEXT:
            return sig(S(TEXT), 1, S(INT), NULL, NULL);
        case MO_CORE_INT_OP_TO_DECIMAL:
            return sig(S(DECIMAL), 1, S(INT), NULL, NULL);
        case MO_CORE_INT_OP_TO_BIG:
            return sig(S(BIG_INT), 1, S(INT), NULL, NULL);
        case MO_CORE_INT_OP_TO_FLOAT:
            return sig(S(FLOAT), 1, S(INT), NULL, NULL);
        case MO_CORE_INT_OP_TO_CHAR:
            return sig(O(S(CHAR)), 1, S(INT), NULL, NULL);
        case MO_CORE_INT_OP_TO_BYTE:
            return sig(O(S(BYTE)), 1, S(INT), NULL, NULL);
        case MO_CORE_INT_OP_SECONDS:
        case MO_CORE_INT_OP_MILLISECONDS:
        case MO_CORE_INT_OP_MINUTES:
        case MO_CORE_INT_OP_HOURS:
        case MO_CORE_INT_OP_DAYS:
            return sig(S(DURATION), 1, S(INT), NULL, NULL);
        }
        break;
    case MO_CORE_PRIM_BIG:
        switch (p->as.big) {
        case MO_CORE_BIG_OP_ADD:
        case MO_CORE_BIG_OP_SUB:
        case MO_CORE_BIG_OP_MUL:
        case MO_CORE_BIG_OP_DIV:
        case MO_CORE_BIG_OP_REM:
        case MO_CORE_BIG_OP_MIN:
        case MO_CORE_BIG_OP_MAX:
        case MO_CORE_BIG_OP_FLOOR_DIV:
        case MO_CORE_BIG_OP_FLOOR_MOD:
            return sig(S(BIG_INT), 2, S(BIG_INT), S(BIG_INT), NULL);
        case MO_CORE_BIG_OP_POW:
            return sig(S(BIG_INT), 2, S(BIG_INT), S(INT), NULL);
        case MO_CORE_BIG_OP_NEG:
        case MO_CORE_BIG_OP_ABS:
            return sig(S(BIG_INT), 1, S(BIG_INT), NULL, NULL);
        case MO_CORE_BIG_OP_TO_INT:
            return sig(O(S(INT)), 1, S(BIG_INT), NULL, NULL);
        case MO_CORE_BIG_OP_TO_TEXT:
            return sig(S(TEXT), 1, S(BIG_INT), NULL, NULL);
        }
        break;
    case MO_CORE_PRIM_BYTE:
        switch (p->as.byte) {
        case MO_CORE_BYTE_OP_TO_INT:
            return sig(S(INT), 1, S(BYTE), NULL, NULL);
        case MO_CORE_BYTE_OP_TO_TEXT:
            return sig(S(TEXT), 1, S(BYTE), NULL, NULL);
        }
        break;
    case MO_CORE_PRIM_DEC:
        switch (p->as.dec) {
        case MO_CORE_DEC_OP_ADD:
        case MO_CORE_DEC_OP_SUB:
        case MO_CORE_DEC_OP_MUL:
        case MO_CORE_DEC_OP_DIV:
        case MO_CORE_DEC_OP_REM:
        case MO_CORE_DEC_OP_MIN:
        case MO_CORE_DEC_OP_MAX:
            return sig(S(DECIMAL), 2, S(DECIMAL), S(DECIMAL), NULL);
        case MO_CORE_DEC_OP_NEG:
        case MO_CORE_DEC_OP_ABS:
        case MO_CORE_DEC_OP_FLOOR:
        case MO_CORE_DEC_OP_CEIL:
            return sig(S(DECIMAL), 1, S(DECIMAL), NULL, NULL);
        case MO_CORE_DEC_OP_ROUND:
            return sig(S(DECIMAL), 2, S(DECIMAL), S(INT), NULL);
        case MO_CORE_DEC_OP_TO_INT:
            return sig(O(S(INT)), 1, S(DECIMAL), NULL, NULL);
        case MO_CORE_DEC_OP_TO_TEXT:
            return sig(S(TEXT), 1, S(DECIMAL), NULL, NULL);
        case MO_CORE_DEC_OP_TO_FIXED:
            return sig(S(TEXT), 2, S(DECIMAL), S(INT), NULL);
        case MO_CORE_DEC_OP_TO_FLOAT:
            return sig(S(FLOAT), 1, S(DECIMAL), NULL, NULL);
        }
        break;
    case MO_CORE_PRIM_FLOAT:
        switch (p->as.float_) {
        case MO_CORE_FLOAT_OP_ADD:
        case MO_CORE_FLOAT_OP_SUB:
        case MO_CORE_FLOAT_OP_MUL:
        case MO_CORE_FLOAT_OP_DIV:
        case MO_CORE_FLOAT_OP_REM:
            return sig(S(FLOAT), 2, S(FLOAT), S(FLOAT), NULL);
        case MO_CORE_FLOAT_OP_NEG:
        case MO_CORE_FLOAT_OP_ABS:
        case MO_CORE_FLOAT_OP_SQRT:
        case MO_CORE_FLOAT_OP_FLOOR:
        case MO_CORE_FLOAT_OP_CEIL:
        case MO_CORE_FLOAT_OP_ROUND:
            return sig(S(FLOAT), 1, S(FLOAT), NULL, NULL);
        case MO_CORE_FLOAT_OP_IS_NAN:
        case MO_CORE_FLOAT_OP_IS_INFINITE:
            return sig(S(BOOL), 1, S(FLOAT), NULL, NULL);
        case MO_CORE_FLOAT_OP_TO_DECIMAL:
            return sig(O(S(DECIMAL)), 1, S(FLOAT), NULL, NULL);
        case MO_CORE_FLOAT_OP_TO_TEXT:
            return sig(S(TEXT), 1, S(FLOAT), NULL, NULL);
        }
        break;
    case MO_CORE_PRIM_NOT:
        return sig(S(BOOL), 1, S(BOOL), NULL, NULL);
    case MO_CORE_PRIM_CMP: {
        const enum mo_core_ty_tag bases[] = {
            MO_CORE_TY_INT,   MO_CORE_TY_BIG_INT, MO_CORE_TY_BYTE,    MO_CORE_TY_DECIMAL,
            MO_CORE_TY_FLOAT, MO_CORE_TY_BOOL,    MO_CORE_TY_CHAR,    MO_CORE_TY_TEXT,
            MO_CORE_TY_BYTES, MO_CORE_TY_TIME,    MO_CORE_TY_DURATION};
        t = scalar(c, bases[p->as.cmp.base]);
        return sig(S(BOOL), 2, t, t, NULL);
    }
    case MO_CORE_PRIM_CHAR:
        switch (p->as.char_) {
        case MO_CORE_CHAR_OP_CODE:
            return sig(S(INT), 1, S(CHAR), NULL, NULL);
        case MO_CORE_CHAR_OP_IS_DIGIT:
        case MO_CORE_CHAR_OP_IS_LETTER:
        case MO_CORE_CHAR_OP_IS_WHITE_SPACE:
            return sig(S(BOOL), 1, S(CHAR), NULL, NULL);
        case MO_CORE_CHAR_OP_LOWER:
        case MO_CORE_CHAR_OP_UPPER:
            return sig(S(CHAR), 1, S(CHAR), NULL, NULL);
        case MO_CORE_CHAR_OP_TO_TEXT:
            return sig(S(TEXT), 1, S(CHAR), NULL, NULL);
        }
        break;
    case MO_CORE_PRIM_TEXT:
        switch (p->as.text) {
        case MO_CORE_TEXT_OP_CONCAT:
            return sig(S(TEXT), 2, S(TEXT), S(TEXT), NULL);
        case MO_CORE_TEXT_OP_BYTE_COUNT:
        case MO_CORE_TEXT_OP_CHAR_COUNT:
            return sig(S(INT), 1, S(TEXT), NULL, NULL);
        case MO_CORE_TEXT_OP_IS_EMPTY:
            return sig(S(BOOL), 1, S(TEXT), NULL, NULL);
        case MO_CORE_TEXT_OP_CHARS:
            return sig(L(S(CHAR)), 1, S(TEXT), NULL, NULL);
        case MO_CORE_TEXT_OP_FROM_CHARS:
            return sig(S(TEXT), 1, L(S(CHAR)), NULL, NULL);
        case MO_CORE_TEXT_OP_LINES:
        case MO_CORE_TEXT_OP_WORDS:
            return sig(L(S(TEXT)), 1, S(TEXT), NULL, NULL);
        case MO_CORE_TEXT_OP_SPLIT:
            return sig(L(S(TEXT)), 2, S(TEXT), S(TEXT), NULL);
        case MO_CORE_TEXT_OP_TRIM:
        case MO_CORE_TEXT_OP_TRIM_START:
        case MO_CORE_TEXT_OP_TRIM_END:
        case MO_CORE_TEXT_OP_LOWER:
        case MO_CORE_TEXT_OP_UPPER:
            return sig(S(TEXT), 1, S(TEXT), NULL, NULL);
        case MO_CORE_TEXT_OP_STARTS_WITH:
        case MO_CORE_TEXT_OP_ENDS_WITH:
        case MO_CORE_TEXT_OP_CONTAINS:
            return sig(S(BOOL), 2, S(TEXT), S(TEXT), NULL);
        case MO_CORE_TEXT_OP_DROP_PREFIX:
        case MO_CORE_TEXT_OP_DROP_SUFFIX:
            return sig(O(S(TEXT)), 2, S(TEXT), S(TEXT), NULL);
        case MO_CORE_TEXT_OP_REPLACE:
            return sig(S(TEXT), 3, S(TEXT), S(TEXT), S(TEXT));
        case MO_CORE_TEXT_OP_REPEAT:
        case MO_CORE_TEXT_OP_PAD_LEFT:
        case MO_CORE_TEXT_OP_PAD_RIGHT:
            return sig(S(TEXT), 2, S(TEXT), S(INT), NULL);
        case MO_CORE_TEXT_OP_TO_INT:
            return sig(O(S(INT)), 1, S(TEXT), NULL, NULL);
        case MO_CORE_TEXT_OP_TO_DECIMAL:
            return sig(O(S(DECIMAL)), 1, S(TEXT), NULL, NULL);
        case MO_CORE_TEXT_OP_TO_FLOAT:
            return sig(O(S(FLOAT)), 1, S(TEXT), NULL, NULL);
        case MO_CORE_TEXT_OP_JOIN:
            return sig(S(TEXT), 2, L(S(TEXT)), S(TEXT), NULL);
        case MO_CORE_TEXT_OP_BYTES:
            return sig(S(BYTES), 1, S(TEXT), NULL, NULL);
        }
        break;
    case MO_CORE_PRIM_BYTES:
        switch (p->as.bytes) {
        case MO_CORE_BYTES_OP_SIZE:
            return sig(S(INT), 1, S(BYTES), NULL, NULL);
        case MO_CORE_BYTES_OP_GET:
            return sig(O(S(BYTE)), 2, S(BYTES), S(INT), NULL);
        case MO_CORE_BYTES_OP_DECODE_UTF8:
            return sig(O(S(TEXT)), 1, S(BYTES), NULL, NULL);
        }
        break;
    case MO_CORE_PRIM_UNICODE16:
        switch (p->as.unicode16) {
        case MO_CORE_UNICODE_OP_GRAPHEMES:
            return sig(L(S(TEXT)), 1, S(TEXT), NULL, NULL);
        case MO_CORE_UNICODE_OP_DISPLAY_WIDTH:
            return sig(S(INT), 1, S(TEXT), NULL, NULL);
        case MO_CORE_UNICODE_OP_NORMALIZE_NFC:
        case MO_CORE_UNICODE_OP_LOWERCASE:
        case MO_CORE_UNICODE_OP_UPPERCASE:
            return sig(S(TEXT), 1, S(TEXT), NULL, NULL);
        case MO_CORE_UNICODE_OP_LETTER:
        case MO_CORE_UNICODE_OP_DIGIT:
            return sig(S(BOOL), 1, S(CHAR), NULL, NULL);
        }
        break;
    case MO_CORE_PRIM_RANGE:
        switch (p->as.range) {
        case MO_CORE_RANGE_OP_MAKE:
            return sig(S(RANGE), 2, S(INT), S(INT), NULL);
        case MO_CORE_RANGE_OP_START:
        case MO_CORE_RANGE_OP_END:
            return sig(S(INT), 1, S(RANGE), NULL, NULL);
        case MO_CORE_RANGE_OP_TO_LIST:
            return sig(L(S(INT)), 1, S(RANGE), NULL, NULL);
        case MO_CORE_RANGE_OP_CONTAINS:
            return sig(S(BOOL), 2, S(RANGE), S(INT), NULL);
        case MO_CORE_RANGE_OP_TO_TEXT:
            return sig(S(TEXT), 1, S(RANGE), NULL, NULL);
        }
        break;
    case MO_CORE_PRIM_TIME:
        switch (p->as.time) {
        case MO_CORE_TIME_OP_SUB:
            return sig(S(DURATION), 2, S(TIME), S(TIME), NULL);
        case MO_CORE_TIME_OP_ADD:
        case MO_CORE_TIME_OP_SUB_DUR:
            return sig(S(TIME), 2, S(TIME), S(DURATION), NULL);
        case MO_CORE_TIME_OP_CHECKED_ADD:
            return sig(O(S(TIME)), 2, S(TIME), S(DURATION), NULL);
        case MO_CORE_TIME_OP_TO_TEXT:
            return sig(S(TEXT), 1, S(TIME), NULL, NULL);
        case MO_CORE_TIME_OP_PARSE:
            return sig(O(S(TIME)), 1, S(TEXT), NULL, NULL);
        case MO_CORE_TIME_OP_DUR_ADD:
        case MO_CORE_TIME_OP_DUR_SUB:
            return sig(S(DURATION), 2, S(DURATION), S(DURATION), NULL);
        case MO_CORE_TIME_OP_DUR_MUL:
            return sig(S(DURATION), 2, S(DURATION), S(INT), NULL);
        case MO_CORE_TIME_OP_DUR_NEG:
            return sig(S(DURATION), 1, S(DURATION), NULL, NULL);
        case MO_CORE_TIME_OP_IN_MILLISECONDS:
            return sig(S(INT), 1, S(DURATION), NULL, NULL);
        case MO_CORE_TIME_OP_IN_SECONDS:
            return sig(S(DECIMAL), 1, S(DURATION), NULL, NULL);
        case MO_CORE_TIME_OP_DUR_TO_TEXT:
            return sig(S(TEXT), 1, S(DURATION), NULL, NULL);
        }
        break;
    case MO_CORE_PRIM_LIST:
        t = annotation(c, p->as.list.t, 0);
        collection = L(t);
        switch (p->as.list.op.tag) {
        case MO_CORE_LIST_OP_LIT: {
            size_t n;
            struct signature s = sig(collection, argc + 1, t, NULL, NULL);
            s.repeated = true;
            if (natural(p->as.list.op.as.lit, argc, &n) && n == argc)
                s.count = argc;
            return s;
        }
        case MO_CORE_LIST_OP_SIZE:
            return sig(S(INT), 1, collection, NULL, NULL);
        case MO_CORE_LIST_OP_IS_EMPTY:
            return sig(S(BOOL), 1, collection, NULL, NULL);
        case MO_CORE_LIST_OP_GET:
            return sig(O(t), 2, collection, S(INT), NULL);
        case MO_CORE_LIST_OP_FIRST:
        case MO_CORE_LIST_OP_LAST:
            return sig(O(t), 1, collection, NULL, NULL);
        case MO_CORE_LIST_OP_APPEND:
        case MO_CORE_LIST_OP_PREPEND:
            return sig(collection, 2, collection, t, NULL);
        case MO_CORE_LIST_OP_CONCAT:
            return sig(collection, 2, collection, collection, NULL);
        case MO_CORE_LIST_OP_REVERSE:
        case MO_CORE_LIST_OP_UNIQUE:
            return sig(collection, 1, collection, NULL, NULL);
        case MO_CORE_LIST_OP_TAKE:
        case MO_CORE_LIST_OP_DROP:
            return sig(collection, 2, collection, S(INT), NULL);
        case MO_CORE_LIST_OP_SET:
            return sig(O(collection), 3, collection, S(INT), t);
        case MO_CORE_LIST_OP_REMOVE_AT:
            return sig(O(collection), 2, collection, S(INT), NULL);
        case MO_CORE_LIST_OP_UNCONS:
            return sig(O(binary(c, MO_CORE_TY_TUPLE, t, collection)), 1, collection, NULL, NULL);
        case MO_CORE_LIST_OP_ENUMERATE:
            return sig(L(binary(c, MO_CORE_TY_TUPLE, S(INT), t)), 1, collection, NULL, NULL);
        case MO_CORE_LIST_OP_CONTAINS:
            return sig(S(BOOL), 2, collection, t, NULL);
        case MO_CORE_LIST_OP_ZIP:
            v = annotation(c, p->as.list.op.as.zip, 0);
            return sig(L(binary(c, MO_CORE_TY_TUPLE, t, v)), 2, collection, L(v), NULL);
        }
        break;
    case MO_CORE_PRIM_MAP:
        k = annotation(c, p->as.map.k, 0);
        v = annotation(c, p->as.map.v, 0);
        collection = binary(c, MO_CORE_TY_MAP, k, v);
        switch (p->as.map.op.tag) {
        case MO_CORE_MAP_OP_LIT: {
            size_t n;
            struct signature s = sig(collection, argc + 1, k, v, NULL);
            s.repeated = true;
            if (natural(p->as.map.op.lit, argc / 2, &n) && argc % 2 == 0 && n == argc / 2)
                s.count = argc;
            return s;
        }
        case MO_CORE_MAP_OP_EMPTY:
            return sig(collection, 0, NULL, NULL, NULL);
        case MO_CORE_MAP_OP_SIZE:
            return sig(S(INT), 1, collection, NULL, NULL);
        case MO_CORE_MAP_OP_IS_EMPTY:
            return sig(S(BOOL), 1, collection, NULL, NULL);
        case MO_CORE_MAP_OP_GET:
            return sig(O(v), 2, collection, k, NULL);
        case MO_CORE_MAP_OP_HAS:
            return sig(S(BOOL), 2, collection, k, NULL);
        case MO_CORE_MAP_OP_SET:
            return sig(collection, 3, collection, k, v);
        case MO_CORE_MAP_OP_REMOVE:
            return sig(collection, 2, collection, k, NULL);
        case MO_CORE_MAP_OP_KEYS:
            return sig(L(k), 1, collection, NULL, NULL);
        case MO_CORE_MAP_OP_VALUES:
            return sig(L(v), 1, collection, NULL, NULL);
        case MO_CORE_MAP_OP_ENTRIES:
            return sig(L(binary(c, MO_CORE_TY_TUPLE, k, v)), 1, collection, NULL, NULL);
        case MO_CORE_MAP_OP_OF_LIST:
            return sig(collection, 1, L(binary(c, MO_CORE_TY_TUPLE, k, v)), NULL, NULL);
        }
        break;
    case MO_CORE_PRIM_SET:
        t = annotation(c, p->as.set.t, 0);
        collection = unary(c, MO_CORE_TY_SET, t);
        switch (p->as.set.op) {
        case MO_CORE_SET_OP_EMPTY:
            return sig(collection, 0, NULL, NULL, NULL);
        case MO_CORE_SET_OP_SIZE:
            return sig(S(INT), 1, collection, NULL, NULL);
        case MO_CORE_SET_OP_IS_EMPTY:
            return sig(S(BOOL), 1, collection, NULL, NULL);
        case MO_CORE_SET_OP_HAS:
            return sig(S(BOOL), 2, collection, t, NULL);
        case MO_CORE_SET_OP_ADD:
        case MO_CORE_SET_OP_REMOVE:
            return sig(collection, 2, collection, t, NULL);
        case MO_CORE_SET_OP_UNION:
        case MO_CORE_SET_OP_INTERSECTION:
        case MO_CORE_SET_OP_DIFFERENCE:
            return sig(collection, 2, collection, collection, NULL);
        case MO_CORE_SET_OP_TO_LIST:
            return sig(L(t), 1, collection, NULL, NULL);
        case MO_CORE_SET_OP_OF_LIST:
            return sig(collection, 1, L(t), NULL, NULL);
        }
        break;
    case MO_CORE_PRIM_EQ:
        t = annotation(c, p->as.eq, 0);
        return sig(S(BOOL), 2, t, t, NULL);
    case MO_CORE_PRIM_STRUCTURAL:
        t = annotation(c, p->as.structural, 0);
        return sig(S(TEXT), 1, t, NULL, NULL);
    case MO_CORE_PRIM_IO: {
        struct type *files = p->as.io.ro ? C(READ_FILES) : C(FILES);
        struct type *error = S(FILE_ERROR);
        struct signature s = {0};
        switch (p->as.io.tag) {
        case MO_CORE_IO_OP_ARGS_LIST:
            return sig(L(S(TEXT)), 1, C(ARGS), NULL, NULL);
        case MO_CORE_IO_OP_ENV_GET:
            return sig(O(S(TEXT)), 2, C(ENV), S(TEXT), NULL);
        case MO_CORE_IO_OP_PRINT:
        case MO_CORE_IO_OP_PRINT_ERROR:
            return sig(S(UNIT), 2, C(CONSOLE), S(TEXT), NULL);
        case MO_CORE_IO_OP_READ_LINE:
            return sig(O(S(TEXT)), 1, C(CONSOLE), NULL, NULL);
        case MO_CORE_IO_OP_PRINTED:
        case MO_CORE_IO_OP_PRINTED_ERRORS:
            s = sig(L(S(TEXT)), 1, C(CONSOLE), NULL, NULL);
            break;
        case MO_CORE_IO_OP_READ_TEXT:
            return sig(binary(c, MO_CORE_TY_RESULT, S(TEXT), error), 2, files, S(TEXT), NULL);
        case MO_CORE_IO_OP_READ_BYTES:
            return sig(binary(c, MO_CORE_TY_RESULT, S(BYTES), error), 2, files, S(TEXT), NULL);
        case MO_CORE_IO_OP_WRITE_TEXT:
        case MO_CORE_IO_OP_APPEND_TEXT:
            return sig(binary(c, MO_CORE_TY_RESULT, S(UNIT), error), 3, C(FILES), S(TEXT), S(TEXT));
        case MO_CORE_IO_OP_REMOVE:
        case MO_CORE_IO_OP_MAKE_FOLDER:
            return sig(binary(c, MO_CORE_TY_RESULT, S(UNIT), error), 2, C(FILES), S(TEXT), NULL);
        case MO_CORE_IO_OP_LIST:
            return sig(binary(c, MO_CORE_TY_RESULT, L(S(TEXT)), error), 2, files, S(TEXT), NULL);
        case MO_CORE_IO_OP_ENTRIES:
            return sig(binary(c, MO_CORE_TY_RESULT, L(S(ENTRY)), error), 2, files, S(TEXT), NULL);
        case MO_CORE_IO_OP_EXISTS:
            return sig(S(BOOL), 2, files, S(TEXT), NULL);
        case MO_CORE_IO_OP_WITHIN:
            return sig(files, 2, files, S(TEXT), NULL);
        case MO_CORE_IO_OP_READ_ONLY:
            return sig(C(READ_FILES), 1, C(FILES), NULL, NULL);
        case MO_CORE_IO_OP_NOW:
            return sig(S(TIME), 1, C(CLOCK), NULL, NULL);
        case MO_CORE_IO_OP_RANDOM_INT:
            return sig(S(INT), 3, C(RANDOM), S(INT), S(INT));
        case MO_CORE_IO_OP_CONSOLE_FAKE:
            s = sig(C(CONSOLE), 0, NULL, NULL, NULL);
            break;
        case MO_CORE_IO_OP_CONSOLE_FAKE_INPUT:
            s = sig(C(CONSOLE), 1, L(S(TEXT)), NULL, NULL);
            break;
        case MO_CORE_IO_OP_FILES_IN_MEMORY:
            s = sig(C(FILES), 1, binary(c, MO_CORE_TY_MAP, S(TEXT), S(TEXT)), NULL, NULL);
            break;
        case MO_CORE_IO_OP_CLOCK_FIXED:
            s = sig(C(CLOCK), 1, S(TIME), NULL, NULL);
            break;
        case MO_CORE_IO_OP_RANDOM_SEEDED:
            s = sig(C(RANDOM), 1, S(INT), NULL, NULL);
            break;
        case MO_CORE_IO_OP_ARGS_OF:
            s = sig(C(ARGS), 1, L(S(TEXT)), NULL, NULL);
            break;
        case MO_CORE_IO_OP_ENV_OF:
            s = sig(C(ENV), 1, binary(c, MO_CORE_TY_MAP, S(TEXT), S(TEXT)), NULL, NULL);
            break;
        }
        s.spec = true;
        return s;
    }
    }
    MO_UNREACHABLE();
}
#undef S
#undef O
#undef L
#undef C

static MO_MUST_USE struct expr *prepare(struct checker *c, mo_core_expr e, size_t depth,
                                        size_t *node);
static MO_MUST_USE bool prepare_list(struct checker *c, struct expr *p, size_t offset,
                                     mo_core_expr_list xs, size_t depth, size_t *node) {
    for (size_t i = 0; i < xs.count; i++) {
        p->children[offset + i] = prepare(c, xs.items[i], depth, node);
        if (p->children[offset + i] == NULL)
            return false;
        p->children[offset + i]->parent = p;
    }
    return true;
}
static MO_MUST_USE struct expr *prepare(struct checker *c, mo_core_expr e, size_t depth,
                                        size_t *node) {
    if (e == NULL || depth >= MO_LIMIT_CORE_DEPTH || ++c->nodes > MO_LIMIT_CORE_NODES) {
        c->fatal = MO_CODE_CORE_CHECK_LIMIT;
        return NULL;
    }
    struct expr *p = allocate(c, &c->body, 1, sizeof *p);
    if (p == NULL)
        return NULL;
    p->source = e;
    p->node = (*node)++;
    mo_core_expr small[3] = {0};
    mo_core_expr_list xs = {0};
    size_t n = 0;
    switch (e->tag) {
    case MO_CORE_EXPR_LIT:
        p->annotation = literal(c, e->as.lit);
        break;
    case MO_CORE_EXPR_VAR:
    case MO_CORE_EXPR_GLOBAL:
    case MO_CORE_EXPR_STOP:
        break;
    case MO_CORE_EXPR_LET:
        small[n++] = e->as.let_.bound;
        small[n++] = e->as.let_.body;
        break;
    case MO_CORE_EXPR_LAM:
        p->params = parameter_types(c, &c->body, e->as.lam.params);
        small[n++] = e->as.lam.body;
        break;
    case MO_CORE_EXPR_APP:
        small[n++] = e->as.app.f;
        xs = e->as.app.args;
        break;
    case MO_CORE_EXPR_CALL:
        xs = e->as.call.args;
        break;
    case MO_CORE_EXPR_TUPLE:
        xs = e->as.tuple;
        break;
    case MO_CORE_EXPR_PROJ:
        small[n++] = e->as.proj.expr;
        break;
    case MO_CORE_EXPR_CON:
        p->annotation = annotation(c, e->as.con.ty, 0);
        xs = e->as.con.args;
        break;
    case MO_CORE_EXPR_CASE:
        small[n++] = e->as.case_.expr;
        break;
    case MO_CORE_EXPR_IF:
        small[n++] = e->as.if_.cond;
        small[n++] = e->as.if_.then_;
        small[n++] = e->as.if_.else_;
        break;
    case MO_CORE_EXPR_PRIM:
        xs = e->as.prim.args;
        p->signature = allocate(c, &c->body, 1, sizeof *p->signature);
        if (p->signature == NULL)
            return NULL;
        *p->signature = primitive(c, &e->as.prim.prim, xs.count);
        break;
    case MO_CORE_EXPR_FAIL:
        small[n++] = e->as.fail;
        break;
    case MO_CORE_EXPR_JOIN:
        p->params = parameter_types(c, &c->body, e->as.join.params);
        small[n++] = e->as.join.body;
        small[n++] = e->as.join.rest;
        break;
    case MO_CORE_EXPR_JUMP:
        xs = e->as.jump.args;
        break;
    }
    if (c->fatal != 0)
        return NULL;
    size_t arms = e->tag == MO_CORE_EXPR_CASE ? e->as.case_.arms.count : 0;
    p->count = n + xs.count + arms;
    p->children = p->count == 0 ? NULL : allocate(c, &c->body, p->count, sizeof *p->children);
    if (p->count != 0 && p->children == NULL)
        return NULL;
    if (!prepare_list(c, p, 0, (mo_core_expr_list){small, n}, depth + 1, node) ||
        !prepare_list(c, p, n, xs, depth + 1, node))
        return NULL;
    for (size_t i = 0; i < arms; i++) {
        p->children[n + i] = prepare(c, e->as.case_.arms.items[i].body, depth + 1, node);
        if (p->children[n + i] == NULL)
            return NULL;
        p->children[n + i]->parent = p;
    }
    return p;
}
static MO_MUST_USE mo_core_string text(const char *s) {
    return (mo_core_string){s, strlen(s)};
}
static MO_MUST_USE struct data *constructors(struct checker *c, struct type *t) {
    if (t->resolved)
        return t->constructors;
    t->resolved = true;
    if (t->tag == MO_CORE_TY_DATA) {
        t->constructors = find(c->data, t->name);
        return t->constructors;
    }
    struct data *d;
    const char *names[4] = {0};
    struct type *fields[4][2] = {{0}};
    size_t counts[4] = {0}, n = 0;
    if (t->tag == MO_CORE_TY_OPTION) {
        n = 2;
        names[0] = "None";
        names[1] = "Some";
        counts[1] = 1;
        fields[1][0] = t->parts[0];
    } else if (t->tag == MO_CORE_TY_RESULT) {
        n = 2;
        names[0] = "Ok";
        names[1] = "Error";
        counts[0] = counts[1] = 1;
        fields[0][0] = t->parts[0];
        fields[1][0] = t->parts[1];
    } else if (t->tag == MO_CORE_TY_ORDERING) {
        n = 3;
        names[0] = "Less";
        names[1] = "Equal";
        names[2] = "Greater";
    } else if (t->tag == MO_CORE_TY_FILE_ERROR) {
        n = 4;
        names[0] = "NotFound";
        names[1] = "NotAllowed";
        names[2] = "NotText";
        names[3] = "Failed";
        for (size_t i = 0; i < n; i++) {
            counts[i] = 1;
            fields[i][0] = scalar(c, MO_CORE_TY_TEXT);
        }
        counts[3] = 2;
        fields[3][1] = fields[3][0];
    } else if (t->tag == MO_CORE_TY_SIMULATION_ERROR) {
        n = 2;
        names[0] = "ServiceDown";
        names[1] = "ToolFailed";
    } else if (t->tag == MO_CORE_TY_ENTRY) {
        n = 1;
        names[0] = "Entry";
        counts[0] = 2;
        fields[0][0] = scalar(c, MO_CORE_TY_TEXT);
        fields[0][1] = scalar(c, MO_CORE_TY_ENTRY_KIND);
    } else if (t->tag == MO_CORE_TY_ENTRY_KIND) {
        n = 4;
        names[0] = "File";
        names[1] = "Folder";
        names[2] = "Link";
        names[3] = "Other";
    } else {
        return NULL;
    }
    d = allocate(c, &c->arena, 1, sizeof *d);
    if (d == NULL)
        return NULL;
    d->count = n;
    d->ctors = allocate(c, &c->arena, n, sizeof *d->ctors);
    if (d->ctors == NULL)
        return NULL;
    for (size_t i = 0; i < n; i++) {
        struct constructor *k = &d->ctors[i];
        k->name = text(names[i]);
        k->count = counts[i];
        k->fields = allocate(c, &c->arena, k->count, sizeof *k->fields);
        if (k->fields == NULL)
            return NULL;
        for (size_t j = 0; j < k->count; j++) {
            k->fields[j] = fields[i][j];
            if (k->fields[j] == NULL)
                return NULL;
        }
        if (!put(c, &c->arena, &d->names, k->name, k, false))
            return NULL;
    }
    t->constructors = d;
    return d;
}
static MO_MUST_USE void *lookup(const struct context *ctx, mo_core_string name) {
    for (; ctx != NULL; ctx = ctx->outer) {
        void *value = find(ctx->names, name);
        if (value != NULL)
            return value;
    }
    return NULL;
}
static MO_MUST_USE bool bind_params(struct checker *c, struct context *ctx, mo_core_params ps,
                                    struct type **types) {
    for (size_t i = 0; i < ps.count; i++)
        if (!put(c, &c->body, &ctx->names, ps.items[i].name, types[i], false))
            return false;
    return true;
}
static MO_MUST_USE struct answer bad(const struct expr *e, uint16_t code) {
    return (struct answer){.code = code, .at = e};
}
static MO_MUST_USE struct answer expect(const struct expr *e, struct type *exp, struct type *t) {
    if (t == NULL)
        return bad(e, MO_CODE_CORE_CHECK_INFERENCE);
    if (exp != NULL && exp != t)
        return (struct answer){.ty = t, .expected = exp, .at = e, .code = MO_CODE_CORE_CHECK_TYPE};
    return (struct answer){.ty = t};
}
static MO_MUST_USE struct answer tc(struct checker *c, struct expr *e, const struct context *ctx,
                                    const struct context *joins, struct type *exp);
static MO_MUST_USE struct answer args(struct checker *c, struct expr *e, size_t offset,
                                      const struct context *ctx, struct type **ts, size_t n) {
    if (e->count - offset != n)
        return bad(e, MO_CODE_CORE_CHECK_ARGUMENTS);
    for (size_t i = 0; i < n; i++) {
        struct answer a = tc(c, e->children[offset + i], ctx, NULL, ts[i]);
        if (a.code != 0)
            return a;
    }
    return (struct answer){0};
}
static MO_MUST_USE struct answer arm(struct checker *c, struct expr *e, size_t i,
                                     const struct context *ctx, const struct context *joins,
                                     const struct constructor *k, struct type *exp) {
    struct mo_core_arm *a = &e->source->as.case_.arms.items[i];
    if (compare(a->ctor, k->name) != 0 || a->names.count != k->count)
        return bad(e, MO_CODE_CORE_CHECK_ARM);
    struct expr *body = e->children[i + 1];
    if (!body->arm_bound) {
        for (size_t j = 0; j < k->count; j++)
            if (!put(c, &c->body, &body->arm_bindings, a->names.items[j], k->fields[j], false))
                return bad(e, c->fatal);
        body->arm_bound = true;
    }
    struct context local = {.names = body->arm_bindings, .outer = ctx};
    return tc(c, body, &local, joins, exp);
}
static MO_MUST_USE struct answer branches(struct checker *c, struct expr *e,
                                          const struct context *ctx, const struct context *joins,
                                          struct type *exp, size_t first, size_t second) {
    if (exp != NULL) {
        struct answer a = tc(c, e->children[first], ctx, joins, exp);
        return a.code != 0 ? a : tc(c, e->children[second], ctx, joins, exp);
    }
    struct answer a = tc(c, e->children[first], ctx, joins, NULL);
    if (a.code == 0)
        return tc(c, e->children[second], ctx, joins, a.ty);
    struct answer b = tc(c, e->children[second], ctx, joins, NULL);
    return b.code != 0 ? b : tc(c, e->children[first], ctx, joins, b.ty);
}
static MO_MUST_USE struct answer tc_uncached(struct checker *c, struct expr *e,
                                             const struct context *ctx, const struct context *joins,
                                             struct type *exp) {
    mo_core_expr s = e->source;
    struct answer a = {0};
    switch (s->tag) {
    case MO_CORE_EXPR_LIT:
        return e->annotation == NULL ? bad(e, MO_CODE_CORE_CHECK_LITERAL)
                                     : expect(e, exp, e->annotation);
    case MO_CORE_EXPR_VAR: {
        struct type *t = lookup(ctx, s->as.var);
        return t == NULL ? bad(e, MO_CODE_CORE_CHECK_VARIABLE) : expect(e, exp, t);
    }
    case MO_CORE_EXPR_GLOBAL: {
        const struct constant *k = find(c->consts, s->as.global);
        return k == NULL || k->index >= c->scope ? bad(e, MO_CODE_CORE_CHECK_GLOBAL)
                                                 : expect(e, exp, k->ty);
    }
    case MO_CORE_EXPR_LET: {
        a = tc(c, e->children[0], ctx, NULL, NULL);
        if (a.code != 0)
            return a;
        if (!e->bound) {
            if (!put(c, &c->body, &e->bindings, s->as.let_.name, a.ty, false))
                return bad(e, c->fatal);
            e->bound = true;
        }
        struct context local = {.names = e->bindings, .outer = ctx};
        return tc(c, e->children[1], &local, joins, exp);
    }
    case MO_CORE_EXPR_LAM: {
        size_t n = s->as.lam.params.count;
        if (exp != NULL) {
            if (exp->tag != MO_CORE_TY_FN || exp->count != n + 1)
                return bad(e, MO_CODE_CORE_CHECK_LAMBDA);
            for (size_t i = 0; i < n; i++)
                if (e->params[i] != exp->parts[i])
                    return bad(e, MO_CODE_CORE_CHECK_LAMBDA);
        }
        struct context local = {.names = e->bindings, .outer = ctx};
        if (!e->bound) {
            if (!bind_params(c, &local, s->as.lam.params, e->params))
                return bad(e, c->fatal);
            e->bindings = local.names;
            e->bound = true;
        }
        a = tc(c, e->children[0], &local, NULL, exp == NULL ? NULL : exp->parts[n]);
        if (a.code != 0)
            return a;
        if (exp != NULL)
            return expect(e, exp, exp);
        struct type **parts = allocate(c, &c->body, n + 1, sizeof *parts);
        if (parts == NULL)
            return bad(e, c->fatal);
        for (size_t i = 0; i < n; i++)
            parts[i] = e->params[i];
        parts[n] = a.ty;
        return expect(
            e, NULL,
            intern(c, MO_CORE_TY_FN, parts, n + 1, (mo_core_string){0}, MO_CORE_CAP_CONSOLE));
    }
    case MO_CORE_EXPR_APP:
        a = tc(c, e->children[0], ctx, NULL, NULL);
        if (a.code != 0)
            return a;
        if (a.ty->tag != MO_CORE_TY_FN)
            return bad(e, MO_CODE_CORE_CHECK_APPLICATION);
        {
            struct type *f = a.ty;
            a = args(c, e, 1, ctx, f->parts, f->count - 1);
            return a.code != 0 ? a : expect(e, exp, f->parts[f->count - 1]);
        }
    case MO_CORE_EXPR_CALL: {
        const struct function *f = find(c->funs, s->as.call.name);
        if (f == NULL)
            return bad(e, MO_CODE_CORE_CHECK_FUNCTION);
        if (f->scope > c->scope)
            return bad(e, MO_CODE_CORE_CHECK_CALL_PREFIX);
        if (!c->spec && f->source->kind != MO_CORE_FUNCTION)
            return bad(e, MO_CODE_CORE_CHECK_SPEC_CALL);
        a = args(c, e, 0, ctx, f->params, f->source->params.count);
        return a.code != 0 ? a : expect(e, exp, f->ret);
    }
    case MO_CORE_EXPR_TUPLE: {
        struct type **ts = allocate(c, &c->body, e->count, sizeof *ts);
        if (ts == NULL)
            return bad(e, c->fatal);
        for (size_t i = 0; i < e->count; i++) {
            a = tc(c, e->children[i], ctx, NULL, NULL);
            if (a.code != 0)
                return a;
            ts[i] = a.ty;
        }
        return expect(
            e, exp,
            intern(c, MO_CORE_TY_TUPLE, ts, e->count, (mo_core_string){0}, MO_CORE_CAP_CONSOLE));
    }
    case MO_CORE_EXPR_PROJ: {
        a = tc(c, e->children[0], ctx, NULL, NULL);
        if (a.code != 0)
            return a;
        size_t i;
        if (a.ty->tag != MO_CORE_TY_TUPLE || a.ty->count == 0 ||
            !natural(s->as.proj.index, a.ty->count - 1, &i))
            return bad(e, MO_CODE_CORE_CHECK_PROJECTION);
        return expect(e, exp, a.ty->parts[i]);
    }
    case MO_CORE_EXPR_CON: {
        struct data *d = constructors(c, e->annotation);
        if (d == NULL)
            return bad(e, MO_CODE_CORE_CHECK_CONSTRUCTORS);
        const struct constructor *k = find(d->names, s->as.con.ctor);
        if (k == NULL)
            return bad(e, MO_CODE_CORE_CHECK_CONSTRUCTOR);
        a = args(c, e, 0, ctx, k->fields, k->count);
        return a.code != 0 ? a : expect(e, exp, e->annotation);
    }
    case MO_CORE_EXPR_CASE: {
        a = tc(c, e->children[0], ctx, NULL, NULL);
        if (a.code != 0)
            return a;
        struct data *d = constructors(c, a.ty);
        if (d == NULL)
            return bad(e, MO_CODE_CORE_CHECK_CONSTRUCTORS);
        if (s->as.case_.arms.count != d->count)
            return bad(e, MO_CODE_CORE_CHECK_ARMS);
        /* Unlike recursive tcArms, each failed inference is visited once before a later arm
         * supplies the type; then check all arms at that type. Memoization preserves errors. */
        if (exp == NULL) {
            for (size_t i = 0; i < d->count; i++) {
                const struct mo_core_arm *wire = &s->as.case_.arms.items[i];
                if (compare(wire->ctor, d->ctors[i].name) != 0 ||
                    wire->names.count != d->ctors[i].count)
                    return bad(e, MO_CODE_CORE_CHECK_ARM);
                a = arm(c, e, i, ctx, joins, &d->ctors[i], NULL);
                if (a.code == 0) {
                    exp = a.ty;
                    break;
                }
            }
            if (exp == NULL)
                return d->count == 0 ? bad(e, MO_CODE_CORE_CHECK_INFERENCE) : a;
        }
        for (size_t i = 0; i < d->count; i++) {
            a = arm(c, e, i, ctx, joins, &d->ctors[i], exp);
            if (a.code != 0)
                return a;
        }
        return expect(e, exp, exp);
    }
    case MO_CORE_EXPR_IF:
        a = tc(c, e->children[0], ctx, NULL, scalar(c, MO_CORE_TY_BOOL));
        return a.code != 0 ? a : branches(c, e, ctx, joins, exp, 1, 2);
    case MO_CORE_EXPR_PRIM: {
        const struct signature *p = e->signature;
        if (p->spec && !c->spec)
            return bad(e, MO_CODE_CORE_CHECK_SPEC_PRIMITIVE);
        if (p->count != e->count)
            return bad(e, MO_CODE_CORE_CHECK_ARGUMENTS);
        for (size_t i = 0; i < e->count; i++) {
            struct type *t = p->selected != NULL ? p->selected[i]
                             : p->repeated       ? p->args[p->args[1] == NULL ? 0 : i % 2]
                                                 : p->args[i];
            a = tc(c, e->children[i], ctx, NULL, t);
            if (a.code != 0)
                return a;
        }
        return expect(e, exp, p->ret);
    }
    case MO_CORE_EXPR_STOP:
        return expect(e, exp, exp);
    case MO_CORE_EXPR_FAIL:
        if (!c->spec)
            return bad(e, MO_CODE_CORE_CHECK_FAIL);
        a = tc(c, e->children[0], ctx, NULL, scalar(c, MO_CORE_TY_TEXT));
        return a.code != 0 ? a : expect(e, exp, exp);
    case MO_CORE_EXPR_JOIN: {
        struct context local = {.names = e->bindings, .outer = ctx};
        if (!e->bound) {
            if (!bind_params(c, &local, s->as.join.params, e->params) ||
                !put(c, &c->body, &e->joins, s->as.join.name, e, false))
                return bad(e, c->fatal);
            e->bindings = local.names;
            e->bound = true;
        }
        struct context j = {.names = e->joins, .outer = joins};
        if (exp != NULL) {
            a = tc(c, e->children[0], &local, &j, exp);
            return a.code != 0 ? a : tc(c, e->children[1], ctx, &j, exp);
        }
        a = tc(c, e->children[1], ctx, &j, NULL);
        if (a.code == 0)
            return tc(c, e->children[0], &local, &j, a.ty);
        a = tc(c, e->children[0], &local, &j, NULL);
        return a.code != 0 ? a : tc(c, e->children[1], ctx, &j, a.ty);
    }
    case MO_CORE_EXPR_JUMP: {
        const struct expr *j = lookup(joins, s->as.jump.name);
        if (j == NULL)
            return bad(e, MO_CODE_CORE_CHECK_JUMP);
        a = args(c, e, 0, ctx, j->params, j->source->as.join.params.count);
        return a.code != 0 ? a : expect(e, exp, exp);
    }
    }
    MO_UNREACHABLE();
}
static MO_MUST_USE struct answer tc(struct checker *c, struct expr *e, const struct context *ctx,
                                    const struct context *joins, struct type *exp) {
    if (c->fatal != 0)
        return bad(e, c->fatal);
    /* Each prepared occurrence has fixed bindings, joins, spec mode and constant prefix.
     * Only its expected type varies when inference retries an if, case or join. */
    size_t id = exp == NULL ? 0 : exp->id;
    for (unsigned i = 0; i < e->memo_count; i++)
        if (e->memo[i].expected == id)
            return e->memo[i].answer;
    for (struct memo *m = e->extra; m != NULL; m = m->next)
        if (m->expected == id)
            return m->answer;
    struct answer a = tc_uncached(c, e, ctx, joins, exp);
    if (c->fatal != 0)
        return bad(e, c->fatal);
    struct memo *saved =
        e->memo_count < 2 ? &e->memo[e->memo_count++] : allocate(c, &c->body, 1, sizeof *saved);
    if (saved == NULL)
        return bad(e, c->fatal);
    *saved = (struct memo){.expected = id, .answer = a};
    if (saved != &e->memo[0] && saved != &e->memo[1]) {
        saved->next = e->extra;
        e->extra = saved;
    }
    return a;
}

static MO_MUST_USE const char *reason(uint16_t code) {
    switch (code) {
    case MO_CODE_CORE_CHECK_METADATA:
        return "Compiler metadata has an invalid shape.";
    case MO_CODE_CORE_CHECK_CONSTANT_NAME:
        return "Two Core constants have the same lookup name.";
    case MO_CODE_CORE_CHECK_ENTRY:
        return "The Core entry function does not exist.";
    case MO_CODE_CORE_CHECK_ENTRY_KIND:
        return "The Core entry names spec code rather than an ordinary function.";
    case MO_CODE_CORE_CHECK_KIND:
        return "A Core spec function has the wrong parameter or result signature.";
    case MO_CODE_CORE_CHECK_PREFIX:
        return "A Core function requires more constants than the program declares.";
    case MO_CODE_CORE_CHECK_LITERAL:
        return "A Core literal is outside its type's numeric range.";
    case MO_CODE_CORE_CHECK_VARIABLE:
        return "A Core variable has no lexical binding.";
    case MO_CODE_CORE_CHECK_GLOBAL:
        return "A Core constant is absent or outside the available prefix.";
    case MO_CODE_CORE_CHECK_TYPE:
        return "The Core expression's type differs from its expected type.";
    case MO_CODE_CORE_CHECK_ARGUMENTS:
        return "A Core application, call, jump, constructor or primitive has the wrong argument "
               "count.";
    case MO_CODE_CORE_CHECK_LAMBDA:
        return "A Core lambda's parameter signature differs from the expected function type.";
    case MO_CODE_CORE_CHECK_APPLICATION:
        return "Core applies an expression whose inferred type is not a function.";
    case MO_CODE_CORE_CHECK_FUNCTION:
        return "A Core call names a function that does not exist.";
    case MO_CODE_CORE_CHECK_CALL_PREFIX:
        return "A Core call requires constants outside the caller's available prefix.";
    case MO_CODE_CORE_CHECK_SPEC_CALL:
        return "Ordinary Core code calls a spec function.";
    case MO_CODE_CORE_CHECK_PROJECTION:
        return "Core projects from a non-tuple or beyond the tuple's parts.";
    case MO_CODE_CORE_CHECK_CONSTRUCTORS:
        return "Core constructs or cases on a non-choice type or an undeclared data type.";
    case MO_CODE_CORE_CHECK_CONSTRUCTOR:
        return "The Core type has no constructor with this name.";
    case MO_CODE_CORE_CHECK_ARMS:
        return "A Core case does not have exactly one arm per constructor.";
    case MO_CODE_CORE_CHECK_ARM:
        return "A Core case arm has the wrong constructor order or field-binding count.";
    case MO_CODE_CORE_CHECK_SPEC_PRIMITIVE:
        return "Ordinary Core code allocates a fake or observes console output.";
    case MO_CODE_CORE_CHECK_INFERENCE:
        return "Core cannot infer a type here; stop, fail, jump or an empty case needs an expected "
               "type.";
    case MO_CODE_CORE_CHECK_FAIL:
        return "Ordinary Core code contains fail.";
    case MO_CODE_CORE_CHECK_JUMP:
        return "A Core jump has no in-scope join point in this tail position.";
    case MO_CODE_CORE_CHECK_LIMIT:
        return "Core checking exceeds its node or nesting limit.";
    case MO_CODE_CORE_CHECK_MEMORY:
        return "Core checking exhausts its bounded scratch arena or system memory.";
    default:
        MO_UNREACHABLE();
    }
}
/* Names and type trees in labels are elided, not allowed to amplify hostile input. */
static MO_MUST_USE bool label_name(struct mo_buffer *b, mo_core_string name) {
    size_t n = name.length < 128 ? name.length : 128;
    while (n < name.length && n != 0 && ((uint8_t)name.bytes[n] & 0xc0u) == 0x80u)
        n--;
    if (!mo_buffer_append_byte(b, '"'))
        return false;
    for (size_t i = 0; i < n; i++) {
        uint8_t ch = (uint8_t)name.bytes[i];
        if (ch < 32 || ch == 127) {
            if (!mo_buffer_append_text(b, "\\x") || !mo_buffer_append_hex(b, ch, 2))
                return false;
        } else {
            if ((ch == '"' || ch == '\\') && !mo_buffer_append_byte(b, '\\'))
                return false;
            if (!mo_buffer_append_byte(b, ch))
                return false;
        }
    }
    return (n == name.length || mo_buffer_append_text(b, "...")) && mo_buffer_append_byte(b, '"');
}
static MO_MUST_USE bool label_type(struct mo_buffer *b, const struct type *t, size_t *remaining) {
    static const char *const names[] = {
        "Int",       "BigInt", "Byte",      "Decimal", "Float",  "Bool",       "Char",
        "Text",      "Bytes",  "Unit",      "Range",   "Time",   "Duration",   "Tuple",
        "List",      "Map",    "Set",       "Data",    "Option", "Result",     "Ordering",
        "FileError", "Entry",  "EntryKind", "Fn",      "Cap",    "Simulation", "SimulationError"};
    static const char *const caps[] = {"Console", "Files", "ReadFiles", "Clock",
                                       "Random",  "Args",  "Env"};
    if (*remaining == 0)
        return mo_buffer_append_text(b, "...");
    (*remaining)--;
    if (!mo_buffer_append_text(b, names[t->tag]))
        return false;
    if (t->tag == MO_CORE_TY_DATA)
        return mo_buffer_append_byte(b, '(') && label_name(b, t->name) &&
               mo_buffer_append_byte(b, ')');
    if (t->tag == MO_CORE_TY_CAP)
        return mo_buffer_append_byte(b, '(') && mo_buffer_append_text(b, caps[t->cap]) &&
               mo_buffer_append_byte(b, ')');
    if (t->count == 0)
        return true;
    if (!mo_buffer_append_byte(b, '('))
        return false;
    for (size_t i = 0; i < t->count; i++) {
        if (i != 0 && !mo_buffer_append_text(b, ", "))
            return false;
        if (!label_type(b, t->parts[i], remaining))
            return false;
        if (*remaining == 0) {
            if (i + 1 < t->count && !mo_buffer_append_text(b, ", ..."))
                return false;
            break;
        }
    }
    return mo_buffer_append_byte(b, ')');
}
static MO_MUST_USE bool label(struct mo_buffer *b, const struct checker *c, struct answer a) {
    if (!mo_buffer_append_text(b, "in declaration ") || !label_name(b, c->declaration))
        return false;
    if (a.at != NULL) {
        mo_core_expr s = a.at->source;
        mo_core_string name = {0};
        const char *what = NULL;
        if (s->tag == MO_CORE_EXPR_VAR) {
            what = ", variable ";
            name = s->as.var;
        } else if (s->tag == MO_CORE_EXPR_GLOBAL) {
            what = ", constant ";
            name = s->as.global;
        } else if (s->tag == MO_CORE_EXPR_CALL) {
            what = ", call ";
            name = s->as.call.name;
        } else if (s->tag == MO_CORE_EXPR_CON) {
            what = ", constructor ";
            name = s->as.con.ctor;
        } else if (s->tag == MO_CORE_EXPR_JUMP) {
            what = ", jump ";
            name = s->as.jump.name;
        }
        if (what != NULL && (!mo_buffer_append_text(b, what) || !label_name(b, name)))
            return false;
        for (const struct expr *at = a.at->parent; at != NULL; at = at->parent) {
            s = at->source;
            if (s->tag == MO_CORE_EXPR_LET) {
                if (!mo_buffer_append_text(b, ", in binding ") || !label_name(b, s->as.let_.name))
                    return false;
                break;
            }
        }
    }
    return a.expected == NULL ||
           (mo_buffer_append_text(b, ": expected ") && label_type(b, a.expected, &(size_t){16}) &&
            mo_buffer_append_text(b, ", found ") && label_type(b, a.ty, &(size_t){16}));
}
static MO_MUST_USE bool report(const struct checker *c, struct mo_diags *diags, uint16_t code,
                               struct mo_core_span span, struct answer a) {
    bool resource = code == MO_CODE_CORE_CHECK_LIMIT || code == MO_CODE_CORE_CHECK_MEMORY;
    struct mo_diag d = {.code = code,
                        .message = resource ? "Core checker resource limit exceeded"
                                            : "invalid compiler Core (bug in Mo)",
                        .reason = reason(code)};
    /* With no source, file UINT32_MAX is deliberately unavailable, never fabricated file zero. */
    if (!span.some)
        span = c->declaration_span;
    d.primary.span = span.some ? span.value : (struct mo_span){.file = UINT32_MAX};
    struct mo_buffer b;
    mo_buffer_init(&b, MO_LIMIT_DIAGNOSTICS_SIZE);
    d.primary.text = label(&b, c, a) && mo_buffer_append_byte(&b, 0)
                         ? (const char *)b.bytes
                         : "Core refused; diagnostic context could not be allocated";
    bool added = mo_diags_add(diags, &d);
    mo_buffer_free(&b);
    if (!added)
        return false;
    return false;
}
static MO_MUST_USE bool declarations(struct checker *c, const struct mo_core *p,
                                     struct function **funs_out, struct constant **consts_out) {
    struct function *fs = allocate(c, &c->arena, p->funs.count, sizeof *fs);
    struct constant *ks = allocate(c, &c->arena, p->consts.count, sizeof *ks);
    if (fs == NULL || ks == NULL)
        return false;
    *funs_out = fs;
    *consts_out = ks;
    for (size_t i = 0; i < p->data.count; i++) {
        const struct mo_core_data *source = &p->data.items[i];
        struct data *d = allocate(c, &c->arena, 1, sizeof *d);
        if (d == NULL)
            return false;
        d->count = source->ctors.count;
        d->ctors = allocate(c, &c->arena, d->count, sizeof *d->ctors);
        if (d->ctors == NULL)
            return false;
        for (size_t j = 0; j < d->count; j++) {
            struct constructor *k = &d->ctors[j];
            k->name = source->ctors.items[j].name;
            k->count = source->ctors.items[j].fields.count;
            k->fields = parameter_types(c, &c->arena, source->ctors.items[j].fields);
            if (k->fields == NULL || !put(c, &c->arena, &d->names, k->name, k, false))
                return false;
        }
        if (!put(c, &c->arena, &c->data, source->name, d, false))
            return false;
    }
    for (size_t i = 0; i < p->consts.count; i++) {
        c->declaration = p->consts.items[i].name;
        c->declaration_span =
            p->metadata.source ? p->metadata.consts.items[i].span : (struct mo_core_span){0};
        if (find(c->consts, p->consts.items[i].name) != NULL) {
            c->fatal = MO_CODE_CORE_CHECK_CONSTANT_NAME;
            return false;
        }
        ks[i].index = i;
        ks[i].ty = annotation(c, p->consts.items[i].ty, 0);
        if (ks[i].ty == NULL ||
            !put(c, &c->arena, &c->consts, p->consts.items[i].name, &ks[i], false))
            return false;
    }
    for (size_t i = 0; i < p->funs.count; i++) {
        const struct mo_core_fun *f = &p->funs.items[i];
        c->declaration = f->name;
        c->declaration_span =
            p->metadata.source ? p->metadata.funs.items[i].span : (struct mo_core_span){0};
        fs[i].source = f;
        fs[i].params = parameter_types(c, &c->arena, f->params);
        fs[i].ret = annotation(c, f->ret, 0);
        if (fs[i].params == NULL || fs[i].ret == NULL)
            return false;
        if (!natural(f->const_count, p->consts.count, &fs[i].scope)) {
            c->fatal = MO_CODE_CORE_CHECK_PREFIX;
            return false;
        }
        if (!put(c, &c->arena, &c->funs, f->name, &fs[i], false))
            return false;
    }
    return true;
}
static MO_MUST_USE struct answer check_body(struct checker *c, mo_core_expr body, mo_core_params ps,
                                            struct type **types, struct type *ret) {
    size_t node = 0;
    struct expr *e = prepare(c, body, 0, &node);
    if (e == NULL)
        return bad(NULL, c->fatal);
    struct context ctx = {0};
    if (!bind_params(c, &ctx, ps, types))
        return bad(e, c->fatal);
    return tc(c, e, &ctx, NULL, ret);
}
bool mo_core_check_services(const struct mo_core *p, const struct mo_services *services,
                            struct mo_diags *diags) {
    MO_ASSERT(p != NULL && diags != NULL);
    struct checker c = {.core = p, .services = services, .declaration = p->entry};
    if (!mo_core_metadata_valid(p))
        return report(&c, diags, MO_CODE_CORE_CHECK_METADATA, (struct mo_core_span){0},
                      (struct answer){0});
    mo_arena_init(&c.arena, MO_LIMIT_CORE_CHECK_SCRATCH_SIZE / 2);
    mo_arena_init(&c.body, MO_LIMIT_CORE_CHECK_SCRATCH_SIZE / 2);
    struct function *fs = NULL;
    struct constant *ks = NULL;
    uint16_t code = 0;
    struct mo_core_span span = {0};
    struct answer failure = {0};
    bool result;
    if (!declarations(&c, p, &fs, &ks)) {
        code = c.fatal;
        goto done;
    }
    const struct function *entry = find(c.funs, p->entry);
    c.declaration = p->entry;
    c.declaration_span = p->metadata.source && entry != NULL
                             ? p->metadata.funs.items[(size_t)(entry->source - p->funs.items)].span
                             : (struct mo_core_span){0};
    if (entry == NULL || entry->source->kind != MO_CORE_FUNCTION) {
        code = entry == NULL ? MO_CODE_CORE_CHECK_ENTRY : MO_CODE_CORE_CHECK_ENTRY_KIND;
        goto done;
    }
    mo_arena_free(&c.body);
    for (size_t i = 0; i < p->consts.count; i++) {
        c.declaration = p->consts.items[i].name;
        c.declaration_span =
            p->metadata.source ? p->metadata.consts.items[i].span : (struct mo_core_span){0};
        c.scope = i;
        c.spec = false;
        struct answer a =
            check_body(&c, p->consts.items[i].body, (mo_core_params){0}, NULL, ks[i].ty);
        if (a.code != 0) {
            code = a.code;
            failure = a;
            if (p->metadata.source)
                span = a.at == NULL ? p->metadata.consts.items[i].span
                                    : p->metadata.consts.items[i].body.spans.items[a.at->node];
            goto done;
        }
        mo_arena_free(&c.body);
    }
    for (size_t i = 0; i < p->funs.count; i++) {
        const struct mo_core_fun *f = fs[i].source;
        c.declaration = f->name;
        c.declaration_span =
            p->metadata.source ? p->metadata.funs.items[i].span : (struct mo_core_span){0};
        bool valid =
            f->kind == MO_CORE_FUNCTION ||
            ((f->kind == MO_CORE_TEST || f->kind == MO_CORE_EXAMPLE) && f->params.count == 0 &&
             fs[i].ret->tag == MO_CORE_TY_UNIT) ||
            (f->kind == MO_CORE_PROMISE && f->params.count != 0 &&
             fs[i].ret->tag == MO_CORE_TY_INT) ||
            (f->kind == MO_CORE_ANCHOR && f->params.count == 0 && fs[i].ret->tag == MO_CORE_TY_INT);
        if (!valid) {
            code = MO_CODE_CORE_CHECK_KIND;
            if (p->metadata.source)
                span = p->metadata.funs.items[i].span;
            goto done;
        }
        c.scope = fs[i].scope;
        c.spec = f->kind != MO_CORE_FUNCTION;
        struct answer a = check_body(&c, f->body, f->params, fs[i].params, fs[i].ret);
        if (a.code != 0) {
            code = a.code;
            failure = a;
            if (p->metadata.source)
                span = a.at == NULL ? p->metadata.funs.items[i].span
                                    : p->metadata.funs.items[i].body.spans.items[a.at->node];
            goto done;
        }
        mo_arena_free(&c.body);
    }
done:
    result = code == 0 || report(&c, diags, code, span, failure);
    mo_arena_free(&c.body);
    mo_arena_free(&c.arena);
    return result;
}

bool mo_core_check(const struct mo_core *p, struct mo_diags *diags) {
    return mo_core_check_services(p, NULL, diags);
}
