#ifndef MO_RUNTIME_IDENTITY_H
#define MO_RUNTIME_IDENTITY_H
#include "base/base.h"
#include <stdbool.h>
#include <stdint.h>
#ifndef MO_BUILD_CONFIGURATION
#define MO_BUILD_CONFIGURATION "unspecified"
#endif
/* Read once during executable startup. Failure leaves identity unavailable;
 * ordinary commands continue to work. The digest, not the label, is identity. */
void mo_runtime_identity_init(void);
MO_MUST_USE bool mo_runtime_identity(uint8_t digest[32]);
MO_MUST_USE const char *mo_runtime_build_label(void);
#endif
