/* Source observations: inventory and exact selection precede all execution. */
#ifndef MO_TOOLS_TESTS_H
#define MO_TOOLS_TESTS_H

#include "check/check.h"
#include "lower/lower.h"
#include "tools/tools.h"
#include "vm/vm.h"

struct mo_observation {
    const char *name;
    const struct mo_checked_spec *source;
    const struct mo_lowered_promise *instance; /* generic promise only, NULL in the inventory */
};

struct mo_test_selection {
    uint32_t count;
    struct mo_observation *items;
    const char *mistake; /* exit 2; no records may have been published */
};

/* NULL on resource failure (diagnosed). Includes uninstantiated promises;
 * anchors are compiler checks, never user-selectable observations. */
MO_MUST_USE struct mo_test_selection *mo_select_tests(struct mo_arena *arena,
                                                      const struct mo_program *program,
                                                      const struct mo_cli *cli,
                                                      struct mo_diags *diags);
MO_MUST_USE const char *mo_observation_kind(enum mo_checked_spec_kind kind);
MO_MUST_USE struct mo_test_selection *
mo_instantiate_tests(struct mo_arena *arena, const struct mo_test_selection *selection,
                     const struct mo_lowered_promises *promises, struct mo_diags *diags);
MO_MUST_USE bool mo_write_test(FILE *output, bool json, const struct mo_sources *sources,
                               const struct mo_observation *observation);

struct mo_promise_result;
/* All result data is borrowed until the complete record is flushed. */
struct mo_test_result {
    const struct mo_observation *observation;
    const struct mo_bytecode *file;
    struct mo_process *process;
    const struct mo_defect *defect;
    enum mo_outcome outcome;
    uint64_t microseconds;
    struct mo_span line;                     /* original assert or false promise condition */
    const struct mo_promise_result *promise; /* NULL for ordinary observations */
};
/* One complete bounded record, or false with the buffer empty. Never truncates
 * a field to manufacture a record; all process views remain borrowed. */
MO_MUST_USE bool mo_render_test_result(const struct mo_sources *sources,
                                       const struct mo_test_result *result, bool json,
                                       struct mo_buffer *out);
MO_MUST_USE bool mo_write_test_result(FILE *output, bool json, const struct mo_sources *sources,
                                      const struct mo_test_result *result);
/* Selection has passed generator preflight and is nonempty. Never enters main. */
void mo_test_program(const struct mo_cli *cli, const struct mo_program *program,
                     const struct mo_test_selection *selection, const struct mo_buffer *wire,
                     const struct mo_sources *sources, FILE *output, struct mo_diags *diags,
                     struct mo_test_totals *totals);

#endif
