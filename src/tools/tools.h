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
    MO_COMMAND_EMIT,
    MO_COMMAND_SERVE,
    MO_COMMAND_VERSION,
    MO_COMMAND_FETCH,
};

#define MO_EXIT_SERVICE_STOPPED 69
/* EX_TEMPFAIL: another process held the store; a service manager retries. */
#define MO_EXIT_STORE_BUSY 75

enum mo_emit { MO_EMIT_NONE, MO_EMIT_CORE, MO_EMIT_BYTECODE };

struct mo_cli {
    enum mo_command command;
    enum mo_emit emit;
    bool json, list, reference;
    struct mo_limits limits;
    uint32_t calls, seconds;
    const char *fetch_url, *fetch_token;
    uint64_t fetch_failure;
    const char *module;
    const char *output; /* emit's -o file */
    uint16_t port;
    const char *data;
    const char *allow_host;
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
struct mo_test_totals {
    uint32_t passed, failed, stopped;
};
/* Writes and flushes each complete record. Sorts diagnostics in place. Returns
 * 0 on success, 1 on diagnostics/drops/output failure. Text goes to errors,
 * JSON exclusively to output. JSON's version must have been written first.
 * Optional test totals also determine status; NULL means no observations ran. */
MO_MUST_USE bool mo_write_version(FILE *output, enum mo_command command);
MO_MUST_USE int mo_write_diagnostics(FILE *output, FILE *errors, bool json, enum mo_command command,
                                     const struct mo_sources *sources, struct mo_diags *diags,
                                     const struct mo_test_totals *totals);
struct mo_process;
struct mo_defect;
/* Structural VM report with terminal-safe text escaping. Borrows the stopped
 * process/image until returning, including initialization reports with NULL process. */
MO_MUST_USE bool mo_write_defect(FILE *errors, const struct mo_process *process,
                                 const struct mo_defect *defect);
struct mo_image;
struct mo_export_page;
enum mo_fetch_read_status { MO_FETCH_PAGE, MO_FETCH_REFUSED, MO_FETCH_REPLACED, MO_FETCH_RELEASED };
/* Bounded embedded HTTP/MCP reader; refuses malformed page encodings. */
MO_MUST_USE enum mo_fetch_read_status mo_fetch_read(struct mo_image *image, uint32_t function,
                                                    const struct mo_buffer *wire,
                                                    struct mo_export_page *out);
MO_MUST_USE int mo_fetch(const struct mo_cli *cli, FILE *errors);
/* Whole checking and the certified lowering/runtime pipeline run inside one
 * explicit source-stack call. Owns no borrowed data after returning. */
MO_MUST_USE int mo_cli_execute(const struct mo_cli *cli, FILE *output, FILE *errors);

#endif
