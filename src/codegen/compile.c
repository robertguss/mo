#include "codegen/codegen.h"

#include <stdlib.h>
#include <string.h>

#include "base/codes.h"
#include "bytecode/bytecode.h"
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
