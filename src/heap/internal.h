/* Private allocation bookkeeping precedes the bytecode's object header. */
#ifndef MO_HEAP_INTERNAL_H
#define MO_HEAP_INTERNAL_H

#include "heap/heap.h"

#define MO_BUILTIN_LAYOUT(kind) (UINT32_MAX - (uint32_t)(kind))
#define MO_ABILITY_EQUAL 1
#define MO_ABILITY_HASH 2
#define MO_ABILITY_ORDER 4

struct mo_allocation {
    struct mo_allocation *previous;
    struct mo_allocation *next;
    struct mo_allocation *pending;
    size_t size;
    uint64_t hash;
    uint64_t hash_key[2];
    uint64_t index_key[2];
    uint32_t type;
    uint32_t length;
    uint32_t capacity;
    uint32_t auxiliary;
    uint32_t active;
    bool hash_valid;
    uint8_t reserved[3];
    struct mo_object object;
    struct mo_cell cells[];
};
_Static_assert(offsetof(struct mo_allocation, cells) ==
                   offsetof(struct mo_allocation, object) + sizeof(struct mo_object),
               "fields immediately follow the header");

struct mo_layout_lookup {
    uint32_t category;
    uint32_t first;
    uint32_t second;
    uint32_t index;
};

struct mo_process {
    const struct mo_types *types;
    struct mo_allocation *allocations;
    size_t used;
    size_t limit;
    size_t objects;
    size_t baseline;
    uint64_t hash_key[2];
    uint8_t *abilities;
    uint32_t builtins[MO_TYPE_BIGINT + 1];
    uint32_t lookup_count;
    struct mo_layout_lookup lookup[];
};

MO_MUST_USE struct mo_allocation *mo_heap_allocation(struct mo_object *object);
MO_MUST_USE struct mo_allocation *mo_heap_allocate(struct mo_process *process, uint32_t type,
                                                   uint32_t layout, size_t bytes);
void mo_heap_release(struct mo_process *process, struct mo_allocation *allocation);
MO_MUST_USE struct mo_object *mo_value_object(const struct mo_types *types, uint32_t type,
                                              struct mo_cell value);
MO_MUST_USE uint32_t mo_heap_layout(const struct mo_process *process, uint32_t category,
                                    uint32_t first, uint32_t second);
MO_MUST_USE bool mo_heap_abilities(struct mo_process *process);

MO_MUST_USE uint64_t mo_heap_hash(const uint64_t key[2], const void *bytes, size_t length);
MO_MUST_USE uint64_t mo_heap_mix(const uint64_t key[2], uint64_t left, uint64_t right);
MO_MUST_USE bool mo_value_hash_key(struct mo_process *process, uint32_t type, struct mo_cell value,
                                   const uint64_t key[2], uint64_t *out);
MO_MUST_USE uint32_t mo_table_stride(const struct mo_allocation *table);
MO_MUST_USE uint64_t *mo_table_hashes(struct mo_allocation *table);
MO_MUST_USE uint8_t *mo_table_live(struct mo_allocation *table);
MO_MUST_USE uint32_t *mo_table_slots(struct mo_allocation *table);

struct mo_scratch {
    struct mo_arena arena;
    struct mo_process *process;
};
void mo_scratch_init(struct mo_scratch *scratch, struct mo_process *process);
void mo_scratch_free(struct mo_scratch *scratch);
MO_MUST_USE void *mo_scratch_resize(struct mo_scratch *scratch, void *bytes, size_t old_size,
                                    size_t new_size);

#endif
