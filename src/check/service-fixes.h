#ifndef MO_CHECK_SERVICE_FIXES_H
#define MO_CHECK_SERVICE_FIXES_H

#include "check/check.h"

/* The concrete local handler returns exactly State. Keep its service name and
 * documentation on an adapter and rename its bound module uses to a helper.
 * Bodies and specs must already be checked. Exported and ?-named handlers get no fixes. */
MO_MUST_USE bool mo_service_state_error(struct mo_arena *arena, const struct mo_program *program,
                                        const struct mo_syntax *tool_name, uint32_t function,
                                        struct mo_diags *diags);

#endif
