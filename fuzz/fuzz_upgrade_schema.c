#include "tools/upgrade.h"
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    struct mo_sources sources;
    if (!mo_sources_init(&sources))
        return 0;
    (void)mo_upgrade_schema_sources(data, size, &sources);
    mo_sources_free(&sources);
    return 0;
}
