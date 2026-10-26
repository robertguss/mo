#include "heap/internal.h"

#include <errno.h>
#include <string.h>

#include "base/utf8.h"

static MO_MUST_USE bool byte_value(struct mo_heap *process, enum mo_type_kind kind,
                                   const uint8_t *bytes, size_t length, uint32_t auxiliary,
                                   struct mo_cell *out) {
    MO_ASSERT(process != NULL && out != NULL);
    if (length > MO_LIMIT_VALUE_BYTES || (length != 0 && bytes == NULL) ||
        process->builtins[kind] == MO_NO_INDEX) {
        return false;
    }
    if (kind == MO_TYPE_TEXT && !mo_utf8_valid(bytes, length)) {
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

/* Shared copies have no reason to inherit slack. Unique growth reserves slack
 * from the requested size, using part of the available heap if doubling will
 * not fit. A native slack-allocation refusal gets one exact-size retry. */
static MO_MUST_USE struct mo_allocation *grow_room(struct mo_heap *process,
                                                   struct mo_allocation *old, size_t needed,
                                                   size_t maximum, size_t minimum, size_t unit) {
    MO_ASSERT(unit != 0 && needed <= maximum && needed % unit == 0);
    size_t room = needed;
    if (old->object.count == 1) {
        room = needed > maximum / 2 ? maximum : 2 * needed;
        if (room < minimum)
            room = minimum;
        size_t available = process->limit - process->used;
        size_t header = offsetof(struct mo_allocation, cells);
        available = available > header ? available - header : 0;
        available -= available % unit;
        if (room > available && needed <= available)
            room = needed + (available - needed) / 2;
        room -= room % unit;
    }
    struct mo_allocation *result = mo_heap_allocate(process, old->type, old->object.layout, room);
    if (result == NULL && errno == ENOMEM && room != needed)
        result = mo_heap_allocate(process, old->type, old->object.layout, needed);
    return result;
}

/* Contraction is best effort. Call after removing discarded values, so their
 * freed heap can help. A failed exact allocation leaves the unique buffer. */
static MO_MUST_USE struct mo_allocation *shrink_room(struct mo_heap *process,
                                                     struct mo_allocation *old, size_t needed) {
    MO_ASSERT(old->object.count == 1);
    size_t capacity = old->size - offsetof(struct mo_allocation, cells);
    if (capacity != 0 && needed <= (capacity - 1) / 4) {
        struct mo_allocation *result =
            mo_heap_allocate(process, old->type, old->object.layout, needed);
        if (result != NULL)
            return result;
    }
    return old;
}

bool mo_text_concat(struct mo_heap *process, struct mo_cell *text, struct mo_cell right) {
    MO_ASSERT(process != NULL && text != NULL);
    struct mo_allocation *old = mo_heap_allocation(mo_cell_get_ref(*text));
    struct mo_allocation *other = mo_heap_allocation(mo_cell_get_ref(right));
    MO_ASSERT(old->object.layout == MO_BUILTIN_LAYOUT(MO_TYPE_TEXT) &&
              other->object.layout == MO_BUILTIN_LAYOUT(MO_TYPE_TEXT));
    if (other->length == 0)
        return true;
    if (other->length > MO_LIMIT_VALUE_BYTES - old->length)
        return false;
    size_t needed = (size_t)old->length + other->length;
    size_t capacity = old->size - offsetof(struct mo_allocation, cells);
    struct mo_allocation *result = old;
    if (old->object.count != 1 || needed > capacity) {
        result = grow_room(process, old, needed, MO_LIMIT_VALUE_BYTES, 32, 1);
        if (result == NULL)
            return false;
        memcpy(result->cells, old->cells, old->length);
    }
    memcpy((uint8_t *)result->cells + old->length, other->cells, other->length);
    result->length = (uint32_t)needed;
    result->hash_valid = false;
    if (result != old) {
        mo_value_drop(process, old->type, *text);
        *text = mo_cell_ref(&result->object);
    }
    return true;
}

bool mo_text_slice(struct mo_heap *process, struct mo_cell *text, size_t start, size_t length) {
    MO_ASSERT(process != NULL && text != NULL);
    struct mo_allocation *old = mo_heap_allocation(mo_cell_get_ref(*text));
    MO_ASSERT(old->object.layout == MO_BUILTIN_LAYOUT(MO_TYPE_TEXT) && start <= old->length &&
              length <= old->length - start);
    const uint8_t *bytes = (const uint8_t *)old->cells;
    MO_ASSERT((start == old->length || (bytes[start] & 0xc0) != 0x80) &&
              (start + length == old->length || (bytes[start + length] & 0xc0) != 0x80));
    if (length == old->length)
        return true;
    struct mo_allocation *result = old;
    if (old->object.count != 1) {
        result = mo_heap_allocate(process, old->type, old->object.layout, length);
        if (result == NULL)
            return false;
    }
    memmove(result->cells, bytes + start, length);
    if (result == old) {
        result = shrink_room(process, old, length);
        if (result != old)
            memcpy(result->cells, old->cells, length);
    }
    result->length = (uint32_t)length;
    result->hash_valid = false;
    if (result != old) {
        mo_value_drop(process, old->type, *text);
        *text = mo_cell_ref(&result->object);
    }
    return true;
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
    *out = allocation->cells[allocation->start + index];
    return true;
}

static void list_compact(struct mo_allocation *list) {
    MO_ASSERT(list->object.count == 1);
    memmove(list->cells, list->cells + list->start, (size_t)list->length * sizeof(struct mo_cell));
    list->start = 0;
}

/* Move retained ownership only when contraction succeeds; a refusal keeps
 * the window. Periodic compaction charges its work to the discarded prefix. */
static MO_MUST_USE struct mo_allocation *list_reclaim(struct mo_heap *process,
                                                      struct mo_allocation *old) {
    uint32_t length = old->length;
    struct mo_allocation *result =
        shrink_room(process, old, (size_t)length * sizeof(struct mo_cell));
    if (result != old) {
        memcpy(result->cells, old->cells + old->start, (size_t)length * sizeof(struct mo_cell));
        result->length = length;
        old->length = 0;
        old->start = 0;
    } else {
        size_t capacity =
            (old->size - offsetof(struct mo_allocation, cells)) / sizeof(struct mo_cell);
        if (old->start > capacity / 2)
            list_compact(old);
        if (length == 0)
            old->start = 0;
    }
    return result;
}

static MO_MUST_USE struct mo_allocation *list_room(struct mo_heap *process, uint32_t type,
                                                   struct mo_allocation *old, uint32_t needed) {
    uint32_t capacity =
        (uint32_t)((old->size - offsetof(struct mo_allocation, cells)) / sizeof(struct mo_cell));
    MO_ASSERT(old->start <= capacity && old->length <= capacity - old->start);
    if (old->object.count == 1) {
        if (needed <= capacity - old->start)
            return old;
        if (old->start >= capacity / 2 && needed <= capacity) {
            list_compact(old);
            return old;
        }
    }
    struct mo_allocation *copy =
        grow_room(process, old, (size_t)needed * sizeof(struct mo_cell),
                  (size_t)MO_LIMIT_COLLECTION_SIZE * sizeof(struct mo_cell),
                  4 * sizeof(struct mo_cell), sizeof(struct mo_cell));
    if (copy == NULL) {
        return NULL;
    }
    copy->length = old->length;
    uint32_t element_type = process->types->items[type].parameters[0];
    for (uint32_t i = 0; i < old->length; i++) {
        copy->cells[i] = mo_value_dup(process, element_type, old->cells[old->start + i]);
    }
    return copy;
}

static void list_commit(struct mo_heap *process, uint32_t type, struct mo_cell *list,
                        struct mo_allocation *old, struct mo_allocation *result) {
    result->hash_valid = false;
    if (result != old) {
        mo_value_drop(process, type, *list);
        *list = mo_cell_ref(&result->object);
    }
}

bool mo_list_prepend(struct mo_heap *process, uint32_t type, struct mo_cell *list,
                     struct mo_cell element) {
    MO_ASSERT(process != NULL && list != NULL && type < process->types->count);
    struct mo_allocation *old = mo_heap_allocation(mo_cell_get_ref(*list));
    MO_ASSERT(old->type == type && old->object.layout == MO_BUILTIN_LAYOUT(MO_TYPE_LIST));
    if (old->length == MO_LIMIT_COLLECTION_SIZE)
        return false;
    if (old->object.count == 1 && old->start != 0) {
        old->cells[--old->start] =
            mo_value_dup(process, process->types->items[type].parameters[0], element);
        old->length++;
        old->hash_valid = false;
        return true;
    }
    struct mo_allocation *result = list_room(process, type, old, old->length + 1);
    if (result == NULL)
        return false;
    struct mo_cell *cells = result->cells + result->start;
    memmove(cells + 1, cells, (size_t)result->length * sizeof(struct mo_cell));
    cells[0] = mo_value_dup(process, process->types->items[type].parameters[0], element);
    result->length++;
    list_commit(process, type, list, old, result);
    return true;
}

bool mo_list_concat(struct mo_heap *process, uint32_t type, struct mo_cell *list,
                    struct mo_cell right) {
    MO_ASSERT(process != NULL && list != NULL && type < process->types->count);
    struct mo_allocation *old = mo_heap_allocation(mo_cell_get_ref(*list));
    struct mo_allocation *other = mo_heap_allocation(mo_cell_get_ref(right));
    MO_ASSERT(old->type == type && other->type == type &&
              old->object.layout == MO_BUILTIN_LAYOUT(MO_TYPE_LIST));
    if (other->length == 0)
        return true;
    if (other->length > MO_LIMIT_COLLECTION_SIZE - old->length)
        return false;
    uint32_t length = old->length + other->length;
    struct mo_allocation *result = list_room(process, type, old, length);
    if (result == NULL)
        return false;
    uint32_t element_type = process->types->items[type].parameters[0];
    /* Keep both lengths fixed until all elements are copied: right may be left. */
    for (uint32_t i = 0; i < other->length; i++)
        result->cells[result->start + old->length + i] =
            mo_value_dup(process, element_type, other->cells[other->start + i]);
    result->length = length;
    list_commit(process, type, list, old, result);
    return true;
}

bool mo_list_slice(struct mo_heap *process, uint32_t type, struct mo_cell *list, uint32_t start,
                   uint32_t count) {
    MO_ASSERT(process != NULL && list != NULL && type < process->types->count);
    struct mo_allocation *old = mo_heap_allocation(mo_cell_get_ref(*list));
    MO_ASSERT(old->type == type && old->object.layout == MO_BUILTIN_LAYOUT(MO_TYPE_LIST) &&
              start <= old->length && count <= old->length - start);
    if (count == old->length)
        return true;
    uint32_t element_type = process->types->items[type].parameters[0];
    struct mo_allocation *result = old;
    if (old->object.count != 1) {
        result = mo_heap_allocate(process, type, old->object.layout,
                                  (size_t)count * sizeof(struct mo_cell));
        if (result == NULL)
            return false;
        for (uint32_t i = 0; i < count; i++)
            result->cells[i] =
                mo_value_dup(process, element_type, old->cells[old->start + start + i]);
    } else {
        struct mo_cell *cells = old->cells + old->start;
        if (mo_type_is_reference(process->types, element_type)) {
            for (uint32_t i = 0; i < start; i++) {
                mo_value_drop(process, element_type, cells[i]);
                cells[i] = mo_cell_unit();
            }
            for (uint32_t i = start + count; i < old->length; i++) {
                mo_value_drop(process, element_type, cells[i]);
                cells[i] = mo_cell_unit();
            }
        }
        old->start += start;
        old->length = count;
        result = list_reclaim(process, old);
    }
    result->length = count;
    list_commit(process, type, list, old, result);
    return true;
}

bool mo_list_reverse(struct mo_heap *process, uint32_t type, struct mo_cell *list) {
    MO_ASSERT(process != NULL && list != NULL && type < process->types->count);
    struct mo_allocation *old = mo_heap_allocation(mo_cell_get_ref(*list));
    MO_ASSERT(old->type == type && old->object.layout == MO_BUILTIN_LAYOUT(MO_TYPE_LIST));
    if (old->length < 2)
        return true;
    struct mo_allocation *result = list_room(process, type, old, old->length);
    if (result == NULL)
        return false;
    struct mo_cell *cells = result->cells + result->start;
    for (uint32_t i = 0; i < result->length / 2; i++) {
        uint32_t j = result->length - i - 1;
        struct mo_cell temporary = cells[i];
        cells[i] = cells[j];
        cells[j] = temporary;
    }
    list_commit(process, type, list, old, result);
    return true;
}

bool mo_list_remove_at(struct mo_heap *process, uint32_t type, struct mo_cell *list,
                       uint32_t index) {
    MO_ASSERT(process != NULL && list != NULL && type < process->types->count);
    struct mo_allocation *old = mo_heap_allocation(mo_cell_get_ref(*list));
    MO_ASSERT(old->type == type && old->object.layout == MO_BUILTIN_LAYOUT(MO_TYPE_LIST) &&
              index < old->length);
    if (index == 0)
        return mo_list_slice(process, type, list, 1, old->length - 1);
    uint32_t length = old->length - 1;
    uint32_t element_type = process->types->items[type].parameters[0];
    struct mo_allocation *result = old;
    if (old->object.count != 1) {
        result = mo_heap_allocate(process, type, old->object.layout,
                                  (size_t)length * sizeof(struct mo_cell));
        if (result == NULL)
            return false;
        for (uint32_t i = 0; i < length; i++)
            result->cells[i] = mo_value_dup(process, element_type,
                                            old->cells[old->start + (i < index ? i : i + 1)]);
    } else {
        struct mo_cell *cells = old->cells + old->start;
        mo_value_drop(process, element_type, cells[index]);
        memmove(cells + index, cells + index + 1,
                (size_t)(length - index) * sizeof(struct mo_cell));
        cells[length] = mo_cell_unit();
        old->length = length;
        result = list_reclaim(process, old);
    }
    result->length = length;
    list_commit(process, type, list, old, result);
    return true;
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
    allocation->cells[allocation->start + allocation->length++] =
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
    mo_value_drop(process, element_type, allocation->cells[allocation->start + index]);
    allocation->cells[allocation->start + index] = held;
    allocation->hash_valid = false;
    if (allocation != old) {
        mo_value_drop(process, type, *list);
        *list = mo_cell_ref(&allocation->object);
    }
    return true;
}
