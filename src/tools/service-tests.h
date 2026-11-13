/* A service's promises and recovery check (reference 12.5), judged on one search's sequences. */
#ifndef MO_TOOLS_SERVICE_TESTS_H
#define MO_TOOLS_SERVICE_TESTS_H

#include "tools/tests.h"

/* items are count consecutive selected checks of one service, the one at index service in image,
 * which is initialized and anchor-checked. Writes their results in order. false when a result
 * could not be written, which ends the run. */
MO_MUST_USE bool mo_test_service(const struct mo_cli *cli, const struct mo_program *program,
                                 struct mo_image *image, const struct mo_sources *sources,
                                 const struct mo_observation *items, uint32_t count,
                                 uint32_t service, FILE *output, struct mo_diags *diags,
                                 struct mo_test_totals *totals);

#endif
