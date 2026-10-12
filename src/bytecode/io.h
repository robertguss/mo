/* Private bytecode I/O. Decoding is structural, not permission to execute. */
#ifndef MO_BYTECODE_IO_H
#define MO_BYTECODE_IO_H

#include "base/arena.h"
#include "bytecode/bytecode.h"

/* Copies all data into arena, retaining file indices. NULL on structural refusal or allocation
 * failure; error is required and has static reason text. Failure may use arena storage, which
 * the caller frees. No pointer into bytes survives. The verifier must run before publication. */
MO_MUST_USE struct mo_bytecode *mo_bytecode_decode(struct mo_arena *arena, const uint8_t *bytes,
                                                   size_t length, struct mo_load_error *error);

/* For the decoder's complete, bounded tables only, not arbitrary caller-built structures.
 * Checks section 3, without ownership dataflow or execution. Relational refusals point to the
 * start of the payload; the reader's local wire errors point to its cursor. */
MO_MUST_USE bool mo_bytecode_validate(const struct mo_bytecode *file, struct mo_load_error *error);

#endif
