#include "base/bundle.h"
#include <string.h>
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    struct mo_bundle out = {.count = 99};
    if (mo_bundle_read(data, size, &out)) {
        MO_ASSERT(size <= MO_LIMIT_BUNDLE_BYTES);
        MO_ASSERT(out.count > 0 && out.count <= MO_LIMIT_BUNDLE_FILES);
        bool found = false;
        for (uint32_t i = 0; i < out.count; i++) {
            struct mo_bundle_file f = out.files[i];
            MO_ASSERT(f.path.length > 0 && f.path.length <= MO_LIMIT_BUNDLE_PATH_BYTES);
            MO_ASSERT(f.path.bytes >= data && f.path.bytes + f.path.length <= data + size);
            MO_ASSERT(f.source.bytes >= data && f.source.bytes + f.source.length <= data + size);
            found = found || (f.path.length == out.entry.length &&
                              memcmp(f.path.bytes, out.entry.bytes, f.path.length) == 0);
        }
        MO_ASSERT(found);
    } else
        MO_ASSERT(out.count == 99);
    return 0;
}
