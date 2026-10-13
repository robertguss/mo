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
    case MO_TYPE_TEXT:
    case MO_TYPE_BYTES:
    case MO_TYPE_BIGINT: {
        uint8_t bytes[64];
        size_t length = byte(in) % 65;
        for (size_t i = 0; i < length; i++)
            bytes[i] = t->kind == MO_TYPE_TEXT ? byte(in) & 127 : byte(in);
        if (t->kind == MO_TYPE_TEXT)
            return mo_value_text(f->heap, (const char *)bytes, length, out);
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
            if (t->kind == MO_TYPE_LIST)
                ok = mo_value_list(f->heap, type, a, count, out);
            else if (t->kind == MO_TYPE_SET)
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

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2)
        return 0;
    struct input in = {data, size, 0};
    enum mo_primitive_id id = (enum mo_primitive_id)(byte(&in) % (MO_PRIM_RANGE_CONTAINS + 1));
    struct prim_fixture f;
    prim_fixture_init(&f, byte(&in) % 2 == 0 ? 16384U : 65536U);
    size_t baseline = mo_heap_used(f.heap);
    static const char *const ownership[] = {
#define PRIM(id, module, name, signature, params, bugs) params,
#include "base/prims.def"
    };
    const struct mo_type *signature = &f.types.items[f.signature[id]];
    uint32_t result_type =
        signature->kind == MO_TYPE_FUNCTION ? signature->result : f.signature[id];
    size_t count = strlen(ownership[id]);
    MO_ASSERT(count <= 3);
    struct mo_cell args[3], result = mo_cell_int(41), unchanged = result;
    struct mo_buffer before[3];
    size_t built = 0;
    for (; built < count; built++) {
        if (!cell(&f, signature->parameters[built], &in, 0, &args[built]))
            break;
        mo_buffer_init(&before[built], 16384);
        MO_ASSERT(
            mo_value_print(f.heap, signature->parameters[built], args[built], &before[built]));
    }
    enum mo_prim_status status = MO_PRIM_OUT_OF_MEMORY;
    if (built == count) {
        const char *reason;
        status = mo_prim_call(f.heap, &f.types, id, f.signature[id], args, &result, &reason);
        MO_ASSERT(status != MO_PRIM_NOT_IMPLEMENTED);
        if (status == MO_PRIM_RETURNED) {
            MO_ASSERT(reason == NULL);
            struct mo_buffer output;
            mo_buffer_init(&output, 16384);
            /* Rendering may exceed its independent output or scratch ceiling. */
            bool printed = mo_value_print(f.heap, result_type, result, &output);
            (void)printed;
            mo_buffer_free(&output);
            mo_value_drop(f.heap, result_type, result);
        } else
            MO_ASSERT(reason != NULL && memcmp(&result, &unchanged, sizeof result) == 0);
    }
    for (size_t i = 0; i < built; i++) {
        if (status != MO_PRIM_RETURNED || ownership[id][i] == 'b') {
            struct mo_buffer after;
            mo_buffer_init(&after, 16384);
            MO_ASSERT(mo_value_print(f.heap, signature->parameters[i], args[i], &after));
            MO_ASSERT(
                before[i].length == after.length &&
                (after.length == 0 || memcmp(before[i].bytes, after.bytes, after.length) == 0));
            mo_buffer_free(&after);
            mo_value_drop(f.heap, signature->parameters[i], args[i]);
        }
        mo_buffer_free(&before[i]);
    }
    MO_ASSERT(mo_heap_objects(f.heap) == 0 && mo_heap_used(f.heap) == baseline);
    prim_fixture_free(&f);
    return 0;
}
