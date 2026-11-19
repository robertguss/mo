#ifndef MO_TOOLS_GATES_H
#define MO_TOOLS_GATES_H

#include "tools/tests.h"

void mo_gates(const struct mo_cli *cli, struct mo_arena *arena, const struct mo_program *program,
              const struct mo_sources *sources, const struct mo_test_selection *selection,
              FILE *output, struct mo_diags *diags, struct mo_test_totals *totals);

MO_MUST_USE bool mo_render_gates(struct mo_buffer *out, const struct mo_gate_totals *gates,
                                 bool json);

#endif
