#include "tools/protocol.h"

static const enum mo_type_kind kinds[] = {
    MO_TYPE_UNIT,  MO_TYPE_BOOL,   MO_TYPE_BYTE,     MO_TYPE_INT,     MO_TYPE_FLOAT,
    MO_TYPE_CHAR,  MO_TYPE_TIME,   MO_TYPE_DURATION, MO_TYPE_DECIMAL, MO_TYPE_TEXT,
    MO_TYPE_BYTES, MO_TYPE_BIGINT, MO_TYPE_RANGE,
};

static uint32_t scalar_tag(enum mo_type_kind kind) {
    for (uint32_t i = 0; i < sizeof kinds / sizeof *kinds; i++)
        if (kinds[i] == kind)
            return i;
    return MO_NO_INDEX;
}

static enum mo_protocol_value_status copy_leaf(struct mo_heap *to, enum mo_type_kind kind,
                                               struct mo_cell value, size_t limit,
                                               struct mo_cell *out) {
    switch (kind) {
    case MO_TYPE_UNIT:
        *out = mo_cell_unit();
        break;
    case MO_TYPE_BOOL:
        *out = mo_cell_bool(mo_cell_get_bool(value));
        break;
    case MO_TYPE_BYTE:
        *out = mo_cell_byte(mo_cell_get_byte(value));
        break;
    case MO_TYPE_INT:
        *out = mo_cell_int(mo_cell_get_int(value));
        break;
    case MO_TYPE_FLOAT:
        *out = mo_cell_float(mo_cell_get_float(value));
        break;
    case MO_TYPE_CHAR:
        *out = mo_cell_char(mo_cell_get_char(value));
        break;
    case MO_TYPE_TIME:
        *out = mo_cell_time(mo_cell_get_time(value));
        break;
    case MO_TYPE_DECIMAL:
        *out = mo_cell_decimal(mo_cell_get_decimal(value));
        break;
    case MO_TYPE_TEXT:
    case MO_TYPE_BIGINT: {
        size_t size;
        const uint8_t *bytes = mo_value_data(value, &size);
        if (size > limit)
            return MO_PROTOCOL_VALUE_LIMIT;
        bool ok = kind == MO_TYPE_TEXT
                      ? mo_value_text(to, (const char *)bytes, size, out)
                      : mo_value_bigint(to, mo_value_bigint_sign(value), bytes, size, out);
        if (!ok)
            return MO_PROTOCOL_VALUE_MEMORY;
        break;
    }
    case MO_TYPE_DURATION:
    case MO_TYPE_RANGE:
    case MO_TYPE_BYTES:
    case MO_TYPE_TUPLE:
    case MO_TYPE_LIST:
    case MO_TYPE_SET:
    case MO_TYPE_MAP:
    case MO_TYPE_RECORD:
    case MO_TYPE_CHOICE:
    case MO_TYPE_FUNCTION:
    case MO_TYPE_ENV:
    case MO_TYPE_CAPABILITY:
        return MO_PROTOCOL_VALUE_INVALID;
    }
    return MO_PROTOCOL_VALUE_OK;
}

static enum mo_protocol_value_status pack_bytes(struct mo_heap *to, uint32_t list_type,
                                                struct mo_cell value, size_t limit,
                                                struct mo_cell *out) {
    size_t size;
    const uint8_t *bytes = mo_value_data(value, &size);
    if (size > limit)
        return MO_PROTOCOL_VALUE_LIMIT;
    struct mo_cell list;
    if (!mo_value_list(to, list_type, NULL, 0, &list))
        return MO_PROTOCOL_VALUE_MEMORY;
    for (size_t i = 0; i < size; i++) {
        if (!mo_list_append(to, list_type, &list, mo_cell_byte(bytes[i]))) {
            mo_value_drop(to, list_type, list);
            return MO_PROTOCOL_VALUE_MEMORY;
        }
    }
    *out = list;
    return MO_PROTOCOL_VALUE_OK;
}

enum mo_protocol_value_status mo_protocol_scalar_pack(const struct mo_types *protocol,
                                                      uint32_t scalar, struct mo_heap *to,
                                                      enum mo_type_kind kind, struct mo_cell value,
                                                      size_t byte_limit, struct mo_cell *out) {
    uint32_t tag = scalar_tag(kind);
    if (tag == MO_NO_INDEX)
        return MO_PROTOCOL_VALUE_INVALID;
    const struct mo_type_case *shape = &protocol->items[scalar].cases[tag];
    struct mo_cell fields[2];
    enum mo_protocol_value_status status;
    if (kind == MO_TYPE_RANGE) {
        struct mo_range range = mo_cell_get_range(value);
        fields[0] = mo_cell_int(range.start);
        fields[1] = mo_cell_int(range.end);
    } else if (kind == MO_TYPE_DURATION)
        fields[0] = mo_cell_int(mo_cell_get_duration(value));
    else if (kind == MO_TYPE_BYTES) {
        status = pack_bytes(to, shape->fields[0].type, value, byte_limit, &fields[0]);
        if (status != MO_PROTOCOL_VALUE_OK)
            return status;
    } else if (kind != MO_TYPE_UNIT) {
        status = copy_leaf(to, kind, value, byte_limit, &fields[0]);
        if (status != MO_PROTOCOL_VALUE_OK)
            return status;
    }
    bool ok = mo_value_case(to, scalar, tag, fields, shape->field_count, out);
    for (uint32_t i = 0; i < shape->field_count; i++)
        mo_value_drop(to, shape->fields[i].type, fields[i]);
    return ok ? MO_PROTOCOL_VALUE_OK : MO_PROTOCOL_VALUE_MEMORY;
}

static enum mo_protocol_value_status unpack_bytes(struct mo_heap *to, struct mo_cell list,
                                                  size_t limit, struct mo_cell *out) {
    uint32_t count = mo_list_size(list);
    if (count > limit)
        return MO_PROTOCOL_VALUE_LIMIT;
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, to);
    uint8_t *bytes = count == 0 ? NULL : mo_scratch_resize(&scratch, NULL, 0, count);
    bool ok = count == 0 || bytes != NULL;
    if (ok) {
        for (uint32_t i = 0; i < count; i++) {
            struct mo_cell item;
            MO_ASSERT(mo_list_get(list, i, &item));
            bytes[i] = mo_cell_get_byte(item);
        }
        ok = mo_value_bytes(to, bytes, count, out);
    }
    mo_scratch_free(&scratch);
    return ok ? MO_PROTOCOL_VALUE_OK : MO_PROTOCOL_VALUE_MEMORY;
}

enum mo_protocol_value_status mo_protocol_scalar_unpack(const struct mo_types *protocol,
                                                        uint32_t scalar, struct mo_heap *to,
                                                        enum mo_type_kind kind,
                                                        struct mo_cell value, size_t byte_limit,
                                                        struct mo_cell *out) {
    uint32_t tag = scalar_tag(kind);
    if (tag == MO_NO_INDEX || mo_cell_get_tag(value) != tag)
        return MO_PROTOCOL_VALUE_INVALID;
    if (kind == MO_TYPE_UNIT) {
        *out = mo_cell_unit();
        return MO_PROTOCOL_VALUE_OK;
    }
    struct mo_cell payload = mo_cell_get_payload(value);
    enum mo_case_shape shape = mo_case_shape(protocol, scalar, tag);
    if (kind == MO_TYPE_RANGE) {
        *out = mo_cell_range(mo_cell_get_int(mo_value_field(payload, 0)),
                             mo_cell_get_int(mo_value_field(payload, 1)));
        return MO_PROTOCOL_VALUE_OK;
    }
    if (shape == MO_CASE_BOXED)
        payload = mo_value_field(payload, 0);
    if (kind == MO_TYPE_DURATION) {
        *out = mo_cell_duration(mo_cell_get_int(payload));
        return MO_PROTOCOL_VALUE_OK;
    }
    if (kind == MO_TYPE_BYTES)
        return unpack_bytes(to, payload, byte_limit, out);
    return copy_leaf(to, kind, payload, byte_limit, out);
}
