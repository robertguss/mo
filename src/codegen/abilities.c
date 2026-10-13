#include "codegen/abilities.h"

struct edge {
    uint32_t parent, next;
};

/* Start with both abilities and remove bits through reverse edges. This greatest fixed point
 * handles recursive nominal types without revisiting a type graph for every instruction. */
uint8_t *mo_codegen_abilities(struct mo_arena *arena, const struct mo_types *types) {
    MO_ASSERT(types->finished);
    size_t count = 0;
    for (uint32_t i = 0; i < types->count; i++) {
        const struct mo_type *t = &types->items[i];
        count += t->parameter_count + t->field_count;
        for (uint32_t j = 0; j < t->case_count; j++)
            count += t->cases[j].field_count;
    }
    if (count > UINT32_MAX)
        return NULL;
    struct edge *edges =
        mo_arena_allocate_array(arena, count, sizeof *edges, _Alignof(struct edge));
    uint32_t *heads =
        mo_arena_allocate_array(arena, types->count, sizeof *heads, _Alignof(uint32_t));
    uint32_t *queue =
        mo_arena_allocate_array(arena, (size_t)types->count * 2, sizeof *queue, _Alignof(uint32_t));
    uint8_t *flags = mo_arena_allocate_array(arena, types->count, sizeof *flags, _Alignof(uint8_t));
    if (edges == NULL || heads == NULL || queue == NULL || flags == NULL)
        return NULL;
    uint32_t end = 0, n = 0;
    for (uint32_t i = 0; i < types->count; i++)
        heads[i] = MO_NO_INDEX;
    for (uint32_t i = 0; i < types->count; i++) {
        const struct mo_type *t = &types->items[i];
        flags[i] = MO_CODEGEN_EQUAL | MO_CODEGEN_HASH;
        if (t->kind == MO_TYPE_FUNCTION || t->kind == MO_TYPE_ENV || t->kind == MO_TYPE_CAPABILITY)
            flags[i] = 0;
        else if (t->kind == MO_TYPE_FLOAT)
            flags[i] = MO_CODEGEN_EQUAL;
        if (flags[i] != (MO_CODEGEN_EQUAL | MO_CODEGEN_HASH))
            queue[end++] = i;
        uint32_t total = t->parameter_count + t->field_count;
        for (uint32_t j = 0; j < total; j++) {
            uint32_t child =
                j < t->parameter_count ? t->parameters[j] : t->fields[j - t->parameter_count].type;
            MO_ASSERT(n < count && child < types->count);
            edges[n] = (struct edge){i, heads[child]};
            heads[child] = n++;
        }
        for (uint32_t j = 0; j < t->case_count; j++)
            for (uint32_t k = 0; k < t->cases[j].field_count; k++) {
                uint32_t child = t->cases[j].fields[k].type;
                MO_ASSERT(n < count && child < types->count);
                edges[n] = (struct edge){i, heads[child]};
                heads[child] = n++;
            }
    }
    for (uint32_t at = 0; at < end; at++)
        for (uint32_t e = heads[queue[at]]; e != MO_NO_INDEX; e = edges[e].next) {
            uint32_t parent = edges[e].parent;
            uint8_t after = flags[parent] & flags[queue[at]];
            if (after != flags[parent]) {
                flags[parent] = after;
                MO_ASSERT(end < 2u * types->count);
                queue[end++] = parent;
            }
        }
    return flags;
}
