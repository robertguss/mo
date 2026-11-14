#include "base/intent-list.h"
#include <string.h>
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    struct mo_intent_list list = {.count = UINT32_MAX};
    if (!mo_intent_list_read(data, size, &list)) {
        MO_ASSERT(list.count == UINT32_MAX);
        return 0;
    }
    MO_ASSERT(size <= MO_LIMIT_INTENT_LIST_BYTES && list.count <= MO_LIMIT_INTENT_ITEMS);
    struct mo_binary cursor = mo_intent_list_first(&list);
    for (uint32_t i = 0; i < list.count; i++) {
        struct mo_intent_entry entry;
        bool next = mo_intent_list_next(&cursor, &entry);
        MO_ASSERT(next && mo_intent_list_has(&list, entry.digest));
        MO_ASSERT(entry.owner.length != 0 && entry.name.length != 0 && entry.text.length != 0);
        (void)mo_intent_kind_name(entry.kind);
        struct mo_bundle_span name;
        for (uint32_t r = 0; r < entry.read_count; r++) {
            bool read = mo_intent_list_read_name(&entry.reads, &name);
            MO_ASSERT(read && name.bytes >= data && name.bytes + name.length <= data + size);
        }
    }
    MO_ASSERT(cursor.position == size);
    return 0;
}
