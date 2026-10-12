#include "heap/internal.h"

#include <stdlib.h>
#include <string.h>

struct mo_process *mo_heap_new(const struct mo_types *types, const struct mo_limits *limits) {
    MO_ASSERT(types != NULL && types->finished && limits != NULL);
    if (limits->heap_size < sizeof(struct mo_process)) {
        return NULL;
    }
    struct mo_process *process = calloc(1, sizeof *process);
    if (process == NULL) {
        return NULL;
    }
    process->types = types;
    process->used = sizeof *process;
    process->limit = limits->heap_size;
    for (uint32_t i = 0; i <= MO_TYPE_BIGINT; i++) {
        process->builtins[i] = MO_NO_INDEX;
    }
    for (uint32_t i = 0; i < types->count; i++) {
        if (types->items[i].kind <= MO_TYPE_BIGINT) {
            process->builtins[types->items[i].kind] = i;
        }
    }
    return process;
}

size_t mo_heap_used(const struct mo_process *process) {
    MO_ASSERT(process != NULL);
    return process->used;
}

size_t mo_heap_objects(const struct mo_process *process) {
    MO_ASSERT(process != NULL);
    return process->objects;
}

struct mo_allocation *mo_heap_allocation(struct mo_object *object) {
    MO_ASSERT(object != NULL);
    return (struct mo_allocation *)((uint8_t *)object - offsetof(struct mo_allocation, object));
}

struct mo_allocation *mo_heap_allocate(struct mo_process *process, uint32_t type, uint32_t layout,
                                       size_t bytes) {
    MO_ASSERT(process != NULL && type < process->types->count);
    size_t header = offsetof(struct mo_allocation, cells);
    if (bytes > SIZE_MAX - header || header + bytes > process->limit - process->used) {
        return NULL;
    }
    size_t size = header + bytes;
    struct mo_allocation *allocation = calloc(1, size);
    if (allocation == NULL) {
        return NULL;
    }
    allocation->size = size;
    allocation->type = type;
    allocation->object = (struct mo_object){.count = 1, .layout = layout};
    allocation->next = process->allocations;
    if (allocation->next != NULL) {
        allocation->next->previous = allocation;
    }
    process->allocations = allocation;
    process->used += size;
    process->objects++;
    return allocation;
}

void mo_heap_release(struct mo_process *process, struct mo_allocation *allocation) {
    MO_ASSERT(process != NULL && allocation != NULL && process->objects != 0);
    if (allocation->previous != NULL) {
        allocation->previous->next = allocation->next;
    } else {
        MO_ASSERT(process->allocations == allocation);
        process->allocations = allocation->next;
    }
    if (allocation->next != NULL) {
        allocation->next->previous = allocation->previous;
    }
    MO_ASSERT(process->used >= allocation->size);
    process->used -= allocation->size;
    process->objects--;
    /* Poison before returning memory even in a build without sanitizers. */
    memset(allocation, 0xa5, allocation->size);
    free(allocation);
}

void mo_heap_free(struct mo_process *process) {
    if (process == NULL) {
        return;
    }
    while (process->allocations != NULL) {
        mo_heap_release(process, process->allocations);
    }
    MO_ASSERT(process->used == sizeof *process && process->objects == 0);
    free(process);
}

struct mo_object *mo_value_object(const struct mo_types *types, uint32_t type,
                                  struct mo_cell value) {
    MO_ASSERT(types != NULL && types->finished && type < types->count);
    if (types->items[type].kind == MO_TYPE_CHOICE) {
        enum mo_case_shape shape = mo_case_shape(types, type, mo_cell_get_tag(value));
        if (shape == MO_CASE_REF || shape == MO_CASE_BOXED) {
            return mo_cell_get_ref(mo_cell_get_payload(value));
        }
        return NULL;
    }
    return mo_type_is_reference(types, type) ? mo_cell_get_ref(value) : NULL;
}

struct mo_cell mo_value_dup(struct mo_process *process, uint32_t type, struct mo_cell value) {
    MO_ASSERT(process != NULL);
    struct mo_object *object = mo_value_object(process->types, type, value);
    if (object != NULL) {
        MO_ASSERT(object->count != 0);
        if (object->count != MO_IMMORTAL_COUNT) {
            object->count++;
        }
    }
    return value;
}

static void enqueue(const struct mo_types *types, uint32_t type, struct mo_cell value,
                    struct mo_allocation **pending) {
    struct mo_object *object = mo_value_object(types, type, value);
    if (object == NULL || object->count == MO_IMMORTAL_COUNT) {
        return;
    }
    MO_ASSERT(object->count != 0);
    object->count--;
    if (object->count == 0) {
        struct mo_allocation *allocation = mo_heap_allocation(object);
        allocation->pending = *pending;
        *pending = allocation;
    }
}

void mo_value_drop(struct mo_process *process, uint32_t type, struct mo_cell value) {
    MO_ASSERT(process != NULL);
    struct mo_allocation *pending = NULL;
    enqueue(process->types, type, value, &pending);
    /* Zero-count allocations supply the work list; drop never allocates. */
    while (pending != NULL) {
        struct mo_allocation *allocation = pending;
        pending = allocation->pending;
        uint32_t layout = allocation->object.layout;
        if (layout < process->types->layout_count) {
            const struct mo_layout *fixed = &process->types->layouts[layout];
            for (uint32_t i = 0; i < fixed->field_count; i++) {
                enqueue(process->types, fixed->fields[i], allocation->cells[i], &pending);
            }
        } else {
            const struct mo_type *kind = &process->types->items[allocation->type];
            if (kind->kind == MO_TYPE_LIST || kind->kind == MO_TYPE_SET) {
                for (uint32_t i = 0; i < allocation->length; i++) {
                    enqueue(process->types, kind->parameters[0], allocation->cells[i], &pending);
                }
            } else if (kind->kind == MO_TYPE_MAP) {
                for (uint32_t i = 0; i < allocation->length; i++) {
                    enqueue(process->types, kind->parameters[0], allocation->cells[2 * i],
                            &pending);
                    enqueue(process->types, kind->parameters[1], allocation->cells[2 * i + 1],
                            &pending);
                }
            }
        }
        mo_heap_release(process, allocation);
    }
}
