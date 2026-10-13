#include "codegen/registers.h"

#include <string.h>

#include "base/codes.h"
#include "codegen/operations.h"
#include "heap/types.h"

struct interval {
    uint32_t first, last, block, slot, next, replaced, written;
    bool pinned, closed;
};
struct slot {
    uint32_t type, next, previous, block, clear_next;
};
struct allocation {
    const struct mo_bytecode *context;
    const struct mo_bytecode_function *function;
    struct interval *intervals;
    struct slot *slots;
    bool *starts;
    uint32_t *heads, *ends, *offsets, *code;
    uint32_t count, used, capacity, block, clear;
    uint64_t work;
    uint16_t error;
};
static MO_MUST_USE bool charge(struct allocation *a, uint64_t work) {
    if (work > MO_LIMIT_CODEGEN_REGISTER_WORK - a->work) {
        a->error = MO_CODE_CODEGEN_REGISTER_WORK;
        return false;
    }
    a->work += work;
    return true;
}
static MO_MUST_USE uint32_t width(const struct allocation *a, uint32_t pc) {
    const struct mo_opcode_info *op = mo_opcode_find(a->function->code[pc] & MO_OPCODE_MASK);
    MO_ASSERT(op != NULL);
    uint32_t n = 1u + op->fixed_count + (a->function->code[pc] >> MO_OPCODE_VARIABLE_SHIFT);
    MO_ASSERT(n <= a->function->code_count - pc);
    return n;
}
static MO_MUST_USE bool terminal(uint32_t op) {
    return op == MO_OP_RET || op == MO_OP_STOP || op == MO_OP_FAIL || op == MO_OP_TAIL_CALL ||
           op == MO_OP_TAIL_CALL_CLOSURE;
}
static MO_MUST_USE bool transfer(uint32_t op) {
    return terminal(op) || op == MO_OP_JUMP || op == MO_OP_BRANCH || op == MO_OP_SWITCH;
}
static const char *const host_params[] = {
#define HOSTOP(id, module, name, signature, params, bugs, spec_only) params,
#include "base/hostops.def"
};
static MO_MUST_USE enum mo_operand_access access(const struct allocation *a, uint32_t pc,
                                                 uint32_t i) {
    const uint32_t *code = a->function->code + pc;
    const struct mo_opcode_info *op = mo_opcode_find(code[0] & MO_OPCODE_MASK);
    MO_ASSERT(op != NULL);
    if (i < op->fixed_count)
        return op->fixed[i].access;
    uint32_t parameter = i - op->fixed_count;
    if (op->variable.access == MO_ACCESS_CALL_PARAMETER) {
        uint32_t callee = code[op->number == MO_OP_CALL ? 2 : 1];
        return a->context->functions[callee].parameters[parameter].ownership == MO_PARAMETER_OWNED
                   ? MO_ACCESS_CONSUME
                   : MO_ACCESS_READ;
    }
    if (op->variable.access == MO_ACCESS_PRIM_PARAMETER)
        return mo_codegen_primitive((enum mo_primitive_id)code[2]).params[parameter] == 'o'
                   ? MO_ACCESS_CONSUME
                   : MO_ACCESS_READ;
    if (op->variable.access == MO_ACCESS_HOST_PARAMETER)
        return host_params[code[2]][parameter] == 'o' ? MO_ACCESS_CONSUME : MO_ACCESS_READ;
    return op->variable.access;
}
static MO_MUST_USE bool scan(struct allocation *a, uint32_t prologue) {
    const struct mo_bytecode_function *f = a->function;
    a->starts[0] = true;
    for (uint32_t pc = 0; pc < f->code_count;) {
        uint32_t n = width(a, pc), op = f->code[pc] & MO_OPCODE_MASK;
        const struct mo_opcode_info *info = mo_opcode_find(op);
        MO_ASSERT(info != NULL);
        if (!charge(a, n))
            return false;
        if (transfer(op) && pc + n < f->code_count)
            a->starts[pc + n] = true;
        for (uint32_t i = 0; i + 1 < n; i++) {
            struct mo_operand_info operand =
                i < info->fixed_count ? info->fixed[i] : info->variable;
            if (operand.kind == MO_OPERAND_TARGET) {
                MO_ASSERT(f->code[pc + 1 + i] < f->code_count);
                a->starts[f->code[pc + 1 + i]] = true;
            }
        }
        pc += n;
    }
    uint32_t block = 0;
    for (uint32_t pc = 0; pc < f->code_count;) {
        uint32_t n = width(a, pc), op = f->code[pc] & MO_OPCODE_MASK;
        const struct mo_opcode_info *info = mo_opcode_find(op);
        MO_ASSERT(info != NULL);
        if (a->starts[pc])
            block++;
        if (!charge(a, UINT64_C(2) * n))
            return false;
        /* Reads precede writes, even when the same register occurs on both sides. */
        for (unsigned writing = 0; writing < 2; writing++) {
            for (uint32_t i = 0; i + 1 < n; i++) {
                struct mo_operand_info operand =
                    i < info->fixed_count ? info->fixed[i] : info->variable;
                if (operand.kind != MO_OPERAND_REGISTER)
                    continue;
                enum mo_operand_access mode = access(a, pc, i);
                bool write = mode == MO_ACCESS_WRITE || mode == MO_ACCESS_TOKEN_WRITE;
                if (write != (writing != 0))
                    continue;
                uint32_t r = f->code[pc + 1 + i];
                MO_ASSERT(r < f->register_count);
                struct interval *v = &a->intervals[r];
                if (pc < prologue) {
                    if (!write)
                        v->pinned = true;
                    continue;
                }
                if (v->first == MO_NO_INDEX) {
                    v->first = pc;
                    v->written = pc + n;
                    v->block = block;
                    if (!write)
                        v->pinned = true;
                } else if (v->block != block)
                    v->pinned = true;
                if (mode == MO_ACCESS_TOKEN_WRITE || mode == MO_ACCESS_TOKEN_CONSUME)
                    v->pinned = true;
                bool ref = a->context->types[f->register_types[r]].reference;
                if (ref && transfer(op) && !terminal(op))
                    v->pinned = true;
                v->last = pc;
                v->closed =
                    mode == MO_ACCESS_CONSUME || mode == MO_ACCESS_MOVE ||
                    (write && (op == MO_OP_CLEAR || op == MO_OP_CONST || op == MO_OP_GLOBAL));
            }
        }
        pc += n;
    }
    uint32_t inputs = f->parameter_count + (f->kind == MO_FUNCTION_CLOSURE ? 1u : 0u);
    if (!charge(a, f->register_count))
        return false;
    for (uint32_t r = 0; r < f->register_count; r++) {
        struct interval *v = &a->intervals[r];
        if (r < inputs)
            v->pinned = true;
        if (v->first == MO_NO_INDEX && !v->pinned)
            continue;
        if (a->context->types[f->register_types[r]].reference && !v->closed)
            v->pinned = true;
        if (v->pinned) {
            if (a->count == MO_LIMIT_FUNCTION_REGISTERS) {
                a->error = MO_CODE_TOO_MANY_REGISTERS;
                return false;
            }
            v->slot = a->count;
            a->slots[a->count++] = (struct slot){.type = f->register_types[r]};
        } else {
            v->next = a->ends[v->last];
            a->ends[v->last] = r;
        }
    }
    return true;
}
static MO_MUST_USE bool append(struct allocation *a, const uint32_t *words, uint32_t count) {
    if (!charge(a, count))
        return false;
    if (count > a->capacity - a->used) {
        a->error = MO_CODE_FUNCTION_TOO_LONG;
        return false;
    }
    memcpy(a->code + a->used, words, (size_t)count * sizeof *words);
    a->used += count;
    return true;
}
static MO_MUST_USE bool clear(struct allocation *a) {
    for (uint32_t r = a->clear; r != MO_NO_INDEX; r = a->slots[r].clear_next) {
        MO_ASSERT(a->slots[r].previous != MO_NO_INDEX);
        if (!append(a, (uint32_t[]){MO_OP_CLEAR, a->slots[r].previous}, 2))
            return false;
    }
    return true;
}
static MO_MUST_USE bool rewrite(struct allocation *a, uint32_t prologue) {
    const struct mo_bytecode_function *f = a->function;
    a->block = 0;
    a->clear = MO_NO_INDEX;
    for (uint32_t pc = 0; pc < f->code_count;) {
        uint32_t n = width(a, pc), op = f->code[pc] & MO_OPCODE_MASK;
        const struct mo_opcode_info *info = mo_opcode_find(op);
        MO_ASSERT(info != NULL);
        if (!charge(a, n + 1u))
            return false;
        if (a->starts[pc]) {
            a->block++;
            a->clear = MO_NO_INDEX;
        }
        a->offsets[pc] = a->used;
        bool unused = false;
        for (uint32_t i = 0; i + 1 < n; i++) {
            struct mo_operand_info operand =
                i < info->fixed_count ? info->fixed[i] : info->variable;
            if (operand.kind != MO_OPERAND_REGISTER)
                continue;
            uint32_t r = f->code[pc + 1 + i];
            struct interval *v = &a->intervals[r];
            if (v->first == MO_NO_INDEX && !v->pinned) {
                MO_ASSERT(pc < prologue && operand.access == MO_ACCESS_WRITE);
                unused = true;
            }
            if (v->first != pc || v->pinned || v->slot != MO_NO_INDEX)
                continue;
            uint32_t ty = f->register_types[r], slot = a->heads[ty];
            if (slot == MO_NO_INDEX) {
                if (a->count == MO_LIMIT_FUNCTION_REGISTERS) {
                    a->error = MO_CODE_TOO_MANY_REGISTERS;
                    return false;
                }
                slot = a->count++;
                a->slots[slot] = (struct slot){.type = ty, .previous = MO_NO_INDEX};
            } else {
                a->heads[ty] = a->slots[slot].next;
                a->intervals[a->slots[slot].previous].replaced = pc;
            }
            v->slot = slot;
            if (a->context->types[ty].reference && a->slots[slot].block != a->block) {
                a->slots[slot].block = a->block;
                a->slots[slot].clear_next = a->clear;
                a->clear = slot;
            }
        }
        bool end = pc + n == f->code_count || a->starts[pc + n];
        if (end && transfer(op) && !terminal(op) && !clear(a))
            return false;
        /* Keep virtual operands until the complete plan exists: prologue writes
         * precede local allocation. Inserted clears name the last retired virtual. */
        if (!unused && !append(a, f->code + pc, n))
            return false;
        for (uint32_t r = a->ends[pc]; r != MO_NO_INDEX; r = a->intervals[r].next) {
            uint32_t slot = a->intervals[r].slot, ty = a->slots[slot].type;
            a->slots[slot].previous = r;
            a->slots[slot].next = a->heads[ty];
            a->heads[ty] = slot;
        }
        if (end && !transfer(op) && !clear(a))
            return false;
        pc += n;
    }
    a->offsets[f->code_count] = a->used;
    return true;
}
uint16_t mo_codegen_registers(struct mo_arena *arena, const struct mo_bytecode *context,
                              struct mo_bytecode_function *function,
                              struct mo_bytecode_debug *debug, uint32_t prologue) {
    struct mo_arena temporary;
    mo_arena_init(&temporary, MO_LIMIT_CODEGEN_SCRATCH_SIZE);
    struct allocation a = {.context = context, .function = function};
    uint32_t registers = function->register_count, words = function->code_count;
    uint32_t slots =
        registers < MO_LIMIT_FUNCTION_REGISTERS ? registers : MO_LIMIT_FUNCTION_REGISTERS;
    uint64_t capacity = (uint64_t)words + UINT64_C(2) * registers;
    a.capacity =
        capacity < MO_LIMIT_FUNCTION_CODE_WORDS ? (uint32_t)capacity : MO_LIMIT_FUNCTION_CODE_WORDS;
#define ARRAY(field, count)                                                                        \
    a.field = mo_arena_allocate_array(&temporary, count, sizeof *a.field, _Alignof(max_align_t))
    ARRAY(intervals, registers);
    ARRAY(slots, slots);
    ARRAY(starts, words);
    ARRAY(heads, context->type_count);
    ARRAY(ends, words);
    ARRAY(offsets, (size_t)words + 1);
    ARRAY(code, a.capacity);
#undef ARRAY
    uint16_t result = MO_CODE_CODEGEN_MEMORY;
    if (a.intervals != NULL && a.slots != NULL && a.starts != NULL && a.heads != NULL &&
        a.ends != NULL && a.offsets != NULL && a.code != NULL &&
        charge(&a, (uint64_t)registers + UINT64_C(2) * words + context->type_count)) {
        memset(a.starts, 0, (size_t)words * sizeof *a.starts);
        memset(a.ends, 255, (size_t)words * sizeof *a.ends);
        memset(a.heads, 255, (size_t)context->type_count * sizeof *a.heads);
        for (uint32_t r = 0; r < registers; r++)
            a.intervals[r] =
                (struct interval){.first = MO_NO_INDEX, .slot = MO_NO_INDEX, .replaced = words};
        if (scan(&a, prologue) && rewrite(&a, prologue) &&
            charge(&a, UINT64_C(2) * words + UINT64_C(2) * a.used + a.count + debug->span_count +
                           UINT64_C(2) * debug->name_count)) {
            uint32_t blocks = 0;
            for (uint32_t pc = 0; pc < words; pc += width(&a, pc))
                blocks += a.starts[pc] ? 1u : 0u;
            if ((uint64_t)blocks * a.count > MO_LIMIT_FUNCTION_REGISTER_BLOCKS)
                result = MO_CODE_BYTECODE_TOO_BIG;
            else {
                uint32_t *code =
                    mo_arena_copy(arena, a.code, (size_t)a.used * sizeof *code, _Alignof(uint32_t));
                uint32_t *types =
                    mo_arena_allocate_array(arena, a.count, sizeof *types, _Alignof(uint32_t));
                struct mo_bytecode_span *spans = mo_arena_allocate_array(
                    arena, debug->span_count, sizeof *spans, _Alignof(struct mo_bytecode_span));
                struct mo_bytecode_register_name *name_ranges =
                    mo_arena_allocate_array(arena, debug->name_count, sizeof *name_ranges,
                                            _Alignof(struct mo_bytecode_register_name));
                if (code != NULL && types != NULL && spans != NULL && name_ranges != NULL) {
                    for (uint32_t i = 0; i < a.count; i++)
                        types[i] = a.slots[i].type;
                    for (uint32_t pc = 0; pc < a.used;) {
                        const struct mo_opcode_info *op = mo_opcode_find(code[pc] & MO_OPCODE_MASK);
                        MO_ASSERT(op != NULL);
                        uint32_t n = 1u + op->fixed_count + (code[pc] >> MO_OPCODE_VARIABLE_SHIFT);
                        for (uint32_t i = 0; i + 1 < n; i++) {
                            struct mo_operand_info operand =
                                i < op->fixed_count ? op->fixed[i] : op->variable;
                            if (operand.kind == MO_OPERAND_TARGET)
                                code[pc + 1 + i] = a.offsets[code[pc + 1 + i]];
                            else if (operand.kind == MO_OPERAND_REGISTER) {
                                uint32_t r = code[pc + 1 + i];
                                MO_ASSERT(a.intervals[r].slot != MO_NO_INDEX);
                                code[pc + 1 + i] = a.intervals[r].slot;
                            }
                        }
                        pc += n;
                    }
                    uint32_t span_count = 0, source = 0;
                    for (uint32_t pc = 0; pc < words && debug->span_count != 0;) {
                        while (source + 1 < debug->span_count &&
                               debug->spans[source + 1].code_offset <= pc)
                            source++;
                        uint32_t next = pc + width(&a, pc);
                        if (a.offsets[pc] != a.offsets[next]) {
                            struct mo_bytecode_span s = debug->spans[source];
                            if (span_count == 0 || spans[span_count - 1].file != s.file ||
                                spans[span_count - 1].start != s.start ||
                                spans[span_count - 1].end != s.end) {
                                s.code_offset = a.offsets[pc];
                                spans[span_count++] = s;
                            }
                        }
                        pc = next;
                    }
                    uint32_t names = 0;
                    for (uint32_t i = 0; i < debug->name_count; i++) {
                        struct mo_bytecode_register_name name = debug->names[i];
                        struct interval *v = &a.intervals[name.reg];
                        if (v->slot == MO_NO_INDEX)
                            continue;
                        if (!v->pinned) {
                            if (name.code_start < v->written)
                                name.code_start = v->written;
                            if (name.code_end > v->replaced)
                                name.code_end = v->replaced;
                        }
                        if (name.code_start >= name.code_end)
                            continue;
                        name.reg = v->slot;
                        name.code_start = a.offsets[name.code_start];
                        name.code_end = a.offsets[name.code_end];
                        if (name.code_start < name.code_end)
                            name_ranges[names++] = name;
                    }
                    debug->name_count = names;
                    debug->names = name_ranges;
                    debug->span_count = span_count;
                    debug->spans = spans;
                    function->code = code;
                    function->code_count = a.used;
                    function->register_types = types;
                    function->register_count = a.count;
                    result = 0;
                }
            }
        } else
            result = a.error;
    } else if (a.error != 0)
        result = a.error;
    mo_arena_free(&temporary);
    return result;
}
