#ifndef MO_TOOLS_INTENT_H
#define MO_TOOLS_INTENT_H

#include "check/check.h"
#include "tools/tools.h"

#define MO_INTENT_DIGEST_VERSION 1

struct mo_intent_text {
    const uint8_t *bytes;
    size_t length;
};

struct mo_intent_read {
    const char *module;
    const char *name;
    const char *label;
    struct mo_intent_text text;
};

struct mo_intent_item {
    const struct mo_checked_spec *spec;
    const char *name, *owner, *kind;
    struct mo_intent_text text;
    uint32_t read_count;
    struct mo_intent_read *reads;
    uint8_t digest[32];
};

struct mo_intent {
    uint32_t count;
    struct mo_intent_item *items;
};

MO_MUST_USE bool mo_intent_items(struct mo_arena *arena, const struct mo_program *program,
                                 struct mo_intent *out);

MO_MUST_USE bool mo_intent_list_write(const struct mo_intent *intent, struct mo_buffer *out);

void mo_intent_show(const struct mo_cli *cli, struct mo_arena *arena,
                    const struct mo_program *program, const char *library, FILE *output,
                    struct mo_diags *diags);

#endif
