#ifndef MO_TOOLS_GATES_H
#define MO_TOOLS_GATES_H

#include "tools/tests.h"

void mo_gates(const struct mo_cli *cli, struct mo_arena *arena, const struct mo_program *program,
              const struct mo_sources *sources, const struct mo_test_selection *selection,
              FILE *output, struct mo_diags *diags, struct mo_test_totals *totals);

void mo_gates_list(struct mo_arena *arena, const struct mo_program *program,
                   const struct mo_sources *sources, bool json, FILE *output,
                   struct mo_diags *diags);

MO_MUST_USE uint64_t mo_gate_limit(uint64_t steps);

MO_MUST_USE bool mo_gate_start_failed(const struct mo_diags *diags);

MO_MUST_USE bool mo_render_gates(struct mo_buffer *out, const struct mo_mutant_totals *mutants,
                                 bool json);

#endif
