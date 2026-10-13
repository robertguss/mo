/* The command and its bounded output. Tools is the integration boundary for the
 * staged compiler; it never presents local inference as a checked program. */
#ifndef MO_TOOLS_H
#define MO_TOOLS_H

#include "base/buffer.h"
#include "base/diag.h"
#include "base/limits.h"

#include <stdio.h>

#define MO_CLI_ARGUMENTS 256
#define MO_CLI_ARGUMENT_SIZE 4096
#define MO_CLI_ARGUMENT_BYTES (64 << 10)
#define MO_CLI_DIAGNOSTIC_SIZE (1 << 20)
#define MO_CLI_CHECK_SIZE (512 << 20)

enum mo_command {
    MO_COMMAND_CHECK,
    MO_COMMAND_RUN,
    MO_COMMAND_TEST,
    MO_COMMAND_FORMAT,
    MO_COMMAND_FIX,
    MO_COMMAND_GUIDE,
    MO_COMMAND_VERSION,
};

enum mo_emit { MO_EMIT_NONE, MO_EMIT_CORE, MO_EMIT_BYTECODE };

struct mo_cli {
    enum mo_command command;
    enum mo_emit emit;
    bool json, list, reference;
    struct mo_limits limits;
    uint32_t calls, seconds;
    const char *module;
    const char *only[MO_CLI_ARGUMENTS];
    uint32_t only_count;
    const char *files[MO_CLI_ARGUMENTS];
    uint32_t file_count;
    const char *const *arguments; /* run's arguments, borrowed from argv */
    uint32_t argument_count;
};

/* On refusal, error is a static, newline-free command error (exit 2). Nothing
 * has been written. Arguments and their strings are borrowed, not modified. */
MO_MUST_USE bool mo_cli_parse(int argc, const char *const argv[], struct mo_cli *out,
                              const char **error);
MO_MUST_USE const char *mo_version(void);
MO_MUST_USE const char *mo_version_line(void);

#endif
