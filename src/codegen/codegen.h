/* Core to bytecode (architecture 12). No source or checked-Mo dependencies. */
#ifndef MO_CODEGEN_H
#define MO_CODEGEN_H

#include "base/buffer.h"
#include "core/core.h"

/* Borrows complete checked Core. out must be initialized, empty and independent of Core.
 * Like the Core walkers, run this on MO_SOURCE_STACK_SIZE through mo_with_stack (base/stack.h).
 * Refusal appends a diagnostic and leaves out empty; capacity may have grown.
 * The staged compiler reports unimplemented Core forms explicitly, never omits their code.
 * E1201 permanently excludes representations that source lowering never produces (architecture 12).
 * The first-match ordinary entry may have any full name, unique across the function table.
 * Success goes through the bytecode writer's structural checks and full verification. */
MO_MUST_USE bool mo_compile(const struct mo_core *core, struct mo_buffer *out,
                            struct mo_diags *diags);

#endif
