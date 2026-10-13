/* Exact-type recycling of block-local registers in trusted emitted code. */
#ifndef MO_CODEGEN_REGISTERS_H
#define MO_CODEGEN_REGISTERS_H

#include "base/arena.h"
#include "bytecode/bytecode.h"

/* Inputs/env and cross-block registers remain distinct. prologue is the first real
 * instruction after immediate initialization. Returns zero or a codegen/bytecode limit code. */
MO_MUST_USE uint16_t mo_codegen_registers(struct mo_arena *arena, const struct mo_bytecode *context,
                                          struct mo_bytecode_function *function,
                                          struct mo_bytecode_debug *debug, uint32_t prologue);

#endif
