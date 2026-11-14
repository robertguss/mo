#ifndef MO_TOOLS_INTENT_H
#define MO_TOOLS_INTENT_H

#include "check/check.h"

#define MO_INTENT_DIGEST_VERSION 1

struct mo_intent_item {
    const struct mo_checked_spec *spec;
    uint8_t digest[32];
};

struct mo_intent {
    uint32_t count;
    struct mo_intent_item *items;
};

MO_MUST_USE bool mo_intent_items(struct mo_arena *arena, const struct mo_program *program,
                                 struct mo_intent *out);

#endif
