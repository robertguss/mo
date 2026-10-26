#include "bytecode/names.h"
#include "bytecode/text.h"

#include <string.h>

const char *const mo_bytecode_type_names[] = {
    "unit",    "bool",   "byte",   "int",      "float",  "char",      "time", "duration",
    "decimal", "range",  "text",   "bytes",    "bigint", "tuple",     "list", "set",
    "map",     "record", "choice", "function", "env",    "capability"};
const char *const mo_bytecode_capability_names[] = {"Args",      "Env",   "Console", "Files",
                                                    "ReadFiles", "Clock", "Random"};
const char *const mo_bytecode_function_names[] = {"function", "closure", "init",  "test",
                                                  "example",  "promise", "anchor"};
const char *const mo_bytecode_expectation_names[] = {"none", "allows", "forbids"};
const char *const mo_bytecode_operand_prefixes[] = {"",  "r", "t", "L", "k", "g",
                                                    "f", "p", "h", "@", "",  ""};

struct printer {
    struct mo_buffer *out;
    struct mo_load_error *error;
    struct mo_bytecode_names names;
};

static MO_MUST_USE bool put(struct printer *p, const void *bytes, size_t length) {
    size_t limit = p->out->limit < MO_LIMIT_BYTECODE_TEXT_SIZE
                       ? p->out->limit
                       : (size_t)MO_LIMIT_BYTECODE_TEXT_SIZE;
    if (length > limit - p->out->length) {
        *p->error = (struct mo_load_error){.reason = "bytecode text output limit",
                                           .kind = MO_BYTECODE_LIMIT};
        return false;
    }
    if (!mo_buffer_append(p->out, bytes, length)) {
        *p->error = (struct mo_load_error){.reason = "no memory", .kind = MO_BYTECODE_NO_MEMORY};
        return false;
    }
    return true;
}

static MO_MUST_USE bool text(struct printer *p, const char *s) {
    return put(p, s, strlen(s));
}

static MO_MUST_USE bool number(struct printer *p, const char *prefix, uint32_t value) {
    char digits[10];
    size_t n = 0;
    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    for (size_t i = 0; i < n / 2; i++) {
        char tmp = digits[i];
        digits[i] = digits[n - i - 1];
        digits[n - i - 1] = tmp;
    }
    return text(p, prefix) && put(p, digits, n);
}

static MO_MUST_USE bool optional(struct printer *p, const char *prefix, uint32_t value) {
    return value == MO_BYTECODE_NO_INDEX ? text(p, "-") : number(p, prefix, value);
}

static MO_MUST_USE bool quoted(struct printer *p, const struct mo_bytecode_string *s) {
    static const char hex[] = "0123456789abcdef";
    if (!text(p, "\""))
        return false;
    /* Append runs rather than one byte at a time for large ASCII strings. */
    uint32_t start = 0;
    for (uint32_t i = 0; i < s->length; i++) {
        uint8_t c = s->bytes[i];
        if (c >= 0x20 && c <= 0x7e && c != '"' && c != '\\')
            continue;
        if (!put(p, s->bytes + start, i - start))
            return false;
        if (c == '"' || c == '\\') {
            char escaped[] = {'\\', (char)c};
            if (!put(p, escaped, sizeof escaped))
                return false;
        } else {
            char escaped[] = {'\\', 'x', hex[c >> 4], hex[c & 15]};
            if (!put(p, escaped, sizeof escaped))
                return false;
        }
        start = i + 1;
    }
    return put(p, s->bytes + start, s->length - start) && text(p, "\"");
}

static MO_MUST_USE bool hexadecimal(struct printer *p, const uint8_t *bytes, uint32_t length) {
    static const char hex[] = "0123456789abcdef";
    if (!text(p, "hex\""))
        return false;
    char chunk[512];
    for (uint32_t i = 0; i < length;) {
        uint32_t n = length - i > sizeof chunk / 2 ? sizeof chunk / 2 : length - i;
        for (uint32_t j = 0; j < n; j++) {
            chunk[2 * j] = hex[bytes[i + j] >> 4];
            chunk[2 * j + 1] = hex[bytes[i + j] & 15];
        }
        if (!put(p, chunk, 2 * n))
            return false;
        i += n;
    }
    return text(p, "\"");
}

static MO_MUST_USE bool indices(struct printer *p, const char *prefix, const uint32_t *items,
                                uint32_t count) {
    if (!text(p, "["))
        return false;
    for (uint32_t i = 0; i < count; i++) {
        if ((i != 0 && !text(p, ", ")) || !number(p, prefix, items[i]))
            return false;
    }
    return text(p, "]");
}

static MO_MUST_USE bool name(struct printer *p, const char *s) {
    uint32_t index = s == NULL ? MO_BYTECODE_NO_INDEX : mo_bytecode_name_pointer(&p->names, s);
    MO_ASSERT(s == NULL || index != MO_BYTECODE_NO_INDEX);
    return optional(p, "s", index);
}

static MO_MUST_USE bool fields(struct printer *p, const struct mo_type_field *items,
                               uint32_t count) {
    if (!text(p, "["))
        return false;
    for (uint32_t i = 0; i < count; i++) {
        if ((i != 0 && !text(p, ", ")) || !text(p, "(") || !name(p, items[i].name) ||
            !text(p, ", ") || !number(p, "t", items[i].type) || !text(p, ")"))
            return false;
    }
    return text(p, "]");
}

static MO_MUST_USE bool types(struct printer *p, const struct mo_bytecode *file) {
    if (!text(p, "types\n"))
        return false;
    for (uint32_t i = 0; i < file->type_count; i++) {
        const struct mo_type *t = &file->types[i];
        if (!number(p, "  t", i) || !text(p, " ") || !text(p, mo_bytecode_type_names[t->kind]))
            return false;
        switch (t->kind) {
        case MO_TYPE_TUPLE:
        case MO_TYPE_FUNCTION:
            if (!text(p, " ") || !indices(p, "t", t->parameters, t->parameter_count))
                return false;
            if (t->kind == MO_TYPE_FUNCTION && (!text(p, " ") || !number(p, "t", t->result)))
                return false;
            break;
        case MO_TYPE_LIST:
        case MO_TYPE_SET:
        case MO_TYPE_MAP:
            for (uint32_t j = 0; j < t->parameter_count; j++) {
                if (!text(p, " ") || !number(p, "t", t->parameters[j]))
                    return false;
            }
            break;
        case MO_TYPE_RECORD:
            if (!text(p, " ") || !name(p, t->name) || !text(p, " ") ||
                !fields(p, t->fields, t->field_count))
                return false;
            break;
        case MO_TYPE_CHOICE:
            if (!text(p, " ") || !name(p, t->name) || !text(p, " ["))
                return false;
            for (uint32_t j = 0; j < t->case_count; j++) {
                const struct mo_type_case *c = &t->cases[j];
                if ((j != 0 && !text(p, ", ")) || !text(p, "(") || !name(p, c->name) ||
                    !text(p, ", ") || !fields(p, c->fields, c->field_count) || !text(p, ")"))
                    return false;
            }
            if (!text(p, "]"))
                return false;
            break;
        case MO_TYPE_ENV:
            if (!text(p, " ") || !number(p, "f", t->function))
                return false;
            break;
        case MO_TYPE_CAPABILITY:
            if (!text(p, " ") || !text(p, mo_bytecode_capability_names[t->capability]))
                return false;
            break;
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
        }
        if (!text(p, "\n"))
            return false;
    }
    return text(p, "end\n");
}

static MO_MUST_USE bool layouts(struct printer *p, const struct mo_bytecode *file) {
    static const char *const kinds[] = {"record", "tuple", "case", "closure"};
    if (!text(p, "layouts\n"))
        return false;
    for (uint32_t i = 0; i < file->layout_count; i++) {
        const struct mo_layout *l = &file->layouts[i];
        if (!number(p, "  L", i) || !text(p, " ") || !text(p, kinds[l->kind]) || !text(p, " ") ||
            !number(p, "t", l->type))
            return false;
        if (l->kind == MO_LAYOUT_CASE && (!text(p, " ") || !number(p, "", l->case_index)))
            return false;
        if (l->kind == MO_LAYOUT_CLOSURE && (!text(p, " ") || !number(p, "f", l->function)))
            return false;
        if (!text(p, " ") || !indices(p, "t", l->fields, l->field_count) || !text(p, "\n"))
            return false;
    }
    return text(p, "end\n");
}

static MO_MUST_USE bool scalar(struct printer *p, enum mo_type_kind kind, const uint8_t *bytes) {
    mo_u128 bits = 0;
    uint32_t width = kind == MO_TYPE_DECIMAL ? 16 : 8;
    for (uint32_t i = 0; i < width; i++)
        bits |= (mo_u128)bytes[i] << (8 * i);
    if (kind == MO_TYPE_UNIT)
        return text(p, "()");
    if (kind == MO_TYPE_BOOL)
        return text(p, bits == 0 ? "false" : "true");
    if (kind == MO_TYPE_FLOAT)
        return text(p, "bits ") && hexadecimal(p, bytes, 8);
    if (kind == MO_TYPE_CHAR) {
        char digits[8];
        size_t n = 0;
        static const char hex[] = "0123456789abcdef";
        do {
            digits[n++] = hex[bits & 15];
            bits >>= 4;
        } while (bits != 0);
        if (!text(p, "U+"))
            return false;
        while (n != 0)
            if (!put(p, &digits[--n], 1))
                return false;
        return true;
    }
    bool negative = (bytes[width - 1] & 0x80) != 0;
    if (negative) {
        bits = width == 16 ? -bits : (mo_u128)(-(uint64_t)bits);
        if (!text(p, "-"))
            return false;
    }
    char digits[40];
    size_t n = 0;
    do {
        digits[n++] = (char)('0' + bits % 10);
        bits /= 10;
    } while (bits != 0);
    if (kind == MO_TYPE_DECIMAL) {
        while (n <= 18)
            digits[n++] = '0';
        for (size_t j = n; j > 18;)
            if (!put(p, &digits[--j], 1))
                return false;
        size_t last = 0;
        while (last < 18 && digits[last] == '0')
            last++;
        if (last != 18) {
            if (!text(p, "."))
                return false;
            for (size_t j = 18; j > last;)
                if (!put(p, &digits[--j], 1))
                    return false;
        }
        return true;
    }
    while (n != 0)
        if (!put(p, &digits[--n], 1))
            return false;
    return true;
}

static MO_MUST_USE bool constants(struct printer *p, const struct mo_bytecode *file) {
    static const char *const signs[] = {"zero", "positive", "negative"};
    if (!text(p, "constants\n"))
        return false;
    for (uint32_t i = 0; i < file->constant_count; i++) {
        const struct mo_bytecode_constant *k = &file->constants[i];
        if (!number(p, "  k", i) || !text(p, " ") || !number(p, "t", k->type) || !text(p, " "))
            return false;
        switch (k->encoding) {
        case MO_CONSTANT_IMMEDIATE64:
        case MO_CONSTANT_DECIMAL128:
            if (!scalar(p, file->types[k->type].kind, k->value.immediate))
                return false;
            break;
        case MO_CONSTANT_TEXT:
            if (!text(p, "text ") || !number(p, "s", k->value.string))
                return false;
            break;
        case MO_CONSTANT_BYTES:
            if (!text(p, "bytes ") || !hexadecimal(p, k->value.bytes.bytes, k->value.bytes.length))
                return false;
            break;
        case MO_CONSTANT_BIGINT:
            if (!text(p, "bigint ") || !text(p, signs[k->value.bigint.sign]) || !text(p, " ") ||
                !hexadecimal(p, k->value.bigint.magnitude.bytes, k->value.bigint.magnitude.length))
                return false;
            break;
        case MO_CONSTANT_CLOSURE:
            if (!text(p, "closure ") || !number(p, "f", k->value.function))
                return false;
            break;
        }
        if (!text(p, "\n"))
            return false;
    }
    return text(p, "end\n");
}

static MO_MUST_USE bool functions(struct printer *p, const struct mo_bytecode *file) {
    if (!text(p, "functions\n"))
        return false;
    for (uint32_t i = 0; i < file->function_count; i++) {
        const struct mo_bytecode_function *f = &file->functions[i];
        if (!number(p, "  f", i) || !text(p, " ") || !number(p, "s", f->name) || !text(p, " ") ||
            !number(p, "m", f->module) || !text(p, " ") ||
            !text(p, mo_bytecode_function_names[f->kind]) || !number(p, " spec ", f->spec_code) ||
            !text(p, "\n    params ["))
            return false;
        for (uint32_t j = 0; j < f->parameter_count; j++) {
            if ((j != 0 && !text(p, ", ")) || !number(p, "(t", f->parameters[j].type) ||
                !text(p, f->parameters[j].ownership == MO_PARAMETER_OWNED ? ", owned)"
                                                                          : ", borrowed)"))
                return false;
        }
        if (!text(p, "]\n") || !number(p, "    result t", f->result_type) ||
            !text(p, "\n    captures ") || !indices(p, "t", f->capture_types, f->capture_count) ||
            !text(p, "\n    registers ") ||
            !indices(p, "t", f->register_types, f->register_count) || !text(p, "\n    code\n"))
            return false;
        for (uint32_t pc = 0; pc < f->code_count;) {
            uint32_t head = f->code[pc++];
            const struct mo_opcode_info *op = mo_opcode_find(head & MO_OPCODE_MASK);
            MO_ASSERT(op != NULL);
            uint32_t n = op->fixed_count + (head >> MO_OPCODE_VARIABLE_SHIFT);
            if (!text(p, "      ") || !text(p, op->name))
                return false;
            for (uint32_t j = 0; j < n; j++) {
                enum mo_operand_kind kind =
                    j < op->fixed_count ? op->fixed[j].kind : op->variable.kind;
                if (!text(p, j == 0 ? " " : ", ") ||
                    !number(p, mo_bytecode_operand_prefixes[kind], f->code[pc++]))
                    return false;
            }
            if (!text(p, "\n"))
                return false;
        }
        if (!text(p, "    end\n  end\n"))
            return false;
    }
    return text(p, "end\n");
}

static MO_MUST_USE bool modules(struct printer *p, const struct mo_bytecode *file) {
    if (!text(p, "modules\n"))
        return false;
    for (uint32_t i = 0; i < file->module_count; i++) {
        const struct mo_bytecode_module *m = &file->modules[i];
        if (!number(p, "  m", i) || !text(p, " ") || !number(p, "s", m->name) ||
            !text(p, " types ["))
            return false;
        for (uint32_t j = 0; j < m->type_count; j++) {
            if ((j != 0 && !text(p, ", ")) || !number(p, "(t", m->types[j].type) ||
                !number(p, ", ", m->types[j].contents_exposed) || !text(p, ")"))
                return false;
        }
        if (!text(p, "] functions ") || !indices(p, "f", m->functions, m->function_count) ||
            !text(p, " globals ") || !indices(p, "g", m->globals, m->global_count) ||
            !text(p, "\n"))
            return false;
    }
    return text(p, "end\n");
}

static MO_MUST_USE bool location(struct printer *p, uint32_t file, uint32_t start, uint32_t end) {
    return optional(p, "P", file) && number(p, ", ", start) && number(p, ", ", end);
}

static MO_MUST_USE bool debug(struct printer *p, const struct mo_bytecode *file) {
    if (!text(p, "debug\n"))
        return false;
    for (uint32_t i = 0; i < file->debug_count; i++) {
        const struct mo_bytecode_debug *d = &file->debug[i];
        if (!number(p, "  d", i) || !text(p, " spans ["))
            return false;
        for (uint32_t j = 0; j < d->span_count; j++) {
            const struct mo_bytecode_span *s = &d->spans[j];
            if ((j != 0 && !text(p, ", ")) || !number(p, "(@", s->code_offset) || !text(p, ", ") ||
                !location(p, s->file, s->start, s->end) || !text(p, ")"))
                return false;
        }
        if (!text(p, "] variables ["))
            return false;
        for (uint32_t j = 0; j < d->name_count; j++) {
            const struct mo_bytecode_register_name *n = &d->names[j];
            if ((j != 0 && !text(p, ", ")) || !number(p, "(r", n->reg) ||
                !number(p, ", s", n->name) || !number(p, ", @", n->code_start) ||
                !number(p, ", @", n->code_end) || !text(p, ")"))
                return false;
        }
        if (!text(p, "] text ") || !optional(p, "s", d->text) || !text(p, " declaration (") ||
            !location(p, d->file, d->start, d->end) || !text(p, ") subject ") ||
            !optional(p, "f", d->subject) || !text(p, " expectation ") ||
            !text(p, mo_bytecode_expectation_names[d->expectation]) || !text(p, " lines ["))
            return false;
        for (uint32_t j = 0; j < d->line_count; j++) {
            const struct mo_bytecode_spec_line *l = &d->lines[j];
            if ((j != 0 && !text(p, ", ")) || !text(p, "(") ||
                !location(p, l->file, l->start, l->end) || !number(p, ", s", l->text) ||
                !text(p, ")"))
                return false;
        }
        if (!text(p, "]\n"))
            return false;
    }
    return text(p, "end\n");
}

static MO_MUST_USE bool services(struct printer *p, const struct mo_bytecode *f) {
    if (f->service_count == 0)
        return true;
    if (!text(p, "services\n"))
        return false;
    for (uint32_t i = 0; i < f->service_count; i++) {
        const struct mo_bytecode_service *s = &f->services[i];
        if (!number(p, "  S", i) || !number(p, " m", s->module) ||
            !number(p, " t", s->state_type) || !number(p, " f", s->start) ||
            !number(p, " s", s->token_env) || !text(p, " tools ["))
            return false;
        for (uint32_t j = 0; j < s->tool_count; j++) {
            const struct mo_bytecode_service_tool *t = &s->tools[j];
            if ((j != 0 && !text(p, ", ")) || !number(p, "(s", t->name) ||
                !number(p, ", f", t->function) || !text(p, t->read_only ? ", 1, [" : ", 0, ["))
                return false;
            for (uint32_t k = 0; k < t->argument_count; k++)
                if ((k != 0 && !text(p, ", ")) || !number(p, "t", t->argument_types[k]))
                    return false;
            if (!number(p, "], t", t->result_type) || !number(p, ", s", t->description) ||
                !text(p, ", ["))
                return false;
            for (uint32_t k = 0; k < t->argument_name_count; k++)
                if ((k != 0 && !text(p, ", ")) || !number(p, "s", t->argument_names[k]))
                    return false;
            if (!text(p, "])"))
                return false;
        }
        if (!text(p, "] timers ["))
            return false;
        for (uint32_t j = 0; j < s->timer_count; j++)
            if ((j != 0 && !text(p, ", ")) || !number(p, "(f", s->timers[j].function) ||
                !number(p, ", f", s->timers[j].interval) || !text(p, ")"))
                return false;
        if (!text(p, "]\n"))
            return false;
    }
    return text(p, "end\n");
}

static MO_MUST_USE bool print(struct printer *p, const struct mo_bytecode *f) {
    if (!text(p, "bytecode 0.2 indexed\nstrings\n"))
        return false;
    for (uint32_t i = 0; i < f->string_count; i++) {
        if (!number(p, "  s", i) || !text(p, " ") || !quoted(p, &f->strings[i]) || !text(p, "\n"))
            return false;
    }
    if (!text(p, "end\n") || !types(p, f) || !layouts(p, f) || !constants(p, f) ||
        !text(p, "globals\n"))
        return false;
    for (uint32_t i = 0; i < f->global_count; i++) {
        const struct mo_bytecode_global *g = &f->globals[i];
        if (!number(p, "  g", i) || !number(p, " t", g->type) || !number(p, " s", g->name) ||
            !number(p, " m", g->module) || !text(p, "\n"))
            return false;
    }
    if (!text(p, "end\ninit\n"))
        return false;
    for (uint32_t i = 0; i < f->init_count; i++) {
        if (!number(p, "  i", i) || !number(p, " f", f->init[i]) || !text(p, "\n"))
            return false;
    }
    if (!text(p, "end\n") || !functions(p, f) || !modules(p, f) || !text(p, "files\n"))
        return false;
    for (uint32_t i = 0; i < f->file_count; i++) {
        if (!number(p, "  P", i) || !text(p, " ") ||
            !hexadecimal(p, f->files[i].bytes, f->files[i].length) || !text(p, "\n"))
            return false;
    }
    return text(p, "end\n") && debug(p, f) && services(p, f);
}

bool mo_bytecode_disassemble(const uint8_t *bytes, size_t length, struct mo_buffer *out,
                             struct mo_load_error *error_out) {
    MO_ASSERT(out != NULL && out->length == 0 && error_out != NULL);
    struct mo_image *image = mo_load(bytes, length, error_out);
    if (image == NULL)
        return false;
    struct mo_arena arena;
    mo_arena_init(&arena, mo_bytecode_work_budget(length));
    struct printer p = {.out = out, .error = error_out};
    bool ok = mo_bytecode_names_init(&p.names, &arena, mo_image_bytecode(image));
    if (!ok) {
        *error_out = (struct mo_load_error){.reason = "no memory", .kind = MO_BYTECODE_NO_MEMORY};
    } else {
        ok = print(&p, mo_image_bytecode(image));
    }
    if (!ok)
        mo_buffer_clear(out);
    mo_arena_free(&arena);
    mo_image_free(image);
    return ok;
}
