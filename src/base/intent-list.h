#ifndef MO_INTENT_LIST_H
#define MO_INTENT_LIST_H
#include "base/binary.h"
#include "base/bundle.h"

enum mo_intent_kind { MO_INTENT_PROMISE, MO_INTENT_EXAMPLE, MO_INTENT_SERVICE_PROMISE };

struct mo_intent_entry {
    const uint8_t *digest;
    enum mo_intent_kind kind;
    struct mo_bundle_span owner, name, text;
    uint32_t read_count;
    struct mo_binary reads;
};

struct mo_intent_list {
    const uint8_t *bytes;
    size_t length;
    uint8_t version;
    uint32_t count;
};

MO_MUST_USE bool mo_intent_list_read(const uint8_t *bytes, size_t length,
                                     struct mo_intent_list *out);
struct mo_binary mo_intent_list_first(const struct mo_intent_list *list);
MO_MUST_USE bool mo_intent_list_next(struct mo_binary *cursor, struct mo_intent_entry *out);
MO_MUST_USE bool mo_intent_list_read_name(struct mo_binary *reads, struct mo_bundle_span *out);
const char *mo_intent_kind_name(enum mo_intent_kind kind);
bool mo_intent_list_has(const struct mo_intent_list *list, const uint8_t digest[32]);

#endif
