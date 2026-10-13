/* Abilities required by the runtime ABI, not Core's broader typing relation. */
#ifndef MO_CODEGEN_ABILITIES_H
#define MO_CODEGEN_ABILITIES_H

#include "heap/types.h"

enum { MO_CODEGEN_EQUAL = 1, MO_CODEGEN_HASH = 2 };
MO_MUST_USE uint8_t *mo_codegen_abilities(struct mo_arena *arena, const struct mo_types *types);

#endif
