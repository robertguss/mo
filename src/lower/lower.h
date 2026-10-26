/* Checked Mo to monomorphic Core. The result owns all its storage; it does
 * not borrow the checked program, syntax or original sources. */
#ifndef MO_LOWER_H
#define MO_LOWER_H

#include "base/service.h"
#include "check/check.h"
#include "core/core.h"

/* program must be certified by mo_check (or an equivalent contract fixture).
 * sources is its original, unchanged source table. NULL on a resource limit
 * or an explicitly unfinished form, with a diagnostic. Failure may retain
 * allocations in arena. No source/type/expression walk uses C recursion. */
MO_MUST_USE struct mo_core *mo_lower(struct mo_arena *arena, const struct mo_program *program,
                                     const struct mo_sources *sources, struct mo_diags *diags);

/* Test-only provenance, outside the Core/wire contract. Storage belongs to arena;
 * source declarations and names inside parameter types borrow program. */
struct mo_lowered_promise {
    const struct mo_checked_spec *source;
    mo_core_string name;
    uint32_t parameter_count;
    const struct mo_source_type **parameters;
};
struct mo_lowered_promises {
    uint32_t count;
    struct mo_lowered_promise *items;
};
/* The same lowering, retaining each concrete promise's closed input types. */
MO_MUST_USE struct mo_core *mo_lower_for_tests(struct mo_arena *arena,
                                               const struct mo_program *program,
                                               const struct mo_sources *sources,
                                               struct mo_diags *diags,
                                               struct mo_lowered_promises *promises);

/* Retains service descriptors alongside ordinary Core functions. promises may be
 * NULL. services is required and is cleared on refusal. Neither output borrows
 * the source program, except the documented test-only promise provenance. */
MO_MUST_USE struct mo_core *
mo_lower_services(struct mo_arena *arena, const struct mo_program *program,
                  const struct mo_sources *sources, struct mo_diags *diags,
                  struct mo_lowered_promises *promises, struct mo_services *services);

#endif
