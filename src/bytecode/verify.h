/* Private verification of structurally decoded tables. */
#ifndef MO_BYTECODE_VERIFY_H
#define MO_BYTECODE_VERIFY_H

#include "bytecode/io.h"

MO_MUST_USE bool mo_bytecode_verify(const struct mo_bytecode *file, const uint32_t *code_offsets,
                                    struct mo_load_error *error);

#endif
