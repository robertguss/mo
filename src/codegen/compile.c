#include "codegen/codegen.h"

#include <string.h>

#include "base/codes.h"
#include "bytecode/bytecode.h"
#include "codegen/index.h"
#include "heap/types.h"

/* Arrays grow geometrically. Retained arena blocks cost less than twice the final capacity. */
struct vector {
    void *items;
    uint32_t count, capacity;
    size_t width;
};
struct reg {
    uint32_t type, name;
};
struct node {
    mo_core_expr core;
    struct mo_core_span span;
    uint32_t type, reg, index;
    uint32_t count, bind_count;
    struct node **children;
    uint32_t *binds, *cases;
};
struct function {
    struct vector regs, code, spans, names;
    struct node *body;
    struct mo_codegen_index *locals, *bindings, *zeros;
    const struct mo_core_fun_meta *meta;
    uint32_t preorder, instructions;
};
struct compiler {
    const struct mo_core *core;
    struct mo_arena arena;
    struct mo_types types;
    struct vector strings, constants, modules, constructors;
    struct mo_codegen_index *string_index, *constant_index, *data_index, *fun_index, *module_index,
        *builtin_index;
    uint32_t *data, *files;
    struct mo_bytecode_function *funs;
    struct mo_bytecode_debug *debug;
    struct function *bodies;
    uint32_t debug_entries;
    const struct mo_core_fun *current;
    struct mo_core_span span, declaration;
    uint16_t error;
    const char *reason;
};

static MO_MUST_USE bool problem(struct compiler *c, uint16_t code, const char *reason) {
    if (c->error == 0) {
        c->error = code;
        c->reason = reason;
    }
    return false;
}
static MO_MUST_USE bool unsupported(struct compiler *c, const char *reason) {
    return problem(c, MO_CODE_CODEGEN_UNSUPPORTED, reason);
}
static MO_MUST_USE void *allocate(struct compiler *c, size_t count, size_t width) {
    void *p = mo_arena_allocate_array(&c->arena, count, width, _Alignof(max_align_t));
    if (p == NULL) {
        if (!problem(c, MO_CODE_CODEGEN_MEMORY,
                     "Code generation exhausted its scratch limit or memory."))
            return NULL;
    }
    return p;
}
static MO_MUST_USE void *append(struct compiler *c, struct vector *v, uint32_t n, uint32_t limit,
                                uint16_t code) {
    if (n > limit - v->count) {
        if (!problem(c, code,
                     "Code generation exceeds the corresponding bytecode table or function limit."))
            return NULL;
    }
    uint32_t required = v->count + n;
    if (required > v->capacity) {
        uint32_t capacity = v->capacity == 0 ? 16 : v->capacity;
        while (capacity < required)
            capacity = capacity > limit / 2 ? limit : capacity * 2;
        void *p = allocate(c, capacity, v->width);
        if (p == NULL)
            return NULL;
        if (v->count != 0)
            memcpy(p, v->items, (size_t)v->count * v->width);
        v->items = p;
        v->capacity = capacity;
    }
    void *p = (uint8_t *)v->items + (size_t)v->count * v->width;
    v->count = required;
    return p;
}
static MO_MUST_USE mo_core_string text(const char *s) {
    return (mo_core_string){s, strlen(s)};
}
static MO_MUST_USE struct mo_codegen_index *index_add(struct compiler *c,
                                                      struct mo_codegen_index **root,
                                                      mo_core_string key, uint32_t value,
                                                      bool copy) {
    struct mo_codegen_index *entry = mo_codegen_find(*root, key);
    if (entry != NULL)
        return entry;
    if (copy) {
        char *bytes = allocate(c, key.length, 1);
        if (bytes == NULL)
            return NULL;
        if (key.length != 0)
            memcpy(bytes, key.bytes, key.length);
        key.bytes = bytes;
    }
    entry = mo_codegen_insert(&c->arena, root, key, value);
    if (entry == NULL &&
        !problem(c, MO_CODE_CODEGEN_MEMORY, "No memory for a code generation index."))
        return NULL;
    return entry;
}
static MO_MUST_USE uint32_t string(struct compiler *c, mo_core_string s) {
    struct mo_codegen_index *old = mo_codegen_find(c->string_index, s);
    if (old != NULL)
        return old->value;
    if (s.length > MO_LIMIT_STRING_SIZE) {
        if (!problem(c, MO_CODE_BYTECODE_TOO_BIG,
                     "A generated string exceeds MO_LIMIT_STRING_SIZE."))
            return MO_NO_INDEX;
    }
    uint32_t id = c->strings.count;
    struct mo_bytecode_string *item =
        append(c, &c->strings, 1, MO_LIMIT_STRINGS, MO_CODE_BYTECODE_TOO_BIG);
    struct mo_codegen_index *entry = index_add(c, &c->string_index, s, id, true);
    if (item == NULL || entry == NULL)
        return MO_NO_INDEX;
    *item = (struct mo_bytecode_string){(uint32_t)s.length, (const uint8_t *)entry->key.bytes};
    return id;
}
static MO_MUST_USE const char *name(struct compiler *c, mo_core_string s) {
    if (s.length == 0 || memchr(s.bytes, 0, s.length) != NULL) {
        if (!unsupported(
                c, "Empty or NUL-containing Core names are not implemented by code generation."))
            return NULL;
    }
    uint32_t id = string(c, s);
    if (id == MO_NO_INDEX)
        return NULL;
    char *p = allocate(c, s.length + 1, 1);
    if (p == NULL)
        return NULL;
    memcpy(p, s.bytes, s.length);
    p[s.length] = 0;
    return p;
}
static MO_MUST_USE uint32_t type_result(struct compiler *c, enum mo_type_problem status,
                                        uint32_t id) {
    if (status == MO_TYPES_OK)
        return id;
    uint16_t code =
        status == MO_TYPES_NO_MEMORY ? MO_CODE_CODEGEN_MEMORY : MO_CODE_BYTECODE_TOO_BIG;
    if (status == MO_TYPES_INVALID)
        code = MO_CODE_CODEGEN_UNSUPPORTED;
    if (!problem(c, code, "Core type cannot be represented by this compiler's type table."))
        return MO_NO_INDEX;
    return MO_NO_INDEX;
}
static MO_MUST_USE uint32_t scalar(struct compiler *c, enum mo_type_kind kind) {
    uint32_t id = MO_NO_INDEX;
    enum mo_type_problem status = mo_types_intern(&c->types, &(struct mo_type){.kind = kind}, &id);
    return type_result(c, status, id);
}
static MO_MUST_USE const char *type_name(struct compiler *c, uint32_t id) {
    static const char *const names[] = {"Unit", "Bool",  "Byte",     "Int",     "Float",
                                        "Char", "Time",  "Duration", "Decimal", "Range",
                                        "Text", "Bytes", "BigInt"};
    const struct mo_type *t = &c->types.items[id];
    return t->kind <= MO_TYPE_BIGINT ? names[t->kind] : t->name;
}
static MO_MUST_USE uint32_t type(struct compiler *c, mo_core_ty t, unsigned depth) {
    if (depth >= MO_LIMIT_CORE_DEPTH) {
        if (!problem(c, MO_CODE_NESTED_TOO_DEEPLY,
                     "Core type nesting exceeds MO_LIMIT_CORE_DEPTH."))
            return MO_NO_INDEX;
    }
    enum mo_type_kind kind;
    switch (t->tag) {
    case MO_CORE_TY_UNIT:
        kind = MO_TYPE_UNIT;
        break;
    case MO_CORE_TY_BOOL:
        kind = MO_TYPE_BOOL;
        break;
    case MO_CORE_TY_BYTE:
        kind = MO_TYPE_BYTE;
        break;
    case MO_CORE_TY_INT:
        kind = MO_TYPE_INT;
        break;
    case MO_CORE_TY_FLOAT:
        kind = MO_TYPE_FLOAT;
        break;
    case MO_CORE_TY_CHAR:
        kind = MO_TYPE_CHAR;
        break;
    case MO_CORE_TY_TIME:
        kind = MO_TYPE_TIME;
        break;
    case MO_CORE_TY_DURATION:
        kind = MO_TYPE_DURATION;
        break;
    case MO_CORE_TY_DECIMAL:
        kind = MO_TYPE_DECIMAL;
        break;
    case MO_CORE_TY_DATA: {
        struct mo_codegen_index *entry = mo_codegen_find(c->data_index, t->as.data);
        if (entry != NULL)
            return entry->value;
        if (!unsupported(c, "Undeclared data types have no bytecode representation."))
            return MO_NO_INDEX;
        return MO_NO_INDEX;
    }
    case MO_CORE_TY_OPTION:
    case MO_CORE_TY_RESULT:
    case MO_CORE_TY_ORDERING:
    case MO_CORE_TY_ENTRY_KIND: {
        uint32_t a = MO_NO_INDEX, b = MO_NO_INDEX;
        if (t->tag == MO_CORE_TY_OPTION)
            a = type(c, t->as.option, depth + 1);
        if (t->tag == MO_CORE_TY_RESULT) {
            a = type(c, t->as.result.t, depth + 1);
            b = type(c, t->as.result.e, depth + 1);
        }
        if (c->error != 0)
            return MO_NO_INDEX;
        const uint32_t key[] = {(uint32_t)t->tag, a, b};
        mo_core_string bytes = {(const char *)key, sizeof key};
        struct mo_codegen_index *old = mo_codegen_find(c->builtin_index, bytes);
        if (old != NULL)
            return old->value;
        const char *base = t->tag == MO_CORE_TY_OPTION     ? "Option"
                           : t->tag == MO_CORE_TY_RESULT   ? "Result"
                           : t->tag == MO_CORE_TY_ORDERING ? "Ordering"
                                                           : "EntryKind";
        struct mo_buffer buf;
        mo_buffer_init(&buf, MO_LIMIT_STRING_SIZE);
        bool ok = mo_buffer_append_text(&buf, base);
        if (a != MO_NO_INDEX)
            ok = ok && mo_buffer_append_byte(&buf, '(') &&
                 mo_buffer_append_text(&buf, type_name(c, a));
        if (b != MO_NO_INDEX)
            ok = ok && mo_buffer_append_text(&buf, ", ") &&
                 mo_buffer_append_text(&buf, type_name(c, b));
        if (a != MO_NO_INDEX)
            ok = ok && mo_buffer_append_byte(&buf, ')');
        const char *nominal =
            ok ? name(c, (mo_core_string){(const char *)buf.bytes, buf.length}) : NULL;
        mo_buffer_free(&buf);
        if (nominal == NULL) {
            if (!problem(c, MO_CODE_CODEGEN_MEMORY, "No memory for an instantiated type name."))
                return MO_NO_INDEX;
        }
        uint32_t id = MO_NO_INDEX;
        enum mo_type_problem status = mo_types_declare(&c->types, MO_TYPE_CHOICE, nominal, &id);
        if (type_result(c, status, id) == MO_NO_INDEX)
            return MO_NO_INDEX;
        const char *const *names;
        static const char *const option[] = {"None", "Some"}, *const result[] = {"Ok", "Error"},
                                 *const ordering[] = {"Less", "Equal", "Greater"},
                                 *const entry[] = {"File", "Folder", "Link", "Other"};
        names = t->tag == MO_CORE_TY_OPTION     ? option
                : t->tag == MO_CORE_TY_RESULT   ? result
                : t->tag == MO_CORE_TY_ORDERING ? ordering
                                                : entry;
        struct mo_type_field fields[] = {{NULL, a}, {NULL, b}};
        struct mo_type_case cases[4] = {{0}};
        uint32_t n = t->tag == MO_CORE_TY_ENTRY_KIND ? 4 : t->tag == MO_CORE_TY_ORDERING ? 3 : 2;
        for (uint32_t i = 0; i < n; i++) {
            if (string(c, text(names[i])) == MO_NO_INDEX)
                return MO_NO_INDEX;
            cases[i].name = names[i];
            if (t->tag == MO_CORE_TY_RESULT || (t->tag == MO_CORE_TY_OPTION && i == 1)) {
                cases[i].field_count = 1;
                cases[i].fields = &fields[t->tag == MO_CORE_TY_OPTION ? 0 : i];
                cases[i].unnamed = true;
            }
        }
        status = mo_types_define_choice(&c->types, id, cases, n);
        if (type_result(c, status, id) == MO_NO_INDEX ||
            index_add(c, &c->builtin_index, bytes, id, true) == NULL)
            return MO_NO_INDEX;
        return id;
    }
    case MO_CORE_TY_BIG_INT:
    case MO_CORE_TY_TEXT:
    case MO_CORE_TY_BYTES:
    case MO_CORE_TY_RANGE:
    case MO_CORE_TY_TUPLE:
    case MO_CORE_TY_LIST:
    case MO_CORE_TY_MAP:
    case MO_CORE_TY_SET:
    case MO_CORE_TY_FILE_ERROR:
    case MO_CORE_TY_ENTRY:
    case MO_CORE_TY_FN:
    case MO_CORE_TY_CAP:
        if (!unsupported(
                c, "Reference, aggregate, Range and capability types are not implemented yet."))
            return MO_NO_INDEX;
        return MO_NO_INDEX;
    }
    return scalar(c, kind);
}
static MO_MUST_USE uint32_t constant(struct compiler *c, struct mo_bytecode_constant value) {
    uint8_t key[20];
    memcpy(key, &value.type, sizeof value.type);
    memcpy(key + 4, value.value.immediate, 16);
    mo_core_string bytes = {(const char *)key, sizeof key};
    struct mo_codegen_index *old = mo_codegen_find(c->constant_index, bytes);
    if (old != NULL)
        return old->value;
    uint32_t id = c->constants.count;
    struct mo_bytecode_constant *item =
        append(c, &c->constants, 1, MO_LIMIT_CONSTANTS, MO_CODE_BYTECODE_TOO_BIG);
    if (item == NULL || index_add(c, &c->constant_index, bytes, id, true) == NULL)
        return MO_NO_INDEX;
    *item = value;
    return id;
}
static MO_MUST_USE uint32_t literal(struct compiler *c, mo_core_lit lit, uint32_t *ty) {
    static const enum mo_type_kind kinds[] = {MO_TYPE_INT,     MO_TYPE_BIGINT, MO_TYPE_BYTE,
                                              MO_TYPE_DECIMAL, MO_TYPE_FLOAT,  MO_TYPE_BOOL,
                                              MO_TYPE_CHAR,    MO_TYPE_TEXT,   MO_TYPE_BYTES,
                                              MO_TYPE_UNIT,    MO_TYPE_TIME,   MO_TYPE_DURATION};
    if (lit->tag == MO_CORE_LIT_BIG_INT || lit->tag == MO_CORE_LIT_TEXT ||
        lit->tag == MO_CORE_LIT_BYTES) {
        if (!unsupported(c, "Reference literals are not implemented yet."))
            return MO_NO_INDEX;
    }
    *ty = scalar(c, kinds[lit->tag]);
    if (*ty == MO_NO_INDEX)
        return MO_NO_INDEX;
    mo_u128 bits = 0;
    const mo_core_integer *integer = NULL;
    switch (lit->tag) {
    case MO_CORE_LIT_INT:
        integer = &lit->as.int_;
        break;
    case MO_CORE_LIT_BYTE:
        integer = &lit->as.byte;
        break;
    case MO_CORE_LIT_DECIMAL:
        integer = &lit->as.decimal;
        break;
    case MO_CORE_LIT_TIME:
        integer = &lit->as.time;
        break;
    case MO_CORE_LIT_DURATION:
        integer = &lit->as.duration;
        break;
    case MO_CORE_LIT_FLOAT:
        bits = lit->as.float_;
        break;
    case MO_CORE_LIT_BOOL:
        bits = lit->as.bool_;
        break;
    case MO_CORE_LIT_CHAR:
        bits = lit->as.char_;
        break;
    case MO_CORE_LIT_UNIT:
        break;
    case MO_CORE_LIT_BIG_INT:
    case MO_CORE_LIT_TEXT:
    case MO_CORE_LIT_BYTES:
        MO_UNREACHABLE();
    }
    if (integer != NULL) {
        for (size_t i = 0; i < integer->digits.length; i++)
            bits = bits * 10 + (unsigned)(integer->digits.bytes[i] - '0');
        if (integer->negative)
            bits = (mo_u128)0 - bits;
    }
    struct mo_bytecode_constant k = {.type = *ty};
    unsigned n = lit->tag == MO_CORE_LIT_DECIMAL ? 16 : 8;
    for (unsigned i = 0; i < n; i++) {
        k.value.immediate[i] = (uint8_t)bits;
        bits >>= 8;
    }
    return constant(c, k);
}
static MO_MUST_USE uint32_t register_add(struct compiler *c, struct function *f, uint32_t ty,
                                         uint32_t original) {
    if (ty == MO_NO_INDEX)
        return MO_NO_INDEX;
    uint32_t id = f->regs.count;
    struct reg *r = append(c, &f->regs, 1, MO_LIMIT_FUNCTION_REGISTERS, MO_CODE_TOO_MANY_REGISTERS);
    if (r == NULL)
        return MO_NO_INDEX;
    *r = (struct reg){ty, original};
    return id;
}
static MO_MUST_USE uint32_t binding_name(struct function *f, uint32_t node, uint32_t slot) {
    const uint32_t key[] = {node, slot};
    struct mo_codegen_index *entry =
        mo_codegen_find(f->bindings, (mo_core_string){(const char *)key, sizeof key});
    return entry == NULL ? MO_NO_INDEX : entry->value;
}
static MO_MUST_USE uint32_t constructor(struct compiler *c, uint32_t ty, mo_core_string key) {
    if (c->constructors.count < c->types.count) {
        uint32_t count = c->types.count - c->constructors.count;
        struct mo_codegen_index **new_roots =
            append(c, &c->constructors, count, MO_LIMIT_TYPES, MO_CODE_BYTECODE_TOO_BIG);
        if (new_roots == NULL)
            return MO_NO_INDEX;
        memset(new_roots, 0, (size_t)count * sizeof *new_roots);
    }
    struct mo_codegen_index **root = &((struct mo_codegen_index **)c->constructors.items)[ty];
    const struct mo_type *t = &c->types.items[ty];
    if (*root == NULL) {
        for (uint32_t i = 0; i < t->case_count; i++)
            if (index_add(c, root, text(t->cases[i].name), i, false) == NULL)
                return MO_NO_INDEX;
    }
    struct mo_codegen_index *entry = mo_codegen_find(*root, key);
    MO_ASSERT(entry != NULL);
    return entry->value;
}
static MO_MUST_USE struct node *prepare(struct compiler *c, struct function *f, mo_core_expr expr,
                                        unsigned depth);
static MO_MUST_USE bool children(struct compiler *c, struct function *f, struct node *n,
                                 mo_core_expr_list xs, unsigned depth) {
    n->count = (uint32_t)xs.count;
    n->children = allocate(c, xs.count, sizeof *n->children);
    if (n->children == NULL)
        return false;
    for (uint32_t i = 0; i < n->count; i++) {
        n->children[i] = prepare(c, f, xs.items[i], depth + 1);
        if (n->children[i] == NULL)
            return false;
    }
    return true;
}
static MO_MUST_USE struct node *prepare(struct compiler *c, struct function *f, mo_core_expr expr,
                                        unsigned depth) {
    if (depth >= MO_LIMIT_CORE_DEPTH) {
        if (!problem(c, MO_CODE_NESTED_TOO_DEEPLY,
                     "Core expression nesting exceeds MO_LIMIT_CORE_DEPTH."))
            return NULL;
    }
    uint32_t preorder = f->preorder++;
    struct node *n = allocate(c, 1, sizeof *n);
    if (n == NULL)
        return NULL;
    *n = (struct node){.core = expr, .type = MO_NO_INDEX, .reg = MO_NO_INDEX, .index = MO_NO_INDEX};
    if (f->meta != NULL)
        n->span = f->meta->body.spans.items[preorder];
    c->span = n->span;
    mo_core_expr small[3];
    switch (expr->tag) {
    case MO_CORE_EXPR_LIT:
        n->index = literal(c, expr->as.lit, &n->type);
        break;
    case MO_CORE_EXPR_VAR: {
        struct mo_codegen_index *entry = mo_codegen_find(f->locals, expr->as.var);
        MO_ASSERT(entry != NULL && entry->value != MO_NO_INDEX);
        n->reg = entry->value;
        n->type = ((struct reg *)f->regs.items)[n->reg].type;
        return n;
    }
    case MO_CORE_EXPR_LET: {
        n->children = allocate(c, 2, sizeof *n->children);
        n->binds = allocate(c, 1, sizeof *n->binds);
        if (n->children == NULL || n->binds == NULL)
            return NULL;
        n->count = 2;
        n->bind_count = 1;
        n->children[0] = prepare(c, f, expr->as.let_.bound, depth + 1);
        if (n->children[0] == NULL)
            return NULL;
        n->binds[0] = register_add(c, f, n->children[0]->type, binding_name(f, preorder, 0));
        struct mo_codegen_index *entry =
            index_add(c, &f->locals, expr->as.let_.name, MO_NO_INDEX, false);
        if (entry == NULL || n->binds[0] == MO_NO_INDEX)
            return NULL;
        uint32_t saved = entry->value;
        entry->value = n->binds[0];
        n->children[1] = prepare(c, f, expr->as.let_.body, depth + 1);
        entry->value = saved;
        if (n->children[1] == NULL)
            return NULL;
        n->type = n->children[1]->type;
        n->reg = n->children[1]->reg;
        return n;
    }
    case MO_CORE_EXPR_CALL: {
        struct mo_codegen_index *entry = mo_codegen_find(c->fun_index, expr->as.call.name);
        MO_ASSERT(entry != NULL);
        n->index = entry->value;
        n->type = c->funs[n->index].result_type;
        if (!children(c, f, n, expr->as.call.args, depth))
            return NULL;
        break;
    }
    case MO_CORE_EXPR_IF:
        small[0] = expr->as.if_.cond;
        small[1] = expr->as.if_.then_;
        small[2] = expr->as.if_.else_;
        if (!children(c, f, n, (mo_core_expr_list){small, 3}, depth))
            return NULL;
        n->type = n->children[1]->type != MO_NO_INDEX ? n->children[1]->type : n->children[2]->type;
        break;
    case MO_CORE_EXPR_CON:
        n->type = type(c, expr->as.con.ty, 0);
        if (n->type == MO_NO_INDEX || !children(c, f, n, expr->as.con.args, depth))
            return NULL;
        n->index = constructor(c, n->type, expr->as.con.ctor);
        if (n->index == MO_NO_INDEX)
            return NULL;
        break;
    case MO_CORE_EXPR_CASE: {
        n->count = 1 + (uint32_t)expr->as.case_.arms.count;
        n->children = allocate(c, n->count, sizeof *n->children);
        n->cases = allocate(c, n->count - 1, sizeof *n->cases);
        if (n->children == NULL || n->cases == NULL)
            return NULL;
        n->children[0] = prepare(c, f, expr->as.case_.expr, depth + 1);
        if (n->children[0] == NULL)
            return NULL;
        uint32_t ty = n->children[0]->type;
        MO_ASSERT(ty != MO_NO_INDEX);
        uint32_t slot = 0;
        for (uint32_t i = 1; i < n->count; i++) {
            const struct mo_core_arm *arm = &expr->as.case_.arms.items[i - 1];
            uint32_t case_id = constructor(c, ty, arm->ctor);
            if (case_id == MO_NO_INDEX)
                return NULL;
            n->cases[i - 1] = case_id;
            const struct mo_type_case *ctor = &c->types.items[ty].cases[case_id];
            struct mo_codegen_index **entries = allocate(c, arm->names.count, sizeof *entries);
            uint32_t *saved = allocate(c, arm->names.count, sizeof *saved);
            uint32_t *regs = allocate(c, arm->names.count, sizeof *regs);
            if (entries == NULL || saved == NULL || regs == NULL)
                return NULL;
            for (uint32_t j = 0; j < ctor->field_count; j++) {
                regs[j] =
                    register_add(c, f, ctor->fields[j].type, binding_name(f, preorder, slot++));
                entries[j] = index_add(c, &f->locals, arm->names.items[j], MO_NO_INDEX, false);
                if (regs[j] == MO_NO_INDEX || entries[j] == NULL)
                    return NULL;
            }
            for (uint32_t j = ctor->field_count; j-- > 0;) {
                saved[j] = entries[j]->value;
                entries[j]->value = regs[j];
            }
            struct node *body = prepare(c, f, arm->body, depth + 1);
            for (uint32_t j = 0; j < ctor->field_count; j++)
                entries[j]->value = saved[j];
            if (body == NULL)
                return NULL;
            /* Arm wrapper holds pattern registers independently of its body's binders. */
            struct node *wrapper = allocate(c, 1, sizeof *wrapper);
            if (wrapper == NULL)
                return NULL;
            *wrapper = *body;
            wrapper->children = allocate(c, 1, sizeof *wrapper->children);
            if (wrapper->children == NULL)
                return NULL;
            wrapper->children[0] = body;
            wrapper->binds = regs;
            wrapper->bind_count = ctor->field_count;
            n->children[i] = wrapper;
            if (n->type == MO_NO_INDEX)
                n->type = body->type;
        }
        break;
    }
    case MO_CORE_EXPR_STOP: {
        uint32_t ty = scalar(c, MO_TYPE_TEXT), s = string(c, expr->as.stop);
        if (ty == MO_NO_INDEX || s == MO_NO_INDEX)
            return NULL;
        struct mo_bytecode_constant k = {.type = ty};
        k.value.string = s;
        n->index = constant(c, k);
        n->reg = register_add(c, f, ty, MO_NO_INDEX);
        return c->error == 0 ? n : NULL;
    }
    case MO_CORE_EXPR_GLOBAL:
    case MO_CORE_EXPR_LAM:
    case MO_CORE_EXPR_APP:
    case MO_CORE_EXPR_TUPLE:
    case MO_CORE_EXPR_PROJ:
    case MO_CORE_EXPR_PRIM:
    case MO_CORE_EXPR_FAIL:
    case MO_CORE_EXPR_JOIN:
    case MO_CORE_EXPR_JUMP:
        if (!unsupported(c, "Globals, closures, aggregates, primitives, fail and joins are not "
                            "implemented yet."))
            return NULL;
    }
    if (c->error != 0)
        return NULL;
    n->reg = register_add(c, f, n->type, MO_NO_INDEX);
    return c->error == 0 ? n : NULL;
}

static MO_MUST_USE struct mo_bytecode_span span(struct compiler *c, struct mo_core_span s) {
    if (s.some && s.value.end > MO_LIMIT_FILE_SIZE) {
        if (!problem(c, MO_CODE_BYTECODE_TOO_BIG,
                     "Source span exceeds the bytecode MO_LIMIT_FILE_SIZE bound."))
            return (struct mo_bytecode_span){.file = MO_NO_INDEX};
    }
    return s.some ? (struct mo_bytecode_span){0, c->files[s.value.file], s.value.start, s.value.end}
                  : (struct mo_bytecode_span){.file = MO_NO_INDEX};
}
static MO_MUST_USE bool debug_add(struct compiler *c) {
    if (c->debug_entries == MO_LIMIT_DEBUG_ENTRIES)
        return problem(c, MO_CODE_BYTECODE_TOO_BIG,
                       "Debug information exceeds MO_LIMIT_DEBUG_ENTRIES.");
    c->debug_entries++;
    return true;
}
static MO_MUST_USE uint32_t emit(struct compiler *c, struct function *f, struct mo_core_span source,
                                 enum mo_opcode op, const uint32_t *fixed, uint32_t n,
                                 const uint32_t *variable, uint32_t count) {
    c->span = source;
    if (f->instructions == MO_LIMIT_FUNCTION_INSTRUCTIONS) {
        if (!problem(c, MO_CODE_FUNCTION_TOO_LONG,
                     "Function exceeds MO_LIMIT_FUNCTION_INSTRUCTIONS."))
            return MO_NO_INDEX;
    }
    uint32_t offset = f->code.count;
    uint32_t *words =
        append(c, &f->code, 1 + n + count, MO_LIMIT_FUNCTION_CODE_WORDS, MO_CODE_FUNCTION_TOO_LONG);
    if (words == NULL)
        return MO_NO_INDEX;
    f->instructions++;
    words[0] = (uint32_t)op | (count << MO_OPCODE_VARIABLE_SHIFT);
    if (n != 0)
        memcpy(words + 1, fixed, (size_t)n * sizeof *fixed);
    if (count != 0)
        memcpy(words + 1 + n, variable, (size_t)count * sizeof *variable);
    if (c->core->metadata.source) {
        struct mo_bytecode_span current = span(c, source);
        if (c->error != 0)
            return MO_NO_INDEX;
        struct mo_bytecode_span *previous =
            f->spans.count == 0 ? NULL
                                : &((struct mo_bytecode_span *)f->spans.items)[f->spans.count - 1];
        if (previous == NULL || previous->file != current.file ||
            previous->start != current.start || previous->end != current.end) {
            struct mo_bytecode_span *s =
                append(c, &f->spans, 1, MO_LIMIT_DEBUG_ENTRIES, MO_CODE_BYTECODE_TOO_BIG);
            if (s == NULL || !debug_add(c))
                return MO_NO_INDEX;
            current.code_offset = offset;
            *s = current;
        }
    }
    return offset;
}
static MO_MUST_USE bool named(struct compiler *c, struct function *f, uint32_t reg, uint32_t start,
                              uint32_t end) {
    uint32_t name_id = ((struct reg *)f->regs.items)[reg].name;
    if (name_id == MO_NO_INDEX || start == end)
        return true;
    struct mo_bytecode_register_name *n =
        append(c, &f->names, 1, MO_LIMIT_DEBUG_ENTRIES, MO_CODE_BYTECODE_TOO_BIG);
    if (n == NULL || !debug_add(c))
        return false;
    *n = (struct mo_bytecode_register_name){reg, name_id, start, end};
    return true;
}
/* Value states at joins are identical. This first slice uses only immediates, plus borrowed
 * immortal stop reasons. Initialize every temporary before control splits; no owned values. */
static MO_MUST_USE bool initialize(struct compiler *c, struct function *f, uint32_t reg,
                                   unsigned depth) {
    MO_ASSERT(depth < MO_LIMIT_CORE_DEPTH);
    uint32_t ty = ((struct reg *)f->regs.items)[reg].type;
    const struct mo_type *t = &c->types.items[ty];
    if (t->kind == MO_TYPE_CHOICE) {
        const struct mo_type_case *first = &t->cases[0];
        uint32_t field = MO_NO_INDEX;
        uint32_t count = first->field_count;
        if (count != 0) {
            uint32_t field_type = first->fields[0].type;
            mo_core_string key = {(const char *)&field_type, sizeof field_type};
            struct mo_codegen_index *old = mo_codegen_find(f->zeros, key);
            if (old != NULL) {
                field = old->value;
            } else {
                field = register_add(c, f, field_type, MO_NO_INDEX);
                if (field == MO_NO_INDEX || !initialize(c, f, field, depth + 1) ||
                    index_add(c, &f->zeros, key, field, true) == NULL)
                    return false;
            }
        }
        const uint32_t fixed[] = {reg, ty, 0};
        return emit(c, f, c->declaration, MO_OP_MAKE_CASE, fixed, 3, &field, count) != MO_NO_INDEX;
    }
    struct mo_bytecode_constant k = {.type = ty};
    if (t->kind == MO_TYPE_TEXT) {
        k.value.string = string(c, text(""));
        if (k.value.string == MO_NO_INDEX)
            return false;
    }
    uint32_t id = constant(c, k);
    const uint32_t fixed[] = {reg, id};
    return id != MO_NO_INDEX &&
           emit(c, f, c->declaration, MO_OP_CONST, fixed, 2, NULL, 0) != MO_NO_INDEX;
}
/* false through *live means a terminal path, not a compiler refusal. */
static MO_MUST_USE bool generate(struct compiler *c, struct function *f, struct node *n, bool tail,
                                 unsigned depth, bool *live) {
    MO_ASSERT(depth < MO_LIMIT_CORE_DEPTH);
    c->span = n->span;
    *live = true;
    switch (n->core->tag) {
    case MO_CORE_EXPR_LIT: {
        const uint32_t args[] = {n->reg, n->index};
        if (emit(c, f, n->span, MO_OP_CONST, args, 2, NULL, 0) == MO_NO_INDEX)
            return false;
        break;
    }
    case MO_CORE_EXPR_VAR:
        break;
    case MO_CORE_EXPR_LET: {
        if (!generate(c, f, n->children[0], false, depth + 1, live) || !*live)
            return c->error == 0;
        uint32_t start = f->code.count;
        const uint32_t args[] = {n->binds[0], n->children[0]->reg};
        if (emit(c, f, n->span, MO_OP_MOVE, args, 2, NULL, 0) == MO_NO_INDEX ||
            !generate(c, f, n->children[1], tail, depth + 1, live))
            return false;
        return named(c, f, n->binds[0], start, f->code.count);
    }
    case MO_CORE_EXPR_CALL:
    case MO_CORE_EXPR_CON: {
        uint32_t *regs = allocate(c, n->count, sizeof *regs);
        if (regs == NULL)
            return false;
        for (uint32_t i = 0; i < n->count; i++) {
            if (!generate(c, f, n->children[i], false, depth + 1, live) || !*live)
                return c->error == 0;
            regs[i] = n->children[i]->reg;
        }
        uint32_t args[] = {n->reg, n->index, 0};
        enum mo_opcode op = MO_OP_CALL;
        uint32_t count = 2;
        if (n->core->tag == MO_CORE_EXPR_CON) {
            op = MO_OP_MAKE_CASE;
            args[1] = n->type;
            args[2] = n->index;
            count = 3;
        } else if (tail) {
            op = MO_OP_TAIL_CALL;
            args[0] = n->index;
            count = 1;
            *live = false;
        }
        if (emit(c, f, n->span, op, args, count, regs, n->count) == MO_NO_INDEX)
            return false;
        break;
    }
    case MO_CORE_EXPR_IF:
    case MO_CORE_EXPR_CASE: {
        if (!generate(c, f, n->children[0], false, depth + 1, live) || !*live)
            return c->error == 0;
        uint32_t arms = n->count - 1;
        uint32_t *targets = allocate(c, arms, sizeof *targets),
                 *jumps = allocate(c, arms, sizeof *jumps);
        if (targets == NULL || jumps == NULL)
            return false;
        memset(targets, 0, (size_t)arms * sizeof *targets);
        for (uint32_t i = 0; i < arms; i++)
            jumps[i] = MO_NO_INDEX;
        bool choice = n->core->tag == MO_CORE_EXPR_CASE;
        uint32_t fixed[] = {n->children[0]->reg, 0, 0};
        uint32_t at = emit(c, f, n->span, choice ? MO_OP_SWITCH : MO_OP_BRANCH, fixed,
                           choice ? 1 : 3, choice ? targets : NULL, choice ? arms : 0);
        if (at == MO_NO_INDEX)
            return false;
        bool any = false;
        for (uint32_t i = 0; i < arms; i++) {
            uint32_t case_id = choice ? n->cases[i] : i;
            targets[case_id] = f->code.count;
            uint32_t start = f->code.count;
            struct node *arm = n->children[i + 1], *body = choice ? arm->children[0] : arm;
            if (choice) {
                for (uint32_t j = 0; j < arm->bind_count; j++) {
                    const uint32_t field[] = {arm->binds[j], n->children[0]->reg, case_id, j};
                    if (emit(c, f, n->span, MO_OP_CASE_FIELD, field, 4, NULL, 0) == MO_NO_INDEX)
                        return false;
                }
            }
            if (!generate(c, f, body, tail, depth + 1, live))
                return false;
            if (choice) {
                for (uint32_t j = 0; j < arm->bind_count; j++)
                    if (!named(c, f, arm->binds[j], start, f->code.count))
                        return false;
            }
            if (*live) {
                any = true;
                const uint32_t move[] = {n->reg, body->reg}, jump[] = {0};
                if (emit(c, f, n->span, MO_OP_MOVE, move, 2, NULL, 0) == MO_NO_INDEX)
                    return false;
                jumps[i] = emit(c, f, n->span, MO_OP_JUMP, jump, 1, NULL, 0);
                if (jumps[i] == MO_NO_INDEX)
                    return false;
            }
        }
        uint32_t *words = f->code.items;
        memcpy(words + at + 2, targets, (size_t)arms * sizeof *targets);
        for (uint32_t i = 0; i < arms; i++)
            if (jumps[i] != MO_NO_INDEX)
                words[jumps[i] + 1] = f->code.count;
        *live = any;
        break;
    }
    case MO_CORE_EXPR_STOP: {
        const uint32_t args[] = {n->reg, n->index}, stop[] = {n->reg};
        if (emit(c, f, n->span, MO_OP_CONST, args, 2, NULL, 0) == MO_NO_INDEX ||
            emit(c, f, n->span, MO_OP_STOP, stop, 1, NULL, 0) == MO_NO_INDEX)
            return false;
        *live = false;
        break;
    }
    case MO_CORE_EXPR_GLOBAL:
    case MO_CORE_EXPR_LAM:
    case MO_CORE_EXPR_APP:
    case MO_CORE_EXPR_TUPLE:
    case MO_CORE_EXPR_PROJ:
    case MO_CORE_EXPR_PRIM:
    case MO_CORE_EXPR_FAIL:
    case MO_CORE_EXPR_JOIN:
    case MO_CORE_EXPR_JUMP:
        MO_UNREACHABLE();
    }
    if (*live && tail) {
        const uint32_t args[] = {n->reg};
        if (emit(c, f, n->span, MO_OP_RET, args, 1, NULL, 0) == MO_NO_INDEX)
            return false;
        *live = false;
    }
    return true;
}

/* Core permits an arbitrary entry key. The image used by mo run has a stricter name contract. */
static MO_MUST_USE bool entry_valid(struct compiler *c) {
    const struct mo_core *p = c->core;
    for (size_t i = 0; i < p->funs.count; i++) {
        const struct mo_core_fun *f = &p->funs.items[i];
        if (f->name.length != p->entry.length ||
            memcmp(f->name.bytes, p->entry.bytes, f->name.length) != 0)
            continue;
        c->current = f;
        c->declaration =
            p->metadata.source ? p->metadata.funs.items[i].span : (struct mo_core_span){0};
        bool valid;
        if (p->metadata.source) {
            mo_core_string n = p->metadata.funs.items[i].name;
            valid = n.length >= 5 && n.length - 5 == f->module.length &&
                    memcmp(n.bytes, f->module.bytes, f->module.length) == 0 &&
                    memcmp(n.bytes + f->module.length, ".main", 5) == 0;
        } else {
            valid = f->name.length == 4 && memcmp(f->name.bytes, "main", 4) == 0;
        }
        return valid || problem(c, MO_CODE_CODEGEN_ENTRY,
                                "Core's ordinary entry must have the full name <Module>.main.");
    }
    MO_UNREACHABLE(); /* mo_core_check established the ordinary entry. */
}
static MO_MUST_USE bool declarations(struct compiler *c) {
    const struct mo_core *p = c->core;
    if (p->consts.count != 0)
        return unsupported(c, "Global initialization is not implemented yet.");
    if (p->funs.count > MO_LIMIT_FUNCTIONS || p->data.count > MO_LIMIT_TYPES)
        return problem(c, MO_CODE_BYTECODE_TOO_BIG,
                       "Core has too many function or type declarations.");
    c->data = allocate(c, p->data.count, sizeof *c->data);
    c->funs = allocate(c, p->funs.count, sizeof *c->funs);
    c->bodies = allocate(c, p->funs.count, sizeof *c->bodies);
    c->debug = allocate(c, p->funs.count, sizeof *c->debug);
    size_t file_count = p->metadata.source ? p->metadata.files.count : 0;
    c->files = allocate(c, file_count, sizeof *c->files);
    if (c->data == NULL || c->funs == NULL || c->bodies == NULL || c->debug == NULL ||
        c->files == NULL)
        return false;
    memset(c->funs, 0, p->funs.count * sizeof *c->funs);
    memset(c->bodies, 0, p->funs.count * sizeof *c->bodies);
    memset(c->debug, 0, p->funs.count * sizeof *c->debug);
    for (size_t i = 0; i < file_count; i++) {
        c->files[i] = string(c, p->metadata.files.items[i]);
        if (c->files[i] == MO_NO_INDEX)
            return false;
    }
    for (size_t i = 0; i < p->data.count; i++) {
        const char *n = name(c, p->data.items[i].name);
        if (n == NULL)
            return false;
        c->data[i] = MO_NO_INDEX;
        enum mo_type_problem status = mo_types_declare(&c->types, MO_TYPE_CHOICE, n, &c->data[i]);
        if (type_result(c, status, c->data[i]) == MO_NO_INDEX ||
            index_add(c, &c->data_index, p->data.items[i].name, c->data[i], false) == NULL)
            return false;
    }
    for (size_t i = 0; i < p->data.count; i++) {
        const struct mo_core_data *d = &p->data.items[i];
        if (d->ctors.count == 0)
            return unsupported(
                c, "Empty data declarations have no bytecode choice representation yet.");
        struct mo_type_case *cases = allocate(c, d->ctors.count, sizeof *cases);
        struct mo_codegen_index *seen = NULL;
        if (cases == NULL)
            return false;
        for (size_t j = 0; j < d->ctors.count; j++) {
            const struct mo_core_ctor *ctor = &d->ctors.items[j];
            if (mo_codegen_find(seen, ctor->name) != NULL)
                return unsupported(c,
                                   "Duplicate Core constructor names are not representable yet.");
            if (index_add(c, &seen, ctor->name, 0, false) == NULL)
                return false;
            if (ctor->fields.count > 1)
                return unsupported(c, "Boxed data payloads are not implemented yet.");
            const char *n = name(c, ctor->name);
            struct mo_type_field *field = allocate(c, ctor->fields.count, sizeof *field);
            if (n == NULL || field == NULL)
                return false;
            for (size_t k = 0; k < ctor->fields.count; k++) {
                field[k].name = ctor->fields.items[k].name.length == 0
                                    ? NULL
                                    : name(c, ctor->fields.items[k].name);
                field[k].type = type(c, ctor->fields.items[k].ty, 0);
                if (c->error != 0)
                    return false;
            }
            cases[j] = (struct mo_type_case){n, field, (uint32_t)ctor->fields.count,
                                             ctor->fields.count == 1 && field[0].name == NULL};
        }
        enum mo_type_problem status =
            mo_types_define_choice(&c->types, c->data[i], cases, (uint32_t)d->ctors.count);
        if (type_result(c, status, c->data[i]) == MO_NO_INDEX)
            return false;
    }
    size_t module_count = p->metadata.source ? p->metadata.modules.count : 0;
    for (size_t i = 0; i < module_count; i++) {
        const struct mo_core_module_meta *m = &p->metadata.modules.items[i];
        uint32_t id = c->modules.count;
        struct mo_bytecode_module *b =
            append(c, &c->modules, 1, MO_LIMIT_MODULES, MO_CODE_BYTECODE_TOO_BIG);
        struct mo_bytecode_type_export *exports =
            allocate(c, m->exposed_types.count, sizeof *exports);
        if (b == NULL || exports == NULL)
            return false;
        for (size_t j = 0; j < m->exposed_types.count; j++)
            exports[j] =
                (struct mo_bytecode_type_export){c->data[m->exposed_types.items[j].index],
                                                 m->exposed_types.items[j].contents_exposed};
        *b = (struct mo_bytecode_module){.name = string(c, m->name),
                                         .type_count = (uint32_t)m->exposed_types.count,
                                         .types = exports,
                                         .function_count = (uint32_t)m->exposed_funs.count,
                                         .functions = m->exposed_funs.items};
        if (b->name == MO_NO_INDEX || index_add(c, &c->module_index, m->name, id, false) == NULL)
            return false;
    }
    static const enum mo_function_kind kinds[] = {MO_FUNCTION_FUNCTION, MO_FUNCTION_TEST,
                                                  MO_FUNCTION_EXAMPLE, MO_FUNCTION_PROMISE,
                                                  MO_FUNCTION_ANCHOR};
    for (size_t i = 0; i < p->funs.count; i++) {
        c->current = &p->funs.items[i];
        const struct mo_core_fun *fun = c->current;
        const struct mo_core_fun_meta *meta =
            p->metadata.source ? &p->metadata.funs.items[i] : NULL;
        c->declaration = meta == NULL ? (struct mo_core_span){0} : meta->span;
        c->span = c->declaration;
        if (fun->params.count > MO_LIMIT_BYTECODE_PARAMETERS)
            return problem(c, MO_CODE_BYTECODE_TOO_BIG,
                           "Function exceeds MO_LIMIT_BYTECODE_PARAMETERS.");
        struct mo_codegen_index *module = mo_codegen_find(c->module_index, fun->module);
        if (module == NULL) {
            uint32_t id = c->modules.count;
            struct mo_bytecode_module *m =
                append(c, &c->modules, 1, MO_LIMIT_MODULES, MO_CODE_BYTECODE_TOO_BIG);
            if (m == NULL || name(c, fun->module) == NULL)
                return false;
            *m = (struct mo_bytecode_module){.name = string(c, fun->module)};
            module = index_add(c, &c->module_index, fun->module, id, false);
            if (module == NULL)
                return false;
        }
        uint32_t full_name = MO_NO_INDEX;
        if (meta != NULL)
            full_name = string(c, meta->name);
        else {
            struct mo_buffer b;
            mo_buffer_init(&b, MO_LIMIT_STRING_SIZE);
            bool ok = mo_buffer_append(&b, fun->module.bytes, fun->module.length) &&
                      mo_buffer_append_byte(&b, '.') &&
                      mo_buffer_append(&b, fun->name.bytes, fun->name.length);
            if (ok && name(c, (mo_core_string){(const char *)b.bytes, b.length}) != NULL)
                full_name = string(c, (mo_core_string){(const char *)b.bytes, b.length});
            mo_buffer_free(&b);
            if (full_name == MO_NO_INDEX &&
                !problem(c, MO_CODE_BYTECODE_TOO_BIG, "Generated function name exceeds its limit."))
                return false;
        }
        struct mo_parameter *params = allocate(c, fun->params.count, sizeof *params);
        if (params == NULL)
            return false;
        for (size_t j = 0; j < fun->params.count; j++) {
            params[j] =
                (struct mo_parameter){type(c, fun->params.items[j].ty, 0), MO_PARAMETER_OWNED};
            if (params[j].type == MO_NO_INDEX)
                return false;
        }
        c->funs[i] = (struct mo_bytecode_function){.name = full_name,
                                                   .module = module->value,
                                                   .kind = kinds[fun->kind],
                                                   .spec_code = fun->kind != MO_CORE_FUNCTION,
                                                   .parameter_count = (uint32_t)fun->params.count,
                                                   .parameters = params,
                                                   .result_type = type(c, fun->ret, 0)};
        c->bodies[i].meta = meta;
        if (c->funs[i].result_type == MO_NO_INDEX ||
            index_add(c, &c->fun_index, fun->name, (uint32_t)i, false) == NULL)
            return false;
    }
    return c->error == 0;
}
static MO_MUST_USE bool bodies(struct compiler *c) {
    for (size_t i = 0; i < c->core->funs.count; i++) {
        c->current = &c->core->funs.items[i];
        struct function *f = &c->bodies[i];
        const struct mo_core_fun_meta *meta = f->meta;
        c->declaration = meta == NULL ? (struct mo_core_span){0} : meta->span;
        c->span = c->declaration;
        f->regs.width = sizeof(struct reg);
        f->code.width = sizeof(uint32_t);
        f->spans.width = sizeof(struct mo_bytecode_span);
        f->names.width = sizeof(struct mo_bytecode_register_name);
        if (meta != NULL) {
            for (size_t j = 0; j < meta->body.bindings.count; j++) {
                const struct mo_core_binding_meta *binding = &meta->body.bindings.items[j];
                const uint32_t key[] = {binding->node, binding->slot};
                uint32_t n = string(c, binding->name);
                if (n == MO_NO_INDEX ||
                    index_add(c, &f->bindings, (mo_core_string){(const char *)key, sizeof key}, n,
                              true) == NULL)
                    return false;
            }
        }
        for (uint32_t j = 0; j < c->funs[i].parameter_count; j++) {
            uint32_t original = MO_NO_INDEX;
            if (meta != NULL && meta->body.params.items[j].some)
                original = string(c, meta->body.params.items[j].value);
            if (register_add(c, f, c->funs[i].parameters[j].type, original) == MO_NO_INDEX)
                return false;
        }
        for (uint32_t j = c->funs[i].parameter_count; j-- > 0;) {
            struct mo_codegen_index *entry =
                index_add(c, &f->locals, c->current->params.items[j].name, MO_NO_INDEX, false);
            if (entry == NULL)
                return false;
            entry->value = j;
        }
        f->body = prepare(c, f, c->current->body, 0);
        if (f->body == NULL)
            return false;
    }
    uint32_t ignored = 0;
    if (type_result(c, mo_types_finish(&c->types), ignored) == MO_NO_INDEX)
        return false;
    for (uint32_t i = 0; i < c->types.count; i++) {
        const struct mo_type *t = &c->types.items[i];
        if (t->kind == MO_TYPE_CHOICE) {
            for (uint32_t j = 0; j < t->case_count; j++)
                if (mo_case_shape(&c->types, i, j) != MO_CASE_EMPTY &&
                    mo_case_shape(&c->types, i, j) != MO_CASE_INLINE)
                    return unsupported(
                        c, "Reference and boxed choice payloads are not implemented yet.");
        }
    }
    return true;
}
static MO_MUST_USE bool output(struct compiler *c, struct mo_buffer *out) {
    for (size_t i = 0; i < c->core->funs.count; i++) {
        c->current = &c->core->funs.items[i];
        struct function *f = &c->bodies[i];
        struct mo_bytecode_function *fun = &c->funs[i];
        const struct mo_core_fun_meta *meta = f->meta;
        c->declaration = meta == NULL ? (struct mo_core_span){0} : meta->span;
        /* Helpers added while initializing are already initialized. Do not reinitialize them. */
        c->span = c->declaration;
        uint32_t count = f->regs.count;
        for (uint32_t j = fun->parameter_count; j < count; j++)
            if (!initialize(c, f, j, 0))
                return false;
        bool live;
        if (!generate(c, f, f->body, true, 0, &live))
            return false;
        MO_ASSERT(!live);
        uint32_t *types = allocate(c, f->regs.count, sizeof *types);
        if (types == NULL)
            return false;
        for (uint32_t j = 0; j < f->regs.count; j++)
            types[j] = ((struct reg *)f->regs.items)[j].type;
        for (uint32_t j = 0; j < fun->parameter_count; j++)
            if (!named(c, f, j, 0, f->code.count))
                return false;
        fun->register_count = f->regs.count;
        fun->register_types = types;
        fun->code_count = f->code.count;
        fun->code = f->code.items;
        struct mo_bytecode_span declaration = span(c, c->declaration);
        struct mo_bytecode_debug *d = &c->debug[i];
        *d = (struct mo_bytecode_debug){.span_count = f->spans.count,
                                        .spans = f->spans.items,
                                        .name_count = f->names.count,
                                        .names = f->names.items,
                                        .text = MO_NO_INDEX,
                                        .file = declaration.file,
                                        .start = declaration.start,
                                        .end = declaration.end,
                                        .subject = MO_NO_INDEX};
        if (meta != NULL) {
            if (meta->text.some)
                d->text = string(c, meta->text.value);
            if (meta->subject.some)
                d->subject = meta->subject.value;
            if (meta->allows.some)
                d->expectation = meta->allows.value ? MO_BYTECODE_ALLOWS : MO_BYTECODE_FORBIDS;
            struct mo_bytecode_spec_line *lines = allocate(c, meta->lines.count, sizeof *lines);
            if (lines == NULL)
                return false;
            for (size_t j = 0; j < meta->lines.count; j++) {
                struct mo_bytecode_span s = span(c, meta->lines.items[j].span);
                lines[j] = (struct mo_bytecode_spec_line){s.file, s.start, s.end,
                                                          string(c, meta->lines.items[j].text)};
                if (lines[j].text == MO_NO_INDEX || !debug_add(c))
                    return false;
            }
            d->line_count = (uint32_t)meta->lines.count;
            d->lines = lines;
        }
        if (c->error != 0)
            return false;
    }
    const struct mo_bytecode file = {.string_count = c->strings.count,
                                     .strings = c->strings.items,
                                     .type_count = c->types.count,
                                     .types = c->types.items,
                                     .layout_count = c->types.layout_count,
                                     .layouts = c->types.layouts,
                                     .constant_count = c->constants.count,
                                     .constants = c->constants.items,
                                     .function_count = (uint32_t)c->core->funs.count,
                                     .functions = c->funs,
                                     .module_count = c->modules.count,
                                     .modules = c->modules.items,
                                     .debug_count = (uint32_t)c->core->funs.count,
                                     .debug = c->debug};
    struct mo_write_error error;
    struct mo_buffer encoded;
    mo_buffer_init(&encoded, MO_LIMIT_BYTECODE_SIZE);
    bool written = mo_bytecode_write(&file, &encoded, &error);
    if (written) {
        bool fits = encoded.length <= out->limit;
        bool ok = fits && mo_buffer_append(out, encoded.bytes, encoded.length);
        mo_buffer_free(&encoded);
        return ok || problem(c, fits ? MO_CODE_CODEGEN_MEMORY : MO_CODE_BYTECODE_TOO_BIG,
                             fits ? "No memory for bytecode output."
                                  : "Bytecode exceeds the caller's output limit.");
    }
    mo_buffer_free(&encoded);
    uint16_t code = error.kind == MO_BYTECODE_NO_MEMORY ? MO_CODE_CODEGEN_MEMORY
                    : error.kind == MO_BYTECODE_LIMIT   ? MO_CODE_BYTECODE_TOO_BIG
                                                        : MO_CODE_CODEGEN_INVALID;
    return problem(c, code, error.reason);
}
bool mo_compile(const struct mo_core *core, struct mo_buffer *out, struct mo_diags *diags) {
    MO_ASSERT(core != NULL && out != NULL && out->length == 0 && diags != NULL);
    if (!mo_core_check(core, diags))
        return false;
    struct compiler c = {.core = core};
    mo_arena_init(&c.arena, MO_LIMIT_CODEGEN_SCRATCH_SIZE);
    mo_types_init(&c.types, &c.arena);
    c.strings.width = sizeof(struct mo_bytecode_string);
    c.constants.width = sizeof(struct mo_bytecode_constant);
    c.modules.width = sizeof(struct mo_bytecode_module);
    c.constructors.width = sizeof(struct mo_codegen_index *);
    bool ok = entry_valid(&c) && declarations(&c) && bodies(&c) && output(&c, out);
    if (!ok) {
        MO_ASSERT(c.error != 0 && out->length == 0);
        struct mo_buffer label;
        mo_buffer_init(&label, MO_LIMIT_DIAGNOSTICS_SIZE);
        bool described =
            (c.current == NULL ||
             (mo_buffer_append(&label, c.current->name.bytes, c.current->name.length) &&
              mo_buffer_append_text(&label, ": "))) &&
            mo_buffer_append_text(&label, c.reason) && mo_buffer_append_byte(&label, 0);
        struct mo_diag d = {
            .code = c.error,
            .message = c.error == MO_CODE_CODEGEN_ENTRY ? "invalid lowered entry name: bug in Mo"
                       : c.error == MO_CODE_CODEGEN_INVALID
                           ? "code generation produced invalid bytecode: bug in Mo"
                       : c.error == MO_CODE_CODEGEN_UNSUPPORTED
                           ? "Core code generation is not implemented for this form yet"
                           : "Core code generation reached a resource limit",
            .reason = c.reason,
            .primary = {.span = c.span.some          ? c.span.value
                                : c.declaration.some ? c.declaration.value
                                                     : (struct mo_span){.file = UINT32_MAX},
                        .text = described ? (const char *)label.bytes : c.reason}};
        if (!mo_diags_add(diags, &d))
            ok = false;
        mo_buffer_free(&label);
    }
    mo_arena_free(&c.arena);
    return ok;
}
