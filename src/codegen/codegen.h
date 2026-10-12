/* Core to bytecode (architecture 12). No source or checked-Mo dependencies. */
#ifndef MO_CODEGEN_H
#define MO_CODEGEN_H

#include "base/buffer.h"
#include "core/core.h"

/* Borrows complete checked Core. out must be initialized, empty and independent of Core.
 * Refusal appends a diagnostic and leaves out empty; capacity may have grown.
 * The staged compiler reports unimplemented Core forms explicitly, never omits their code.
 * Success goes through the bytecode writer's checks (and verifier when implemented). */
MO_MUST_USE bool mo_compile(const struct mo_core *core, struct mo_buffer *out,
                            struct mo_diags *diags);

#endif
