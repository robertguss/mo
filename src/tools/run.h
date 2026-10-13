/* Execute the verified lowered entry. Called inside the source-stack callback. */
#ifndef MO_TOOLS_RUN_H
#define MO_TOOLS_RUN_H

#include "core/core.h"
#include "tools/tools.h"

MO_MUST_USE int mo_run_program(const struct mo_cli *cli, const struct mo_core *core,
                               const struct mo_buffer *wire, const struct mo_sources *sources,
                               FILE *output, FILE *errors, struct mo_diags *diags);

#endif
