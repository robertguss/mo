#include "base/compile-context.h"
#include "base/binary.h"
#include "base/compile-metadata.h"
#include <string.h>
bool mo_compile_context_read(const uint8_t *bytes, size_t length, struct mo_compile_context *out) {
    if (length < 13 || length > MO_LIMIT_BUNDLE_BYTES || memcmp(bytes, "MOCC\1", 5) != 0)
        return false;
    struct mo_binary in = {bytes, length, 5};
    struct mo_compile_context next = {0};
    uint64_t count;
    if (!mo_compile_text_read(&in, MO_LIMIT_UPGRADE_MIGRATION_NAME, &next.migration) ||
        !mo_binary_uint(&in, 4, &count) || count > MO_LIMIT_BUNDLE_FILES)
        return false;
    next.count = (uint32_t)count;
    if (!mo_compile_files_read(&in, next.count, next.files))
        return false;
    if (in.position != length) {
        uint64_t size;
        const uint8_t *metadata;
        struct mo_compile_metadata parsed;
        if (!mo_binary_uint(&in, 4, &size) || !mo_binary_read(&in, (size_t)size, &metadata) ||
            !mo_compile_metadata_read(metadata, (size_t)size, &parsed))
            return false;
        next.metadata = (struct mo_bundle_span){metadata, (size_t)size};
    }
    if (in.position != length || (next.count == 0 && next.migration.length != 0))
        return false;
    *out = next;
    return true;
}
