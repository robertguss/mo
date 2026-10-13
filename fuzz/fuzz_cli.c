/* Fuzzes the argument parser: the input's zero bytes split it into arguments.
 * Whatever arrives, the parser must return a known command without tripping
 * an assertion or a sanitizer. */
#include "tools/tools.h"

#include "base/codes.h"

#include <locale.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerInitialize(int *argc, char ***argv);

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    const char *locales[] = {"C.UTF-8", "en_US.UTF-8", "UTF-8"};
    for (unsigned i = 0; i < sizeof(locales) / sizeof(locales[0]); i++) {
        if (setlocale(LC_CTYPE, locales[i]) != NULL)
            return 0;
    }
    MO_ASSERT(false); /* Exercise the cluster path, not just ASCII escapes. */
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    enum { MAX_ARGUMENTS = 16 };
    if (size > MO_CLI_ARGUMENT_BYTES)
        return 0;
    char *copy = malloc(size + 1);
    if (copy == NULL) {
        return 0;
    }
    if (size > 0) {
        memcpy(copy, data, size);
    }
    copy[size] = '\0';

    const char *argv[MAX_ARGUMENTS + 1];
    int argc = 0;
    argv[argc++] = "mo";
    size_t argument_start = 0;
    for (size_t i = 0; i <= size && argc < MAX_ARGUMENTS; i++) {
        if (i == size || copy[i] == '\0') {
            argv[argc++] = copy + argument_start;
            argument_start = i + 1;
        }
    }
    argv[argc] = NULL;

    struct mo_cli cli;
    const char *error;
    bool parsed = mo_cli_parse(argc, argv, &cli, &error);
    MO_ASSERT(parsed || error != NULL);
    /* Exercise successful command prefixes too, not only unknown commands. */
    if (argc > 1) {
        const char *commands[] = {"check", "run", "test", "format", "fix", "guide", "--version"};
        argv[1] = commands[size == 0 ? 0 : data[0] % 7];
        parsed = mo_cli_parse(argc, argv, &cli, &error);
        MO_ASSERT(parsed || error != NULL);
    }
    if (parsed) {
        for (uint32_t i = 0; i < cli.file_count; i++)
            MO_ASSERT(mo_cli_file(&cli, i)[0] != '\0');
    } else {
        MO_ASSERT(strlen(error) < MO_CLI_ERROR_SIZE && strchr(error, '\n') == NULL);
    }
    struct mo_sources sources;
    MO_ASSERT(mo_sources_init(&sources));
    uint32_t file;
    struct mo_source_error problem;
    char path[MO_LIMIT_PATH_SIZE + 1];
    size_t path_size = size < MO_LIMIT_PATH_SIZE ? size : MO_LIMIT_PATH_SIZE;
    if (path_size != 0)
        memcpy(path, data, path_size);
    path[path_size] = '\0';
    MO_ASSERT(mo_sources_add_bytes(&sources, path, data, size, &file, &problem));
    uint32_t start = 0, end = 0;
    for (size_t i = 0; i < size && i < 8; i++) {
        if (i < 4)
            start = (start << 8) | data[i];
        else
            end = (end << 8) | data[i];
    }
    start %= (uint32_t)size + 1;
    end %= (uint32_t)size + 1;
    if (end < start) {
        uint32_t swap = start;
        start = end;
        end = swap;
    }
    struct mo_span span = {size > 2 && data[2] % 4 == 0 ? UINT32_MAX : file, start, end};
    struct mo_edit edit = {span, copy};
    struct mo_fix fix = {copy, &edit, 1};
    struct mo_label secondary[] = {{span, copy}, {{UINT32_MAX, 0, 0}, NULL}};
    static const uint16_t codes[] = {
#define FUZZ_CODE(number, name, section) number,
        MO_CODES(FUZZ_CODE)
#undef FUZZ_CODE
    };
    uint16_t code = codes[size == 0 ? 0 : data[0] % (sizeof(codes) / sizeof(codes[0]))];
    struct mo_diag d = {.code = code,
                        .message = copy,
                        .reason = copy,
                        .section = mo_code_find(code)->section,
                        .primary = {span, copy},
                        .secondary = secondary,
                        .secondary_count = 2,
                        .fixes = &fix,
                        .fix_count = 1};
    struct mo_buffer out;
    size_t limit = size > 3 ? ((size_t)data[3] + 1) * 4096 : MO_CLI_DIAGNOSTIC_SIZE;
    mo_buffer_init(&out, limit);
    bool rendered = mo_render_diagnostic(&sources, &d, true, &out);
    MO_ASSERT(rendered || out.length == 0);
    rendered = mo_render_diagnostic(&sources, &d, false, &out);
    MO_ASSERT(rendered || out.length == 0);
    mo_buffer_free(&out);
    mo_sources_free(&sources);
    free(copy);
    return 0;
}
