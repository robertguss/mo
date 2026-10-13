/* Prepared signatures from the shared primitive and host tables. */
#ifndef MO_CHECK_NATIVE_H
#define MO_CHECK_NATIVE_H

#include "check/check.h"

struct mo_native_signature {
    const char *module, *name, *qualified;
    struct mo_symbol owner; /* owns abstract parameters, not a source function */
    const struct mo_source_type *type;
    bool spec_only;
};

#endif
