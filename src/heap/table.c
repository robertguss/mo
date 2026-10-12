#include "heap/internal.h"

#include <string.h>

uint32_t mo_table_stride(const struct mo_allocation *table) {
    MO_ASSERT(table->object.layout == MO_BUILTIN_LAYOUT(MO_TYPE_MAP) ||
              table->object.layout == MO_BUILTIN_LAYOUT(MO_TYPE_SET));
    return table->object.layout == MO_BUILTIN_LAYOUT(MO_TYPE_MAP) ? 2 : 1;
}

uint64_t *mo_table_hashes(struct mo_allocation *table) {
    return (uint64_t *)(table->cells + (size_t)table->capacity * mo_table_stride(table));
}

uint8_t *mo_table_live(struct mo_allocation *table) {
    return (uint8_t *)(mo_table_hashes(table) + table->capacity);
}

uint32_t *mo_table_slots(struct mo_allocation *table) {
    size_t padded = ((size_t)table->capacity + 3) & ~(size_t)3;
    return (uint32_t *)(mo_table_live(table) + padded);
}

static MO_MUST_USE struct mo_allocation *new_table(struct mo_process *process, uint32_t type,
                                                   uint32_t capacity) {
    enum mo_type_kind kind = process->types->items[type].kind;
    uint32_t stride = kind == MO_TYPE_MAP ? 2 : 1;
    MO_ASSERT(capacity >= 4 && capacity <= MO_LIMIT_COLLECTION_SIZE);
    size_t bytes = (size_t)capacity * (stride * sizeof(struct mo_cell) + sizeof(uint64_t) + 1) +
                   (size_t)capacity * 2 * sizeof(uint32_t);
    struct mo_allocation *table = mo_heap_allocate(process, type, MO_BUILTIN_LAYOUT(kind), bytes);
    if (table != NULL) {
        table->capacity = capacity;
        table->auxiliary = capacity * 2;
        memcpy(table->index_key, process->hash_key, sizeof table->index_key);
    }
    return table;
}

/* Index rebuilding visits entries in original insertion order. Removing entries
 * cannot lengthen a probe sequence relative to their original insertion. */
static void reindex(struct mo_allocation *table) {
    uint32_t *slots = mo_table_slots(table);
    memset(slots, 0, (size_t)table->auxiliary * sizeof *slots);
    for (uint32_t i = 0; i < table->length; i++) {
        if (mo_table_live(table)[i] == 0) {
            continue;
        }
        uint32_t slot = (uint32_t)(mo_table_hashes(table)[i] & (table->auxiliary - 1));
        uint32_t probes = 0;
        while (slots[slot] != 0) {
            MO_ASSERT(++probes < MO_LIMIT_HEAP_PROBES);
            slot = (slot + 1) & (table->auxiliary - 1);
        }
        slots[slot] = i + 1;
    }
}

static void compact(struct mo_allocation *table) {
    uint32_t stride = mo_table_stride(table);
    uint32_t next = 0;
    for (uint32_t i = 0; i < table->length; i++) {
        if (mo_table_live(table)[i] == 0) {
            continue;
        }
        if (next != i) {
            memmove(table->cells + stride * next, table->cells + stride * i,
                    (size_t)stride * sizeof(struct mo_cell));
            mo_table_hashes(table)[next] = mo_table_hashes(table)[i];
        }
        mo_table_live(table)[next++] = 1;
    }
    memset(mo_table_live(table) + next, 0, table->capacity - next);
    table->length = next;
    MO_ASSERT(table->length == table->active);
    reindex(table);
}

static MO_MUST_USE bool lookup(struct mo_process *process, uint32_t type,
                               struct mo_allocation *table, struct mo_cell key, uint64_t hash,
                               uint32_t *position, uint32_t *slot_out) {
    uint32_t slot = (uint32_t)(hash & (table->auxiliary - 1));
    uint32_t vacant = MO_NO_INDEX;
    uint32_t stride = mo_table_stride(table);
    for (uint32_t probes = 0; probes < MO_LIMIT_HEAP_PROBES; probes++) {
        uint32_t entry = mo_table_slots(table)[slot];
        if (entry == 0) {
            *position = MO_NO_INDEX;
            *slot_out = vacant == MO_NO_INDEX ? slot : vacant;
            return true;
        }
        if (entry == MO_NO_INDEX) {
            if (vacant == MO_NO_INDEX) {
                vacant = slot;
            }
        } else if (mo_table_hashes(table)[entry - 1] == hash) {
            bool equal;
            if (!mo_value_equal(process, process->types->items[type].parameters[0], key,
                                table->cells[stride * (entry - 1)], &equal)) {
                return false;
            }
            if (equal) {
                *position = entry - 1;
                *slot_out = slot;
                return true;
            }
        }
        slot = (slot + 1) & (table->auxiliary - 1);
    }
    return false;
}

static MO_MUST_USE struct mo_allocation *copy_table(struct mo_process *process, uint32_t type,
                                                    struct mo_allocation *old, uint32_t capacity,
                                                    uint64_t hash, uint32_t position,
                                                    uint32_t *slot_out) {
    struct mo_allocation *copy = new_table(process, type, capacity);
    if (copy == NULL) {
        return NULL;
    }
    uint32_t stride = mo_table_stride(old);
    const struct mo_type *kind = &process->types->items[type];
    /* Keep positions until lookup completes; compact at the next update. */
    copy->length = old->length;
    copy->active = old->active;
    memcpy(copy->index_key, old->index_key, sizeof copy->index_key);
    for (uint32_t i = 0; i < old->length; i++) {
        if (mo_table_live(old)[i] == 0) {
            continue;
        }
        for (uint32_t j = 0; j < stride; j++) {
            copy->cells[stride * i + j] = old->cells[stride * i + j];
        }
        mo_table_hashes(copy)[i] = mo_table_hashes(old)[i];
        mo_table_live(copy)[i] = 1;
    }
    /* A larger table changes the mask: rebuild with checked probing. */
    for (uint32_t i = 0; i < copy->length; i++) {
        if (mo_table_live(copy)[i] == 0) {
            continue;
        }
        uint32_t slot = (uint32_t)(mo_table_hashes(copy)[i] & (copy->auxiliary - 1));
        uint32_t probes = 0;
        while (mo_table_slots(copy)[slot] != 0 && probes < MO_LIMIT_HEAP_PROBES) {
            probes++;
            slot = (slot + 1) & (copy->auxiliary - 1);
        }
        if (probes == MO_LIMIT_HEAP_PROBES) {
            mo_heap_release(process, copy);
            return NULL;
        }
        mo_table_slots(copy)[slot] = i + 1;
    }
    uint32_t slot = (uint32_t)(hash & (copy->auxiliary - 1));
    uint32_t probes = 0;
    while (mo_table_slots(copy)[slot] != (position == MO_NO_INDEX ? 0 : position + 1)) {
        if (++probes == MO_LIMIT_HEAP_PROBES) {
            mo_heap_release(process, copy);
            return NULL;
        }
        slot = (slot + 1) & (copy->auxiliary - 1);
    }
    *slot_out = slot;
    /* Nothing can fail after references are duplicated, including saturation. */
    for (uint32_t i = 0; i < copy->length; i++) {
        if (mo_table_live(copy)[i] != 0) {
            for (uint32_t j = 0; j < stride; j++) {
                copy->cells[stride * i + j] =
                    mo_value_dup(process, kind->parameters[j], copy->cells[stride * i + j]);
            }
        }
    }
    return copy;
}

uint32_t mo_table_size(struct mo_cell value) {
    struct mo_allocation *table = mo_heap_allocation(mo_cell_get_ref(value));
    uint32_t stride = mo_table_stride(table);
    (void)stride;
    return table->active;
}

bool mo_table_next(struct mo_cell value, uint32_t *position, struct mo_cell *key,
                   struct mo_cell *element) {
    MO_ASSERT(position != NULL && key != NULL && element != NULL);
    struct mo_allocation *table = mo_heap_allocation(mo_cell_get_ref(value));
    uint32_t stride = mo_table_stride(table);
    while (*position < table->length) {
        uint32_t i = (*position)++;
        if (mo_table_live(table)[i] != 0) {
            *key = table->cells[stride * i];
            *element = stride == 2 ? table->cells[stride * i + 1] : *key;
            return true;
        }
    }
    return false;
}

bool mo_table_get(struct mo_process *process, uint32_t type, struct mo_cell value,
                  struct mo_cell key, bool *found, struct mo_cell *out) {
    MO_ASSERT(process != NULL && found != NULL && out != NULL);
    struct mo_allocation *table = mo_heap_allocation(mo_cell_get_ref(value));
    MO_ASSERT(table->type == type);
    uint64_t hash;
    uint32_t position, slot;
    if (!mo_value_hash_key(process, process->types->items[type].parameters[0], key,
                           table->index_key, &hash) ||
        !lookup(process, type, table, key, hash, &position, &slot)) {
        return false;
    }
    *found = position != MO_NO_INDEX;
    if (*found) {
        uint32_t stride = mo_table_stride(table);
        *out = table->cells[stride * position + stride - 1];
    }
    return true;
}

bool mo_table_set(struct mo_process *process, uint32_t type, struct mo_cell *value,
                  struct mo_cell key, struct mo_cell element) {
    MO_ASSERT(process != NULL && value != NULL);
    struct mo_allocation *old = mo_heap_allocation(mo_cell_get_ref(*value));
    MO_ASSERT(old->type == type);
    const struct mo_type *kind = &process->types->items[type];
    uint64_t hash;
    uint32_t position, slot;
    if (!mo_value_hash_key(process, kind->parameters[0], key, old->index_key, &hash) ||
        !lookup(process, type, old, key, hash, &position, &slot)) {
        return false;
    }
    uint32_t stride = mo_table_stride(old);
    if (position != MO_NO_INDEX && stride == 1) {
        return true;
    }
    bool grow = position == MO_NO_INDEX && old->length == old->capacity;
    uint32_t capacity = old->capacity;
    if (grow) {
        if (capacity == MO_LIMIT_COLLECTION_SIZE) {
            return false;
        }
        capacity *= 2;
    }
    struct mo_allocation *table = old;
    if (old->object.count != 1 || grow) {
        table = copy_table(process, type, old, capacity, hash, position, &slot);
        if (table == NULL) {
            return false;
        }
    }
    if (position == MO_NO_INDEX) {
        position = table->length++;
        table->cells[stride * position] = mo_value_dup(process, kind->parameters[0], key);
        mo_table_hashes(table)[position] = hash;
        mo_table_live(table)[position] = 1;
        mo_table_slots(table)[slot] = position + 1;
        table->active++;
        if (stride == 2) {
            table->cells[stride * position + 1] =
                mo_value_dup(process, kind->parameters[1], element);
        }
    } else {
        struct mo_cell held = mo_value_dup(process, kind->parameters[1], element);
        mo_value_drop(process, kind->parameters[1], table->cells[stride * position + 1]);
        table->cells[stride * position + 1] = held;
    }
    table->hash_valid = false;
    if (table != old) {
        mo_value_drop(process, type, *value);
        *value = mo_cell_ref(&table->object);
    }
    return true;
}

bool mo_table_remove(struct mo_process *process, uint32_t type, struct mo_cell *value,
                     struct mo_cell key) {
    MO_ASSERT(process != NULL && value != NULL);
    struct mo_allocation *old = mo_heap_allocation(mo_cell_get_ref(*value));
    MO_ASSERT(old->type == type);
    uint64_t hash;
    uint32_t position, slot;
    const struct mo_type *kind = &process->types->items[type];
    if (!mo_value_hash_key(process, kind->parameters[0], key, old->index_key, &hash) ||
        !lookup(process, type, old, key, hash, &position, &slot)) {
        return false;
    }
    if (position == MO_NO_INDEX) {
        return true;
    }
    struct mo_allocation *table = old;
    if (old->object.count != 1) {
        table = copy_table(process, type, old, old->capacity, hash, position, &slot);
        if (table == NULL) {
            return false;
        }
    }
    uint32_t stride = mo_table_stride(table);
    for (uint32_t i = 0; i < stride; i++) {
        mo_value_drop(process, kind->parameters[i], table->cells[stride * position + i]);
        table->cells[stride * position + i] = mo_cell_unit();
    }
    mo_table_live(table)[position] = 0;
    mo_table_slots(table)[slot] = MO_NO_INDEX;
    table->active--;
    table->hash_valid = false;
    if ((table->length - table->active) * 4 >= table->length) {
        compact(table);
    }
    if (table != old) {
        mo_value_drop(process, type, *value);
        *value = mo_cell_ref(&table->object);
    }
    return true;
}

static MO_MUST_USE bool build_table(struct mo_process *process, uint32_t type,
                                    enum mo_type_kind kind, const struct mo_cell *keys,
                                    const struct mo_cell *values, uint32_t count,
                                    struct mo_cell *out) {
    MO_ASSERT(process != NULL && out != NULL);
    if (type >= process->types->count || process->types->items[type].kind != kind ||
        count > MO_LIMIT_COLLECTION_SIZE || (count != 0 && keys == NULL) ||
        (kind == MO_TYPE_MAP && count != 0 && values == NULL)) {
        return false;
    }
    if ((process->abilities[process->types->items[type].parameters[0]] & MO_ABILITY_HASH) == 0) {
        return false;
    }
    uint32_t capacity = 4;
    while (capacity < count) {
        capacity *= 2;
    }
    struct mo_allocation *table = new_table(process, type, capacity);
    if (table == NULL) {
        return false;
    }
    struct mo_cell result = mo_cell_ref(&table->object);
    const struct mo_type *definition = &process->types->items[type];
    uint32_t stride = mo_table_stride(table);
    for (uint32_t i = 0; i < count; i++) {
        uint64_t hash;
        uint32_t position, slot;
        if (!mo_value_hash_key(process, definition->parameters[0], keys[i], table->index_key,
                               &hash) ||
            !lookup(process, type, table, keys[i], hash, &position, &slot)) {
            mo_heap_release(process, table);
            return false;
        }
        if (position == MO_NO_INDEX) {
            position = table->length++;
            table->cells[stride * position] = keys[i];
            mo_table_hashes(table)[position] = hash;
            mo_table_live(table)[position] = 1;
            mo_table_slots(table)[slot] = position + 1;
            table->active++;
        }
        if (stride == 2) {
            table->cells[stride * position + 1] = values[i];
        }
    }
    for (uint32_t i = 0; i < table->length; i++) {
        for (uint32_t j = 0; j < stride; j++) {
            table->cells[stride * i + j] =
                mo_value_dup(process, definition->parameters[j], table->cells[stride * i + j]);
        }
    }
    *out = result;
    return true;
}

bool mo_value_map(struct mo_process *process, uint32_t type, const struct mo_cell *keys,
                  const struct mo_cell *values, uint32_t count, struct mo_cell *out) {
    return build_table(process, type, MO_TYPE_MAP, keys, values, count, out);
}

bool mo_value_set(struct mo_process *process, uint32_t type, const struct mo_cell *elements,
                  uint32_t count, struct mo_cell *out) {
    return build_table(process, type, MO_TYPE_SET, elements, NULL, count, out);
}
