#define _POSIX_C_SOURCE 200809L

#include "tools/format.h"

#include "base/codes.h"
#include "base/stack.h"
#include "tools/fix.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void problem(struct mo_diags *diags, uint32_t file, uint16_t code, const char *path,
                    const char *detail) {
    char message[MO_LIMIT_PATH_SIZE + 256];
    size_t length = strlen(path);
    int written = snprintf(message, sizeof message, "%s: %.*s%s", detail, (int)MO_LIMIT_PATH_SIZE,
                           path, length > MO_LIMIT_PATH_SIZE ? "..." : "");
    MO_ASSERT(written >= 0 && (size_t)written < sizeof message);
    struct mo_diag diag = {.code = code, .message = message, .primary = {.span = {.file = file}}};
    bool added = mo_diags_add(diags, &diag);
    if (!added)
        MO_ASSERT(diags->dropped != 0);
}

struct format_work {
    struct mo_sources *sources;
    struct mo_diags *diags;
    struct mo_buffer *output;
    struct mo_format_options options;
    bool rendered;
};

static void print_source(void *argument) {
    struct format_work *work = argument;
    struct mo_arena arena;
    mo_arena_init(&arena, MO_CLI_CHECK_SIZE);
    struct mo_module_syntax *module = mo_parse(&arena, work->sources, 0, work->diags);
    if (module != NULL && !mo_diags_any(work->diags)) {
        work->rendered = mo_format(module, work->options, work->output);
        if (!work->rendered)
            problem(work->diags, 0, MO_CODE_TOOL_FORMAT_LIMIT, work->sources->files[0].path,
                    "source printing exceeds this build's memory, depth, output or line limit");
    }
    mo_arena_free(&arena);
}

static void format_file(const char *path, struct mo_format_options options,
                        struct mo_sources *sources, struct mo_diags *diags) {
    if (strlen(path) > MO_LIMIT_PATH_SIZE) {
        problem(diags, UINT32_MAX, MO_CODE_PATH_TOO_LONG, path,
                "the source path exceeds this build's limit");
        return;
    }
    /* Check before reading: a FIFO or device must never block the command. */
    struct stat original;
    if (lstat(path, &original) != 0 || !S_ISREG(original.st_mode)) {
        problem(diags, UINT32_MAX, MO_CODE_TOOL_FORMAT_IO, path,
                "format needs a readable regular file, not a symlink or special file");
        return;
    }
    uint32_t file;
    struct mo_source_error error;
    if (!mo_sources_add(sources, path, &file, &error)) {
        uint16_t code = error.problem == MO_SOURCE_UNREADABLE      ? MO_CODE_TOOL_FORMAT_IO
                        : error.problem == MO_SOURCE_TOO_BIG       ? MO_CODE_FILE_TOO_BIG
                        : error.problem == MO_SOURCE_PATH_TOO_LONG ? MO_CODE_PATH_TOO_LONG
                                                                   : MO_CODE_TOOL_FORMAT_LIMIT;
        problem(diags, UINT32_MAX, code, path, "the source file could not be read for formatting");
        return;
    }
    MO_ASSERT(file == 0);
    struct mo_buffer output;
    mo_buffer_init(&output, MO_LIMIT_FILE_SIZE);
    struct format_work work = {sources, diags, &output, options, false};
    int status = mo_with_stack(MO_SOURCE_STACK_SIZE, print_source, &work);
    if (status != 0)
        problem(diags, file, MO_CODE_TOOL_FORMAT_LIMIT, path,
                "the source stack could not be prepared for formatting");
    const struct mo_source *source = &sources->files[file];
    if (work.rendered && (output.length != source->length ||
                          memcmp(output.bytes, source->bytes, output.length) != 0)) {
        int system_error = 0;
        if (!mo_replace_file(source->path, &output, &original, &system_error)) {
            char detail[192];
            int written =
                snprintf(detail, sizeof detail, "could not replace the formatted file (%s)",
                         strerror(system_error));
            MO_ASSERT(written >= 0);
            problem(diags, file, MO_CODE_TOOL_FORMAT_IO, path, detail);
        }
    }
    mo_buffer_free(&output);
}

int mo_format_files(const struct mo_cli *cli, struct mo_format_options options, FILE *errors) {
    uint32_t shown = 0;
    uint64_t dropped = 0;
    bool failed = false;
    struct mo_buffer rendered;
    mo_buffer_init(&rendered, MO_CLI_DIAGNOSTIC_SIZE);
    for (uint32_t i = 0; i < cli->file_count; i++) {
        /* Independent files do not consume a whole program's source budget.
         * Render before freeing each file, retaining at most one file's AST. */
        struct mo_sources sources;
        struct mo_diags diags;
        bool have_sources = mo_sources_init(&sources);
        bool have_diags = mo_diags_init(&diags);
        if (!have_sources || !have_diags) {
            dropped++;
            failed = true;
        } else {
            format_file(mo_cli_file(cli, i), options, &sources, &diags);
            failed = failed || mo_diags_any(&diags);
            dropped += diags.dropped;
            mo_diags_sort(&diags);
            for (uint32_t j = 0; j < diags.count; j++) {
                if (shown == MO_LIMIT_DIAGNOSTICS ||
                    !mo_render_diagnostic(&sources, &diags.items[j], false, &rendered)) {
                    dropped++;
                    continue;
                }
                if (fwrite(rendered.bytes, 1, rendered.length, errors) != rendered.length ||
                    fflush(errors) != 0) {
                    mo_sources_free(&sources);
                    mo_diags_free(&diags);
                    mo_buffer_free(&rendered);
                    return 1;
                }
                shown++;
            }
        }
        mo_sources_free(&sources);
        mo_diags_free(&diags);
    }
    mo_buffer_free(&rendered);
    if (dropped != 0 &&
        (fprintf(errors, "mo: %" PRIu64 " diagnostics could not be shown\n", dropped) < 0 ||
         fflush(errors) != 0))
        return 1;
    return failed || dropped != 0 ? 1 : 0;
}
