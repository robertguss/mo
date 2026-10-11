/* The mo command's arguments. A placeholder until Phase 2 builds the real
 * toolchain. */
#ifndef MO_CLI_H
#define MO_CLI_H

#include "base.h"

/* What the command line asks for. */
typedef enum {
    MO_COMMAND_STATUS,  /* No arguments, or arguments the placeholder doesn't know. */
    MO_COMMAND_VERSION, /* --version or -V. */
} mo_command;

/* Reads the arguments exactly as main receives them, program name first. */
MO_MUST_USE mo_command mo_cli_parse(int argc, const char *const argv[]);

/* The version line, such as "mo 0.0.0". */
MO_MUST_USE const char *mo_version_line(void);

#endif
