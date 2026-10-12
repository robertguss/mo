/* Private allocation bookkeeping precedes the bytecode's object header. */
#ifndef MO_HEAP_INTERNAL_H
#define MO_HEAP_INTERNAL_H

#include "heap/heap.h"

#define MO_BUILTIN_LAYOUT(kind) (UINT32_MAX - (uint32_t)(kind))

struct mo_allocation {
    struct mo_allocation *previous;
    struct mo_allocation *next;
    struct mo_allocation *pending;
    size_t size;
    uint32_t type;
    uint32_t length;
    uint32_t capacity;
    uint32_t auxiliary;
    struct mo_object object;
    struct mo_cell cells[];
};
_Static_assert(offsetof(struct mo_allocation, cells) ==
                   offsetof(struct mo_allocation, object) + sizeof(struct mo_object),
               "fields immediately follow the header");

struct mo_process {
    const struct mo_types *types;
    struct mo_allocation *allocations;
    size_t used;
    size_t limit;
    size_t objects;
    uint32_t builtins[MO_TYPE_BIGINT + 1];
};

MO_MUST_USE struct mo_allocation *mo_heap_allocation(struct mo_object *object);
MO_MUST_USE struct mo_allocation *mo_heap_allocate(struct mo_process *process, uint32_t type,
                                                   uint32_t layout, size_t bytes);
void mo_heap_release(struct mo_process *process, struct mo_allocation *allocation);
MO_MUST_USE struct mo_object *mo_value_object(const struct mo_types *types, uint32_t type,
                                              struct mo_cell value);

#endif
