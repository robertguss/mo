/* Checked Mo to monomorphic Core. The result owns all its storage; it does
 * not borrow the checked program, syntax or original sources. */
#ifndef MO_LOWER_H
#define MO_LOWER_H

#include "check/check.h"
#include "core/core.h"

/* program must be certified by mo_check (or an equivalent contract fixture).
 * sources is its original, unchanged source table. NULL on a resource limit
 * or an explicitly unfinished form, with a diagnostic. Failure may retain
 * allocations in arena. No source/type/expression walk uses C recursion. */
MO_MUST_USE struct mo_core *mo_lower(struct mo_arena *arena, const struct mo_program *program,
                                     const struct mo_sources *sources, struct mo_diags *diags);

#endif
