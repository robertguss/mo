#include "base/intent-list.h"
#include "base/compile-metadata.h"
#include <string.h>

static const char *const kinds[] = {"promise", "example", "service promise"};

const char *mo_intent_kind_name(enum mo_intent_kind kind) {
    MO_ASSERT((unsigned)kind < sizeof kinds / sizeof *kinds);
    return kinds[kind];
}

static bool text(struct mo_binary *in, struct mo_bundle_span *out) {
    return mo_compile_text_read(in, MO_LIMIT_INTENT_LIST_BYTES, out) && out->length != 0;
}

bool mo_intent_list_read_name(struct mo_binary *reads, struct mo_bundle_span *out) {
    return text(reads, out);
}

bool mo_intent_list_next(struct mo_binary *cursor, struct mo_intent_entry *out) {
    struct mo_binary in = *cursor;
    struct mo_intent_entry next;
    const uint8_t *kind;
    uint64_t reads;
    if (!mo_binary_read(&in, 32, &next.digest) || !mo_binary_read(&in, 1, &kind) ||
        *kind >= sizeof kinds / sizeof *kinds || !text(&in, &next.owner) ||
        !text(&in, &next.name) || !text(&in, &next.text) || !mo_binary_uint(&in, 4, &reads) ||
        reads > MO_LIMIT_INTENT_LIST_BYTES)
        return false;
    next.kind = (enum mo_intent_kind)kind[0];
    next.read_count = (uint32_t)reads;
    next.reads = in;
    struct mo_bundle_span name;
    for (uint64_t i = 0; i < reads; i++)
        if (!text(&in, &name))
            return false;
    *out = next;
    *cursor = in;
    return true;
}

struct mo_binary mo_intent_list_first(const struct mo_intent_list *list) {
    return (struct mo_binary){list->bytes, list->length, 10};
}

bool mo_intent_list_read(const uint8_t *bytes, size_t length, struct mo_intent_list *out) {
    if (length < 10 || length > MO_LIMIT_INTENT_LIST_BYTES || memcmp(bytes, "MOIL\1", 5) != 0 ||
        bytes[5] == 0)
        return false;
    struct mo_binary in = {bytes, length, 6};
    uint64_t count;
    if (!mo_binary_uint(&in, 4, &count) || count > MO_LIMIT_INTENT_ITEMS)
        return false;
    struct mo_intent_entry entry;
    for (uint64_t i = 0; i < count; i++)
        if (!mo_intent_list_next(&in, &entry))
            return false;
    if (in.position != length)
        return false;
    *out = (struct mo_intent_list){bytes, length, bytes[5], (uint32_t)count};
    return true;
}

bool mo_intent_list_has(const struct mo_intent_list *list, const uint8_t digest[32]) {
    struct mo_binary cursor = mo_intent_list_first(list);
    struct mo_intent_entry entry;
    for (uint32_t i = 0; i < list->count; i++) {
        MO_ASSERT(mo_intent_list_next(&cursor, &entry));
        if (memcmp(entry.digest, digest, 32) == 0)
            return true;
    }
    return false;
}
