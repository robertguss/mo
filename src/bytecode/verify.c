#include "bytecode/verify.h"

#include <string.h>

/* Ownership is checked separately from case knowledge: the former must agree exactly. */
enum state { UNDEFINED, VALUE, OWNED, BORROWED, MOVED, TOKEN };

static MO_MUST_USE bool refuse(struct mo_load_error *error, uint32_t offset, const char *reason) {
    *error = (struct mo_load_error){offset, reason};
    return false;
}

bool mo_bytecode_verify(const struct mo_bytecode *file, const uint32_t *code_offsets,
                        struct mo_load_error *error) {
    for (uint32_t index = 0; index < file->function_count; index++) {
        const struct mo_bytecode_function *fn = &file->functions[index];
        struct mo_arena scratch;
        mo_arena_init(&scratch, SIZE_MAX);
        uint8_t *states = mo_arena_allocate_array(&scratch, fn->register_count, 1, 1);
        if (states == NULL) {
            mo_arena_free(&scratch);
            return refuse(error, 0, "no memory");
        }
        memset(states, UNDEFINED, fn->register_count);
        for (uint32_t i = 0; i < fn->parameter_count; i++) {
            states[i] = file->types[fn->register_types[i]].reference
                            ? (fn->parameters[i].ownership == MO_PARAMETER_OWNED ? OWNED : BORROWED)
                            : VALUE;
        }
        if (fn->kind == MO_FUNCTION_CLOSURE) {
            states[fn->parameter_count] = OWNED;
        }
        for (uint32_t pc = 0; pc < fn->code_count;) {
            const struct mo_opcode_info *op = mo_opcode_find(fn->code[pc] & MO_OPCODE_MASK);
            MO_ASSERT(op != NULL);
            uint32_t n = fn->code[pc] >> MO_OPCODE_VARIABLE_SHIFT;
            for (uint32_t i = 0; i < op->fixed_count + n; i++) {
                struct mo_operand_info operand = i < op->fixed_count ? op->fixed[i] : op->variable;
                if (operand.kind != MO_OPERAND_REGISTER || operand.access == MO_ACCESS_WRITE ||
                    operand.access == MO_ACCESS_TOKEN_WRITE) {
                    continue;
                }
                uint32_t reg = fn->code[pc + 1 + i];
                if (states[reg] != VALUE && states[reg] != OWNED && states[reg] != BORROWED) {
                    mo_arena_free(&scratch);
                    return refuse(error, code_offsets[index] + 4 * (pc + 1 + i),
                                  "register is not readable");
                }
            }
            for (uint32_t i = 0; i < op->fixed_count; i++) {
                if (op->fixed[i].access == MO_ACCESS_WRITE) {
                    uint32_t reg = fn->code[pc + 1 + i];
                    states[reg] = file->types[fn->register_types[reg]].reference ? OWNED : VALUE;
                }
            }
            pc += 1U + op->fixed_count + n;
        }
        mo_arena_free(&scratch);
    }
    return true;
}
