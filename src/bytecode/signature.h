/* Matching the trusted library signatures against decoded runtime types. */
#ifndef MO_BYTECODE_SIGNATURE_H
#define MO_BYTECODE_SIGNATURE_H

#include "bytecode/verify.h"

MO_MUST_USE bool mo_bytecode_signature(const struct mo_verify_function *v, const char *signature,
                                       const uint32_t *parameters, uint32_t count, uint32_t result,
                                       const char **reason);
MO_MUST_USE bool mo_bytecode_ordering(const struct mo_type *type);

#endif
