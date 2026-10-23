#include "test.h"
#include "tools/tools.h"

#include "base/codes.h"
#include "base/stack.h"
#include "bytecode/bytecode.h"
#include "core/core.h"
#include "vm/vm.h"

#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct emission {
    const char *path;
    bool core, ok;
};

/* Consume real CLI output through the public codecs, not a prefix check. A
 * truncated record or lost metadata must not pass the emission regression. */
static void roundtrip_emission(void *argument) {
    struct emission *e = argument;
    struct mo_sources sources;
    struct mo_source_error error;
    struct mo_diags diags;
    struct mo_arena arena;
    struct mo_buffer printed, wire;
    MO_ASSERT(mo_sources_init(&sources) && mo_diags_init(&diags));
    mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
    mo_buffer_init(&printed, e->core ? MO_LIMIT_CORE_TEXT_SIZE : MO_LIMIT_BYTECODE_TEXT_SIZE);
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    uint32_t file;
    if (mo_sources_add(&sources, e->path, &file, &error)) {
        const struct mo_source *s = &sources.files[file];
        if (e->core) {
            struct mo_core *core = mo_core_parse(&arena, (const char *)s->bytes, s->length, &diags);
            e->ok = core != NULL && mo_core_check(core, &diags) && mo_core_print(core, &printed);
        } else {
            struct mo_assemble_error assembled;
            struct mo_load_error loaded;
            e->ok = mo_bytecode_assemble((const char *)s->bytes, s->length, &wire, &assembled) &&
                    mo_bytecode_disassemble(wire.bytes, wire.length, &printed, &loaded);
        }
        e->ok =
            e->ok && printed.length == s->length && memcmp(printed.bytes, s->bytes, s->length) == 0;
    }
    mo_buffer_free(&wire);
    mo_buffer_free(&printed);
    mo_arena_free(&arena);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
}

/* The stand-in for the loader `mo emit` leaves to a later task: loads the bytes of an emitted
 * file, which verifies them, then runs the program's entry wrapper with the real console. Its exit
 * status is the program's, or 70 when anything before main failed, so a driver can compare it
 * with `mo run` on the source. Programs whose main takes no arguments, environment or files. */
static int run_bytecode(const char *path) {
    struct mo_buffer bytes;
    mo_buffer_init(&bytes, MO_LIMIT_BYTECODE_SIZE);
    FILE *file = fopen(path, "rb");
    int status = 70;
    uint8_t chunk[4096];
    size_t got = 0;
    bool read_all = file != NULL;
    while (read_all && (got = fread(chunk, 1, sizeof chunk, file)) > 0)
        read_all = mo_buffer_append(&bytes, chunk, got);
    read_all = read_all && file != NULL && !ferror(file);
    if (file != NULL)
        (void)fclose(file);
    struct mo_load_error loaded;
    struct mo_image *image = read_all ? mo_load(bytes.bytes, bytes.length, &loaded) : NULL;
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    if (image != NULL && mo_image_init(image, &limits, &defect) == MO_RETURNED) {
        const struct mo_bytecode *code = mo_image_bytecode(image);
        uint32_t main = UINT32_MAX;
        static const char suffix[] = ".main.entry";
        for (uint32_t i = 0; i < code->function_count; i++) {
            const struct mo_bytecode_string *name = &code->strings[code->functions[i].name];
            if (name->length >= sizeof suffix - 1 &&
                memcmp(name->bytes + name->length - (sizeof suffix - 1), suffix,
                       sizeof suffix - 1) == 0)
                main = i;
        }
        struct mo_process *process = main == UINT32_MAX ? NULL : mo_process_new(image, &limits);
        if (process != NULL) {
            struct mo_host *host = mo_process_host(process);
            host->output = STDOUT_FILENO;
            host->errors = STDERR_FILENO;
            struct mo_cell result;
            if (mo_call_main(process, main, NULL, NULL, -1, NULL, &result, &defect) == MO_RETURNED)
                status = (int)mo_cell_get_int(result);
            mo_process_free(process);
        }
    }
    if (image != NULL)
        mo_image_free(image);
    mo_buffer_free(&bytes);
    return status;
}

static void parse_commands(void) {
    struct mo_cli cli;
    const char *error;
    const char *args[] = {"mo", "run", "--max-stack", "17MiB", "main.mo", "--json", ""};
    CHECK(mo_cli_parse(7, args, &cli, &error));
    CHECK(cli.limits.stack_size == 17u * 1024u * 1024u);
    CHECK(cli.argument_count == 2 && strcmp(cli.arguments[0], "--json") == 0);
    const char *wrong[] = {"mo", "check", "--json", "--max-heap", "1GiB", "main.mo"};
    CHECK(!mo_cli_parse(6, wrong, &cli, &error));
    CHECK(strcmp(error, "flag is not accepted by this command: \"--max-heap\"") == 0);
    const char *files[] = {"mo", "format", "one.mo", "--", "-two.mo", "three.mo"};
    CHECK(mo_cli_parse(6, files, &cli, &error));
    CHECK(cli.file_count == 3 && strcmp(mo_cli_file(&cli, 0), "one.mo") == 0 &&
          strcmp(mo_cli_file(&cli, 1), "-two.mo") == 0 &&
          strcmp(mo_cli_file(&cli, 2), "three.mo") == 0);
    CHECK(!mo_cli_parse(0, NULL, &cli, &error));
    const char *emit[] = {"mo", "emit", "main.mo", "-o", "main.bc"};
    CHECK(mo_cli_parse(5, emit, &cli, &error));
    CHECK(cli.command == MO_COMMAND_EMIT && cli.file_count == 1 && cli.emit == MO_EMIT_NONE &&
          strcmp(cli.output, "main.bc") == 0);
    const char *first[] = {"mo", "emit", "-o", "main.bc", "main.mo"};
    CHECK(mo_cli_parse(5, first, &cli, &error) && strcmp(cli.output, "main.bc") == 0);
    const char *bare[] = {"mo", "emit", "main.mo"};
    CHECK(!mo_cli_parse(3, bare, &cli, &error));
    CHECK(strcmp(error, "emit needs -o FILE: \"emit\"") == 0);
    const char *twice[] = {"mo", "emit", "main.mo", "-o", "a.bc", "-o", "b.bc"};
    CHECK(!mo_cli_parse(7, twice, &cli, &error));
    const char *dash[] = {"mo", "emit", "main.mo", "-o", "-a.bc"};
    CHECK(!mo_cli_parse(5, dash, &cli, &error));
    const char *wrong_command[] = {"mo", "check", "main.mo", "-o", "main.bc"};
    CHECK(!mo_cli_parse(5, wrong_command, &cli, &error));
    CHECK(strcmp(error, "flag is not accepted by this command: \"-o\"") == 0);
    const char *two[] = {"mo", "emit", "a.mo", "b.mo", "-o", "main.bc"};
    CHECK(!mo_cli_parse(6, two, &cli, &error));
    const char *json[] = {"mo", "emit", "--json", "main.mo", "-o", "main.bc"};
    CHECK(!mo_cli_parse(6, json, &cli, &error));
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
    CHECK(mo_cli_parse(4, boundary, &cli, &error));
    const char *option[] = {"mo", "test", "--only", long_arg, "main.mo"};
    CHECK(!mo_cli_parse(5, option, &cli, &error));
    long_arg[MO_CLI_ARGUMENT_SIZE] = '\0';
    CHECK(mo_cli_parse(5, option, &cli, &error));
    char tail[3970]; /* 16 * 7 flag bytes + 15 * 4097 + 3969 == 65536. */
    memset(tail, 'x', sizeof(tail));
    tail[3968] = '\0';
    const char *aggregate[35] = {"mo", "test"};
    for (unsigned i = 2; i < 34; i += 2) {
        aggregate[i] = "--only";
        aggregate[i + 1] = i == 32 ? tail : long_arg;
    }
    aggregate[34] = "main.mo";
    CHECK(mo_cli_parse(35, aggregate, &cli, &error));
    tail[3968] = 'x';
    tail[3969] = '\0';
    CHECK(!mo_cli_parse(35, aggregate, &cli, &error));
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
    const char rows[] = "X\nX\nX\nX\nX\nX\nX\nX\nX\nX\nX\nX\nX\nX\nX\n";
    MO_ASSERT(mo_sources_add_bytes(&sources, "rows.mo", (const uint8_t *)rows, sizeof(rows) - 1,
                                   &file, &error));
    d.primary.span = (struct mo_span){file, 14, 29}; /* Lines 8 through 15. */
    CHECK(mo_render_diagnostic(&sources, &d, false, &out));
    MO_ASSERT(mo_buffer_append_byte(&out, 0));
    CHECK(strstr((const char *)out.bytes,
                 "  9 | X\n    | ^\n     | ...\n  14 | X\n     | ^\n  15 | X\n     | ^ here\n") !=
          NULL);
    mo_buffer_free(&out);
    mo_sources_free(&sources);
}

static void utf8_display(void) {
    /* Tabs, CJK, combining marks, paired regional indicators, skin tones and
     * keycaps all have asymmetric byte/character/display-column counts. */
    const char source[] = "a\té界é🇫🇷👍🏽1️⃣ Z\n";
    struct mo_sources sources;
    MO_ASSERT(mo_sources_init(&sources));
    uint32_t file;
    struct mo_source_error error;
    MO_ASSERT(mo_sources_add_bytes(&sources, "café/main.mo", (const uint8_t *)source,
                                   sizeof(source) - 1, &file, &error));
    struct mo_label secondary[] = {{{file, 5, 6}, "inside UTF-8"},
                                   {{file, 10, 18}, "flag"},
                                   {{file, 18, 26}, "skin tone"},
                                   {{file, 26, 33}, "keycap"}};
    struct mo_diag d = {.code = MO_CODE_NAME_UNKNOWN,
                        .message = "UTF-8 café",
                        .primary = {{file, 34, 35}, "日本語"},
                        .secondary = secondary,
                        .secondary_count = 4};
    struct mo_buffer out;
    mo_buffer_init(&out, MO_CLI_DIAGNOSTIC_SIZE);
    CHECK(mo_render_diagnostic(&sources, &d, false, &out));
    MO_ASSERT(mo_buffer_append_byte(&out, 0));
    const char *rendered = (const char *)out.bytes;
    CHECK(strstr(rendered, "UTF-8 café  café/main.mo:1:15") != NULL);
    CHECK(strstr(rendered, "  1 | a   é界é🇫🇷👍🏽1️⃣ Z\n") != NULL);
    CHECK(strstr(rendered, "    |                ^ 日本語\n") != NULL);
    CHECK(strstr(rendered, "    |      ^^ inside UTF-8\n") != NULL);
    CHECK(strstr(rendered, "    |         ^^ flag\n") != NULL);
    CHECK(strstr(rendered, "    |           ^^ skin tone\n") != NULL);
    CHECK(strstr(rendered, "    |             ^^ keycap\n") != NULL);

    /* Byte spans may start/end inside a cluster, not just inside a scalar. */
    secondary[0] = (struct mo_label){{file, 8, 10}, "accent"};
    secondary[1].span = (struct mo_span){file, 14, 18};
    secondary[2].span = (struct mo_span){file, 22, 26};
    secondary[3].span = (struct mo_span){file, 27, 33};
    CHECK(mo_render_diagnostic(&sources, &d, false, &out));
    MO_ASSERT(mo_buffer_append_byte(&out, 0));
    CHECK(strstr((const char *)out.bytes, "    |        ^ accent\n") != NULL);
    CHECK(strstr((const char *)out.bytes, "    |         ^^ flag\n") != NULL);
    CHECK(strstr((const char *)out.bytes, "    |           ^^ skin tone\n") != NULL);
    CHECK(strstr((const char *)out.bytes, "    |             ^^ keycap\n") != NULL);

    const char letters[] = "🇺 🇺🇸 Z\n";
    MO_ASSERT(mo_sources_add_bytes(&sources, "letters.mo", (const uint8_t *)letters,
                                   sizeof letters - 1, &file, &error));
    d.primary = (struct mo_label){{file, 14, 15}, "tail"};
    secondary[0] = (struct mo_label){{file, 0, 4}, "lone letter"};
    secondary[1] = (struct mo_label){{file, 5, 13}, "pair"};
    d.secondary_count = 2;
    CHECK(mo_render_diagnostic(&sources, &d, false, &out));
    MO_ASSERT(mo_buffer_append_byte(&out, 0));
    CHECK(strstr((const char *)out.bytes, "    |      ^ tail\n") != NULL);
    CHECK(strstr((const char *)out.bytes, "    | ^ lone letter\n") != NULL);
    CHECK(strstr((const char *)out.bytes, "    |   ^^ pair\n") != NULL);

    /* At a one-cell remainder, a heart's text base fits but its emoji
     * selector does not. Clip the entire grapheme, not just the selector. */
    char clipped[156];
    memset(clipped, 'A', 147);
    memcpy(clipped + 147, "♥️X\n", 9);
    MO_ASSERT(
        mo_sources_add_bytes(&sources, "clipped.mo", (const uint8_t *)clipped, 155, &file, &error));
    d.primary = (struct mo_label){{file, 0, 1}, NULL};
    d.secondary_count = 0;
    CHECK(mo_render_diagnostic(&sources, &d, false, &out));
    MO_ASSERT(mo_buffer_append_byte(&out, 0));
    CHECK(strstr((const char *)out.bytes, "♥") == NULL);
    CHECK(strstr((const char *)out.bytes, "A...\n    | ^\n") != NULL);

    struct mo_buffer row;
    mo_buffer_init(&row, MO_LIMIT_FILE_SIZE);
    const char *clusters[] = {"🇫🇷", "é"};
    for (unsigned i = 0; i < 2; i++) {
        mo_buffer_clear(&row);
        for (unsigned j = 0; j < 120; j++)
            MO_ASSERT(mo_buffer_append_text(&row, clusters[i]));
        uint32_t focus = (uint32_t)row.length;
        MO_ASSERT(mo_buffer_append_text(&row, "Z\n"));
        MO_ASSERT(mo_sources_add_bytes(&sources, "left.mo", row.bytes, row.length, &file, &error));
        d.primary.span = (struct mo_span){file, focus, focus + 1};
        CHECK(mo_render_diagnostic(&sources, &d, false, &out));
        MO_ASSERT(mo_buffer_append_byte(&out, 0));
        CHECK(strstr((const char *)out.bytes, i == 0 ? "  1 | ...🇫🇷" : "  1 | ...é") != NULL);
        CHECK(strstr((const char *)out.bytes, i == 0 ? "...🇷" : "...́") == NULL);
        CHECK(strstr((const char *)out.bytes, i == 0 ? "    |                            ^\n"
                                                     : "    |                ^\n") != NULL);
    }
    /* Reserve three bytes for elision: a 4093-byte cluster fits, but a
     * 4095-byte cluster does not. Both still occupy just one display cell. */
    for (unsigned count = 2046; count <= 2047; count++) {
        mo_buffer_clear(&row);
        MO_ASSERT(mo_buffer_append_byte(&row, 'a'));
        for (unsigned i = 0; i < count; i++)
            MO_ASSERT(mo_buffer_append_text(&row, "́"));
        MO_ASSERT(mo_buffer_append_byte(&row, 'Z'));
        MO_ASSERT(mo_sources_add_bytes(&sources, "cap.mo", row.bytes, row.length, &file, &error));
        d.primary.span = (struct mo_span){file, 0, 1};
        CHECK(mo_render_diagnostic(&sources, &d, false, &out));
        MO_ASSERT(mo_buffer_append_byte(&out, 0));
        CHECK(strstr((const char *)out.bytes, count == 2046 ? "  1 | á" : "  1 | ...\n") != NULL);
        CHECK(strstr((const char *)out.bytes, "Z\n") == NULL);
    }
    /* A single oversized zero-width extension must elide the whole cluster,
     * not defeat the byte cap or leave a dangling combining mark. */
    mo_buffer_clear(&row);
    MO_ASSERT(mo_buffer_append_byte(&row, 'a'));
    for (unsigned i = 0; i < 30000; i++)
        MO_ASSERT(mo_buffer_append_text(&row, "́"));
    MO_ASSERT(mo_sources_add_bytes(&sources, "budget.mo", row.bytes, row.length, &file, &error));
    d.primary.span = (struct mo_span){file, 1, (uint32_t)row.length};
    CHECK(mo_render_diagnostic(&sources, &d, false, &out));
    MO_ASSERT(mo_buffer_append_byte(&out, 0));
    CHECK(strstr((const char *)out.bytes, "  1 | ...\n    | ^\n") != NULL);
    CHECK(out.length < 256);
    /* Context must not let that giant cluster hide a later, narrow span. */
    MO_ASSERT(mo_buffer_append_byte(&row, 'Z'));
    MO_ASSERT(mo_sources_add_bytes(&sources, "after.mo", row.bytes, row.length, &file, &error));
    d.primary.span = (struct mo_span){file, (uint32_t)row.length - 1, (uint32_t)row.length};
    CHECK(mo_render_diagnostic(&sources, &d, false, &out));
    MO_ASSERT(mo_buffer_append_byte(&out, 0));
    CHECK(strstr((const char *)out.bytes, "  1 | ...Z\n    |    ^\n") != NULL);
    mo_buffer_free(&row);
    mo_buffer_free(&out);
    mo_sources_free(&sources);
}

static void ascii_locale(void) {
    MO_ASSERT(setlocale(LC_CTYPE, "C") != NULL);
    const uint8_t source[] = "a\téÛ\n";
    struct mo_sources sources;
    MO_ASSERT(mo_sources_init(&sources));
    uint32_t file;
    struct mo_source_error error;
    MO_ASSERT(mo_sources_add_bytes(&sources, "café.mo", source, sizeof(source) - 1, &file, &error));
    struct mo_edit edit = {{file, 2, 4}, "界"};
    struct mo_fix fix = {"日本語", &edit, 1};
    struct mo_diag d = {.code = MO_CODE_TOOL_UNAVAILABLE,
                        .message = "José",
                        .primary = {{file, 4, 6}, "Û"},
                        .fixes = &fix,
                        .fix_count = 1};
    struct mo_buffer out;
    mo_buffer_init(&out, MO_CLI_DIAGNOSTIC_SIZE);
    CHECK(mo_render_diagnostic(&sources, &d, false, &out));
    MO_ASSERT(mo_buffer_append_byte(&out, 0));
    CHECK(strstr((const char *)out.bytes, "Jos\\u{0e9}  caf\\u{0e9}.mo:1:4") != NULL);
    CHECK(strstr((const char *)out.bytes, "  1 | a   \\u{0e9}\\u{0db}\n") != NULL);
    CHECK(strstr((const char *)out.bytes, "    |            ^^^^^^^ \\u{0db}\n") != NULL);
    CHECK(strstr((const char *)out.bytes, "Fix: \\u{65e5}\\u{672c}\\u{8a9e}") != NULL);
    CHECK(strstr((const char *)out.bytes, "  1 | a   \\u{754c}\\u{0db}\n") != NULL);
    for (size_t i = 0; i < out.length; i++)
        CHECK(out.bytes[i] < 128);
    /* JSON stays UTF-8, independent of the terminal's encoding. */
    CHECK(mo_render_diagnostic(&sources, &d, true, &out));
    MO_ASSERT(mo_buffer_append_byte(&out, 0));
    CHECK(strstr((const char *)out.bytes, "\"headline\":\"José\"") != NULL);
    mo_buffer_free(&out);
    mo_sources_free(&sources);
}

static void control_display(void) {
    /* One member of every refused range, plus controls and an invalid byte.
     * Their raw bytes must not reach the terminal even outside source rows. */
    const char controls[] = "\x01\t\r\n\x7f\xff\xc2\x85\xc2\xad\xd8\x9c\xe1\xa0\x8e"
                            "\xe2\x80\x8b\xe2\x80\xa8\xe2\x81\xa0\xe2\x81\xa6"
                            "\xef\xbb\xbf\xef\xbf\xb9\xf3\xa0\x80\x81\xf3\xa0\x80\xa0";
    struct mo_sources sources = {0};
    struct mo_diag d = {.code = MO_CODE_TOOL_UNAVAILABLE,
                        .message = controls,
                        .primary = {{UINT32_MAX, 0, 0}, NULL}};
    struct mo_buffer out;
    mo_buffer_init(&out, MO_CLI_DIAGNOSTIC_SIZE);
    CHECK(mo_render_diagnostic(&sources, &d, false, &out));
    MO_ASSERT(mo_buffer_append_byte(&out, 0));
    CHECK(strcmp((const char *)out.bytes,
                 "error E1400: \\x01    \\x0d\\x0a\\x7f\\xff\\u{085}\\u{0ad}\\u{61c}\\u{180e}"
                 "\\u{200b}\\u{2028}\\u{2060}\\u{2066}\\u{feff}\\u{fff9}\\u{e0001}\\u{e0020}"
                 "  <unavailable>\n\n\n") == 0);
    CHECK(mo_render_diagnostic(&sources, &d, true, &out));
    MO_ASSERT(mo_buffer_append_byte(&out, 0));
    CHECK(strstr((const char *)out.bytes,
                 "\\u0001\\u0009\\u000d\\u000a\\u007f\\u00ff\\u0085\\u00ad\\u061c"
                 "\\u180e\\u200b\\u2028\\u2060\\u2066\\ufeff\\ufff9"
                 "\\udb40\\udc01\\udb40\\udc20") != NULL);
    mo_buffer_free(&out);
}

static void split_crlf_fix(void) {
    struct mo_sources sources;
    MO_ASSERT(mo_sources_init(&sources));
    uint32_t file;
    struct mo_source_error error;
    MO_ASSERT(
        mo_sources_add_bytes(&sources, "split.mo", (const uint8_t *)"Z\r\n\r\n", 5, &file, &error));
    struct mo_edit edit = {{file, 0, 2}, "A"};
    struct mo_fix fix = {"replace", &edit, 1};
    struct mo_diag d = {.code = MO_CODE_TOOL_UNAVAILABLE,
                        .message = "split CRLF",
                        .primary = {{file, 0, 1}, NULL},
                        .fixes = &fix,
                        .fix_count = 1};
    struct mo_buffer out;
    mo_buffer_init(&out, MO_CLI_DIAGNOSTIC_SIZE);
    CHECK(mo_render_diagnostic(&sources, &d, false, &out));
    MO_ASSERT(mo_buffer_append_byte(&out, 0));
    CHECK(strcmp((const char *)out.bytes,
                 "error E1400: split CRLF  split.mo:1:1\n\n"
                 "  1 | Z\n    | ^\n\nFix: replace\n  split.mo:1:1\n  1 | A\n\n") == 0);
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
    CHECK(mo_write_diagnostics(stream, stderr, true, MO_COMMAND_CHECK, &sources, &diags, NULL) ==
          1);
    rewind(stream);
    char line[256] = {0};
    CHECK(fgets(line, sizeof(line), stream) != NULL);
    CHECK(strcmp(line, "{\"record\":\"totals\",\"errors\":0,\"dropped\":1,\"status\":1}\n") == 0);
    CHECK(fclose(stream) == 0);
    mo_diags_free(&diags);
}

static void sorted_output(void) {
    struct mo_sources sources = {0};
    struct mo_diags diags;
    MO_ASSERT(mo_diags_init(&diags));
    struct mo_diag d = {
        .code = MO_CODE_TOOL_UNAVAILABLE, .primary = {{UINT32_MAX, 2, 3}, NULL}, .message = "late"};
    MO_ASSERT(mo_diags_add(&diags, &d));
    d.primary.span.start = 0;
    d.primary.span.end = 1;
    d.message = "first at tie";
    MO_ASSERT(mo_diags_add(&diags, &d));
    d.message = "second at tie";
    MO_ASSERT(mo_diags_add(&diags, &d));
    FILE *stream = tmpfile();
    MO_ASSERT(stream != NULL);
    CHECK(mo_write_diagnostics(stream, stderr, true, MO_COMMAND_CHECK, &sources, &diags, NULL) ==
          1);
    rewind(stream);
    const char *order[] = {"\"headline\":\"first at tie\"", "\"headline\":\"second at tie\"",
                           "\"headline\":\"late\""};
    for (unsigned i = 0; i < 3; i++) {
        char line[512] = {0};
        CHECK(fgets(line, sizeof(line), stream) != NULL);
        CHECK(strstr(line, order[i]) != NULL);
    }
    CHECK(fclose(stream) == 0);
    mo_diags_free(&diags);
}

int main(int argc, char *argv[]) {
    /* Explicit UTF-8 test locale, including on macOS; production selects the
     * user's locale instead. The C-locale regression runs last. */
    const char *locales[] = {"C.UTF-8", "en_US.UTF-8", "UTF-8"};
    bool utf8 = false;
    for (unsigned i = 0; i < sizeof(locales) / sizeof(locales[0]); i++) {
        if (setlocale(LC_CTYPE, locales[i]) != NULL) {
            utf8 = true;
            break;
        }
    }
    MO_ASSERT(utf8);
    if (argc == 3 && (strcmp(argv[1], "--roundtrip-core") == 0 ||
                      strcmp(argv[1], "--roundtrip-bytecode") == 0)) {
        struct emission e = {.path = argv[2], .core = strcmp(argv[1], "--roundtrip-core") == 0};
        return mo_with_stack(MO_SOURCE_STACK_SIZE, roundtrip_emission, &e) == 0 && e.ok ? 0 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "--run-bytecode") == 0)
        return run_bytecode(argv[2]);
    if (argc == 2 && strcmp(argv[1], "--render-bytes") == 0)
        return render_bytes() ? 0 : 1;
    if (argc == 2 &&
        (strcmp(argv[1], "--render-json") == 0 || strcmp(argv[1], "--render-text") == 0))
        return rendering(strcmp(argv[1], "--render-json") == 0, stdout) ? 0 : 1;
    parse_commands();
    limit_values();
    long_line();
    utf8_display();
    control_display();
    split_crlf_fix();
    omission();
    sorted_output();
    ascii_locale();
    return mo_test_report("test_cli");
}
