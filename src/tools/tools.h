/* The command and its bounded output. Tools is the integration boundary for the
 * staged compiler; it never presents local inference as a checked program. */
#ifndef MO_TOOLS_H
#define MO_TOOLS_H

#include "base/buffer.h"
#include "base/diag.h"
#include "base/limits.h"

#include <stdio.h>

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
    const char *const *files; /* borrowed argv slice; use mo_cli_file to skip -- */
    uint32_t file_count;
    uint32_t file_separator;      /* index of an intervening --, or UINT32_MAX */
    const char *const *arguments; /* run's arguments, borrowed from argv */
    uint32_t argument_count;
    char error[MO_CLI_ERROR_SIZE];
};

/* On refusal, error points into out: a bounded, newline-free command error
 * naming the offending argument (exit 2). Nothing has been written. Arguments
 * and file lists are borrowed, not modified; only Mo's own options are bounded. */
MO_MUST_USE bool mo_cli_parse(int argc, const char *const argv[], struct mo_cli *out,
                              const char **error);
MO_MUST_USE const char *mo_cli_file(const struct mo_cli *cli, uint32_t index);
MO_MUST_USE const char *mo_version(void);
MO_MUST_USE const char *mo_version_line(void);

/* Renders one complete diagnostic, including its trailing newline. The caller
 * supplies a buffer bounded by MO_CLI_DIAGNOSTIC_SIZE. False clears the buffer:
 * count that diagnostic as dropped, never publish an incomplete JSON record.
 * Missing files (including UINT32_MAX) are unavailable, not source indices. */
MO_MUST_USE bool mo_render_diagnostic(const struct mo_sources *sources, const struct mo_diag *diag,
                                      bool json, struct mo_buffer *out);
/* Writes and flushes each complete record. Sorts diagnostics in place. Returns
 * 0 on success, 1 on diagnostics/drops/output failure. Text goes to errors,
 * JSON exclusively to output. JSON's version must have been written first. */
MO_MUST_USE bool mo_write_version(FILE *output, enum mo_command command);
MO_MUST_USE int mo_write_diagnostics(FILE *output, FILE *errors, bool json, enum mo_command command,
                                     const struct mo_sources *sources, struct mo_diags *diags);
struct mo_process;
struct mo_defect;
/* Structural VM report with terminal-safe text escaping. Borrows the stopped
 * process/image until returning, including initialization reports with NULL process. */
MO_MUST_USE bool mo_write_defect(FILE *errors, const struct mo_process *process,
                                 const struct mo_defect *defect);
/* Whole checking and the certified lowering/runtime pipeline run inside one
 * explicit source-stack call. Owns no borrowed data after returning. */
MO_MUST_USE int mo_cli_execute(const struct mo_cli *cli, FILE *output, FILE *errors);

#endif
