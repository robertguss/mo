#include "bytecode/io.h"
#include "bytecode/names.h"

#include <string.h>

#include "heap/types.h"

struct writer {
    const struct mo_bytecode *file;
    struct mo_buffer *out;
    struct mo_arena scratch;
    struct mo_bytecode_names names;
    const char *reason;
    enum mo_bytecode_error_kind kind;
};

static MO_MUST_USE bool word(struct writer *w, uint32_t value) {
    if (w->out->length > MO_LIMIT_BYTECODE_SIZE - 4 || !mo_buffer_append_le32(w->out, value)) {
        w->reason = "bytecode/output limit or no memory";
        w->kind = w->out->length > MO_LIMIT_BYTECODE_SIZE - 4 || w->out->limit - w->out->length < 4
                      ? MO_BYTECODE_LIMIT
                      : MO_BYTECODE_NO_MEMORY;
        return false;
    }
    return true;
}

static MO_MUST_USE bool raw(struct writer *w, const uint8_t *bytes, size_t length) {
    if ((length != 0 && bytes == NULL) || length > MO_LIMIT_BYTECODE_SIZE - w->out->length ||
        !mo_buffer_append(w->out, bytes, length)) {
        w->reason = "invalid bytes, bytecode/output limit or no memory";
        w->kind = length != 0 && bytes == NULL ? MO_BYTECODE_INVALID
                  : length > MO_LIMIT_BYTECODE_SIZE - w->out->length ||
                          length > w->out->limit - w->out->length
                      ? MO_BYTECODE_LIMIT
                      : MO_BYTECODE_NO_MEMORY;
        return false;
    }
    return true;
}

static MO_MUST_USE bool count(struct writer *w, uint32_t n, uint32_t limit, const void *items) {
    if (n > limit || (n != 0 && items == NULL)) {
        w->reason = "invalid array or count past limit";
        w->kind = n > limit ? MO_BYTECODE_LIMIT : MO_BYTECODE_INVALID;
        return false;
    }
    return word(w, n);
}

#define WRITE(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

static MO_MUST_USE bool indices(struct writer *w, const uint32_t *items, uint32_t n,
                                uint32_t limit) {
    WRITE(count(w, n, limit, items));
    for (uint32_t i = 0; i < n; i++) {
        WRITE(word(w, items[i]));
    }
    return true;
}

static MO_MUST_USE bool name(struct writer *w, const char *text, bool optional) {
    if (text == NULL && optional) {
        return word(w, MO_BYTECODE_NO_INDEX);
    }
    if (text == NULL || text[0] == '\0') {
        w->reason = "invalid name";
        return false;
    }
    uint32_t cached = mo_bytecode_name_pointer(&w->names, text);
    if (cached != MO_BYTECODE_NO_INDEX) {
        return word(w, cached);
    }
    size_t length = 0;
    while (length <= MO_LIMIT_STRING_SIZE && text[length] != '\0') {
        length++;
    }
    if (length <= MO_LIMIT_STRING_SIZE) {
        uint32_t index = mo_bytecode_name_find(&w->names, (const uint8_t *)text, length);
        if (index != MO_BYTECODE_NO_INDEX) {
            return word(w, index);
        }
    }
    w->reason = "name missing from strings or past limit";
    w->kind = length > MO_LIMIT_STRING_SIZE ? MO_BYTECODE_LIMIT : MO_BYTECODE_INVALID;
    return false;
}

static MO_MUST_USE bool fields(struct writer *w, const struct mo_type_field *items, uint32_t n,
                               bool optional) {
    WRITE(count(w, n, MO_LIMIT_FIELDS, items));
    for (uint32_t i = 0; i < n; i++) {
        WRITE(name(w, items[i].name, optional && n == 1));
        WRITE(word(w, items[i].type));
    }
    return true;
}

static MO_MUST_USE bool types(struct writer *w) {
    const struct mo_bytecode *f = w->file;
    WRITE(count(w, f->type_count, MO_LIMIT_TYPES, f->types));
    for (uint32_t i = 0; i < f->type_count; i++) {
        const struct mo_type *t = &f->types[i];
        WRITE(word(w, (uint32_t)t->kind));
        switch (t->kind) {
        case MO_TYPE_UNIT:
        case MO_TYPE_BOOL:
        case MO_TYPE_BYTE:
        case MO_TYPE_INT:
        case MO_TYPE_FLOAT:
        case MO_TYPE_CHAR:
        case MO_TYPE_TIME:
        case MO_TYPE_DURATION:
        case MO_TYPE_DECIMAL:
        case MO_TYPE_RANGE:
        case MO_TYPE_TEXT:
        case MO_TYPE_BYTES:
        case MO_TYPE_BIGINT:
            break;
        case MO_TYPE_TUPLE:
        case MO_TYPE_LIST:
        case MO_TYPE_SET:
        case MO_TYPE_MAP:
        case MO_TYPE_FUNCTION:
            WRITE(indices(w, t->parameters, t->parameter_count,
                          t->kind == MO_TYPE_FUNCTION ? MO_LIMIT_BYTECODE_PARAMETERS
                                                      : MO_LIMIT_FIELDS));
            if (t->kind == MO_TYPE_FUNCTION) {
                WRITE(word(w, t->result));
            }
            break;
        case MO_TYPE_RECORD:
            if (!t->defined) {
                w->reason = "incomplete nominal type";
                return false;
            }
            WRITE(name(w, t->name, false));
            WRITE(fields(w, t->fields, t->field_count, false));
            break;
        case MO_TYPE_CHOICE:
            if (!t->defined) {
                w->reason = "incomplete nominal type";
                return false;
            }
            WRITE(name(w, t->name, false));
            WRITE(count(w, t->case_count, MO_LIMIT_CHOICE_CASES, t->cases));
            for (uint32_t j = 0; j < t->case_count; j++) {
                WRITE(name(w, t->cases[j].name, false));
                WRITE(fields(w, t->cases[j].fields, t->cases[j].field_count, true));
            }
            break;
        case MO_TYPE_ENV:
            WRITE(word(w, t->function));
            break;
        case MO_TYPE_CAPABILITY:
            WRITE(word(w, (uint32_t)t->capability));
            break;
        default:
            w->reason = "unknown type kind";
            return false;
        }
    }
    return true;
}

static MO_MUST_USE bool constants(struct writer *w) {
    const struct mo_bytecode *f = w->file;
    WRITE(count(w, f->constant_count, MO_LIMIT_CONSTANTS, f->constants));
    for (uint32_t i = 0; i < f->constant_count; i++) {
        const struct mo_bytecode_constant *k = &f->constants[i];
        if (k->type >= f->type_count) {
            w->reason = "constant type out of range";
            return false;
        }
        WRITE(word(w, k->type));
        enum mo_type_kind kind = f->types[k->type].kind;
        if (kind <= MO_TYPE_DURATION) {
            WRITE(raw(w, k->value.immediate, 8));
        } else if (kind == MO_TYPE_DECIMAL) {
            WRITE(raw(w, k->value.immediate, 16));
        } else if (kind == MO_TYPE_TEXT) {
            WRITE(word(w, k->value.string));
        } else if (kind == MO_TYPE_FUNCTION) {
            WRITE(word(w, k->value.function));
        } else if (kind == MO_TYPE_BYTES) {
            WRITE(count(w, k->value.bytes.length, MO_LIMIT_STRING_SIZE, k->value.bytes.bytes));
            WRITE(raw(w, k->value.bytes.bytes, k->value.bytes.length));
        } else if (kind == MO_TYPE_BIGINT) {
            WRITE(word(w, (uint32_t)k->value.bigint.sign));
            const struct mo_bytecode_bytes *b = &k->value.bigint.magnitude;
            WRITE(count(w, b->length, MO_LIMIT_STRING_SIZE, b->bytes));
            WRITE(raw(w, b->bytes, b->length));
        } else {
            w->reason = "type has no literal encoding";
            return false;
        }
    }
    return true;
}

static MO_MUST_USE bool sections(struct writer *w) {
    const struct mo_bytecode *f = w->file;
    WRITE(count(w, f->string_count, MO_LIMIT_STRINGS, f->strings));
    for (uint32_t i = 0; i < f->string_count; i++) {
        WRITE(count(w, f->strings[i].length, MO_LIMIT_STRING_SIZE, f->strings[i].bytes));
        WRITE(raw(w, f->strings[i].bytes, f->strings[i].length));
    }
    if (!mo_bytecode_names_init(&w->names, &w->scratch, f)) {
        w->reason = "no memory";
        w->kind = MO_BYTECODE_NO_MEMORY;
        return false;
    }
    WRITE(types(w));
    WRITE(count(w, f->layout_count, MO_LIMIT_LAYOUTS, f->layouts));
    for (uint32_t i = 0; i < f->layout_count; i++) {
        const struct mo_layout *l = &f->layouts[i];
        if (l->kind < MO_LAYOUT_RECORD || l->kind > MO_LAYOUT_CLOSURE) {
            w->reason = "unknown layout kind";
            return false;
        }
        WRITE(word(w, (uint32_t)l->kind));
        WRITE(word(w, l->type));
        if (l->kind == MO_LAYOUT_CASE) {
            WRITE(word(w, l->case_index));
        } else if (l->kind == MO_LAYOUT_CLOSURE) {
            WRITE(word(w, l->function));
        }
        WRITE(indices(w, l->fields, l->field_count, MO_LIMIT_FIELDS));
    }
    WRITE(constants(w));
    WRITE(count(w, f->global_count, MO_LIMIT_GLOBALS, f->globals));
    for (uint32_t i = 0; i < f->global_count; i++) {
        WRITE(word(w, f->globals[i].type));
        WRITE(word(w, f->globals[i].name));
        WRITE(word(w, f->globals[i].module));
    }
    WRITE(indices(w, f->init, f->init_count, MO_LIMIT_INITS));
    WRITE(count(w, f->function_count, MO_LIMIT_FUNCTIONS, f->functions));
    for (uint32_t i = 0; i < f->function_count; i++) {
        const struct mo_bytecode_function *fn = &f->functions[i];
        WRITE(word(w, fn->name));
        WRITE(word(w, fn->module));
        WRITE(word(w, (uint32_t)fn->kind));
        WRITE(word(w, fn->spec_code ? 1 : 0));
        WRITE(count(w, fn->parameter_count, MO_LIMIT_BYTECODE_PARAMETERS, fn->parameters));
        for (uint32_t j = 0; j < fn->parameter_count; j++) {
            WRITE(word(w, fn->parameters[j].type));
            WRITE(word(w, (uint32_t)fn->parameters[j].ownership));
        }
        WRITE(word(w, fn->result_type));
        WRITE(indices(w, fn->capture_types, fn->capture_count, MO_LIMIT_FIELDS));
        WRITE(indices(w, fn->register_types, fn->register_count, MO_LIMIT_FUNCTION_REGISTERS));
        WRITE(indices(w, fn->code, fn->code_count, MO_LIMIT_FUNCTION_CODE_WORDS));
    }
    WRITE(count(w, f->module_count, MO_LIMIT_MODULES, f->modules));
    for (uint32_t i = 0; i < f->module_count; i++) {
        const struct mo_bytecode_module *m = &f->modules[i];
        WRITE(word(w, m->name));
        WRITE(count(w, m->type_count, MO_LIMIT_MODULE_NAMES, m->types));
        for (uint32_t j = 0; j < m->type_count; j++) {
            WRITE(word(w, m->types[j].type));
            WRITE(word(w, m->types[j].contents_exposed ? 1 : 0));
        }
        WRITE(indices(w, m->functions, m->function_count, MO_LIMIT_MODULE_NAMES));
        WRITE(indices(w, m->globals, m->global_count, MO_LIMIT_MODULE_NAMES));
    }
    WRITE(count(w, f->file_count, MO_LIMIT_FILES, f->files));
    for (uint32_t i = 0; i < f->file_count; i++) {
        WRITE(count(w, f->files[i].length, MO_LIMIT_STRING_SIZE, f->files[i].bytes));
        WRITE(raw(w, f->files[i].bytes, f->files[i].length));
    }
    WRITE(count(w, f->debug_count, MO_LIMIT_FUNCTIONS, f->debug));
    for (uint32_t i = 0; i < f->debug_count; i++) {
        const struct mo_bytecode_debug *d = &f->debug[i];
        WRITE(count(w, d->span_count, MO_LIMIT_DEBUG_ENTRIES, d->spans));
        for (uint32_t j = 0; j < d->span_count; j++) {
            WRITE(word(w, d->spans[j].code_offset));
            WRITE(word(w, d->spans[j].file));
            WRITE(word(w, d->spans[j].start));
            WRITE(word(w, d->spans[j].end));
        }
        WRITE(count(w, d->name_count, MO_LIMIT_DEBUG_ENTRIES, d->names));
        for (uint32_t j = 0; j < d->name_count; j++) {
            WRITE(word(w, d->names[j].reg));
            WRITE(word(w, d->names[j].name));
            WRITE(word(w, d->names[j].code_start));
            WRITE(word(w, d->names[j].code_end));
        }
        WRITE(word(w, d->text));
        WRITE(word(w, d->file));
        WRITE(word(w, d->start));
        WRITE(word(w, d->end));
        WRITE(word(w, d->subject));
        WRITE(word(w, (uint32_t)d->expectation));
        WRITE(count(w, d->line_count, MO_LIMIT_DEBUG_ENTRIES, d->lines));
        for (uint32_t j = 0; j < d->line_count; j++) {
            WRITE(word(w, d->lines[j].file));
            WRITE(word(w, d->lines[j].start));
            WRITE(word(w, d->lines[j].end));
            WRITE(word(w, d->lines[j].text));
        }
    }
    if (f->service_count != 0) {
        WRITE(count(w, f->service_count, MO_LIMIT_SERVICES, f->services));
        for (uint32_t i = 0; i < f->service_count; i++) {
            const struct mo_bytecode_service *s = &f->services[i];
            WRITE(word(w, s->module));
            WRITE(word(w, s->state_type));
            WRITE(word(w, s->start));
            WRITE(word(w, s->token_env));
            WRITE(count(w, s->tool_count, MO_LIMIT_SERVICE_HANDLERS, s->tools));
            for (uint32_t j = 0; j < s->tool_count; j++) {
                const struct mo_bytecode_service_tool *t = &s->tools[j];
                WRITE(word(w, t->name));
                WRITE(word(w, t->function));
                WRITE(word(w, t->read_only ? 1U : 0U));
                WRITE(indices(w, t->argument_types, t->argument_count,
                              MO_LIMIT_BYTECODE_PARAMETERS - 2));
                WRITE(word(w, t->result_type));
                WRITE(word(w, t->description));
                WRITE(indices(w, t->argument_names, t->argument_name_count,
                              MO_LIMIT_BYTECODE_PARAMETERS - 2));
            }
            WRITE(count(w, s->timer_count, MO_LIMIT_SERVICE_HANDLERS, s->timers));
            for (uint32_t j = 0; j < s->timer_count; j++) {
                WRITE(word(w, s->timers[j].function));
                WRITE(word(w, s->timers[j].interval));
            }
        }
    }
    return true;
}

bool mo_bytecode_encode(const struct mo_bytecode *file, struct mo_buffer *out,
                        struct mo_write_error *error_out) {
    MO_ASSERT(file != NULL && out != NULL && error_out != NULL && out->length == 0);
    struct writer w = {.file = file, .out = out, .reason = "bytecode/output limit or no memory"};
    size_t name_bytes = file->string_count <= MO_LIMIT_STRINGS ? (size_t)file->string_count * 4 : 0;
    mo_arena_init(&w.scratch, mo_bytecode_work_budget(MO_BYTECODE_HEADER_SIZE + name_bytes));
    uint8_t header[MO_BYTECODE_HEADER_SIZE];
    mo_bytecode_header_bytes(header);
    header[8] = file->service_count != 0 ? MO_BYTECODE_SERVICE_FLAG : 0;
    bool encoded = raw(&w, header, sizeof header) && sections(&w);
    mo_arena_free(&w.scratch);
    if (!encoded) {
        mo_buffer_clear(out);
        error_out->reason = w.reason;
        error_out->kind = w.kind;
        return false;
    }
    uint64_t checksum = MO_BYTECODE_FNV_OFFSET;
    for (size_t i = MO_BYTECODE_HEADER_SIZE; i < out->length; i++) {
        checksum = (checksum ^ out->bytes[i]) * MO_BYTECODE_FNV_PRIME;
    }
    for (unsigned i = 0; i < 8; i++) {
        out->bytes[12 + i] = (uint8_t)(checksum >> (i * 8));
    }
    /* Validate the actual encoding, so caches and inactive fields cannot influence acceptance. */
    struct mo_arena scratch;
    struct mo_load_error error;
    mo_arena_init(&scratch, mo_bytecode_work_budget(out->length));
    bool ok = mo_bytecode_decode(&scratch, out->bytes, out->length, &error) != NULL;
    mo_arena_free(&scratch);
    if (!ok) {
        mo_buffer_clear(out);
        error_out->reason = error.reason;
        error_out->kind = error.kind;
    }
    return ok;
}

bool mo_bytecode_write(const struct mo_bytecode *file, struct mo_buffer *out,
                       struct mo_write_error *error_out) {
    if (!mo_bytecode_encode(file, out, error_out)) {
        return false;
    }
    struct mo_load_error error;
    struct mo_image *image = mo_load(out->bytes, out->length, &error);
    if (image == NULL) {
        mo_buffer_clear(out);
        error_out->reason = error.reason;
        error_out->kind = error.kind;
        return false;
    }
    mo_image_free(image);
    return true;
}
