/* Core operations name bytecode instructions or rows in the trusted library tables. */
#ifndef MO_CODEGEN_OPERATIONS_H
#define MO_CODEGEN_OPERATIONS_H

#include "bytecode/bytecode.h"
#include "core/core.h"

struct mo_codegen_operation {
    enum mo_opcode opcode;
    uint32_t row;
    const char *signature, *params;
};

MO_MUST_USE bool mo_codegen_operation(const struct mo_core_prim *prim,
                                      struct mo_codegen_operation *out);

#endif
