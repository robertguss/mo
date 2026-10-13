/* Private allocation bookkeeping precedes the bytecode's object header. */
#ifndef MO_HEAP_INTERNAL_H
#define MO_HEAP_INTERNAL_H

#include "heap/heap.h"

#define MO_BUILTIN_LAYOUT(kind) (UINT32_MAX - (uint32_t)(kind))
#define MO_STATE_LAYOUT (UINT32_MAX - 32)
#define MO_ABILITY_EQUAL 1
#define MO_ABILITY_HASH 2
#define MO_ABILITY_ORDER 4

struct mo_allocation {
    struct mo_allocation *previous;
    struct mo_allocation *next;
    struct mo_allocation *pending;
    size_t size;
    uint64_t hash;
    uint32_t type;
    uint32_t length;
    bool hash_valid;
    bool deep_immortal;
    bool preparing;
    uint8_t padding[5];
    struct mo_object object;
    struct mo_cell cells[];
};
_Static_assert(offsetof(struct mo_allocation, cells) ==
                   offsetof(struct mo_allocation, object) + sizeof(struct mo_object),
               "fields immediately follow the header");

struct mo_layout_lookup {
    struct mo_object *constant;
    uint32_t category;
    uint32_t first;
    uint32_t second;
    uint32_t index;
};

struct mo_heap {
    const struct mo_types *types;
    struct mo_heap_image *image;
    struct mo_allocation *allocations;
    size_t used;
    size_t limit;
    size_t objects;
    const uint64_t *hash_key;
    const uint8_t *abilities;
    const uint32_t *builtins;
};

struct mo_heap_image {
    const struct mo_types *types;
    struct mo_heap *constants;
    size_t limit, used;
    uint64_t hash_key[2], index_key[2];
    uint8_t *abilities;
    uint32_t builtins[MO_TYPE_BIGINT + 1];
    uint32_t lookup_count, layout_count;
    struct mo_layout_lookup *lookup;
    bool ready;
};

/* The trailer keeps entry cells adjacent to the object header. */
struct mo_table_info {
    uint32_t capacity, active, slots, reserved;
};

MO_MUST_USE struct mo_allocation *mo_heap_allocation(struct mo_object *object);
MO_MUST_USE struct mo_allocation *mo_heap_allocate(struct mo_heap *process, uint32_t type,
                                                   uint32_t layout, size_t bytes);
void mo_heap_release(struct mo_heap *process, struct mo_allocation *allocation);
void mo_heap_drop_object(struct mo_heap *process, struct mo_object *object);
MO_MUST_USE struct mo_object *mo_value_object(const struct mo_types *types, uint32_t type,
                                              struct mo_cell value);
MO_MUST_USE struct mo_layout_lookup *mo_heap_layout(struct mo_heap *process, uint32_t category,
                                                    uint32_t first, uint32_t second);
MO_MUST_USE bool mo_heap_abilities(struct mo_heap_image *image);

MO_MUST_USE uint64_t mo_heap_mix(const uint64_t key[2], uint64_t left, uint64_t right);
MO_MUST_USE bool mo_value_index_hash(struct mo_heap *process, uint32_t type, struct mo_cell value,
                                     uint64_t *out);
MO_MUST_USE struct mo_table_info *mo_table_info(struct mo_allocation *table);
MO_MUST_USE uint32_t mo_table_stride(const struct mo_allocation *table);
MO_MUST_USE uint64_t *mo_table_hashes(struct mo_allocation *table);
MO_MUST_USE uint8_t *mo_table_live(struct mo_allocation *table);
MO_MUST_USE uint32_t *mo_table_slots(struct mo_allocation *table);
MO_MUST_USE uint32_t mo_bigint_sign(const struct mo_allocation *value);
MO_MUST_USE uint32_t mo_value_children(const struct mo_types *types, uint32_t type,
                                       struct mo_cell value);
MO_MUST_USE struct mo_cell mo_value_child(const struct mo_types *types, uint32_t type,
                                          struct mo_cell value, uint32_t index,
                                          uint32_t *child_type);
MO_MUST_USE bool mo_bigint_text(struct mo_heap *process, struct mo_cell value,
                                struct mo_buffer *out);

#endif
