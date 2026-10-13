/* Convert checked Core decimal digits to the bytecode's canonical binary magnitude. */
#ifndef MO_CODEGEN_BIGINT_H
#define MO_CODEGEN_BIGINT_H

#include "bytecode/bytecode.h"
#include "core/core.h"

enum mo_codegen_bigint_problem {
    MO_CODEGEN_BIGINT_OK,
    MO_CODEGEN_BIGINT_LIMIT,
    MO_CODEGEN_BIGINT_MEMORY
};

/* Work is shared across the program. Scratch is released per literal; output belongs to arena.
 * Failure leaves out unchanged. Input is the checker's shortest, valid ASCII decimal form. */
MO_MUST_USE enum mo_codegen_bigint_problem mo_codegen_bigint(struct mo_arena *arena,
                                                             mo_core_integer integer, size_t *work,
                                                             struct mo_bytecode_bigint *out);

#endif
