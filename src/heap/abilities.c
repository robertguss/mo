/* Propagate missing abilities along reverse type edges. Recursive nominal
 * types start with all abilities; only a reachable forbidden kind removes one. */
#include "heap/internal.h"

#include <string.h>

struct ability_edge {
    uint32_t parent, next;
};

static MO_MUST_USE uint8_t initial_abilities(enum mo_type_kind kind) {
    if (kind == MO_TYPE_FUNCTION || kind == MO_TYPE_ENV || kind == MO_TYPE_CAPABILITY) {
        return 0;
    }
    if (kind == MO_TYPE_FLOAT) {
        return MO_ABILITY_EQUAL | MO_ABILITY_ORDER;
    }
    if (kind == MO_TYPE_RANGE || kind == MO_TYPE_MAP || kind == MO_TYPE_SET) {
        return MO_ABILITY_EQUAL | MO_ABILITY_HASH;
    }
    return MO_ABILITY_EQUAL | MO_ABILITY_HASH | MO_ABILITY_ORDER;
}

static MO_MUST_USE uint32_t dependency_count(const struct mo_type *type) {
    if (type->kind == MO_TYPE_RECORD) {
        return type->field_count;
    }
    if (type->kind == MO_TYPE_TUPLE || type->kind == MO_TYPE_LIST || type->kind == MO_TYPE_MAP ||
        type->kind == MO_TYPE_SET) {
        return type->parameter_count;
    }
    return 0;
}

static void add_edge(struct ability_edge *edges, uint32_t *heads, uint32_t *next, uint32_t parent,
                     uint32_t child) {
    edges[*next] = (struct ability_edge){.parent = parent, .next = heads[child]};
    heads[child] = (*next)++;
}

bool mo_heap_abilities(struct mo_heap_image *process) {
    const struct mo_types *types = process->types;
    size_t edge_count = 0;
    size_t work = types->count;
    for (uint32_t i = 0; i < types->count; i++) {
        const struct mo_type *type = &types->items[i];
        uint32_t count = dependency_count(type);
        edge_count += count;
        work += count;
        if (type->kind == MO_TYPE_CHOICE) {
            work += type->case_count;
            if (work > MO_LIMIT_HEAP_WORK) {
                return false;
            }
            for (uint32_t j = 0; j < type->case_count; j++) {
                edge_count += type->cases[j].field_count;
                work += type->cases[j].field_count;
                if (work > MO_LIMIT_HEAP_WORK) {
                    return false;
                }
            }
        }
        if (work > MO_LIMIT_HEAP_WORK) {
            return false;
        }
    }
    struct mo_arena scratch;
    mo_arena_init(&scratch, process->limit - process->used);
    size_t bytes =
        (size_t)types->count * 4 * sizeof(uint32_t) + edge_count * sizeof(struct ability_edge);
    uint32_t *heads = mo_arena_allocate(&scratch, bytes, _Alignof(uint32_t));
    if (heads == NULL) {
        mo_arena_free(&scratch);
        return false;
    }
    uint32_t *queue = heads + types->count;
    struct ability_edge *edges = (struct ability_edge *)(queue + (size_t)types->count * 3);
    uint32_t next = 0;
    size_t read_index = 0, write_index = 0;
    for (uint32_t i = 0; i < types->count; i++) {
        heads[i] = MO_NO_INDEX;
    }
    for (uint32_t i = 0; i < types->count; i++) {
        const struct mo_type *type = &types->items[i];
        process->abilities[i] = initial_abilities(type->kind);
        if (process->abilities[i] != (MO_ABILITY_EQUAL | MO_ABILITY_HASH | MO_ABILITY_ORDER)) {
            queue[write_index++] = i;
        }
        uint32_t count = dependency_count(type);
        for (uint32_t j = 0; j < count; j++) {
            add_edge(edges, heads, &next, i,
                     type->kind == MO_TYPE_RECORD ? type->fields[j].type : type->parameters[j]);
        }
        if (type->kind == MO_TYPE_CHOICE) {
            for (uint32_t j = 0; j < type->case_count; j++) {
                for (uint32_t k = 0; k < type->cases[j].field_count; k++) {
                    add_edge(edges, heads, &next, i, type->cases[j].fields[k].type);
                }
            }
        }
    }
    MO_ASSERT(next == edge_count);
    while (read_index < write_index) {
        uint32_t child = queue[read_index++];
        for (uint32_t i = heads[child]; i != MO_NO_INDEX; i = edges[i].next) {
            uint32_t parent = edges[i].parent;
            uint8_t flags = process->abilities[parent] & process->abilities[child];
            if (flags != process->abilities[parent]) {
                process->abilities[parent] = flags;
                MO_ASSERT(write_index < (size_t)types->count * 3);
                queue[write_index++] = parent;
            }
        }
    }
    mo_arena_free(&scratch);
    return true;
}
