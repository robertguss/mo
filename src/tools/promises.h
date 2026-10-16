/* Promise trials and retained reports. Generation stays in inputs.h. */
#ifndef MO_TOOLS_PROMISES_H
#define MO_TOOLS_PROMISES_H

#include "tools/inputs.h"
#include "tools/tests.h"

/* Borrowed through the complete result write, never into a cancelled process. */
struct mo_promise_result {
    const struct mo_cell *input;
    uint32_t subject;
    uint64_t calls;
    struct mo_cell value;
    bool has_value;
    const struct mo_input_extreme *extreme;
};

/* Refuse the entire selected set before initialization/observations. Listing
 * needs no generator. Closed scalars, tuples, Lists and visible data only;
 * generic subjects, opaque builders and factory inputs remain unavailable. */
MO_MUST_USE bool mo_promises_supported(const struct mo_program *program,
                                       const struct mo_test_selection *selection,
                                       struct mo_diags *diags);
/* An initialized, anchor-checked image. Owns all trial processes/retained values
 * until after reporting. A diagnosed inability has no invented outcome; false
 * means output itself failed and the selection must not continue. */
MO_MUST_USE bool mo_test_promise(const struct mo_cli *cli, struct mo_image *image,
                                 const struct mo_sources *sources,
                                 const struct mo_observation *observation, uint32_t function,
                                 FILE *output, struct mo_diags *diags,
                                 struct mo_test_totals *totals);

#endif
