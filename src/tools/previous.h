#ifndef MO_TOOLS_PREVIOUS_H
#define MO_TOOLS_PREVIOUS_H

#include "tools/intent.h"
#include "tools/tests.h"

struct mo_version {
    const struct mo_program *program;
    const struct mo_sources *sources;
    const struct mo_gated_run *run;
    struct mo_texts code;
};

struct mo_previous {
    struct mo_arena arena;
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_gated_run run;
    struct mo_version version;
    char digest[65];
};

MO_MUST_USE bool mo_previous_open(struct mo_previous *old, const struct mo_cli *cli,
                                  struct mo_arena *arena, const struct mo_program *program,
                                  const char *library, struct mo_diags *diags);

void mo_previous_close(struct mo_previous *old);

MO_MUST_USE bool *mo_previous_select(struct mo_arena *arena, const struct mo_version *old,
                                     const struct mo_version *now);

MO_MUST_USE char *mo_function_name(struct mo_arena *arena, const struct mo_program *program,
                                   uint32_t function);

MO_MUST_USE bool mo_same_text(struct mo_intent_text a, struct mo_intent_text b);

MO_MUST_USE const char *mo_changed_helper(const struct mo_check_text *a,
                                          const struct mo_check_text *b);

#endif
