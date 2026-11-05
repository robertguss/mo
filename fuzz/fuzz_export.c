#include "base/binary.h"
#include "base/export.h"
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    uint8_t digest[32];
    bool schema = mo_export_schema(data, size, digest);
    bool expected = size >= 9 && size <= MO_LIMIT_SAVED_BYTES && memcmp(data, "MOSV\1", 5) == 0;
    uint64_t length = 0;
    if (expected) {
        for (unsigned i = 0; i < 4; i++)
            length |= (uint64_t)data[5 + i] << (8 * i);
        expected = length > 0 && length <= size - 9;
    }
    MO_ASSERT(schema == expected);
    if (size >= 16) {
        uint64_t total = 0, offset = 0;
        for (unsigned i = 0; i < 8; i++) {
            total |= (uint64_t)data[i] << (8 * i);
            offset |= (uint64_t)data[8 + i] << (8 * i);
        }
        size_t page = 77, count = 88;
        bool ok = mo_export_page_bounds((size_t)total, (size_t)offset, &page, &count);
        MO_ASSERT(ok == (total > 0 && total <= MO_LIMIT_EXPORT_BYTES / MO_LIMIT_EXPORT_PINS &&
                         offset < total && offset % MO_LIMIT_EXPORT_PAGE_BYTES == 0));
        if (ok) {
            MO_ASSERT(page > 0 && page <= MO_LIMIT_EXPORT_PAGE_BYTES && page <= total - offset);
            MO_ASSERT(count > 0 && (count - 1) * MO_LIMIT_EXPORT_PAGE_BYTES < total);
        } else
            MO_ASSERT(page == 77 && count == 88);
    }
    return 0;
}
