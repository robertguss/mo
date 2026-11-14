#include "codegen/codegen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/capabilities.h"
#include "base/codes.h"
#include "bytecode/bytecode.h"
#include "codegen/bigint.h"
#include "codegen/control.h"
#include "codegen/index.h"
#include "codegen/internal.h"
#include "codegen/operations.h"
#include "codegen/registers.h"
#include "heap/types.h"

MO_MUST_USE bool codegen_problem(struct compiler *c, uint16_t code, const char *reason) {
    if (c->error == 0) {
        c->error = code;
        c->reason = reason;
    }
    return false;
}
MO_MUST_USE bool codegen_unsupported(struct compiler *c, const char *reason) {
    return codegen_problem(c, MO_CODE_CODEGEN_UNSUPPORTED, reason);
}
MO_MUST_USE void *codegen_allocate(struct compiler *c, size_t count, size_t width) {
    void *p = mo_arena_allocate_array(&c->arena, count, width, MO_ARENA_ALIGNMENT);
    if (p == NULL) {
        if (!codegen_problem(c, MO_CODE_CODEGEN_MEMORY,
                             "Code generation exhausted its scratch limit or memory."))
            return NULL;
    }
    return p;
}
MO_MUST_USE void *codegen_append(struct compiler *c, struct vector *v, uint32_t n, uint32_t limit,
                                 uint16_t code) {
    if (n > limit - v->count) {
        if (!codegen_problem(
                c, code,
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
        void *p = codegen_allocate(c, capacity, v->width);
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
MO_MUST_USE bool codegen_service_shape(struct compiler *c, uint32_t type,
                                       const struct mo_service_shape *source) {
    if (c->services == NULL || c->services->count == 0)
        return true;
    struct mo_bytecode_service_shape *shape =
        codegen_append(c, &c->shapes, 1, MO_LIMIT_TYPES, MO_CODE_BYTECODE_TOO_BIG);
    if (shape == NULL)
        return false;
    bool *unnamed = NULL;
    if (source->case_count != 0) {
        unnamed = codegen_allocate(c, source->case_count, sizeof *unnamed);
        if (unnamed == NULL)
            return false;
        memcpy(unnamed, source->unnamed, source->case_count * sizeof *unnamed);
    }
    *shape =
        (struct mo_bytecode_service_shape){type, source->is_struct, source->case_count, unnamed};
    return true;
}
MO_MUST_USE mo_core_string codegen_text(const char *s) {
    return (mo_core_string){s, strlen(s)};
}
MO_MUST_USE struct mo_codegen_index *codegen_index_add(struct compiler *c,
                                                       struct mo_codegen_index **root,
                                                       mo_core_string key, uint32_t value,
                                                       bool copy) {
    struct mo_codegen_index *entry = mo_codegen_find(*root, key);
    if (entry != NULL)
        return entry;
    if (copy) {
        char *bytes = codegen_allocate(c, key.length, 1);
        if (bytes == NULL)
            return NULL;
        if (key.length != 0)
            memcpy(bytes, key.bytes, key.length);
        key.bytes = bytes;
    }
    entry = mo_codegen_insert(&c->arena, root, key, value);
    if (entry == NULL &&
        !codegen_problem(c, MO_CODE_CODEGEN_MEMORY, "No memory for a code generation index."))
        return NULL;
    return entry;
}
MO_MUST_USE uint32_t codegen_string(struct compiler *c, mo_core_string s) {
    struct mo_codegen_index *old = mo_codegen_find(c->string_index, s);
    if (old != NULL)
        return old->value;
    if (s.length > MO_LIMIT_STRING_SIZE) {
        if (!codegen_problem(c, MO_CODE_BYTECODE_TOO_BIG,
                             "A generated string exceeds MO_LIMIT_STRING_SIZE."))
            return MO_NO_INDEX;
    }
    uint32_t id = c->strings.count;
    struct mo_bytecode_string *item =
        codegen_append(c, &c->strings, 1, MO_LIMIT_STRINGS, MO_CODE_BYTECODE_TOO_BIG);
    struct mo_codegen_index *entry = codegen_index_add(c, &c->string_index, s, id, true);
    if (item == NULL || entry == NULL)
        return MO_NO_INDEX;
    *item = (struct mo_bytecode_string){(uint32_t)s.length, (const uint8_t *)entry->key.bytes};
    return id;
}
MO_MUST_USE bool codegen_name_valid(struct compiler *c, mo_core_string s) {
    return (s.length != 0 && memchr(s.bytes, 0, s.length) == NULL) ||
           codegen_problem(c, MO_CODE_CODEGEN_REPRESENTATION,
                           "Empty or NUL-containing Core names cannot come from lowering.");
}
MO_MUST_USE const char *codegen_name(struct compiler *c, mo_core_string s) {
    if (!codegen_name_valid(c, s))
        return NULL;
    uint32_t id = codegen_string(c, s);
    if (id == MO_NO_INDEX)
        return NULL;
    char *p = codegen_allocate(c, s.length + 1, 1);
    if (p == NULL)
        return NULL;
    memcpy(p, s.bytes, s.length);
    p[s.length] = 0;
    return p;
}
MO_MUST_USE uint32_t codegen_suffixed_name(struct compiler *c, mo_core_string base,
                                           const char *suffix) {
    size_t extra = strlen(suffix);
    if (!codegen_name_valid(c, base))
        return MO_NO_INDEX;
    if (base.length > MO_LIMIT_STRING_SIZE - extra) {
        if (!codegen_problem(c, MO_CODE_BYTECODE_TOO_BIG,
                             "Generated initializer name exceeds MO_LIMIT_STRING_SIZE."))
            return MO_NO_INDEX;
    }
    char *bytes = codegen_allocate(c, base.length + extra, 1);
    if (bytes == NULL)
        return MO_NO_INDEX;
    memcpy(bytes, base.bytes, base.length);
    memcpy(bytes + base.length, suffix, extra);
    return codegen_string(c, (mo_core_string){bytes, base.length + extra});
}
MO_MUST_USE uint32_t codegen_type_result(struct compiler *c, enum mo_type_problem status,
                                         uint32_t id) {
    if (status == MO_TYPES_OK)
        return id;
    uint16_t code =
        status == MO_TYPES_NO_MEMORY ? MO_CODE_CODEGEN_MEMORY : MO_CODE_BYTECODE_TOO_BIG;
    if (status == MO_TYPES_INVALID)
        code = MO_CODE_CODEGEN_INVALID;
    if (!codegen_problem(c, code,
                         "The generated type or layout was refused by the runtime type builder."))
        return MO_NO_INDEX;
    return MO_NO_INDEX;
}
MO_MUST_USE uint32_t codegen_scalar(struct compiler *c, enum mo_type_kind kind) {
    uint32_t id = MO_NO_INDEX;
    enum mo_type_problem status = mo_types_intern(&c->types, &(struct mo_type){.kind = kind}, &id);
    return codegen_type_result(c, status, id);
}
static MO_MUST_USE bool type_text(struct compiler *c, struct mo_buffer *out, const char *s) {
    size_t length = strlen(s);
    if (length > out->limit - out->length)
        return codegen_problem(c, MO_CODE_BYTECODE_TOO_BIG,
                               "Instantiated type name exceeds MO_LIMIT_STRING_SIZE.");
    return mo_buffer_append(out, s, length) ||
           codegen_problem(c, MO_CODE_CODEGEN_MEMORY, "No memory for an instantiated type name.");
}
static MO_MUST_USE bool type_name(struct compiler *c, uint32_t id, struct mo_buffer *out,
                                  unsigned depth) {
    if (depth >= MO_LIMIT_CORE_DEPTH)
        return codegen_problem(c, MO_CODE_NESTED_TOO_DEEPLY,
                               "Instantiated type name nesting exceeds MO_LIMIT_CORE_DEPTH.");
    static const char *const names[] = {"Unit", "Bool",  "Byte",     "Int",     "Float",
                                        "Char", "Time",  "Duration", "Decimal", "Range",
                                        "Text", "Bytes", "BigInt"};
    const struct mo_type *t = &c->types.items[id];
    if (t->kind == MO_TYPE_CAPABILITY)
        return type_text(c, out, mo_capability_names[t->capability]);
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
MO_MUST_USE uint32_t codegen_type(struct compiler *c, mo_core_ty t, unsigned depth) {
    if (depth >= MO_LIMIT_CORE_DEPTH) {
        if (!codegen_problem(c, MO_CODE_NESTED_TOO_DEEPLY,
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
    if (id == MO_NO_INDEX || codegen_index_add(c, &c->annotation_index, key, id, true) == NULL)
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
        c->range_int = codegen_scalar(c, MO_TYPE_INT);
        if (c->range_int == MO_NO_INDEX)
            return MO_NO_INDEX;
        kind = MO_TYPE_RANGE;
        break;
    case MO_CORE_TY_TUPLE: {
        if (t->as.tuple.count < 2) {
            if (!codegen_problem(c, MO_CODE_CODEGEN_REPRESENTATION,
                                 "Zero/one-element tuples cannot come from lowering."))
                return MO_NO_INDEX;
        }
        if (t->as.tuple.count > MO_LIMIT_FIELDS) {
            if (!codegen_problem(c, MO_CODE_BYTECODE_TOO_BIG, "Tuple exceeds MO_LIMIT_FIELDS."))
                return MO_NO_INDEX;
        }
        uint32_t *params = codegen_allocate(c, t->as.tuple.count, sizeof *params);
        if (params == NULL)
            return MO_NO_INDEX;
        for (size_t i = 0; i < t->as.tuple.count; i++) {
            params[i] = codegen_type(c, t->as.tuple.items[i], depth + 1);
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
        return codegen_type_result(c, status, id);
    }
    case MO_CORE_TY_DATA: {
        struct mo_codegen_index *entry = mo_codegen_find(c->data_index, t->as.data);
        if (entry != NULL)
            return entry->value;
        if (!codegen_problem(c, MO_CODE_CODEGEN_REPRESENTATION,
                             "Undeclared data types cannot come from lowering."))
            return MO_NO_INDEX;
        return MO_NO_INDEX;
    }
    case MO_CORE_TY_OPTION:
    case MO_CORE_TY_RESULT:
    case MO_CORE_TY_ORDERING:
    case MO_CORE_TY_FILE_ERROR:
    case MO_CORE_TY_SIMULATION_ERROR:
    case MO_CORE_TY_ENTRY_KIND: {
        uint32_t a = MO_NO_INDEX, b = MO_NO_INDEX;
        if (t->tag == MO_CORE_TY_OPTION)
            a = codegen_type(c, t->as.option, depth + 1);
        if (t->tag == MO_CORE_TY_RESULT) {
            a = codegen_type(c, t->as.result.t, depth + 1);
            b = codegen_type(c, t->as.result.e, depth + 1);
        }
        if (c->error != 0)
            return MO_NO_INDEX;
        const uint32_t key[] = {(uint32_t)t->tag, a, b};
        mo_core_string bytes = {(const char *)key, sizeof key};
        struct mo_codegen_index *old = mo_codegen_find(c->builtin_index, bytes);
        if (old != NULL)
            return old->value;
        const char *base = t->tag == MO_CORE_TY_OPTION             ? "Option"
                           : t->tag == MO_CORE_TY_RESULT           ? "Result"
                           : t->tag == MO_CORE_TY_ORDERING         ? "Ordering"
                           : t->tag == MO_CORE_TY_FILE_ERROR       ? "FileError"
                           : t->tag == MO_CORE_TY_SIMULATION_ERROR ? "SimulationError"
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
            ok ? codegen_name(c, (mo_core_string){(const char *)buf.bytes, buf.length}) : NULL;
        mo_buffer_free(&buf);
        if (nominal == NULL) {
            if (!codegen_problem(c, MO_CODE_CODEGEN_MEMORY,
                                 "No memory for an instantiated type name."))
                return MO_NO_INDEX;
        }
        uint32_t id = MO_NO_INDEX;
        enum mo_type_problem status = mo_types_declare(&c->types, MO_TYPE_CHOICE, nominal, &id);
        if (codegen_type_result(c, status, id) == MO_NO_INDEX)
            return MO_NO_INDEX;
        const char *const *names;
        static const char *const option[] = {"None", "Some"}, *const result[] = {"Ok", "Error"},
                                 *const ordering[] = {"Less", "Equal", "Greater"},
                                 *const entry[] = {"File", "Folder", "Link", "Other"},
                                 *const simulation_errors[] = {"ServiceDown", "ToolFailed"},
                                 *const errors[] = {"NotFound", "NotAllowed", "NotText", "Failed"};
        names = t->tag == MO_CORE_TY_OPTION             ? option
                : t->tag == MO_CORE_TY_RESULT           ? result
                : t->tag == MO_CORE_TY_ORDERING         ? ordering
                : t->tag == MO_CORE_TY_FILE_ERROR       ? errors
                : t->tag == MO_CORE_TY_SIMULATION_ERROR ? simulation_errors
                                                        : entry;
        struct mo_type_field fields[] = {{NULL, a}, {NULL, b}};
        if (t->tag == MO_CORE_TY_FILE_ERROR) {
            uint32_t text_type = codegen_scalar(c, MO_TYPE_TEXT);
            if (text_type == MO_NO_INDEX ||
                codegen_string(c, codegen_text("path")) == MO_NO_INDEX ||
                codegen_string(c, codegen_text("reason")) == MO_NO_INDEX)
                return MO_NO_INDEX;
            fields[0] = (struct mo_type_field){"path", text_type};
            fields[1] = (struct mo_type_field){"reason", text_type};
        }
        struct mo_type_case cases[4] = {{0}};
        uint32_t n = t->tag == MO_CORE_TY_ENTRY_KIND || t->tag == MO_CORE_TY_FILE_ERROR ? 4
                     : t->tag == MO_CORE_TY_ORDERING                                    ? 3
                                                                                        : 2;
        for (uint32_t i = 0; i < n; i++) {
            if (codegen_string(c, codegen_text(names[i])) == MO_NO_INDEX)
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
        const bool unnamed[] = {cases[0].unnamed, cases[1].unnamed, cases[2].unnamed,
                                cases[3].unnamed};
        if (codegen_type_result(c, status, id) == MO_NO_INDEX ||
            !codegen_service_shape(
                c, id, &(struct mo_service_shape){.case_count = n, .unnamed = unnamed}) ||
            codegen_index_add(c, &c->builtin_index, bytes, id, true) == NULL)
            return MO_NO_INDEX;
        return id;
    }
    case MO_CORE_TY_FN: {
        if (t->as.fn.params.count > MO_LIMIT_PARAMETERS) {
            if (!codegen_problem(c, MO_CODE_BYTECODE_TOO_BIG,
                                 "Closure signature exceeds MO_LIMIT_PARAMETERS."))
                return MO_NO_INDEX;
        }
        uint32_t *params = codegen_allocate(c, t->as.fn.params.count, sizeof *params);
        if (params == NULL)
            return MO_NO_INDEX;
        for (size_t i = 0; i < t->as.fn.params.count; i++) {
            params[i] = codegen_type(c, t->as.fn.params.items[i], depth + 1);
            if (params[i] == MO_NO_INDEX)
                return MO_NO_INDEX;
        }
        uint32_t result = codegen_type(c, t->as.fn.ret, depth + 1), id = MO_NO_INDEX;
        if (result == MO_NO_INDEX)
            return MO_NO_INDEX;
        enum mo_type_problem status =
            mo_types_intern(&c->types,
                            &(struct mo_type){.kind = MO_TYPE_FUNCTION,
                                              .parameters = params,
                                              .parameter_count = (uint32_t)t->as.fn.params.count,
                                              .result = result},
                            &id);
        return codegen_type_result(c, status, id);
    }
    case MO_CORE_TY_LIST:
    case MO_CORE_TY_SET:
    case MO_CORE_TY_MAP: {
        uint32_t params[2], count = t->tag == MO_CORE_TY_MAP ? 2 : 1;
        params[0] = codegen_type(c,
                                 t->tag == MO_CORE_TY_MAP    ? t->as.map.k
                                 : t->tag == MO_CORE_TY_LIST ? t->as.list
                                                             : t->as.set,
                                 depth + 1);
        if (params[0] == MO_NO_INDEX)
            return MO_NO_INDEX;
        if (count == 2 && (params[1] = codegen_type(c, t->as.map.v, depth + 1)) == MO_NO_INDEX)
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
        return codegen_type_result(c, status, id);
    }
    case MO_CORE_TY_ENTRY: {
        const uint32_t key[] = {MO_CORE_TY_ENTRY, MO_NO_INDEX, MO_NO_INDEX};
        mo_core_string bytes = {(const char *)key, sizeof key};
        struct mo_codegen_index *old = mo_codegen_find(c->builtin_index, bytes);
        if (old != NULL)
            return old->value;
        /* These immutable synthetic annotations are translated directly, not pointer-memoized. */
        struct mo_core_ty entry_kind = {.tag = MO_CORE_TY_ENTRY_KIND};
        uint32_t text_type = codegen_scalar(c, MO_TYPE_TEXT),
                 kind_type = type_uncached(c, &entry_kind, 0);
        if (text_type == MO_NO_INDEX || kind_type == MO_NO_INDEX ||
            codegen_string(c, codegen_text("Entry")) == MO_NO_INDEX ||
            codegen_string(c, codegen_text("name")) == MO_NO_INDEX ||
            codegen_string(c, codegen_text("kind")) == MO_NO_INDEX)
            return MO_NO_INDEX;
        uint32_t id = MO_NO_INDEX;
        enum mo_type_problem status = mo_types_declare(&c->types, MO_TYPE_RECORD, "Entry", &id);
        if (codegen_type_result(c, status, id) == MO_NO_INDEX)
            return MO_NO_INDEX;
        const struct mo_type_field fields[] = {{"name", text_type}, {"kind", kind_type}};
        status = mo_types_define_record(&c->types, id, fields, 2);
        if (codegen_type_result(c, status, id) == MO_NO_INDEX ||
            !codegen_service_shape(c, id, &(struct mo_service_shape){.is_struct = true}) ||
            codegen_index_add(c, &c->builtin_index, bytes, id, true) == NULL)
            return MO_NO_INDEX;
        return id;
    }
    case MO_CORE_TY_SIMULATION:
    case MO_CORE_TY_CAP: {
        static const enum mo_capability caps[] = {MO_CAP_CONSOLE, MO_CAP_FILES,  MO_CAP_READ_FILES,
                                                  MO_CAP_CLOCK,   MO_CAP_RANDOM, MO_CAP_ARGS,
                                                  MO_CAP_ENV};
        uint32_t id = MO_NO_INDEX;
        enum mo_type_problem status = mo_types_intern(
            &c->types,
            &(struct mo_type){.kind = MO_TYPE_CAPABILITY,
                              .capability = t->tag == MO_CORE_TY_SIMULATION ? MO_CAP_SIMULATION
                                                                            : caps[t->as.cap->tag]},
            &id);
        return codegen_type_result(c, status, id);
    }
    }
    MO_ASSERT((uint32_t)t->tag < MO_CORE_TY_TUPLE);
    return codegen_scalar(c, kind);
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
    MO_ASSERT(depth < MO_LIMIT_DEF_SIGNATURE_DEPTH);
    struct mo_core_ty *t = codegen_allocate(c, 1, sizeof *t);
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
                    t->as.cap = codegen_allocate(c, 1, sizeof *t->as.cap);
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
        t->as.tuple.items = codegen_allocate(c, count, sizeof *arguments);
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
MO_MUST_USE uint32_t codegen_simulation_signature(struct compiler *c,
                                                  const struct mo_core_sim_op *op) {
    struct mo_core_ty *types = codegen_allocate(c, 3, sizeof *types);
    if (types == NULL)
        return MO_NO_INDEX;
    types[0] = (struct mo_core_ty){.tag = MO_CORE_TY_SIMULATION};
    types[1] = (struct mo_core_ty){.tag = MO_CORE_TY_SIMULATION_ERROR};
    types[2] = (struct mo_core_ty){.tag = MO_CORE_TY_RESULT, .as.result = {op->result, &types[1]}};
    uint32_t parameters[MO_LIMIT_BYTECODE_PARAMETERS];
    uint32_t count = 1, result;
    parameters[0] = codegen_type(c, &types[0], 0);
    if (op->tag == MO_SIM_START) {
        result = parameters[0];
        parameters[0] = codegen_scalar(c, MO_TYPE_TIME);
    } else if (op->tag == MO_SIM_SEND) {
        if (op->arguments.count >= MO_LIMIT_BYTECODE_PARAMETERS) {
            bool reported =
                codegen_problem(c, MO_CODE_BYTECODE_TOO_BIG,
                                "Simulation send exceeds MO_LIMIT_BYTECODE_PARAMETERS.");
            (void)reported;
            return MO_NO_INDEX;
        }
        for (size_t i = 0; i < op->arguments.count; i++)
            parameters[count++] = codegen_type(c, op->arguments.items[i], 0);
        result = codegen_type(c, &types[2], 0);
    } else {
        result = codegen_scalar(c, MO_TYPE_UNIT);
        if (op->tag == MO_SIM_ADVANCE)
            parameters[count++] = codegen_scalar(c, MO_TYPE_DURATION);
    }
    if (c->error != 0)
        return MO_NO_INDEX;
    uint32_t id = MO_NO_INDEX;
    enum mo_type_problem status = mo_types_intern(&c->types,
                                                  &(struct mo_type){.kind = MO_TYPE_FUNCTION,
                                                                    .parameters = parameters,
                                                                    .parameter_count = count,
                                                                    .result = result},
                                                  &id);
    return codegen_type_result(c, status, id);
}

MO_MUST_USE uint32_t codegen_selector(mo_core_natural number) {
    uint32_t out = 0;
    for (size_t i = 0; i < number.digits.length; i++) {
        uint32_t digit = (uint32_t)(number.digits.bytes[i] - '0');
        MO_ASSERT(digit <= 9 && out <= (UINT32_MAX - digit) / 10);
        out = out * 10 + digit;
    }
    return out;
}

MO_MUST_USE uint32_t codegen_operation_signature(struct compiler *c,
                                                 const struct mo_codegen_operation *op,
                                                 const mo_core_ty variables[4]) {
    uint32_t key[7] = {
        (uint32_t)op->opcode,   op->row, MO_NO_INDEX, MO_NO_INDEX, MO_NO_INDEX, MO_NO_INDEX,
        (uint32_t)op->expansion};
    for (uint32_t i = 0; i < 4; i++)
        if (variables[i] != NULL && (key[i + 2] = codegen_type(c, variables[i], 0)) == MO_NO_INDEX)
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
                if (t == NULL || (params[count++] = codegen_type(c, t, 0)) == MO_NO_INDEX)
                    return MO_NO_INDEX;
            } while (signature_take(&cursor, ','));
            bool closed = signature_take(&cursor, ')');
            MO_ASSERT(closed);
        }
    }
    uint32_t result = MO_NO_INDEX;
    if (!function || signature_take(&cursor, ':')) {
        mo_core_ty t = signature_type(c, &cursor, variables, 0);
        if (t == NULL || (result = codegen_type(c, t, 0)) == MO_NO_INDEX)
            return MO_NO_INDEX;
    } else
        result = codegen_scalar(c, MO_TYPE_UNIT);
    uint32_t id = MO_NO_INDEX;
    enum mo_type_problem status = mo_types_intern(&c->types,
                                                  &(struct mo_type){.kind = MO_TYPE_FUNCTION,
                                                                    .parameters = params,
                                                                    .parameter_count = count,
                                                                    .result = result},
                                                  &id);
    if (codegen_type_result(c, status, id) == MO_NO_INDEX ||
        codegen_index_add(c, &c->operation_index, bytes, id, true) == NULL)
        return MO_NO_INDEX;
    while (*cursor == ' ')
        cursor++;
    uint32_t constrained = MO_NO_INDEX;
    uint8_t ability = 0;
    if (op->opcode == MO_OP_EQUAL) {
        constrained = key[2];
        ability = MO_ABILITY_EQUAL;
    } else if (*cursor != 0) {
        MO_ASSERT(strncmp(cursor, "where ", 6) == 0);
        cursor += 6;
        unsigned slot = *cursor == 'T' ? 0u : *cursor == 'U' ? 1u : *cursor == 'K' ? 2u : 3u;
        MO_ASSERT((*cursor == 'T' || *cursor == 'U' || *cursor == 'K' || *cursor == 'V') &&
                  key[2 + slot] != MO_NO_INDEX && cursor[1] == ':' && cursor[2] == ' ');
        constrained = key[2 + slot];
        cursor += 3;
        MO_ASSERT(strcmp(cursor, "Equal") == 0 || strcmp(cursor, "Hash") == 0);
        ability = strcmp(cursor, "Equal") == 0 ? MO_ABILITY_EQUAL : MO_ABILITY_HASH;
    }
    if (ability != 0) {
        struct constraint *item =
            codegen_append(c, &c->constraints, 1, MO_LIMIT_CORE_NODES, MO_CODE_CODEGEN_MEMORY);
        if (item == NULL)
            return MO_NO_INDEX;
        *item = (struct constraint){constrained, ability, c->current, c->span, c->declaration};
    }
    return id;
}
MO_MUST_USE uint32_t codegen_constant(struct compiler *c, struct mo_bytecode_constant value) {
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
        char *payload = codegen_allocate(c, prefix + data.length, 1);
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
        codegen_append(c, &c->constants, 1, MO_LIMIT_CONSTANTS, MO_CODE_BYTECODE_TOO_BIG);
    if (item == NULL || codegen_index_add(c, &c->constant_index, bytes, id, true) == NULL)
        return MO_NO_INDEX;
    *item = value;
    return id;
}
MO_MUST_USE uint32_t codegen_literal(struct compiler *c, mo_core_lit lit, uint32_t *ty) {
    static const enum mo_type_kind kinds[] = {MO_TYPE_INT,     MO_TYPE_BIGINT, MO_TYPE_BYTE,
                                              MO_TYPE_DECIMAL, MO_TYPE_FLOAT,  MO_TYPE_BOOL,
                                              MO_TYPE_CHAR,    MO_TYPE_TEXT,   MO_TYPE_BYTES,
                                              MO_TYPE_UNIT,    MO_TYPE_TIME,   MO_TYPE_DURATION};
    *ty = codegen_scalar(c, kinds[lit->tag]);
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
            if (!codegen_problem(c,
                                 status == MO_CODEGEN_BIGINT_LIMIT ? MO_CODE_CODEGEN_BIGINT_WORK
                                                                   : MO_CODE_CODEGEN_MEMORY,
                                 status == MO_CODEGEN_BIGINT_LIMIT
                                     ? "BigInt conversion exceeds MO_LIMIT_CODEGEN_BIGINT_WORK."
                                     : "No memory for BigInt literal conversion."))
                return MO_NO_INDEX;
        }
        uint32_t id = codegen_constant(c, k);
        if (id == MO_NO_INDEX ||
            codegen_index_add(c, &c->bigint_index[sign], digits, id, false) == NULL)
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
        uint32_t s = codegen_string(c, lit->as.text);
        if (s == MO_NO_INDEX)
            return MO_NO_INDEX;
        struct mo_bytecode_constant k = {.type = *ty};
        k.value.string = s;
        return codegen_constant(c, k);
    }
    case MO_CORE_LIT_BYTES: {
        size_t count = lit->as.bytes.count;
        if (count > MO_LIMIT_STRING_SIZE) {
            if (!codegen_problem(c, MO_CODE_BYTECODE_TOO_BIG,
                                 "Bytes literal exceeds MO_LIMIT_STRING_SIZE."))
                return MO_NO_INDEX;
        }
        uint8_t *payload = codegen_allocate(c, count, 1);
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
        return codegen_constant(c, k);
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
    return codegen_constant(c, k);
}
MO_MUST_USE uint32_t codegen_register_add(struct compiler *c, struct function *f, uint32_t ty,
                                          uint32_t original) {
    if (ty == MO_NO_INDEX)
        return MO_NO_INDEX;
    uint32_t id = f->regs.count;
    /* Virtual slots share the ownership graph's bound. The encoded register and
     * register × block limits apply to the physical allocation, after pruning. */
    struct reg *r =
        codegen_append(c, &f->regs, 1, MO_LIMIT_CODEGEN_OWNERSHIP_SLOTS, MO_CODE_CODEGEN_MEMORY);
    if (r == NULL)
        return MO_NO_INDEX;
    uint32_t flow = c->ownership.count;
    struct ownership *o = codegen_append(c, &c->ownership, 1, MO_LIMIT_CODEGEN_OWNERSHIP_SLOTS,
                                         MO_CODE_CODEGEN_MEMORY);
    if (o == NULL)
        return MO_NO_INDEX;
    *o = (struct ownership){.type = ty, .edges = MO_NO_INDEX};
    *r = (struct reg){
        .type = ty, .name = original, .flow = flow, .previous = MO_NO_INDEX, .next = MO_NO_INDEX};
    return id;
}
MO_MUST_USE uint32_t codegen_binding_name(struct function *f, uint32_t node, uint32_t slot) {
    const uint32_t key[] = {node, slot};
    struct mo_codegen_index *entry =
        mo_codegen_find(f->bindings, (mo_core_string){(const char *)key, sizeof key});
    return entry == NULL ? MO_NO_INDEX : entry->value;
}
MO_MUST_USE uint32_t codegen_constructor(struct compiler *c, uint32_t ty, mo_core_string key) {
    if (c->types.items[ty].kind == MO_TYPE_RECORD) {
        MO_ASSERT(key.length == 5 && memcmp(key.bytes, "Entry", 5) == 0);
        return 0;
    }
    if (c->constructors.count < c->types.count) {
        uint32_t count = c->types.count - c->constructors.count;
        struct mo_codegen_index **new_roots =
            codegen_append(c, &c->constructors, count, MO_LIMIT_TYPES, MO_CODE_BYTECODE_TOO_BIG);
        if (new_roots == NULL)
            return MO_NO_INDEX;
        memset(new_roots, 0, (size_t)count * sizeof *new_roots);
    }
    struct mo_codegen_index **root = &((struct mo_codegen_index **)c->constructors.items)[ty];
    const struct mo_type *t = &c->types.items[ty];
    if (*root == NULL) {
        for (uint32_t i = 0; i < t->case_count; i++)
            if (codegen_index_add(c, root, codegen_text(t->cases[i].name), i, false) == NULL)
                return MO_NO_INDEX;
    }
    struct mo_codegen_index *entry = mo_codegen_find(*root, key);
    MO_ASSERT(entry != NULL);
    return entry->value;
}
void codegen_function_init(struct function *f) {
    f->regs.width = sizeof(struct reg);
    f->code.width = sizeof(uint32_t);
    f->spans.width = sizeof(struct mo_bytecode_span);
    f->names.width = sizeof(struct mo_bytecode_register_name);
    f->captures.width = sizeof(struct capture);
    f->joins.width = sizeof(struct join);
    f->changes.width = sizeof(struct change);
    f->owned = MO_NO_INDEX;
}

static MO_MUST_USE bool output(struct compiler *c, struct mo_buffer *out) {
    for (uint32_t i = 0; i < c->function_count; i++) {
        struct function *f = &c->bodies[i];
        c->current = f->source;
        struct mo_bytecode_function *fun = &c->funs[i];
        const struct mo_core_fun_meta *meta = f->meta;
        c->declaration = f->declaration;
        if (fun->kind != MO_FUNCTION_INIT && !codegen_live_infer(c, f))
            return false;
        /* Helpers added while initializing are already initialized. Do not reinitialize them. */
        c->span = c->declaration;
        for (uint32_t j = 0; j < fun->parameter_count; j++)
            if (codegen_reference(c, fun->parameters[j].type)) {
                bool owned = fun->parameters[j].ownership == MO_PARAMETER_OWNED;
                ((struct reg *)f->regs.items)[j].state = owned ? REF_OWNED : REF_BORROWED;
                if (owned)
                    codegen_owner_add(f, j);
            }
        bool closure = fun->kind == MO_FUNCTION_CLOSURE;
        uint32_t inputs = fun->parameter_count + (closure ? 1u : 0u);
        if (closure) {
            ((struct reg *)f->regs.items)[fun->parameter_count].state = REF_OWNED;
            codegen_owner_add(f, fun->parameter_count);
        }
        uint32_t count = f->regs.count;
        for (uint32_t j = inputs; j < count; j++)
            if (!codegen_initialize(c, f, j, 0))
                return false;
        uint32_t capture_start = f->code.count;
        for (uint32_t j = 0; j < f->captures.count; j++) {
            uint32_t r = ((struct capture *)f->captures.items)[j].reg;
            const uint32_t args[] = {r, fun->parameter_count, j};
            if (codegen_emit(c, f, c->declaration, MO_OP_FIELD, args, 3, NULL, 0) == MO_NO_INDEX)
                return false;
            if (codegen_reference(c, ((struct reg *)f->regs.items)[r].type))
                codegen_owner_add(f, r);
        }
        if (fun->kind == MO_FUNCTION_INIT) {
            if (!codegen_initializer(c, f, i))
                return false;
        } else {
            bool live = codegen_reachable_walk(f, f->body, 0);
            if (!codegen_generate(c, f, f->body, true, 0, &live))
                return false;
            MO_ASSERT(!live);
        }
        uint32_t *types = codegen_allocate(c, f->regs.count, sizeof *types);
        if (types == NULL)
            return false;
        for (uint32_t j = 0; j < f->regs.count; j++)
            types[j] = ((struct reg *)f->regs.items)[j].type;
        for (uint32_t j = 0; j < fun->parameter_count; j++)
            if (!codegen_named(c, f, j, 0, f->code.count))
                return false;
        for (uint32_t j = 0; j < f->captures.count; j++)
            if (!codegen_named(c, f, ((struct capture *)f->captures.items)[j].reg,
                               capture_start + 4 * j, f->code.count))
                return false;
        fun->register_count = f->regs.count;
        fun->register_types = types;
        fun->code_count = f->code.count;
        fun->code = f->code.items;
        struct mo_bytecode_span declaration = codegen_span(c, c->declaration);
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
                d->text = codegen_string(c, meta->text.value);
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
                        return codegen_problem(
                            c, MO_CODE_CODEGEN_REPRESENTATION,
                            "Promise metadata parameters differ from its subject.");
                }
            }
            if (meta->allows.some)
                d->expectation = meta->allows.value ? MO_BYTECODE_ALLOWS : MO_BYTECODE_FORBIDS;
            struct mo_bytecode_spec_line *lines =
                codegen_allocate(c, meta->lines.count, sizeof *lines);
            if (lines == NULL)
                return false;
            for (size_t j = 0; j < meta->lines.count; j++) {
                struct mo_bytecode_span s = codegen_span(c, meta->lines.items[j].span);
                lines[j] = (struct mo_bytecode_spec_line){
                    s.file, s.start, s.end, codegen_string(c, meta->lines.items[j].text)};
                if (lines[j].text == MO_NO_INDEX || !codegen_debug_add(c))
                    return false;
            }
            d->line_count = (uint32_t)meta->lines.count;
            d->lines = lines;
        }
        if (c->error != 0)
            return false;
        if (!mo_codegen_prune(&c->arena, fun, d))
            return codegen_problem(c, MO_CODE_CODEGEN_MEMORY,
                                   "No memory for code generation's reachability pass.");
        const struct mo_bytecode context = {.type_count = c->types.count,
                                            .types = c->types.items,
                                            .function_count = c->function_count,
                                            .functions = c->funs};
        uint16_t allocation = mo_codegen_registers(&c->arena, &context, fun, d, capture_start);
        if (allocation != 0)
            return codegen_problem(
                c, allocation,
                allocation == MO_CODE_TOO_MANY_REGISTERS
                    ? "Function exceeds MO_LIMIT_FUNCTION_REGISTERS after recycling."
                : allocation == MO_CODE_BYTECODE_TOO_BIG
                    ? "Function exceeds MO_LIMIT_FUNCTION_REGISTER_BLOCKS after recycling."
                : allocation == MO_CODE_FUNCTION_TOO_LONG
                    ? "Recycled code exceeds MO_LIMIT_FUNCTION_INSTRUCTIONS or "
                      "MO_LIMIT_FUNCTION_CODE_WORDS."
                : allocation == MO_CODE_CODEGEN_REGISTER_WORK
                    ? "Register recycling exceeds MO_LIMIT_CODEGEN_REGISTER_WORK."
                    : "No memory for code generation's register recycling pass.");
    }
    struct mo_bytecode file = {
        .string_count = c->strings.count,
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
        .file_count = c->core->metadata.source ? (uint32_t)c->core->metadata.files.count : 0,
        .files = c->files,
        .debug_count = c->function_count,
        .debug = c->debug};
    if (!codegen_service_descriptors(c, &file))
        return false;
    struct mo_write_error error;
    struct mo_buffer encoded;
    mo_buffer_init(&encoded, MO_LIMIT_BYTECODE_SIZE);
    bool written = mo_bytecode_write(&file, &encoded, &error);
    if (written) {
        bool fits = encoded.length <= out->limit;
        bool ok = fits && mo_buffer_append(out, encoded.bytes, encoded.length);
        mo_buffer_free(&encoded);
        return ok || codegen_problem(c, fits ? MO_CODE_CODEGEN_MEMORY : MO_CODE_BYTECODE_TOO_BIG,
                                     fits ? "No memory for bytecode output."
                                          : "Bytecode exceeds the caller's output limit.");
    }
    mo_buffer_free(&encoded);
    uint16_t code = error.kind == MO_BYTECODE_NO_MEMORY ? MO_CODE_CODEGEN_MEMORY
                    : error.kind == MO_BYTECODE_LIMIT   ? MO_CODE_BYTECODE_TOO_BIG
                                                        : MO_CODE_CODEGEN_INVALID;
    return codegen_problem(c, code, error.reason);
}
bool mo_compile_services(const struct mo_core *core, const struct mo_services *services,
                         struct mo_buffer *out, struct mo_diags *diags) {
    MO_ASSERT(core != NULL && out != NULL && out->length == 0 && diags != NULL);
    if (!mo_core_check_services(core, services, diags))
        return false;
    struct compiler c = {.core = core, .services = services, .range_int = MO_NO_INDEX};
    mo_arena_init(&c.arena, MO_LIMIT_CODEGEN_SCRATCH_SIZE);
    mo_types_init(&c.types, &c.arena);
    c.strings.width = sizeof(struct mo_bytecode_string);
    c.constants.width = sizeof(struct mo_bytecode_constant);
    c.modules.width = sizeof(struct mo_bytecode_module);
    c.constructors.width = sizeof(struct mo_codegen_index *);
    c.ownership.width = sizeof(struct ownership);
    c.edges.width = sizeof(struct edge);
    c.constraints.width = sizeof(struct constraint);
    c.shapes.width = sizeof(struct mo_bytecode_service_shape);
    bool ok = codegen_declarations(&c) && codegen_globals(&c) && codegen_bodies(&c) &&
              codegen_entry_valid(&c) && codegen_abilities_valid(&c) && codegen_borrow_infer(&c) &&
              output(&c, out);
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

bool mo_compile(const struct mo_core *core, struct mo_buffer *out, struct mo_diags *diags) {
    return mo_compile_services(core, NULL, out, diags);
}
