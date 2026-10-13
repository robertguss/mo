/* Body graph production, private to check. Shares the inference walk. */
#ifndef MO_CHECK_CHECKED_H
#define MO_CHECK_CHECKED_H

#include "check/check.h"

MO_MUST_USE bool mo_check_function(struct mo_arena *arena,
                                   const struct mo_module_interfaces *interfaces,
                                   const struct mo_signatures *signatures, uint32_t function,
                                   struct mo_diags *diags, struct mo_checked_function *out);

#endif
