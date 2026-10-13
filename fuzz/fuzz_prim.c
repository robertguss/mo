#include "../tests/prim_fixture.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

struct input {
    const uint8_t *bytes;
    size_t length, at;
};

static MO_MUST_USE uint8_t byte(struct input *in) {
    return in->at < in->length ? in->bytes[in->at++] : 0;
}

static MO_MUST_USE uint64_t word(struct input *in) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++)
        value |= (uint64_t)byte(in) << (8 * i);
    return value;
}

static void cluster_agreement(struct mo_cell text, struct mo_cell clusters) {
    size_t length, at = 0;
    const uint8_t *bytes = mo_value_data(text, &length);
    for (uint32_t i = 0; i < mo_list_size(clusters); i++) {
        struct mo_cell item;
        MO_ASSERT(mo_list_get(clusters, i, &item));
        size_t next, item_length;
        MO_ASSERT(mo_unicode16_next_grapheme(bytes + at, length - at, &next));
        const uint8_t *item_bytes = mo_value_data(item, &item_length);
        MO_ASSERT(next == item_length && memcmp(bytes + at, item_bytes, next) == 0);
        at += next;
    }
    MO_ASSERT(at == length);
}

static MO_MUST_USE bool cell(struct prim_fixture *f, uint32_t type, struct input *in,
                             unsigned depth, struct mo_cell *out) {
    MO_ASSERT(depth < 4);
    const struct mo_type *t = &f->types.items[type];
    switch ((unsigned)t->kind) {
    case MO_TYPE_UNIT:
        *out = mo_cell_unit();
        return true;
    case MO_TYPE_BOOL:
        *out = mo_cell_bool(byte(in) % 2 != 0);
        return true;
    case MO_TYPE_BYTE:
        *out = mo_cell_byte(byte(in));
        return true;
    case MO_TYPE_CHAR: {
        uint32_t n = (uint32_t)(word(in) % 0x110000);
        *out = mo_cell_char(n >= 0xd800 && n <= 0xdfff ? 0 : n);
        return true;
    }
    case MO_TYPE_INT: {
        uint64_t raw = word(in);
        int64_t n;
        memcpy(&n, &raw, sizeof n);
        *out = mo_cell_int(n);
        return true;
    }
    case MO_TYPE_TIME:
        *out = mo_cell_time((int64_t)(word(in) % UINT64_C(315537897600000000)) -
                            INT64_C(62135596800000000));
        return true;
    case MO_TYPE_DURATION: {
        uint64_t raw = word(in);
        int64_t n;
        memcpy(&n, &raw, sizeof n);
        *out = mo_cell_duration(n);
        return true;
    }
    case MO_TYPE_FLOAT: {
        uint64_t raw = word(in);
        double n;
        memcpy(&n, &raw, sizeof n);
        *out = mo_cell_float(n);
        return true;
    }
    case MO_TYPE_DECIMAL: {
        mo_u128 n = word(in);
        n |= (mo_u128)(word(in) >> 1) << 64;
        *out = mo_cell_decimal(byte(in) % 2 == 0 ? (mo_i128)n : -(mo_i128)n);
        return true;
    }
    case MO_TYPE_RANGE: {
        uint64_t a = word(in), b = word(in);
        int64_t start, end;
        memcpy(&start, &a, sizeof start);
        memcpy(&end, &b, sizeof end);
        *out = mo_cell_range(start, end);
        return true;
    }
    case MO_TYPE_TEXT: {
        static const uint32_t samples[] = {
            0,       'A',     'a',    '\'',   0x3a3,  0x345,  0x130,  0xdf,    0x301,
            0x315,   0x915,   0x94d,  0x924,  0x200d, 0xfe0e, 0xfe0f, 0x1f469, 0x1f1e6,
            0x1f1fa, 0x1f3fb, 0x2764, 0x20e3, 0x1100, 0x1161, 0x11a8, 0x16d63, 0x16d67,
            '\t',    '\n',    '\r',   0x1f,   0x7f,   0x80,   0x9f};
        uint8_t bytes[1024];
        size_t count = byte(in), length = 0;
        bool policy_only = true;
        uint64_t policy_width = 0;
        for (size_t i = 0; i < count; i++) {
            uint64_t raw = word(in);
            uint32_t cp = (raw & 1) != 0
                              ? samples[(raw >> 1) % (sizeof samples / sizeof samples[0])]
                              : (uint32_t)((raw >> 1) % 0x110000);
            if (cp >= 0xd800 && cp <= 0xdfff)
                cp = 0;
            /* On texts of controls and regional indicators, D238's width is
             * exactly the number of indicators, independent of pairing. */
            if (cp >= 0x1f1e6 && cp <= 0x1f1ff)
                policy_width++;
            else if (!(cp < 0x20 || (cp >= 0x7f && cp <= 0x9f)))
                policy_only = false;
            unsigned n = cp < 0x80 ? 1U : cp < 0x800 ? 2U : cp < 0x10000 ? 3U : 4U;
            for (unsigned j = n; j > 1; j--) {
                bytes[length + j - 1] = (uint8_t)(0x80 | (cp & 63));
                cp >>= 6;
            }
            bytes[length] = (uint8_t)(cp | (n == 1 ? 0 : n == 2 ? 0xc0 : n == 3 ? 0xe0 : 0xf0));
            length += n;
        }
        uint64_t width;
        MO_ASSERT(mo_unicode16_display_width(bytes, length, &width));
        MO_ASSERT(!policy_only || width == policy_width);
        bool slack = byte(in) % 2 != 0 && length >= 32;
        if (!mo_value_text(f->heap, (const char *)bytes, length, out))
            return false;
        /* Self-concat reserves four times the original bytes; retaining one
         * quarter avoids contraction and exercises subsequent writes to slack. */
        if (slack &&
            (!mo_text_concat(f->heap, out, *out) || !mo_text_slice(f->heap, out, 0, length))) {
            mo_value_drop(f->heap, type, *out);
            return false;
        }
        struct mo_cell result;
        const char *reason;
        MO_ASSERT(mo_prim_call(f->heap, &f->types, MO_PRIM_UNICODE16_DISPLAY_WIDTH,
                               f->signature[MO_PRIM_UNICODE16_DISPLAY_WIDTH], out, &result,
                               &reason) == MO_PRIM_RETURNED);
        MO_ASSERT(reason == NULL && (uint64_t)mo_cell_get_int(result) == width);
        enum mo_primitive_id id = MO_PRIM_UNICODE16_GRAPHEMES;
        enum mo_prim_status status =
            mo_prim_call(f->heap, &f->types, id, f->signature[id], out, &result, &reason);
        if (status == MO_PRIM_RETURNED) {
            cluster_agreement(*out, result);
            mo_value_drop(f->heap, f->types.items[f->signature[id]].result, result);
        } else
            MO_ASSERT(status == MO_PRIM_OUT_OF_MEMORY);
        return true;
    }
    case MO_TYPE_BYTES:
    case MO_TYPE_BIGINT: {
        uint8_t bytes[1024];
        size_t length = word(in) % 1025;
        for (size_t i = 0; i < length; i++)
            bytes[i] = byte(in);
        if (t->kind == MO_TYPE_BYTES)
            return mo_value_bytes(f->heap, bytes, length, out);
        while (length != 0 && bytes[length - 1] == 0)
            length--;
        enum mo_bigint_sign sign = length == 0         ? MO_BIGINT_ZERO
                                   : byte(in) % 2 == 0 ? MO_BIGINT_POSITIVE
                                                       : MO_BIGINT_NEGATIVE;
        return mo_value_bigint(f->heap, sign, bytes, length, out);
    }
    case MO_TYPE_LIST:
    case MO_TYPE_SET:
    case MO_TYPE_MAP:
    case MO_TYPE_TUPLE: {
        struct mo_cell a[8], b[8];
        uint32_t count = t->kind == MO_TYPE_TUPLE ? t->parameter_count : byte(in) % 9;
        MO_ASSERT(count <= 8);
        uint32_t built = 0, values = 0;
        bool ok = true;
        for (uint32_t i = 0; i < count && ok; i++) {
            ok = cell(f, t->parameters[t->kind == MO_TYPE_TUPLE ? i : 0], in, depth + 1, &a[i]);
            built += ok ? 1U : 0U;
            if (ok && t->kind == MO_TYPE_MAP) {
                ok = cell(f, t->parameters[1], in, depth + 1, &b[i]);
                values += ok ? 1U : 0U;
            }
        }
        if (ok) {
            if (t->kind == MO_TYPE_LIST) {
                ok = mo_value_list(f->heap, type, a, count, out);
                unsigned shape = byte(in) % 5;
                uint32_t first = 0, kept = count;
                if (ok && count != 0 && shape != 0) {
                    if (shape == 4 && count > 2) {
                        /* An exact buffer crosses its half-capacity prefix. */
                        first = count / 2 + 1;
                        kept = count - first;
                        ok = mo_list_slice(f->heap, type, out, first, kept);
                    } else if (shape != 4) {
                        /* Slack, contraction, or a nonzero live window. */
                        for (unsigned i = 0; i < (shape == 2 ? 3U : 1U) && ok; i++)
                            ok = mo_list_concat(f->heap, type, out, *out);
                        if (ok)
                            ok = mo_list_slice(f->heap, type, out, shape == 3 ? count : 0, count);
                    }
                    if (!ok)
                        mo_value_drop(f->heap, type, *out);
                }
                if (ok) {
                    MO_ASSERT(mo_list_size(*out) == kept);
                    for (uint32_t i = 0; i < kept; i++) {
                        struct mo_cell got;
                        MO_ASSERT(mo_list_get(*out, i, &got) &&
                                  memcmp(got.bytes, a[first + i].bytes, sizeof got.bytes) == 0);
                    }
                }
            } else if (t->kind == MO_TYPE_SET)
                ok = mo_value_set(f->heap, type, a, count, out);
            else if (t->kind == MO_TYPE_MAP)
                ok = mo_value_map(f->heap, type, a, b, count, out);
            else {
                uint32_t layout = 0;
                while (layout < f->types.layout_count && f->types.layouts[layout].type != type)
                    layout++;
                MO_ASSERT(layout < f->types.layout_count);
                ok = mo_value_construct(f->heap, layout, a, count, out);
            }
        }
        for (uint32_t i = 0; i < built; i++)
            mo_value_drop(f->heap, t->parameters[t->kind == MO_TYPE_TUPLE ? i : 0], a[i]);
        for (uint32_t i = 0; i < values; i++)
            mo_value_drop(f->heap, t->parameters[1], b[i]);
        return ok;
    }
    default:
        MO_UNREACHABLE();
    }
}

static void unicode_laws(struct prim_fixture *f, enum mo_primitive_id id,
                         const struct mo_cell *args, struct mo_cell result) {
    if (id == MO_PRIM_UNICODE16_GRAPHEMES) {
        cluster_agreement(args[0], result);
        size_t length, at = 0;
        const uint8_t *original = mo_value_data(args[0], &length);
        for (uint32_t i = 0; i < mo_list_size(result); i++) {
            struct mo_cell item, again;
            MO_ASSERT(mo_list_get(result, i, &item));
            size_t n;
            const uint8_t *bytes = mo_value_data(item, &n);
            MO_ASSERT(n != 0 && n <= length - at && memcmp(bytes, original + at, n) == 0);
            at += n;
            const char *reason;
            enum mo_prim_status status =
                mo_prim_call(f->heap, &f->types, id, f->signature[id], &item, &again, &reason);
            if (status == MO_PRIM_RETURNED) {
                MO_ASSERT(mo_list_size(again) == 1);
                mo_value_drop(f->heap, f->types.items[f->signature[id]].result, again);
            } else
                MO_ASSERT(status == MO_PRIM_OUT_OF_MEMORY);
        }
        MO_ASSERT(at == length);
    } else if (id == MO_PRIM_UNICODE16_NORMALIZE_NFC) {
        uint32_t text = f->builtin[MO_TYPE_TEXT];
        struct mo_cell duplicate = mo_value_dup(f->heap, text, result), again;
        const char *reason;
        enum mo_prim_status status =
            mo_prim_call(f->heap, &f->types, id, f->signature[id], &duplicate, &again, &reason);
        if (status == MO_PRIM_RETURNED) {
            size_t a, b;
            const uint8_t *left = mo_value_data(result, &a), *right = mo_value_data(again, &b);
            MO_ASSERT(a == b && (a == 0 || memcmp(left, right, a) == 0));
            mo_value_drop(f->heap, text, again);
        } else {
            MO_ASSERT(status == MO_PRIM_OUT_OF_MEMORY);
            mo_value_drop(f->heap, text, duplicate);
        }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    uint64_t width = UINT64_MAX;
    if (!mo_unicode16_display_width(data, size, &width))
        MO_ASSERT(width == UINT64_MAX);
    if (size < 4)
        return 0;
    struct input in = {data, size, 0};
    enum mo_primitive_id id = (enum mo_primitive_id)(byte(&in) % MO_PRIM_COUNT);
    struct prim_fixture f;
    size_t budget = byte(&in) % 2 == 0 ? 65536U : 262144U;
    enum mo_type_kind element = byte(&in) % 2 == 0 ? MO_TYPE_INT : MO_TYPE_TEXT;
    if (id == MO_PRIM_LIST_SUM)
        element = MO_TYPE_INT;
    unsigned holders = byte(&in) % 3;
    if (element == MO_TYPE_INT)
        prim_fixture_init(&f, budget);
    else
        prim_fixture_init_type(&f, budget, element);
    size_t baseline = mo_heap_used(f.heap);
    static const char *const ownership[] = {
#define PRIM(id, module, name, signature, params, bugs) params,
#include "base/prims.def"
    };
    const struct mo_type *signature = &f.types.items[f.signature[id]];
    uint32_t result_type = signature->result;
    size_t count = strlen(ownership[id]);
    MO_ASSERT(count <= 3);
    struct mo_cell args[3], held[3], result = mo_cell_int(41), unchanged = result;
    struct mo_buffer before[3];
    size_t arguments_at = in.at;
    size_t built = 0;
    for (; built < count; built++) {
        if (!cell(&f, signature->parameters[built], &in, 0, &args[built]))
            break;
        if (holders == 1)
            held[built] = mo_value_dup(f.heap, signature->parameters[built], args[built]);
        else if (holders == 2)
            MO_ASSERT(mo_value_make_immortal(f.heap, signature->parameters[built], args[built]));
        mo_buffer_init(&before[built], 262144);
        MO_ASSERT(
            mo_value_print(f.heap, signature->parameters[built], args[built], &before[built]));
    }
    enum mo_prim_status status = MO_PRIM_OUT_OF_MEMORY;
    if (built == count) {
        const char *reason;
        status = mo_prim_call(f.heap, &f.types, id, f.signature[id], args, &result, &reason);
        if (status == MO_PRIM_RETURNED) {
            MO_ASSERT(reason == NULL);
            unicode_laws(&f, id, args, result);
            struct mo_buffer output;
            mo_buffer_init(&output, 262144);
            /* Rendering may exceed its independent output or scratch ceiling. */
            bool printed = mo_value_print(f.heap, result_type, result, &output);
            /* Compare an independent shared call with the unique/immortal
             * path. This catches wrong in-place results, not just crashes. */
            if (printed) {
                struct prim_fixture reference;
                prim_fixture_init_type(&reference, 4 << 20, element);
                struct input replay = {data, size, arguments_at};
                struct mo_cell operands[3], retained[3], expected;
                for (size_t i = 0; i < count; i++) {
                    MO_ASSERT(cell(&reference, signature->parameters[i], &replay, 0, &operands[i]));
                    retained[i] =
                        mo_value_dup(reference.heap, signature->parameters[i], operands[i]);
                }
                enum mo_prim_status other =
                    mo_prim_call(reference.heap, &reference.types, id, reference.signature[id],
                                 operands, &expected, &reason);
                MO_ASSERT(other == MO_PRIM_RETURNED);
                struct mo_buffer text;
                mo_buffer_init(&text, 262144);
                bool rendered = mo_value_print(reference.heap, result_type, expected, &text);
                MO_ASSERT(rendered && output.length == text.length &&
                          (text.length == 0 || memcmp(output.bytes, text.bytes, text.length) == 0));
                mo_buffer_free(&text);
                mo_value_drop(reference.heap, result_type, expected);
                for (size_t i = 0; i < count; i++) {
                    if (ownership[id][i] == 'b')
                        mo_value_drop(reference.heap, signature->parameters[i], operands[i]);
                    mo_value_drop(reference.heap, signature->parameters[i], retained[i]);
                }
                MO_ASSERT(mo_heap_objects(reference.heap) == 0);
                prim_fixture_free(&reference);
            }
            mo_buffer_free(&output);
            mo_value_drop(f.heap, result_type, result);
        } else
            MO_ASSERT(reason != NULL && memcmp(&result, &unchanged, sizeof result) == 0);
    }
    for (size_t i = 0; i < built; i++) {
        bool preserved = status != MO_PRIM_RETURNED || ownership[id][i] == 'b';
        if (preserved || holders != 0) {
            struct mo_cell original = holders == 1 ? held[i] : args[i];
            struct mo_buffer after;
            mo_buffer_init(&after, 262144);
            MO_ASSERT(mo_value_print(f.heap, signature->parameters[i], original, &after));
            MO_ASSERT(
                before[i].length == after.length &&
                (after.length == 0 || memcmp(before[i].bytes, after.bytes, after.length) == 0));
            mo_buffer_free(&after);
        }
        if (preserved)
            mo_value_drop(f.heap, signature->parameters[i], args[i]);
        if (holders == 1)
            mo_value_drop(f.heap, signature->parameters[i], held[i]);
        mo_buffer_free(&before[i]);
    }
    if (holders != 2)
        MO_ASSERT(mo_heap_objects(f.heap) == 0 && mo_heap_used(f.heap) == baseline);
    prim_fixture_free(&f);
    return 0;
}
