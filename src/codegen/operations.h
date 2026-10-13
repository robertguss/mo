/* Core operations name bytecode instructions or rows in the trusted library tables. */
#ifndef MO_CODEGEN_OPERATIONS_H
#define MO_CODEGEN_OPERATIONS_H

#include "bytecode/bytecode.h"
#include "core/core.h"

struct mo_codegen_operation {
    enum mo_opcode opcode;
    uint32_t row;
    const char *signature, *params;
    bool reverse, compare;
    enum { MO_CODEGEN_LITERAL_NONE, MO_CODEGEN_LITERAL_LIST, MO_CODEGEN_LITERAL_MAP } literal;
    enum { MO_CODEGEN_EXPAND_NONE, MO_CODEGEN_EXPAND_RANGE, MO_CODEGEN_EXPAND_UNCONS } expansion;
};

MO_MUST_USE bool mo_codegen_operation(const struct mo_core_prim *prim,
                                      struct mo_codegen_operation *out);
MO_MUST_USE struct mo_codegen_operation mo_codegen_primitive(enum mo_primitive_id row);

#endif
