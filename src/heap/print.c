#include "heap/internal.h"

#include <string.h>

static MO_MUST_USE bool scaled_number(struct mo_buffer *out, mo_i128 value, unsigned places) {
    mo_u128 magnitude = value < 0 ? -(mo_u128)value : (mo_u128)value;
    char digits[40];
    unsigned start = sizeof digits;
    do {
        digits[--start] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    unsigned length = (unsigned)sizeof digits - start;
    int point = (int)length - (int)places;
    while (length > 1 && digits[start + length - 1] == '0' && (int)length > point) {
        length--;
    }
    if (value < 0 && !mo_buffer_append_byte(out, '-')) {
        return false;
    }
    if (point <= 0) {
        if (!mo_buffer_append_text(out, "0.")) {
            return false;
        }
        for (int i = 0; i > point; i--) {
            if (!mo_buffer_append_byte(out, '0')) {
                return false;
            }
        }
    }
    for (unsigned i = 0; i < length; i++) {
        if (point > 0 && (int)i == point && !mo_buffer_append_byte(out, '.')) {
            return false;
        }
        if (!mo_buffer_append_byte(out, (uint8_t)digits[start + i])) {
            return false;
        }
    }
    return true;
}

static MO_MUST_USE bool fraction(struct mo_buffer *out, uint64_t value, unsigned places) {
    if (value == 0) {
        return true;
    }
    char digits[18];
    MO_ASSERT(places <= sizeof digits);
    for (unsigned i = places; i != 0; i--) {
        digits[i - 1] = (char)('0' + value % 10);
        value /= 10;
    }
    while (places != 0 && digits[places - 1] == '0') {
        places--;
    }
    return mo_buffer_append_byte(out, '.') && mo_buffer_append(out, digits, places);
}

static MO_MUST_USE bool fixed_digits(struct mo_buffer *out, int64_t value, unsigned width) {
    char digits[4];
    MO_ASSERT(value >= 0 && width <= sizeof digits);
    for (unsigned i = width; i != 0; i--) {
        digits[i - 1] = (char)('0' + value % 10);
        value /= 10;
    }
    MO_ASSERT(value == 0);
    return mo_buffer_append(out, digits, width);
}

static MO_MUST_USE bool time_text(struct mo_buffer *out, int64_t micros) {
    const int64_t day_micros = INT64_C(86400000000);
    int64_t days = micros / day_micros, in_day = micros % day_micros;
    if (in_day < 0) {
        days--;
        in_day += day_micros;
    }
    int64_t z = days + 719468;
    int64_t era = z >= 0 ? z / 146097 : (z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t year = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t day = doy - (153 * mp + 2) / 5 + 1;
    int64_t month = mp < 10 ? mp + 3 : mp - 9;
    year += month <= 2 ? 1 : 0;
    int64_t seconds = in_day / 1000000;
    return fixed_digits(out, year, 4) && mo_buffer_append_byte(out, '-') &&
           fixed_digits(out, month, 2) && mo_buffer_append_byte(out, '-') &&
           fixed_digits(out, day, 2) && mo_buffer_append_byte(out, 'T') &&
           fixed_digits(out, seconds / 3600, 2) && mo_buffer_append_byte(out, ':') &&
           fixed_digits(out, seconds / 60 % 60, 2) && mo_buffer_append_byte(out, ':') &&
           fixed_digits(out, seconds % 60, 2) && fraction(out, (uint64_t)(in_day % 1000000), 6) &&
           mo_buffer_append_byte(out, 'Z');
}

static MO_MUST_USE bool quote(struct mo_buffer *out, const uint8_t *bytes, size_t length,
                              uint8_t delimiter) {
    if (delimiter != 0 && !mo_buffer_append_byte(out, delimiter)) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        uint8_t c = bytes[i];
        bool c1 = c == 0xc2 && i + 1 < length && bytes[i + 1] >= 0x80 && bytes[i + 1] <= 0x9f;
        const char *escape = c == '\n' ? "\\n" : c == '\r' ? "\\r" : c == '\t' ? "\\t" : NULL;
        if (escape != NULL) {
            if (!mo_buffer_append_text(out, escape)) {
                return false;
            }
        } else if (c < 32 || c == 127 || c1) {
            uint32_t code = c1 ? bytes[++i] : c;
            if (!mo_buffer_append_text(out, "\\u{") || !mo_buffer_append_hex(out, code, 1) ||
                !mo_buffer_append_byte(out, '}')) {
                return false;
            }
        } else {
            if ((c == delimiter || c == '\\' ||
                 (c == '#' && i + 1 < length && bytes[i + 1] == '{')) &&
                !mo_buffer_append_byte(out, '\\')) {
                return false;
            }
            if (!mo_buffer_append_byte(out, c)) {
                return false;
            }
        }
    }
    return delimiter == 0 || mo_buffer_append_byte(out, delimiter);
}

static MO_MUST_USE unsigned char_bytes(uint32_t code, uint8_t bytes[4]) {
    if (code < 0x80) {
        bytes[0] = (uint8_t)code;
        return 1;
    }
    unsigned length = code < 0x800 ? 2 : code < 0x10000 ? 3 : 4;
    for (unsigned i = length; i > 1; i--) {
        bytes[i - 1] = (uint8_t)(0x80 | (code & 0x3f));
        code >>= 6;
    }
    bytes[0] = (uint8_t)((length == 2 ? 0xc0 : length == 3 ? 0xe0 : 0xf0) | code);
    return length;
}

static MO_MUST_USE bool scalar_text(struct mo_heap *process, enum mo_type_kind kind,
                                    struct mo_cell value, bool nested, struct mo_buffer *out) {
    switch (kind) {
    case MO_TYPE_UNIT:
        return mo_buffer_append_text(out, "Unit");
    case MO_TYPE_BOOL:
        return mo_buffer_append_text(out, mo_cell_get_bool(value) ? "true" : "false");
    case MO_TYPE_BYTE:
        return mo_buffer_append_uint(out, mo_cell_get_byte(value));
    case MO_TYPE_INT:
        return mo_buffer_append_int(out, mo_cell_get_int(value));
    case MO_TYPE_FLOAT:
        return mo_float_text(mo_cell_get_float(value), out);
    case MO_TYPE_CHAR: {
        uint8_t bytes[4];
        unsigned length = char_bytes(mo_cell_get_char(value), bytes);
        return nested ? quote(out, bytes, length, '\'') : mo_buffer_append(out, bytes, length);
    }
    case MO_TYPE_TIME:
        return time_text(out, mo_cell_get_time(value));
    case MO_TYPE_DURATION: {
        int64_t micros = mo_cell_get_duration(value);
        return (micros == 0 ? mo_buffer_append_byte(out, '0') : scaled_number(out, micros, 6)) &&
               mo_buffer_append_byte(out, 's');
    }
    case MO_TYPE_DECIMAL: {
        mo_i128 number = mo_cell_get_decimal(value);
        return number == 0 ? mo_buffer_append_byte(out, '0') : scaled_number(out, number, 18);
    }
    case MO_TYPE_RANGE: {
        struct mo_range range = mo_cell_get_range(value);
        return mo_buffer_append_int(out, range.start) && mo_buffer_append_text(out, "..") &&
               mo_buffer_append_int(out, range.end);
    }
    case MO_TYPE_TEXT:
    case MO_TYPE_BYTES: {
        size_t length;
        const uint8_t *bytes = mo_value_data(value, &length);
        if (kind == MO_TYPE_TEXT) {
            return nested ? quote(out, bytes, length, '"') : mo_buffer_append(out, bytes, length);
        }
        if (!mo_buffer_append_text(out, "bytes[")) {
            return false;
        }
        for (size_t i = 0; i < length; i++) {
            if ((i != 0 && !mo_buffer_append_text(out, ", ")) ||
                !mo_buffer_append_uint(out, bytes[i])) {
                return false;
            }
        }
        return mo_buffer_append_byte(out, ']');
    }
    case MO_TYPE_BIGINT:
        return mo_bigint_text(process, value, out);
    case MO_TYPE_FUNCTION:
        return mo_buffer_append_text(out, "<function>");
    case MO_TYPE_ENV:
        return mo_buffer_append_text(out, "<environment>");
    case MO_TYPE_CAPABILITY: {
        static const char *const names[] = {"<Args>",      "<Env>",   "<Console>", "<Files>",
                                            "<ReadFiles>", "<Clock>", "<Random>"};
        struct mo_allocation *a = mo_heap_allocation(mo_cell_get_ref(value));
        enum mo_capability capability = process->types->items[a->type].capability;
        MO_ASSERT((unsigned)capability < sizeof names / sizeof names[0]);
        return mo_buffer_append_text(out, names[capability]);
    }
    case MO_TYPE_TUPLE:
    case MO_TYPE_LIST:
    case MO_TYPE_SET:
    case MO_TYPE_MAP:
    case MO_TYPE_RECORD:
    case MO_TYPE_CHOICE:
        MO_UNREACHABLE();
    }
    MO_UNREACHABLE();
}

struct print_frame {
    struct mo_cell value;
    uint32_t type, next, count;
    bool entered, emitted;
};

bool mo_value_print(struct mo_heap *process, uint32_t type, struct mo_cell value,
                    struct mo_buffer *out) {
    MO_ASSERT(process != NULL && type < process->types->count && out != NULL);
    size_t before = out->length, capacity = 16, depth = 1;
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, process);
    struct print_frame local[16];
    struct print_frame *stack = local;
    bool ok = true;
    stack[0] = (struct print_frame){.type = type, .value = value};
    while (ok && depth != 0) {
        struct print_frame *f = &stack[depth - 1];
        const struct mo_type *t = &process->types->items[f->type];
        enum mo_type_kind kind = t->kind;
        bool compound = kind == MO_TYPE_TUPLE || kind == MO_TYPE_RECORD || kind == MO_TYPE_CHOICE ||
                        kind == MO_TYPE_LIST || kind == MO_TYPE_MAP || kind == MO_TYPE_SET;
        if (!f->entered) {
            f->entered = true;
            if (!compound) {
                ok = scalar_text(process, kind, f->value, depth != 1, out);
                depth--;
                continue;
            }
            f->count = mo_value_children(process->types, f->type, f->value);
            const char *name = kind == MO_TYPE_RECORD   ? t->name
                               : kind == MO_TYPE_CHOICE ? t->cases[mo_cell_get_tag(f->value)].name
                                                        : NULL;
            if (name != NULL) {
                size_t length = strlen(name);
<<<<<<< HEAD
                if (length > MO_LIMIT_HEAP_WORK - work) {
                    ok = false;
                    break;
                }
                work += length;
                ok = quote(out, (const uint8_t *)name, length, 0);
=======
                ok = mo_buffer_append(out, name, length);
>>>>>>> 3ab44e9 (Bound primitive results by the runtime heap)
                if (f->count == 0) {
                    depth--;
                    continue;
                }
            }
            ok = ok &&
                 mo_buffer_append_byte(out, kind == MO_TYPE_MAP                           ? '{'
                                            : kind == MO_TYPE_LIST || kind == MO_TYPE_SET ? '['
                                                                                          : '(');
        }
        uint32_t stride = kind == MO_TYPE_MAP ? 2 : 1;
        if (kind == MO_TYPE_MAP || kind == MO_TYPE_SET) {
            struct mo_allocation *a = mo_heap_allocation(mo_cell_get_ref(f->value));
            while (f->next < f->count && mo_table_live(a)[f->next / stride] == 0) {
                f->next += stride;
            }
        }
        if (!ok) {
            break;
        }
        if (f->next == f->count) {
            ok = mo_buffer_append_text(out, kind == MO_TYPE_MAP    ? "}"
                                            : kind == MO_TYPE_SET  ? "].to_set"
                                            : kind == MO_TYPE_LIST ? "]"
                                                                   : ")");
            depth--;
            continue;
        }
        if (f->emitted) {
            ok = mo_buffer_append_text(out, kind == MO_TYPE_MAP && f->next % 2 == 1 ? ": " : ", ");
        }
        const char *name = NULL;
        if (kind == MO_TYPE_RECORD) {
            name = t->fields[f->next].name;
        } else if (kind == MO_TYPE_CHOICE) {
            name = t->cases[mo_cell_get_tag(f->value)].fields[f->next].name;
        }
        if (name != NULL) {
            size_t length = strlen(name);
            ok = ok && quote(out, (const uint8_t *)name, length, 0) &&
                 mo_buffer_append_text(out, ": ");
        }
        uint32_t child_type;
        struct mo_cell item =
            mo_value_child(process->types, f->type, f->value, f->next++, &child_type);
        f->emitted = true;
        if (depth == capacity) {
            if (capacity > SIZE_MAX / (2 * sizeof *stack)) {
                ok = false;
                break;
            }
            struct print_frame *grown = mo_scratch_resize(
                &scratch, stack == local ? NULL : stack,
                stack == local ? 0 : capacity * sizeof *stack, 2 * capacity * sizeof *stack);
            if (grown == NULL) {
                ok = false;
                break;
            }
            if (stack == local) {
                memcpy(grown, local, sizeof local);
            }
            stack = grown;
            capacity *= 2;
        }
        stack[depth++] = (struct print_frame){.type = child_type, .value = item};
    }
    mo_scratch_free(&scratch);
    if (!ok) {
        out->length = before;
    }
    return ok;
}
