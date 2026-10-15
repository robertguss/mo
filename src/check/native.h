#ifndef MO_CHECK_NATIVE_H
#define MO_CHECK_NATIVE_H

#include "check/check.h"

struct mo_native_signature {
    const char *module, *name, *qualified;
    struct mo_symbol owner; /* owns abstract parameters, not a source function */
    const struct mo_source_type *type;
    uint8_t requirements[26];
    bool spec_only;
};

#endif
