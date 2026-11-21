/* Install a verified Mo CLI program as a bytecode image and POSIX launcher. */
#ifndef MO_TOOLS_INSTALL_H
#define MO_TOOLS_INSTALL_H

#include "tools/tools.h"

void mo_install_program(const struct mo_cli *cli, const struct mo_sources *sources,
                        const struct mo_buffer *wire, FILE *output, struct mo_diags *diags);

#endif
