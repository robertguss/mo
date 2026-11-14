#ifndef MO_TOOLS_STRENGTH_H
#define MO_TOOLS_STRENGTH_H

#include "tools/tools.h"

struct mo_strength_run {
    const struct mo_cli *cli;
    struct mo_arena *arena;
    const struct mo_program *program;
    const struct mo_sources *sources;
    const char *library;
    FILE *output;
    struct mo_diags *diags;
    struct mo_test_totals *totals;
    bool *version;
    const char **mistake;
};

void mo_strength(const struct mo_strength_run *run);

#endif
