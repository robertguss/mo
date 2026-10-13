#include "codegen/codegen.h"

#include <stdio.h>
#include <string.h>

#include "base/codes.h"
#include "bytecode/bytecode.h"
#include "codegen/abilities.h"
#include "codegen/bigint.h"
#include "codegen/control.h"
#include "codegen/index.h"
#include "codegen/operations.h"
#include "heap/types.h"

/* Arrays grow geometrically. Retained arena blocks cost less than twice the final capacity. */
struct vector {
    void *items;
    uint32_t count, capacity;
    size_t width;
};
enum ref_state { REF_UNDEFINED, REF_BORROWED, REF_OWNED, REF_MOVED };
struct reg {
    uint32_t type, name;
    uint32_t flow, previous, next;
    uint32_t mark;
    enum ref_state state, goal;
    bool owned, protected;
};
struct change {
    uint32_t reg;
    enum ref_state previous;
};
struct ownership {
    uint32_t type, edges;
    bool owned;
};
struct edge {
    uint32_t to, next;
};
struct capture {
    uint32_t source, reg;
};
struct constraint {
    uint32_t type;
    uint8_t ability;
    const struct mo_core_fun *function;
    struct mo_core_span span, declaration;
};
struct node {
    mo_core_expr core;
    struct mo_core_span span;
    uint32_t type, reg, index;
    uint32_t count, bind_count;
    struct node **children;
    uint32_t *binds, *cases;
    struct mo_codegen_operation operation;
    uint32_t signature;
    bool *last, *after, *work;
    bool temporary;
};
struct join {
    struct node *node;
    uint32_t offset, checkpoint;
    bool *live;
};
struct function {
    struct vector regs, code, spans, names, captures, joins, changes;
    struct node *body;
    struct mo_codegen_index *locals, *bindings, *zeros, *closure_names, *capture_index, *join_names;
    const struct mo_core_fun_meta *meta;
    const struct mo_core_fun *source;
    struct function *parent, *root;
    struct mo_core_span declaration;
    uint32_t lambda_number, closure_type;
    uint32_t preorder, instructions, blocks, owned, mark;
    uint32_t live_count, live_controls;
    uint64_t live_work;
    bool next_block;
};
struct compiler {
    const struct mo_core *core;
    struct mo_arena arena;
    struct mo_types types;
    struct vector strings, constants, modules, constructors, ownership, edges, constraints;
    struct mo_codegen_index *string_index, *constant_index, *data_index, *fun_index, *global_index,
        *module_index, *builtin_index, *annotation_index, *operation_index;
    struct mo_codegen_index *bigint_index[2];
    uint32_t *data, *files, *layouts;
    uint32_t range_int, function_count, function_capacity, unit_constant;
    struct mo_bytecode_global *globals;
    uint32_t *init;
    struct mo_bytecode_function *funs;
    struct mo_parameter **parameters;
    struct mo_bytecode_debug *debug;
    struct function *bodies;
    uint32_t debug_entries;
    const struct mo_core_fun *current;
    struct mo_core_span span, declaration;
    uint16_t error;
    const char *reason;
    size_t bigint_work;
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
                     code == MO_CODE_CODEGEN_MEMORY
                         ? "Code generation exceeds its ownership graph limit."
                         : "Code generation exceeds the corresponding bytecode table or function "
                           "limit."))
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
static MO_MUST_USE bool name_valid(struct compiler *c, mo_core_string s) {
    return (s.length != 0 && memchr(s.bytes, 0, s.length) == NULL) ||
           problem(c, MO_CODE_CODEGEN_REPRESENTATION,
                   "Empty or NUL-containing Core names cannot come from lowering.");
}
static MO_MUST_USE const char *name(struct compiler *c, mo_core_string s) {
    if (!name_valid(c, s))
        return NULL;
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
        code = MO_CODE_CODEGEN_INVALID;
    if (!problem(c, code, "The generated type or layout was refused by the runtime type builder."))
        return MO_NO_INDEX;
    return MO_NO_INDEX;
}
static MO_MUST_USE uint32_t scalar(struct compiler *c, enum mo_type_kind kind) {
    uint32_t id = MO_NO_INDEX;
    enum mo_type_problem status = mo_types_intern(&c->types, &(struct mo_type){.kind = kind}, &id);
    return type_result(c, status, id);
}
static MO_MUST_USE bool type_text(struct compiler *c, struct mo_buffer *out, const char *s) {
    size_t length = strlen(s);
    if (length > out->limit - out->length)
        return problem(c, MO_CODE_BYTECODE_TOO_BIG,
                       "Instantiated type name exceeds MO_LIMIT_STRING_SIZE.");
    return mo_buffer_append(out, s, length) ||
           problem(c, MO_CODE_CODEGEN_MEMORY, "No memory for an instantiated type name.");
}
static MO_MUST_USE bool type_name(struct compiler *c, uint32_t id, struct mo_buffer *out,
                                  unsigned depth) {
    if (depth >= MO_LIMIT_CORE_DEPTH)
        return problem(c, MO_CODE_NESTED_TOO_DEEPLY,
                       "Instantiated type name nesting exceeds MO_LIMIT_CORE_DEPTH.");
    static const char *const names[] = {"Unit", "Bool",  "Byte",     "Int",     "Float",
                                        "Char", "Time",  "Duration", "Decimal", "Range",
                                        "Text", "Bytes", "BigInt"};
    const struct mo_type *t = &c->types.items[id];
    if (t->kind == MO_TYPE_CAPABILITY) {
        static const char *const caps[] = {"Args",      "Env",   "Console", "Files",
                                           "ReadFiles", "Clock", "Random"};
        return type_text(c, out, caps[t->capability]);
    }
    bool collection = t->kind == MO_TYPE_LIST || t->kind == MO_TYPE_SET || t->kind == MO_TYPE_MAP;
    if (t->kind != MO_TYPE_TUPLE && t->kind != MO_TYPE_FUNCTION && !collection)
        return type_text(c, out, t->kind <= MO_TYPE_BIGINT ? names[t->kind] : t->name);
    const char *start = t->kind == MO_TYPE_FUNCTION ? "fn("
                        : t->kind == MO_TYPE_LIST   ? "List("
                        : t->kind == MO_TYPE_SET    ? "Set("
                        : t->kind == MO_TYPE_MAP    ? "Map("
                                                    : "(";
    if (!type_text(c, out, start))
        return false;
    for (uint32_t i = 0; i < t->parameter_count; i++)
        if ((i != 0 && !type_text(c, out, ", ")) || !type_name(c, t->parameters[i], out, depth + 1))
            return false;
    return type_text(c, out, ")") &&
           (t->kind != MO_TYPE_FUNCTION ||
            (type_text(c, out, " : ") && type_name(c, t->result, out, depth + 1)));
}
static MO_MUST_USE uint32_t type_uncached(struct compiler *c, mo_core_ty t, unsigned depth);
static MO_MUST_USE uint32_t type(struct compiler *c, mo_core_ty t, unsigned depth) {
    if (depth >= MO_LIMIT_CORE_DEPTH) {
        if (!problem(c, MO_CODE_NESTED_TOO_DEEPLY,
                     "Core type nesting exceeds MO_LIMIT_CORE_DEPTH."))
            return MO_NO_INDEX;
    }
    if (t->tag < MO_CORE_TY_TUPLE)
        return type_uncached(c, t, depth);
    mo_core_string key = {(const char *)&t, sizeof t};
    struct mo_codegen_index *old = mo_codegen_find(c->annotation_index, key);
    if (old != NULL)
        return old->value;
    uint32_t id = type_uncached(c, t, depth);
    if (id == MO_NO_INDEX || index_add(c, &c->annotation_index, key, id, true) == NULL)
        return MO_NO_INDEX;
    return id;
}
static MO_MUST_USE uint32_t type_uncached(struct compiler *c, mo_core_ty t, unsigned depth) {
    enum mo_type_kind kind = MO_TYPE_UNIT;
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
    case MO_CORE_TY_TEXT:
        kind = MO_TYPE_TEXT;
        break;
    case MO_CORE_TY_BYTES:
        kind = MO_TYPE_BYTES;
        break;
    case MO_CORE_TY_BIG_INT:
        kind = MO_TYPE_BIGINT;
        break;
    case MO_CORE_TY_RANGE:
        c->range_int = scalar(c, MO_TYPE_INT);
        if (c->range_int == MO_NO_INDEX)
            return MO_NO_INDEX;
        kind = MO_TYPE_RANGE;
        break;
    case MO_CORE_TY_TUPLE: {
        if (t->as.tuple.count < 2) {
            if (!problem(c, MO_CODE_CODEGEN_REPRESENTATION,
                         "Zero/one-element tuples cannot come from lowering."))
                return MO_NO_INDEX;
        }
        if (t->as.tuple.count > MO_LIMIT_FIELDS) {
            if (!problem(c, MO_CODE_BYTECODE_TOO_BIG, "Tuple exceeds MO_LIMIT_FIELDS."))
                return MO_NO_INDEX;
        }
        uint32_t *params = allocate(c, t->as.tuple.count, sizeof *params);
        if (params == NULL)
            return MO_NO_INDEX;
        for (size_t i = 0; i < t->as.tuple.count; i++) {
            params[i] = type(c, t->as.tuple.items[i], depth + 1);
            if (params[i] == MO_NO_INDEX)
                return MO_NO_INDEX;
        }
        uint32_t id = MO_NO_INDEX;
        enum mo_type_problem status =
            mo_types_intern(&c->types,
                            &(struct mo_type){.kind = MO_TYPE_TUPLE,
                                              .parameters = params,
                                              .parameter_count = (uint32_t)t->as.tuple.count},
                            &id);
        return type_result(c, status, id);
    }
    case MO_CORE_TY_DATA: {
        struct mo_codegen_index *entry = mo_codegen_find(c->data_index, t->as.data);
        if (entry != NULL)
            return entry->value;
        if (!problem(c, MO_CODE_CODEGEN_REPRESENTATION,
                     "Undeclared data types cannot come from lowering."))
            return MO_NO_INDEX;
        return MO_NO_INDEX;
    }
    case MO_CORE_TY_OPTION:
    case MO_CORE_TY_RESULT:
    case MO_CORE_TY_ORDERING:
    case MO_CORE_TY_FILE_ERROR:
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
        const char *base = t->tag == MO_CORE_TY_OPTION       ? "Option"
                           : t->tag == MO_CORE_TY_RESULT     ? "Result"
                           : t->tag == MO_CORE_TY_ORDERING   ? "Ordering"
                           : t->tag == MO_CORE_TY_FILE_ERROR ? "FileError"
                                                             : "EntryKind";
        struct mo_buffer buf;
        mo_buffer_init(&buf, MO_LIMIT_STRING_SIZE);
        bool ok = type_text(c, &buf, base);
        if (a != MO_NO_INDEX)
            ok = ok && type_text(c, &buf, "(") && type_name(c, a, &buf, 0);
        if (b != MO_NO_INDEX)
            ok = ok && type_text(c, &buf, ", ") && type_name(c, b, &buf, 0);
        if (a != MO_NO_INDEX)
            ok = ok && type_text(c, &buf, ")");
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
                                 *const entry[] = {"File", "Folder", "Link", "Other"},
                                 *const errors[] = {"NotFound", "NotAllowed", "NotText", "Failed"};
        names = t->tag == MO_CORE_TY_OPTION       ? option
                : t->tag == MO_CORE_TY_RESULT     ? result
                : t->tag == MO_CORE_TY_ORDERING   ? ordering
                : t->tag == MO_CORE_TY_FILE_ERROR ? errors
                                                  : entry;
        struct mo_type_field fields[] = {{NULL, a}, {NULL, b}};
        if (t->tag == MO_CORE_TY_FILE_ERROR) {
            uint32_t text_type = scalar(c, MO_TYPE_TEXT);
            if (text_type == MO_NO_INDEX || string(c, text("path")) == MO_NO_INDEX ||
                string(c, text("reason")) == MO_NO_INDEX)
                return MO_NO_INDEX;
            fields[0] = (struct mo_type_field){"path", text_type};
            fields[1] = (struct mo_type_field){"reason", text_type};
        }
        struct mo_type_case cases[4] = {{0}};
        uint32_t n = t->tag == MO_CORE_TY_ENTRY_KIND || t->tag == MO_CORE_TY_FILE_ERROR ? 4
                     : t->tag == MO_CORE_TY_ORDERING                                    ? 3
                                                                                        : 2;
        for (uint32_t i = 0; i < n; i++) {
            if (string(c, text(names[i])) == MO_NO_INDEX)
                return MO_NO_INDEX;
            cases[i].name = names[i];
            if (t->tag == MO_CORE_TY_RESULT || (t->tag == MO_CORE_TY_OPTION && i == 1)) {
                cases[i].field_count = 1;
                cases[i].fields = &fields[t->tag == MO_CORE_TY_OPTION ? 0 : i];
                cases[i].unnamed = true;
            }
            if (t->tag == MO_CORE_TY_FILE_ERROR) {
                cases[i].field_count = i == 3 ? 2 : 1;
                cases[i].fields = fields;
            }
        }
        status = mo_types_define_choice(&c->types, id, cases, n);
        if (type_result(c, status, id) == MO_NO_INDEX ||
            index_add(c, &c->builtin_index, bytes, id, true) == NULL)
            return MO_NO_INDEX;
        return id;
    }
    case MO_CORE_TY_FN: {
        if (t->as.fn.params.count > MO_LIMIT_PARAMETERS) {
            if (!problem(c, MO_CODE_BYTECODE_TOO_BIG,
                         "Closure signature exceeds MO_LIMIT_PARAMETERS."))
                return MO_NO_INDEX;
        }
        uint32_t *params = allocate(c, t->as.fn.params.count, sizeof *params);
        if (params == NULL)
            return MO_NO_INDEX;
        for (size_t i = 0; i < t->as.fn.params.count; i++) {
            params[i] = type(c, t->as.fn.params.items[i], depth + 1);
            if (params[i] == MO_NO_INDEX)
                return MO_NO_INDEX;
        }
        uint32_t result = type(c, t->as.fn.ret, depth + 1), id = MO_NO_INDEX;
        if (result == MO_NO_INDEX)
            return MO_NO_INDEX;
        enum mo_type_problem status =
            mo_types_intern(&c->types,
                            &(struct mo_type){.kind = MO_TYPE_FUNCTION,
                                              .parameters = params,
                                              .parameter_count = (uint32_t)t->as.fn.params.count,
                                              .result = result},
                            &id);
        return type_result(c, status, id);
    }
    case MO_CORE_TY_LIST:
    case MO_CORE_TY_SET:
    case MO_CORE_TY_MAP: {
        uint32_t params[2], count = t->tag == MO_CORE_TY_MAP ? 2 : 1;
        params[0] = type(c,
                         t->tag == MO_CORE_TY_MAP    ? t->as.map.k
                         : t->tag == MO_CORE_TY_LIST ? t->as.list
                                                     : t->as.set,
                         depth + 1);
        if (params[0] == MO_NO_INDEX)
            return MO_NO_INDEX;
        if (count == 2 && (params[1] = type(c, t->as.map.v, depth + 1)) == MO_NO_INDEX)
            return MO_NO_INDEX;
        uint32_t id = MO_NO_INDEX;
        enum mo_type_problem status =
            mo_types_intern(&c->types,
                            &(struct mo_type){.kind = t->tag == MO_CORE_TY_LIST  ? MO_TYPE_LIST
                                                      : t->tag == MO_CORE_TY_SET ? MO_TYPE_SET
                                                                                 : MO_TYPE_MAP,
                                              .parameter_count = count,
                                              .parameters = params},
                            &id);
        return type_result(c, status, id);
    }
    case MO_CORE_TY_ENTRY: {
        const uint32_t key[] = {MO_CORE_TY_ENTRY, MO_NO_INDEX, MO_NO_INDEX};
        mo_core_string bytes = {(const char *)key, sizeof key};
        struct mo_codegen_index *old = mo_codegen_find(c->builtin_index, bytes);
        if (old != NULL)
            return old->value;
        /* These immutable synthetic annotations are translated directly, not pointer-memoized. */
        struct mo_core_ty entry_kind = {.tag = MO_CORE_TY_ENTRY_KIND};
        uint32_t text_type = scalar(c, MO_TYPE_TEXT), kind_type = type_uncached(c, &entry_kind, 0);
        if (text_type == MO_NO_INDEX || kind_type == MO_NO_INDEX ||
            string(c, text("Entry")) == MO_NO_INDEX || string(c, text("name")) == MO_NO_INDEX ||
            string(c, text("kind")) == MO_NO_INDEX)
            return MO_NO_INDEX;
        uint32_t id = MO_NO_INDEX;
        enum mo_type_problem status = mo_types_declare(&c->types, MO_TYPE_RECORD, "Entry", &id);
        if (type_result(c, status, id) == MO_NO_INDEX)
            return MO_NO_INDEX;
        const struct mo_type_field fields[] = {{"name", text_type}, {"kind", kind_type}};
        status = mo_types_define_record(&c->types, id, fields, 2);
        if (type_result(c, status, id) == MO_NO_INDEX ||
            index_add(c, &c->builtin_index, bytes, id, true) == NULL)
            return MO_NO_INDEX;
        return id;
    }
    case MO_CORE_TY_CAP: {
        static const enum mo_capability caps[] = {MO_CAP_CONSOLE, MO_CAP_FILES,  MO_CAP_READ_FILES,
                                                  MO_CAP_CLOCK,   MO_CAP_RANDOM, MO_CAP_ARGS,
                                                  MO_CAP_ENV};
        uint32_t id = MO_NO_INDEX;
        enum mo_type_problem status = mo_types_intern(
            &c->types,
            &(struct mo_type){.kind = MO_TYPE_CAPABILITY, .capability = caps[t->as.cap->tag]}, &id);
        return type_result(c, status, id);
    }
    }
    MO_ASSERT((uint32_t)t->tag < MO_CORE_TY_TUPLE);
    return scalar(c, kind);
}
static MO_MUST_USE bool signature_take(const char **cursor, char ch) {
    while (**cursor == ' ')
        (*cursor)++;
    if (**cursor != ch)
        return false;
    (*cursor)++;
    return true;
}
/* Only the trusted .def templates are parsed, never a source signature or a type graph. */
static MO_MUST_USE mo_core_ty signature_type(struct compiler *c, const char **cursor,
                                             const mo_core_ty variables[4], unsigned depth) {
    MO_ASSERT(depth < 16);
    struct mo_core_ty *t = allocate(c, 1, sizeof *t);
    if (t == NULL)
        return NULL;
    memset(t, 0, sizeof *t);
    mo_core_ty arguments[MO_LIMIT_BYTECODE_PARAMETERS];
    uint32_t count = 0;
    if (signature_take(cursor, '(')) {
        t->tag = MO_CORE_TY_TUPLE;
    } else {
        const char *start = *cursor;
        while ((**cursor >= 'A' && **cursor <= 'Z') || (**cursor >= 'a' && **cursor <= 'z'))
            (*cursor)++;
        size_t length = (size_t)(*cursor - start);
        if (length == 1) {
            unsigned slot = *start == 'T' ? 0u : *start == 'U' ? 1u : *start == 'K' ? 2u : 3u;
            MO_ASSERT((*start == 'T' || *start == 'U' || *start == 'K' || *start == 'V') &&
                      variables[slot] != NULL);
            return variables[slot];
        }
        static const struct {
            const char *name;
            enum mo_core_ty_tag tag;
        } names[] = {{"Unit", MO_CORE_TY_UNIT},
                     {"Bool", MO_CORE_TY_BOOL},
                     {"Byte", MO_CORE_TY_BYTE},
                     {"Int", MO_CORE_TY_INT},
                     {"Float", MO_CORE_TY_FLOAT},
                     {"Char", MO_CORE_TY_CHAR},
                     {"Time", MO_CORE_TY_TIME},
                     {"Duration", MO_CORE_TY_DURATION},
                     {"Decimal", MO_CORE_TY_DECIMAL},
                     {"Range", MO_CORE_TY_RANGE},
                     {"Text", MO_CORE_TY_TEXT},
                     {"Bytes", MO_CORE_TY_BYTES},
                     {"BigInt", MO_CORE_TY_BIG_INT},
                     {"List", MO_CORE_TY_LIST},
                     {"Set", MO_CORE_TY_SET},
                     {"Map", MO_CORE_TY_MAP},
                     {"Option", MO_CORE_TY_OPTION},
                     {"Result", MO_CORE_TY_RESULT},
                     {"Ordering", MO_CORE_TY_ORDERING},
                     {"Entry", MO_CORE_TY_ENTRY},
                     {"FileError", MO_CORE_TY_FILE_ERROR}};
        bool found = false;
        for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
            if (strlen(names[i].name) == length && memcmp(start, names[i].name, length) == 0) {
                t->tag = names[i].tag;
                found = true;
                break;
            }
        }
        if (!found) {
            static const char *const caps[] = {"Console", "Files", "ReadFiles", "Clock",
                                               "Random",  "Args",  "Env"};
            for (uint32_t i = 0; i < sizeof caps / sizeof *caps; i++)
                if (strlen(caps[i]) == length && memcmp(start, caps[i], length) == 0) {
                    t->tag = MO_CORE_TY_CAP;
                    t->as.cap = allocate(c, 1, sizeof *t->as.cap);
                    if (t->as.cap == NULL)
                        return NULL;
                    t->as.cap->tag = (enum mo_core_cap_tag)i;
                    found = true;
                    break;
                }
        }
        MO_ASSERT(found);
        if (t->tag != MO_CORE_TY_LIST && t->tag != MO_CORE_TY_SET && t->tag != MO_CORE_TY_MAP &&
            t->tag != MO_CORE_TY_OPTION && t->tag != MO_CORE_TY_RESULT)
            return t;
        bool opened = signature_take(cursor, '(');
        MO_ASSERT(opened);
    }
    do {
        MO_ASSERT(count < MO_LIMIT_BYTECODE_PARAMETERS);
        arguments[count] = signature_type(c, cursor, variables, depth + 1);
        if (arguments[count++] == NULL)
            return NULL;
    } while (signature_take(cursor, ','));
    bool closed = signature_take(cursor, ')');
    MO_ASSERT(closed);
    if (t->tag == MO_CORE_TY_TUPLE) {
        t->as.tuple.items = allocate(c, count, sizeof *arguments);
        if (t->as.tuple.items == NULL)
            return NULL;
        memcpy(t->as.tuple.items, arguments, (size_t)count * sizeof *arguments);
        t->as.tuple.count = count;
    } else if (t->tag == MO_CORE_TY_MAP || t->tag == MO_CORE_TY_RESULT) {
        MO_ASSERT(count == 2);
        if (t->tag == MO_CORE_TY_MAP) {
            t->as.map.k = arguments[0];
            t->as.map.v = arguments[1];
        } else {
            t->as.result.t = arguments[0];
            t->as.result.e = arguments[1];
        }
    } else {
        MO_ASSERT(count == 1);
        if (t->tag == MO_CORE_TY_LIST)
            t->as.list = arguments[0];
        else if (t->tag == MO_CORE_TY_SET)
            t->as.set = arguments[0];
        else
            t->as.option = arguments[0];
    }
    return t;
}
static MO_MUST_USE uint32_t operation_signature(struct compiler *c,
                                                const struct mo_codegen_operation *op,
                                                const mo_core_ty variables[4]) {
    uint32_t key[7] = {
        (uint32_t)op->opcode,   op->row, MO_NO_INDEX, MO_NO_INDEX, MO_NO_INDEX, MO_NO_INDEX,
        (uint32_t)op->expansion};
    for (uint32_t i = 0; i < 4; i++)
        if (variables[i] != NULL && (key[i + 2] = type(c, variables[i], 0)) == MO_NO_INDEX)
            return MO_NO_INDEX;
    mo_core_string bytes = {(const char *)key, sizeof key};
    struct mo_codegen_index *old = mo_codegen_find(c->operation_index, bytes);
    if (old != NULL)
        return old->value;
    const char *cursor = op->signature;
    uint32_t params[MO_LIMIT_BYTECODE_PARAMETERS], count = 0;
    bool function = strncmp(cursor, "fn(", 3) == 0;
    if (function) {
        cursor += 3;
        if (!signature_take(&cursor, ')')) {
            do {
                MO_ASSERT(count < MO_LIMIT_BYTECODE_PARAMETERS);
                mo_core_ty t = signature_type(c, &cursor, variables, 0);
                if (t == NULL || (params[count++] = type(c, t, 0)) == MO_NO_INDEX)
                    return MO_NO_INDEX;
            } while (signature_take(&cursor, ','));
            bool closed = signature_take(&cursor, ')');
            MO_ASSERT(closed);
        }
    }
    uint32_t result = MO_NO_INDEX;
    if (!function || signature_take(&cursor, ':')) {
        mo_core_ty t = signature_type(c, &cursor, variables, 0);
        if (t == NULL || (result = type(c, t, 0)) == MO_NO_INDEX)
            return MO_NO_INDEX;
    } else
        result = scalar(c, MO_TYPE_UNIT);
    uint32_t id = MO_NO_INDEX;
    enum mo_type_problem status = mo_types_intern(&c->types,
                                                  &(struct mo_type){.kind = MO_TYPE_FUNCTION,
                                                                    .parameters = params,
                                                                    .parameter_count = count,
                                                                    .result = result},
                                                  &id);
    if (type_result(c, status, id) == MO_NO_INDEX ||
        index_add(c, &c->operation_index, bytes, id, true) == NULL)
        return MO_NO_INDEX;
    while (*cursor == ' ')
        cursor++;
    uint32_t constrained = MO_NO_INDEX;
    uint8_t ability = 0;
    if (op->opcode == MO_OP_EQUAL) {
        constrained = key[2];
        ability = MO_CODEGEN_EQUAL;
    } else if (*cursor != 0) {
        MO_ASSERT(strncmp(cursor, "where ", 6) == 0);
        cursor += 6;
        unsigned slot = *cursor == 'T' ? 0u : *cursor == 'U' ? 1u : *cursor == 'K' ? 2u : 3u;
        MO_ASSERT((*cursor == 'T' || *cursor == 'U' || *cursor == 'K' || *cursor == 'V') &&
                  key[2 + slot] != MO_NO_INDEX && cursor[1] == ':' && cursor[2] == ' ');
        constrained = key[2 + slot];
        cursor += 3;
        MO_ASSERT(strcmp(cursor, "Equal") == 0 || strcmp(cursor, "Hash") == 0);
        ability = strcmp(cursor, "Equal") == 0 ? MO_CODEGEN_EQUAL : MO_CODEGEN_HASH;
    }
    if (ability != 0) {
        struct constraint *item =
            append(c, &c->constraints, 1, MO_LIMIT_CORE_NODES, MO_CODE_CODEGEN_MEMORY);
        if (item == NULL)
            return MO_NO_INDEX;
        *item = (struct constraint){constrained, ability, c->current, c->span, c->declaration};
    }
    return id;
}
static MO_MUST_USE uint32_t constant(struct compiler *c, struct mo_bytecode_constant value) {
    uint8_t key[20] = {0};
    memcpy(key, &value.type, sizeof value.type);
    enum mo_type_kind kind = c->types.items[value.type].kind;
    if (kind == MO_TYPE_TEXT)
        memcpy(key + 4, &value.value.string, sizeof value.value.string);
    else if (kind == MO_TYPE_FUNCTION)
        memcpy(key + 4, &value.value.function, sizeof value.value.function);
    else if (kind != MO_TYPE_BYTES && kind != MO_TYPE_BIGINT)
        memcpy(key + 4, value.value.immediate, 16);
    mo_core_string bytes = {(const char *)key, sizeof key};
    if (kind == MO_TYPE_BYTES || kind == MO_TYPE_BIGINT) {
        struct mo_bytecode_bytes data =
            kind == MO_TYPE_BYTES ? value.value.bytes : value.value.bigint.magnitude;
        size_t prefix = kind == MO_TYPE_BYTES ? 4u : 8u;
        char *payload = allocate(c, prefix + data.length, 1);
        if (payload == NULL)
            return MO_NO_INDEX;
        memcpy(payload, &value.type, 4);
        if (kind == MO_TYPE_BIGINT) {
            uint32_t sign = (uint32_t)value.value.bigint.sign;
            memcpy(payload + 4, &sign, 4);
        }
        if (data.length != 0)
            memcpy(payload + prefix, data.bytes, data.length);
        bytes = (mo_core_string){payload, prefix + data.length};
    }
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
    *ty = scalar(c, kinds[lit->tag]);
    if (*ty == MO_NO_INDEX)
        return MO_NO_INDEX;
    if (lit->tag == MO_CORE_LIT_BIG_INT) {
        /* Checked decimal digits are canonical. Borrow their key and distinguish signs,
         * so a repeated constant pays for neither conversion work nor conversion scratch. */
        unsigned sign = lit->as.big_int.negative ? 1u : 0u;
        mo_core_string digits = lit->as.big_int.digits;
        struct mo_codegen_index *old = mo_codegen_find(c->bigint_index[sign], digits);
        if (old != NULL)
            return old->value;
        struct mo_bytecode_constant k = {.type = *ty};
        enum mo_codegen_bigint_problem status =
            mo_codegen_bigint(&c->arena, lit->as.big_int, &c->bigint_work, &k.value.bigint);
        if (status != MO_CODEGEN_BIGINT_OK) {
            if (!problem(c,
                         status == MO_CODEGEN_BIGINT_LIMIT ? MO_CODE_CODEGEN_BIGINT_WORK
                                                           : MO_CODE_CODEGEN_MEMORY,
                         status == MO_CODEGEN_BIGINT_LIMIT
                             ? "BigInt conversion exceeds MO_LIMIT_CODEGEN_BIGINT_WORK."
                             : "No memory for BigInt literal conversion."))
                return MO_NO_INDEX;
        }
        uint32_t id = constant(c, k);
        if (id == MO_NO_INDEX || index_add(c, &c->bigint_index[sign], digits, id, false) == NULL)
            return MO_NO_INDEX;
        return id;
    }
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
    case MO_CORE_LIT_TEXT: {
        uint32_t s = string(c, lit->as.text);
        if (s == MO_NO_INDEX)
            return MO_NO_INDEX;
        struct mo_bytecode_constant k = {.type = *ty};
        k.value.string = s;
        return constant(c, k);
    }
    case MO_CORE_LIT_BYTES: {
        size_t count = lit->as.bytes.count;
        if (count > MO_LIMIT_STRING_SIZE) {
            if (!problem(c, MO_CODE_BYTECODE_TOO_BIG,
                         "Bytes literal exceeds MO_LIMIT_STRING_SIZE."))
                return MO_NO_INDEX;
        }
        uint8_t *payload = allocate(c, count, 1);
        if (payload == NULL)
            return MO_NO_INDEX;
        for (size_t i = 0; i < count; i++) {
            mo_core_string digits = lit->as.bytes.items[i].digits;
            unsigned byte = 0;
            for (size_t j = 0; j < digits.length; j++)
                byte = byte * 10 + (unsigned)(digits.bytes[j] - '0');
            payload[i] = (uint8_t)byte;
        }
        struct mo_bytecode_constant k = {.type = *ty};
        k.value.bytes = (struct mo_bytecode_bytes){(uint32_t)count, payload};
        return constant(c, k);
    }
    case MO_CORE_LIT_BIG_INT:
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
    if ((uint64_t)f->regs.count * f->blocks > MO_LIMIT_FUNCTION_REGISTER_BLOCKS) {
        if (!problem(c, MO_CODE_BYTECODE_TOO_BIG,
                     "Function exceeds MO_LIMIT_FUNCTION_REGISTER_BLOCKS."))
            return MO_NO_INDEX;
    }
    uint32_t flow = c->ownership.count;
    struct ownership *o =
        append(c, &c->ownership, 1, MO_LIMIT_CODEGEN_OWNERSHIP_SLOTS, MO_CODE_CODEGEN_MEMORY);
    if (o == NULL)
        return MO_NO_INDEX;
    *o = (struct ownership){.type = ty, .edges = MO_NO_INDEX};
    *r = (struct reg){
        .type = ty, .name = original, .flow = flow, .previous = MO_NO_INDEX, .next = MO_NO_INDEX};
    return id;
}
static MO_MUST_USE uint32_t binding_name(struct function *f, uint32_t node, uint32_t slot) {
    const uint32_t key[] = {node, slot};
    struct mo_codegen_index *entry =
        mo_codegen_find(f->bindings, (mo_core_string){(const char *)key, sizeof key});
    return entry == NULL ? MO_NO_INDEX : entry->value;
}
static MO_MUST_USE uint32_t constructor(struct compiler *c, uint32_t ty, mo_core_string key) {
    if (c->types.items[ty].kind == MO_TYPE_RECORD) {
        MO_ASSERT(key.length == 5 && memcmp(key.bytes, "Entry", 5) == 0);
        return 0;
    }
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
static void function_init(struct function *f) {
    f->regs.width = sizeof(struct reg);
    f->code.width = sizeof(uint32_t);
    f->spans.width = sizeof(struct mo_bytecode_span);
    f->names.width = sizeof(struct mo_bytecode_register_name);
    f->captures.width = sizeof(struct capture);
    f->joins.width = sizeof(struct join);
    f->changes.width = sizeof(struct change);
    f->owned = MO_NO_INDEX;
}
/* Capture lookup happens while all enclosing lexical scopes are still active. Intermediate
 * closures retain a grandparent's value even if only a nested closure uses it. */
static MO_MUST_USE uint32_t variable(struct compiler *c, struct function *f, mo_core_string key,
                                     unsigned depth) {
    MO_ASSERT(depth < MO_LIMIT_CORE_DEPTH);
    struct mo_codegen_index *entry = mo_codegen_find(f->locals, key);
    if (entry != NULL && entry->value != MO_NO_INDEX)
        return entry->value;
    MO_ASSERT(f->parent != NULL);
    uint32_t source = variable(c, f->parent, key, depth + 1);
    if (source == MO_NO_INDEX)
        return MO_NO_INDEX;
    mo_core_string capture_key = {(const char *)&source, sizeof source};
    struct mo_codegen_index *old = mo_codegen_find(f->capture_index, capture_key);
    uint32_t r;
    if (old != NULL) {
        r = old->value;
    } else {
        struct reg parent = ((struct reg *)f->parent->regs.items)[source];
        r = register_add(c, f, parent.type, parent.name);
        struct capture *item =
            append(c, &f->captures, 1, MO_LIMIT_FIELDS, MO_CODE_BYTECODE_TOO_BIG);
        if (r == MO_NO_INDEX || item == NULL ||
            index_add(c, &f->capture_index, capture_key, r, true) == NULL)
            return MO_NO_INDEX;
        *item = (struct capture){source, r};
    }
    entry = index_add(c, &f->locals, key, r, false);
    if (entry == NULL)
        return MO_NO_INDEX;
    entry->value = r;
    return r;
}
static MO_MUST_USE uint32_t suffixed_name(struct compiler *c, mo_core_string base,
                                          const char *suffix);
static MO_MUST_USE struct node *prepare(struct compiler *c, struct function *f, mo_core_expr expr,
                                        uint32_t expected, unsigned depth);
/* Only inference failures need a later context. Resolve them once, without retrying the Core
 * walk: an if/case may infer from a later arm and type an earlier stopping lambda. */
static MO_MUST_USE bool context(struct compiler *c, struct function *f, struct node *n,
                                uint32_t expected, unsigned depth) {
    MO_ASSERT(depth < MO_LIMIT_CORE_DEPTH);
    if (expected == MO_NO_INDEX || n->type != MO_NO_INDEX)
        return true;
    if (n->core->tag == MO_CORE_EXPR_STOP || n->core->tag == MO_CORE_EXPR_FAIL ||
        n->core->tag == MO_CORE_EXPR_JUMP)
        return true;
    if (n->core->tag == MO_CORE_EXPR_LAM) {
        struct function *lifted = &c->bodies[n->index];
        uint32_t result = c->types.items[expected].result;
        c->funs[n->index].result_type = result;
        lifted->closure_type = expected;
        if (!context(c, lifted, lifted->body, result, depth + 1))
            return false;
    } else if (n->core->tag == MO_CORE_EXPR_LET) {
        if (!context(c, f, n->children[1], expected, depth + 1))
            return false;
        n->type = n->children[1]->type;
        n->reg = n->children[1]->reg;
        n->temporary = n->children[1]->temporary || n->reg == n->binds[0];
        return true;
    } else if (n->core->tag == MO_CORE_EXPR_JOIN) {
        for (uint32_t i = 0; i < n->count; i++)
            if (!context(c, f, n->children[i], expected, depth + 1))
                return false;
    } else {
        MO_ASSERT(n->core->tag == MO_CORE_EXPR_IF || n->core->tag == MO_CORE_EXPR_CASE);
        for (uint32_t i = 1; i < n->count; i++) {
            struct node *body =
                n->core->tag == MO_CORE_EXPR_CASE ? n->children[i]->children[0] : n->children[i];
            if (!context(c, f, body, expected, depth + 1))
                return false;
        }
    }
    n->type = expected;
    n->reg = register_add(c, f, expected, MO_NO_INDEX);
    return n->reg != MO_NO_INDEX;
}
static MO_MUST_USE bool lambda(struct compiler *c, struct function *parent, struct node *n,
                               uint32_t preorder, uint32_t expected, unsigned depth) {
    MO_ASSERT(c->function_count < c->function_capacity);
    uint32_t index = c->function_count++;
    n->index = index;
    struct function *f = &c->bodies[index];
    *f = (struct function){.parent = parent,
                           .root = parent->root,
                           .meta = parent->meta,
                           .source = parent->source,
                           .preorder = parent->preorder,
                           .bindings = parent->bindings,
                           .declaration = n->span};
    function_init(f);
    uint32_t number = ++f->root->lambda_number;
    struct mo_codegen_index *named_closure = mo_codegen_find(
        f->root->closure_names, (mo_core_string){(const char *)&preorder, sizeof preorder});
    char suffix[32];
    int length = snprintf(suffix, sizeof suffix, ".fn.%u", number);
    MO_ASSERT(length > 0 && (size_t)length < sizeof suffix);
    uint32_t name_id =
        named_closure == NULL ? suffixed_name(c, f->source->name, suffix) : named_closure->value;
    uint32_t parent_index = (uint32_t)(parent - c->bodies);
    mo_core_params ps = n->core->as.lam.params;
    if (ps.count > MO_LIMIT_PARAMETERS)
        return problem(c, MO_CODE_BYTECODE_TOO_BIG, "Closure exceeds MO_LIMIT_PARAMETERS.");
    struct mo_parameter *parameters = allocate(c, ps.count, sizeof *parameters);
    uint32_t *types = allocate(c, ps.count, sizeof *types);
    if (name_id == MO_NO_INDEX || parameters == NULL || types == NULL)
        return false;
    c->parameters[index] = parameters;
    c->funs[index] = (struct mo_bytecode_function){.name = name_id,
                                                   .module = c->funs[parent_index].module,
                                                   .kind = MO_FUNCTION_CLOSURE,
                                                   .spec_code = c->funs[parent_index].spec_code,
                                                   .parameter_count = (uint32_t)ps.count,
                                                   .parameters = parameters};
    for (uint32_t i = 0; i < ps.count; i++) {
        if (!name_valid(c, ps.items[i].name))
            return false;
        types[i] = type(c, ps.items[i].ty, 0);
        parameters[i] = (struct mo_parameter){types[i], MO_PARAMETER_OWNED};
        if (register_add(c, f, types[i], binding_name(parent, preorder, i)) == MO_NO_INDEX)
            return false;
    }
    uint32_t env = MO_NO_INDEX;
    enum mo_type_problem status =
        mo_types_intern(&c->types, &(struct mo_type){.kind = MO_TYPE_ENV, .function = index}, &env);
    if (type_result(c, status, env) == MO_NO_INDEX ||
        register_add(c, f, env, MO_NO_INDEX) == MO_NO_INDEX)
        return false;
    for (uint32_t i = (uint32_t)ps.count; i-- > 0;) {
        struct mo_codegen_index *entry = index_add(c, &f->locals, ps.items[i].name, i, false);
        if (entry == NULL)
            return false;
        entry->value = i;
    }
    uint32_t result = expected == MO_NO_INDEX ? MO_NO_INDEX : c->types.items[expected].result;
    f->body = prepare(c, f, n->core->as.lam.body, result, depth + 1);
    parent->preorder = f->preorder;
    if (f->body == NULL)
        return false;
    if (result == MO_NO_INDEX)
        result = f->body->type;
    c->funs[index].result_type = result;
    if (result != MO_NO_INDEX) {
        status = mo_types_intern(&c->types,
                                 &(struct mo_type){.kind = MO_TYPE_FUNCTION,
                                                   .parameters = types,
                                                   .parameter_count = (uint32_t)ps.count,
                                                   .result = result},
                                 &n->type);
        if (type_result(c, status, n->type) == MO_NO_INDEX)
            return false;
    }
    uint32_t *captures = allocate(c, f->captures.count, sizeof *captures);
    n->binds = allocate(c, f->captures.count, sizeof *n->binds);
    if (captures == NULL || n->binds == NULL)
        return false;
    n->bind_count = f->captures.count;
    for (uint32_t i = 0; i < f->captures.count; i++) {
        const struct capture *item = &((struct capture *)f->captures.items)[i];
        n->binds[i] = item->source;
        captures[i] = ((struct reg *)f->regs.items)[item->reg].type;
    }
    c->funs[index].capture_count = f->captures.count;
    c->funs[index].capture_types = captures;
    f->closure_type = n->type;
    return true;
}
static MO_MUST_USE bool children(struct compiler *c, struct function *f, struct node *n,
                                 mo_core_expr_list xs, uint32_t expected, unsigned depth) {
    n->count = (uint32_t)xs.count;
    n->children = allocate(c, xs.count, sizeof *n->children);
    if (n->children == NULL)
        return false;
    for (uint32_t i = 0; i < n->count; i++) {
        uint32_t child_type = MO_NO_INDEX;
        if (n->core->tag == MO_CORE_EXPR_CALL)
            child_type = c->funs[n->index].parameters[i].type;
        else if (n->core->tag == MO_CORE_EXPR_JUMP) {
            const struct node *target = ((struct join *)f->joins.items)[n->index].node;
            child_type = ((struct reg *)f->regs.items)[target->binds[i]].type;
        } else if (n->core->tag == MO_CORE_EXPR_CON) {
            const struct mo_type *t = &c->types.items[n->type];
            child_type =
                t->kind == MO_TYPE_RECORD ? t->fields[i].type : t->cases[n->index].fields[i].type;
        } else if (n->core->tag == MO_CORE_EXPR_IF)
            child_type = i == 0 ? scalar(c, MO_TYPE_BOOL) : expected;
        else if (n->core->tag == MO_CORE_EXPR_PRIM) {
            uint32_t slot = n->operation.literal == MO_CODEGEN_LITERAL_LIST  ? 1
                            : n->operation.literal == MO_CODEGEN_LITERAL_MAP ? 1 + i % 2
                                                                             : i;
            child_type = c->types.items[n->signature].parameters[slot];
        }
        n->children[i] = prepare(c, f, xs.items[i], child_type, depth + 1);
        if (n->children[i] == NULL)
            return false;
    }
    return true;
}
static MO_MUST_USE struct node *prepare(struct compiler *c, struct function *f, mo_core_expr expr,
                                        uint32_t expected, unsigned depth) {
    if (depth >= MO_LIMIT_CORE_DEPTH) {
        if (!problem(c, MO_CODE_NESTED_TOO_DEEPLY,
                     "Core expression nesting exceeds MO_LIMIT_CORE_DEPTH."))
            return NULL;
    }
    uint32_t preorder = f->preorder++;
    struct node *n = allocate(c, 1, sizeof *n);
    if (n == NULL)
        return NULL;
    *n = (struct node){.core = expr,
                       .type = MO_NO_INDEX,
                       .reg = MO_NO_INDEX,
                       .index = MO_NO_INDEX,
                       .temporary = true};
    if (f->meta != NULL)
        n->span = f->meta->body.spans.items[preorder];
    c->span = n->span;
    mo_core_expr small[3];
    switch (expr->tag) {
    case MO_CORE_EXPR_LIT:
        n->index = literal(c, expr->as.lit, &n->type);
        break;
    case MO_CORE_EXPR_VAR: {
        n->reg = variable(c, f, expr->as.var, 0);
        if (n->reg == MO_NO_INDEX)
            return NULL;
        n->type = ((struct reg *)f->regs.items)[n->reg].type;
        n->temporary = false;
        return n;
    }
    case MO_CORE_EXPR_GLOBAL: {
        struct mo_codegen_index *entry = mo_codegen_find(c->global_index, expr->as.global);
        MO_ASSERT(entry != NULL);
        n->index = entry->value;
        n->type = c->globals[n->index].type;
        break;
    }
    case MO_CORE_EXPR_LET: {
        if (!name_valid(c, expr->as.let_.name))
            return NULL;
        n->children = allocate(c, 2, sizeof *n->children);
        n->binds = allocate(c, 1, sizeof *n->binds);
        if (n->children == NULL || n->binds == NULL)
            return NULL;
        n->count = 2;
        n->bind_count = 1;
        n->children[0] = prepare(c, f, expr->as.let_.bound, MO_NO_INDEX, depth + 1);
        if (n->children[0] == NULL)
            return NULL;
        n->binds[0] = register_add(c, f, n->children[0]->type, binding_name(f, preorder, 0));
        struct mo_codegen_index *entry =
            index_add(c, &f->locals, expr->as.let_.name, MO_NO_INDEX, false);
        if (entry == NULL || n->binds[0] == MO_NO_INDEX)
            return NULL;
        uint32_t saved = entry->value;
        entry->value = n->binds[0];
        n->children[1] = prepare(c, f, expr->as.let_.body, expected, depth + 1);
        entry->value = saved;
        if (n->children[1] == NULL)
            return NULL;
        n->type = n->children[1]->type;
        n->reg = n->children[1]->reg;
        n->temporary = n->children[1]->temporary || n->reg == n->binds[0];
        return n;
    }
    case MO_CORE_EXPR_CALL: {
        struct mo_codegen_index *entry = mo_codegen_find(c->fun_index, expr->as.call.name);
        MO_ASSERT(entry != NULL);
        n->index = entry->value;
        n->type = c->funs[n->index].result_type;
        if (!children(c, f, n, expr->as.call.args, MO_NO_INDEX, depth))
            return NULL;
        break;
    }
    case MO_CORE_EXPR_IF:
        small[0] = expr->as.if_.cond;
        small[1] = expr->as.if_.then_;
        small[2] = expr->as.if_.else_;
        if (!children(c, f, n, (mo_core_expr_list){small, 3}, expected, depth))
            return NULL;
        n->type = n->children[1]->type != MO_NO_INDEX ? n->children[1]->type : n->children[2]->type;
        if (n->type == MO_NO_INDEX)
            n->type = expected;
        for (uint32_t i = 1; i < n->count; i++)
            if (!context(c, f, n->children[i], n->type, depth + 1))
                return NULL;
        break;
    case MO_CORE_EXPR_CON:
        n->type = type(c, expr->as.con.ty, 0);
        if (n->type == MO_NO_INDEX)
            return NULL;
        n->index = constructor(c, n->type, expr->as.con.ctor);
        if (n->index == MO_NO_INDEX || !children(c, f, n, expr->as.con.args, MO_NO_INDEX, depth))
            return NULL;
        break;
    case MO_CORE_EXPR_CASE: {
        n->count = 1 + (uint32_t)expr->as.case_.arms.count;
        n->children = allocate(c, n->count, sizeof *n->children);
        n->cases = allocate(c, n->count - 1, sizeof *n->cases);
        if (n->children == NULL || n->cases == NULL)
            return NULL;
        n->children[0] = prepare(c, f, expr->as.case_.expr, MO_NO_INDEX, depth + 1);
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
            const struct mo_type *t = &c->types.items[ty];
            struct mo_type_case record = {.fields = t->fields, .field_count = t->field_count};
            const struct mo_type_case *ctor =
                t->kind == MO_TYPE_RECORD ? &record : &t->cases[case_id];
            struct mo_codegen_index **entries = allocate(c, arm->names.count, sizeof *entries);
            uint32_t *saved = allocate(c, arm->names.count, sizeof *saved);
            uint32_t *regs = allocate(c, arm->names.count, sizeof *regs);
            if (entries == NULL || saved == NULL || regs == NULL)
                return NULL;
            for (uint32_t j = 0; j < ctor->field_count; j++) {
                c->span = n->span;
                if (!name_valid(c, arm->names.items[j]))
                    return NULL;
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
            struct node *body = prepare(c, f, arm->body, expected, depth + 1);
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
        if (n->type == MO_NO_INDEX)
            n->type = expected;
        for (uint32_t i = 1; i < n->count; i++)
            if (!context(c, f, n->children[i]->children[0], n->type, depth + 1))
                return NULL;
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
    case MO_CORE_EXPR_TUPLE: {
        if (expr->as.tuple.count < 2) {
            if (!problem(c, MO_CODE_CODEGEN_REPRESENTATION,
                         "Zero/one-element tuples cannot come from lowering."))
                return NULL;
        }
        if (!children(c, f, n, expr->as.tuple, MO_NO_INDEX, depth))
            return NULL;
        uint32_t *params = allocate(c, n->count, sizeof *params);
        if (params == NULL)
            return NULL;
        for (uint32_t i = 0; i < n->count; i++)
            params[i] = n->children[i]->type;
        enum mo_type_problem status = mo_types_intern(
            &c->types,
            &(struct mo_type){
                .kind = MO_TYPE_TUPLE, .parameters = params, .parameter_count = n->count},
            &n->type);
        if (type_result(c, status, n->type) == MO_NO_INDEX)
            return NULL;
        break;
    }
    case MO_CORE_EXPR_PROJ:
        small[0] = expr->as.proj.expr;
        if (!children(c, f, n, (mo_core_expr_list){small, 1}, MO_NO_INDEX, depth))
            return NULL;
        n->index = 0;
        for (size_t i = 0; i < expr->as.proj.index.digits.length; i++)
            n->index = n->index * 10u + (unsigned)(expr->as.proj.index.digits.bytes[i] - '0');
        n->type = c->types.items[n->children[0]->type].parameters[n->index];
        break;
    case MO_CORE_EXPR_FAIL:
        small[0] = expr->as.fail;
        if (!children(c, f, n, (mo_core_expr_list){small, 1}, MO_NO_INDEX, depth))
            return NULL;
        return n;
    case MO_CORE_EXPR_PRIM: {
        const struct mo_core_prim *p = &expr->as.prim.prim;
        if (!mo_codegen_operation(p, &n->operation)) {
            if (!unsupported(c, "The Core primitive has no code generation route."))
                return NULL;
        }
        mo_core_ty variables[4] = {NULL};
        if (p->tag == MO_CORE_PRIM_LIST) {
            variables[0] = p->as.list.t;
            if (p->as.list.op.tag == MO_CORE_LIST_OP_ZIP)
                variables[1] = p->as.list.op.as.zip;
        } else if (p->tag == MO_CORE_PRIM_MAP) {
            variables[2] = p->as.map.k;
            variables[3] = p->as.map.v;
        } else if (p->tag == MO_CORE_PRIM_SET)
            variables[0] = p->as.set.t;
        else if (p->tag == MO_CORE_PRIM_EQ)
            variables[0] = p->as.eq;
        n->signature = operation_signature(c, &n->operation, variables);
        if (n->signature == MO_NO_INDEX)
            return NULL;
        n->type =
            n->operation.compare ? scalar(c, MO_TYPE_BOOL) : c->types.items[n->signature].result;
        if (!children(c, f, n, expr->as.prim.args, MO_NO_INDEX, depth))
            return NULL;
        if (n->operation.expansion == MO_CODEGEN_EXPAND_RANGE) {
            n->bind_count = 6;
            n->binds = allocate(c, n->bind_count, sizeof *n->binds);
            n->cases = allocate(c, 2, sizeof *n->cases);
            struct mo_codegen_operation convert = mo_codegen_primitive(MO_PRIM_INT_TO_TEXT),
                                        concat = mo_codegen_primitive(MO_PRIM_TEXT_CONCAT);
            if (n->binds == NULL || n->cases == NULL)
                return NULL;
            n->cases[0] = operation_signature(c, &convert, variables);
            n->cases[1] = operation_signature(c, &concat, variables);
            uint32_t integer = scalar(c, MO_TYPE_INT), separator = string(c, text(".."));
            if (n->cases[0] == MO_NO_INDEX || n->cases[1] == MO_NO_INDEX ||
                integer == MO_NO_INDEX || separator == MO_NO_INDEX)
                return NULL;
            n->index = constant(
                c, (struct mo_bytecode_constant){.type = n->type, .value.string = separator});
            if (n->index == MO_NO_INDEX)
                return NULL;
            for (uint32_t i = 0; i < n->bind_count; i++)
                if ((n->binds[i] = register_add(c, f, i < 2 ? integer : n->type, MO_NO_INDEX)) ==
                    MO_NO_INDEX)
                    return NULL;
        } else if (n->operation.expansion == MO_CODEGEN_EXPAND_UNCONS) {
            n->bind_count = 5;
            n->binds = allocate(c, n->bind_count, sizeof *n->binds);
            n->cases = allocate(c, 2, sizeof *n->cases);
            struct mo_codegen_operation first = mo_codegen_primitive(MO_PRIM_LIST_FIRST),
                                        rest = mo_codegen_primitive(MO_PRIM_LIST_DROP);
            if (n->binds == NULL || n->cases == NULL)
                return NULL;
            n->cases[0] = operation_signature(c, &first, variables);
            n->cases[1] = operation_signature(c, &rest, variables);
            if (n->cases[0] == MO_NO_INDEX || n->cases[1] == MO_NO_INDEX)
                return NULL;
            uint32_t option = c->types.items[n->cases[0]].result,
                     pair = c->types.items[n->type].cases[1].fields[0].type;
            const uint32_t types[] = {option, c->types.items[option].cases[1].fields[0].type,
                                      c->types.items[n->signature].parameters[0],
                                      c->types.items[n->cases[1]].parameters[1], pair};
            n->index = constant(
                c, (struct mo_bytecode_constant){.type = types[3], .value.immediate = {1}});
            if (n->index == MO_NO_INDEX)
                return NULL;
            for (uint32_t i = 0; i < n->bind_count; i++)
                if ((n->binds[i] = register_add(c, f, types[i], MO_NO_INDEX)) == MO_NO_INDEX)
                    return NULL;
        }
        if (n->operation.compare) {
            n->binds = allocate(c, 2, sizeof *n->binds);
            if (n->binds == NULL)
                return NULL;
            uint32_t ordering = c->types.items[n->signature].result;
            for (uint32_t i = 0; i < 2; i++)
                if ((n->binds[i] = register_add(c, f, ordering, MO_NO_INDEX)) == MO_NO_INDEX)
                    return NULL;
        }
        if (n->operation.literal != MO_CODEGEN_LITERAL_NONE) {
            bool map = n->operation.literal == MO_CODEGEN_LITERAL_MAP;
            struct mo_codegen_operation empty =
                mo_codegen_primitive(map ? MO_PRIM_MAP_EMPTY : MO_PRIM_LIST_EMPTY);
            n->index = operation_signature(c, &empty, variables);
            n->bind_count = n->count / (map ? 2u : 1u);
            n->binds = allocate(c, n->bind_count, sizeof *n->binds);
            if (n->index == MO_NO_INDEX || n->binds == NULL)
                return NULL;
            for (uint32_t i = 0; i < n->bind_count; i++)
                if ((n->binds[i] = register_add(c, f, n->type, MO_NO_INDEX)) == MO_NO_INDEX)
                    return NULL;
        }
        break;
    }
    case MO_CORE_EXPR_LAM:
        if (!lambda(c, f, n, preorder, expected, depth))
            return NULL;
        break;
    case MO_CORE_EXPR_APP: {
        n->count = 1 + (uint32_t)expr->as.app.args.count;
        n->children = allocate(c, n->count, sizeof *n->children);
        if (n->children == NULL)
            return NULL;
        n->children[0] = prepare(c, f, expr->as.app.f, MO_NO_INDEX, depth + 1);
        if (n->children[0] == NULL)
            return NULL;
        n->type = c->types.items[n->children[0]->type].result;
        for (uint32_t i = 1; i < n->count; i++) {
            uint32_t arg_type = c->types.items[n->children[0]->type].parameters[i - 1];
            n->children[i] = prepare(c, f, expr->as.app.args.items[i - 1], arg_type, depth + 1);
            if (n->children[i] == NULL)
                return NULL;
        }
        break;
    }
    case MO_CORE_EXPR_JOIN: {
        mo_core_params ps = expr->as.join.params;
        if (!name_valid(c, expr->as.join.name))
            return NULL;
        n->index = f->joins.count;
        struct join *item = append(c, &f->joins, 1, MO_LIMIT_CORE_NODES, MO_CODE_CODEGEN_MEMORY);
        n->count = 2;
        n->bind_count = (uint32_t)ps.count;
        n->binds = allocate(c, ps.count, sizeof *n->binds);
        n->children = allocate(c, 2, sizeof *n->children);
        struct mo_codegen_index **entries = allocate(c, ps.count, sizeof *entries);
        uint32_t *saved = allocate(c, ps.count, sizeof *saved);
        struct mo_codegen_index *entry =
            index_add(c, &f->join_names, expr->as.join.name, MO_NO_INDEX, false);
        if (item == NULL || n->binds == NULL || n->children == NULL || entries == NULL ||
            saved == NULL || entry == NULL)
            return NULL;
        *item = (struct join){.node = n, .offset = MO_NO_INDEX, .checkpoint = MO_NO_INDEX};
        for (uint32_t i = 0; i < ps.count; i++) {
            if (!name_valid(c, ps.items[i].name))
                return NULL;
            uint32_t ty = type(c, ps.items[i].ty, 0);
            n->binds[i] = register_add(c, f, ty, binding_name(f, preorder, i));
            entries[i] = index_add(c, &f->locals, ps.items[i].name, MO_NO_INDEX, false);
            if (n->binds[i] == MO_NO_INDEX || entries[i] == NULL)
                return NULL;
        }
        uint32_t prior = entry->value;
        entry->value = n->index;
        for (uint32_t i = (uint32_t)ps.count; i-- > 0;) {
            saved[i] = entries[i]->value;
            entries[i]->value = n->binds[i];
        }
        n->children[0] = prepare(c, f, expr->as.join.body, expected, depth + 1);
        for (uint32_t i = 0; i < ps.count; i++)
            entries[i]->value = saved[i];
        n->children[1] =
            n->children[0] == NULL ? NULL : prepare(c, f, expr->as.join.rest, expected, depth + 1);
        entry->value = prior;
        if (n->children[0] == NULL || n->children[1] == NULL)
            return NULL;
        n->type = expected != MO_NO_INDEX               ? expected
                  : n->children[1]->type != MO_NO_INDEX ? n->children[1]->type
                                                        : n->children[0]->type;
        for (uint32_t i = 0; i < n->count; i++)
            if (!context(c, f, n->children[i], n->type, depth + 1))
                return NULL;
        break;
    }
    case MO_CORE_EXPR_JUMP: {
        struct mo_codegen_index *entry = mo_codegen_find(f->join_names, expr->as.jump.name);
        MO_ASSERT(entry != NULL && entry->value != MO_NO_INDEX);
        n->index = entry->value;
        if (!children(c, f, n, expr->as.jump.args, MO_NO_INDEX, depth))
            return NULL;
        n->bind_count = n->count;
        n->binds = allocate(c, n->count, sizeof *n->binds);
        if (n->binds == NULL)
            return NULL;
        const struct node *target = ((struct join *)f->joins.items)[n->index].node;
        for (uint32_t i = 0; i < n->count; i++) {
            uint32_t ty = ((struct reg *)f->regs.items)[target->binds[i]].type;
            n->binds[i] = register_add(c, f, ty, MO_NO_INDEX);
            if (n->binds[i] == MO_NO_INDEX)
                return NULL;
        }
        return n;
    }
    }
    if (c->error != 0)
        return NULL;
    n->reg = register_add(c, f, n->type, MO_NO_INDEX);
    return c->error == 0 ? n : NULL;
}

static MO_MUST_USE bool reference(struct compiler *c, uint32_t type_id) {
    return type_id != MO_NO_INDEX && mo_type_is_reference(&c->types, type_id);
}
static MO_MUST_USE bool abilities_valid(struct compiler *c) {
    uint8_t *flags = mo_codegen_abilities(&c->arena, &c->types);
    if (flags == NULL)
        return problem(c, MO_CODE_CODEGEN_MEMORY,
                       "No scratch memory for runtime ability inference.");
    for (uint32_t i = 0; i < c->types.count; i++) {
        const struct mo_type *t = &c->types.items[i];
        if ((t->kind == MO_TYPE_MAP || t->kind == MO_TYPE_SET) &&
            (flags[t->parameters[0]] & MO_CODEGEN_HASH) == 0)
            return problem(c, MO_CODE_CODEGEN_REPRESENTATION,
                           "Map keys and Set elements without Hash cannot come from lowering.");
    }
    for (uint32_t i = 0; i < c->constraints.count; i++) {
        const struct constraint *item = &((struct constraint *)c->constraints.items)[i];
        if ((flags[item->type] & item->ability) == 0) {
            c->current = item->function;
            c->span = item->span;
            c->declaration = item->declaration;
            return problem(c, MO_CODE_CODEGEN_REPRESENTATION,
                           item->ability == MO_CODEGEN_EQUAL
                               ? "An operation on a type without Equal cannot come from lowering."
                               : "An operation on a type without Hash cannot come from lowering.");
        }
    }
    return true;
}
static MO_MUST_USE uint32_t flow_id(struct function *f, uint32_t r) {
    return r == MO_NO_INDEX ? MO_NO_INDEX : ((struct reg *)f->regs.items)[r].flow;
}
static MO_MUST_USE bool operation_owned(const struct node *n, uint32_t slot) {
    return n->operation.literal != MO_CODEGEN_LITERAL_NONE || n->operation.params[slot] == 'o';
}
static void seed(struct compiler *c, uint32_t id) {
    if (id != MO_NO_INDEX) {
        struct ownership *o = &((struct ownership *)c->ownership.items)[id];
        if (reference(c, o->type))
            o->owned = true;
    }
}
static MO_MUST_USE bool depends(struct compiler *c, uint32_t from, uint32_t to) {
    if (from == MO_NO_INDEX || to == MO_NO_INDEX || from == to)
        return true;
    struct ownership *o = c->ownership.items;
    if (!reference(c, o[from].type) || !reference(c, o[to].type))
        return true;
    uint32_t id = c->edges.count;
    struct edge *e =
        append(c, &c->edges, 1, MO_LIMIT_CODEGEN_OWNERSHIP_EDGES, MO_CODE_CODEGEN_MEMORY);
    if (e == NULL)
        return false;
    *e = (struct edge){to, o[from].edges};
    o[from].edges = id;
    return true;
}
/* Dependencies propagate ownership once per slot. Tail calls add the reverse dependency:
 * an argument owned by the disappearing frame makes the callee's parameter owned too. */
static MO_MUST_USE bool borrow_walk(struct compiler *c, struct function *f, struct node *n,
                                    bool tail, unsigned depth) {
    MO_ASSERT(depth < MO_LIMIT_CORE_DEPTH);
    uint32_t own = flow_id(f, n->reg);
    if (n->core->tag == MO_CORE_EXPR_CASE) {
        if (!borrow_walk(c, f, n->children[0], false, depth + 1))
            return false;
        for (uint32_t i = 1; i < n->count; i++) {
            struct node *arm = n->children[i], *body = arm->children[0];
            for (uint32_t j = 0; j < arm->bind_count; j++) {
                uint32_t field = flow_id(f, arm->binds[j]);
                seed(c, field);
            }
            if (!borrow_walk(c, f, body, tail, depth + 1) ||
                !depends(c, own, flow_id(f, body->reg)) || !depends(c, flow_id(f, body->reg), own))
                return false;
        }
    } else {
        for (uint32_t i = 0; i < n->count; i++) {
            bool child_tail = tail && ((n->core->tag == MO_CORE_EXPR_LET && i == 1) ||
                                       (n->core->tag == MO_CORE_EXPR_IF && i != 0) ||
                                       n->core->tag == MO_CORE_EXPR_JOIN);
            if (!borrow_walk(c, f, n->children[i], child_tail, depth + 1))
                return false;
        }
    }
    if (n->core->tag == MO_CORE_EXPR_LET) {
        uint32_t bound = flow_id(f, n->children[0]->reg), binding = flow_id(f, n->binds[0]);
        if (!depends(c, binding, bound) || !depends(c, bound, binding))
            return false;
    } else if (n->core->tag == MO_CORE_EXPR_IF || n->core->tag == MO_CORE_EXPR_JOIN) {
        for (uint32_t i = n->core->tag == MO_CORE_EXPR_JOIN ? 0u : 1u; i < n->count; i++)
            if (!depends(c, own, flow_id(f, n->children[i]->reg)) ||
                !depends(c, flow_id(f, n->children[i]->reg), own))
                return false;
        if (n->core->tag == MO_CORE_EXPR_JOIN)
            for (uint32_t i = 0; i < n->bind_count; i++)
                seed(c, flow_id(f, n->binds[i]));
    } else if (n->core->tag == MO_CORE_EXPR_JUMP) {
        for (uint32_t i = 0; i < n->count; i++)
            seed(c, flow_id(f, n->children[i]->reg));
    } else if (n->core->tag == MO_CORE_EXPR_CALL) {
        struct function *callee = &c->bodies[n->index];
        for (uint32_t i = 0; i < n->count; i++) {
            uint32_t arg = flow_id(f, n->children[i]->reg), param = flow_id(callee, i);
            if (!depends(c, param, arg) || (tail && !depends(c, arg, param)))
                return false;
        }
        seed(c, own);
    } else if (n->core->tag == MO_CORE_EXPR_PRIM) {
        for (uint32_t i = 0; i < n->count; i++)
            if (operation_owned(n, i))
                seed(c, flow_id(f, n->children[i]->reg));
        seed(c, own);
    } else if (n->core->tag == MO_CORE_EXPR_LAM) {
        for (uint32_t i = 0; i < n->bind_count; i++)
            seed(c, flow_id(f, n->binds[i]));
        seed(c, own);
    } else if (n->core->tag == MO_CORE_EXPR_APP || n->core->tag == MO_CORE_EXPR_CON ||
               n->core->tag == MO_CORE_EXPR_TUPLE) {
        for (uint32_t i = 0; i < n->count; i++)
            seed(c, flow_id(f, n->children[i]->reg));
        seed(c, own);
    } else if (n->core->tag == MO_CORE_EXPR_PROJ) {
        seed(c, own);
    }
    if (tail && n->core->tag != MO_CORE_EXPR_STOP)
        seed(c, own);
    return true;
}
static MO_MUST_USE bool borrow_infer(struct compiler *c) {
    for (uint32_t i = 0; i < c->function_count; i++) {
        struct function *f = &c->bodies[i];
        if (c->funs[i].kind == MO_FUNCTION_INIT)
            continue;
        if (c->funs[i].kind != MO_FUNCTION_FUNCTION)
            for (uint32_t j = 0; j < c->funs[i].parameter_count; j++)
                seed(c, flow_id(f, j));
        for (uint32_t j = 0; j < f->captures.count; j++)
            seed(c, flow_id(f, ((struct capture *)f->captures.items)[j].reg));
        if (!borrow_walk(c, f, f->body, true, 0))
            return false;
    }
    uint32_t *queue = allocate(c, c->ownership.count, sizeof *queue);
    if (queue == NULL)
        return false;
    struct ownership *slots = c->ownership.items;
    const struct edge *edges = c->edges.items;
    uint32_t end = 0;
    for (uint32_t i = 0; i < c->ownership.count; i++)
        if (slots[i].owned)
            queue[end++] = i;
    for (uint32_t at = 0; at < end; at++) {
        for (uint32_t e = slots[queue[at]].edges; e != MO_NO_INDEX; e = edges[e].next) {
            uint32_t to = edges[e].to;
            if (!slots[to].owned) {
                slots[to].owned = true;
                queue[end++] = to;
            }
        }
    }
    for (uint32_t i = 0; i < c->function_count; i++)
        for (uint32_t j = 0; j < c->funs[i].parameter_count; j++)
            if (reference(c, c->parameters[i][j].type) && !slots[flow_id(&c->bodies[i], j)].owned)
                c->parameters[i][j].ownership = MO_PARAMETER_BORROWED;
    return true;
}

/* Backward liveness is independent of ownership inference. Join inputs grow monotonically
 * to a fixed point; temporary sets are bounded by registers × control nodes, and every
 * node/operand visit and set-cell operation is charged to the per-function work ceiling. */
static MO_MUST_USE bool live_charge(struct compiler *c, struct function *f, uint64_t count) {
    if (count > MO_LIMIT_CODEGEN_LIVE_WORK - f->live_work)
        return problem(c, MO_CODE_CODEGEN_LIVE_WORK,
                       "Reference liveness exceeds MO_LIMIT_CODEGEN_LIVE_WORK.");
    f->live_work += count;
    return true;
}
static void live_use(struct compiler *c, struct function *f, bool *live, uint32_t r) {
    if (r != MO_NO_INDEX && reference(c, ((struct reg *)f->regs.items)[r].type)) {
        MO_ASSERT(r < f->live_count);
        live[r] = true;
    }
}
static MO_MUST_USE bool operand_owned(struct compiler *c, const struct node *n, uint32_t i) {
    if (n->core->tag == MO_CORE_EXPR_CALL)
        return c->funs[n->index].parameters[i].ownership == MO_PARAMETER_OWNED;
    if (n->core->tag == MO_CORE_EXPR_PRIM)
        return operation_owned(n, i);
    return true;
}
static MO_MUST_USE bool live_copy(struct compiler *c, struct function *f, bool *to,
                                  const bool *from) {
    if (!live_charge(c, f, f->live_count))
        return false;
    memcpy(to, from, (size_t)f->live_count * sizeof *to);
    return true;
}
static MO_MUST_USE bool live_clear(struct compiler *c, struct function *f, bool *live) {
    if (!live_charge(c, f, f->live_count))
        return false;
    memset(live, 0, (size_t)f->live_count * sizeof *live);
    return true;
}
static MO_MUST_USE bool live_union(struct compiler *c, struct function *f, bool *to,
                                   const bool *from, bool *changed) {
    if (!live_charge(c, f, f->live_count))
        return false;
    for (uint32_t i = 0; i < f->live_count; i++)
        if (from[i] && !to[i]) {
            to[i] = true;
            *changed = true;
        }
    return true;
}
static MO_MUST_USE bool live_walk(struct compiler *c, struct function *f, struct node *n,
                                  bool *live, bool *changed, unsigned depth) {
    MO_ASSERT(depth < MO_LIMIT_CORE_DEPTH);
    c->span = n->span;
    if (!live_charge(c, f, 2u + UINT64_C(3) * n->count + UINT64_C(2) * n->bind_count))
        return false;
    if (n->last == NULL) {
        uint32_t count = n->core->tag == MO_CORE_EXPR_LAM ? n->bind_count : n->count;
        n->last = allocate(c, count == 0 ? 1 : count, sizeof *n->last);
        if (n->last == NULL)
            return false;
    }
    if (n->core->tag == MO_CORE_EXPR_VAR)
        return true; /* The parent instruction already marked this lexical register. */
    if (n->core->tag == MO_CORE_EXPR_LET) {
        if (!live_walk(c, f, n->children[1], live, changed, depth + 1))
            return false;
        live[n->binds[0]] = false;
        uint32_t r = n->children[0]->reg;
        n->last[0] = r == MO_NO_INDEX || !live[r];
        live_use(c, f, live, r);
        return live_walk(c, f, n->children[0], live, changed, depth + 1);
    }
    if (n->reg != MO_NO_INDEX)
        live[n->reg] = false;
    bool branch = n->core->tag == MO_CORE_EXPR_IF || n->core->tag == MO_CORE_EXPR_CASE;
    bool join = n->core->tag == MO_CORE_EXPR_JOIN;
    if (branch || join) {
        if (n->after == NULL) {
            f->live_controls++;
            if ((uint64_t)f->live_controls * f->live_count > MO_LIMIT_FUNCTION_REGISTER_BLOCKS)
                return problem(c, MO_CODE_BYTECODE_TOO_BIG,
                               "Function exceeds MO_LIMIT_FUNCTION_REGISTER_BLOCKS.");
            n->after = allocate(c, f->live_count, sizeof *n->after);
            n->work = allocate(c, f->live_count, sizeof *n->work);
            if (n->after == NULL || n->work == NULL)
                return false;
        }
        if (!live_copy(c, f, n->after, live))
            return false;
        if (branch) {
            bool choice = n->core->tag == MO_CORE_EXPR_CASE;
            if (!live_clear(c, f, live))
                return false;
            for (uint32_t i = 1; i < n->count; i++) {
                struct node *arm = n->children[i], *body = choice ? arm->children[0] : arm;
                if (!live_copy(c, f, n->work, n->after))
                    return false;
                n->last[i] = body->reg == MO_NO_INDEX || !n->work[body->reg];
                live_use(c, f, n->work, body->reg);
                if (!live_walk(c, f, body, n->work, changed, depth + 1))
                    return false;
                if (choice) {
                    if (!live_charge(c, f, arm->bind_count))
                        return false;
                    for (uint32_t j = 0; j < arm->bind_count; j++)
                        n->work[arm->binds[j]] = false;
                }
                bool ignored = false;
                if (!live_union(c, f, live, n->work, &ignored))
                    return false;
            }
            live_use(c, f, live, n->children[0]->reg);
            return live_walk(c, f, n->children[0], live, changed, depth + 1);
        }
        struct join *target = &((struct join *)f->joins.items)[n->index];
        if (!live_copy(c, f, n->work, n->after))
            return false;
        n->last[0] = n->children[0]->reg == MO_NO_INDEX || !n->work[n->children[0]->reg];
        live_use(c, f, n->work, n->children[0]->reg);
        if (!live_walk(c, f, n->children[0], n->work, changed, depth + 1))
            return false;
        for (uint32_t i = 0; i < n->bind_count; i++)
            n->work[n->binds[i]] = false;
        if (!live_union(c, f, target->live, n->work, changed) || !live_copy(c, f, live, n->after))
            return false;
        n->last[1] = n->children[1]->reg == MO_NO_INDEX || !live[n->children[1]->reg];
        live_use(c, f, live, n->children[1]->reg);
        return live_walk(c, f, n->children[1], live, changed, depth + 1);
    }
    if (n->core->tag == MO_CORE_EXPR_JUMP) {
        const struct join *target = &((struct join *)f->joins.items)[n->index];
        if (!live_copy(c, f, live, target->live))
            return false;
    } else if ((n->core->tag == MO_CORE_EXPR_STOP || n->core->tag == MO_CORE_EXPR_FAIL) &&
               !live_clear(c, f, live))
        return false;
    if (n->core->tag == MO_CORE_EXPR_LAM) {
        for (uint32_t i = n->bind_count; i-- > 0;) {
            uint32_t r = n->binds[i];
            n->last[i] = !live[r];
            live_use(c, f, live, r);
        }
        return true;
    }
    /* Borrowed operands sharing an owned operand need their original register throughout
     * the instruction. Repeated owned operands duplicate all but the final occurrence. */
    for (uint32_t i = 0; i < n->count; i++)
        if (!operand_owned(c, n, i))
            live_use(c, f, live, n->children[i]->reg);
    for (uint32_t i = n->count; i-- > 0;) {
        uint32_t r = n->children[i]->reg;
        n->last[i] = r == MO_NO_INDEX || !live[r];
        live_use(c, f, live, r);
    }
    for (uint32_t i = n->count; i-- > 0;)
        if (!live_walk(c, f, n->children[i], live, changed, depth + 1))
            return false;
    return true;
}
static MO_MUST_USE bool live_infer(struct compiler *c, struct function *f) {
    f->live_count = f->regs.count;
    f->live_controls = f->joins.count;
    if ((uint64_t)f->live_controls * f->live_count > MO_LIMIT_FUNCTION_REGISTER_BLOCKS)
        return problem(c, MO_CODE_BYTECODE_TOO_BIG,
                       "Function exceeds MO_LIMIT_FUNCTION_REGISTER_BLOCKS.");
    bool *live = allocate(c, f->live_count, sizeof *live);
    if (live == NULL)
        return false;
    for (uint32_t i = 0; i < f->joins.count; i++) {
        struct join *j = &((struct join *)f->joins.items)[i];
        j->live = allocate(c, f->live_count, sizeof *j->live);
        if (j->live == NULL || !live_clear(c, f, j->live))
            return false;
    }
    bool changed;
    do {
        changed = false;
        if (!live_clear(c, f, live))
            return false;
        live_use(c, f, live, f->body->reg);
        if (!live_walk(c, f, f->body, live, &changed, 0))
            return false;
    } while (changed);
    return true;
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
/* Journal only reference-state changes. Branch restoration and normalization visit this log,
 * never the full register table, and nested checkpoints reuse its storage. */
static MO_MUST_USE bool state(struct compiler *c, struct function *f, uint32_t r,
                              enum ref_state value) {
    struct reg *reg = &((struct reg *)f->regs.items)[r];
    if (!reference(c, reg->type) || reg->state == value)
        return true;
    struct change *change =
        append(c, &f->changes, 1, 2 * MO_LIMIT_FUNCTION_CODE_WORDS, MO_CODE_CODEGEN_MEMORY);
    if (change == NULL)
        return false;
    *change = (struct change){r, reg->state};
    reg->state = value;
    return true;
}
static const char *const host_params[] = {
#define HOSTOP(id, module, name, signature, params, bugs, spec_only) params,
#include "base/hostops.def"
};
static MO_MUST_USE bool consume(struct compiler *c, struct function *f, uint32_t r,
                                enum mo_operand_access access, const uint32_t *fixed,
                                enum mo_opcode op, uint32_t slot) {
    bool owned = access == MO_ACCESS_CONSUME;
    if (access == MO_ACCESS_MOVE)
        owned = ((struct reg *)f->regs.items)[r].state == REF_OWNED;
    else if (access == MO_ACCESS_CALL_PARAMETER) {
        uint32_t callee = fixed[op == MO_OP_CALL ? 1 : 0];
        owned = c->funs[callee].parameters[slot].ownership == MO_PARAMETER_OWNED;
    } else if (access == MO_ACCESS_PRIM_PARAMETER)
        owned = mo_codegen_primitive((enum mo_primitive_id)fixed[1]).params[slot] == 'o';
    else if (access == MO_ACCESS_HOST_PARAMETER)
        owned = host_params[fixed[1]][slot] == 'o';
    return !owned || state(c, f, r, REF_MOVED);
}
static MO_MUST_USE bool transfer(struct compiler *c, struct function *f, enum mo_opcode op,
                                 const uint32_t *fixed, const uint32_t *variable, uint32_t count) {
    const struct mo_opcode_info *info = mo_opcode_find((uint32_t)op);
    MO_ASSERT(info != NULL);
    enum ref_state result = op == MO_OP_CONST || op == MO_OP_GLOBAL ? REF_BORROWED : REF_OWNED;
    if (op == MO_OP_MOVE)
        result = ((struct reg *)f->regs.items)[fixed[1]].state;
    else if (op == MO_OP_CLEAR)
        result = REF_UNDEFINED;
    for (uint32_t i = 0; i < info->fixed_count; i++)
        if (info->fixed[i].kind == MO_OPERAND_REGISTER &&
            !consume(c, f, fixed[i], info->fixed[i].access, fixed, op, i))
            return false;
    if (info->variable.kind == MO_OPERAND_REGISTER)
        for (uint32_t i = 0; i < count; i++)
            if (!consume(c, f, variable[i], info->variable.access, fixed, op, i))
                return false;
    for (uint32_t i = 0; i < info->fixed_count; i++)
        if (info->fixed[i].kind == MO_OPERAND_REGISTER &&
            info->fixed[i].access == MO_ACCESS_WRITE && !state(c, f, fixed[i], result))
            return false;
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
    /* Every target this allocator emits begins just after a transfer instruction. */
    if (f->instructions == 0 || f->next_block) {
        f->blocks++;
        if ((uint64_t)f->regs.count * f->blocks > MO_LIMIT_FUNCTION_REGISTER_BLOCKS) {
            if (!problem(c, MO_CODE_BYTECODE_TOO_BIG,
                         "Function exceeds MO_LIMIT_FUNCTION_REGISTER_BLOCKS."))
                return MO_NO_INDEX;
        }
    }
    f->next_block = op == MO_OP_JUMP || op == MO_OP_BRANCH || op == MO_OP_SWITCH ||
                    (op >= MO_OP_TAIL_CALL && op <= MO_OP_FAIL);
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
    if (!transfer(c, f, op, fixed, variable, count))
        return MO_NO_INDEX;
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
/* Track only live owners, not every register at every return. */
static void owner_add(struct function *f, uint32_t r) {
    struct reg *regs = f->regs.items;
    MO_ASSERT(!regs[r].owned);
    regs[r].owned = true;
    regs[r].previous = MO_NO_INDEX;
    regs[r].next = f->owned;
    if (f->owned != MO_NO_INDEX)
        regs[f->owned].previous = r;
    f->owned = r;
}
static void owner_remove(struct function *f, uint32_t r) {
    struct reg *regs = f->regs.items;
    if (!regs[r].owned)
        return;
    if (regs[r].previous == MO_NO_INDEX)
        f->owned = regs[r].next;
    else
        regs[regs[r].previous].next = regs[r].next;
    if (regs[r].next != MO_NO_INDEX)
        regs[regs[r].next].previous = regs[r].previous;
    regs[r].owned = false;
}
static MO_MUST_USE bool drop(struct compiler *c, struct function *f, uint32_t r,
                             struct mo_core_span s) {
    if (r == MO_NO_INDEX || !((struct reg *)f->regs.items)[r].owned)
        return true;
    if (emit(c, f, s, MO_OP_DROP, &r, 1, NULL, 0) == MO_NO_INDEX)
        return false;
    owner_remove(f, r);
    return true;
}
static MO_MUST_USE bool cleanup(struct compiler *c, struct function *f, struct mo_core_span s) {
    while (f->owned != MO_NO_INDEX)
        if (!drop(c, f, f->owned, s))
            return false;
    return true;
}
static void restore(struct function *f, uint32_t checkpoint) {
    struct reg *regs = f->regs.items;
    MO_ASSERT(checkpoint <= f->changes.count);
    while (f->changes.count != checkpoint) {
        struct change change = ((struct change *)f->changes.items)[--f->changes.count];
        struct reg *r = &regs[change.reg];
        if (r->owned && change.previous != REF_OWNED)
            owner_remove(f, change.reg);
        else if (!r->owned && change.previous == REF_OWNED)
            owner_add(f, change.reg);
        r->state = change.previous;
    }
}
/* Every live edge uses the same undefined state for dead references, including lexical
 * registers consumed within just one arm. Restoration publishes that state without code. */
static MO_MUST_USE bool forget_dead(struct compiler *c, struct function *f, const bool *live,
                                    uint32_t keep, struct mo_core_span s, bool instructions) {
    if (!live_charge(c, f, f->live_count))
        return false;
    struct reg *regs = f->regs.items;
    for (uint32_t i = 0; i < f->live_count; i++) {
        if (i == keep || live[i] || regs[i].protected || regs[i].state == REF_UNDEFINED)
            continue;
        if (instructions) {
            if (!drop(c, f, i, s) || emit(c, f, s, MO_OP_CLEAR, &i, 1, NULL, 0) == MO_NO_INDEX)
                return false;
        } else {
            owner_remove(f, i);
            if (!state(c, f, i, REF_UNDEFINED))
                return false;
        }
    }
    return true;
}
static MO_MUST_USE bool normalize(struct compiler *c, struct function *f, uint32_t checkpoint,
                                  uint32_t keep, struct mo_core_span s, const bool *live) {
    MO_ASSERT(checkpoint <= f->changes.count && f->mark != UINT32_MAX);
    if (live != NULL && !forget_dead(c, f, live, keep, s, true))
        return false;
    uint32_t mark = ++f->mark, end = f->changes.count;
    struct reg *regs = f->regs.items;
    for (uint32_t i = checkpoint; i < end; i++) {
        struct change change = ((struct change *)f->changes.items)[i];
        struct reg *r = &regs[change.reg];
        if (r->mark != mark) {
            r->mark = mark;
            r->goal = change.previous;
        }
    }
    for (uint32_t i = end; i != checkpoint;) {
        uint32_t id = ((struct change *)f->changes.items)[--i].reg;
        struct reg *r = &regs[id];
        if (r->mark != mark)
            continue;
        r->mark = 0;
        if (id == keep || r->protected || r->state == r->goal ||
            (live != NULL && id < f->live_count && !live[id]))
            continue;
        /* Only path-local slots change on a live edge; lexical owners outside it survive.
         * Jump argument copies are protected until simultaneous parameter transfer. */
        MO_ASSERT(r->goal == REF_UNDEFINED);
        if (r->state == REF_OWNED) {
            MO_ASSERT(r->owned);
            if (!drop(c, f, id, s))
                return false;
        }
        if (emit(c, f, s, MO_OP_CLEAR, &id, 1, NULL, 0) == MO_NO_INDEX)
            return false;
    }
    return true;
}
static MO_MUST_USE bool merged(struct compiler *c, struct function *f, uint32_t r) {
    if (!reference(c, ((struct reg *)f->regs.items)[r].type))
        return true;
    bool owned = ((struct ownership *)c->ownership.items)[flow_id(f, r)].owned;
    if (!state(c, f, r, owned ? REF_OWNED : REF_BORROWED))
        return false;
    if (owned)
        owner_add(f, r);
    return true;
}
static MO_MUST_USE bool copy(struct compiler *c, struct function *f, uint32_t to, uint32_t from,
                             bool owned, struct mo_core_span s, bool last) {
    bool ref = reference(c, ((struct reg *)f->regs.items)[from].type);
    bool move = ref && owned && last && ((struct reg *)f->regs.items)[from].owned;
    enum mo_opcode op = ref && owned && !move ? MO_OP_DUP : MO_OP_MOVE;
    MO_ASSERT(!ref || owned || !((struct reg *)f->regs.items)[from].owned);
    const uint32_t args[] = {to, from};
    if (emit(c, f, s, op, args, 2, NULL, 0) == MO_NO_INDEX)
        return false;
    if (move)
        owner_remove(f, from);
    if (ref && owned)
        owner_add(f, to);
    return true;
}
static MO_MUST_USE uint32_t argument(struct compiler *c, struct function *f, uint32_t r, bool owned,
                                     struct mo_core_span s, bool last) {
    uint32_t ty = ((struct reg *)f->regs.items)[r].type;
    if (!reference(c, ty) || !owned || (last && ((struct reg *)f->regs.items)[r].owned))
        return r;
    uint32_t tmp = register_add(c, f, ty, MO_NO_INDEX);
    if (tmp == MO_NO_INDEX || !copy(c, f, tmp, r, true, s, false))
        return MO_NO_INDEX;
    return tmp;
}
/* Immediate slots have a stable value state at every edge. Reference slots start undefined
 * and are cleared back to that state when their path-local value dies. */
static MO_MUST_USE bool initialize(struct compiler *c, struct function *f, uint32_t reg,
                                   unsigned depth) {
    MO_ASSERT(depth < MO_LIMIT_CORE_DEPTH);
    uint32_t ty = ((struct reg *)f->regs.items)[reg].type;
    const struct mo_type *t = &c->types.items[ty];
    if (reference(c, ty))
        return true;
    if (t->kind == MO_TYPE_RANGE) {
        uint32_t field = register_add(c, f, c->range_int, MO_NO_INDEX);
        if (field == MO_NO_INDEX || !initialize(c, f, field, depth + 1))
            return false;
        const uint32_t args[] = {reg, field, field};
        return emit(c, f, c->declaration, MO_OP_MAKE_RANGE, args, 3, NULL, 0) != MO_NO_INDEX;
    }
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
    uint32_t id = constant(c, k);
    const uint32_t fixed[] = {reg, id};
    return id != MO_NO_INDEX &&
           emit(c, f, c->declaration, MO_OP_CONST, fixed, 2, NULL, 0) != MO_NO_INDEX;
}
/* Expansion temporaries are private owners; the trusted row specifies which ones are consumed. */
static MO_MUST_USE bool library(struct compiler *c, struct function *f, enum mo_primitive_id row,
                                uint32_t signature, uint32_t result, const uint32_t *args,
                                uint32_t count, struct mo_core_span source) {
    struct mo_codegen_operation op = mo_codegen_primitive(row);
    MO_ASSERT(count == c->types.items[signature].parameter_count);
    for (uint32_t i = 0; i < count; i++)
        if (op.params[i] == 'o' && reference(c, ((struct reg *)f->regs.items)[args[i]].type)) {
            MO_ASSERT(((struct reg *)f->regs.items)[args[i]].owned);
            owner_remove(f, args[i]);
        }
    const uint32_t fixed[] = {result, (uint32_t)row, signature};
    if (emit(c, f, source, MO_OP_PRIM, fixed, 3, args, count) == MO_NO_INDEX)
        return false;
    if (reference(c, ((struct reg *)f->regs.items)[result].type))
        owner_add(f, result);
    return true;
}
static MO_MUST_USE bool range_text(struct compiler *c, struct function *f, struct node *n,
                                   uint32_t value) {
    const uint32_t start[] = {n->binds[0], value}, end[] = {n->binds[1], value},
                   separator[] = {n->binds[4], n->index}, prefix[] = {n->binds[2], n->binds[4]},
                   suffix[] = {n->binds[5], n->binds[3]};
    return emit(c, f, n->span, MO_OP_RANGE_START, start, 2, NULL, 0) != MO_NO_INDEX &&
           emit(c, f, n->span, MO_OP_RANGE_END, end, 2, NULL, 0) != MO_NO_INDEX &&
           emit(c, f, n->span, MO_OP_CONST, separator, 2, NULL, 0) != MO_NO_INDEX &&
           library(c, f, MO_PRIM_INT_TO_TEXT, n->cases[0], n->binds[2], n->binds, 1, n->span) &&
           library(c, f, MO_PRIM_INT_TO_TEXT, n->cases[0], n->binds[3], n->binds + 1, 1, n->span) &&
           library(c, f, MO_PRIM_TEXT_CONCAT, n->cases[1], n->binds[5], prefix, 2, n->span) &&
           library(c, f, MO_PRIM_TEXT_CONCAT, n->cases[1], n->reg, suffix, 2, n->span) &&
           drop(c, f, n->binds[3], n->span);
}
static MO_MUST_USE bool uncons(struct compiler *c, struct function *f, struct node *n,
                               uint32_t list) {
    uint32_t first = n->binds[0], head = n->binds[1], rest = n->binds[2], one = n->binds[3],
             pair = n->binds[4];
    if (!library(c, f, MO_PRIM_LIST_FIRST, n->cases[0], first, &list, 1, n->span))
        return false;
    /* Both arms consume this argument. Keep its moved state across normalization,
     * then publish that common state after restoring the branch checkpoint. */
    MO_ASSERT(((struct reg *)f->regs.items)[list].owned &&
              !((struct reg *)f->regs.items)[list].protected);
    ((struct reg *)f->regs.items)[list].protected = true;
    uint32_t checkpoint = f->changes.count;
    uint32_t branch = emit(c, f, n->span, MO_OP_SWITCH, &first, 1, (uint32_t[]){0, 0}, 2);
    if (branch == MO_NO_INDEX)
        return false;
    ((uint32_t *)f->code.items)[branch + 2] = f->code.count;
    const uint32_t none[] = {n->reg, n->type, 0};
    if (!drop(c, f, list, n->span) ||
        emit(c, f, n->span, MO_OP_MAKE_CASE, none, 3, NULL, 0) == MO_NO_INDEX)
        return false;
    owner_add(f, n->reg);
    uint32_t skip = emit(c, f, n->span, MO_OP_JUMP, (uint32_t[]){0}, 1, NULL, 0);
    if (skip == MO_NO_INDEX)
        return false;
    restore(f, checkpoint);
    ((uint32_t *)f->code.items)[branch + 3] = f->code.count;
    const uint32_t field[] = {head, first, 1, 0}, count[] = {one, n->index};
    if (emit(c, f, n->span, MO_OP_CASE_FIELD, field, 4, NULL, 0) == MO_NO_INDEX ||
        emit(c, f, n->span, MO_OP_CONST, count, 2, NULL, 0) == MO_NO_INDEX)
        return false;
    if (reference(c, ((struct reg *)f->regs.items)[head].type))
        owner_add(f, head);
    if (!library(c, f, MO_PRIM_LIST_DROP, n->cases[1], rest, (uint32_t[]){list, one}, 2, n->span))
        return false;
    const uint32_t construct[] = {pair, c->layouts[((struct reg *)f->regs.items)[pair].type]},
                   some[] = {n->reg, n->type, 1}, fields[] = {head, rest};
    owner_remove(f, head);
    owner_remove(f, rest);
    if (emit(c, f, n->span, MO_OP_CONSTRUCT, construct, 2, fields, 2) == MO_NO_INDEX)
        return false;
    /* The private tuple moves straight into Some; it never joins the live-owner set. */
    if (emit(c, f, n->span, MO_OP_MAKE_CASE, some, 3, &pair, 1) == MO_NO_INDEX)
        return false;
    owner_add(f, n->reg);
    if (!normalize(c, f, checkpoint, n->reg, n->span, NULL))
        return false;
    ((uint32_t *)f->code.items)[skip + 1] = f->code.count;
    restore(f, checkpoint);
    ((struct reg *)f->regs.items)[list].protected = false;
    owner_remove(f, list);
    return state(c, f, list, REF_MOVED) && merged(c, f, n->reg) && drop(c, f, first, n->span);
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
    case MO_CORE_EXPR_GLOBAL: {
        const uint32_t args[] = {n->reg, n->index};
        if (emit(c, f, n->span, MO_OP_GLOBAL, args, 2, NULL, 0) == MO_NO_INDEX)
            return false;
        break;
    }
    case MO_CORE_EXPR_LET: {
        if (!generate(c, f, n->children[0], false, depth + 1, live) || !*live)
            return c->error == 0;
        uint32_t start = f->code.count;
        bool owned = ((struct ownership *)c->ownership.items)[flow_id(f, n->binds[0])].owned;
        if (!copy(c, f, n->binds[0], n->children[0]->reg, owned, n->span, n->last[0]) ||
            (n->children[0]->temporary && !drop(c, f, n->children[0]->reg, n->span)) ||
            !generate(c, f, n->children[1], tail, depth + 1, live))
            return false;
        if (*live && n->binds[0] != n->reg && !drop(c, f, n->binds[0], n->span))
            return false;
        return named(c, f, n->binds[0], start, f->code.count);
    }
    case MO_CORE_EXPR_LAM: {
        if (n->bind_count == 0) {
            struct mo_bytecode_constant k = {.type = n->type};
            k.value.function = n->index;
            uint32_t id = constant(c, k);
            const uint32_t args[] = {n->reg, id};
            if (id == MO_NO_INDEX ||
                emit(c, f, n->span, MO_OP_CONST, args, 2, NULL, 0) == MO_NO_INDEX)
                return false;
        } else {
            uint32_t *regs = allocate(c, n->bind_count, sizeof *regs);
            if (regs == NULL)
                return false;
            for (uint32_t i = 0; i < n->bind_count; i++) {
                regs[i] = argument(c, f, n->binds[i], true, n->span, n->last[i]);
                if (regs[i] == MO_NO_INDEX)
                    return false;
                owner_remove(f, regs[i]);
            }
            const uint32_t args[] = {n->reg, n->index};
            if (emit(c, f, n->span, MO_OP_MAKE_CLOSURE, args, 2, regs, n->bind_count) ==
                MO_NO_INDEX)
                return false;
            owner_add(f, n->reg);
        }
        break;
    }
    case MO_CORE_EXPR_APP: {
        uint32_t *regs = allocate(c, n->count, sizeof *regs);
        if (regs == NULL)
            return false;
        for (uint32_t i = 0; i < n->count; i++) {
            if (!generate(c, f, n->children[i], false, depth + 1, live) || !*live)
                return c->error == 0;
        }
        for (uint32_t i = 0; i < n->count; i++) {
            regs[i] = argument(c, f, n->children[i]->reg, true, n->span, n->last[i]);
            if (regs[i] == MO_NO_INDEX)
                return false;
        }
        for (uint32_t i = 0; i < n->count; i++)
            owner_remove(f, regs[i]);
        if (tail && !cleanup(c, f, n->span))
            return false;
        const uint32_t args[] = {n->reg, regs[0]};
        if (emit(c, f, n->span, tail ? MO_OP_TAIL_CALL_CLOSURE : MO_OP_CALL_CLOSURE,
                 tail ? args + 1 : args, tail ? 1 : 2, regs + 1, n->count - 1) == MO_NO_INDEX)
            return false;
        *live = !tail;
        if (*live) {
            if (reference(c, n->type))
                owner_add(f, n->reg);
            for (uint32_t i = 0; i < n->count; i++)
                if (n->children[i]->temporary && !drop(c, f, n->children[i]->reg, n->span))
                    return false;
        }
        break;
    }
    case MO_CORE_EXPR_CALL:
    case MO_CORE_EXPR_CON:
    case MO_CORE_EXPR_TUPLE: {
        uint32_t *regs = allocate(c, n->count, sizeof *regs);
        if (regs == NULL)
            return false;
        for (uint32_t i = 0; i < n->count; i++) {
            if (!generate(c, f, n->children[i], false, depth + 1, live) || !*live)
                return c->error == 0;
        }
        for (uint32_t i = 0; i < n->count; i++) {
            regs[i] =
                argument(c, f, n->children[i]->reg, operand_owned(c, n, i), n->span, n->last[i]);
            if (regs[i] == MO_NO_INDEX)
                return false;
        }
        uint32_t args[] = {n->reg, n->index, 0};
        enum mo_opcode op = MO_OP_CALL;
        uint32_t count = 2;
        if (n->core->tag == MO_CORE_EXPR_CON) {
            if (c->types.items[n->type].kind == MO_TYPE_RECORD) {
                op = MO_OP_CONSTRUCT;
                args[1] = c->layouts[n->type];
            } else {
                op = MO_OP_MAKE_CASE;
                args[1] = n->type;
                args[2] = n->index;
                count = 3;
            }
        } else if (n->core->tag == MO_CORE_EXPR_TUPLE) {
            op = MO_OP_CONSTRUCT;
            args[1] = c->layouts[n->type];
        } else if (tail) {
            op = MO_OP_TAIL_CALL;
            args[0] = n->index;
            count = 1;
            *live = false;
        }
        /* Reserve consumed operands while dropping the rest of a tail frame. */
        for (uint32_t i = 0; i < n->count; i++)
            if (operand_owned(c, n, i))
                owner_remove(f, regs[i]);
        if (op == MO_OP_TAIL_CALL && !cleanup(c, f, n->span))
            return false;
        if (emit(c, f, n->span, op, args, count, regs, n->count) == MO_NO_INDEX)
            return false;
        if (*live) {
            if (reference(c, n->type))
                owner_add(f, n->reg);
            for (uint32_t i = 0; i < n->count; i++)
                if (n->children[i]->temporary && !drop(c, f, n->children[i]->reg, n->span))
                    return false;
        }
        break;
    }
    case MO_CORE_EXPR_IF:
    case MO_CORE_EXPR_CASE: {
        if (!generate(c, f, n->children[0], false, depth + 1, live) || !*live)
            return c->error == 0;
        bool choice = n->core->tag == MO_CORE_EXPR_CASE;
        if (choice && c->types.items[n->children[0]->type].kind == MO_TYPE_RECORD) {
            MO_ASSERT(n->count == 2);
            struct node *arm = n->children[1], *body = arm->children[0];
            for (uint32_t j = 0; j < arm->bind_count; j++) {
                const uint32_t field[] = {arm->binds[j], n->children[0]->reg, j};
                if (emit(c, f, n->span, MO_OP_FIELD, field, 3, NULL, 0) == MO_NO_INDEX)
                    return false;
                if (reference(c, ((struct reg *)f->regs.items)[arm->binds[j]].type))
                    owner_add(f, arm->binds[j]);
            }
            uint32_t start = f->code.count;
            if (!generate(c, f, body, tail, depth + 1, live))
                return false;
            if (*live) {
                bool owned = ((struct ownership *)c->ownership.items)[flow_id(f, n->reg)].owned;
                if (!copy(c, f, n->reg, body->reg, owned, n->span, n->last[1]) ||
                    (body->temporary && !drop(c, f, body->reg, n->span)) ||
                    (n->children[0]->temporary && !drop(c, f, n->children[0]->reg, n->span)))
                    return false;
                for (uint32_t j = 0; j < arm->bind_count; j++)
                    if (!drop(c, f, arm->binds[j], n->span))
                        return false;
            }
            for (uint32_t j = 0; j < arm->bind_count; j++)
                if (!named(c, f, arm->binds[j], start, f->code.count))
                    return false;
            break;
        }
        uint32_t arms = n->count - 1;
        uint32_t *targets = allocate(c, arms, sizeof *targets),
                 *jumps = allocate(c, arms, sizeof *jumps);
        if (targets == NULL || jumps == NULL)
            return false;
        memset(targets, 0, (size_t)arms * sizeof *targets);
        for (uint32_t i = 0; i < arms; i++)
            jumps[i] = MO_NO_INDEX;
        uint32_t fixed[] = {n->children[0]->reg, 0, 0};
        uint32_t at = emit(c, f, n->span, choice ? MO_OP_SWITCH : MO_OP_BRANCH, fixed,
                           choice ? 1 : 3, choice ? targets : NULL, choice ? arms : 0);
        if (at == MO_NO_INDEX)
            return false;
        uint32_t checkpoint = f->changes.count;
        bool any = false;
        for (uint32_t i = 0; i < arms; i++) {
            restore(f, checkpoint);
            uint32_t case_id = choice ? n->cases[i] : i;
            targets[case_id] = f->code.count;
            uint32_t start = f->code.count;
            struct node *arm = n->children[i + 1], *body = choice ? arm->children[0] : arm;
            if (choice) {
                for (uint32_t j = 0; j < arm->bind_count; j++) {
                    const uint32_t field[] = {arm->binds[j], n->children[0]->reg, case_id, j};
                    if (emit(c, f, n->span, MO_OP_CASE_FIELD, field, 4, NULL, 0) == MO_NO_INDEX)
                        return false;
                    if (reference(c, ((struct reg *)f->regs.items)[arm->binds[j]].type))
                        owner_add(f, arm->binds[j]);
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
                const uint32_t jump[] = {0};
                bool owned = n->reg != MO_NO_INDEX &&
                             ((struct ownership *)c->ownership.items)[flow_id(f, n->reg)].owned;
                if (!copy(c, f, n->reg, body->reg, owned, n->span, n->last[i + 1]) ||
                    !normalize(c, f, checkpoint, n->reg, n->span, n->after))
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
        restore(f, checkpoint);
        if (any && (!forget_dead(c, f, n->after, n->reg, n->span, false) || !merged(c, f, n->reg)))
            return false;
        *live = any;
        break;
    }
    case MO_CORE_EXPR_PROJ: {
        if (!generate(c, f, n->children[0], false, depth + 1, live) || !*live)
            return c->error == 0;
        const uint32_t args[] = {n->reg, n->children[0]->reg, n->index};
        if (emit(c, f, n->span, MO_OP_FIELD, args, 3, NULL, 0) == MO_NO_INDEX)
            return false;
        if (reference(c, n->type))
            owner_add(f, n->reg);
        if (n->children[0]->temporary && !drop(c, f, n->children[0]->reg, n->span))
            return false;
        break;
    }
    case MO_CORE_EXPR_PRIM: {
        uint32_t *regs = allocate(c, n->count, sizeof *regs);
        if (regs == NULL)
            return false;
        /* Evaluate every operand before calling or inserting, including later literal pairs.
         * A duplicate map key must not suppress the effects of a later Core argument. */
        for (uint32_t i = 0; i < n->count; i++) {
            if (!generate(c, f, n->children[i], false, depth + 1, live) || !*live)
                return c->error == 0;
        }
        for (uint32_t i = 0; i < n->count; i++) {
            regs[i] =
                argument(c, f, n->children[i]->reg, operation_owned(n, i), n->span, n->last[i]);
            if (regs[i] == MO_NO_INDEX)
                return false;
        }
        if (n->operation.expansion == MO_CODEGEN_EXPAND_RANGE) {
            if (!range_text(c, f, n, regs[0]))
                return false;
        } else if (n->operation.expansion == MO_CODEGEN_EXPAND_UNCONS) {
            if (!uncons(c, f, n, regs[0]))
                return false;
        } else if (n->operation.literal != MO_CODEGEN_LITERAL_NONE) {
            bool map = n->operation.literal == MO_CODEGEN_LITERAL_MAP;
            uint32_t width = map ? 2 : 1;
            uint32_t builder = n->bind_count == 0 ? n->reg : n->binds[0];
            const uint32_t empty[] = {builder, map ? MO_PRIM_MAP_EMPTY : MO_PRIM_LIST_EMPTY,
                                      n->index};
            if (emit(c, f, n->span, MO_OP_PRIM, empty, 3, NULL, 0) == MO_NO_INDEX)
                return false;
            owner_add(f, builder);
            for (uint32_t i = 0; i < n->bind_count; i++) {
                uint32_t dest = i + 1 == n->bind_count ? n->reg : n->binds[i + 1];
                uint32_t args[3] = {builder, regs[i * width], map ? regs[i * width + 1] : 0};
                owner_remove(f, builder);
                for (uint32_t j = 0; j < width; j++)
                    owner_remove(f, regs[i * width + j]);
                const uint32_t insert[] = {dest, n->operation.row, n->signature};
                if (emit(c, f, n->span, MO_OP_PRIM, insert, 3, args, width + 1) == MO_NO_INDEX)
                    return false;
                owner_add(f, dest);
                builder = dest;
            }
        } else {
            for (uint32_t i = 0; i < n->count; i++)
                if (operation_owned(n, i))
                    owner_remove(f, regs[i]);
            if (n->operation.reverse) {
                MO_ASSERT(n->count == 2);
                uint32_t swap = regs[0];
                regs[0] = regs[1];
                regs[1] = swap;
            }
            if (n->operation.opcode == MO_OP_PRIM || n->operation.opcode == MO_OP_HOST) {
                const uint32_t fixed[] = {n->operation.compare ? n->binds[0] : n->reg,
                                          n->operation.row, n->signature};
                if (emit(c, f, n->span, n->operation.opcode, fixed,
                         n->operation.opcode == MO_OP_HOST ? 2 : 3, regs, n->count) == MO_NO_INDEX)
                    return false;
            } else if (n->operation.opcode == MO_OP_EQUAL) {
                const uint32_t fixed[] = {n->reg, c->types.items[n->signature].parameters[0],
                                          regs[0], regs[1]};
                if (emit(c, f, n->span, MO_OP_EQUAL, fixed, 4, NULL, 0) == MO_NO_INDEX)
                    return false;
            } else {
                uint32_t fixed[4] = {n->reg, 0, 0, 0};
                MO_ASSERT(n->count <= 2);
                memcpy(fixed + 1, regs, (size_t)n->count * sizeof *regs);
                if (emit(c, f, n->span, n->operation.opcode, fixed, 1 + n->count, NULL, 0) ==
                    MO_NO_INDEX)
                    return false;
            }
            if (n->operation.compare) {
                const uint32_t equal[] = {n->binds[1], c->types.items[n->signature].result, 1};
                const uint32_t test[] = {n->reg, n->binds[0], n->binds[1]};
                enum mo_core_cmp_op cmp = n->core->as.prim.prim.as.cmp.op;
                enum mo_opcode op = cmp == MO_CORE_CMP_OP_LE || cmp == MO_CORE_CMP_OP_GE
                                        ? MO_OP_LE_CHOICE
                                        : MO_OP_LT_CHOICE;
                if (emit(c, f, n->span, MO_OP_MAKE_CASE, equal, 3, NULL, 0) == MO_NO_INDEX ||
                    emit(c, f, n->span, op, test, 3, NULL, 0) == MO_NO_INDEX)
                    return false;
            }
            if (reference(c, n->type))
                owner_add(f, n->reg);
        }
        for (uint32_t i = 0; i < n->count; i++)
            if (n->children[i]->temporary && !drop(c, f, n->children[i]->reg, n->span))
                return false;
        break;
    }
    case MO_CORE_EXPR_FAIL:
        if (!generate(c, f, n->children[0], false, depth + 1, live) || !*live)
            return c->error == 0;
        if (emit(c, f, n->span, MO_OP_FAIL, &n->children[0]->reg, 1, NULL, 0) == MO_NO_INDEX)
            return false;
        *live = false;
        break;
    case MO_CORE_EXPR_STOP: {
        const uint32_t args[] = {n->reg, n->index}, stop[] = {n->reg};
        if (emit(c, f, n->span, MO_OP_CONST, args, 2, NULL, 0) == MO_NO_INDEX ||
            emit(c, f, n->span, MO_OP_STOP, stop, 1, NULL, 0) == MO_NO_INDEX)
            return false;
        *live = false;
        break;
    }
    case MO_CORE_EXPR_JOIN: {
        uint32_t skip = emit(c, f, n->span, MO_OP_JUMP, (uint32_t[]){0}, 1, NULL, 0);
        if (skip == MO_NO_INDEX)
            return false;
        uint32_t checkpoint = f->changes.count;
        struct join *joins = f->joins.items;
        joins[n->index].offset = f->code.count;
        joins[n->index].checkpoint = checkpoint;
        if (!forget_dead(c, f, joins[n->index].live, MO_NO_INDEX, n->span, false))
            return false;
        /* Every incoming jump writes owned reference parameters. The first entry
         * and loop backedges share the declaration's lexical state outside those parameters. */
        for (uint32_t i = 0; i < n->bind_count; i++)
            if (reference(c, ((struct reg *)f->regs.items)[n->binds[i]].type)) {
                if (!state(c, f, n->binds[i], REF_OWNED))
                    return false;
                owner_add(f, n->binds[i]);
            }
        uint32_t start = f->code.count;
        if (!generate(c, f, n->children[0], tail, depth + 1, live))
            return false;
        uint32_t exit = MO_NO_INDEX;
        bool any = *live;
        if (*live) {
            bool owned = ((struct ownership *)c->ownership.items)[flow_id(f, n->reg)].owned;
            if (!copy(c, f, n->reg, n->children[0]->reg, owned, n->span, n->last[0]) ||
                !normalize(c, f, checkpoint, n->reg, n->span, n->after))
                return false;
            exit = emit(c, f, n->span, MO_OP_JUMP, (uint32_t[]){0}, 1, NULL, 0);
            if (exit == MO_NO_INDEX)
                return false;
        }
        for (uint32_t i = 0; i < n->bind_count; i++)
            if (!named(c, f, n->binds[i], start, f->code.count))
                return false;
        restore(f, checkpoint);
        ((uint32_t *)f->code.items)[skip + 1] = f->code.count;
        if (!generate(c, f, n->children[1], tail, depth + 1, live))
            return false;
        if (*live) {
            bool owned = ((struct ownership *)c->ownership.items)[flow_id(f, n->reg)].owned;
            if (!copy(c, f, n->reg, n->children[1]->reg, owned, n->span, n->last[1]) ||
                !normalize(c, f, checkpoint, n->reg, n->span, n->after))
                return false;
        }
        if (exit != MO_NO_INDEX)
            ((uint32_t *)f->code.items)[exit + 1] = f->code.count;
        *live |= any;
        restore(f, checkpoint);
        if (*live &&
            (!forget_dead(c, f, n->after, n->reg, n->span, false) || !merged(c, f, n->reg)))
            return false;
        break;
    }
    case MO_CORE_EXPR_JUMP: {
        const struct join *target = &((struct join *)f->joins.items)[n->index];
        MO_ASSERT(target->offset != MO_NO_INDEX);
        for (uint32_t i = 0; i < n->count; i++) {
            if (!generate(c, f, n->children[i], false, depth + 1, live) || !*live)
                return c->error == 0;
        }
        for (uint32_t i = 0; i < n->count; i++) {
            if (!copy(c, f, n->binds[i], n->children[i]->reg, true, n->span, n->last[i]))
                return false;
        }
        for (uint32_t i = 0; i < n->count; i++)
            ((struct reg *)f->regs.items)[n->binds[i]].protected = true;
        if (!normalize(c, f, target->checkpoint, MO_NO_INDEX, n->span, target->live))
            return false;
        for (uint32_t i = 0; i < n->count; i++) {
            uint32_t to = target->node->binds[i], from = n->binds[i];
            const uint32_t args[] = {to, from};
            if (emit(c, f, n->span, MO_OP_MOVE, args, 2, NULL, 0) == MO_NO_INDEX)
                return false;
            ((struct reg *)f->regs.items)[from].protected = false;
            if (reference(c, ((struct reg *)f->regs.items)[from].type)) {
                owner_remove(f, from);
                owner_add(f, to);
                if (emit(c, f, n->span, MO_OP_CLEAR, &from, 1, NULL, 0) == MO_NO_INDEX)
                    return false;
            }
        }
        if (emit(c, f, n->span, MO_OP_JUMP, &target->offset, 1, NULL, 0) == MO_NO_INDEX)
            return false;
        *live = false;
        break;
    }
    }
    if (*live && tail) {
        uint32_t result = argument(c, f, n->reg, true, n->span, true);
        if (result == MO_NO_INDEX)
            return false;
        owner_remove(f, result);
        if (!cleanup(c, f, n->span) ||
            emit(c, f, n->span, MO_OP_RET, &result, 1, NULL, 0) == MO_NO_INDEX)
            return false;
        *live = false;
    }
    return true;
}

/* Core established the first-match ordinary entry. Tools must find it by full name. */
static MO_MUST_USE bool entry_valid(struct compiler *c) {
    const struct mo_core *p = c->core;
    struct mo_codegen_index *entry = mo_codegen_find(c->fun_index, p->entry);
    MO_ASSERT(entry != NULL);
    c->current = &p->funs.items[entry->value];
    c->declaration =
        p->metadata.source ? p->metadata.funs.items[entry->value].span : (struct mo_core_span){0};
    c->span = c->declaration;
    uint32_t full_name = c->funs[entry->value].name;
    for (uint32_t i = 0; i < c->function_count; i++) {
        if (i != entry->value && c->funs[i].name == full_name)
            return problem(c, MO_CODE_CODEGEN_ENTRY,
                           "The entry's full runtime name is not unique in the image.");
    }
    return true;
}
/* One sizing walk keeps function pointers stable during recursive lifting. */
static MO_MUST_USE bool count_lambdas(struct compiler *c, mo_core_expr e, unsigned depth) {
    MO_ASSERT(depth < MO_LIMIT_CORE_DEPTH);
    mo_core_expr small[3];
    mo_core_expr_list xs = {0};
    switch (e->tag) {
    case MO_CORE_EXPR_LAM:
        if (c->function_capacity == MO_LIMIT_FUNCTIONS)
            return problem(c, MO_CODE_BYTECODE_TOO_BIG,
                           "Lifted closures exceed MO_LIMIT_FUNCTIONS.");
        c->function_capacity++;
        small[0] = e->as.lam.body;
        xs = (mo_core_expr_list){small, 1};
        break;
    case MO_CORE_EXPR_LET:
        small[0] = e->as.let_.bound;
        small[1] = e->as.let_.body;
        xs = (mo_core_expr_list){small, 2};
        break;
    case MO_CORE_EXPR_APP:
        if (!count_lambdas(c, e->as.app.f, depth + 1))
            return false;
        xs = e->as.app.args;
        break;
    case MO_CORE_EXPR_CALL:
        xs = e->as.call.args;
        break;
    case MO_CORE_EXPR_TUPLE:
        xs = e->as.tuple;
        break;
    case MO_CORE_EXPR_CON:
        xs = e->as.con.args;
        break;
    case MO_CORE_EXPR_PRIM:
        xs = e->as.prim.args;
        break;
    case MO_CORE_EXPR_JUMP:
        xs = e->as.jump.args;
        break;
    case MO_CORE_EXPR_CASE:
        if (!count_lambdas(c, e->as.case_.expr, depth + 1))
            return false;
        for (size_t i = 0; i < e->as.case_.arms.count; i++)
            if (!count_lambdas(c, e->as.case_.arms.items[i].body, depth + 1))
                return false;
        break;
    case MO_CORE_EXPR_IF:
        small[0] = e->as.if_.cond;
        small[1] = e->as.if_.then_;
        small[2] = e->as.if_.else_;
        xs = (mo_core_expr_list){small, 3};
        break;
    case MO_CORE_EXPR_PROJ:
        small[0] = e->as.proj.expr;
        xs = (mo_core_expr_list){small, 1};
        break;
    case MO_CORE_EXPR_FAIL:
        small[0] = e->as.fail;
        xs = (mo_core_expr_list){small, 1};
        break;
    case MO_CORE_EXPR_JOIN:
        small[0] = e->as.join.body;
        small[1] = e->as.join.rest;
        xs = (mo_core_expr_list){small, 2};
        break;
    case MO_CORE_EXPR_LIT:
    case MO_CORE_EXPR_VAR:
    case MO_CORE_EXPR_GLOBAL:
    case MO_CORE_EXPR_STOP:
        break;
    }
    for (size_t i = 0; i < xs.count; i++)
        if (!count_lambdas(c, xs.items[i], depth + 1))
            return false;
    return true;
}
static MO_MUST_USE bool declarations(struct compiler *c) {
    const struct mo_core *p = c->core;
    if (p->funs.count > MO_LIMIT_FUNCTIONS ||
        p->consts.count > (MO_LIMIT_FUNCTIONS - p->funs.count) / 2 ||
        p->consts.count > MO_LIMIT_GLOBALS || p->data.count > MO_LIMIT_TYPES)
        return problem(c, MO_CODE_BYTECODE_TOO_BIG,
                       "Core and its initializer helpers exceed a bytecode declaration limit.");
    c->function_count = (uint32_t)(p->funs.count + 2 * p->consts.count);
    c->function_capacity = c->function_count;
    for (size_t i = 0; i < p->funs.count; i++)
        if (!count_lambdas(c, p->funs.items[i].body, 0))
            return false;
    for (size_t i = 0; i < p->consts.count; i++)
        if (!count_lambdas(c, p->consts.items[i].body, 0))
            return false;
    c->data = allocate(c, p->data.count, sizeof *c->data);
    c->funs = allocate(c, c->function_capacity, sizeof *c->funs);
    c->parameters = allocate(c, c->function_capacity, sizeof *c->parameters);
    c->bodies = allocate(c, c->function_capacity, sizeof *c->bodies);
    c->debug = allocate(c, c->function_capacity, sizeof *c->debug);
    size_t file_count = p->metadata.source ? p->metadata.files.count : 0;
    c->files = allocate(c, file_count, sizeof *c->files);
    if (c->data == NULL || c->funs == NULL || c->parameters == NULL || c->bodies == NULL ||
        c->debug == NULL || c->files == NULL)
        return false;
    memset(c->funs, 0, c->function_capacity * sizeof *c->funs);
    memset(c->parameters, 0, c->function_capacity * sizeof *c->parameters);
    memset(c->bodies, 0, c->function_capacity * sizeof *c->bodies);
    memset(c->debug, 0, c->function_capacity * sizeof *c->debug);
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
            return problem(c, MO_CODE_CODEGEN_REPRESENTATION,
                           "Empty data declarations cannot come from lowering.");
        struct mo_type_case *cases = allocate(c, d->ctors.count, sizeof *cases);
        struct mo_codegen_index *seen = NULL;
        if (cases == NULL)
            return false;
        for (size_t j = 0; j < d->ctors.count; j++) {
            const struct mo_core_ctor *ctor = &d->ctors.items[j];
            if (mo_codegen_find(seen, ctor->name) != NULL)
                return problem(c, MO_CODE_CODEGEN_REPRESENTATION,
                               "Duplicate Core constructor names cannot come from lowering.");
            if (index_add(c, &seen, ctor->name, 0, false) == NULL)
                return false;
            if (ctor->fields.count > MO_LIMIT_FIELDS)
                return problem(c, MO_CODE_BYTECODE_TOO_BIG, "Constructor exceeds MO_LIMIT_FIELDS.");
            const char *n = name(c, ctor->name);
            struct mo_type_field *field = allocate(c, ctor->fields.count, sizeof *field);
            if (n == NULL || field == NULL)
                return false;
            struct mo_codegen_index *field_names = NULL;
            for (size_t k = 0; k < ctor->fields.count; k++) {
                mo_core_string key = ctor->fields.items[k].name;
                if ((key.length == 0 && ctor->fields.count != 1) ||
                    mo_codegen_find(field_names, key) != NULL)
                    return problem(c, MO_CODE_CODEGEN_REPRESENTATION,
                                   "Empty or duplicate named fields cannot come from lowering.");
                if (index_add(c, &field_names, key, 0, false) == NULL)
                    return false;
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
                                         .functions = m->exposed_funs.items,
                                         .global_count = (uint32_t)m->consts.count,
                                         .globals = m->consts.items};
        if (b->name == MO_NO_INDEX || index_add(c, &c->module_index, m->name, id, false) == NULL)
            return false;
    }
    if (!p->metadata.source && (p->data.count != 0 || p->consts.count != 0)) {
        struct mo_bytecode_module *m =
            append(c, &c->modules, 1, MO_LIMIT_MODULES, MO_CODE_BYTECODE_TOO_BIG);
        if (m == NULL || index_add(c, &c->module_index, text("%Core"), 0, false) == NULL)
            return false;
        *m = (struct mo_bytecode_module){.name = string(c, text("%Core"))};
        if (m->name == MO_NO_INDEX)
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
        if (!name_valid(c, fun->name) || !name_valid(c, fun->module))
            return false;
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
        c->parameters[i] = params;
        for (size_t j = 0; j < fun->params.count; j++) {
            if (!name_valid(c, fun->params.items[j].name))
                return false;
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
        c->bodies[i].source = fun;
        if (c->funs[i].result_type == MO_NO_INDEX ||
            index_add(c, &c->fun_index, fun->name, (uint32_t)i, false) == NULL)
            return false;
    }
    return c->error == 0;
}
static MO_MUST_USE uint32_t suffixed_name(struct compiler *c, mo_core_string base,
                                          const char *suffix) {
    size_t extra = strlen(suffix);
    if (!name_valid(c, base))
        return MO_NO_INDEX;
    if (base.length > MO_LIMIT_STRING_SIZE - extra) {
        if (!problem(c, MO_CODE_BYTECODE_TOO_BIG,
                     "Generated initializer name exceeds MO_LIMIT_STRING_SIZE."))
            return MO_NO_INDEX;
    }
    char *bytes = allocate(c, base.length + extra, 1);
    if (bytes == NULL)
        return MO_NO_INDEX;
    memcpy(bytes, base.bytes, base.length);
    memcpy(bytes + base.length, suffix, extra);
    return string(c, (mo_core_string){bytes, base.length + extra});
}
static MO_MUST_USE bool globals(struct compiler *c) {
    const struct mo_core *p = c->core;
    if (p->consts.count == 0)
        return true;
    c->globals = allocate(c, p->consts.count, sizeof *c->globals);
    c->init = allocate(c, p->consts.count, sizeof *c->init);
    if (c->globals == NULL || c->init == NULL)
        return false;
    if (p->metadata.source) {
        for (uint32_t i = 0; i < p->metadata.modules.count; i++) {
            const mo_core_indices owned = p->metadata.modules.items[i].consts;
            for (size_t j = 0; j < owned.count; j++)
                c->globals[owned.items[j]].module = i;
        }
    } else {
        if (p->consts.count > MO_LIMIT_MODULE_NAMES)
            return problem(c, MO_CODE_BYTECODE_TOO_BIG,
                           "Source-less constants exceed MO_LIMIT_MODULE_NAMES in %Core.");
        uint32_t *ids = allocate(c, p->consts.count, sizeof *ids);
        if (ids == NULL)
            return false;
        struct mo_bytecode_module *module = c->modules.items;
        module[0].global_count = (uint32_t)p->consts.count;
        module[0].globals = ids;
        for (uint32_t i = 0; i < p->consts.count; i++) {
            ids[i] = i;
            c->globals[i].module = 0;
        }
    }
    uint32_t unit = scalar(c, MO_TYPE_UNIT);
    if (unit == MO_NO_INDEX)
        return false;
    c->unit_constant = constant(c, (struct mo_bytecode_constant){.type = unit});
    if (c->unit_constant == MO_NO_INDEX)
        return false;
    for (uint32_t i = 0; i < p->consts.count; i++) {
        const struct mo_core_const *k = &p->consts.items[i];
        struct mo_core_fun *source = allocate(c, 1, sizeof *source);
        if (source == NULL)
            return false;
        *source = (struct mo_core_fun){.name = k->name, .ret = k->ty, .body = k->body};
        c->current = source;
        const struct mo_core_const_meta *meta =
            p->metadata.source ? &p->metadata.consts.items[i] : NULL;
        c->declaration = meta == NULL ? (struct mo_core_span){0} : meta->span;
        c->span = c->declaration;
        if (!name_valid(c, k->name))
            return false;
        mo_core_string full_name = meta == NULL ? k->name : meta->name;
        c->globals[i].name = string(c, full_name);
        c->globals[i].type = type(c, k->ty, 0);
        uint32_t value = (uint32_t)p->funs.count + 2 * i, init = value + 1;
        c->init[i] = init;
        c->funs[value] =
            (struct mo_bytecode_function){.name = suffixed_name(c, full_name, ".init.%value"),
                                          .module = c->globals[i].module,
                                          .kind = MO_FUNCTION_FUNCTION,
                                          .result_type = c->globals[i].type};
        c->funs[init] = (struct mo_bytecode_function){.name = suffixed_name(c, full_name, ".init"),
                                                      .module = c->globals[i].module,
                                                      .kind = MO_FUNCTION_INIT,
                                                      .result_type = unit};
        c->bodies[value].source = c->bodies[init].source = source;
        if (meta != NULL) {
            struct mo_core_fun_meta *body_meta = allocate(c, 1, sizeof *body_meta);
            if (body_meta == NULL)
                return false;
            *body_meta = (struct mo_core_fun_meta){
                .name = meta->name, .span = meta->span, .body = meta->body};
            c->bodies[value].meta = c->bodies[init].meta = body_meta;
        }
        if (c->error != 0 || index_add(c, &c->global_index, k->name, i, false) == NULL)
            return false;
    }
    return true;
}
static MO_MUST_USE bool bodies(struct compiler *c) {
    uint32_t declared = c->function_count;
    for (uint32_t i = 0; i < declared; i++) {
        struct function *f = &c->bodies[i];
        c->current = f->source;
        const struct mo_core_fun_meta *meta = f->meta;
        c->declaration = meta == NULL ? (struct mo_core_span){0} : meta->span;
        c->span = c->declaration;
        f->declaration = c->declaration;
        f->root = f;
        function_init(f);
        if (c->funs[i].kind == MO_FUNCTION_INIT)
            continue;
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
            for (size_t j = 0; j < meta->body.closures.count; j++) {
                const struct mo_core_closure_meta *closure = &meta->body.closures.items[j];
                uint32_t id = string(c, closure->name);
                if (id == MO_NO_INDEX ||
                    index_add(c, &f->closure_names,
                              (mo_core_string){(const char *)&closure->node, sizeof closure->node},
                              id, true) == NULL)
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
        f->body = prepare(c, f, c->current->body, c->funs[i].result_type, 0);
        if (f->body == NULL)
            return false;
    }
    uint32_t ignored = 0;
    if (type_result(c, mo_types_finish(&c->types), ignored) == MO_NO_INDEX)
        return false;
    for (uint32_t i = declared; i < c->function_count; i++) {
        const struct mo_bytecode_function *fun = &c->funs[i];
        enum mo_type_problem status =
            mo_types_layout(&c->types,
                            &(struct mo_layout){.kind = MO_LAYOUT_CLOSURE,
                                                .type = c->bodies[i].closure_type,
                                                .case_index = MO_NO_INDEX,
                                                .function = i,
                                                .fields = fun->capture_types,
                                                .field_count = fun->capture_count},
                            &ignored);
        if (type_result(c, status, ignored) == MO_NO_INDEX)
            return false;
    }
    c->layouts = allocate(c, c->types.count, sizeof *c->layouts);
    if (c->layouts == NULL)
        return false;
    for (uint32_t i = 0; i < c->types.count; i++) {
        const struct mo_type *t = &c->types.items[i];
        c->layouts[i] = MO_NO_INDEX;
        if (t->kind == MO_TYPE_TUPLE || t->kind == MO_TYPE_RECORD) {
            uint32_t count = t->kind == MO_TYPE_TUPLE ? t->parameter_count : t->field_count;
            const uint32_t *fields = t->parameters;
            if (t->kind == MO_TYPE_RECORD) {
                uint32_t *ids = allocate(c, count, sizeof *ids);
                if (ids == NULL)
                    return false;
                for (uint32_t j = 0; j < count; j++)
                    ids[j] = t->fields[j].type;
                fields = ids;
            }
            enum mo_type_problem status = mo_types_layout(
                &c->types,
                &(struct mo_layout){.kind = t->kind == MO_TYPE_TUPLE ? MO_LAYOUT_TUPLE
                                                                     : MO_LAYOUT_RECORD,
                                    .type = i,
                                    .case_index = MO_NO_INDEX,
                                    .function = MO_NO_INDEX,
                                    .fields = fields,
                                    .field_count = count},
                &c->layouts[i]);
            if (type_result(c, status, c->layouts[i]) == MO_NO_INDEX)
                return false;
        }
        if (t->kind == MO_TYPE_CHOICE) {
            for (uint32_t j = 0; j < t->case_count; j++) {
                if (mo_case_shape(&c->types, i, j) != MO_CASE_BOXED)
                    continue;
                uint32_t *fields = allocate(c, t->cases[j].field_count, sizeof *fields);
                if (fields == NULL)
                    return false;
                for (uint32_t k = 0; k < t->cases[j].field_count; k++)
                    fields[k] = t->cases[j].fields[k].type;
                enum mo_type_problem status =
                    mo_types_layout(&c->types,
                                    &(struct mo_layout){.kind = MO_LAYOUT_CASE,
                                                        .type = i,
                                                        .case_index = j,
                                                        .function = MO_NO_INDEX,
                                                        .fields = fields,
                                                        .field_count = t->cases[j].field_count},
                                    &ignored);
                if (type_result(c, status, ignored) == MO_NO_INDEX)
                    return false;
            }
        }
    }
    return true;
}
/* A normal call keeps the publication point reachable even for a body which always stops.
 * No code follows stop in the value function; its wrapper stores only a returned result. */
static MO_MUST_USE bool initializer(struct compiler *c, struct function *f, uint32_t index) {
    uint32_t global = (index - (uint32_t)c->core->funs.count) / 2;
    uint32_t value = register_add(c, f, c->globals[global].type, MO_NO_INDEX),
             unit = register_add(c, f, c->funs[index].result_type, MO_NO_INDEX);
    if (value == MO_NO_INDEX || unit == MO_NO_INDEX)
        return false;
    const uint32_t call[] = {value, index - 1}, store[] = {global, value},
                   result[] = {unit, c->unit_constant};
    struct mo_core_span source =
        f->meta == NULL ? (struct mo_core_span){0} : f->meta->body.spans.items[0];
    return emit(c, f, source, MO_OP_CALL, call, 2, NULL, 0) != MO_NO_INDEX &&
           emit(c, f, c->declaration, MO_OP_SET_GLOBAL, store, 2, NULL, 0) != MO_NO_INDEX &&
           emit(c, f, c->declaration, MO_OP_CONST, result, 2, NULL, 0) != MO_NO_INDEX &&
           emit(c, f, c->declaration, MO_OP_RET, &unit, 1, NULL, 0) != MO_NO_INDEX;
}
static MO_MUST_USE bool output(struct compiler *c, struct mo_buffer *out) {
    for (uint32_t i = 0; i < c->function_count; i++) {
        struct function *f = &c->bodies[i];
        c->current = f->source;
        struct mo_bytecode_function *fun = &c->funs[i];
        const struct mo_core_fun_meta *meta = f->meta;
        c->declaration = f->declaration;
        if (fun->kind != MO_FUNCTION_INIT && !live_infer(c, f))
            return false;
        /* Helpers added while initializing are already initialized. Do not reinitialize them. */
        c->span = c->declaration;
        for (uint32_t j = 0; j < fun->parameter_count; j++)
            if (reference(c, fun->parameters[j].type)) {
                bool owned = fun->parameters[j].ownership == MO_PARAMETER_OWNED;
                ((struct reg *)f->regs.items)[j].state = owned ? REF_OWNED : REF_BORROWED;
                if (owned)
                    owner_add(f, j);
            }
        bool closure = fun->kind == MO_FUNCTION_CLOSURE;
        uint32_t inputs = fun->parameter_count + (closure ? 1u : 0u);
        if (closure) {
            ((struct reg *)f->regs.items)[fun->parameter_count].state = REF_OWNED;
            owner_add(f, fun->parameter_count);
        }
        uint32_t count = f->regs.count;
        for (uint32_t j = inputs; j < count; j++)
            if (!initialize(c, f, j, 0))
                return false;
        uint32_t capture_start = f->code.count;
        for (uint32_t j = 0; j < f->captures.count; j++) {
            uint32_t r = ((struct capture *)f->captures.items)[j].reg;
            const uint32_t args[] = {r, fun->parameter_count, j};
            if (emit(c, f, c->declaration, MO_OP_FIELD, args, 3, NULL, 0) == MO_NO_INDEX)
                return false;
            if (reference(c, ((struct reg *)f->regs.items)[r].type))
                owner_add(f, r);
        }
        if (fun->kind == MO_FUNCTION_INIT) {
            if (!initializer(c, f, i))
                return false;
        } else {
            bool live;
            if (!generate(c, f, f->body, true, 0, &live))
                return false;
            MO_ASSERT(!live);
        }
        uint32_t *types = allocate(c, f->regs.count, sizeof *types);
        if (types == NULL)
            return false;
        for (uint32_t j = 0; j < f->regs.count; j++)
            types[j] = ((struct reg *)f->regs.items)[j].type;
        for (uint32_t j = 0; j < fun->parameter_count; j++)
            if (!named(c, f, j, 0, f->code.count))
                return false;
        for (uint32_t j = 0; j < f->captures.count; j++)
            if (!named(c, f, ((struct capture *)f->captures.items)[j].reg, capture_start + 4 * j,
                       f->code.count))
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
        if (meta != NULL && !closure) {
            if (meta->text.some)
                d->text = string(c, meta->text.value);
            if (meta->subject.some) {
                d->subject = meta->subject.value;
                /* Core metadata checks transport shape. Bytecode additionally requires
                 * a promise's parameters to be its subject's inputs followed by the result. */
                if (fun->kind == MO_FUNCTION_PROMISE) {
                    const struct mo_bytecode_function *subject = &c->funs[d->subject];
                    bool matches = fun->parameter_count == subject->parameter_count + 1u;
                    for (uint32_t j = 0; matches && j < subject->parameter_count; j++)
                        matches = fun->parameters[j].type == subject->parameters[j].type;
                    if (!matches ||
                        fun->parameters[subject->parameter_count].type != subject->result_type)
                        return problem(c, MO_CODE_CODEGEN_REPRESENTATION,
                                       "Promise metadata parameters differ from its subject.");
                }
            }
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
        if (!mo_codegen_prune(&c->arena, fun, d))
            return problem(c, MO_CODE_CODEGEN_MEMORY,
                           "No memory for code generation's reachability pass.");
    }
    const struct mo_bytecode file = {.string_count = c->strings.count,
                                     .strings = c->strings.items,
                                     .type_count = c->types.count,
                                     .types = c->types.items,
                                     .layout_count = c->types.layout_count,
                                     .layouts = c->types.layouts,
                                     .constant_count = c->constants.count,
                                     .constants = c->constants.items,
                                     .global_count = (uint32_t)c->core->consts.count,
                                     .globals = c->globals,
                                     .init_count = (uint32_t)c->core->consts.count,
                                     .init = c->init,
                                     .function_count = c->function_count,
                                     .functions = c->funs,
                                     .module_count = c->modules.count,
                                     .modules = c->modules.items,
                                     .debug_count = c->function_count,
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
    struct compiler c = {.core = core, .range_int = MO_NO_INDEX};
    mo_arena_init(&c.arena, MO_LIMIT_CODEGEN_SCRATCH_SIZE);
    mo_types_init(&c.types, &c.arena);
    c.strings.width = sizeof(struct mo_bytecode_string);
    c.constants.width = sizeof(struct mo_bytecode_constant);
    c.modules.width = sizeof(struct mo_bytecode_module);
    c.constructors.width = sizeof(struct mo_codegen_index *);
    c.ownership.width = sizeof(struct ownership);
    c.edges.width = sizeof(struct edge);
    c.constraints.width = sizeof(struct constraint);
    bool ok = declarations(&c) && globals(&c) && bodies(&c) && entry_valid(&c) &&
              abilities_valid(&c) && borrow_infer(&c) && output(&c, out);
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
                       : c.error == MO_CODE_CODEGEN_REPRESENTATION
                           ? "invalid lowered representation: bug in Mo"
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
