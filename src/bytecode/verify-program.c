#include "bytecode/verify-program.h"

#include <string.h>

struct edge {
    uint32_t from, to, next, reverse;
};
struct graph {
    struct edge *edges;
    uint32_t *heads, *reverse;
    uint32_t count;
};
static void add(struct graph *g, uint32_t from, uint32_t to) {
    g->edges[g->count] = (struct edge){from, to, g->heads[from], g->reverse[to]};
    g->heads[from] = g->count;
    g->reverse[to] = g->count++;
}
static MO_MUST_USE uint32_t next(const struct mo_bytecode_function *fn, uint32_t pc) {
    const struct mo_opcode_info *op = mo_opcode_find(fn->code[pc] & MO_OPCODE_MASK);
    MO_ASSERT(op != NULL);
    return pc + 1U + op->fixed_count + (fn->code[pc] >> 16);
}
static MO_MUST_USE bool refuse(struct mo_load_error *error, uint32_t offset, const char *reason) {
    *error = (struct mo_load_error){offset, reason};
    return false;
}

/* Kosaraju with explicit stacks. Condensation components are source-first; one reverse sweep
 * computes maximum dependencies even across mutually recursive helpers, without fixed points. */
static MO_MUST_USE bool summaries(struct mo_arena *arena, struct graph *g, uint32_t nodes,
                                  uint32_t *required) {
    uint32_t *stack = mo_arena_allocate_array(arena, nodes, sizeof *stack, _Alignof(uint32_t));
    uint32_t *cursor = mo_arena_allocate_array(arena, nodes, sizeof *cursor, _Alignof(uint32_t));
    uint32_t *order = mo_arena_allocate_array(arena, nodes, sizeof *order, _Alignof(uint32_t));
    uint32_t *component =
        mo_arena_allocate_array(arena, nodes, sizeof *component, _Alignof(uint32_t));
    uint32_t *maximum = mo_arena_allocate_array(arena, nodes, sizeof *maximum, _Alignof(uint32_t));
    bool *seen = mo_arena_allocate_array(arena, nodes, sizeof *seen, _Alignof(bool));
    if (stack == NULL || cursor == NULL || order == NULL || component == NULL || maximum == NULL ||
        seen == NULL) {
        return false;
    }
    memset(seen, 0, nodes * sizeof *seen);
    uint32_t count = 0;
    for (uint32_t root = 0; root < nodes; root++) {
        if (seen[root]) {
            continue;
        }
        uint32_t depth = 1;
        stack[0] = root;
        cursor[0] = g->heads[root];
        seen[root] = true;
        while (depth != 0) {
            uint32_t e = cursor[depth - 1];
            if (e == MO_NO_INDEX) {
                order[count++] = stack[--depth];
                continue;
            }
            cursor[depth - 1] = g->edges[e].next;
            uint32_t child = g->edges[e].to;
            if (!seen[child]) {
                seen[child] = true;
                stack[depth] = child;
                cursor[depth++] = g->heads[child];
            }
        }
    }
    for (uint32_t i = 0; i < nodes; i++) {
        component[i] = MO_NO_INDEX;
        maximum[i] = 0;
    }
    uint32_t components = 0;
    for (uint32_t i = count; i != 0; i--) {
        uint32_t root = order[i - 1];
        if (component[root] != MO_NO_INDEX) {
            continue;
        }
        uint32_t depth = 1;
        stack[0] = root;
        component[root] = components;
        while (depth != 0) {
            uint32_t node = stack[--depth];
            if (maximum[components] < required[node]) {
                maximum[components] = required[node];
            }
            for (uint32_t e = g->reverse[node]; e != MO_NO_INDEX; e = g->edges[e].reverse) {
                uint32_t child = g->edges[e].from;
                if (component[child] == MO_NO_INDEX) {
                    component[child] = components;
                    stack[depth++] = child;
                }
            }
        }
        components++;
    }
    for (uint32_t i = 0; i < components; i++) {
        g->heads[i] = MO_NO_INDEX;
    }
    for (uint32_t i = 0; i < g->count; i++) {
        uint32_t from = component[g->edges[i].from], to = component[g->edges[i].to];
        if (from != to) {
            MO_ASSERT(from < to);
            g->edges[i].to = to;
            g->edges[i].next = g->heads[from];
            g->heads[from] = i;
        }
    }
    for (uint32_t i = components; i != 0; i--) {
        uint32_t c = i - 1;
        for (uint32_t e = g->heads[c]; e != MO_NO_INDEX; e = g->edges[e].next) {
            if (maximum[c] < maximum[g->edges[e].to]) {
                maximum[c] = maximum[g->edges[e].to];
            }
        }
    }
    for (uint32_t i = 0; i < nodes; i++) {
        required[i] = maximum[component[i]];
    }
    return true;
}

bool mo_verify_program_init(struct mo_arena *arena, const struct mo_bytecode *f,
                            const uint32_t *code_offsets, struct mo_verify_program *out,
                            struct mo_load_error *error) {
    uint32_t nodes = f->function_count + f->type_count;
    size_t capacity = f->layout_count;
    for (uint32_t i = 0; i < f->function_count; i++) {
        capacity += f->functions[i].code_count;
    }
    struct graph g = {0};
    out->global_rank =
        mo_arena_allocate_array(arena, f->global_count, sizeof(uint32_t), _Alignof(uint32_t));
    out->init_rank =
        mo_arena_allocate_array(arena, f->function_count, sizeof(uint32_t), _Alignof(uint32_t));
    out->required = mo_arena_allocate_array(arena, nodes, sizeof(uint32_t), _Alignof(uint32_t));
    g.edges = mo_arena_allocate_array(arena, capacity, sizeof *g.edges, _Alignof(struct edge));
    g.heads = mo_arena_allocate_array(arena, nodes, sizeof(uint32_t), _Alignof(uint32_t));
    g.reverse = mo_arena_allocate_array(arena, nodes, sizeof(uint32_t), _Alignof(uint32_t));
    if (out->global_rank == NULL || out->init_rank == NULL || out->required == NULL ||
        g.edges == NULL || g.heads == NULL || g.reverse == NULL) {
        return refuse(error, 0, "no memory");
    }
    memset(out->global_rank, 0, f->global_count * sizeof(uint32_t));
    memset(out->init_rank, 0, f->function_count * sizeof(uint32_t));
    memset(out->required, 0, nodes * sizeof(uint32_t));
    for (uint32_t i = 0; i < nodes; i++) {
        g.heads[i] = MO_NO_INDEX;
        g.reverse[i] = MO_NO_INDEX;
    }
    for (uint32_t i = 0; i < f->init_count; i++) {
        uint32_t index = f->init[i], target = MO_NO_INDEX;
        const struct mo_bytecode_function *fn = &f->functions[index];
        out->init_rank[index] = i + 1;
        for (uint32_t pc = 0; pc < fn->code_count; pc = next(fn, pc)) {
            if ((fn->code[pc] & MO_OPCODE_MASK) == MO_OP_SET_GLOBAL) {
                uint32_t global = fn->code[pc + 1];
                if (target != MO_NO_INDEX && target != global) {
                    return refuse(error, code_offsets[index] + pc * 4,
                                  "init sets different globals");
                }
                target = global;
            }
        }
        if (target == MO_NO_INDEX) {
            return refuse(error, code_offsets[index], "init has no global store");
        }
        if (out->global_rank[target] != 0) {
            return refuse(error, 20, "global has multiple initializers");
        }
        out->global_rank[target] = i + 1;
    }
    for (uint32_t i = 0; i < f->global_count; i++) {
        if (out->global_rank[i] == 0) {
            return refuse(error, 20, "global has no initializer");
        }
    }
    for (uint32_t i = 0; i < f->layout_count; i++) {
        const struct mo_layout *l = &f->layouts[i];
        if (l->kind == MO_LAYOUT_CLOSURE && !f->functions[l->function].spec_code) {
            MO_ASSERT(g.count < capacity);
            add(&g, f->function_count + l->type, l->function);
        }
    }
    for (uint32_t i = 0; i < f->function_count; i++) {
        const struct mo_bytecode_function *fn = &f->functions[i];
        for (uint32_t pc = 0; pc < fn->code_count; pc = next(fn, pc)) {
            uint32_t op = fn->code[pc] & MO_OPCODE_MASK;
            if (op == MO_OP_GLOBAL) {
                uint32_t rank = out->global_rank[fn->code[pc + 2]];
                if (out->required[i] < rank) {
                    out->required[i] = rank;
                }
            }
            if (op == MO_OP_CALL || op == MO_OP_TAIL_CALL) {
                MO_ASSERT(g.count < capacity);
                add(&g, i, fn->code[pc + (op == MO_OP_CALL ? 2U : 1U)]);
            }
            if (op == MO_OP_CALL_CLOSURE || op == MO_OP_TAIL_CALL_CLOSURE) {
                uint32_t r = fn->code[pc + (op == MO_OP_CALL_CLOSURE ? 2U : 1U)];
                MO_ASSERT(g.count < capacity);
                add(&g, i, f->function_count + fn->register_types[r]);
            }
        }
    }
    if (!summaries(arena, &g, nodes, out->required)) {
        return refuse(error, 0, "no memory");
    }
    return true;
}
