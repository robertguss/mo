#define _POSIX_C_SOURCE 200809L

#include "tools/tools.h"

#include "base/codes.h"
#include "base/stack.h"
#include "bytecode/bytecode.h"
#include "check/check.h"
#include "codegen/codegen.h"
#include "lower/lower.h"
#include "prim/prim.h"
#include "tools/embedded.h"
#include "tools/format.h"
#include "tools/run.h"
#include "tools/tests.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

static uint32_t display_width(const uint8_t *bytes, size_t length) {
    uint64_t width;
    MO_ASSERT(mo_unicode16_display_width(bytes, length, &width));
    MO_ASSERT(width <= UINT32_MAX);
    return (uint32_t)width;
}

static void tool_error(struct mo_diags *diags, uint16_t code, const char *message,
                       const char *detail) {
    struct mo_diag d = {.code = code,
                        .message = message,
                        .primary = {.span = {.file = UINT32_MAX}, .text = detail}};
    bool added = mo_diags_add(diags, &d);
    if (!added)
        MO_ASSERT(mo_diags_any(diags));
}

struct pipeline_work {
    const struct mo_cli *cli;
    struct mo_arena *arena;
    struct mo_sources *sources;
    struct mo_diags *diags;
    FILE *output, *errors;
    const char *mistake;
    bool version;
    int status;
};

static void execute_pipeline(void *argument) {
    struct pipeline_work *work = argument;
    const struct mo_cli *cli = work->cli;
    struct mo_arena *arena = work->arena;
    struct mo_sources *sources = work->sources;
    struct mo_diags *diags = work->diags;
    if (cli->command == MO_COMMAND_FIX) {
        tool_error(diags, MO_CODE_TOOL_UNAVAILABLE, "this build cannot apply fixes yet", NULL);
        return;
    }
    struct stat st;
    /* No installed library yet is normal; an existing but unreadable folder
     * must still reach the loader's E1102 rather than silently disappear. */
    const char *library = mo_library_path;
    if (stat(library, &st) != 0 && errno == ENOENT)
        library = NULL;
    struct mo_module_set *set =
        mo_load_program(arena, sources, mo_cli_file(cli, 0), library, diags);
    if (set == NULL || mo_diags_any(diags))
        return;
    struct mo_program *program = mo_check(arena, set, diags);
    if (program == NULL || mo_diags_any(diags))
        return;
    /* Test observations are separate from the compiler's anchor checks. */
    if (cli->command == MO_COMMAND_TEST) {
        struct mo_test_selection *selection = mo_select_tests(arena, program, cli, diags);
        if (selection == NULL)
            return;
        if (selection->mistake != NULL) {
            work->mistake = selection->mistake;
            return;
        }
        if (cli->json) {
            work->version = mo_write_version(work->output, cli->command);
            if (!work->version) {
                work->status = 1;
                return;
            }
        }
        if (cli->list) {
            for (uint32_t i = 0; i < selection->count; i++)
                if (!mo_write_test(work->output, cli->json, sources, &selection->items[i])) {
                    tool_error(diags, MO_CODE_TOOL_PIPELINE, "test inventory could not be written",
                               NULL);
                    break;
                }
            return;
        }
        tool_error(diags, MO_CODE_TOOL_UNAVAILABLE,
                   "this build cannot execute specification code yet", NULL);
        return;
    }
    bool anchors = false;
    for (uint32_t i = 0; i < program->spec_count; i++)
        anchors |= program->specs[i].kind == MO_SPEC_ANCHOR;
    struct mo_arena lowered;
    mo_arena_init(&lowered, MO_LIMIT_LOWER_WORK);
    struct mo_core *core = mo_lower(&lowered, program, sources, diags);
    struct mo_buffer wire, printed;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    mo_buffer_init(&printed, cli->emit == MO_EMIT_CORE ? MO_LIMIT_CORE_TEXT_SIZE
                                                       : MO_LIMIT_BYTECODE_TEXT_SIZE);
    if (core == NULL || mo_diags_any(diags) || !mo_core_check(core, diags))
        goto done;
    /* Plain check validates lowering too, but never executes the user's main. */
    if (cli->command == MO_COMMAND_CHECK && cli->emit == MO_EMIT_NONE && !anchors)
        goto done;
    if (anchors || cli->command == MO_COMMAND_RUN || cli->emit == MO_EMIT_BYTECODE) {
        if (!mo_compile(core, &wire, diags))
            goto done;
    }
    if (anchors || cli->command == MO_COMMAND_RUN) {
        work->status = mo_run_program(cli, core, &wire, sources, work->output, work->errors, diags);
        if (work->status != 0 || mo_diags_any(diags))
            goto done;
    }
    if (cli->emit == MO_EMIT_CORE) {
        if (!mo_core_print(core, &printed))
            tool_error(diags, MO_CODE_TOOL_PIPELINE, "Core text exceeds its output budget", NULL);
    } else if (cli->emit == MO_EMIT_BYTECODE) {
        struct mo_load_error error;
        if (!mo_bytecode_disassemble(wire.bytes, wire.length, &printed, &error))
            tool_error(diags, MO_CODE_TOOL_PIPELINE, "bytecode text could not be emitted",
                       error.reason);
    }
    if (cli->emit != MO_EMIT_NONE && !mo_diags_any(diags) &&
        (fwrite(printed.bytes, 1, printed.length, work->output) != printed.length ||
         fflush(work->output) != 0))
        tool_error(diags, MO_CODE_TOOL_PIPELINE, "compiled output could not be written", NULL);
done:
    mo_buffer_free(&printed);
    mo_buffer_free(&wire);
    mo_arena_free(&lowered);
}

int mo_cli_execute(const struct mo_cli *cli, FILE *output, FILE *errors) {
    if (cli->command == MO_COMMAND_VERSION) {
        bool ok = fprintf(output, "%s\n", mo_version_line()) >= 0 && fflush(output) == 0;
        if (!ok)
            (void)fputs("mo: could not write version\n", errors);
        return ok ? 0 : 1;
    }
    if (cli->command == MO_COMMAND_GUIDE) {
        if (!cli->reference && mo_embedded_guide_size == 0) {
            (void)fputs("the agent guide isn't written yet; use mo guide --reference\n", errors);
            return 1;
        }
        const unsigned char *data = cli->reference ? mo_embedded_reference : mo_embedded_guide;
        size_t length = cli->reference ? mo_embedded_reference_size : mo_embedded_guide_size;
        bool ok = fwrite(data, 1, length, output) == length && fflush(output) == 0;
        /* A reader such as head chose to stop; other output failures still fail. */
        if (!ok && errno == EPIPE)
            return 0;
        if (!ok)
            (void)fputs("mo: could not write guide\n", errors);
        return ok ? 0 : 1;
    }
    if (cli->command == MO_COMMAND_FORMAT) {
        struct mo_format_options options = {100, display_width};
        return mo_format_files(cli, options, errors);
    }
    bool version = cli->json && cli->command != MO_COMMAND_TEST;
    if (version && !mo_write_version(output, cli->command)) {
        (void)fputs("mo: could not write version record\n", errors);
        return 1;
    }
    struct mo_sources sources;
    struct mo_diags diags;
    bool have_sources = mo_sources_init(&sources);
    bool have_diags = mo_diags_init(&diags);
    if (!have_sources || !have_diags) {
        /* The failed initializers still prepare their arenas for freeing. */
        diags.dropped = 1;
        if (cli->json && !version)
            version = mo_write_version(output, cli->command);
        int status = cli->json && !version ? 1
                                           : mo_write_diagnostics(output, errors, cli->json,
                                                                  cli->command, &sources, &diags);
        mo_sources_free(&sources);
        mo_diags_free(&diags);
        return status;
    }
    struct mo_arena arena;
    mo_arena_init(&arena, MO_CLI_CHECK_SIZE);
    struct pipeline_work work = {.cli = cli,
                                 .arena = &arena,
                                 .sources = &sources,
                                 .diags = &diags,
                                 .output = output,
                                 .errors = errors,
                                 .version = version};
    int error = mo_with_stack(MO_SOURCE_STACK_SIZE, execute_pipeline, &work);
    if (error != 0)
        tool_error(&diags, MO_CODE_TOOL_SOURCE_STACK, "could not prepare the source pipeline stack",
                   strerror(error));
    int status;
    if (work.mistake != NULL) {
        (void)fprintf(errors, "%s\n", work.mistake);
        status = 2;
    } else if (cli->json && !work.version && work.status != 0) {
        (void)fputs("mo: could not write version record\n", errors);
        status = 1;
    } else if (cli->json && !work.version && !mo_write_version(output, cli->command))
        status = 1;
    else
        status = mo_write_diagnostics(output, errors, cli->json, cli->command, &sources, &diags);
    mo_arena_free(&arena);
    mo_sources_free(&sources);
    mo_diags_free(&diags);
    return status != 0 ? status : work.status;
}
