#include "vm/internal.h"

#include <errno.h>
#include <string.h>

static MO_MUST_USE struct mo_cell read(const struct mo_process *p, struct mo_vm_frame *f,
                                       uint32_t reg) {
    MO_ASSERT(reg < p->file->functions[f->function].register_count);
    uint8_t state = mo_vm_states(p, f)[reg];
    MO_ASSERT(state == VM_VALUE || state == VM_OWNED || state == VM_BORROWED);
    return mo_vm_cells(f)[reg];
}

static void consume(const struct mo_process *p, struct mo_vm_frame *f, uint32_t reg) {
    MO_ASSERT(reg < p->file->functions[f->function].register_count);
    uint32_t type = p->file->functions[f->function].register_types[reg];
    uint8_t *states = mo_vm_states(p, f);
    if (p->types->items[type].reference) {
        MO_ASSERT(states[reg] == VM_OWNED && f->owners != 0);
        f->owners--;
        states[reg] = VM_MOVED;
    } else
        MO_ASSERT(states[reg] == VM_VALUE);
}

static void write(const struct mo_process *p, struct mo_vm_frame *f, uint32_t reg,
                  struct mo_cell value, bool borrowed) {
    const struct mo_bytecode_function *fn = &p->file->functions[f->function];
    MO_ASSERT(reg < fn->register_count);
    uint8_t *states = mo_vm_states(p, f);
    MO_ASSERT(states[reg] != VM_OWNED && states[reg] != VM_TOKEN);
    mo_vm_cells(f)[reg] = value;
    states[reg] = p->types->items[fn->register_types[reg]].reference
                      ? (borrowed ? VM_BORROWED : VM_OWNED)
                      : VM_VALUE;
    f->owners += states[reg] == VM_OWNED ? 1u : 0u;
}

static void clean(const struct mo_vm_frame *f) {
    MO_ASSERT(f->owners == 0);
}

static MO_MUST_USE enum mo_outcome memory(struct mo_process *p) {
    return mo_vm_stop(p, "running out of memory", errno == ENOMEM ? NULL : "--max-heap", false,
                      mo_cell_unit(), MO_STOPPED);
}

/* The single primitive/host dispatch seam. Implementations added in the next
 * slices must preserve operands on refusal; no ownership changes precede it. */
static MO_MUST_USE const char *dispatch(uint32_t opcode) {
    return opcode == MO_OP_PRIM ? "internal error: primitive dispatch unavailable"
                                : "internal error: host dispatch unavailable";
}

enum mo_outcome mo_vm_execute(struct mo_process *p, struct mo_cell *result) {
    for (;;) {
        struct mo_vm_frame *f = mo_vm_frame(p);
        const struct mo_bytecode_function *fn = &p->file->functions[f->function];
        MO_ASSERT(f->pc < fn->code_count);
        if (p->bounded) {
            if (p->instructions == 0)
                return mo_vm_stop(p, "internal error: instruction budget exhausted", NULL, false,
                                  mo_cell_unit(), MO_STOPPED);
            p->instructions--;
        }
        uint32_t head = fn->code[f->pc], op = head & MO_OPCODE_MASK;
        uint32_t count = head >> MO_OPCODE_VARIABLE_SHIFT;
        const struct mo_opcode_info *info = mo_opcode_find(op);
        MO_ASSERT(info != NULL);
        const uint32_t *a = fn->code + f->pc + 1;
        uint32_t next = f->pc + 1 + info->fixed_count + count;
        struct mo_cell value = mo_cell_unit();
        if (op >= MO_OP_ADD_INT && op <= MO_OP_RANGE_END) {
            const char *reason = NULL;
            struct mo_cell left = read(p, f, a[1]);
            struct mo_cell right = info->fixed_count == 3 ? read(p, f, a[2]) : mo_cell_unit();
            if (!mo_vm_number(op, left, right, &value, &reason))
                return mo_vm_stop(p, reason, NULL, false, mo_cell_unit(), MO_STOPPED);
            write(p, f, a[0], value, false);
            f->pc = next;
            continue;
        }
        switch (op) {
        case MO_OP_MOVE: {
            value = read(p, f, a[1]);
            bool borrowed = mo_vm_states(p, f)[a[1]] == VM_BORROWED;
            if (!borrowed)
                consume(p, f, a[1]);
            write(p, f, a[0], value, borrowed);
            break;
        }
        case MO_OP_DUP:
            value = mo_value_dup(p->heap, fn->register_types[a[1]], read(p, f, a[1]));
            write(p, f, a[0], value, false);
            break;
        case MO_OP_DROP:
            value = read(p, f, a[0]);
            consume(p, f, a[0]);
            mo_value_drop(p->heap, fn->register_types[a[0]], value);
            break;
        case MO_OP_CONST:
            write(p, f, a[0], p->runtime->constants[a[1]], true);
            break;
        case MO_OP_GLOBAL:
            if (!p->runtime->set[a[1]])
                return mo_vm_stop(p, "internal error: unset global", NULL, false, mo_cell_unit(),
                                  MO_STOPPED);
            write(p, f, a[0], p->runtime->globals[a[1]], true);
            break;
        case MO_OP_SET_GLOBAL:
            value = read(p, f, a[1]);
            MO_ASSERT(!p->runtime->set[a[0]]);
            if (!mo_value_make_immortal(p->heap, fn->register_types[a[1]], value))
                return memory(p);
            consume(p, f, a[1]);
            p->runtime->globals[a[0]] = value;
            p->runtime->set[a[0]] = true;
            break;
        case MO_OP_JUMP:
            f->pc = a[0];
            continue;
        case MO_OP_BRANCH:
            f->pc = mo_cell_get_bool(read(p, f, a[0])) ? a[1] : a[2];
            continue;
        case MO_OP_SWITCH: {
            uint32_t tag = mo_cell_get_tag(read(p, f, a[0]));
            MO_ASSERT(tag < count);
            f->pc = a[1 + tag];
            continue;
        }
        case MO_OP_CALL:
        case MO_OP_CALL_CLOSURE:
        case MO_OP_TAIL_CALL:
        case MO_OP_TAIL_CALL_CLOSURE: {
            bool tail = op == MO_OP_TAIL_CALL || op == MO_OP_TAIL_CALL_CLOSURE;
            bool closure = op == MO_OP_CALL_CLOSURE || op == MO_OP_TAIL_CALL_CLOSURE;
            uint32_t target = a[tail ? 0 : 1];
            struct mo_cell environment = mo_cell_unit();
            if (closure) {
                environment = read(p, f, target);
                uint32_t layout = mo_cell_get_ref(environment)->layout;
                MO_ASSERT(layout < p->file->layout_count);
                MO_ASSERT(p->file->layouts[layout].kind == MO_LAYOUT_CLOSURE);
                target = p->file->layouts[layout].function;
            }
            MO_ASSERT(target < p->file->function_count);
            const struct mo_bytecode_function *callee = &p->file->functions[target];
            size_t offset = tail ? p->top : p->used;
            size_t bytes = mo_vm_frame_size(callee->register_count);
            const char *limit = !tail && p->depth == UINT32_MAX ? "--max-depth"
                                : bytes > SIZE_MAX - offset
                                    ? "--max-stack"
                                    : mo_vm_reserve(p, offset + bytes, p->depth + (tail ? 0u : 1u));
            if (limit != NULL) {
                bool native = strcmp(limit, "native memory") == 0;
                return mo_vm_stop(p, native ? "running out of memory" : "out of stack",
                                  native ? NULL : limit, false, mo_cell_unit(), MO_STOPPED);
            }
            f = mo_vm_frame(p); /* reserve may move the native stack */
            struct mo_cell arguments[MO_LIMIT_BYTECODE_PARAMETERS];
            const uint32_t *regs = a + (tail ? 1 : 2);
            for (uint32_t i = 0; i < count; i++)
                arguments[i] = read(p, f, regs[i]);
            for (uint32_t i = 0; i < count; i++)
                if (callee->parameters[i].ownership == MO_PARAMETER_OWNED)
                    consume(p, f, regs[i]);
            if (closure)
                consume(p, f, a[tail ? 0 : 1]);
            if (tail)
                clean(f);
            mo_vm_enter(p, target, arguments, environment, closure, tail, tail ? 0 : a[0], next);
            continue;
        }
        case MO_OP_RET: {
            value = read(p, f, a[0]);
            consume(p, f, a[0]);
            clean(f);
            size_t previous = f->previous;
            uint32_t destination = f->destination, resume = f->next;
            p->used = p->top;
            p->depth--;
            if (previous == SIZE_MAX) {
                *result = value;
                return MO_RETURNED;
            }
            p->top = previous;
            f = mo_vm_frame(p);
            write(p, f, destination, value, false);
            f->pc = resume;
            continue;
        }
        case MO_OP_STOP:
        case MO_OP_FAIL:
            return mo_vm_stop(p, op == MO_OP_STOP ? "stop" : "failed assertion", NULL, true,
                              read(p, f, a[0]), op == MO_OP_STOP ? MO_STOPPED : MO_FAILED);
        case MO_OP_CONSTRUCT:
        case MO_OP_MAKE_CASE:
        case MO_OP_MAKE_CLOSURE:
        case MO_OP_REUSE: {
            struct mo_scratch scratch;
            mo_scratch_init(&scratch, p->heap);
            struct mo_cell local[16];
            struct mo_cell *fields =
                count <= 16
                    ? local
                    : mo_scratch_resize(&scratch, NULL, 0, (size_t)count * sizeof(struct mo_cell));
            if (fields == NULL) {
                mo_scratch_free(&scratch);
                return memory(p);
            }
            const uint32_t *regs = a + info->fixed_count;
            for (uint32_t i = 0; i < count; i++)
                fields[i] = read(p, f, regs[i]);
            struct mo_token token = {0};
            bool ok;
            if (op == MO_OP_REUSE) {
                MO_ASSERT(a[1] < fn->register_count);
                MO_ASSERT(mo_vm_states(p, f)[a[1]] == VM_TOKEN);
                memcpy(&token, &mo_vm_cells(f)[a[1]], sizeof token);
                MO_ASSERT(token.layout == a[2]);
                ok = mo_value_reuse_owned(p->heap, &token, fields, count, &value);
            } else if (op == MO_OP_MAKE_CASE)
                ok = mo_value_case_owned(p->heap, a[1], a[2], fields, count, &value);
            else if (op == MO_OP_MAKE_CLOSURE)
                ok = mo_value_closure_owned(p->heap, a[1], fields, count, &value);
            else
                ok = mo_value_construct_owned(p->heap, a[1], fields, count, &value);
            int failure = errno;
            mo_scratch_free(&scratch);
            if (!ok) {
                errno = failure;
                return memory(p);
            }
            for (uint32_t i = 0; i < count; i++)
                consume(p, f, regs[i]);
            if (op == MO_OP_REUSE) {
                MO_ASSERT(f->owners != 0);
                f->owners--;
                mo_vm_states(p, f)[a[1]] = VM_MOVED;
            }
            write(p, f, a[0], value, false);
            break;
        }
        case MO_OP_FIELD:
        case MO_OP_CASE_FIELD: {
            struct mo_cell object = read(p, f, a[1]);
            if (op == MO_OP_CASE_FIELD) {
                MO_ASSERT(mo_cell_get_tag(object) == a[2]);
                uint32_t type = fn->register_types[a[1]];
                enum mo_case_shape shape = mo_case_shape(p->types, type, a[2]);
                struct mo_cell payload = mo_cell_get_payload(object);
                value = shape == MO_CASE_BOXED ? mo_value_field(payload, a[3]) : payload;
            } else
                value = mo_value_field(object, a[2]);
            value = mo_value_dup(p->heap, fn->register_types[a[0]], value);
            write(p, f, a[0], value, false);
            break;
        }
        case MO_OP_SET_FIELD:
            if (!mo_value_set_field_owned(p->heap, fn->register_types[a[1]], read(p, f, a[1]), a[2],
                                          read(p, f, a[3]), &value))
                return memory(p);
            consume(p, f, a[1]);
            consume(p, f, a[3]);
            write(p, f, a[0], value, false);
            break;
        case MO_OP_EQUAL: {
            bool equal;
            if (!mo_value_equal(p->heap, a[1], read(p, f, a[2]), read(p, f, a[3]), &equal))
                return memory(p);
            write(p, f, a[0], mo_cell_bool(equal), false);
            break;
        }
        case MO_OP_RESET: {
            value = read(p, f, a[1]);
            struct mo_token token = mo_value_reset(p->heap, fn->register_types[a[1]], value);
            consume(p, f, a[1]);
            MO_ASSERT(a[0] < fn->register_count);
            MO_ASSERT(mo_vm_states(p, f)[a[0]] != VM_OWNED && mo_vm_states(p, f)[a[0]] != VM_TOKEN);
            memcpy(&mo_vm_cells(f)[a[0]], &token, sizeof token);
            mo_vm_states(p, f)[a[0]] = VM_TOKEN;
            f->owners++;
            break;
        }
        case MO_OP_FREE_TOKEN: {
            MO_ASSERT(a[0] < fn->register_count);
            MO_ASSERT(mo_vm_states(p, f)[a[0]] == VM_TOKEN);
            struct mo_token token;
            memcpy(&token, &mo_vm_cells(f)[a[0]], sizeof token);
            mo_token_free(p->heap, &token);
            MO_ASSERT(f->owners != 0);
            f->owners--;
            mo_vm_states(p, f)[a[0]] = VM_MOVED;
            break;
        }
        case MO_OP_PRIM:
        case MO_OP_HOST:
            return mo_vm_stop(p, dispatch(op), NULL, false, mo_cell_unit(), MO_STOPPED);
        default:
            MO_UNREACHABLE();
        }
        f->pc = next;
    }
}
