#ifndef MO_FUZZ_SERVICE_H
#define MO_FUZZ_SERVICE_H

#include "service_restore.h"
#include "store_record.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    fuzz_store_record(MO_FUZZ_STORE_KIND, data, size);
    fuzz_restore(MO_FUZZ_STORE_KIND, data, size);
    return 0;
}

#endif
