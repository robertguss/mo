#include "heap/internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static MO_MUST_USE bool random_key(uint64_t key[2]) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return false;
    }
    size_t offset = 0;
    while (offset < 2 * sizeof(uint64_t)) {
        ssize_t count = read(fd, (uint8_t *)key + offset, 2 * sizeof(uint64_t) - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            int status = close(fd);
            (void)status;
            return false;
        }
        offset += (size_t)count;
    }
    return close(fd) == 0;
}

static MO_MUST_USE int compare_lookup(const void *left, const void *right) {
    const struct mo_layout_lookup *a = left, *b = right;
    if (a->category != b->category) {
        return a->category < b->category ? -1 : 1;
    }
    if (a->first != b->first) {
        return a->first < b->first ? -1 : 1;
    }
    return (a->second > b->second) - (a->second < b->second);
}

static void sift_lookup(struct mo_layout_lookup *items, uint32_t count, uint32_t root) {
    while (root < count / 2) {
        uint32_t child = 2 * root + 1;
        if (child + 1 < count && compare_lookup(&items[child], &items[child + 1]) < 0) {
            child++;
        }
        if (compare_lookup(&items[root], &items[child]) >= 0) {
            return;
        }
        struct mo_layout_lookup temporary = items[root];
        items[root] = items[child];
        items[child] = temporary;
        root = child;
    }
}

static void sort_lookup(struct mo_layout_lookup *items, uint32_t count) {
    for (uint32_t root = count / 2; root != 0;) {
        sift_lookup(items, count, --root);
    }
    for (uint32_t end = count; end > 1;) {
        struct mo_layout_lookup temporary = items[0];
        items[0] = items[--end];
        items[end] = temporary;
        sift_lookup(items, end, 0);
    }
}

uint32_t mo_heap_layout(const struct mo_process *process, uint32_t category, uint32_t first,
                        uint32_t second) {
    struct mo_layout_lookup key = {.category = category, .first = first, .second = second};
    uint32_t low = 0, high = process->lookup_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        int cmp = compare_lookup(&key, &process->lookup[middle]);
        if (cmp == 0) {
            return process->lookup[middle].index;
        }
        if (cmp < 0) {
            high = middle;
        } else {
            low = middle + 1;
        }
    }
    return MO_NO_INDEX;
}

struct mo_process *mo_heap_new(const struct mo_types *types, const struct mo_limits *limits) {
    MO_ASSERT(types != NULL && types->finished && limits != NULL);
    size_t baseline = sizeof(struct mo_process) +
                      (size_t)types->layout_count * sizeof(struct mo_layout_lookup) + types->count;
    if (limits->heap_size < baseline) {
        return NULL;
    }
    struct mo_process *process = calloc(1, baseline);
    if (process == NULL) {
        return NULL;
    }
    if (!random_key(process->hash_key)) {
        free(process);
        return NULL;
    }
    process->types = types;
    process->used = baseline;
    process->baseline = baseline;
    process->limit = limits->heap_size;
    process->abilities = (uint8_t *)(process->lookup + types->layout_count);
    if (!mo_heap_abilities(process)) {
        free(process);
        return NULL;
    }
    for (uint32_t i = 0; i <= MO_TYPE_BIGINT; i++) {
        process->builtins[i] = MO_NO_INDEX;
    }
    for (uint32_t i = 0; i < types->count; i++) {
        if (types->items[i].kind <= MO_TYPE_BIGINT) {
            process->builtins[types->items[i].kind] = i;
        }
    }
    for (uint32_t i = 0; i < types->layout_count; i++) {
        const struct mo_layout *layout = &types->layouts[i];
        if (layout->kind == MO_LAYOUT_CASE || layout->kind == MO_LAYOUT_CLOSURE) {
            bool closure = layout->kind == MO_LAYOUT_CLOSURE;
            process->lookup[process->lookup_count++] =
                (struct mo_layout_lookup){.category = closure ? 1 : 0,
                                          .first = closure ? layout->function : layout->type,
                                          .second = closure ? 0 : layout->case_index,
                                          .index = i};
        }
    }
    sort_lookup(process->lookup, process->lookup_count);
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
    MO_ASSERT(process->used == process->baseline && process->objects == 0);
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
            if (kind->kind == MO_TYPE_LIST) {
                for (uint32_t i = 0; i < allocation->length; i++) {
                    enqueue(process->types, kind->parameters[0], allocation->cells[i], &pending);
                }
            } else if (kind->kind == MO_TYPE_MAP || kind->kind == MO_TYPE_SET) {
                uint32_t stride = mo_table_stride(allocation);
                for (uint32_t i = 0; i < allocation->length; i++) {
                    if (mo_table_live(allocation)[i] == 0) {
                        continue;
                    }
                    enqueue(process->types, kind->parameters[0], allocation->cells[stride * i],
                            &pending);
                    if (stride == 2) {
                        enqueue(process->types, kind->parameters[1],
                                allocation->cells[stride * i + 1], &pending);
                    }
                }
            }
        }
        mo_heap_release(process, allocation);
    }
}

void mo_scratch_init(struct mo_scratch *scratch, struct mo_process *process) {
    MO_ASSERT(scratch != NULL && process != NULL);
    size_t available = process->limit - process->used;
    mo_arena_init(&scratch->arena,
                  available < MO_LIMIT_HEAP_SCRATCH ? available : MO_LIMIT_HEAP_SCRATCH);
    scratch->process = process;
}

void mo_scratch_free(struct mo_scratch *scratch) {
    MO_ASSERT(scratch->process->used >= scratch->arena.used);
    scratch->process->used -= scratch->arena.used;
    mo_arena_free(&scratch->arena);
}

void *mo_scratch_resize(struct mo_scratch *scratch, void *bytes, size_t old_size, size_t new_size) {
    size_t before = scratch->arena.used;
    void *result =
        mo_arena_resize(&scratch->arena, bytes, old_size, new_size, _Alignof(max_align_t));
    scratch->process->used = scratch->process->used - before + scratch->arena.used;
    MO_ASSERT(scratch->process->used <= scratch->process->limit);
    return result;
}
