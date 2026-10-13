/* Independent type-table walk for generated values. No generator internals. */
#ifndef MO_TEST_INPUT_VALUES_H
#define MO_TEST_INPUT_VALUES_H

#include "heap/heap.h"
#include "prim/prim.h"

struct input_value_frame {
    struct mo_cell value, pending;
    uint32_t type, count, index, position;
    bool entered;
};

static MO_MUST_USE bool input_value_enter(struct mo_heap *heap, const struct mo_types *types,
                                          struct input_value_frame *frame) {
    const struct mo_type *type = &types->items[frame->type];
    struct mo_cell value = frame->value;
    size_t used = 16;
    struct mo_object *object = NULL;
    enum mo_layout_kind layout_kind = MO_LAYOUT_RECORD;
    uint32_t tag = MO_NO_INDEX;
    switch (type->kind) {
    case MO_TYPE_UNIT:
        used = 0;
        break;
    case MO_TYPE_BOOL:
        if (value.bytes[0] > 1)
            return false;
        used = 1;
        break;
    case MO_TYPE_BYTE:
        used = 1;
        break;
    case MO_TYPE_CHAR: {
        uint32_t scalar = mo_cell_get_char(value);
        if (scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff))
            return false;
        used = 4;
        break;
    }
    case MO_TYPE_TIME:
        if (mo_cell_get_time(value) < -INT64_C(62135596800000000) ||
            mo_cell_get_time(value) > INT64_C(253402300799999999))
            return false;
        used = 8;
        break;
    case MO_TYPE_INT:
    case MO_TYPE_FLOAT:
    case MO_TYPE_DURATION:
        used = 8;
        break;
    case MO_TYPE_DECIMAL: {
        mo_i128 maximum = (mo_i128)(((mo_u128)1 << 127) - 1);
        if (mo_cell_get_decimal(value) < -maximum)
            return false;
        break;
    }
    case MO_TYPE_RANGE:
        break;
    case MO_TYPE_TEXT:
    case MO_TYPE_BYTES:
    case MO_TYPE_BIGINT: {
        object = mo_cell_get_ref(value);
        if (object == NULL || object->count == 0)
            return false;
        size_t length;
        const uint8_t *bytes = mo_value_data(value, &length);
        struct mo_cell exemplar;
        bool ok;
        if (type->kind == MO_TYPE_TEXT) {
            uint64_t width;
            if (!mo_unicode16_display_width(bytes, length, &width))
                return false;
            ok = mo_value_text(heap, NULL, 0, &exemplar);
        } else if (type->kind == MO_TYPE_BYTES)
            ok = mo_value_bytes(heap, NULL, 0, &exemplar);
        else {
            enum mo_bigint_sign sign = mo_value_bigint_sign(value);
            if ((length == 0) != (sign == MO_BIGINT_ZERO) ||
                (length != 0 && (bytes[length - 1] == 0 ||
                                 (sign != MO_BIGINT_POSITIVE && sign != MO_BIGINT_NEGATIVE))))
                return false;
            ok = mo_value_bigint(heap, MO_BIGINT_ZERO, NULL, 0, &exemplar);
        }
        if (!ok)
            return false;
        bool same = mo_cell_get_ref(exemplar)->layout == object->layout;
        mo_value_drop(heap, frame->type, exemplar);
        return same;
    }
    case MO_TYPE_RECORD:
        frame->count = type->field_count;
        object = mo_cell_get_ref(value);
        break;
    case MO_TYPE_TUPLE:
        frame->count = type->parameter_count;
        object = mo_cell_get_ref(value);
        layout_kind = MO_LAYOUT_TUPLE;
        break;
    case MO_TYPE_CHOICE: {
        tag = mo_cell_get_tag(value);
        if (tag >= type->case_count)
            return false;
        for (size_t i = 4; i < 8; i++)
            if (value.bytes[i] != 0)
                return false;
        frame->count = type->cases[tag].field_count;
        if (mo_case_shape(types, frame->type, tag) == MO_CASE_BOXED) {
            object = mo_cell_get_ref(mo_cell_get_payload(value));
            layout_kind = MO_LAYOUT_CASE;
        } else if (frame->count == 0) {
            for (size_t i = 8; i < 16; i++)
                if (value.bytes[i] != 0)
                    return false;
        }
        break;
    }
    case MO_TYPE_LIST:
        frame->count = mo_list_size(value);
        return true;
    case MO_TYPE_SET:
    case MO_TYPE_MAP:
        frame->count = mo_table_size(value) * (type->kind == MO_TYPE_MAP ? 2U : 1U);
        return true;
    case MO_TYPE_FUNCTION:
    case MO_TYPE_ENV:
    case MO_TYPE_CAPABILITY:
        return false;
    }
    if (object != NULL) {
        if (object->count == 0 || object->layout >= types->layout_count)
            return false;
        const struct mo_layout *layout = &types->layouts[object->layout];
        return layout->type == frame->type && layout->kind == layout_kind &&
               layout->field_count == frame->count && layout->case_index == tag;
    }
    if (type->kind == MO_TYPE_RECORD || type->kind == MO_TYPE_TUPLE ||
        (type->kind == MO_TYPE_CHOICE && tag != MO_NO_INDEX &&
         mo_case_shape(types, frame->type, tag) == MO_CASE_BOXED))
        return false;
    for (size_t i = used; i < 16; i++)
        if (value.bytes[i] != 0)
            return false;
    return true;
}

static MO_MUST_USE bool input_value_typed(struct mo_heap *heap, const struct mo_types *types,
                                          uint32_t type, struct mo_cell value) {
    struct input_value_frame stack[MO_LIMIT_INPUT_DEPTH];
    stack[0] = (struct input_value_frame){.type = type, .value = value};
    uint32_t depth = 1, nodes = 0;
    while (depth != 0) {
        struct input_value_frame *frame = &stack[depth - 1];
        if (!frame->entered) {
            if (++nodes > MO_LIMIT_INPUT_NODES || !input_value_enter(heap, types, frame))
                return false;
            frame->entered = true;
        }
        if (frame->index == frame->count) {
            depth--;
            continue;
        }
        if (depth == MO_LIMIT_INPUT_DEPTH)
            return false;
        const struct mo_type *parent = &types->items[frame->type];
        uint32_t index = frame->index++;
        struct mo_cell child;
        uint32_t child_type;
        if (parent->kind == MO_TYPE_LIST) {
            if (!mo_list_get(frame->value, index, &child))
                return false;
            child_type = parent->parameters[0];
        } else if (parent->kind == MO_TYPE_SET || parent->kind == MO_TYPE_MAP) {
            if (parent->kind == MO_TYPE_MAP && index % 2 != 0) {
                child = frame->pending;
                child_type = parent->parameters[1];
            } else {
                if (!mo_table_next(frame->value, &frame->position, &child, &frame->pending))
                    return false;
                child_type = parent->parameters[0];
            }
        } else if (parent->kind == MO_TYPE_CHOICE) {
            uint32_t c = mo_cell_get_tag(frame->value);
            struct mo_cell payload = mo_cell_get_payload(frame->value);
            child_type = parent->cases[c].fields[index].type;
            child = mo_case_shape(types, frame->type, c) == MO_CASE_BOXED
                        ? mo_value_field(payload, index)
                        : payload;
        } else {
            child_type = parent->kind == MO_TYPE_TUPLE ? parent->parameters[index]
                                                       : parent->fields[index].type;
            child = mo_value_field(frame->value, index);
        }
        stack[depth++] = (struct input_value_frame){.type = child_type, .value = child};
    }
    return true;
}

#endif
