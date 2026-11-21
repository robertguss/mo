#ifndef MO_TOOLS_API_SERVER_H
#define MO_TOOLS_API_SERVER_H

#include "tools/tools.h"

MO_MUST_USE bool mo_serve_maybe_api(const struct mo_cli *cli, FILE *output, FILE *errors,
                                    int *status_out);

#endif
