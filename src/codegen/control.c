#include "codegen/control.h"

#include <string.h>

struct graph {
    const uint32_t *code;
    uint32_t count, pending, visited;
    uint32_t *queue, *offsets;
    bool *reachable;
};
static void visit(struct graph *g, uint32_t pc) {
    MO_ASSERT(pc < g->count);
    if (!g->reachable[pc]) {
        g->reachable[pc] = true;
        MO_ASSERT(g->pending < g->count);
        g->queue[g->pending++] = pc;
    }
}
static MO_MUST_USE uint32_t width(const struct graph *g, uint32_t pc) {
    const struct mo_opcode_info *op = mo_opcode_find(g->code[pc] & MO_OPCODE_MASK);
    MO_ASSERT(op != NULL);
    uint32_t n = 1u + op->fixed_count + (g->code[pc] >> MO_OPCODE_VARIABLE_SHIFT);
    MO_ASSERT(n <= g->count - pc);
    return n;
}
static void walk(struct graph *g) {
    visit(g, 0);
    for (uint32_t i = 0; i < g->pending; i++) {
        uint32_t pc = g->queue[i], n = width(g, pc);
        uint32_t op = g->code[pc] & MO_OPCODE_MASK;
        g->visited += n;
        if (op == MO_OP_JUMP) {
            visit(g, g->code[pc + 1]);
        } else if (op == MO_OP_BRANCH) {
            visit(g, g->code[pc + 2]);
            visit(g, g->code[pc + 3]);
        } else if (op == MO_OP_SWITCH) {
            for (uint32_t j = 2; j < n; j++)
                visit(g, g->code[pc + j]);
        } else if (op != MO_OP_RET && op != MO_OP_STOP && op != MO_OP_FAIL &&
                   op != MO_OP_TAIL_CALL && op != MO_OP_TAIL_CALL_CLOSURE) {
            visit(g, pc + n);
        }
    }
}
static void relocate(struct graph *g, uint32_t *code) {
    uint32_t out = 0;
    for (uint32_t pc = 0; pc < g->count;) {
        uint32_t n = width(g, pc);
        g->offsets[pc] = out;
        if (g->reachable[pc]) {
            memcpy(code + out, g->code + pc, (size_t)n * sizeof *code);
            out += n;
        }
        pc += n;
    }
    g->offsets[g->count] = out;
    MO_ASSERT(out == g->visited);
    for (uint32_t pc = 0; pc < out;) {
        const struct mo_opcode_info *op = mo_opcode_find(code[pc] & MO_OPCODE_MASK);
        MO_ASSERT(op != NULL);
        uint32_t n = code[pc] >> MO_OPCODE_VARIABLE_SHIFT;
        for (uint32_t i = 0; i < op->fixed_count; i++)
            if (op->fixed[i].kind == MO_OPERAND_TARGET)
                code[pc + 1 + i] = g->offsets[code[pc + 1 + i]];
        if (op->variable.kind == MO_OPERAND_TARGET)
            for (uint32_t i = 0; i < n; i++)
                code[pc + 1 + op->fixed_count + i] = g->offsets[code[pc + 1 + op->fixed_count + i]];
        pc += 1u + op->fixed_count + n;
    }
}
static void project(struct graph *g, struct mo_bytecode_debug *d, struct mo_bytecode_span *spans,
                    struct mo_bytecode_register_name *names) {
    uint32_t count = 0, source = 0;
    for (uint32_t pc = 0; pc < g->count && d->span_count != 0;) {
        while (source + 1 < d->span_count && d->spans[source + 1].code_offset <= pc)
            source++;
        if (g->reachable[pc]) {
            struct mo_bytecode_span s = d->spans[source];
            /* A span beginning in a removed block can still cover surviving instructions. */
            if (count == 0 || spans[count - 1].file != s.file ||
                spans[count - 1].start != s.start || spans[count - 1].end != s.end) {
                s.code_offset = g->offsets[pc];
                spans[count++] = s;
            }
        }
        pc += width(g, pc);
    }
    d->span_count = count;
    d->spans = spans;
    count = 0;
    for (uint32_t i = 0; i < d->name_count; i++) {
        struct mo_bytecode_register_name n = d->names[i];
        n.code_start = g->offsets[n.code_start];
        n.code_end = g->offsets[n.code_end];
        if (n.code_start != n.code_end)
            names[count++] = n;
    }
    d->name_count = count;
    d->names = names;
}
bool mo_codegen_prune(struct mo_arena *arena, struct mo_bytecode_function *function,
                      struct mo_bytecode_debug *debug) {
    MO_ASSERT(function->code_count != 0 && function->code_count <= MO_LIMIT_FUNCTION_CODE_WORDS);
    struct mo_arena temporary;
    /* One queue, one offset map, one reachability flag per word, including end offsets. */
    mo_arena_init(&temporary, (size_t)(MO_LIMIT_FUNCTION_CODE_WORDS + 1) *
                                      (2 * sizeof(uint32_t) + sizeof(bool)) +
                                  3 * MO_ARENA_CHUNK_HEADER);
    struct graph g = {.code = function->code, .count = function->code_count};
    g.queue = mo_arena_allocate_array(&temporary, g.count, sizeof *g.queue, _Alignof(uint32_t));
    g.offsets = mo_arena_allocate_array(&temporary, (size_t)g.count + 1, sizeof *g.offsets,
                                        _Alignof(uint32_t));
    g.reachable = mo_arena_allocate_array(&temporary, g.count, sizeof *g.reachable, _Alignof(bool));
    bool ok = g.queue != NULL && g.offsets != NULL && g.reachable != NULL;
    if (ok) {
        memset(g.reachable, 0, (size_t)g.count * sizeof *g.reachable);
        walk(&g);
        if (g.visited != g.count) {
            uint32_t *code =
                mo_arena_allocate_array(arena, g.visited, sizeof *code, _Alignof(uint32_t));
            struct mo_bytecode_span *spans = mo_arena_allocate_array(
                arena, debug->span_count, sizeof *spans, _Alignof(struct mo_bytecode_span));
            struct mo_bytecode_register_name *names =
                mo_arena_allocate_array(arena, debug->name_count, sizeof *names,
                                        _Alignof(struct mo_bytecode_register_name));
            ok = code != NULL && spans != NULL && names != NULL;
            if (ok) {
                relocate(&g, code);
                project(&g, debug, spans, names);
                function->code = code;
                function->code_count = g.visited;
            }
        }
    }
    mo_arena_free(&temporary);
    return ok;
}
