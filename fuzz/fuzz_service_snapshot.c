#include "service_restore.h"
#include "store_record.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    fuzz_store_record(MO_STORE_STATE, data, size);
    fuzz_restore(MO_STORE_STATE, data, size);
    return 0;
}
