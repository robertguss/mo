/* Source observations: inventory and exact selection precede all execution. */
#ifndef MO_TOOLS_TESTS_H
#define MO_TOOLS_TESTS_H

#include "check/check.h"
#include "tools/tools.h"

struct mo_observation {
    const char *name;
    const struct mo_checked_spec *source;
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
MO_MUST_USE bool mo_write_test(FILE *output, bool json, const struct mo_sources *sources,
                               const struct mo_observation *observation);

#endif
