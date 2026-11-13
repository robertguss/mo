/* Intent items and their identity (design intent.md): every promise, service promise and example
 * row a program carries, each with a content digest over its owner, kind, canonical text and the
 * canonical text of every declaration it reads, leaving out its subject. */
#ifndef MO_TOOLS_INTENT_H
#define MO_TOOLS_INTENT_H

#include "check/check.h"

#define MO_INTENT_DIGEST_VERSION 1

struct mo_intent_item {
    const struct mo_checked_spec *spec; /* a promise or example row, or a service promise's spec */
    uint8_t digest[32];
};

struct mo_intent {
    uint32_t count;
    struct mo_intent_item *items; /* program order: specs, then each service's promises */
};

/* The items outside the toolchain library. program's set must carry its sources. Uses the
 * explicit source-stage stack. false when memory or MO_LIMIT_INTENT_WORK runs out. */
MO_MUST_USE bool mo_intent_items(struct mo_arena *arena, const struct mo_program *program,
                                 struct mo_intent *out);

#endif
