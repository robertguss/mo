/* Ability proofs and collection-key constraints, private to checking. */
#ifndef MO_CHECK_ABILITIES_H
#define MO_CHECK_ABILITIES_H

#include "check/check.h"

struct mo_ability_cache;
struct mo_ability_context {
    struct mo_arena *arena;
    const struct mo_module_interfaces *interfaces;
    const struct mo_signatures *signatures;
    uint32_t *work;
    struct mo_ability_cache *cache;
    uint16_t error;
    const char *message, *reason;
};

/* No diagnostics are emitted here: the calling stage owns their source span
 * and source-order policy. NULL/false sets error, including arena/work limits. */
MO_MUST_USE const struct mo_ability_implementation *
mo_prove_ability(struct mo_ability_context *context, const struct mo_source_type *type,
                 enum mo_source_ability ability);
MO_MUST_USE bool mo_check_type_abilities(struct mo_ability_context *context,
                                         const struct mo_source_type *type);

#endif
