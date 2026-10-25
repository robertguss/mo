#ifndef MO_CHECK_SERVICE_FIXES_H
#define MO_CHECK_SERVICE_FIXES_H

#include "check/check.h"

/* The concrete local handler returns State, or Result(State, E) when result_state is true.
 * Bodies and specs must already be checked. Only bare-State handlers can get adapters;
 * exported and ?-named handlers get no fixes. Adapters retain the service name and
 * documentation, renaming bound module uses to a helper. */
MO_MUST_USE bool mo_service_state_error(struct mo_arena *arena, const struct mo_program *program,
                                        const struct mo_syntax *tool_name, uint32_t function,
                                        bool result_state, struct mo_diags *diags);

#endif
