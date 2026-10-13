/* Storage and ownership only. src/host supplies operations and handle release. */
#include "heap/internal.h"

#include <string.h>

static MO_MUST_USE struct mo_allocation *state_allocation(struct mo_state *state) {
    struct mo_allocation *a = mo_heap_allocation((struct mo_object *)state);
    MO_ASSERT(a->object.layout == MO_STATE_LAYOUT);
    return a;
}

bool mo_state_new(struct mo_heap *process, const uint32_t *types, const struct mo_cell *values,
                  uint32_t count, struct mo_state **out) {
    MO_ASSERT(process != NULL && out != NULL);
    if (count > MO_LIMIT_FIELDS || (count != 0 && (types == NULL || values == NULL))) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (types[i] >= process->types->count ||
            (process->abilities[types[i]] & MO_ABILITY_EQUAL) == 0) {
            return false;
        }
    }
    struct mo_allocation *a =
        mo_heap_allocate(process, MO_NO_INDEX, MO_STATE_LAYOUT,
                         (size_t)count * (sizeof(struct mo_cell) + sizeof(uint32_t)));
    if (a == NULL) {
        return false;
    }
    a->length = count;
    uint32_t *slot_types = (uint32_t *)(a->cells + count);
    for (uint32_t i = 0; i < count; i++) {
        slot_types[i] = types[i];
        a->cells[i] = mo_value_dup(process, types[i], values[i]);
    }
    *out = (struct mo_state *)&a->object;
    return true;
}

struct mo_state *mo_state_dup(struct mo_state *state) {
    if (state != NULL) {
        struct mo_allocation *a = state_allocation(state);
        MO_ASSERT(a->object.count != 0);
        if (a->object.count != MO_IMMORTAL_COUNT) {
            a->object.count++;
        }
    }
    return state;
}

void mo_state_drop(struct mo_heap *process, struct mo_state *state) {
    if (state != NULL) {
        struct mo_allocation *a = state_allocation(state);
        mo_heap_drop_object(process, &a->object);
    }
}

struct mo_cell mo_state_get(struct mo_state *state, uint32_t index) {
    struct mo_allocation *a = state_allocation(state);
    MO_ASSERT(index < a->length);
    return a->cells[index];
}

void mo_state_set(struct mo_heap *process, struct mo_state *state, uint32_t index,
                  struct mo_cell value) {
    struct mo_allocation *a = state_allocation(state);
    MO_ASSERT(index < a->length);
    const uint32_t *types = (const uint32_t *)(a->cells + a->length);
    struct mo_cell held = mo_value_dup(process, types[index], value);
    mo_value_drop(process, types[index], a->cells[index]);
    a->cells[index] = held;
}

bool mo_value_capability(struct mo_heap *process, uint32_t type,
                         const struct mo_capability_info *info, struct mo_cell *out) {
    MO_ASSERT(process != NULL && info != NULL && out != NULL);
    if (type >= process->types->count || process->types->items[type].kind != MO_TYPE_CAPABILITY ||
        info->operations == NULL ||
        (info->has_prefix && process->builtins[MO_TYPE_TEXT] == MO_NO_INDEX)) {
        return false;
    }
    struct mo_allocation *a = mo_heap_allocate(process, type, MO_BUILTIN_LAYOUT(MO_TYPE_CAPABILITY),
                                               sizeof(struct mo_capability_info));
    if (a == NULL) {
        return false;
    }
    struct mo_capability_info copy = *info;
    copy.state = mo_state_dup(info->state);
    if (copy.has_prefix) {
        copy.prefix = mo_value_dup(process, process->builtins[MO_TYPE_TEXT], info->prefix);
    } else {
        copy.prefix = mo_cell_unit();
    }
    memcpy(a->cells, &copy, sizeof copy);
    *out = mo_cell_ref(&a->object);
    return true;
}

const struct mo_capability_info *mo_capability_get(struct mo_cell value) {
    struct mo_allocation *a = mo_heap_allocation(mo_cell_get_ref(value));
    MO_ASSERT(a->object.layout == MO_BUILTIN_LAYOUT(MO_TYPE_CAPABILITY));
    return (const struct mo_capability_info *)a->cells;
}
