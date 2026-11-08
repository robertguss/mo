#ifndef MO_BUNDLE_H
#define MO_BUNDLE_H
#include "base/base.h"
#include "base/limits.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct mo_bundle_span {
    const uint8_t *bytes;
    size_t length;
};
struct mo_bundle_file {
    struct mo_bundle_span path, source;
};
struct mo_bundle {
    struct mo_bundle_span entry;
    uint32_t count;
    struct mo_bundle_file files[MO_LIMIT_BUNDLE_FILES];
};
/* Validates canonical MOSB v1 framing and paths. Spans borrow input bytes; no allocation.
 * Refusal preserves out. Import containment is checked by the compiler's bundle resolver. */
MO_MUST_USE bool mo_bundle_read(const uint8_t *bytes, size_t length, struct mo_bundle *out);
#endif
