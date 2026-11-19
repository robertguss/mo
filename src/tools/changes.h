#ifndef MO_TOOLS_CHANGES_H
#define MO_TOOLS_CHANGES_H

#include "tools/previous.h"

void mo_changes(const struct mo_cli *cli, struct mo_arena *arena, const struct mo_version *old,
                const char *digest, const struct mo_version *now, FILE *output,
                struct mo_diags *diags);

#endif
