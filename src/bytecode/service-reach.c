#include "bytecode/bytecode.h"

#include <string.h>

#include "heap/types.h"

struct node {
    size_t begin, end, cursor;
    uint32_t index, low, component, next;
};

static uint32_t components(struct node *nodes, uint32_t count, const uint32_t *edges,
                           uint32_t *stack, uint32_t *active, uint32_t *heads) {
    uint32_t serial = 0, groups = 0, pending = 0;
    for (uint32_t root = 0; root < count; root++) {
        if (nodes[root].index != 0)
            continue;
        uint32_t depth = 1;
        stack[0] = root;
        nodes[root].index = nodes[root].low = ++serial;
        active[pending++] = root;
        while (depth != 0) {
            struct node *n = &nodes[stack[depth - 1]];
            if (n->cursor != n->end) {
                uint32_t child = edges[n->cursor++];
                struct node *to = &nodes[child];
                if (to->index == 0) {
                    to->index = to->low = ++serial;
                    active[pending++] = child;
                    stack[depth++] = child;
                } else if (to->component == MO_NO_INDEX && to->index < n->low) {
                    n->low = to->index;
                }
                continue;
            }
            uint32_t current = stack[--depth];
            if (n->index == n->low) {
                heads[groups] = MO_NO_INDEX;
                uint32_t member;
                do {
                    member = active[--pending];
                    nodes[member].component = groups;
                    nodes[member].next = heads[groups];
                    heads[groups] = member;
                } while (member != current);
                groups++;
            }
            if (depth != 0 && n->low < nodes[stack[depth - 1]].low)
                nodes[stack[depth - 1]].low = n->low;
        }
    }
    return groups;
}

bool mo_bytecode_service_reach(struct mo_arena *arena, const struct mo_bytecode *file,
                               struct mo_bytecode_service_reach **out) {
    *out = NULL;
    uint32_t count = file->type_count;
    MO_ASSERT(count <= MO_LIMIT_TYPES && file->service_count <= MO_LIMIT_SERVICES);
    struct node *nodes =
        mo_arena_allocate_array(arena, count, sizeof *nodes, _Alignof(struct node));
    uint32_t *stack = mo_arena_allocate_array(arena, count, sizeof *stack, _Alignof(uint32_t));
    uint32_t *active = mo_arena_allocate_array(arena, count, sizeof *active, _Alignof(uint32_t));
    uint32_t *heads = mo_arena_allocate_array(arena, count, sizeof *heads, _Alignof(uint32_t));
    uint64_t *masks = mo_arena_allocate_array(arena, count, sizeof *masks, _Alignof(uint64_t));
    struct mo_bytecode_service_reach *result = mo_arena_allocate_array(
        arena, file->service_count, sizeof *result, _Alignof(struct mo_bytecode_service_reach));
    if (!nodes || !stack || !active || !heads || !masks || !result)
        return false;
    size_t total = 0;
    for (uint32_t i = 0; i < count; i++) {
        const struct mo_type *t = &file->types[i];
        size_t length = (size_t)t->parameter_count + t->field_count;
        for (uint32_t j = 0; j < t->case_count; j++) {
            if (t->cases[j].field_count > SIZE_MAX - length)
                return false;
            length += t->cases[j].field_count;
        }
        if (length > SIZE_MAX - total)
            return false;
        nodes[i] = (struct node){
            .begin = total, .cursor = total, .end = total + length, .component = MO_NO_INDEX};
        total += length;
    }
    uint32_t *edges = mo_arena_allocate_array(arena, total, sizeof *edges, _Alignof(uint32_t));
    if (edges == NULL)
        return false;
    for (uint32_t i = 0; i < count; i++) {
        const struct mo_type *t = &file->types[i];
        size_t at = nodes[i].begin;
        for (uint32_t j = 0; j < t->parameter_count; j++)
            edges[at++] = t->parameters[j];
        for (uint32_t j = 0; j < t->field_count; j++)
            edges[at++] = t->fields[j].type;
        for (uint32_t j = 0; j < t->case_count; j++)
            for (uint32_t k = 0; k < t->cases[j].field_count; k++)
                edges[at++] = t->cases[j].fields[k].type;
        MO_ASSERT(at == nodes[i].end);
    }
    for (size_t i = 0; i < total; i++)
        MO_ASSERT(edges[i] < count);
    uint32_t groups = components(nodes, count, edges, stack, active, heads);
    for (uint32_t first = 0; first < file->service_count; first += 64) {
        uint32_t width = file->service_count - first;
        if (width > 64)
            width = 64;
        memset(masks, 0, groups * sizeof *masks);
        for (uint32_t i = 0; i < width; i++) {
            const struct mo_bytecode_service *s = &file->services[first + i];
            result[first + i] = (struct mo_bytecode_service_reach){0};
            for (uint32_t j = 0; j < s->tool_count; j++) {
                const struct mo_bytecode_service_tool *t = &s->tools[j];
                for (uint32_t k = 0; k <= t->argument_count; k++) {
                    uint32_t root = k < t->argument_count ? t->argument_types[k] : t->result_type;
                    MO_ASSERT(root < count);
                    masks[nodes[root].component] |= UINT64_C(1) << i;
                }
            }
        }
        for (uint32_t c = groups; c-- > 0;) {
            if (masks[c] == 0)
                continue;
            for (uint32_t n = heads[c]; n != MO_NO_INDEX; n = nodes[n].next)
                for (size_t e = nodes[n].begin; e < nodes[n].end; e++) {
                    uint32_t child = nodes[edges[e]].component;
                    MO_ASSERT(child <= c);
                    masks[child] |= masks[c];
                }
        }
        for (unsigned pass = 0; pass < 2; pass++) {
            for (uint32_t i = 0; i < count; i++) {
                if (file->types[i].kind != MO_TYPE_RECORD && file->types[i].kind != MO_TYPE_CHOICE)
                    continue;
                uint64_t mask = masks[nodes[i].component];
                for (uint32_t bit = 0; mask != 0; bit++, mask >>= 1) {
                    if ((mask & 1) == 0)
                        continue;
                    struct mo_bytecode_service_reach *r = &result[first + bit];
                    if (pass != 0)
                        r->types[r->count] = i;
                    r->count++;
                }
            }
            if (pass == 0)
                for (uint32_t i = 0; i < width; i++) {
                    struct mo_bytecode_service_reach *r = &result[first + i];
                    r->types = mo_arena_allocate_array(arena, r->count, sizeof *r->types,
                                                       _Alignof(uint32_t));
                    if (r->types == NULL)
                        return false;
                    r->count = 0;
                }
        }
    }
    *out = result;
    return true;
}
