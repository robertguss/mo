#ifndef MO_COMPILE_CONTEXT_H
#define MO_COMPILE_CONTEXT_H
#include "base/bundle.h"
/* MOCC v1 stores generated UpgradeOld modules and the retained migration root.
 * It is independently hashed, never part of canonical user-source identity. */
struct mo_compile_context {
    struct mo_bundle_span migration, metadata;
    uint32_t count;
    struct mo_bundle_file files[MO_LIMIT_BUNDLE_FILES];
};
MO_MUST_USE bool mo_compile_context_read(const uint8_t *bytes, size_t length,
                                         struct mo_compile_context *out);
#endif
