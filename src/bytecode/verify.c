#include "bytecode/verify.h"
#include "bytecode/signature.h"
#include "bytecode/verify-program.h"

#include <stdlib.h>
#include <string.h>

enum state { UNDEFINED, VALUE, OWNED, BORROWED, MOVED, TOKEN };
#define CASE_UNKNOWN UINT32_MAX
#define CASE_UNREACHED (UINT32_MAX - 1)
struct block {
    uint32_t start, end, last;
    uint32_t *state;
    uint32_t incoming;
    bool visited;
};
struct undo {
    uint32_t reg, state;
};
struct walk {
    uint32_t block, edge, mark;
};
struct flow {
    struct mo_verify_function v;
    struct mo_arena arena;
    struct block *blocks;
    uint32_t *map, *known, *reset, *queue;
    uint32_t count;
    const struct layout_key *layouts;
    const struct mo_types *types;
    const struct mo_verify_program *program;
    uint32_t index;
    uint32_t replay;
    bool *readable;
    mo_readable_visitor visit;
    void *context;
    uint64_t work;
    uint64_t work_limit;
};

static MO_MUST_USE bool refuse(struct flow *f, uint32_t pc, const char *reason) {
    *f->v.error = (struct mo_load_error){f->v.base + pc * 4, reason, MO_BYTECODE_INVALID};
    return false;
}
static MO_MUST_USE bool charge(struct flow *f, uint32_t pc, size_t steps) {
    if (steps > f->work_limit - f->work) {
        bool ok = refuse(f, pc, "verification work limit");
        f->v.error->kind = MO_BYTECODE_LIMIT;
        return ok;
    }
    f->work += steps;
    return true;
}
static MO_MUST_USE void *array(struct flow *f, size_t count, size_t size, size_t alignment) {
    void *p = mo_arena_allocate_array(&f->arena, count, size, alignment);
    if (p == NULL) {
        *f->v.error = (struct mo_load_error){.reason = "no memory", .kind = MO_BYTECODE_NO_MEMORY};
    }
    return p;
}
static MO_MUST_USE uint32_t next(const struct mo_bytecode_function *fn, uint32_t pc) {
    const struct mo_opcode_info *op = mo_opcode_find(fn->code[pc] & MO_OPCODE_MASK);
    MO_ASSERT(op != NULL);
    return pc + 1U + op->fixed_count + (fn->code[pc] >> MO_OPCODE_VARIABLE_SHIFT);
}
static MO_MUST_USE bool terminal(uint32_t op) {
    return op >= MO_OP_TAIL_CALL && op <= MO_OP_FAIL;
}
static MO_MUST_USE bool transfer(uint32_t op) {
    return terminal(op) || op == MO_OP_JUMP || op == MO_OP_BRANCH || op == MO_OP_SWITCH;
}
static MO_MUST_USE uint32_t edge_count(const struct flow *f, uint32_t b) {
    const struct mo_bytecode_function *fn = f->v.fn;
    uint32_t pc = f->blocks[b].last, op = fn->code[pc] & MO_OPCODE_MASK;
    return terminal(op)         ? 0U
           : op == MO_OP_BRANCH ? 2U
           : op == MO_OP_SWITCH ? fn->code[pc] >> 16
                                : 1U;
}
static MO_MUST_USE uint32_t edge(const struct flow *f, uint32_t b, uint32_t e) {
    const struct mo_bytecode_function *fn = f->v.fn;
    uint32_t pc = f->blocks[b].last, op = fn->code[pc] & MO_OPCODE_MASK;
    uint32_t target = op == MO_OP_JUMP                           ? fn->code[pc + 1]
                      : op == MO_OP_BRANCH || op == MO_OP_SWITCH ? fn->code[pc + 2 + e]
                                                                 : f->blocks[b].end;
    MO_ASSERT(target < fn->code_count);
    return f->map[target];
}

static MO_MUST_USE bool graph(struct flow *f) {
    const struct mo_bytecode_function *fn = f->v.fn;
    if (fn->code_count == 0) {
        return refuse(f, 0, "function has no entry instruction");
    }
    f->map = array(f, (size_t)fn->code_count + 1, sizeof *f->map, _Alignof(uint32_t));
    if (f->map == NULL) {
        return false;
    }
    memset(f->map, 0, ((size_t)fn->code_count + 1) * sizeof *f->map);
    f->map[0] = 1;
    for (uint32_t pc = 0; pc < fn->code_count; pc = next(fn, pc)) {
        if (!charge(f, pc, next(fn, pc) - pc) ||
            (f->readable == NULL && !mo_verify_instruction(&f->v, pc))) {
            return false;
        }
        uint32_t op = fn->code[pc] & MO_OPCODE_MASK, end = next(fn, pc);
        if (transfer(op)) {
            f->map[end] = 1;
        }
        if (op == MO_OP_JUMP) {
            f->map[fn->code[pc + 1]] = 1;
        }
        if (op == MO_OP_BRANCH || op == MO_OP_SWITCH) {
            uint32_t n = op == MO_OP_BRANCH ? 2U : fn->code[pc] >> 16;
            for (uint32_t i = 0; i < n; i++) {
                f->map[fn->code[pc + 2 + i]] = 1;
            }
        }
        if (end == fn->code_count && !transfer(op)) {
            return refuse(f, pc, "function falls off code");
        }
    }
    for (uint32_t pc = 0; pc < fn->code_count; pc = next(fn, pc)) {
        f->count += f->map[pc];
    }
    /* The entry is a block start and every block start is an instruction start: the VM asserts
     * `pc < code_count` at each instruction and every jump target below code_count. */
    MO_ASSERT(f->map[0] == 1 && f->count >= 1 && f->count <= fn->code_count);
    if ((uint64_t)fn->register_count * f->count > MO_LIMIT_FUNCTION_REGISTER_BLOCKS) {
        bool ok = refuse(f, 0, "register-block limit");
        f->v.error->kind = MO_BYTECODE_LIMIT;
        return ok;
    }
    f->blocks = array(f, f->count, sizeof *f->blocks, _Alignof(struct block));
    f->queue = array(f, f->count, sizeof *f->queue, _Alignof(uint32_t));
    f->known = array(f, fn->code_count, sizeof *f->known, _Alignof(uint32_t));
    f->reset = array(f, fn->code_count, sizeof *f->reset, _Alignof(uint32_t));
    if (f->blocks == NULL || f->queue == NULL || f->known == NULL || f->reset == NULL) {
        return false;
    }
    uint32_t b = 0;
    for (uint32_t pc = 0; pc < fn->code_count; pc = next(fn, pc)) {
        if (f->map[pc] != 0) {
            if (pc != 0) {
                f->blocks[b++].end = pc;
            }
            f->blocks[b] = (struct block){.start = pc};
        }
        f->map[pc] = b;
        f->blocks[b].last = pc;
        f->known[pc] = CASE_UNKNOWN;
        f->reset[pc] = MO_NO_INDEX;
    }
    f->blocks[b].end = fn->code_count;
    for (uint32_t i = 0; i < f->count; i++) {
        for (uint32_t e = 0; e < edge_count(f, i); e++) {
            f->blocks[edge(f, i, e)].incoming++;
        }
    }
    bool *seen = array(f, f->count, sizeof *seen, _Alignof(bool));
    if (seen == NULL) {
        return false;
    }
    memset(seen, 0, f->count * sizeof *seen);
    uint32_t end = 1;
    f->queue[0] = 0;
    seen[0] = true;
    for (uint32_t start = 0; start < end; start++) {
        uint32_t from = f->queue[start];
        for (uint32_t i = 0; i < edge_count(f, from); i++) {
            uint32_t to = edge(f, from, i);
            if (!seen[to]) {
                seen[to] = true;
                f->queue[end++] = to;
            }
        }
    }
    for (uint32_t i = 0; i < f->count; i++) {
        if (!seen[i]) {
            return refuse(f, f->blocks[i].start, "unreachable instruction");
        }
    }
    return true;
}

static MO_MUST_USE int layout_order(const void *a, const void *b) {
    const struct layout_key *x = a, *y = b;
    if (x->type != y->type) {
        return (x->type > y->type) - (x->type < y->type);
    }
    return (x->tag > y->tag) - (x->tag < y->tag);
}
uint32_t mo_verify_layout_lookup(const struct layout_key *keys, uint32_t count, uint32_t type,
                                 uint32_t tag) {
    uint32_t lo = 0, hi = count;
    struct layout_key key = {type, tag, 0};
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (layout_order(&keys[mid], &key) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < count && layout_order(&keys[lo], &key) == 0 ? keys[lo].index : MO_NO_INDEX;
}
static MO_MUST_USE uint32_t layout(const struct flow *f, uint32_t type, uint32_t tag) {
    return mo_verify_layout_lookup(f->layouts, f->v.file->layout_count, type, tag);
}

/* Track only choice registers whose case is needed by case_field or reset. Each fact changes
 * at most twice. Both the scans and the work queue are charged before doing their work. */
static MO_MUST_USE bool cases(struct flow *f) {
    const struct mo_bytecode_function *fn = f->v.fn;
    uint32_t *facts = array(f, f->count, sizeof *facts, _Alignof(uint32_t));
    uint32_t *last_write = array(f, f->count, sizeof *last_write, _Alignof(uint32_t));
    bool *queued = array(f, f->count, sizeof *queued, _Alignof(bool));
    bool *needed = array(f, fn->register_count, sizeof *needed, _Alignof(bool));
    if (facts == NULL || queued == NULL || last_write == NULL || needed == NULL) {
        return false;
    }
    memset(needed, 0, fn->register_count * sizeof *needed);
    for (uint32_t pc = 0; pc < fn->code_count; pc = next(fn, pc)) {
        uint32_t op = fn->code[pc] & MO_OPCODE_MASK;
        if (op == MO_OP_CASE_FIELD || op == MO_OP_RESET) {
            needed[fn->code[pc + 2]] = true;
        }
    }
    for (uint32_t r = 0; r < fn->register_count; r++) {
        if (!needed[r] || f->v.file->types[fn->register_types[r]].kind != MO_TYPE_CHOICE) {
            continue;
        }
        for (uint32_t b = 0; b < f->count; b++) {
            facts[b] = CASE_UNREACHED;
            queued[b] = false;
            last_write[b] = MO_NO_INDEX;
            for (uint32_t pc = f->blocks[b].start; pc < f->blocks[b].end; pc = next(fn, pc)) {
                const struct mo_opcode_info *op = mo_opcode_find(fn->code[pc] & MO_OPCODE_MASK);
                MO_ASSERT(op != NULL);
                if (!charge(f, pc, 1U + op->fixed_count)) {
                    return false;
                }
                for (uint32_t i = 0; i < op->fixed_count; i++) {
                    if ((op->fixed[i].access == MO_ACCESS_WRITE ||
                         op->fixed[i].access == MO_ACCESS_TOKEN_WRITE) &&
                        fn->code[pc + 1 + i] == r) {
                        last_write[b] = pc;
                    }
                }
            }
        }
        uint32_t start = 0, end = 0, pending = 1;
        f->queue[end] = 0;
        end = (end + 1) % f->count;
        facts[0] = CASE_UNKNOWN;
        queued[0] = true;
        while (pending != 0) {
            uint32_t b = f->queue[start];
            start = (start + 1) % f->count;
            pending--;
            queued[b] = false;
            uint32_t out = last_write[b] == MO_NO_INDEX ? facts[b] : CASE_UNKNOWN;
            uint32_t pc = f->blocks[b].last;
            bool switches =
                (fn->code[pc] & MO_OPCODE_MASK) == MO_OP_SWITCH && fn->code[pc + 1] == r;
            for (uint32_t i = 0; i < edge_count(f, b); i++) {
                if (!charge(f, pc, 1)) {
                    return false;
                }
                uint32_t to = edge(f, b, i), incoming = switches ? i : out;
                uint32_t merged = facts[to] == CASE_UNREACHED ? incoming
                                  : facts[to] == incoming     ? incoming
                                                              : CASE_UNKNOWN;
                if (merged != facts[to]) {
                    facts[to] = merged;
                    if (!queued[to]) {
                        MO_ASSERT(pending < f->count);
                        f->queue[end] = to;
                        end = (end + 1) % f->count;
                        pending++;
                        queued[to] = true;
                    }
                }
            }
        }
        for (uint32_t b = 0; b < f->count; b++) {
            uint32_t fact = facts[b];
            for (uint32_t pc = f->blocks[b].start; pc < f->blocks[b].end; pc = next(fn, pc)) {
                const struct mo_opcode_info *op = mo_opcode_find(fn->code[pc] & MO_OPCODE_MASK);
                MO_ASSERT(op != NULL);
                uint32_t number = op->number;
                if ((number == MO_OP_CASE_FIELD || number == MO_OP_RESET) &&
                    fn->code[pc + 2] == r) {
                    f->known[pc] = fact;
                }
                for (uint32_t i = 0; i < op->fixed_count; i++) {
                    if ((op->fixed[i].access == MO_ACCESS_WRITE ||
                         op->fixed[i].access == MO_ACCESS_TOKEN_WRITE) &&
                        fn->code[pc + 1 + i] == r) {
                        fact = CASE_UNKNOWN;
                    }
                }
                if (!charge(f, pc, 1U + op->fixed_count)) {
                    return false;
                }
            }
        }
    }
    for (uint32_t pc = 0; pc < fn->code_count; pc = next(fn, pc)) {
        uint32_t op = fn->code[pc] & MO_OPCODE_MASK;
        if (op == MO_OP_CASE_FIELD && f->known[pc] != fn->code[pc + 3]) {
            return refuse(f, pc, "case is not known");
        }
        if (op == MO_OP_RESET || op == MO_OP_MAKE_CASE || op == MO_OP_MAKE_CLOSURE) {
            uint32_t type, tag;
            if (op == MO_OP_RESET) {
                type = fn->register_types[fn->code[pc + 2]];
                tag = f->v.file->types[type].kind == MO_TYPE_CHOICE ? f->known[pc] : MO_NO_INDEX;
            } else if (op == MO_OP_MAKE_CASE) {
                type = fn->code[pc + 2];
                tag = fn->code[pc + 3];
            } else {
                type = fn->register_types[fn->code[pc + 1]];
                tag = fn->code[pc + 2];
            }
            uint32_t l = layout(f, type, tag);
            if (op == MO_OP_RESET && l == MO_NO_INDEX) {
                return refuse(f, pc, "reset needs a known fixed layout");
            }
            /* Inline/empty/ref cases have no fixed block; only boxed cases need a layout. */
            if (op == MO_OP_MAKE_CLOSURE && l == MO_NO_INDEX) {
                return refuse(f, pc, "closure layout is missing");
            }
            if (op == MO_OP_MAKE_CASE && l == MO_NO_INDEX) {
                if (mo_case_shape(f->types, type, tag) == MO_CASE_BOXED) {
                    return refuse(f, pc, "boxed case layout is missing");
                }
            }
            f->reset[pc] = l;
        }
    }
    return true;
}

static MO_MUST_USE enum mo_operand_access access(const struct flow *f, uint32_t pc, uint32_t i) {
    const struct mo_bytecode_function *fn = f->v.fn;
    const struct mo_opcode_info *op = mo_opcode_find(fn->code[pc] & MO_OPCODE_MASK);
    MO_ASSERT(op != NULL);
    if (i < op->fixed_count) {
        return op->fixed[i].access;
    }
    uint32_t parameter = i - op->fixed_count;
    if (op->variable.access == MO_ACCESS_CALL_PARAMETER) {
        uint32_t target = fn->code[pc + (op->number == MO_OP_CALL ? 2U : 1U)];
        return f->v.file->functions[target].parameters[parameter].ownership == MO_PARAMETER_OWNED
                   ? MO_ACCESS_CONSUME
                   : MO_ACCESS_READ;
    }
    if (op->variable.access == MO_ACCESS_PRIM_PARAMETER ||
        op->variable.access == MO_ACCESS_HOST_PARAMETER) {
        const char *modes = mo_verify_parameter_modes(fn->code + pc);
        MO_ASSERT(modes != NULL);
        return modes[parameter] == 'o' ? MO_ACCESS_CONSUME : MO_ACCESS_READ;
    }
    return op->variable.access;
}

/* The ownership walk's registers: a state per register and one more for an init's global (set or
 * not), the instruction that last read and last consumed each register, and the undo log of the
 * slots changed on the current path. */
struct registers {
    uint32_t *state, *seen, *consumed;
    struct undo *undo;
    uint32_t changes;
};

/* The ownership transfer of the instruction at pc: checks its reads against the states, then
 * records each slot it changes in the undo log before changing it. */
static MO_MUST_USE bool step(struct flow *f, struct registers *regs, uint32_t pc) {
    const struct mo_bytecode_function *fn = f->v.fn;
    uint32_t op = fn->code[pc] & MO_OPCODE_MASK;
    const struct mo_opcode_info *info = mo_opcode_find(op);
    MO_ASSERT(info != NULL);
    const uint32_t *a = fn->code + pc + 1;
    uint32_t count = info->fixed_count + (fn->code[pc] >> 16), move = 0;
    if (f->program != NULL && fn->kind == MO_FUNCTION_INIT) {
        uint32_t needed = 0;
        if (op == MO_OP_GLOBAL) {
            needed = f->program->global_rank[a[1]];
        }
        if (op == MO_OP_CALL || op == MO_OP_TAIL_CALL) {
            needed = f->program->required[a[op == MO_OP_CALL ? 1 : 0]];
        }
        uint32_t available = f->program->init_rank[f->index] - 1 + regs->state[fn->register_count];
        if (needed > available) {
            return refuse(f, pc, "init reads an unset global");
        }
    }
    if (f->readable != NULL && pc == f->replay) {
        for (uint32_t r = 0; r < fn->register_count; r++) {
            f->readable[r] =
                regs->state[r] == VALUE || regs->state[r] == OWNED || regs->state[r] == BORROWED;
        }
    }
    if (f->visit != NULL && !f->visit(pc, f->readable, f->context)) {
        return refuse(f, pc, "replay visitor stopped");
    }
    for (uint32_t i = 0; i < count; i++) {
        enum mo_operand_access mode = access(f, pc, i);
        if (mode == MO_ACCESS_NONE || mode == MO_ACCESS_WRITE || mode == MO_ACCESS_TOKEN_WRITE) {
            continue;
        }
        uint32_t r = a[i];
        MO_ASSERT(r < fn->register_count);
        uint32_t s = regs->state[r];
        if (!charge(f, pc, 1)) {
            return false;
        }
        bool consumes = mode == MO_ACCESS_TOKEN_CONSUME || mode == MO_ACCESS_CONSUME ||
                        (mode == MO_ACCESS_MOVE && s == OWNED);
        if (mode == MO_ACCESS_TOKEN_CONSUME) {
            if (s < TOKEN) {
                return refuse(f, pc + 1 + i, "register is not a token");
            }
        } else if (s != VALUE && s != OWNED && s != BORROWED) {
            return refuse(f, pc + 1 + i, "register is not readable");
        }
        if (mode == MO_ACCESS_MOVE) {
            move = s;
        }
        bool reference = f->v.file->types[fn->register_types[r]].reference;
        if (consumes && reference && mode != MO_ACCESS_TOKEN_CONSUME && s != OWNED) {
            return refuse(f, pc + 1 + i, "consume needs an owned reference");
        }
        if (reference || mode == MO_ACCESS_TOKEN_CONSUME) {
            if ((regs->seen[r] == pc + 1 && consumes) || regs->consumed[r] == pc + 1) {
                return refuse(f, pc + 1 + i, "consumed register is repeated");
            }
            regs->seen[r] = pc + 1;
            if (consumes) {
                regs->consumed[r] = pc + 1;
            }
        }
        if (op == MO_OP_TAIL_CALL && i >= info->fixed_count && mode == MO_ACCESS_READ &&
            reference && s != BORROWED) {
            return refuse(f, pc + 1 + i, "tail borrow needs a borrowed reference");
        }
    }
    if (op == MO_OP_REUSE && regs->state[a[1]] != TOKEN + a[2]) {
        return refuse(f, pc, "reuse layout differs from token");
    }
    for (uint32_t i = 0; i < count; i++) {
        enum mo_operand_access mode = access(f, pc, i);
        if (mode == MO_ACCESS_TOKEN_CONSUME ||
            ((mode == MO_ACCESS_CONSUME || mode == MO_ACCESS_MOVE) &&
             f->v.file->types[fn->register_types[a[i]]].reference && regs->state[a[i]] == OWNED)) {
            MO_ASSERT(regs->changes < fn->code_count);
            regs->undo[regs->changes++] = (struct undo){a[i], regs->state[a[i]]};
            regs->state[a[i]] = MOVED;
            if (f->visit != NULL) {
                f->readable[a[i]] = false;
            }
        }
    }
    for (uint32_t i = 0; i < info->fixed_count; i++) {
        enum mo_operand_access mode = info->fixed[i].access;
        if (mode != MO_ACCESS_WRITE && mode != MO_ACCESS_TOKEN_WRITE) {
            continue;
        }
        uint32_t r = a[i];
        if (regs->state[r] == OWNED || regs->state[r] >= TOKEN) {
            return refuse(f, pc + 1 + i, "write overwrites ownership or token");
        }
        MO_ASSERT(regs->changes < fn->code_count);
        regs->undo[regs->changes++] = (struct undo){r, regs->state[r]};
        regs->state[r] = mode == MO_ACCESS_TOKEN_WRITE ? TOKEN + f->reset[pc]
                         : op == MO_OP_CLEAR           ? UNDEFINED
                         : op == MO_OP_MOVE            ? move
                         : f->v.file->types[fn->register_types[r]].reference
                             ? op == MO_OP_CONST || op == MO_OP_GLOBAL ? BORROWED : OWNED
                             : VALUE;
        /* The VM's write() leaves a value, owned or borrowed register; `clear` and token writes
         * are the other cases. */
        MO_ASSERT(mode == MO_ACCESS_TOKEN_WRITE || op == MO_OP_CLEAR || regs->state[r] == VALUE ||
                  regs->state[r] == OWNED || regs->state[r] == BORROWED);
        if (f->visit != NULL) {
            f->readable[r] =
                regs->state[r] == VALUE || regs->state[r] == OWNED || regs->state[r] == BORROWED;
        }
    }
    if (op == MO_OP_SET_GLOBAL) {
        if (regs->state[fn->register_count] != 0) {
            return refuse(f, pc, "init sets its global twice");
        }
        MO_ASSERT(regs->changes < fn->code_count);
        regs->undo[regs->changes++] = (struct undo){fn->register_count, 0};
        regs->state[fn->register_count] = 1;
    }
    if (op == MO_OP_RET || op == MO_OP_TAIL_CALL || op == MO_OP_TAIL_CALL_CLOSURE) {
        for (uint32_t r = 0; r < fn->register_count; r++) {
            if (!charge(f, pc, 1)) {
                return false;
            }
            if (regs->state[r] == OWNED || regs->state[r] >= TOKEN) {
                return refuse(f, pc, "return leaves ownership or token");
            }
        }
        if (fn->kind == MO_FUNCTION_INIT && regs->state[fn->register_count] == 0) {
            return refuse(f, pc, "init returns without setting its global");
        }
    }
    return true;
}

static MO_MUST_USE bool ownership(struct flow *f) {
    const struct mo_bytecode_function *fn = f->v.fn;
    size_t slots = (size_t)fn->register_count + 1;
    struct registers s = {.state = array(f, slots, sizeof *s.state, _Alignof(uint32_t))};
    s.seen = array(f, fn->register_count, sizeof *s.seen, _Alignof(uint32_t));
    s.consumed = array(f, fn->register_count, sizeof *s.consumed, _Alignof(uint32_t));
    struct walk *stack = array(f, f->count, sizeof *stack, _Alignof(struct walk));
    s.undo = array(f, fn->code_count, sizeof *s.undo, _Alignof(struct undo));
    uint32_t *state = s.state;
    if (state == NULL || s.seen == NULL || s.consumed == NULL || stack == NULL || s.undo == NULL) {
        return false;
    }
    memset(state, 0, slots * sizeof *state);
    memset(s.seen, 0, fn->register_count * sizeof *s.seen);
    memset(s.consumed, 0, fn->register_count * sizeof *s.consumed);
    for (uint32_t r = 0; r < fn->parameter_count; r++) {
        state[r] = f->v.file->types[fn->register_types[r]].reference
                       ? fn->parameters[r].ownership == MO_PARAMETER_OWNED ? OWNED : BORROWED
                       : VALUE;
    }
    if (fn->kind == MO_FUNCTION_CLOSURE) {
        state[fn->parameter_count] = OWNED;
    }
    if (f->visit != NULL) {
        for (uint32_t r = 0; r < fn->register_count; r++) {
            f->readable[r] = state[r] == VALUE || state[r] == OWNED || state[r] == BORROWED;
        }
    }
    /* Depth-first traversal restores only changed slots. Full vectors survive only at joins
     * (and a loop back to entry); every block executes once, regardless of its path count. */
    uint32_t depth = 1;
    stack[0] = (struct walk){0, 0, 0};
    while (depth != 0) {
        struct walk *frame = &stack[depth - 1];
        uint32_t b = frame->block;
        if (!f->blocks[b].visited) {
            f->blocks[b].visited = true;
            if (f->blocks[b].incoming > 1 || (b == 0 && f->blocks[b].incoming != 0)) {
                if (!charge(f, f->blocks[b].start, slots)) {
                    return false;
                }
                f->blocks[b].state =
                    mo_arena_copy(&f->arena, state, slots * sizeof *state, _Alignof(uint32_t));
                if (f->blocks[b].state == NULL) {
                    *f->v.error = (struct mo_load_error){.reason = "no memory",
                                                         .kind = MO_BYTECODE_NO_MEMORY};
                    return false;
                }
            }
            for (uint32_t pc = f->blocks[b].start; pc < f->blocks[b].end; pc = next(fn, pc)) {
                if (!step(f, &s, pc)) {
                    return false;
                }
            }
        }
        if (frame->edge < edge_count(f, b)) {
            uint32_t to = edge(f, b, frame->edge++);
            if (!f->blocks[to].visited) {
                MO_ASSERT(depth < f->count);
                stack[depth++] = (struct walk){to, 0, s.changes};
            } else {
                MO_ASSERT(f->blocks[to].state != NULL);
                if (!charge(f, f->blocks[to].start, slots)) {
                    return false;
                }
                if (memcmp(state, f->blocks[to].state, slots * sizeof *state) != 0) {
                    return refuse(f, f->blocks[to].start, "register states disagree at join");
                }
            }
        } else {
            while (s.changes != frame->mark) {
                struct undo change = s.undo[--s.changes];
                state[change.reg] = change.state;
                if (f->visit != NULL && change.reg < fn->register_count) {
                    f->readable[change.reg] =
                        change.state == VALUE || change.state == OWNED || change.state == BORROWED;
                }
            }
            depth--;
        }
    }
    return true;
}

static MO_MUST_USE bool function(struct flow *f) {
    size_t words = (size_t)f->v.fn->register_count + f->v.fn->code_count + 32;
    f->work_limit = (uint64_t)MO_LIMIT_VERIFY_WORK_PER_WORD * words;
    mo_arena_init(&f->arena, MO_LIMIT_VERIFY_BYTES_PER_WORD * words);
    bool ok = graph(f) && cases(f) && ownership(f);
    mo_arena_free(&f->arena);
    return ok;
}

bool mo_bytecode_verify(const struct mo_bytecode *file, const struct mo_types *types,
                        const uint32_t *code_offsets, size_t wire_size, struct mo_arena *arena,
                        struct mo_verification *out, struct mo_bytecode_error_site *site,
                        struct mo_load_error *error) {
    uint8_t *abilities = mo_bytecode_abilities(arena, file->types, file->type_count);
    struct layout_key *layouts = mo_arena_allocate_array(arena, file->layout_count, sizeof *layouts,
                                                         _Alignof(struct layout_key));
    if (abilities == NULL || layouts == NULL) {
        *error = (struct mo_load_error){.reason = "no memory", .kind = MO_BYTECODE_NO_MEMORY};
        return false;
    }
    for (uint32_t i = 0; i < file->type_count; i++) {
        const struct mo_type *t = &file->types[i];
        if ((t->kind == MO_TYPE_SET || t->kind == MO_TYPE_MAP) &&
            (abilities[t->parameters[0]] & MO_ABILITY_HASH) == 0) {
            if (site != NULL)
                *site = (struct mo_bytecode_error_site){MO_SECTION_TYPES, i};
            *error = (struct mo_load_error){20, "map/set key lacks Hash", MO_BYTECODE_INVALID};
            return false;
        }
    }
    for (uint32_t i = 0; i < file->function_count; i++) {
        const struct mo_bytecode_function *fn = &file->functions[i];
        if (fn->kind != MO_FUNCTION_PROMISE || file->debug[i].subject == MO_NO_INDEX) {
            continue;
        }
        const struct mo_bytecode_function *subject = &file->functions[file->debug[i].subject];
        bool matches = fn->parameter_count == subject->parameter_count + 1;
        for (uint32_t j = 0; j < subject->parameter_count && matches; j++) {
            matches = fn->parameters[j].type == subject->parameters[j].type;
        }
        if (!matches || fn->parameters[subject->parameter_count].type != subject->result_type) {
            if (site != NULL)
                *site = (struct mo_bytecode_error_site){MO_SECTION_FUNCTIONS, i};
            *error = (struct mo_load_error){20, "promise parameters differ from subject",
                                            MO_BYTECODE_INVALID};
            return false;
        }
    }
    for (uint32_t i = 0; i < file->layout_count; i++) {
        const struct mo_layout *l = &file->layouts[i];
        layouts[i] = (struct layout_key){l->type,
                                         l->kind == MO_LAYOUT_CASE      ? l->case_index
                                         : l->kind == MO_LAYOUT_CLOSURE ? l->function
                                                                        : MO_NO_INDEX,
                                         i};
    }
    qsort(layouts, file->layout_count, sizeof *layouts, layout_order);
    struct flow lookup = {.v = {.file = file}, .layouts = layouts, .types = types};
    for (uint32_t i = 0; i < file->constant_count; i++) {
        const struct mo_bytecode_constant *c = &file->constants[i];
        if (file->types[c->type].kind == MO_TYPE_FUNCTION &&
            layout(&lookup, c->type, c->value.function) == MO_NO_INDEX) {
            if (site != NULL)
                *site = (struct mo_bytecode_error_site){MO_SECTION_CONSTANTS, i};
            *error = (struct mo_load_error){20, "closure constant layout is missing",
                                            MO_BYTECODE_INVALID};
            return false;
        }
    }
    struct mo_verify_program program;
    struct mo_arena scratch;
    mo_arena_init(&scratch, mo_bytecode_work_budget(wire_size));
    bool ok = mo_verify_program_init(&scratch, file, code_offsets, &program, site, error);
    for (uint32_t i = 0; i < file->function_count && ok; i++) {
        struct flow f = {
            .v = {file, &file->functions[i], abilities, code_offsets[i], error, types, layouts},
            .layouts = layouts,
            .types = types,
            .program = &program,
            .index = i};
        ok = function(&f);
    }
    mo_arena_free(&scratch);
    *out = (struct mo_verification){abilities, layouts};
    return ok;
}

bool mo_bytecode_readable(const struct mo_bytecode *file, const struct mo_types *types,
                          const struct mo_verification *verified, uint32_t index, uint32_t base,
                          uint32_t pc, bool *readable, uint64_t *work,
                          struct mo_load_error *error) {
    MO_ASSERT(file != NULL && types != NULL && index < file->function_count && readable != NULL &&
              work != NULL && error != NULL);
    const struct mo_bytecode_function *fn = &file->functions[index];
    bool boundary = false;
    for (uint32_t at = 0; at < fn->code_count; at = next(fn, at)) {
        boundary |= at == pc;
    }
    if (!boundary) {
        *error = (struct mo_load_error){.reason = "invalid replay instruction"};
        return false;
    }
    struct flow f = {.v = {file, fn, verified->abilities, base, error, types, verified->layouts},
                     .layouts = verified->layouts,
                     .types = types,
                     .replay = pc,
                     .readable = readable};
    bool ok = function(&f);
    *work = f.work;
    return ok;
}

bool mo_bytecode_visit_readable(const struct mo_bytecode *file, const struct mo_types *types,
                                const struct mo_verification *verified, uint32_t index,
                                uint32_t base, bool *readable, mo_readable_visitor visit,
                                void *context, struct mo_load_error *error) {
    MO_ASSERT(file != NULL && types != NULL && index < file->function_count && readable != NULL &&
              visit != NULL && error != NULL);
    struct flow f = {.v = {file, &file->functions[index], verified->abilities, base, error, types,
                           verified->layouts},
                     .layouts = verified->layouts,
                     .types = types,
                     .replay = MO_NO_INDEX,
                     .readable = readable,
                     .visit = visit,
                     .context = context};
    return function(&f);
}
