#ifndef MO_CHECK_INTERNAL_H
#define MO_CHECK_INTERNAL_H

#include "check/check.h"

/* Names always in scope (reference 10.5), including native choice cases. */
MO_MUST_USE bool mo_source_builtin(const uint8_t *name, size_t length);

#endif
