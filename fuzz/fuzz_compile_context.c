#include "base/compile-context.h"
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    struct mo_compile_context out = {.count = 99};
    if (mo_compile_context_read(data, size, &out)) {
        MO_ASSERT(size <= MO_LIMIT_BUNDLE_BYTES && out.count <= MO_LIMIT_BUNDLE_FILES);
        MO_ASSERT(out.migration.bytes >= data &&
                  out.migration.bytes + out.migration.length <= data + size);
        for (uint32_t i = 0; i < out.count; i++) {
            struct mo_bundle_file f = out.files[i];
            MO_ASSERT(f.path.length >= 13 && f.path.length <= MO_LIMIT_BUNDLE_PATH_BYTES);
            MO_ASSERT(f.path.bytes >= data && f.path.bytes + f.path.length <= data + size);
            MO_ASSERT(f.source.bytes >= data && f.source.bytes + f.source.length <= data + size);
        }
    } else
        MO_ASSERT(out.count == 99);
    return 0;
}
