#define _DEFAULT_SOURCE
#include "heap/internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

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

struct mo_layout_lookup *mo_heap_layout(struct mo_heap *process, uint32_t category, uint32_t first,
                                        uint32_t second) {
    struct mo_heap_image *image = process->image;
    if (!mo_heap_image_refresh(image)) {
        return NULL;
    }
    struct mo_layout_lookup key = {.category = category, .first = first, .second = second};
    uint32_t low = 0, high = image->lookup_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        int cmp = compare_lookup(&key, &image->lookup[middle]);
        if (cmp == 0) {
            return &image->lookup[middle];
        }
        if (cmp < 0) {
            high = middle;
        } else {
            low = middle + 1;
        }
    }
    return NULL;
}

struct mo_heap_image *mo_heap_image_new(const struct mo_types *types,
                                        const struct mo_limits *limits) {
    MO_ASSERT(types != NULL && types->finished && limits != NULL);
    errno = ENOSPC;
    size_t baseline = sizeof(struct mo_heap_image) + types->count;
    if (limits->heap_size < baseline) {
        return NULL;
    }
    struct mo_heap_image *image = calloc(1, baseline);
    if (image == NULL) {
        return NULL;
    }
    uint64_t secret[2];
    if (getentropy(secret, sizeof secret) != 0) {
        free(image);
        errno = EIO;
        return NULL;
    }
    errno = ENOSPC;
    for (unsigned i = 0; i < 2; i++) {
        image->hash_key[i] = mo_heap_mix(secret, 0x68617368, i);
        image->index_key[i] = mo_heap_mix(secret, 0x696e646578, i);
    }
    image->types = types;
    image->limit = limits->heap_size;
    image->used = baseline;
    image->abilities = (uint8_t *)(image + 1);
    if (!mo_heap_abilities(image)) {
        free(image);
        return NULL;
    }
    for (uint32_t i = 0; i <= MO_TYPE_BIGINT; i++) {
        image->builtins[i] = MO_NO_INDEX;
    }
    for (uint32_t i = 0; i < types->count; i++) {
        if (types->items[i].kind <= MO_TYPE_BIGINT) {
            image->builtins[types->items[i].kind] = i;
        }
    }
    struct mo_limits constant_limits = *limits;
    constant_limits.heap_size -= image->used;
    image->constants = mo_heap_new(image, &constant_limits);
    if (image->constants == NULL || !mo_heap_image_refresh(image)) {
        int failure = errno;
        mo_heap_image_free(image);
        errno = failure;
        return NULL;
    }
    return image;
}

bool mo_heap_image_refresh(struct mo_heap_image *image) {
    MO_ASSERT(image != NULL);
    const struct mo_types *types = image->types;
    if (image->ready && image->layout_count == types->layout_count) {
        return true;
    }
    if (image->layout_count != types->layout_count) {
        size_t bytes = (size_t)types->layout_count * sizeof(struct mo_layout_lookup);
        size_t old_bytes = (size_t)image->layout_count * sizeof(struct mo_layout_lookup);
        if (bytes > image->limit - image->used - image->constants->used) {
            errno = ENOSPC;
            return false;
        }
        struct mo_layout_lookup *lookup = calloc(1, bytes);
        if (lookup == NULL) {
            return false;
        }
        uint32_t count = 0;
        for (uint32_t i = 0; i < types->layout_count; i++) {
            const struct mo_layout *layout = &types->layouts[i];
            if (layout->kind == MO_LAYOUT_CASE || layout->kind == MO_LAYOUT_CLOSURE ||
                layout->kind == MO_LAYOUT_RECORD || layout->kind == MO_LAYOUT_TUPLE) {
                bool closure = layout->kind == MO_LAYOUT_CLOSURE;
                lookup[count++] =
                    (struct mo_layout_lookup){.category = closure ? 1 : 0,
                                              .first = closure ? layout->function : layout->type,
                                              .second = closure ? 0 : layout->case_index,
                                              .index = i};
            }
        }
        sort_lookup(lookup, count);
        for (uint32_t i = 1; i < count; i++) {
            if (compare_lookup(&lookup[i - 1], &lookup[i]) == 0) {
                free(lookup);
                return false;
            }
        }
        uint32_t previous = 0;
        for (uint32_t i = 0; i < count; i++) {
            while (previous < image->lookup_count &&
                   compare_lookup(&image->lookup[previous], &lookup[i]) < 0) {
                previous++;
            }
            if (previous < image->lookup_count &&
                compare_lookup(&image->lookup[previous], &lookup[i]) == 0) {
                lookup[i].constant = image->lookup[previous].constant;
            }
        }
        free(image->lookup);
        image->lookup = lookup;
        image->lookup_count = count;
        image->used = image->used - old_bytes + bytes;
        image->layout_count = types->layout_count;
        image->constants->limit = image->limit - image->used;
        image->ready = false;
    }
    /* Captureless closures belong to the image, never the first calling heap.
     * Initialize before publication; later builders only read this cache. */
    for (uint32_t i = 0; i < image->lookup_count; i++) {
        struct mo_layout_lookup *entry = &image->lookup[i];
        const struct mo_layout *layout = &types->layouts[entry->index];
        if (layout->kind == MO_LAYOUT_CLOSURE && layout->field_count == 0 &&
            entry->constant == NULL) {
            struct mo_allocation *a =
                mo_heap_allocate(image->constants, layout->type, entry->index, 0);
            if (a == NULL) {
                return false;
            }
            a->object.count = MO_IMMORTAL_COUNT;
            a->deep_immortal = true;
            entry->constant = &a->object;
        }
    }
    image->ready = true;
    return true;
}

void mo_heap_image_free(struct mo_heap_image *image) {
    if (image != NULL) {
        mo_heap_free(image->constants);
        free(image->lookup);
        free(image);
    }
}

struct mo_heap *mo_heap_image_constants(struct mo_heap_image *image) {
    MO_ASSERT(image != NULL);
    return image->constants;
}

struct mo_heap *mo_heap_new(struct mo_heap_image *image, const struct mo_limits *limits) {
    MO_ASSERT(image != NULL && limits != NULL);
    if (limits->heap_size < sizeof(struct mo_heap)) {
        errno = ENOSPC;
        return NULL;
    }
    struct mo_heap *process = calloc(1, sizeof *process);
    if (process == NULL) {
        return NULL;
    }
    process->types = image->types;
    process->image = image;
    process->used = sizeof *process;
    process->limit = limits->heap_size;
    process->hash_key = image->hash_key;
    process->abilities = image->abilities;
    process->builtins = image->builtins;
    return process;
}

size_t mo_heap_used(const struct mo_heap *process) {
    MO_ASSERT(process != NULL);
    return process->used;
}

size_t mo_heap_objects(const struct mo_heap *process) {
    MO_ASSERT(process != NULL);
    return process->objects;
}

struct mo_allocation *mo_heap_allocation(struct mo_object *object) {
    MO_ASSERT(object != NULL);
    return (struct mo_allocation *)((uint8_t *)object - offsetof(struct mo_allocation, object));
}

struct mo_allocation *mo_heap_allocate(struct mo_heap *process, uint32_t type, uint32_t layout,
                                       size_t bytes) {
    MO_ASSERT(process != NULL &&
              (type < process->types->count || (type == MO_NO_INDEX && layout == MO_STATE_LAYOUT)));
    size_t header = offsetof(struct mo_allocation, cells);
    if (bytes > SIZE_MAX - header || header + bytes > process->limit - process->used) {
        errno = ENOSPC;
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

void mo_heap_release(struct mo_heap *process, struct mo_allocation *allocation) {
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
    if (allocation->object.layout == MO_BUILTIN_LAYOUT(MO_TYPE_CAPABILITY)) {
        const struct mo_capability_info *info =
            (const struct mo_capability_info *)allocation->cells;
        if (info->release != NULL) {
            info->release(info->handle);
        }
    }
    /* Poison before returning memory even in a build without sanitizers. */
    void *(*volatile poison)(void *, int, size_t) = memset;
    poison(allocation, 0xa5, allocation->size);
    free(allocation);
}

void mo_heap_free(struct mo_heap *process) {
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

struct mo_cell mo_value_dup(struct mo_heap *process, uint32_t type, struct mo_cell value) {
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

static void enqueue_object(struct mo_object *object, struct mo_allocation **pending) {
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

static void enqueue(const struct mo_types *types, uint32_t type, struct mo_cell value,
                    struct mo_allocation **pending) {
    enqueue_object(mo_value_object(types, type, value), pending);
}

void mo_value_drop(struct mo_heap *process, uint32_t type, struct mo_cell value) {
    MO_ASSERT(process != NULL);
    mo_heap_drop_object(process, mo_value_object(process->types, type, value));
}

void mo_heap_drop_object(struct mo_heap *process, struct mo_object *object) {
    MO_ASSERT(process != NULL);
    struct mo_allocation *pending = NULL;
    enqueue_object(object, &pending);
    /* Zero-count allocations supply the work list; drop never allocates. */
    while (pending != NULL) {
        struct mo_allocation *allocation = pending;
        pending = allocation->pending;
        uint32_t layout = allocation->object.layout;
        if (layout == MO_STATE_LAYOUT) {
            const uint32_t *types = (const uint32_t *)(allocation->cells + allocation->length);
            for (uint32_t i = 0; i < allocation->length; i++) {
                enqueue(process->types, types[i], allocation->cells[i], &pending);
            }
        } else if (layout == MO_BUILTIN_LAYOUT(MO_TYPE_CAPABILITY)) {
            const struct mo_capability_info *info =
                (const struct mo_capability_info *)allocation->cells;
            enqueue_object((struct mo_object *)info->state, &pending);
            if (info->has_prefix) {
                enqueue(process->types, process->builtins[MO_TYPE_TEXT], info->prefix, &pending);
            }
        } else if (layout < process->types->layout_count) {
            const struct mo_layout *fixed = &process->types->layouts[layout];
            for (uint32_t i = 0; i < fixed->field_count; i++) {
                enqueue(process->types, fixed->fields[i], allocation->cells[i], &pending);
            }
        } else {
            const struct mo_type *kind = &process->types->items[allocation->type];
            if (kind->kind == MO_TYPE_LIST) {
                for (uint32_t i = 0; i < allocation->length; i++) {
                    enqueue(process->types, kind->parameters[0],
                            allocation->cells[allocation->start + i], &pending);
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

void mo_heap_confidential(struct mo_heap *process) {
    MO_ASSERT(process != NULL && process->objects == 0);
    process->confidential = true;
}

void mo_scratch_init(struct mo_scratch *scratch, struct mo_heap *process) {
    MO_ASSERT(scratch != NULL && process != NULL);
    size_t available = process->limit - process->used;
    if (process->confidential)
        mo_arena_init_confidential(&scratch->arena, available);
    else
        mo_arena_init(&scratch->arena, available);
    scratch->process = process;
}

void mo_scratch_free(struct mo_scratch *scratch) {
    MO_ASSERT(scratch->process->used >= scratch->arena.used);
    int failure = errno;
    scratch->process->used -= scratch->arena.used;
    mo_arena_free(&scratch->arena);
    errno = failure;
}

void *mo_scratch_resize(struct mo_scratch *scratch, void *bytes, size_t old_size, size_t new_size) {
    size_t before = scratch->arena.used;
    scratch->arena.limit = before + scratch->process->limit - scratch->process->used;
    /* Arena limit refusals don't set errno; POSIX malloc/realloc failures do. */
    errno = ENOSPC;
    void *result = mo_arena_resize(&scratch->arena, bytes, old_size, new_size, 16);
    scratch->process->used = scratch->process->used - before + scratch->arena.used;
    MO_ASSERT(scratch->process->used <= scratch->process->limit);
    return result;
}
