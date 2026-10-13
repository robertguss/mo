#define _POSIX_C_SOURCE 200809L

#include "tools/tools.h"

#include "base/codes.h"
#include "check/check.h"
#include "tools/embedded.h"

#include <errno.h>
#include <sys/stat.h>

static void unavailable(struct mo_diags *diags, const char *message) {
    struct mo_diag d = {.code = MO_CODE_TOOL_UNAVAILABLE,
                        .message = message,
                        .primary = {.span = {.file = UINT32_MAX}}};
    bool added = mo_diags_add(diags, &d);
    if (!added)
        MO_ASSERT(mo_diags_any(diags));
}

static void check_program(const struct mo_cli *cli, struct mo_arena *arena,
                          struct mo_sources *sources, struct mo_diags *diags) {
    struct stat st;
    /* No installed library yet is normal; an existing but unreadable folder
     * must still reach the loader's E1102 rather than silently disappear. */
    const char *library = mo_library_path;
    if (stat(library, &st) != 0 && errno == ENOENT)
        library = NULL;
    struct mo_module_set *set = mo_load_program(arena, sources, cli->files[0], library, diags);
    if (set == NULL || mo_diags_any(diags))
        return;
    struct mo_module_interfaces *interfaces = mo_resolve_modules(arena, set, diags);
    if (interfaces == NULL || mo_diags_any(diags))
        return;
    struct mo_signatures *signatures = mo_prepare_signatures(arena, set, interfaces, diags);
    if (signatures == NULL || mo_diags_any(diags))
        return;
    for (uint32_t i = 0; i < signatures->count; i++) {
        struct mo_inferred_body *body = mo_infer_function(arena, interfaces, signatures, i, diags);
        if (body == NULL) {
            if (diags->dropped != 0)
                break;
            MO_ASSERT(diags->count != 0);
            uint16_t code = diags->items[diags->count - 1].code;
            /* Resource failure is not another independent body mistake. */
            if (code >= 1000 && code != MO_CODE_CHECK_UNFINISHED)
                break;
        }
    }
    /* Integration seam: replace the preparation/inference above with mo_check
     * when it returns a whole checked program. Only that result may reach
     * mo_lower -> mo_compile -> image initialization and promise anchors.
     * Local inference results deliberately cannot be passed to lowering. */
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
        if (!ok)
            (void)fputs("mo: could not write guide\n", errors);
        return ok ? 0 : 1;
    }
    if (cli->json && !mo_write_version(output, cli->command)) {
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
        int status =
            mo_write_diagnostics(output, errors, cli->json, cli->command, &sources, &diags);
        mo_sources_free(&sources);
        mo_diags_free(&diags);
        return status;
    }
    struct mo_arena arena;
    mo_arena_init(&arena, MO_CLI_CHECK_SIZE);
    if (cli->emit != MO_EMIT_NONE)
        unavailable(&diags, "this build cannot emit Core or bytecode yet");
    else if (cli->command == MO_COMMAND_CHECK) {
        /* TODO(stream 1): run this callback on the forthcoming base helper's
         * explicitly sized 16 MiB C stack, including loading and parsing. Do
         * not change process-wide resource limits in this integration layer. */
        check_program(cli, &arena, &sources, &diags);
    } else {
        const char *message = "this build cannot run programs yet";
        if (cli->command == MO_COMMAND_TEST)
            message = "this build cannot run or list tests yet";
        if (cli->command == MO_COMMAND_FORMAT)
            message = "this build cannot format files yet";
        if (cli->command == MO_COMMAND_FIX)
            message = "this build cannot apply fixes yet";
        unavailable(&diags, message);
    }
    int status = mo_write_diagnostics(output, errors, cli->json, cli->command, &sources, &diags);
    mo_arena_free(&arena);
    mo_sources_free(&sources);
    mo_diags_free(&diags);
    return status;
}
