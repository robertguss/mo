#include "bytecode/io.h"
#include "bytecode/names.h"

#include <string.h>

#include "heap/types.h"

struct reader {
    struct mo_arena *arena;
    struct mo_arena scratch;
    struct mo_bytecode_names names;
    const uint8_t *bytes;
    size_t length;
    size_t offset;
    uint32_t field;
    uint32_t *code_positions;
    struct mo_bytecode_debug_positions *debug_positions;
    struct mo_bytecode *file;
    struct mo_type *types;
    struct mo_load_error *error;
    bool failed;
};

static void refuse(struct reader *r, const char *reason) {
    if (!r->failed) {
        *r->error = (struct mo_load_error){r->field, reason, MO_BYTECODE_INVALID};
        r->failed = true;
    }
}

static MO_MUST_USE uint32_t word(struct reader *r) {
    if (r->failed) {
        return 0;
    }
    r->field = (uint32_t)r->offset;
    if (r->length - r->offset < 4) {
        refuse(r, "truncated word");
        return 0;
    }
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; i++) {
        value |= (uint32_t)r->bytes[r->offset++] << (i * 8);
    }
    return value;
}

static MO_MUST_USE void *allocate(struct reader *r, size_t n, size_t size, size_t alignment) {
    if (r->failed || n == 0) {
        return NULL;
    }
    void *p = mo_arena_allocate_array(r->arena, n, size, alignment);
    if (p == NULL) {
        refuse(r, "no memory");
        r->error->kind = MO_BYTECODE_NO_MEMORY;
        r->error->offset = 0;
    } else {
        memset(p, 0, n * size);
    }
    return p;
}

static MO_MUST_USE void *scratch(struct reader *r, uint32_t n, size_t size, size_t alignment) {
    if (r->failed || n == 0) {
        return NULL;
    }
    void *p = mo_arena_allocate_array(&r->scratch, n, size, alignment);
    if (p == NULL) {
        refuse(r, "no memory");
        r->error->kind = MO_BYTECODE_NO_MEMORY;
        r->error->offset = 0;
    }
    return p;
}

/* The minimum wire size bounds allocation even when the advertised count fits its limit.
 * Each caller uses the smallest possible complete entry, not sizeof a native struct. */
static MO_MUST_USE void *array(struct reader *r, uint32_t *n, uint32_t limit, size_t minimum,
                               size_t size, size_t alignment) {
    *n = word(r);
    if (*n > limit || *n > (r->length - r->offset) / minimum) {
        refuse(r, "count past limit or remaining bytes");
        if (*n > limit) {
            r->error->kind = MO_BYTECODE_LIMIT;
        }
    }
    void *p = allocate(r, *n, size, alignment);
    if (r->failed) {
        *n = 0;
    }
    return p;
}

static MO_MUST_USE const uint32_t *indices(struct reader *r, uint32_t *n, uint32_t limit) {
    uint32_t *p = array(r, n, limit, 4, sizeof *p, _Alignof(uint32_t));
    for (uint32_t i = 0; i < *n; i++) {
        p[i] = word(r);
    }
    return p;
}

static MO_MUST_USE bool utf8(const uint8_t *bytes, uint32_t length) {
    for (uint32_t i = 0; i < length;) {
        uint32_t scalar = bytes[i++];
        unsigned n = 0;
        uint32_t minimum = 0;
        if (scalar < 0x80) {
            continue;
        }
        if (scalar >= 0xc2 && scalar <= 0xdf) {
            n = 1;
            minimum = 0x80;
            scalar &= 0x1f;
        } else if (scalar >= 0xe0 && scalar <= 0xef) {
            n = 2;
            minimum = 0x800;
            scalar &= 0x0f;
        } else if (scalar >= 0xf0 && scalar <= 0xf4) {
            n = 3;
            minimum = 0x10000;
            scalar &= 0x07;
        } else {
            return false;
        }
        if (n > length - i) {
            return false;
        }
        for (unsigned j = 0; j < n; j++) {
            uint32_t b = bytes[i++];
            if ((b & 0xc0) != 0x80) {
                return false;
            }
            scalar = (scalar << 6) | (b & 0x3f);
        }
        if (scalar < minimum || scalar >= 0x110000 || (scalar >= 0xd800 && scalar <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

static MO_MUST_USE const uint8_t *blob(struct reader *r, uint32_t *n, bool terminate) {
    *n = word(r);
    if (*n > MO_LIMIT_STRING_SIZE || *n > r->length - r->offset) {
        refuse(r, "byte length past limit or remaining bytes");
        if (*n > MO_LIMIT_STRING_SIZE) {
            r->error->kind = MO_BYTECODE_LIMIT;
        }
    }
    if (!r->failed && terminate && !utf8(r->bytes + r->offset, *n)) {
        r->field = (uint32_t)r->offset;
        refuse(r, "invalid UTF-8 string");
    }
    uint8_t *p = allocate(r, (size_t)*n + (terminate ? 1U : 0U), 1, 1);
    if (r->failed) {
        *n = 0;
        return NULL;
    }
    if (*n != 0) {
        memcpy(p, r->bytes + r->offset, *n);
    }
    r->offset += *n;
    return p;
}

static MO_MUST_USE bool boolean(struct reader *r) {
    uint32_t value = word(r);
    if (value > 1) {
        refuse(r, "invalid boolean");
    }
    return value == 1;
}

static MO_MUST_USE const char *name(struct reader *r, bool optional) {
    uint32_t index = word(r);
    if (optional && index == MO_BYTECODE_NO_INDEX) {
        return NULL;
    }
    if (index >= r->file->string_count) {
        refuse(r, "name string index out of range");
        return NULL;
    }
    const struct mo_bytecode_string *s = &r->file->strings[index];
    if (r->names.canonical[index] == MO_BYTECODE_NO_INDEX) {
        refuse(r, "name is empty or contains NUL");
        return NULL;
    }
    if (r->names.canonical[index] != index) {
        refuse(r, "name does not use lowest string index");
        return NULL;
    }
    return (const char *)s->bytes;
}

static MO_MUST_USE const struct mo_type_field *fields(struct reader *r, uint32_t *n,
                                                      bool optional) {
    struct mo_type_field *p =
        array(r, n, MO_LIMIT_FIELDS, 8, sizeof *p, _Alignof(struct mo_type_field));
    for (uint32_t i = 0; i < *n && !r->failed; i++) {
        p[i].name = name(r, optional && *n == 1);
        p[i].type = word(r);
    }
    return p;
}

static void types(struct reader *r) {
    struct mo_bytecode *f = r->file;
    struct mo_type *p =
        array(r, &f->type_count, MO_LIMIT_TYPES, 4, sizeof *p, _Alignof(struct mo_type));
    f->types = p;
    r->types = p;
    for (uint32_t i = 0; i < f->type_count && !r->failed; i++) {
        struct mo_type *t = &p[i];
        uint32_t kind = word(r);
        if (kind > MO_TYPE_CAPABILITY) {
            refuse(r, "unknown type kind");
            break;
        }
        t->kind = (enum mo_type_kind)kind;
        t->defined = true;
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
        case MO_TYPE_FUNCTION: {
            uint32_t count_offset = (uint32_t)r->offset;
            t->parameters = indices(r, &t->parameter_count,
                                    t->kind == MO_TYPE_FUNCTION ? MO_LIMIT_BYTECODE_PARAMETERS
                                                                : MO_LIMIT_FIELDS);
            if ((t->kind == MO_TYPE_TUPLE && t->parameter_count < 2) ||
                ((t->kind == MO_TYPE_LIST || t->kind == MO_TYPE_SET) && t->parameter_count != 1) ||
                (t->kind == MO_TYPE_MAP && t->parameter_count != 2)) {
                r->field = count_offset;
                refuse(r, "invalid type parameter count");
            }
            if (t->kind == MO_TYPE_FUNCTION) {
                t->result = word(r);
            }
            break;
        }
        case MO_TYPE_RECORD:
            t->name = name(r, false);
            t->fields = fields(r, &t->field_count, false);
            break;
        case MO_TYPE_CHOICE: {
            t->name = name(r, false);
            struct mo_type_case *cases = array(r, &t->case_count, MO_LIMIT_CHOICE_CASES, 8,
                                               sizeof *cases, _Alignof(struct mo_type_case));
            t->cases = cases;
            if (t->case_count == 0) {
                refuse(r, "choice has no cases");
            }
            for (uint32_t j = 0; j < t->case_count && !r->failed; j++) {
                cases[j].name = name(r, false);
                cases[j].fields = fields(r, &cases[j].field_count, true);
                cases[j].unnamed = cases[j].field_count == 1 && cases[j].fields[0].name == NULL;
                t->has_fields |= cases[j].field_count != 0;
            }
            break;
        }
        case MO_TYPE_ENV:
            t->function = word(r);
            break;
        case MO_TYPE_CAPABILITY: {
            uint32_t cap = word(r);
            if (cap > MO_CAP_RANDOM) {
                refuse(r, "unknown capability");
            }
            t->capability = (enum mo_capability)cap;
            break;
        }
        }
    }
    /* Shapes depend only on field kinds and whether nested choices have fields. No recursive
     * fixed point is needed; validate indices before calling the shared shape function. */
}

static void constants(struct reader *r) {
    struct mo_bytecode *f = r->file;
    struct mo_bytecode_constant *p = array(r, &f->constant_count, MO_LIMIT_CONSTANTS, 8, sizeof *p,
                                           _Alignof(struct mo_bytecode_constant));
    f->constants = p;
    for (uint32_t i = 0; i < f->constant_count && !r->failed; i++) {
        struct mo_bytecode_constant *k = &p[i];
        k->type = word(r);
        if (k->type >= f->type_count) {
            refuse(r, "constant type out of range");
            break;
        }
        enum mo_type_kind kind = f->types[k->type].kind;
        if (kind <= MO_TYPE_DECIMAL) {
            size_t n = kind == MO_TYPE_DECIMAL ? 16 : 8;
            if (n > r->length - r->offset) {
                r->field = (uint32_t)r->offset;
                refuse(r, "truncated literal");
                break;
            }
            k->encoding =
                kind == MO_TYPE_DECIMAL ? MO_CONSTANT_DECIMAL128 : MO_CONSTANT_IMMEDIATE64;
            memcpy(k->value.immediate, r->bytes + r->offset, n);
            r->offset += n;
        } else if (kind == MO_TYPE_TEXT) {
            k->encoding = MO_CONSTANT_TEXT;
            k->value.string = word(r);
        } else if (kind == MO_TYPE_BYTES) {
            k->encoding = MO_CONSTANT_BYTES;
            k->value.bytes.bytes = blob(r, &k->value.bytes.length, false);
        } else if (kind == MO_TYPE_BIGINT) {
            k->encoding = MO_CONSTANT_BIGINT;
            uint32_t sign = word(r);
            if (sign > MO_BIGINT_NEGATIVE) {
                refuse(r, "invalid bigint sign");
            }
            k->value.bigint.sign = (enum mo_bigint_sign)sign;
            k->value.bigint.magnitude.bytes = blob(r, &k->value.bigint.magnitude.length, false);
        } else if (kind == MO_TYPE_FUNCTION) {
            k->encoding = MO_CONSTANT_CLOSURE;
            k->value.function = word(r);
        } else {
            refuse(r, "type has no literal encoding");
        }
    }
}

static void functions(struct reader *r) {
    struct mo_bytecode *f = r->file;
    struct mo_bytecode_function *p = array(r, &f->function_count, MO_LIMIT_FUNCTIONS, 36, sizeof *p,
                                           _Alignof(struct mo_bytecode_function));
    f->functions = p;
    r->code_positions =
        scratch(r, f->function_count, sizeof *r->code_positions, _Alignof(uint32_t));
    for (uint32_t i = 0; i < f->function_count && !r->failed; i++) {
        struct mo_bytecode_function *fn = &p[i];
        fn->name = word(r);
        fn->module = word(r);
        uint32_t kind = word(r);
        if (kind > MO_FUNCTION_ANCHOR) {
            refuse(r, "unknown function kind");
        }
        fn->kind = (enum mo_function_kind)kind;
        fn->spec_code = boolean(r);
        struct mo_parameter *params = array(r, &fn->parameter_count, MO_LIMIT_BYTECODE_PARAMETERS,
                                            8, sizeof *params, _Alignof(struct mo_parameter));
        fn->parameters = params;
        for (uint32_t j = 0; j < fn->parameter_count; j++) {
            params[j].type = word(r);
            uint32_t ownership = word(r);
            if (ownership > MO_PARAMETER_BORROWED) {
                refuse(r, "invalid parameter ownership");
            }
            params[j].ownership = (enum mo_parameter_ownership)ownership;
        }
        fn->result_type = word(r);
        fn->capture_types = indices(r, &fn->capture_count, MO_LIMIT_FIELDS);
        fn->register_types = indices(r, &fn->register_count, MO_LIMIT_FUNCTION_REGISTERS);
        r->code_positions[i] = (uint32_t)r->offset + 4;
        fn->code = indices(r, &fn->code_count, MO_LIMIT_FUNCTION_CODE_WORDS);
    }
}

static void sections(struct reader *r) {
    struct mo_bytecode *f = r->file;
    struct mo_bytecode_string *strings =
        array(r, &f->string_count, MO_LIMIT_STRINGS, 4, sizeof *strings,
              _Alignof(struct mo_bytecode_string));
    f->strings = strings;
    for (uint32_t i = 0; i < f->string_count && !r->failed; i++) {
        strings[i].bytes = blob(r, &strings[i].length, true);
    }
    if (r->failed) {
        return;
    }
    if (!mo_bytecode_names_init(&r->names, &r->scratch, f)) {
        refuse(r, "no memory");
        r->error->kind = MO_BYTECODE_NO_MEMORY;
        r->error->offset = 0;
        return;
    }
    types(r);
    struct mo_layout *layouts = array(r, &f->layout_count, MO_LIMIT_LAYOUTS, 12, sizeof *layouts,
                                      _Alignof(struct mo_layout));
    f->layouts = layouts;
    for (uint32_t i = 0; i < f->layout_count && !r->failed; i++) {
        struct mo_layout *l = &layouts[i];
        uint32_t kind = word(r);
        if (kind > MO_LAYOUT_CLOSURE) {
            refuse(r, "unknown layout kind");
        }
        l->kind = (enum mo_layout_kind)kind;
        l->type = word(r);
        l->case_index = l->kind == MO_LAYOUT_CASE ? word(r) : MO_NO_INDEX;
        l->function = l->kind == MO_LAYOUT_CLOSURE ? word(r) : MO_NO_INDEX;
        l->fields = indices(r, &l->field_count, MO_LIMIT_FIELDS);
    }
    constants(r);
    struct mo_bytecode_global *globals =
        array(r, &f->global_count, MO_LIMIT_GLOBALS, 12, sizeof *globals,
              _Alignof(struct mo_bytecode_global));
    f->globals = globals;
    for (uint32_t i = 0; i < f->global_count && !r->failed; i++) {
        globals[i].type = word(r);
        globals[i].name = word(r);
        globals[i].module = word(r);
    }
    f->init = indices(r, &f->init_count, MO_LIMIT_INITS);
    functions(r);
    struct mo_bytecode_module *modules =
        array(r, &f->module_count, MO_LIMIT_MODULES, 16, sizeof *modules,
              _Alignof(struct mo_bytecode_module));
    f->modules = modules;
    for (uint32_t i = 0; i < f->module_count && !r->failed; i++) {
        struct mo_bytecode_module *m = &modules[i];
        m->name = word(r);
        struct mo_bytecode_type_export *exports =
            array(r, &m->type_count, MO_LIMIT_MODULE_NAMES, 8, sizeof *exports,
                  _Alignof(struct mo_bytecode_type_export));
        m->types = exports;
        for (uint32_t j = 0; j < m->type_count; j++) {
            exports[j].type = word(r);
            exports[j].contents_exposed = boolean(r);
        }
        m->functions = indices(r, &m->function_count, MO_LIMIT_MODULE_NAMES);
        m->globals = indices(r, &m->global_count, MO_LIMIT_MODULE_NAMES);
    }
    struct mo_bytecode_debug *debug = array(r, &f->debug_count, MO_LIMIT_FUNCTIONS, 36,
                                            sizeof *debug, _Alignof(struct mo_bytecode_debug));
    f->debug = debug;
    r->debug_positions = scratch(r, f->debug_count, sizeof *r->debug_positions,
                                 _Alignof(struct mo_bytecode_debug_positions));
    for (uint32_t i = 0; i < f->debug_count && !r->failed; i++) {
        struct mo_bytecode_debug *d = &debug[i];
        r->debug_positions[i].spans = (uint32_t)r->offset;
        struct mo_bytecode_span *spans = array(r, &d->span_count, MO_LIMIT_DEBUG_ENTRIES, 16,
                                               sizeof *spans, _Alignof(struct mo_bytecode_span));
        d->spans = spans;
        for (uint32_t j = 0; j < d->span_count; j++) {
            spans[j].code_offset = word(r);
            spans[j].file = word(r);
            spans[j].start = word(r);
            spans[j].end = word(r);
        }
        r->debug_positions[i].names = (uint32_t)r->offset;
        struct mo_bytecode_register_name *names =
            array(r, &d->name_count, MO_LIMIT_DEBUG_ENTRIES, 16, sizeof *names,
                  _Alignof(struct mo_bytecode_register_name));
        d->names = names;
        for (uint32_t j = 0; j < d->name_count; j++) {
            names[j].reg = word(r);
            names[j].name = word(r);
            names[j].code_start = word(r);
            names[j].code_end = word(r);
        }
        r->debug_positions[i].metadata = (uint32_t)r->offset;
        d->text = word(r);
        d->file = word(r);
        d->start = word(r);
        d->end = word(r);
        d->subject = word(r);
        uint32_t expectation = word(r);
        if (expectation > MO_BYTECODE_FORBIDS) {
            refuse(r, "invalid anchor expectation");
        }
        d->expectation = (enum mo_bytecode_anchor_expectation)expectation;
        r->debug_positions[i].lines = (uint32_t)r->offset;
        struct mo_bytecode_spec_line *lines =
            array(r, &d->line_count, MO_LIMIT_DEBUG_ENTRIES, 16, sizeof *lines,
                  _Alignof(struct mo_bytecode_spec_line));
        d->lines = lines;
        for (uint32_t j = 0; j < d->line_count; j++) {
            lines[j].file = word(r);
            lines[j].start = word(r);
            lines[j].end = word(r);
            lines[j].text = word(r);
        }
    }
}

struct mo_bytecode *mo_bytecode_decode_info(struct mo_arena *arena, const uint8_t *bytes,
                                            size_t length, const uint32_t **code_offsets,
                                            struct mo_types *types_out,
                                            struct mo_load_error *error) {
    MO_ASSERT(arena != NULL && error != NULL && (bytes != NULL || length == 0));
    *error = (struct mo_load_error){.reason = "invalid header or bytecode size",
                                    .kind = length > MO_LIMIT_BYTECODE_SIZE ? MO_BYTECODE_LIMIT
                                                                            : MO_BYTECODE_INVALID};
    if (length > MO_LIMIT_BYTECODE_SIZE || length < MO_BYTECODE_HEADER_SIZE ||
        memcmp(bytes, MO_BYTECODE_MAGIC, 4) != 0) {
        return NULL;
    }
    uint16_t major = (uint16_t)((uint16_t)bytes[4] | (uint16_t)((uint16_t)bytes[5] << 8));
    uint16_t minor = (uint16_t)((uint16_t)bytes[6] | (uint16_t)((uint16_t)bytes[7] << 8));
    if (major != MO_BYTECODE_MAJOR || minor != MO_BYTECODE_MINOR) {
        *error = (struct mo_load_error){.offset = major != MO_BYTECODE_MAJOR ? 4U : 6U,
                                        .reason = "unsupported version or flags"};
        return NULL;
    }
    if (bytes[8] != 0 || bytes[9] != 0 || bytes[10] != 0 || bytes[11] != 0) {
        *error = (struct mo_load_error){.offset = 8, .reason = "unsupported version or flags"};
        return NULL;
    }
    uint64_t expected = 0;
    for (unsigned i = 0; i < 8; i++) {
        expected |= (uint64_t)bytes[12 + i] << (i * 8);
    }
    uint64_t checksum = MO_BYTECODE_FNV_OFFSET;
    for (size_t i = MO_BYTECODE_HEADER_SIZE; i < length; i++) {
        checksum = (checksum ^ bytes[i]) * MO_BYTECODE_FNV_PRIME;
    }
    if (checksum != expected) {
        *error = (struct mo_load_error){.offset = 12, .reason = "checksum mismatch"};
        return NULL;
    }
    struct reader r = {.arena = arena,
                       .bytes = bytes,
                       .length = length,
                       .offset = MO_BYTECODE_HEADER_SIZE,
                       .error = error};
    r.file = allocate(&r, 1, sizeof *r.file, _Alignof(struct mo_bytecode));
    if (r.failed) {
        return NULL;
    }
    memcpy(r.file->header.magic, bytes, 4);
    r.file->header.major = major;
    r.file->header.minor = minor;
    r.file->header.checksum = checksum;
    mo_arena_init(&r.scratch, mo_bytecode_work_budget(length));
    sections(&r);
    if (r.failed) {
        mo_arena_free(&r.scratch);
        return NULL;
    }
    if (r.offset != length) {
        r.field = (uint32_t)r.offset;
        refuse(&r, "trailing bytes");
        mo_arena_free(&r.scratch);
        return NULL;
    }
    struct mo_bytecode_read_info info = {
        .types = {.items = r.types, .count = r.file->type_count, .finished = true},
        .names = &r.names,
        .code = r.code_positions,
        .debug = r.debug_positions,
        .wire_size = length};
    bool valid = mo_bytecode_validate(r.file, &info, error);
    if (valid && code_offsets != NULL) {
        *code_offsets =
            mo_arena_copy(arena, r.code_positions,
                          (size_t)r.file->function_count * sizeof(uint32_t), _Alignof(uint32_t));
        if (*code_offsets == NULL) {
            *error = (struct mo_load_error){.reason = "no memory", .kind = MO_BYTECODE_NO_MEMORY};
            valid = false;
        }
    }
    mo_arena_free(&r.scratch);
    if (!valid) {
        return NULL;
    }
    struct mo_types view = {.items = r.types, .count = r.file->type_count, .finished = true};
    for (uint32_t i = 0; i < view.count; i++) {
        struct mo_type *t = &view.items[i];
        t->reference = t->kind >= MO_TYPE_TEXT;
        if (t->kind == MO_TYPE_CHOICE) {
            t->reference = false;
            for (uint32_t j = 0; j < t->case_count; j++) {
                enum mo_case_shape shape = mo_case_shape(&view, i, j);
                t->reference |= shape == MO_CASE_REF || shape == MO_CASE_BOXED;
            }
        }
    }
    if (types_out != NULL) {
        *types_out = view;
    }
    return r.file;
}

struct mo_bytecode *mo_bytecode_decode(struct mo_arena *arena, const uint8_t *bytes, size_t length,
                                       struct mo_load_error *error) {
    return mo_bytecode_decode_info(arena, bytes, length, NULL, NULL, error);
}
