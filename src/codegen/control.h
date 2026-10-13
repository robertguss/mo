/* Remove unreachable code before verification, relocating targets and source ranges. */
#ifndef MO_CODEGEN_CONTROL_H
#define MO_CODEGEN_CONTROL_H

#include "base/arena.h"
#include "bytecode/bytecode.h"

/* The input is emitted code, not untrusted bytecode. Output borrows arena; false means no memory.
 * Temporary graph storage is bounded by MO_LIMIT_FUNCTION_CODE_WORDS and freed per function. */
MO_MUST_USE bool mo_codegen_prune(struct mo_arena *arena, struct mo_bytecode_function *function,
                                  struct mo_bytecode_debug *debug);

#endif
