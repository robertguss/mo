/* Only the resolver-fault target's native host copies use these smaller limits. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#include "base/limits.h"
#undef MO_LIMIT_PATH_STEPS
#define MO_LIMIT_PATH_STEPS 64u
#undef MO_LIMIT_VALUE_BYTES
#define MO_LIMIT_VALUE_BYTES 64u
