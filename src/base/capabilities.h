/* Capability type names in MO_CAP_* order. */
#ifndef MO_CAPABILITIES_H
#define MO_CAPABILITIES_H

#include <stdbool.h>

#include "base/base.h"

enum { MO_CAPABILITY_COUNT = 8 };
/* Host slots are Args..Random; Simulation follows as MO_CAP_SIMULATION. */
enum { MO_HOST_CAPABILITY_COUNT = MO_CAPABILITY_COUNT - 1 };
extern const char *const mo_capability_names[MO_CAPABILITY_COUNT];
MO_MUST_USE bool mo_capability_named(const char *name);

#endif
