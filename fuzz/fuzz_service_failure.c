#include "base/failure.h"
#include "host/host.h"
#include <string.h>
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    struct mo_failure_record r;
    if (mo_failure_read(data, size, &r)) {
        MO_ASSERT(r.id > 0 && r.id <= INT64_MAX);
        MO_ASSERT(r.argument_count <= MO_LIMIT_BYTECODE_PARAMETERS - 2);
        MO_ASSERT(r.context.length <= MO_LIMIT_FAILURE_CONTEXT_BYTES);
        MO_ASSERT(r.arguments.bytes >= data &&
                  r.arguments.bytes + r.arguments.length <= data + size);
        if (size != 0) {
            struct mo_failure_record truncated;
            MO_ASSERT(!mo_failure_read(data, size - 1, &truncated));
        }
    }
    struct mo_store_record frame;
    if (mo_store_record_read(MO_STORE_FAILURE_MESSAGE, data, size, &frame)) {
        bool valid = mo_failure_read(frame.bytes, frame.length, &r);
        (void)valid;
    }
    return 0;
}
