/* Semantic diff between two Mo programs: authority, public/wire surface, saved state schema,
 * and behavioral contracts (OLD's promises and examples run against NEW). */
#ifndef MO_TOOLS_DIFF_H
#define MO_TOOLS_DIFF_H

#include "tools/tools.h"

MO_MUST_USE int mo_diff(const struct mo_cli *cli, FILE *output, FILE *errors);

#endif
