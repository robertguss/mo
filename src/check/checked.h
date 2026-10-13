/* Body graph production, private to check. Shares the inference walk. */
#ifndef MO_CHECK_CHECKED_H
#define MO_CHECK_CHECKED_H

#include "check/check.h"

/* halt distinguishes resource/storage or staging failure from an independent body error,
 * including errors dropped solely because the diagnostic count is full. */
MO_MUST_USE bool mo_check_function(struct mo_arena *arena,
                                   const struct mo_module_interfaces *interfaces,
                                   const struct mo_signatures *signatures, uint32_t function,
                                   struct mo_diags *diags, struct mo_checked_function *out,
                                   bool *halt);

MO_MUST_USE bool mo_check_example(struct mo_arena *arena,
                                  const struct mo_module_interfaces *interfaces,
                                  const struct mo_signatures *signatures, uint32_t function,
                                  const struct mo_syntax *syntax, struct mo_diags *diags,
                                  struct mo_checked_body *out, bool *halt);

MO_MUST_USE bool mo_check_recursion(struct mo_arena *arena, const struct mo_program *program,
                                    struct mo_diags *diags);

MO_MUST_USE bool mo_check_data_recursion(struct mo_arena *arena,
                                         const struct mo_signatures *signatures,
                                         struct mo_diags *diags);

#endif
