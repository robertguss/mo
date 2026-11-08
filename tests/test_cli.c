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

static void refused(int argc, const char *const *argv, const char *expected) {
    struct mo_cli cli;
    const char *error;
    CHECK(!mo_cli_parse(argc, argv, &cli, &error));
    CHECK(error != NULL && strcmp(error, expected) == 0);
}

#define REFUSED(expected, ...)                                                                     \
    do {                                                                                           \
        const char *argv[] = {"mo", "serve", __VA_ARGS__};                                         \
        refused((int)(sizeof argv / sizeof *argv), argv, expected);                                \
    } while (0)

static void parse_fetch(void) {
    const char *args[] = {"mo",         "fetch",       "http://localhost:8080/mcp",
                          "--export",   "state",       "--out",
                          "state.moex", "--token-env", "TOKEN"};
    struct mo_cli cli;
    const char *error;
    CHECK(mo_cli_parse(9, args, &cli, &error));
    CHECK(cli.command == MO_COMMAND_FETCH && strcmp(cli.output, "state.moex") == 0);
    CHECK(strcmp(cli.fetch_url, args[2]) == 0 && strcmp(cli.fetch_token, "TOKEN") == 0);
    CHECK(!mo_cli_parse(8, args, &cli, &error));
    CHECK(strcmp(error,
                 "usage: mo fetch URL --export state|failure ID --out FILE --token-env NAME") == 0);
    CHECK(mo_cli_parse(9, args, &cli, &error) && cli.fetch_failure == 0);
    const char *failure[] = {"mo",           "fetch",        "http://localhost:8080/mcp",
                             "--export",     "failure",      "42",
                             "--out",        "failure.moex", "--token-env",
                             "FAILURE_TOKEN"};
    CHECK(mo_cli_parse(10, failure, &cli, &error));
    CHECK(cli.command == MO_COMMAND_FETCH && cli.fetch_failure == 42);
    CHECK(strcmp(cli.fetch_url, failure[2]) == 0 && strcmp(cli.output, "failure.moex") == 0);
    CHECK(strcmp(cli.fetch_token, "FAILURE_TOKEN") == 0);
    failure[5] = "9223372036854775807";
    CHECK(mo_cli_parse(10, failure, &cli, &error) && cli.fetch_failure == INT64_MAX);
    const char *refused_ids[] = {
        "", "0", "000", "4x", "-1", "+1", "9223372036854775808", "18446744073709551616"};
    for (size_t i = 0; i < sizeof refused_ids / sizeof *refused_ids; i++) {
        failure[5] = refused_ids[i];
        CHECK(!mo_cli_parse(10, failure, &cli, &error));
        CHECK(strcmp(error, "failure id must be positive") == 0);
    }
    failure[5] = "42";
    CHECK(!mo_cli_parse(9, failure, &cli, &error));
    CHECK(strcmp(error,
                 "usage: mo fetch URL --export state|failure ID --out FILE --token-env NAME") == 0);
    failure[4] = "state";
    CHECK(!mo_cli_parse(10, failure, &cli, &error));
    CHECK(strcmp(error,
                 "usage: mo fetch URL --export state|failure ID --out FILE --token-env NAME") == 0);
}

static void parse_serve(void) {
    struct mo_cli cli;
    const char *error;
    const char *happy[] = {"mo", "serve", "main.mo", "--port", "8080", "--data", "state"};
    CHECK(mo_cli_parse(7, happy, &cli, &error));
    CHECK(cli.command == MO_COMMAND_SERVE && cli.file_count == 1 && cli.port == 8080);
    CHECK(strcmp(mo_cli_file(&cli, 0), "main.mo") == 0 && strcmp(cli.data, "state") == 0);
    CHECK(cli.allow_host == NULL);
    const char *folder[] = {"mo", "serve", "--data", "state", "--port", "0"};
    CHECK(mo_cli_parse(6, folder, &cli, &error) && cli.file_count == 0);
    const char *upgrade[] = {"mo",     "serve", "new.mo",    "--data",    "state",
                             "--port", "0",     "--upgrade", "--migrate", "Main.migrate"};
    CHECK(mo_cli_parse(10, upgrade, &cli, &error) && cli.upgrade &&
          strcmp(cli.migration, "Main.migrate") == 0);
    REFUSED("upgrade or adoption needs a source file: \"serve\"", "--data", "state", "--port", "0",
            "--upgrade");
    REFUSED("--migrate needs --upgrade or an old program: \"--migrate\"", "new.mo", "--data",
            "state", "--port", "0", "--migrate", "Main.migrate");

    const char *proxied[] = {"mo",     "serve", "main.mo", "--allow-host", "q.example:8000",
                             "--port", "1",     "--data",  "state"};
    CHECK(mo_cli_parse(9, proxied, &cli, &error) && strcmp(cli.allow_host, "q.example:8000") == 0);
    const char *first[] = {"mo", "serve", "--data", "state", "--port", "0", "main.mo"};
    CHECK(mo_cli_parse(7, first, &cli, &error) && cli.port == 0);
    const char *highest[] = {"mo", "serve", "main.mo", "--port", "65535", "--data", "state"};
    CHECK(mo_cli_parse(7, highest, &cli, &error) && cli.port == 65535);
    const char *padded[] = {"mo", "serve", "main.mo", "--port", "00080", "--data", "state"};
    CHECK(mo_cli_parse(7, padded, &cli, &error) && cli.port == 80);
    REFUSED("invalid port (expected a decimal integer from 0 to 65535): \"--port\" \"65536\"",
            "main.mo", "--port", "65536", "--data", "state");
    REFUSED("invalid port (expected a decimal integer from 0 to 65535): \"--port\" \"-1\"",
            "main.mo", "--port", "-1", "--data", "state");
    REFUSED("invalid port (expected a decimal integer from 0 to 65535): \"--port\" \"1x\"",
            "main.mo", "--port", "1x", "--data", "state");
    REFUSED("flag needs a nonempty value: \"--port\" \"\"", "main.mo", "--port", "", "--data",
            "state");
    REFUSED("flag needs a nonempty value: \"--port\"", "main.mo", "--data", "state", "--port");
    REFUSED("flag needs a nonempty value: \"--port\" \"--data\"", "main.mo", "--port", "--data");
    REFUSED("flag needs a nonempty value: \"--data\" \"-x\"", "main.mo", "--port", "1", "--data",
            "-x");
    REFUSED("flag needs a nonempty value: \"--data\"", "main.mo", "--port", "1", "--data");
    REFUSED("serve needs --port N: \"serve\"", "main.mo", "--data", "state");
    REFUSED("serve needs --data DIR: \"serve\"", "main.mo", "--port", "1");
    REFUSED("flag may be given only once (except --only): \"--port\"", "main.mo", "--port", "1",
            "--port", "2", "--data", "state");
    REFUSED("flag may be given only once (except --only): \"--data\"", "main.mo", "--port", "1",
            "--data", "a", "--data", "b");
    REFUSED("command takes exactly one file: \"b.mo\"", "a.mo", "b.mo", "--port", "1", "--data",
            "state");
    REFUSED("flag is not accepted by this command: \"--json\"", "--json", "main.mo", "--port", "1",
            "--data", "state");
    REFUSED("flag is not accepted by this command: \"--max-heap\"", "--max-heap", "1MiB", "main.mo",
            "--port", "1", "--data", "state");
    REFUSED("flag may be given only once (except --only): \"--allow-host\"", "main.mo", "--port",
            "1", "--data", "state", "--allow-host", "a", "--allow-host", "b");
    REFUSED("flag needs a nonempty value: \"--allow-host\" \"-a\"", "main.mo", "--port", "1",
            "--data", "state", "--allow-host", "-a");
    REFUSED("flag needs a nonempty value: \"--allow-host\"", "main.mo", "--port", "1", "--data",
            "state", "--allow-host");
    const char *wrong_host[] = {"mo", "run", "--allow-host", "a", "main.mo"};
    refused(5, wrong_host, "flag is not accepted by this command: \"--allow-host\"");
    const char *wrong_port[] = {"mo", "run", "--port", "1", "main.mo"};
    refused(5, wrong_port, "flag is not accepted by this command: \"--port\"");
    const char *wrong_data[] = {"mo", "emit", "--data", "state", "main.mo", "-o", "x"};
    refused(7, wrong_data, "flag is not accepted by this command: \"--data\"");
    const char *emit_only[] = {"mo", "serve", "--emit=core", "main.mo"};
    refused(4, emit_only, "flag is not accepted by this command: \"--emit=core\"");
    const char *unknown[] = {"mo", "serve", "--emit", "main.mo"};
    refused(4, unknown, "unknown flag: \"--emit\"");
    const char *none[] = {"mo"};
    refused(1, none,
            "expected a command: check, run, test, format, fix, guide, emit, serve, fetch, replay, "
            "query or history");
    const char *what[] = {"mo", "what"};
    refused(2, what,
            "unknown command (expected check, run, test, format, fix, guide, emit, serve, fetch, "
            "replay, "
            "query or history): \"what\"");
}

#define OFFLINE(expected, ...)                                                                     \
    do {                                                                                           \
        const char *argv[] = {"mo", __VA_ARGS__};                                                  \
        refused((int)(sizeof argv / sizeof *argv), argv, expected);                                \
    } while (0)

static void parse_offline(void) {
    struct mo_cli cli;
    const char *error;
    const char *replay[] = {"mo", "replay", "main.mo", "--failure", "7", "--data", "state"};
    CHECK(mo_cli_parse(7, replay, &cli, &error));
    CHECK(cli.command == MO_COMMAND_REPLAY && cli.failure == 7 && strcmp(cli.data, "state") == 0);
    CHECK(cli.artifact == NULL && cli.revised == NULL && !cli.recover && !cli.json);
    CHECK(cli.file_count == 1 && strcmp(mo_cli_file(&cli, 0), "main.mo") == 0);
    const char *fix[] = {"mo",           "replay",   "--json", "main.mo",   "--fix",
                         "next/main.mo", "--export", "f.moex", "--failure", "9223372036854775807"};
    CHECK(mo_cli_parse(10, fix, &cli, &error));
    CHECK(cli.json && strcmp(cli.revised, "next/main.mo") == 0 && cli.failure == INT64_MAX);
    CHECK(strcmp(cli.artifact, "f.moex") == 0 && cli.data == NULL);
    const char *recover[] = {"mo", "replay", "main.mo", "--failure",
                             "1",  "--data", "d",       "--recover"};
    CHECK(mo_cli_parse(8, recover, &cli, &error) && cli.recover);
    const char *state[] = {"mo", "query", "main.mo", "Main.count", "--export", "s.moex"};
    CHECK(mo_cli_parse(6, state, &cli, &error));
    CHECK(cli.command == MO_COMMAND_QUERY && strcmp(cli.function, "Main.count") == 0);
    CHECK(cli.failure == 0 && strcmp(cli.artifact, "s.moex") == 0);
    CHECK(cli.max_work == MO_LIMIT_QUERY_WORK && cli.limits.heap_size == MO_DEFAULT_HEAP_SIZE);
    const char *limited[] = {"mo",          "query", "--max-work",  "18446744073709551615",
                             "--max-heap",  "2MiB",  "--max-stack", "64KiB",
                             "--max-depth", "9",     "main.mo",     "count",
                             "--data",      "d",     "--failure",   "3",
                             "--json"};
    CHECK(mo_cli_parse(17, limited, &cli, &error));
    CHECK(cli.max_work == UINT64_MAX && cli.limits.heap_size == 2u << 20);
    CHECK(cli.limits.stack_size == 64u << 10 && cli.limits.call_depth == 9);
    CHECK(cli.failure == 3 && strcmp(cli.data, "d") == 0 && cli.json);
    CHECK(strcmp(mo_cli_file(&cli, 0), "main.mo") == 0 && strcmp(cli.function, "count") == 0);
    const char *history[] = {"mo", "history", "state", "--recover", "--json"};
    CHECK(mo_cli_parse(5, history, &cli, &error));
    CHECK(cli.command == MO_COMMAND_HISTORY && cli.recover && cli.json);
    CHECK(cli.file_count == 1 && strcmp(mo_cli_file(&cli, 0), "state") == 0);

    OFFLINE("replay needs --failure ID: \"replay\"", "replay", "main.mo", "--data", "d");
    OFFLINE("give exactly one of --data DIR and --export ARTIFACT: \"replay\"", "replay", "main.mo",
            "--failure", "1");
    OFFLINE("give exactly one of --data DIR and --export ARTIFACT: \"replay\"", "replay", "main.mo",
            "--failure", "1", "--data", "d", "--export", "a");
    OFFLINE("give exactly one of --data DIR and --export ARTIFACT: \"query\"", "query", "main.mo",
            "f");
    OFFLINE("give exactly one of --data DIR and --export ARTIFACT: \"query\"", "query", "main.mo",
            "f", "--data", "d", "--export", "a", "--failure", "1");
    OFFLINE("a query over a data folder needs --failure ID: \"query\"", "query", "main.mo", "f",
            "--data", "d");
    OFFLINE("--recover reads a data folder, not --export: \"--recover\"", "replay", "main.mo",
            "--failure", "1", "--export", "a", "--recover");
    OFFLINE("--recover reads a data folder, not --export: \"--recover\"", "query", "main.mo", "f",
            "--export", "a", "--recover");
    OFFLINE("flag is not accepted by this command: \"--fix\"", "query", "main.mo", "f", "--fix",
            "r.mo", "--export", "a");
    OFFLINE("query needs a file and a function: \"query\"", "query", "main.mo", "--export", "a");
    OFFLINE("query takes exactly one file and one function: \"g\"", "query", "main.mo", "f", "g",
            "--export", "a");
    OFFLINE("command takes exactly one file: \"b.mo\"", "replay", "a.mo", "b.mo", "--failure", "1",
            "--data", "d");
    OFFLINE("command needs a file: \"replay\"", "replay", "--failure", "1", "--data", "d");
    OFFLINE("history needs a data folder: \"history\"", "history", "--json");
    OFFLINE("history takes exactly one data folder: \"b\"", "history", "a", "b");
    OFFLINE("flag may be given only once (except --only): \"--failure\"", "replay", "main.mo",
            "--failure", "1", "--failure", "2", "--data", "d");
    OFFLINE("flag may be given only once (except --only): \"--export\"", "query", "main.mo", "f",
            "--export", "a", "--export", "b");
    OFFLINE("flag may be given only once (except --only): \"--max-work\"", "query", "main.mo", "f",
            "--export", "a", "--max-work", "1", "--max-work", "2");
    OFFLINE("flag may be given only once (except --only): \"--recover\"", "history", "d",
            "--recover", "--recover");
    OFFLINE("flag needs a nonempty value: \"--fix\" \"-r.mo\"", "replay", "main.mo", "--failure",
            "1", "--data", "d", "--fix", "-r.mo");
    OFFLINE("flag needs a nonempty value: \"--export\"", "query", "main.mo", "f", "--export");
    const char *ids[] = {"0", "000", "4x", "-1", "+1", "9223372036854775808"};
    for (size_t i = 0; i < sizeof ids / sizeof *ids; i++) {
        const char *argv[] = {"mo", "replay", "main.mo", "--data", "d", "--failure", ids[i]};
        struct mo_buffer expected;
        mo_buffer_init(&expected, 256);
        CHECK(mo_buffer_append_text(&expected, "failure id must be positive: \"--failure\" \"") &&
              mo_buffer_append_text(&expected, ids[i]) && mo_buffer_append_text(&expected, "\"") &&
              mo_buffer_append_byte(&expected, 0));
        refused(7, argv, (const char *)expected.bytes);
        mo_buffer_free(&expected);
    }
    OFFLINE("invalid limit (expected a positive decimal integer; bytes may use B, KiB, MiB or "
            "GiB): \"--max-work\" \"1KiB\"",
            "query", "main.mo", "f", "--export", "a", "--max-work", "1KiB");
    OFFLINE("invalid limit (expected a positive decimal integer; bytes may use B, KiB, MiB or "
            "GiB): \"--max-work\" \"0\"",
            "query", "main.mo", "f", "--export", "a", "--max-work", "0");
    /* Each new flag on a command that does not take it. */
    const char *elsewhere[][3] = {{"run", "--failure", "1"},       {"history", "--failure", "1"},
                                  {"serve", "--export", "a"},      {"history", "--export", "a"},
                                  {"test", "--fix", "a"},          {"history", "--fix", "a"},
                                  {"check", "--recover", NULL},    {"serve", "--recover", NULL},
                                  {"run", "--max-work", "1"},      {"replay", "--max-work", "1"},
                                  {"history", "--max-work", "1"},  {"replay", "--max-heap", "1"},
                                  {"history", "--max-stack", "1"}, {"replay", "--max-depth", "1"},
                                  {"history", "--data", "d"},      {"run", "--json", NULL}};
    for (size_t i = 0; i < sizeof elsewhere / sizeof *elsewhere; i++) {
        const char *argv[] = {"mo", elsewhere[i][0], elsewhere[i][1], elsewhere[i][2], "x"};
        int argc = elsewhere[i][2] == NULL ? 3 : 4;
        struct mo_buffer expected;
        mo_buffer_init(&expected, 256);
        CHECK(mo_buffer_append_text(&expected, "flag is not accepted by this command: \"") &&
              mo_buffer_append_text(&expected, elsewhere[i][1]) &&
              mo_buffer_append_text(&expected, "\"") && mo_buffer_append_byte(&expected, 0));
        refused(argc, argv, (const char *)expected.bytes);
        mo_buffer_free(&expected);
    }
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
    CHECK(strncmp(error, "command option is too long (maximum 4096 bytes): \"", 49) == 0);
    long_arg[MO_CLI_ARGUMENT_SIZE] = '\0';
    CHECK(mo_cli_parse(5, option, &cli, &error));
    const char *words[2 + 2 * MO_CLI_ARGUMENTS + 2];
    words[0] = "mo";
    words[1] = "test";
    uint32_t at = 2;
    for (uint32_t i = 0; i < MO_CLI_ARGUMENTS / 2; i++) {
        words[at++] = "--only";
        words[at++] = "a";
    }
    words[at++] = "--only";
    words[at++] = "main.mo";
    CHECK(!mo_cli_parse((int)at, words, &cli, &error));
    CHECK(strncmp(error, "too many command options (maximum 256 words): \"", 47) == 0);
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
    CHECK(strncmp(error, "command options are too large (maximum 65536 bytes): \"", 53) == 0);
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

static void parse_rehearsal(void) {
    const char *args[] = {"mo",        "rehearse",     "--artifact", "state.moex", "--old-bundle",
                          "old.mosb",  "--candidate",  "new.mosb",   "--context",  "old.mocc",
                          "--migrate", "Main.migrate", "--json"};
    struct mo_cli cli;
    const char *error;
    CHECK(mo_cli_parse(13, args, &cli, &error));
    CHECK(cli.command == MO_COMMAND_REHEARSE && cli.json);
    CHECK(strcmp(cli.candidate, "new.mosb") == 0);
    CHECK(strcmp(cli.compile_context, "old.mocc") == 0);
    CHECK(!mo_cli_parse(6, args, &cli, &error));
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
    parse_rehearsal();
    parse_commands();
    parse_serve();
    parse_fetch();
    parse_offline();
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
