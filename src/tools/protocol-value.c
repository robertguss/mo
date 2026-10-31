#include "tools/protocol.h"

static bool charge(const struct mo_protocol_values *types, uint64_t amount) {
    if (types->work == NULL)
        return true;
    if (amount > *types->work) {
        *types->work = 0;
        return false;
    }
    *types->work -= amount;
    return true;
}

static bool composite(enum mo_type_kind kind) {
    return kind == MO_TYPE_TUPLE || kind == MO_TYPE_LIST || kind == MO_TYPE_SET ||
           kind == MO_TYPE_MAP || kind == MO_TYPE_RECORD || kind == MO_TYPE_CHOICE;
}

static bool is_struct(const struct mo_protocol_values *types, uint32_t type) {
    uint32_t low = 0, high = types->descriptor->shape_count;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        if (types->descriptor->shapes[mid].type < type)
            low = mid + 1;
        else
            high = mid;
    }
    MO_ASSERT(low < types->descriptor->shape_count && types->descriptor->shapes[low].type == type);
    return types->descriptor->shapes[low].is_struct;
}

static struct mo_cell case_field(const struct mo_types *types, uint32_t type, struct mo_cell value,
                                 uint32_t field) {
    struct mo_cell payload = mo_cell_get_payload(value);
    return mo_case_shape(types, type, mo_cell_get_tag(value)) == MO_CASE_BOXED
               ? mo_value_field(payload, field)
               : payload;
}

static uint32_t child_type(const struct mo_type *type, uint32_t tag, uint32_t field) {
    if (type->kind == MO_TYPE_CHOICE)
        return type->cases[tag].fields[field].type;
    if (type->kind == MO_TYPE_RECORD)
        return type->fields[field].type;
    if (type->kind == MO_TYPE_TUPLE)
        return type->parameters[field];
    return type->parameters[type->kind == MO_TYPE_MAP ? field % 2 : 0];
}

static uint32_t scalar_type(const struct mo_protocol_values *types) {
    return types->protocol->items[types->value].cases[0].fields[0].type;
}

static uint32_t list_type(const struct mo_protocol_values *types) {
    return types->protocol->items[types->value].cases[1].fields[0].type;
}

static bool charge_scalar(const struct mo_protocol_values *types, enum mo_type_kind kind,
                          struct mo_cell value, bool packed) {
    if (types->work == NULL)
        return true;
    if (!charge(types, 1))
        return false;
    if (packed) {
        uint32_t scalar = scalar_type(types), tag = mo_cell_get_tag(value);
        const struct mo_type_case *shape = &types->protocol->items[scalar].cases[tag];
        if (shape->field_count == 0)
            return true;
        kind = types->protocol->items[shape->fields[0].type].kind;
        value = case_field(types->protocol, scalar, value, 0);
    }
    size_t size = 0;
    if (kind == MO_TYPE_TEXT || kind == MO_TYPE_BYTES || kind == MO_TYPE_BIGINT) {
        const uint8_t *bytes = mo_value_data(value, &size);
        (void)bytes;
    } else if (packed && kind == MO_TYPE_LIST)
        size = mo_list_size(value);
    return charge(types, size);
}

static enum mo_protocol_value_status pack(const struct mo_protocol_values *types,
                                          struct mo_heap *to, uint32_t index, struct mo_cell value,
                                          size_t byte_limit, unsigned depth, size_t *nodes,
                                          struct mo_cell *out) {
    if (depth == 0 || *nodes == 0 || !charge(types, 1))
        return MO_PROTOCOL_VALUE_LIMIT;
    --*nodes;
    const struct mo_type *type = &types->service->items[index];
    if (!composite(type->kind)) {
        if (!charge_scalar(types, type->kind, value, false))
            return MO_PROTOCOL_VALUE_LIMIT;
        uint32_t scalar = scalar_type(types);
        struct mo_cell leaf;
        enum mo_protocol_value_status status = mo_protocol_scalar_pack(
            types->protocol, scalar, to, type->kind, value, byte_limit, &leaf);
        if (status != MO_PROTOCOL_VALUE_OK)
            return status;
        bool ok = mo_value_case(to, types->value, 0, &leaf, 1, out);
        mo_value_drop(to, scalar, leaf);
        return ok ? MO_PROTOCOL_VALUE_OK : MO_PROTOCOL_VALUE_MEMORY;
    }
    uint32_t count, tag = 0;
    bool variant = false;
    if (type->kind == MO_TYPE_CHOICE) {
        tag = mo_cell_get_tag(value);
        count = type->cases[tag].field_count;
        variant = !is_struct(types, index);
    } else if (type->kind == MO_TYPE_RECORD)
        count = type->field_count;
    else if (type->kind == MO_TYPE_TUPLE)
        count = type->parameter_count;
    else if (type->kind == MO_TYPE_LIST)
        count = mo_list_size(value);
    else {
        count = mo_table_size(value);
        if (type->kind == MO_TYPE_MAP) {
            if (count > UINT32_MAX / 2)
                return MO_PROTOCOL_VALUE_LIMIT;
            count *= 2;
        }
    }
    if (count > *nodes || !charge(types, count))
        return MO_PROTOCOL_VALUE_LIMIT;
    uint32_t list_index = list_type(types);
    struct mo_cell list;
    if (!mo_value_list(to, list_index, NULL, 0, &list))
        return MO_PROTOCOL_VALUE_MEMORY;
    enum mo_protocol_value_status status = MO_PROTOCOL_VALUE_OK;
    uint32_t position = 0;
    struct mo_cell key = mo_cell_unit(), item = mo_cell_unit();
    for (uint32_t i = 0; i < count; i++) {
        struct mo_cell child;
        if (type->kind == MO_TYPE_LIST)
            MO_ASSERT(mo_list_get(value, i, &child));
        else if (type->kind == MO_TYPE_SET || type->kind == MO_TYPE_MAP) {
            if (type->kind == MO_TYPE_SET || i % 2 == 0)
                MO_ASSERT(mo_table_next(value, &position, &key, &item));
            child = type->kind == MO_TYPE_SET || i % 2 == 0 ? key : item;
        } else if (type->kind == MO_TYPE_CHOICE)
            child = case_field(types->service, index, value, i);
        else
            child = mo_value_field(value, i);
        struct mo_cell copied;
        status =
            pack(types, to, child_type(type, tag, i), child, byte_limit, depth - 1, nodes, &copied);
        if (status != MO_PROTOCOL_VALUE_OK)
            break;
        bool ok = mo_list_append(to, list_index, &list, copied);
        mo_value_drop(to, types->value, copied);
        if (!ok) {
            status = MO_PROTOCOL_VALUE_MEMORY;
            break;
        }
    }
    if (status == MO_PROTOCOL_VALUE_OK) {
        struct mo_cell fields[] = {mo_cell_int(tag), list};
        bool ok = variant ? mo_value_case(to, types->value, 2, fields, 2, out)
                          : mo_value_case(to, types->value, 1, &list, 1, out);
        if (!ok)
            status = MO_PROTOCOL_VALUE_MEMORY;
    }
    mo_value_drop(to, list_index, list);
    return status;
}

static enum mo_protocol_value_status build(struct mo_heap *to, uint32_t index,
                                           const struct mo_type *type, uint32_t tag,
                                           const struct mo_cell *fields, uint32_t count,
                                           struct mo_cell *out) {
    bool ok;
    if (type->kind == MO_TYPE_LIST)
        ok = mo_value_list(to, index, fields, count, out);
    else if (type->kind == MO_TYPE_CHOICE)
        ok = mo_value_case(to, index, tag, fields, count, out);
    else if (type->kind == MO_TYPE_TUPLE || type->kind == MO_TYPE_RECORD)
        ok = mo_value_product(to, index, fields, count, out);
    else {
        struct mo_cell table;
        if (type->kind == MO_TYPE_SET)
            ok = mo_value_set(to, index, fields, count, &table);
        else {
            ok = mo_value_map(to, index, NULL, NULL, 0, &table);
            if (ok) {
                for (uint32_t i = 0; ok && i < count; i += 2)
                    ok = mo_table_set(to, index, &table, fields[i], fields[i + 1]);
                if (!ok)
                    mo_value_drop(to, index, table);
            }
        }
        if (!ok)
            return MO_PROTOCOL_VALUE_MEMORY;
        uint32_t expected = type->kind == MO_TYPE_MAP ? count / 2 : count;
        if (mo_table_size(table) != expected) {
            mo_value_drop(to, index, table);
            return MO_PROTOCOL_VALUE_INVALID;
        }
        *out = table;
    }
    return ok ? MO_PROTOCOL_VALUE_OK : MO_PROTOCOL_VALUE_MEMORY;
}

static enum mo_protocol_value_status unpack(const struct mo_protocol_values *types,
                                            struct mo_heap *to, uint32_t index,
                                            struct mo_cell value, size_t byte_limit, unsigned depth,
                                            struct mo_cell *out) {
    if (depth == 0 || !charge(types, 1))
        return MO_PROTOCOL_VALUE_LIMIT;
    const struct mo_type *type = &types->service->items[index];
    uint32_t shape = mo_cell_get_tag(value);
    if (!composite(type->kind)) {
        if (shape != 0)
            return MO_PROTOCOL_VALUE_INVALID;
        struct mo_cell scalar = case_field(types->protocol, types->value, value, 0);
        if (!charge_scalar(types, type->kind, scalar, true))
            return MO_PROTOCOL_VALUE_LIMIT;
        return mo_protocol_scalar_unpack(types->protocol, scalar_type(types), to, type->kind,
                                         scalar, byte_limit, out);
    }
    uint32_t tag = 0;
    struct mo_cell list;
    if (type->kind == MO_TYPE_CHOICE && !is_struct(types, index)) {
        if (shape != 2)
            return MO_PROTOCOL_VALUE_INVALID;
        int64_t choice = mo_cell_get_int(case_field(types->protocol, types->value, value, 0));
        if (choice < 0 || (uint64_t)choice >= type->case_count)
            return MO_PROTOCOL_VALUE_INVALID;
        tag = (uint32_t)choice;
        list = case_field(types->protocol, types->value, value, 1);
    } else {
        if (shape != 1)
            return MO_PROTOCOL_VALUE_INVALID;
        list = case_field(types->protocol, types->value, value, 0);
    }
    uint32_t count = mo_list_size(list);
    if ((type->kind == MO_TYPE_CHOICE && count != type->cases[tag].field_count) ||
        (type->kind == MO_TYPE_RECORD && count != type->field_count) ||
        (type->kind == MO_TYPE_TUPLE && count != type->parameter_count) ||
        (type->kind == MO_TYPE_MAP && count % 2 != 0))
        return MO_PROTOCOL_VALUE_INVALID;
    if (!charge(types, count))
        return MO_PROTOCOL_VALUE_LIMIT;
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, to);
    size_t size = (size_t)count * sizeof(struct mo_cell);
    struct mo_cell *fields = count == 0 ? NULL : mo_scratch_resize(&scratch, NULL, 0, size);
    enum mo_protocol_value_status status = MO_PROTOCOL_VALUE_OK;
    uint32_t built = 0;
    if (count != 0 && fields == NULL)
        status = MO_PROTOCOL_VALUE_MEMORY;
    while (status == MO_PROTOCOL_VALUE_OK && built < count) {
        struct mo_cell child;
        MO_ASSERT(mo_list_get(list, built, &child));
        status = unpack(types, to, child_type(type, tag, built), child, byte_limit, depth - 1,
                        &fields[built]);
        if (status == MO_PROTOCOL_VALUE_OK)
            built++;
    }
    if (status == MO_PROTOCOL_VALUE_OK)
        status = build(to, index, type, tag, fields, count, out);
    for (uint32_t i = 0; i < built; i++)
        mo_value_drop(to, child_type(type, tag, i), fields[i]);
    mo_scratch_free(&scratch);
    return status;
}

enum mo_protocol_value_status mo_protocol_value_pack(const struct mo_protocol_values *types,
                                                     struct mo_heap *to, uint32_t type,
                                                     struct mo_cell value, size_t byte_limit,
                                                     struct mo_cell *out) {
    size_t nodes = types->node_limit == 0 ? SIZE_MAX : types->node_limit;
    return pack(types, to, type, value, byte_limit, MO_LIMIT_MCP_VALUE_DEPTH, &nodes, out);
}

enum mo_protocol_value_status mo_protocol_value_unpack(const struct mo_protocol_values *types,
                                                       struct mo_heap *to, uint32_t type,
                                                       struct mo_cell value, size_t byte_limit,
                                                       struct mo_cell *out) {
    return unpack(types, to, type, value, byte_limit, MO_LIMIT_MCP_VALUE_DEPTH, out);
}
