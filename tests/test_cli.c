#include "test.h"
#include "tools/tools.h"

#include "base/codes.h"

#include <stdlib.h>
#include <string.h>

static void parse_commands(void) {
    struct mo_cli cli;
    const char *error;
    const char *args[] = {"mo", "run", "--max-stack", "17MiB", "main.mo", "--json", ""};
    CHECK(mo_cli_parse(7, args, &cli, &error));
    CHECK(cli.limits.stack_size == 17u * 1024u * 1024u);
    CHECK(cli.argument_count == 2 && strcmp(cli.arguments[0], "--json") == 0);
    const char *wrong[] = {"mo", "check", "--json", "--max-heap", "1GiB", "main.mo"};
    CHECK(!mo_cli_parse(6, wrong, &cli, &error));
    CHECK(strcmp(error, "flag is not accepted by this command") == 0);
    CHECK(!mo_cli_parse(0, NULL, &cli, &error));
}

static void limit_values(void) {
    struct mo_cli cli;
    const char *error;
    const char *args[] = {"mo",          "test",         "--only",      "Main.test.2",
                          "--only",      "Other.test.1", "--in",        "Main",
                          "--list",      "--json",       "--max-stack", "001KiB",
                          "--max-depth", "4294967295",   "--max-heap",  "18446744073709551615",
                          "--calls",     "37",           "--seconds",   "3",
                          "main.mo"};
    CHECK(mo_cli_parse(21, args, &cli, &error));
    CHECK(cli.only_count == 2 && strcmp(cli.only[1], "Other.test.1") == 0);
    CHECK(cli.limits.stack_size == 1024 && cli.limits.heap_size == SIZE_MAX);
    CHECK(cli.limits.call_depth == UINT32_MAX && cli.calls == 37 && cli.seconds == 3);
    const char *defaults[] = {"mo", "run", "main.mo"};
    CHECK(mo_cli_parse(3, defaults, &cli, &error));
    CHECK(cli.limits.stack_size == 8388608 && cli.limits.call_depth == 10000 &&
          cli.limits.heap_size == 1073741824 && cli.calls == 200 && cli.seconds == 2);
    char long_arg[MO_CLI_ARGUMENT_SIZE + 2];
    memset(long_arg, 'x', sizeof(long_arg));
    long_arg[MO_CLI_ARGUMENT_SIZE] = '\0';
    const char *boundary[] = {"mo", "run", "main.mo", long_arg};
    CHECK(mo_cli_parse(4, boundary, &cli, &error));
    long_arg[MO_CLI_ARGUMENT_SIZE] = 'x';
    long_arg[MO_CLI_ARGUMENT_SIZE + 1] = '\0';
    CHECK(!mo_cli_parse(4, boundary, &cli, &error));
}

/* An externally visible rendering fixture: tabs, wide/combining scalars,
 * CRLF, invalid bytes, a multi-line primary and an EOF secondary. Edits are
 * intentionally unsorted and meet at replacement/insertion boundaries. */
static bool rendering(bool json, FILE *stream) {
    struct mo_sources sources;
    MO_ASSERT(mo_sources_init(&sources));
    const char source[] = "\t\xe7\x95\x8c"
                          "e\xcc\x81\r\nsecond\nthird\nfourth\nfifth\nsixth\n";
    uint32_t file;
    struct mo_source_error error;
    MO_ASSERT(mo_sources_add_bytes(&sources, "bad\xff\"\\.mo", (const uint8_t *)source,
                                   sizeof(source) - 1, &file, &error));
    MO_ASSERT(
        mo_sources_add_bytes(&sources, "other.mo", (const uint8_t *)"abc\nxyz", 7, &file, &error));
    struct mo_label secondary[] = {{{0, sizeof(source) - 1, sizeof(source) - 1}, "insert at EOF"},
                                   {{UINT32_MAX, 0, 0}, NULL}};
    struct mo_edit edits[] = {{{1, 2, 3}, "C\nZ"}, {{1, 1, 2}, "B"}, {{1, 1, 1}, "+"}};
    struct mo_fix fix = {"replace together", edits, 3};
    struct mo_diag d = {.code = MO_CODE_NAME_CLASH,
                        .message = "quoted \"headline\"\\\t\xff",
                        .reason = "A clash between names is an error (reference 10.3).",
                        .section = "10.3",
                        .primary = {{0, 1, sizeof(source) - 1}, "wide span"},
                        .secondary = secondary,
                        .secondary_count = 2,
                        .fixes = &fix,
                        .fix_count = 1};
    struct mo_buffer out;
    mo_buffer_init(&out, MO_CLI_DIAGNOSTIC_SIZE);
    bool ok = mo_render_diagnostic(&sources, &d, json, &out);
    if (ok)
        ok = fwrite(out.bytes, 1, out.length, stream) == out.length;
    mo_buffer_free(&out);
    mo_sources_free(&sources);
    return ok;
}

static bool render_bytes(void) {
    char all[256];
    for (unsigned i = 1; i <= 255; i++)
        all[i - 1] = (char)i;
    all[255] = '\0';
    struct mo_sources sources = {0};
    struct mo_diag d = {.code = MO_CODE_TOOL_UNAVAILABLE,
                        .message = all,
                        .primary = {{UINT32_MAX, 0, 0}, "\xc3\xa9\xe7\x95\x8c\xf0\x9f\x98\x80"}};
    struct mo_buffer out;
    mo_buffer_init(&out, MO_CLI_DIAGNOSTIC_SIZE);
    bool ok = mo_render_diagnostic(&sources, &d, true, &out);
    if (ok)
        ok = fwrite(out.bytes, 1, out.length, stdout) == out.length;
    mo_buffer_free(&out);
    return ok;
}

static void long_line(void) {
    uint8_t source[600];
    memset(source, 'a', sizeof(source));
    source[500] = 'Z';
    struct mo_sources sources;
    MO_ASSERT(mo_sources_init(&sources));
    uint32_t file;
    struct mo_source_error error;
    MO_ASSERT(mo_sources_add_bytes(&sources, "long.mo", source, sizeof(source), &file, &error));
    struct mo_diag d = {.code = MO_CODE_TOOL_UNAVAILABLE,
                        .message = "long line",
                        .primary = {{file, 500, 501}, "here"}};
    struct mo_buffer out;
    mo_buffer_init(&out, MO_CLI_DIAGNOSTIC_SIZE);
    CHECK(mo_render_diagnostic(&sources, &d, false, &out));
    MO_ASSERT(mo_buffer_append_byte(&out, 0));
    CHECK(strstr((const char *)out.bytes, "long.mo:1:501") != NULL);
    CHECK(strstr((const char *)out.bytes, "  1 | ...aaaaaaaaaaaaZ") != NULL);
    CHECK(strstr((const char *)out.bytes, "    |                ^ here") != NULL);
    mo_buffer_free(&out);
    mo_sources_free(&sources);
}

static void omission(void) {
    struct mo_sources sources = {0};
    struct mo_buffer buffer;
    mo_buffer_init(&buffer, 20);
    struct mo_diag d = {.code = MO_CODE_TOOL_UNAVAILABLE,
                        .message = "cannot run yet",
                        .primary = {{UINT32_MAX, 0, 0}, NULL}};
    CHECK(!mo_render_diagnostic(&sources, &d, true, &buffer));
    CHECK(buffer.length == 0);
    mo_buffer_free(&buffer);
    struct mo_diags diags;
    MO_ASSERT(mo_diags_init(&diags));
    char *large = malloc(MO_CLI_DIAGNOSTIC_SIZE + 1);
    MO_ASSERT(large != NULL);
    memset(large, 'x', MO_CLI_DIAGNOSTIC_SIZE);
    large[MO_CLI_DIAGNOSTIC_SIZE] = '\0';
    d.message = large;
    MO_ASSERT(mo_diags_add(&diags, &d));
    free(large);
    FILE *stream = tmpfile();
    MO_ASSERT(stream != NULL);
    CHECK(mo_write_diagnostics(stream, stderr, true, MO_COMMAND_CHECK, &sources, &diags) == 1);
    rewind(stream);
    char line[256];
    CHECK(fgets(line, sizeof(line), stream) != NULL);
    CHECK(strcmp(line, "{\"record\":\"totals\",\"errors\":0,\"dropped\":1,\"status\":1}\n") == 0);
    CHECK(fclose(stream) == 0);
    mo_diags_free(&diags);
}

int main(int argc, char *argv[]) {
    if (argc == 2 && strcmp(argv[1], "--render-bytes") == 0)
        return render_bytes() ? 0 : 1;
    if (argc == 2 &&
        (strcmp(argv[1], "--render-json") == 0 || strcmp(argv[1], "--render-text") == 0))
        return rendering(strcmp(argv[1], "--render-json") == 0, stdout) ? 0 : 1;
    parse_commands();
    limit_values();
    long_line();
    omission();
    return mo_test_report("test_cli");
}
