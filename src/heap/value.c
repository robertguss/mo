#include "heap/internal.h"

#include <errno.h>
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

static MO_MUST_USE bool byte_value(struct mo_heap *process, enum mo_type_kind kind,
                                   const uint8_t *bytes, size_t length, uint32_t auxiliary,
                                   struct mo_cell *out) {
    MO_ASSERT(process != NULL && out != NULL);
    if (length > MO_LIMIT_VALUE_BYTES || (length != 0 && bytes == NULL) ||
        process->builtins[kind] == MO_NO_INDEX) {
        return false;
    }
    if (kind == MO_TYPE_TEXT && !valid_utf8(bytes, length)) {
        errno = EINVAL;
        return false;
    }
    size_t trailer = kind == MO_TYPE_BIGINT ? sizeof(uint64_t) : 0;
    struct mo_allocation *allocation = mo_heap_allocate(process, process->builtins[kind],
                                                        MO_BUILTIN_LAYOUT(kind), length + trailer);
    if (allocation == NULL) {
        return false;
    }
    allocation->length = (uint32_t)length;
    if (trailer != 0) {
        memcpy((uint8_t *)allocation->cells + length, &auxiliary, sizeof auxiliary);
    }
    if (length != 0) {
        memcpy(allocation->cells, bytes, length);
    }
    *out = mo_cell_ref(&allocation->object);
    return true;
}

bool mo_value_text(struct mo_heap *process, const char *utf8, size_t length, struct mo_cell *out) {
    return byte_value(process, MO_TYPE_TEXT, (const uint8_t *)utf8, length, 0, out);
}

bool mo_value_bytes(struct mo_heap *process, const uint8_t *bytes, size_t length,
                    struct mo_cell *out) {
    return byte_value(process, MO_TYPE_BYTES, bytes, length, 0, out);
}

bool mo_value_bigint(struct mo_heap *process, enum mo_bigint_sign sign, const uint8_t *magnitude,
                     size_t length, struct mo_cell *out) {
    if (length > MO_LIMIT_VALUE_BYTES || (length != 0 && magnitude == NULL) ||
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

uint32_t mo_bigint_sign(const struct mo_allocation *value) {
    uint32_t sign;
    memcpy(&sign, (const uint8_t *)value->cells + value->length, sizeof sign);
    return sign;
}

enum mo_bigint_sign mo_value_bigint_sign(struct mo_cell value) {
    struct mo_allocation *a = mo_heap_allocation(mo_cell_get_ref(value));
    MO_ASSERT(a->object.layout == MO_BUILTIN_LAYOUT(MO_TYPE_BIGINT));
    uint32_t sign = mo_bigint_sign(a);
    MO_ASSERT(sign <= MO_BIGINT_NEGATIVE);
    return (enum mo_bigint_sign)sign;
}

struct mo_cell mo_value_field(struct mo_cell value, uint32_t field) {
    struct mo_allocation *allocation = mo_heap_allocation(mo_cell_get_ref(value));
    MO_ASSERT(field < allocation->length);
    return allocation->cells[field];
}

bool mo_value_set_field_owned(struct mo_heap *process, uint32_t type, struct mo_cell value,
                              uint32_t field, struct mo_cell replacement, struct mo_cell *out) {
    MO_ASSERT(process != NULL && out != NULL && type < process->types->count);
    struct mo_allocation *old = mo_heap_allocation(mo_cell_get_ref(value));
    MO_ASSERT(old->type == type && old->object.layout < process->types->layout_count);
    const struct mo_layout *layout = &process->types->layouts[old->object.layout];
    MO_ASSERT((layout->kind == MO_LAYOUT_RECORD || layout->kind == MO_LAYOUT_TUPLE) &&
              field < layout->field_count);
    if (old->object.count == 1) {
        struct mo_cell previous = old->cells[field];
        old->cells[field] = replacement;
        old->hash_valid = false;
        mo_value_drop(process, layout->fields[field], previous);
        *out = value;
        return true;
    }
    struct mo_allocation *copy = mo_heap_allocate(
        process, type, old->object.layout, (size_t)layout->field_count * sizeof(struct mo_cell));
    if (copy == NULL)
        return false;
    copy->length = layout->field_count;
    for (uint32_t i = 0; i < copy->length; i++) {
        copy->cells[i] =
            i == field ? replacement : mo_value_dup(process, layout->fields[i], old->cells[i]);
    }
    mo_value_drop(process, type, value);
    *out = mo_cell_ref(&copy->object);
    return true;
}

static MO_MUST_USE bool construct(struct mo_heap *process, uint32_t layout,
                                  const struct mo_cell *fields, uint32_t count, bool owned,
                                  struct mo_cell *out) {
    MO_ASSERT(process != NULL && out != NULL);
    if (layout >= process->types->layout_count || (count != 0 && fields == NULL)) {
        MO_ASSERT(!owned);
        return false;
    }
    const struct mo_layout *fixed = &process->types->layouts[layout];
    if (fixed->kind == MO_LAYOUT_CLOSURE || count != fixed->field_count) {
        MO_ASSERT(!owned);
        return false;
    }
    struct mo_allocation *allocation =
        mo_heap_allocate(process, fixed->type, layout, (size_t)count * sizeof(struct mo_cell));
    if (allocation == NULL) {
        return false;
    }
    allocation->length = count;
    for (uint32_t i = 0; i < count; i++) {
        allocation->cells[i] =
            owned ? fields[i] : mo_value_dup(process, fixed->fields[i], fields[i]);
    }
    struct mo_cell result = mo_cell_ref(&allocation->object);
    *out = fixed->kind == MO_LAYOUT_CASE ? mo_cell_case(fixed->case_index, result) : result;
    return true;
}

bool mo_value_construct(struct mo_heap *process, uint32_t layout, const struct mo_cell *fields,
                        uint32_t count, struct mo_cell *out) {
    return construct(process, layout, fields, count, false, out);
}

bool mo_value_construct_owned(struct mo_heap *process, uint32_t layout,
                              const struct mo_cell *fields, uint32_t count, struct mo_cell *out) {
    return construct(process, layout, fields, count, true, out);
}

static MO_MUST_USE bool case_value(struct mo_heap *process, uint32_t type, uint32_t tag,
                                   const struct mo_cell *fields, uint32_t count, bool owned,
                                   struct mo_cell *out) {
    MO_ASSERT(process != NULL && out != NULL);
    if (type >= process->types->count || process->types->items[type].kind != MO_TYPE_CHOICE ||
        tag >= process->types->items[type].case_count || (count != 0 && fields == NULL) ||
        count != process->types->items[type].cases[tag].field_count) {
        MO_ASSERT(!owned);
        return false;
    }
    enum mo_case_shape shape = mo_case_shape(process->types, type, tag);
    if (shape == MO_CASE_BOXED) {
        if (!mo_heap_image_refresh(process->image))
            return false;
        struct mo_layout_lookup *layout = mo_heap_layout(process, 0, type, tag);
        MO_ASSERT(!owned || layout != NULL);
        return layout != NULL && construct(process, layout->index, fields, count, owned, out);
    }
    struct mo_cell payload = mo_cell_unit();
    if (shape != MO_CASE_EMPTY) {
        payload = owned
                      ? fields[0]
                      : mo_value_dup(process, process->types->items[type].cases[tag].fields[0].type,
                                     fields[0]);
    }
    *out = mo_cell_case(tag, payload);
    return true;
}

bool mo_value_case(struct mo_heap *process, uint32_t type, uint32_t tag,
                   const struct mo_cell *fields, uint32_t count, struct mo_cell *out) {
    return case_value(process, type, tag, fields, count, false, out);
}

bool mo_value_case_owned(struct mo_heap *process, uint32_t type, uint32_t tag,
                         const struct mo_cell *fields, uint32_t count, struct mo_cell *out) {
    return case_value(process, type, tag, fields, count, true, out);
}

static MO_MUST_USE bool closure(struct mo_heap *process, uint32_t function,
                                const struct mo_cell *captures, uint32_t count, bool owned,
                                struct mo_cell *out) {
    MO_ASSERT(process != NULL && out != NULL);
    if (count != 0 && captures == NULL) {
        MO_ASSERT(!owned);
        return false;
    }
    if (!mo_heap_image_refresh(process->image))
        return false;
    struct mo_layout_lookup *entry = mo_heap_layout(process, 1, function, 0);
    if (entry == NULL) {
        MO_ASSERT(!owned);
        return false;
    }
    uint32_t index = entry->index;
    const struct mo_layout *layout = &process->types->layouts[index];
    if (count != layout->field_count) {
        MO_ASSERT(!owned);
        return false;
    }
    if (entry->constant != NULL) {
        *out = mo_cell_ref(entry->constant);
        return true;
    }
    struct mo_allocation *allocation =
        mo_heap_allocate(process, layout->type, index, (size_t)count * sizeof(struct mo_cell));
    if (allocation == NULL) {
        return false;
    }
    allocation->length = count;
    for (uint32_t j = 0; j < count; j++) {
        allocation->cells[j] =
            owned ? captures[j] : mo_value_dup(process, layout->fields[j], captures[j]);
    }
    MO_ASSERT(count != 0); /* Captureless closures were initialized with the image. */
    *out = mo_cell_ref(&allocation->object);
    return true;
}

bool mo_value_closure(struct mo_heap *process, uint32_t function, const struct mo_cell *captures,
                      uint32_t count, struct mo_cell *out) {
    return closure(process, function, captures, count, false, out);
}

bool mo_value_closure_owned(struct mo_heap *process, uint32_t function,
                            const struct mo_cell *captures, uint32_t count, struct mo_cell *out) {
    return closure(process, function, captures, count, true, out);
}

bool mo_value_list(struct mo_heap *process, uint32_t type, const struct mo_cell *elements,
                   uint32_t count, struct mo_cell *out) {
    MO_ASSERT(process != NULL && out != NULL);
    if (type >= process->types->count || process->types->items[type].kind != MO_TYPE_LIST ||
        count > MO_LIMIT_COLLECTION_SIZE || (count != 0 && elements == NULL)) {
        return false;
    }
    struct mo_allocation *allocation = mo_heap_allocate(
        process, type, MO_BUILTIN_LAYOUT(MO_TYPE_LIST), (size_t)count * sizeof(struct mo_cell));
    if (allocation == NULL) {
        return false;
    }
    allocation->length = count;
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

static MO_MUST_USE struct mo_allocation *list_room(struct mo_heap *process, uint32_t type,
                                                   struct mo_allocation *old, uint32_t needed) {
    uint32_t capacity =
        (uint32_t)((old->size - offsetof(struct mo_allocation, cells)) / sizeof(struct mo_cell));
    if (old->object.count == 1 && needed <= capacity) {
        return old;
    }
    if (needed > capacity) {
        capacity =
            capacity > MO_LIMIT_COLLECTION_SIZE / 2 ? MO_LIMIT_COLLECTION_SIZE : capacity * 2;
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
    uint32_t element_type = process->types->items[type].parameters[0];
    for (uint32_t i = 0; i < old->length; i++) {
        copy->cells[i] = mo_value_dup(process, element_type, old->cells[i]);
    }
    return copy;
}

bool mo_list_append(struct mo_heap *process, uint32_t type, struct mo_cell *list,
                    struct mo_cell element) {
    MO_ASSERT(process != NULL && list != NULL && type < process->types->count);
    MO_ASSERT(process->types->items[type].kind == MO_TYPE_LIST);
    struct mo_allocation *old = mo_heap_allocation(mo_cell_get_ref(*list));
    MO_ASSERT(old->type == type);
    if (old->length == MO_LIMIT_COLLECTION_SIZE) {
        return false;
    }
    struct mo_allocation *allocation = list_room(process, type, old, old->length + 1);
    if (allocation == NULL) {
        return false;
    }
    allocation->cells[allocation->length++] =
        mo_value_dup(process, process->types->items[type].parameters[0], element);
    allocation->hash_valid = false;
    if (allocation != old) {
        mo_value_drop(process, type, *list);
        *list = mo_cell_ref(&allocation->object);
    }
    return true;
}

bool mo_list_set(struct mo_heap *process, uint32_t type, struct mo_cell *list, uint32_t index,
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
    allocation->hash_valid = false;
    if (allocation != old) {
        mo_value_drop(process, type, *list);
        *list = mo_cell_ref(&allocation->object);
    }
    return true;
}
