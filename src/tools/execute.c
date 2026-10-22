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
#include "tools/fix.h"
#include "tools/format.h"
#include "tools/promises.h"
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
    uint32_t file; /* the cli's file whose program this checks */
    struct mo_arena *arena;
    struct mo_sources *sources;
    struct mo_diags *diags;
    FILE *output, *errors;
    const char *mistake;
    bool version;
    struct mo_test_totals totals;
    int status;
};

static void execute_pipeline(void *argument) {
    struct pipeline_work *work = argument;
    const struct mo_cli *cli = work->cli;
    struct mo_arena *arena = work->arena;
    struct mo_sources *sources = work->sources;
    struct mo_diags *diags = work->diags;
    struct stat st;
    /* No installed library yet is normal; an existing but unreadable folder
     * must still reach the loader's E1102 rather than silently disappear. */
    const char *library = mo_library_path;
    if (stat(library, &st) != 0 && errno == ENOENT)
        library = NULL;
    struct mo_module_set *set =
        mo_load_program(arena, sources, mo_cli_file(cli, work->file), library, diags);
    if (set == NULL || mo_diags_any(diags))
        return;
    struct mo_program *program = mo_check(arena, set, diags);
    if (program == NULL || mo_diags_any(diags))
        return;
    if (cli->command == MO_COMMAND_RUN && program->entry.function == UINT32_MAX) {
        const char *message = "the entry module has no main function";
        struct mo_diag diag = {.code = MO_CODE_MAIN_CONTRACT,
                               .primary = {set->modules[0]->span, message},
                               .message = message,
                               .reason = "main declares the program's entry (reference 11.2)."};
        bool added = mo_diags_add(diags, &diag);
        (void)added;
        return;
    }
    /* Test observations are separate from the compiler's anchor checks. */
    struct mo_test_selection *selection = NULL;
    if (cli->command == MO_COMMAND_TEST) {
        selection = mo_select_tests(arena, program, cli, diags);
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
        /* Refuse the entire selection before starting it, never a partial green
         * run that quietly omitted unsupported promises (including templates). */
        if (!mo_promises_supported(program, selection, diags))
            return;
    }
    bool anchors = false;
    for (uint32_t i = 0; i < program->spec_count; i++) {
        uint32_t module = program->specs[i].module;
        /* Lowering leaves out a library module's anchors, as it does all its specs. */
        bool in_library =
            module != 0 && program->set->library != NULL && program->set->library[module];
        anchors |= program->specs[i].kind == MO_SPEC_ANCHOR && !in_library;
    }
    struct mo_arena lowered;
    mo_arena_init(&lowered, MO_LIMIT_LOWER_WORK);
    struct mo_core *core = mo_lower(&lowered, program, sources, diags);
    struct mo_buffer wire, printed;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    mo_buffer_init(&printed, cli->emit == MO_EMIT_CORE ? MO_LIMIT_CORE_TEXT_SIZE
                                                       : MO_LIMIT_BYTECODE_TEXT_SIZE);
    if (core == NULL || mo_diags_any(diags) || !mo_core_check(core, diags))
        goto done;
    /* Plain check and fix validate lowering too, but never execute the user's main. */
    bool checking = cli->command == MO_COMMAND_CHECK || cli->command == MO_COMMAND_FIX;
    if (checking && cli->emit == MO_EMIT_NONE && !anchors)
        goto done;
    if (anchors || cli->command == MO_COMMAND_RUN || cli->command == MO_COMMAND_TEST ||
        cli->emit == MO_EMIT_BYTECODE) {
        if (!mo_compile(core, &wire, diags))
            goto done;
    }
    if (cli->command == MO_COMMAND_TEST) {
        mo_test_program(cli, program, selection, &wire, sources, work->output, diags,
                        &work->totals);
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

/* Applies the fixes the check offers alone to the checked file, which is source 0 once loaded, and
 * says whether it rewrote the file. When it can't, problem says why. */
static bool apply_fixes(const char *path, const struct stat *original,
                        const struct mo_sources *sources, struct mo_diags *diags,
                        char problem[192]) {
    if (sources->count == 0)
        return false;
    struct mo_buffer fixed;
    mo_buffer_init(&fixed, MO_LIMIT_FILE_SIZE);
    uint32_t applied;
    bool rewrote = false;
    if (!mo_fix_source(sources, 0, diags, &fixed, &applied)) {
        (void)snprintf(problem, 192, "the fixed file exceeds this build's file size limit");
    } else if (applied != 0) {
        int error = 0;
        rewrote = mo_replace_file(path, &fixed, original, &error);
        if (!rewrote) {
            int written =
                snprintf(problem, 192, "could not replace the fixed file (%s)", strerror(error));
            MO_ASSERT(written > 0);
        }
    }
    mo_buffer_free(&fixed);
    return rewrote;
}

/* Reports E1407 alone: a file fix can't read or replace, which names the file in its headline as
 * format's E1402 does. */
static int refuse_fix(const char *path, const char *detail, FILE *output, FILE *errors) {
    struct mo_sources sources;
    struct mo_diags diags;
    bool have_sources = mo_sources_init(&sources);
    bool have_diags = mo_diags_init(&diags);
    if (!have_sources || !have_diags)
        diags.dropped = 1;
    else {
        char message[MO_LIMIT_PATH_SIZE + 256];
        int written =
            snprintf(message, sizeof message, "%s: %.*s%s", detail, (int)MO_LIMIT_PATH_SIZE, path,
                     strlen(path) > MO_LIMIT_PATH_SIZE ? "..." : "");
        MO_ASSERT(written >= 0 && (size_t)written < sizeof message);
        tool_error(&diags, MO_CODE_TOOL_FIX_IO, message, NULL);
    }
    int status =
        mo_write_diagnostics(output, errors, false, MO_COMMAND_FIX, &sources, &diags, NULL);
    mo_sources_free(&sources);
    mo_diags_free(&diags);
    return status;
}

/* Checks the program of the cli's file-th file and writes its diagnostics. Given the file's
 * lstat instead, it applies the fixes the check offers and writes nothing about the check: it
 * returns CHECK_AGAIN when it rewrote the file, 0 when no fix applied, and 1 after reporting why it
 * couldn't rewrite it. */
enum { CHECK_AGAIN = -1 };
static int check_program(const struct mo_cli *cli, uint32_t file, FILE *output, FILE *errors,
                         bool version, const struct stat *fixing) {
    struct mo_sources sources;
    struct mo_diags diags;
    bool have_sources = mo_sources_init(&sources);
    bool have_diags = mo_diags_init(&diags);
    if (!have_sources || !have_diags) {
        /* The failed initializers still prepare their arenas for freeing. */
        diags.dropped = 1;
        if (cli->json && !version)
            version = mo_write_version(output, cli->command);
        int status = cli->json && !version
                         ? 1
                         : mo_write_diagnostics(output, errors, cli->json, cli->command, &sources,
                                                &diags, NULL);
        mo_sources_free(&sources);
        mo_diags_free(&diags);
        return status;
    }
    struct mo_arena arena;
    mo_arena_init(&arena, MO_CLI_CHECK_SIZE);
    struct pipeline_work work = {.cli = cli,
                                 .file = file,
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
    char problem[192] = "";
    if (fixing != NULL) {
        const char *path = mo_cli_file(cli, file);
        status = apply_fixes(path, fixing, &sources, &diags, problem) ? CHECK_AGAIN
                 : problem[0] != '\0' ? refuse_fix(path, problem, output, errors)
                                      : 0;
    } else if (work.mistake != NULL) {
        (void)fprintf(errors, "%s\n", work.mistake);
        status = 2;
    } else if (cli->json && !work.version && work.status != 0) {
        (void)fputs("mo: could not write version record\n", errors);
        status = 1;
    } else if (cli->json && !work.version && !mo_write_version(output, cli->command))
        status = 1;
    else
        status = mo_write_diagnostics(output, errors, cli->json, cli->command, &sources, &diags,
                                      &work.totals);
    mo_arena_free(&arena);
    mo_sources_free(&sources);
    mo_diags_free(&diags);
    return status != 0 ? status : work.status;
}

static bool regular_file(const char *path, struct stat *st) {
    return strlen(path) <= MO_LIMIT_PATH_SIZE && lstat(path, st) == 0 && S_ISREG(st->st_mode);
}

/* Each file is checked as the program it starts, fixed, and checked again, until no fix applies or
 * MO_LIMIT_FIX_ROUNDS rounds have rewritten it. Only the given files are rewritten (reference
 * 13.4). Once every file is fixed, each is checked once more for the errors that are left, so no
 * report shows an error a later file's fix removed. */
static int fix_files(const struct mo_cli *cli, FILE *output, FILE *errors) {
    int status = 0;
    struct stat st;
    for (uint32_t i = 0; i < cli->file_count; i++) {
        int result = CHECK_AGAIN;
        for (uint32_t round = 0; result == CHECK_AGAIN && round < MO_LIMIT_FIX_ROUNDS &&
                                 regular_file(mo_cli_file(cli, i), &st);
             round++)
            result = check_program(cli, i, output, errors, false, &st);
        status = result > status ? result : status;
    }
    for (uint32_t i = 0; i < cli->file_count; i++) {
        const char *path = mo_cli_file(cli, i);
        int result =
            regular_file(path, &st)
                ? check_program(cli, i, output, errors, false, NULL)
                : refuse_fix(path,
                             "fix needs a readable regular file, not a symlink or special file",
                             output, errors);
        status = result > status ? result : status;
    }
    return status;
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
    if (cli->command == MO_COMMAND_FIX)
        return fix_files(cli, output, errors);
    bool version = cli->json && cli->command != MO_COMMAND_TEST;
    if (version && !mo_write_version(output, cli->command)) {
        (void)fputs("mo: could not write version record\n", errors);
        return 1;
    }
    return check_program(cli, 0, output, errors, version, NULL);
}
