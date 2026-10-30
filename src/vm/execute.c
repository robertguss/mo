#include "prim/prim.h"
#include "vm/internal.h"

#include <errno.h>
#include <fenv.h>
#include <string.h>

/* The type kind of a register. The verifier (bytecode.md section 9, "Types of operands") checked
 * every operand's type against its opcode; the assertions below state the same facts where the
 * loop relies on them. */
static MO_MUST_USE enum mo_type_kind kind_of(const struct mo_process *p,
                                             const struct mo_bytecode_function *fn, uint32_t reg) {
    MO_ASSERT(reg < fn->register_count);
    return p->types->items[fn->register_types[reg]].kind;
}

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
    return mo_vm_stop(p, "running out of memory", errno == ENOMEM ? "native memory" : "--max-heap",
                      false, mo_cell_unit(), MO_STOPPED);
}

static MO_MUST_USE enum mo_outcome primitive_stop(struct mo_process *p, enum mo_prim_status status,
                                                  const char *reason) {
    switch (status) {
    case MO_PRIM_OVERFLOW:
        MO_ASSERT(reason != NULL);
        reason = strcmp(reason, "Int") == 0       ? "overflow: Int"
                 : strcmp(reason, "Decimal") == 0 ? "overflow: Decimal"
                                                  : "overflow: Duration";
        break;
    case MO_PRIM_DIV_BY_ZERO:
    case MO_PRIM_OUTSIDE:
    case MO_PRIM_REPEATED_KEY:
        break;
    case MO_PRIM_OUT_OF_MEMORY:
        if (strstr(reason, "MO_LIMIT_VALUE_BYTES") == NULL)
            return memory(p);
        break;
    case MO_PRIM_RETURNED:
        MO_UNREACHABLE();
    }
    return mo_vm_stop(p, reason, NULL, false, mo_cell_unit(), MO_STOPPED);
}

static const char *const host_modes[] = {
#define HOSTOP(id, module, name, signature, params, bugs, spec_only) params,
#include "base/hostops.def"
};

enum mo_outcome mo_vm_execute(struct mo_process *p, struct mo_cell *result) {
    MO_ASSERT(fesetround(FE_TONEAREST) == 0 && fegetround() == FE_TONEAREST);
    for (;;) {
        struct mo_vm_frame *f = mo_vm_frame(p);
        const struct mo_bytecode_function *fn = &p->file->functions[f->function];
        MO_ASSERT(f->pc < fn->code_count);
        if (p->keep_running != NULL && !p->keep_running(p->control_context)) {
            p->cancelled = true;
            p->terminal = true;
            /* The call boundary translates this private exit before writing
             * either output. No Mo stop or defect has occurred. */
            return MO_STOPPED;
        }
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
            MO_ASSERT(fn->register_types[a[0]] == fn->register_types[a[1]]);
            value = read(p, f, a[1]);
            bool borrowed = mo_vm_states(p, f)[a[1]] == VM_BORROWED;
            if (!borrowed)
                consume(p, f, a[1]);
            write(p, f, a[0], value, borrowed);
            break;
        }
        case MO_OP_CLEAR:
            MO_ASSERT(a[0] < fn->register_count);
            MO_ASSERT(mo_vm_states(p, f)[a[0]] != VM_OWNED && mo_vm_states(p, f)[a[0]] != VM_TOKEN);
            mo_vm_states(p, f)[a[0]] = VM_UNDEFINED;
            break;
        case MO_OP_DUP:
            MO_ASSERT(fn->register_types[a[0]] == fn->register_types[a[1]]);
            value = mo_value_dup(p->heap, fn->register_types[a[1]], read(p, f, a[1]));
            write(p, f, a[0], value, false);
            break;
        case MO_OP_DROP:
            value = read(p, f, a[0]);
            consume(p, f, a[0]);
            mo_value_drop(p->heap, fn->register_types[a[0]], value);
            break;
        case MO_OP_CONST:
            MO_ASSERT(a[1] < p->file->constant_count &&
                      fn->register_types[a[0]] == p->file->constants[a[1]].type);
            write(p, f, a[0], p->runtime->constants[a[1]], true);
            break;
        case MO_OP_GLOBAL:
            MO_ASSERT(a[1] < p->file->global_count &&
                      fn->register_types[a[0]] == p->file->globals[a[1]].type);
            if (!p->runtime->set[a[1]])
                return mo_vm_stop(p, "internal error: unset global", NULL, false, mo_cell_unit(),
                                  MO_STOPPED);
            write(p, f, a[0], p->runtime->globals[a[1]], true);
            break;
        case MO_OP_SET_GLOBAL:
            MO_ASSERT(fn->kind == MO_FUNCTION_INIT && a[0] < p->file->global_count &&
                      p->file->globals[a[0]].type == fn->register_types[a[1]]);
            value = read(p, f, a[1]);
            MO_ASSERT(!p->runtime->set[a[0]]);
            if (!mo_value_make_immortal(p->heap, fn->register_types[a[1]], value))
                return memory(p);
            consume(p, f, a[1]);
            p->runtime->globals[a[0]] = value;
            p->runtime->set[a[0]] = true;
            break;
        case MO_OP_JUMP:
            MO_ASSERT(a[0] < fn->code_count);
            f->pc = a[0];
            continue;
        case MO_OP_BRANCH:
            MO_ASSERT(kind_of(p, fn, a[0]) == MO_TYPE_BOOL && a[1] < fn->code_count &&
                      a[2] < fn->code_count);
            f->pc = mo_cell_get_bool(read(p, f, a[0])) ? a[1] : a[2];
            continue;
        case MO_OP_SWITCH: {
            uint32_t tag = mo_cell_get_tag(read(p, f, a[0]));
            MO_ASSERT(kind_of(p, fn, a[0]) == MO_TYPE_CHOICE &&
                      count == p->types->items[fn->register_types[a[0]]].case_count);
            MO_ASSERT(tag < count && a[1 + tag] < fn->code_count);
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
            /* Verified: a direct call names a function that is neither closure code nor an
             * init; a closure call's layout names closure code; the count is the callee's. */
            MO_ASSERT(closure ? callee->kind == MO_FUNCTION_CLOSURE
                              : callee->kind != MO_FUNCTION_CLOSURE &&
                                    callee->kind != MO_FUNCTION_INIT);
            MO_ASSERT(count == callee->parameter_count && count <= MO_LIMIT_BYTECODE_PARAMETERS);
            MO_ASSERT((tail ? fn->result_type : fn->register_types[a[0]]) == callee->result_type);
            size_t offset = tail ? p->top : p->used;
            size_t bytes = mo_vm_frame_size(callee->register_count);
            const char *limit = !tail && p->depth == UINT32_MAX ? "--max-depth"
                                : bytes > SIZE_MAX - offset
                                    ? "--max-stack"
                                    : mo_vm_reserve(p, offset + bytes, p->depth + (tail ? 0u : 1u));
            if (limit != NULL) {
                bool native = strcmp(limit, "native memory") == 0;
                return mo_vm_stop(p, native ? "running out of memory" : "out of stack", limit,
                                  false, mo_cell_unit(), MO_STOPPED);
            }
            f = mo_vm_frame(p); /* reserve may move the native stack */
            struct mo_cell arguments[MO_LIMIT_BYTECODE_PARAMETERS];
            const uint32_t *regs = a + (tail ? 1 : 2);
            for (uint32_t i = 0; i < count; i++) {
                MO_ASSERT(fn->register_types[regs[i]] == callee->parameters[i].type);
                arguments[i] = read(p, f, regs[i]);
            }
            for (uint32_t i = 0; i < count; i++)
                if (callee->parameters[i].ownership == MO_PARAMETER_OWNED)
                    consume(p, f, regs[i]);
            if (closure)
                consume(p, f, a[tail ? 0 : 1]);
            if (tail)
                clean(f);
            mo_vm_enter(p, target, arguments, environment, closure, tail, tail ? 0 : a[0], next);
            if (p->confidential)
                mo_host_wipe(arguments, sizeof arguments);
            continue;
        }
        case MO_OP_RET: {
            MO_ASSERT(fn->register_types[a[0]] == fn->result_type);
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
            MO_ASSERT(kind_of(p, fn, a[0]) == MO_TYPE_TEXT);
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
            if (op == MO_OP_MAKE_CASE) {
                MO_ASSERT(a[1] < p->types->count && a[2] < p->types->items[a[1]].case_count &&
                          count == p->types->items[a[1]].cases[a[2]].field_count);
            } else if (op == MO_OP_MAKE_CLOSURE) {
                MO_ASSERT(a[1] < p->file->function_count &&
                          p->file->functions[a[1]].kind == MO_FUNCTION_CLOSURE &&
                          count == p->file->functions[a[1]].capture_count);
            } else {
                uint32_t layout = a[op == MO_OP_REUSE ? 2 : 1];
                MO_ASSERT(layout < p->file->layout_count &&
                          p->file->layouts[layout].kind != MO_LAYOUT_CLOSURE &&
                          count == p->file->layouts[layout].field_count);
            }
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
            if (p->confidential)
                mo_host_wipe(local, sizeof local);
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
                MO_ASSERT(p->types->items[type].kind == MO_TYPE_CHOICE &&
                          a[2] < p->types->items[type].case_count &&
                          a[3] < p->types->items[type].cases[a[2]].field_count);
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
        case MO_OP_PRIM: {
            static const char *const ownership[] = {
#define PRIM(id, module, name, signature, params, bugs) params,
#include "base/prims.def"
            };
            MO_ASSERT(a[1] < MO_PRIM_COUNT && count == strlen(ownership[a[1]]));
            MO_ASSERT(a[2] < p->types->count && p->types->items[a[2]].kind == MO_TYPE_FUNCTION &&
                      p->types->items[a[2]].parameter_count == count);
            struct mo_cell arguments[MO_LIMIT_BYTECODE_PARAMETERS];
            for (uint32_t i = 0; i < count; i++)
                arguments[i] = read(p, f, a[3 + i]);
            const char *reason;
            enum mo_prim_status status = mo_prim_call(p->heap, p->types, (enum mo_primitive_id)a[1],
                                                      a[2], arguments, &value, &reason);
            if (p->confidential)
                mo_host_wipe(arguments, sizeof arguments);
            if (status != MO_PRIM_RETURNED)
                return primitive_stop(p, status, reason);
            /* The primitive transferred owned cells only after success. */
            for (uint32_t i = 0; i < count; i++)
                if (ownership[a[1]][i] == 'o')
                    consume(p, f, a[3 + i]);
            write(p, f, a[0], value, false);
            break;
        }
        case MO_OP_SIM: {
            MO_ASSERT(count <= MO_LIMIT_BYTECODE_PARAMETERS);
            struct mo_cell arguments[MO_LIMIT_BYTECODE_PARAMETERS];
            for (uint32_t i = 0; i < count; i++)
                arguments[i] = read(p, f, a[4 + i]);
            enum mo_outcome outcome =
                mo_vm_simulate(p, a[1], a[2], a[3], fn->register_types[a[0]], arguments, &value);
            if (p->confidential)
                mo_host_wipe(arguments, sizeof arguments);
            if (outcome != MO_RETURNED)
                return outcome;
            write(p, f, a[0], value, false);
            break;
        }
        case MO_OP_HOST: {
            MO_ASSERT(a[0] < fn->register_count && a[1] < MO_HOST_COUNT &&
                      count <= MO_LIMIT_BYTECODE_PARAMETERS);
            MO_ASSERT(count == strlen(host_modes[a[1]]));
            struct mo_cell arguments[MO_LIMIT_BYTECODE_PARAMETERS];
            for (uint32_t i = 0; i < count; i++)
                arguments[i] = read(p, f, a[2 + i]);
            struct mo_host_error error;
            enum mo_host_outcome outcome = mo_host_call(
                &p->host, p->heap, a[1], fn->register_types[a[0]], arguments, &value, &error);
            if (p->confidential)
                mo_host_wipe(arguments, sizeof arguments);
            if (outcome == MO_HOST_OUT_OF_MEMORY && errno == EOVERFLOW)
                return mo_vm_stop(p, "running out of memory: MO_LIMIT_VALUE_BYTES", "native memory",
                                  false, mo_cell_unit(), MO_STOPPED);
            if (outcome == MO_HOST_OUT_OF_MEMORY)
                return memory(p);
            if (outcome != MO_HOST_RETURNED)
                return mo_vm_stop(p, error.reason, NULL, error.has_detail, error.detail,
                                  MO_STOPPED);
            for (uint32_t i = 0; i < count; i++)
                if (host_modes[a[1]][i] == 'o')
                    consume(p, f, a[2 + i]);
            write(p, f, a[0], value, false);
            break;
        }
        default:
            MO_UNREACHABLE();
        }
        f->pc = next;
    }
}
