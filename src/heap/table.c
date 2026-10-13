#include "heap/internal.h"

#include <string.h>

struct mo_table_info *mo_table_info(struct mo_allocation *table) {
    return (struct mo_table_info *)((uint8_t *)table + table->size - sizeof(struct mo_table_info));
}

uint32_t mo_table_stride(const struct mo_allocation *table) {
    MO_ASSERT(table->object.layout == MO_BUILTIN_LAYOUT(MO_TYPE_MAP) ||
              table->object.layout == MO_BUILTIN_LAYOUT(MO_TYPE_SET));
    return table->object.layout == MO_BUILTIN_LAYOUT(MO_TYPE_MAP) ? 2 : 1;
}

uint64_t *mo_table_hashes(struct mo_allocation *table) {
    return (uint64_t *)(table->cells +
                        (size_t)mo_table_info(table)->capacity * mo_table_stride(table));
}

uint8_t *mo_table_live(struct mo_allocation *table) {
    return (uint8_t *)(mo_table_hashes(table) + mo_table_info(table)->capacity);
}

uint32_t *mo_table_slots(struct mo_allocation *table) {
    size_t padded = ((size_t)mo_table_info(table)->capacity + 3) & ~(size_t)3;
    return (uint32_t *)(mo_table_live(table) + padded);
}

static MO_MUST_USE uint32_t capacity_for(uint32_t count) {
    uint32_t capacity = 4;
    while (capacity < count) {
        capacity *= 2;
    }
    return capacity;
}

static MO_MUST_USE struct mo_allocation *new_table(struct mo_heap *process, uint32_t type,
                                                   uint32_t capacity, uint32_t slots) {
    enum mo_type_kind kind = process->types->items[type].kind;
    uint32_t stride = kind == MO_TYPE_MAP ? 2 : 1;
    MO_ASSERT(capacity >= 4 && capacity <= 2 * MO_LIMIT_COLLECTION_SIZE && slots >= 2 * capacity);
    size_t data = (size_t)capacity * (stride * sizeof(struct mo_cell) + sizeof(uint64_t) + 1) +
                  (size_t)slots * sizeof(uint32_t);
    data = (data + 7) & ~(size_t)7;
    struct mo_allocation *table = mo_heap_allocate(process, type, MO_BUILTIN_LAYOUT(kind),
                                                   data + sizeof(struct mo_table_info));
    if (table != NULL) {
        *mo_table_info(table) = (struct mo_table_info){.capacity = capacity, .slots = slots};
    }
    return table;
}

/* Only insertion has a probe bound. Existing-key searches terminate at the
 * first empty slot, including when a cluster exceeds the insertion bound. */
static MO_MUST_USE bool insertion_slot(struct mo_allocation *table, uint64_t hash, uint32_t *out) {
    uint32_t mask = mo_table_info(table)->slots - 1;
    uint32_t slot = (uint32_t)hash & mask;
    for (unsigned probes = 0; probes < MO_LIMIT_HEAP_PROBES; probes++) {
        if (mo_table_slots(table)[slot] == 0) {
            *out = slot;
            return true;
        }
        slot = (slot + 1) & mask;
    }
    return false;
}

static void compact(struct mo_allocation *table) {
    uint32_t stride = mo_table_stride(table), next = 0;
    for (uint32_t i = 0; i < table->length; i++) {
        if (mo_table_live(table)[i] != 0) {
            if (next != i) {
                memmove(table->cells + stride * next, table->cells + stride * i,
                        (size_t)stride * sizeof(struct mo_cell));
                mo_table_hashes(table)[next] = mo_table_hashes(table)[i];
            }
            mo_table_live(table)[next++] = 1;
        }
    }
    memset(mo_table_live(table) + next, 0, mo_table_info(table)->capacity - next);
    table->length = next;
    uint32_t slots = mo_table_info(table)->slots;
    memset(mo_table_slots(table), 0, (size_t)slots * sizeof(uint32_t));
    for (uint32_t i = 0; i < next; i++) {
        uint32_t slot = (uint32_t)mo_table_hashes(table)[i] & (slots - 1);
        while (mo_table_slots(table)[slot] != 0) {
            slot = (slot + 1) & (slots - 1);
        }
        mo_table_slots(table)[slot] = i + 1;
    }
    MO_ASSERT(next == mo_table_info(table)->active);
}

static MO_MUST_USE bool lookup(struct mo_heap *process, uint32_t type, struct mo_allocation *table,
                               struct mo_cell key, uint64_t hash, uint32_t *position,
                               uint32_t *slot_out, uint32_t *distance) {
    uint32_t mask = mo_table_info(table)->slots - 1;
    uint32_t slot = (uint32_t)hash & mask, vacant = MO_NO_INDEX;
    for (uint32_t probes = 0; probes <= mask; probes++) {
        uint32_t entry = mo_table_slots(table)[slot];
        if (entry == 0) {
            *position = MO_NO_INDEX;
            *slot_out = vacant == MO_NO_INDEX ? slot : vacant;
            *distance = probes;
            return true;
        }
        if (entry == MO_NO_INDEX) {
            if (vacant == MO_NO_INDEX) {
                vacant = slot;
            }
        } else if (mo_table_hashes(table)[entry - 1] == hash) {
            bool equal;
            if (!mo_value_equal(process, process->types->items[type].parameters[0], key,
                                table->cells[mo_table_stride(table) * (entry - 1)], &equal)) {
                return false;
            }
            if (equal) {
                *position = entry - 1;
                *slot_out = slot;
                *distance = probes;
                return true;
            }
        }
        slot = (slot + 1) & mask;
    }
    MO_UNREACHABLE(); /* At most half the slots hold entries or tombstones. */
}

/* Copy only live entries, optionally excluding the removed key. Build/check
 * the index and a new insertion before duplicating any retained ownership. */
static MO_MUST_USE struct mo_allocation *copy_table(struct mo_heap *process, uint32_t type,
                                                    struct mo_allocation *old, uint32_t capacity,
                                                    uint32_t slots, uint32_t skip, bool duplicate,
                                                    bool inserting, uint64_t hash,
                                                    uint32_t *position, uint32_t *slot_out) {
    uint32_t stride = mo_table_stride(old);
    const struct mo_type *kind = &process->types->items[type];
    for (;;) {
        struct mo_allocation *copy = new_table(process, type, capacity, slots);
        if (copy == NULL) {
            return NULL;
        }
        uint32_t mapped = MO_NO_INDEX, target_slot = 0;
        bool indexed = true;
        for (uint32_t i = 0; i < old->length; i++) {
            if (i == skip || mo_table_live(old)[i] == 0) {
                continue;
            }
            uint32_t next = copy->length++, slot;
            if (i == *position) {
                mapped = next;
            }
            memcpy(copy->cells + stride * next, old->cells + stride * i,
                   (size_t)stride * sizeof(struct mo_cell));
            mo_table_hashes(copy)[next] = mo_table_hashes(old)[i];
            mo_table_live(copy)[next] = 1;
            if (!insertion_slot(copy, mo_table_hashes(copy)[next], &slot)) {
                indexed = false;
                break;
            }
            mo_table_slots(copy)[slot] = next + 1;
            if (i == *position) {
                target_slot = slot;
            }
        }
        if (indexed && inserting && *position == MO_NO_INDEX) {
            indexed = insertion_slot(copy, hash, &target_slot);
        }
        if (!indexed) {
            mo_heap_release(process, copy);
            if (slots == MO_LIMIT_HEAP_INDEX_SLOTS) {
                return NULL;
            }
            slots *= 2;
            continue;
        }
        mo_table_info(copy)->active = copy->length;
        if (duplicate) {
            for (uint32_t i = 0; i < copy->length; i++) {
                for (uint32_t j = 0; j < stride; j++) {
                    copy->cells[stride * i + j] =
                        mo_value_dup(process, kind->parameters[j], copy->cells[stride * i + j]);
                }
            }
        }
        *position = mapped;
        *slot_out = target_slot;
        return copy;
    }
}

uint32_t mo_table_size(struct mo_cell value) {
    struct mo_allocation *table = mo_heap_allocation(mo_cell_get_ref(value));
    return mo_table_info(table)->active;
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

bool mo_table_get(struct mo_heap *process, uint32_t type, struct mo_cell value, struct mo_cell key,
                  bool *found, struct mo_cell *out) {
    MO_ASSERT(process != NULL && found != NULL && out != NULL);
    struct mo_allocation *table = mo_heap_allocation(mo_cell_get_ref(value));
    MO_ASSERT(table->type == type);
    uint64_t hash;
    uint32_t position, slot, distance;
    if (!mo_value_index_hash(process, process->types->items[type].parameters[0], key, &hash) ||
        !lookup(process, type, table, key, hash, &position, &slot, &distance)) {
        return false;
    }
    *found = position != MO_NO_INDEX;
    if (*found) {
        uint32_t stride = mo_table_stride(table);
        *out = table->cells[stride * position + stride - 1];
    }
    return true;
}

bool mo_table_set(struct mo_heap *process, uint32_t type, struct mo_cell *value, struct mo_cell key,
                  struct mo_cell element) {
    MO_ASSERT(process != NULL && value != NULL);
    struct mo_allocation *old = mo_heap_allocation(mo_cell_get_ref(*value));
    MO_ASSERT(old->type == type);
    const struct mo_type *kind = &process->types->items[type];
    uint64_t hash;
    uint32_t position, slot, distance;
    if (!mo_value_index_hash(process, kind->parameters[0], key, &hash) ||
        !lookup(process, type, old, key, hash, &position, &slot, &distance)) {
        return false;
    }
    uint32_t stride = mo_table_stride(old);
    if (position != MO_NO_INDEX && stride == 1) {
        return true;
    }
    struct mo_table_info *info = mo_table_info(old);
    bool inserting = position == MO_NO_INDEX;
    if (inserting && info->active == MO_LIMIT_COLLECTION_SIZE) {
        return false;
    }
    struct mo_allocation *table = old;
    if (old->object.count != 1 ||
        (inserting && (old->length == info->capacity || distance >= MO_LIMIT_HEAP_PROBES))) {
        uint32_t capacity = capacity_for(info->active + (inserting ? 1 : 0));
        /* A nearly full unique table needs slack, not a same-sized copy after
         * every remove/reinsert. Removal compacts after a quarter are dead.
         * One extra capacity tier also keeps churn amortized at the live limit. */
        if (old->object.count == 1 && inserting && old->length == info->capacity &&
            info->active >= info->capacity / 2 && info->capacity <= MO_LIMIT_COLLECTION_SIZE) {
            capacity = 2 * info->capacity;
        }
        uint32_t slots = 2 * capacity;
        if (distance >= MO_LIMIT_HEAP_PROBES && slots <= info->slots) {
            if (info->slots == MO_LIMIT_HEAP_INDEX_SLOTS) {
                return false;
            }
            slots = info->slots * 2;
        }
        table = copy_table(process, type, old, capacity, slots, MO_NO_INDEX, true, true, hash,
                           &position, &slot);
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
        mo_table_info(table)->active++;
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

bool mo_table_remove(struct mo_heap *process, uint32_t type, struct mo_cell *value,
                     struct mo_cell key) {
    MO_ASSERT(process != NULL && value != NULL);
    struct mo_allocation *old = mo_heap_allocation(mo_cell_get_ref(*value));
    MO_ASSERT(old->type == type);
    uint64_t hash;
    uint32_t position, slot, distance;
    const struct mo_type *kind = &process->types->items[type];
    if (!mo_value_index_hash(process, kind->parameters[0], key, &hash) ||
        !lookup(process, type, old, key, hash, &position, &slot, &distance)) {
        return false;
    }
    if (position == MO_NO_INDEX) {
        return true;
    }
    struct mo_table_info *info = mo_table_info(old);
    uint32_t remaining = info->active - 1;
    if (old->object.count != 1 || (info->capacity > 4 && remaining <= info->capacity / 8) ||
        (info->slots > 8 && remaining <= info->slots / 16)) {
        uint32_t capacity = capacity_for(remaining);
        struct mo_allocation *copy = copy_table(process, type, old, capacity, 2 * capacity,
                                                position, true, false, 0, &position, &slot);
        if (copy == NULL) {
            return false;
        }
        mo_value_drop(process, type, *value);
        *value = mo_cell_ref(&copy->object);
        return true;
    }
    uint32_t stride = mo_table_stride(old);
    for (uint32_t i = 0; i < stride; i++) {
        mo_value_drop(process, kind->parameters[i], old->cells[stride * position + i]);
        old->cells[stride * position + i] = mo_cell_unit();
    }
    mo_table_live(old)[position] = 0;
    mo_table_slots(old)[slot] = MO_NO_INDEX;
    info->active--;
    old->hash_valid = false;
    if ((old->length - info->active) * 4 >= old->length) {
        compact(old);
    }
    return true;
}

static MO_MUST_USE bool build_table(struct mo_heap *process, uint32_t type, enum mo_type_kind kind,
                                    const struct mo_cell *keys, const struct mo_cell *values,
                                    uint32_t count, struct mo_cell *out) {
    MO_ASSERT(process != NULL && out != NULL);
    if (type >= process->types->count || process->types->items[type].kind != kind ||
        count > MO_LIMIT_COLLECTION_SIZE || (count != 0 && keys == NULL) ||
        (kind == MO_TYPE_MAP && count != 0 && values == NULL) ||
        (process->abilities[process->types->items[type].parameters[0]] & MO_ABILITY_HASH) == 0) {
        return false;
    }
    struct mo_allocation *table = new_table(process, type, 4, 8);
    if (table == NULL) {
        return false;
    }
    const struct mo_type *definition = &process->types->items[type];
    uint32_t stride = mo_table_stride(table);
    for (uint32_t i = 0; i < count; i++) {
        uint64_t hash;
        uint32_t position, slot, distance;
        if (!mo_value_index_hash(process, definition->parameters[0], keys[i], &hash) ||
            !lookup(process, type, table, keys[i], hash, &position, &slot, &distance)) {
            mo_heap_release(process, table);
            return false;
        }
        if (position == MO_NO_INDEX &&
            (table->length == mo_table_info(table)->capacity || distance >= MO_LIMIT_HEAP_PROBES)) {
            uint32_t capacity = capacity_for(table->length + 1);
            uint32_t slots = 2 * capacity;
            if (distance >= MO_LIMIT_HEAP_PROBES && slots <= mo_table_info(table)->slots) {
                if (mo_table_info(table)->slots == MO_LIMIT_HEAP_INDEX_SLOTS) {
                    mo_heap_release(process, table);
                    return false;
                }
                slots = mo_table_info(table)->slots * 2;
            }
            struct mo_allocation *copy =
                copy_table(process, type, table, capacity, slots, MO_NO_INDEX, false, true, hash,
                           &position, &slot);
            mo_heap_release(process, table);
            if (copy == NULL) {
                return false;
            }
            table = copy;
        }
        if (position == MO_NO_INDEX) {
            position = table->length++;
            table->cells[stride * position] = keys[i];
            mo_table_hashes(table)[position] = hash;
            mo_table_live(table)[position] = 1;
            mo_table_slots(table)[slot] = position + 1;
            mo_table_info(table)->active++;
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
    *out = mo_cell_ref(&table->object);
    return true;
}

bool mo_value_map(struct mo_heap *process, uint32_t type, const struct mo_cell *keys,
                  const struct mo_cell *values, uint32_t count, struct mo_cell *out) {
    return build_table(process, type, MO_TYPE_MAP, keys, values, count, out);
}

bool mo_value_set(struct mo_heap *process, uint32_t type, const struct mo_cell *elements,
                  uint32_t count, struct mo_cell *out) {
    return build_table(process, type, MO_TYPE_SET, elements, NULL, count, out);
}
