/* Private command integration. The production Unicode width callback is
 * supplied by execute.c, never by syntax. */
#ifndef MO_TOOLS_FORMAT_H
#define MO_TOOLS_FORMAT_H

#include "syntax/format.h"
#include "tools/tools.h"

MO_MUST_USE int mo_format_files(const struct mo_cli *cli, struct mo_format_options options,
                                FILE *errors);

#endif
