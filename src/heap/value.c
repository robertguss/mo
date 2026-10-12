#include "heap/internal.h"

#include <string.h>

/* Validate UTF-8 before allocating or changing ownership. */
static MO_MUST_USE bool valid_utf8(const uint8_t *bytes, size_t length) {
    for (size_t i = 0; i < length;) {
        uint32_t scalar = bytes[i++];
        if (scalar < 0x80) {
            continue;
        }
        unsigned following;
        uint32_t minimum;
        if (scalar >= 0xc2 && scalar <= 0xdf) {
            following = 1;
            minimum = 0x80;
            scalar &= 0x1f;
        } else if (scalar >= 0xe0 && scalar <= 0xef) {
            following = 2;
            minimum = 0x800;
            scalar &= 0x0f;
        } else if (scalar >= 0xf0 && scalar <= 0xf4) {
            following = 3;
            minimum = 0x10000;
            scalar &= 7;
        } else {
            return false;
        }
        if (following > length - i) {
            return false;
        }
        for (unsigned j = 0; j < following; j++) {
            uint32_t byte = bytes[i++];
            if (byte < 0x80 || byte > 0xbf) {
                return false;
            }
            scalar = (scalar << 6) | (byte & 0x3f);
        }
        if (scalar < minimum || scalar >= 0x110000 || (scalar >= 0xd800 && scalar <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

static MO_MUST_USE bool byte_value(struct mo_process *process, enum mo_type_kind kind,
                                   const uint8_t *bytes, size_t length, uint32_t auxiliary,
                                   struct mo_cell *out) {
    MO_ASSERT(process != NULL && out != NULL);
    if (length > MO_LIMIT_STRING_SIZE || (length != 0 && bytes == NULL) ||
        process->builtins[kind] == MO_NO_INDEX) {
        return false;
    }
    if (kind == MO_TYPE_TEXT && !valid_utf8(bytes, length)) {
        return false;
    }
    struct mo_allocation *allocation =
        mo_heap_allocate(process, process->builtins[kind], MO_BUILTIN_LAYOUT(kind), length);
    if (allocation == NULL) {
        return false;
    }
    allocation->length = (uint32_t)length;
    allocation->auxiliary = auxiliary;
    if (length != 0) {
        memcpy(allocation->cells, bytes, length);
    }
    *out = mo_cell_ref(&allocation->object);
    return true;
}

bool mo_value_text(struct mo_process *process, const char *utf8, size_t length,
                   struct mo_cell *out) {
    return byte_value(process, MO_TYPE_TEXT, (const uint8_t *)utf8, length, 0, out);
}

bool mo_value_bytes(struct mo_process *process, const uint8_t *bytes, size_t length,
                    struct mo_cell *out) {
    return byte_value(process, MO_TYPE_BYTES, bytes, length, 0, out);
}

bool mo_value_bigint(struct mo_process *process, enum mo_bigint_sign sign, const uint8_t *magnitude,
                     size_t length, struct mo_cell *out) {
    if (length > MO_LIMIT_STRING_SIZE || (length != 0 && magnitude == NULL) ||
        (sign != MO_BIGINT_ZERO && sign != MO_BIGINT_POSITIVE && sign != MO_BIGINT_NEGATIVE) ||
        ((sign == MO_BIGINT_ZERO) != (length == 0)) ||
        (length != 0 && magnitude[length - 1] == 0)) {
        return false;
    }
    return byte_value(process, MO_TYPE_BIGINT, magnitude, length, (uint32_t)sign, out);
}

const uint8_t *mo_value_data(struct mo_cell value, size_t *length) {
    MO_ASSERT(length != NULL);
    struct mo_allocation *allocation = mo_heap_allocation(mo_cell_get_ref(value));
    MO_ASSERT(allocation->object.layout == MO_BUILTIN_LAYOUT(MO_TYPE_TEXT) ||
              allocation->object.layout == MO_BUILTIN_LAYOUT(MO_TYPE_BYTES) ||
              allocation->object.layout == MO_BUILTIN_LAYOUT(MO_TYPE_BIGINT));
    *length = allocation->length;
    return (const uint8_t *)allocation->cells;
}

struct mo_cell mo_value_field(struct mo_cell value, uint32_t field) {
    struct mo_allocation *allocation = mo_heap_allocation(mo_cell_get_ref(value));
    MO_ASSERT(field < allocation->length);
    return allocation->cells[field];
}

bool mo_value_construct(struct mo_process *process, uint32_t layout, const struct mo_cell *fields,
                        uint32_t count, struct mo_cell *out) {
    MO_ASSERT(process != NULL && out != NULL);
    if (layout >= process->types->layout_count || (count != 0 && fields == NULL)) {
        return false;
    }
    const struct mo_layout *fixed = &process->types->layouts[layout];
    if (fixed->kind == MO_LAYOUT_CLOSURE || count != fixed->field_count) {
        return false;
    }
    struct mo_allocation *allocation =
        mo_heap_allocate(process, fixed->type, layout, (size_t)count * sizeof(struct mo_cell));
    if (allocation == NULL) {
        return false;
    }
    allocation->length = count;
    for (uint32_t i = 0; i < count; i++) {
        allocation->cells[i] = mo_value_dup(process, fixed->fields[i], fields[i]);
    }
    struct mo_cell result = mo_cell_ref(&allocation->object);
    *out = fixed->kind == MO_LAYOUT_CASE ? mo_cell_case(fixed->case_index, result) : result;
    return true;
}

bool mo_value_case(struct mo_process *process, uint32_t type, uint32_t tag,
                   const struct mo_cell *fields, uint32_t count, struct mo_cell *out) {
    MO_ASSERT(process != NULL && out != NULL);
    if (type >= process->types->count || process->types->items[type].kind != MO_TYPE_CHOICE ||
        tag >= process->types->items[type].case_count || (count != 0 && fields == NULL) ||
        count != process->types->items[type].cases[tag].field_count) {
        return false;
    }
    enum mo_case_shape shape = mo_case_shape(process->types, type, tag);
    if (shape == MO_CASE_BOXED) {
        for (uint32_t i = 0; i < process->types->layout_count; i++) {
            const struct mo_layout *layout = &process->types->layouts[i];
            if (layout->kind == MO_LAYOUT_CASE && layout->type == type &&
                layout->case_index == tag) {
                return mo_value_construct(process, i, fields, count, out);
            }
        }
        return false;
    }
    struct mo_cell payload = mo_cell_unit();
    if (shape != MO_CASE_EMPTY) {
        payload =
            mo_value_dup(process, process->types->items[type].cases[tag].fields[0].type, fields[0]);
    }
    *out = mo_cell_case(tag, payload);
    return true;
}

bool mo_value_closure(struct mo_process *process, uint32_t function, const struct mo_cell *captures,
                      uint32_t count, struct mo_cell *out) {
    MO_ASSERT(process != NULL && out != NULL);
    if (count != 0 && captures == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < process->types->layout_count; i++) {
        const struct mo_layout *layout = &process->types->layouts[i];
        if (layout->kind != MO_LAYOUT_CLOSURE || layout->function != function) {
            continue;
        }
        if (count != layout->field_count) {
            return false;
        }
        struct mo_allocation *allocation =
            mo_heap_allocate(process, layout->type, i, (size_t)count * sizeof(struct mo_cell));
        if (allocation == NULL) {
            return false;
        }
        allocation->length = count;
        allocation->auxiliary = function;
        for (uint32_t j = 0; j < count; j++) {
            allocation->cells[j] = mo_value_dup(process, layout->fields[j], captures[j]);
        }
        if (count == 0) {
            allocation->object.count = MO_IMMORTAL_COUNT;
        }
        *out = mo_cell_ref(&allocation->object);
        return true;
    }
    return false;
}

bool mo_value_list(struct mo_process *process, uint32_t type, const struct mo_cell *elements,
                   uint32_t count, struct mo_cell *out) {
    MO_ASSERT(process != NULL && out != NULL);
    if (type >= process->types->count || process->types->items[type].kind != MO_TYPE_LIST ||
        (count != 0 && elements == NULL)) {
        return false;
    }
    struct mo_allocation *allocation = mo_heap_allocate(
        process, type, MO_BUILTIN_LAYOUT(MO_TYPE_LIST), (size_t)count * sizeof(struct mo_cell));
    if (allocation == NULL) {
        return false;
    }
    allocation->length = count;
    allocation->capacity = count;
    uint32_t element_type = process->types->items[type].parameters[0];
    for (uint32_t i = 0; i < count; i++) {
        allocation->cells[i] = mo_value_dup(process, element_type, elements[i]);
    }
    *out = mo_cell_ref(&allocation->object);
    return true;
}

uint32_t mo_list_size(struct mo_cell list) {
    struct mo_allocation *allocation = mo_heap_allocation(mo_cell_get_ref(list));
    MO_ASSERT(allocation->object.layout == MO_BUILTIN_LAYOUT(MO_TYPE_LIST));
    return allocation->length;
}

bool mo_list_get(struct mo_cell list, uint32_t index, struct mo_cell *out) {
    MO_ASSERT(out != NULL);
    struct mo_allocation *allocation = mo_heap_allocation(mo_cell_get_ref(list));
    MO_ASSERT(allocation->object.layout == MO_BUILTIN_LAYOUT(MO_TYPE_LIST));
    if (index >= allocation->length) {
        return false;
    }
    *out = allocation->cells[index];
    return true;
}

static MO_MUST_USE struct mo_allocation *list_room(struct mo_process *process, uint32_t type,
                                                   struct mo_allocation *old, uint32_t needed) {
    if (old->object.count == 1 && needed <= old->capacity) {
        return old;
    }
    uint32_t capacity = old->capacity;
    if (needed > capacity) {
        capacity = capacity > UINT32_MAX / 2 ? UINT32_MAX : capacity * 2;
        if (capacity < 4) {
            capacity = 4;
        }
        if (capacity < needed) {
            capacity = needed;
        }
    }
    struct mo_allocation *copy = mo_heap_allocate(process, type, MO_BUILTIN_LAYOUT(MO_TYPE_LIST),
                                                  (size_t)capacity * sizeof(struct mo_cell));
    if (copy == NULL) {
        return NULL;
    }
    copy->length = old->length;
    copy->capacity = capacity;
    uint32_t element_type = process->types->items[type].parameters[0];
    for (uint32_t i = 0; i < old->length; i++) {
        copy->cells[i] = mo_value_dup(process, element_type, old->cells[i]);
    }
    return copy;
}

bool mo_list_append(struct mo_process *process, uint32_t type, struct mo_cell *list,
                    struct mo_cell element) {
    MO_ASSERT(process != NULL && list != NULL && type < process->types->count);
    MO_ASSERT(process->types->items[type].kind == MO_TYPE_LIST);
    struct mo_allocation *old = mo_heap_allocation(mo_cell_get_ref(*list));
    MO_ASSERT(old->type == type);
    if (old->length == UINT32_MAX) {
        return false;
    }
    struct mo_allocation *allocation = list_room(process, type, old, old->length + 1);
    if (allocation == NULL) {
        return false;
    }
    allocation->cells[allocation->length++] =
        mo_value_dup(process, process->types->items[type].parameters[0], element);
    if (allocation != old) {
        mo_value_drop(process, type, *list);
        *list = mo_cell_ref(&allocation->object);
    }
    return true;
}

bool mo_list_set(struct mo_process *process, uint32_t type, struct mo_cell *list, uint32_t index,
                 struct mo_cell element) {
    MO_ASSERT(process != NULL && list != NULL && type < process->types->count);
    MO_ASSERT(process->types->items[type].kind == MO_TYPE_LIST);
    struct mo_allocation *old = mo_heap_allocation(mo_cell_get_ref(*list));
    MO_ASSERT(old->type == type);
    if (index >= old->length) {
        return false;
    }
    struct mo_allocation *allocation = list_room(process, type, old, old->length);
    if (allocation == NULL) {
        return false;
    }
    uint32_t element_type = process->types->items[type].parameters[0];
    struct mo_cell held = mo_value_dup(process, element_type, element);
    mo_value_drop(process, element_type, allocation->cells[index]);
    allocation->cells[index] = held;
    if (allocation != old) {
        mo_value_drop(process, type, *list);
        *list = mo_cell_ref(&allocation->object);
    }
    return true;
}
