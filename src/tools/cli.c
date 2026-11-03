#include "tools/tools.h"

#include <string.h>

#ifndef MO_VERSION
#define MO_VERSION "0.0.0"
#endif

static void error_text(struct mo_cli *out, size_t *at, const char *text) {
    while (*text != '\0' && *at < sizeof(out->error) - 1)
        out->error[(*at)++] = *text++;
}

/* Command errors stay on one line, even for hostile argv bytes. Quote a
 * bounded identifying prefix; file lists and forwarded arguments aren't copied. */
static void error_argument(struct mo_cli *out, size_t *at, const char *arg) {
    error_text(out, at, " \"");
    size_t i = 0;
    for (; i < 256 && arg[i] != '\0'; i++) {
        unsigned char c = (unsigned char)arg[i];
        if (c < 0x20 || c >= 0x7f || c == '"' || c == '\\') {
            const char hex[] = "0123456789abcdef";
            char escape[] = {'\\', 'x', hex[c >> 4], hex[c & 15], '\0'};
            error_text(out, at, escape);
        } else {
            char character[] = {(char)c, '\0'};
            error_text(out, at, character);
        }
    }
    if (arg[i] != '\0')
        error_text(out, at, "...");
    error_text(out, at, "\"");
}

static bool refuse(struct mo_cli *out, const char **error, const char *message,
                   const char *argument, const char *operand) {
    size_t at = 0;
    error_text(out, &at, message);
    if (argument != NULL) {
        error_text(out, &at, ":");
        error_argument(out, &at, argument);
    }
    if (operand != NULL)
        error_argument(out, &at, operand);
    out->error[at] = '\0';
    *error = out->error;
    return false;
}

static void error_uint(struct mo_cli *out, size_t *at, uint64_t value) {
    char digits[20];
    unsigned n = 0;
    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (n > 0) {
        char digit[] = {digits[--n], '\0'};
        error_text(out, at, digit);
    }
}

static bool refuse_maximum(struct mo_cli *out, const char **error, const char *prefix,
                           uint64_t maximum, const char *suffix, const char *arg) {
    size_t at = 0;
    error_text(out, &at, prefix);
    error_uint(out, &at, maximum);
    error_text(out, &at, suffix);
    error_text(out, &at, ":");
    error_argument(out, &at, arg);
    out->error[at] = '\0';
    *error = out->error;
    return false;
}

static bool option(struct mo_cli *out, const char **error, const char *arg, uint32_t *count,
                   size_t *total) {
    size_t length = 0;
    while (length <= MO_CLI_ARGUMENT_SIZE && arg[length] != '\0')
        length++;
    if (length > MO_CLI_ARGUMENT_SIZE)
        return refuse_maximum(out, error, "command option is too long (maximum ",
                              MO_CLI_ARGUMENT_SIZE, " bytes)", arg);
    if (*count == MO_CLI_ARGUMENTS)
        return refuse_maximum(out, error, "too many command options (maximum ", MO_CLI_ARGUMENTS,
                              " words)", arg);
    (*count)++;
    *total += length + 1;
    if (*total > MO_CLI_ARGUMENT_BYTES)
        return refuse_maximum(out, error, "command options are too large (maximum ",
                              MO_CLI_ARGUMENT_BYTES, " bytes)", arg);
    return true;
}

/* No strtoull: signs, whitespace, overflow and suffixes are all explicit. */
static bool number(const char *text, bool bytes, uint64_t maximum, uint64_t *out) {
    uint64_t value = 0;
    const char *p = text;
    while (*p >= '0' && *p <= '9') {
        unsigned digit = (unsigned)(*p++ - '0');
        if (value > (maximum - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    if (p == text || value == 0)
        return false;
    uint64_t scale = 1;
    if (*p != '\0') {
        if (!bytes)
            return false;
        if (strcmp(p, "B") == 0)
            scale = 1;
        else if (strcmp(p, "KiB") == 0)
            scale = UINT64_C(1) << 10;
        else if (strcmp(p, "MiB") == 0)
            scale = UINT64_C(1) << 20;
        else if (strcmp(p, "GiB") == 0)
            scale = UINT64_C(1) << 30;
        else
            return false;
    }
    if (value > maximum / scale)
        return false;
    *out = value * scale;
    return true;
}

static bool port_number(const char *text, uint16_t *out) {
    uint32_t value = 0;
    const char *p = text;
    for (; *p >= '0' && *p <= '9'; p++) {
        value = value * 10 + (uint32_t)(*p - '0');
        if (value > UINT16_MAX)
            return false;
    }
    if (p == text || *p != '\0')
        return false;
    *out = (uint16_t)value;
    return true;
}

enum {
    FLAG_JSON,
    FLAG_LIST,
    FLAG_REFERENCE,
    FLAG_ONLY,
    FLAG_IN,
    FLAG_MAX_STACK,
    FLAG_MAX_DEPTH,
    FLAG_MAX_HEAP,
    FLAG_CALLS,
    FLAG_SECONDS,
    FLAG_OUTPUT,
    FLAG_PORT,
    FLAG_DATA,
    FLAG_ALLOW_HOST,
    FLAG_EMIT
};

static const struct {
    const char *name;
    bool takes_value;
    unsigned commands;
} flag_table[] = {
    [FLAG_JSON] = {"--json", false, (1u << MO_COMMAND_CHECK) | (1u << MO_COMMAND_TEST)},
    [FLAG_LIST] = {"--list", false, (1u << MO_COMMAND_TEST)},
    [FLAG_REFERENCE] = {"--reference", false, (1u << MO_COMMAND_GUIDE)},
    [FLAG_ONLY] = {"--only", true, (1u << MO_COMMAND_TEST)},
    [FLAG_IN] = {"--in", true, (1u << MO_COMMAND_TEST)},
    [FLAG_MAX_STACK] = {"--max-stack", true, (1u << MO_COMMAND_RUN) | (1u << MO_COMMAND_TEST)},
    [FLAG_MAX_DEPTH] = {"--max-depth", true, (1u << MO_COMMAND_RUN) | (1u << MO_COMMAND_TEST)},
    [FLAG_MAX_HEAP] = {"--max-heap", true, (1u << MO_COMMAND_RUN) | (1u << MO_COMMAND_TEST)},
    [FLAG_CALLS] = {"--calls", true, (1u << MO_COMMAND_TEST)},
    [FLAG_SECONDS] = {"--seconds", true, (1u << MO_COMMAND_TEST)},
    [FLAG_OUTPUT] = {"-o", true, (1u << MO_COMMAND_EMIT)},
    [FLAG_PORT] = {"--port", true, (1u << MO_COMMAND_SERVE)},
    [FLAG_DATA] = {"--data", true, (1u << MO_COMMAND_SERVE)},
    [FLAG_ALLOW_HOST] = {"--allow-host", true, (1u << MO_COMMAND_SERVE)},
    [FLAG_EMIT] = {"--emit=", false, (1u << MO_COMMAND_CHECK)},
};

bool mo_cli_parse(int argc, const char *const argv[], struct mo_cli *out, const char **error) {
    MO_ASSERT(out != NULL && error != NULL && argc >= 0);
    *out = (struct mo_cli){
        .limits = mo_limits_default(), .calls = 200, .seconds = 2, .file_separator = UINT32_MAX};
    *error = NULL;
    if (argc < 2)
        return refuse(out, error,
                      "expected a command: check, run, test, format, fix, guide, emit or serve",
                      NULL, NULL);
    MO_ASSERT(argv != NULL && argv[1] != NULL);
    const char *command = argv[1];
    uint32_t count = 0;
    size_t total = 0;
    if (!option(out, error, command, &count, &total))
        return false;
    /* The command is length-bounded, but isn't an option word. */
    count = 0;
    total = 0;
    if (strcmp(command, "--version") == 0 || strcmp(command, "-V") == 0) {
        out->command = MO_COMMAND_VERSION;
        return argc == 2 || refuse(out, error, "--version takes no other arguments", argv[2], NULL);
    }
    const char *names[] = {"check", "run", "test", "format", "fix", "guide", "emit", "serve"};
    unsigned c = 0;
    for (; c < sizeof(names) / sizeof(names[0]); c++)
        if (strcmp(command, names[c]) == 0)
            break;
    if (c == sizeof(names) / sizeof(names[0]))
        return refuse(
            out, error,
            "unknown command (expected check, run, test, format, fix, guide, emit or serve)",
            command, NULL);
    out->command = (enum mo_command)c;
    bool flags = true;
    uint32_t seen = 0;
    for (int i = 2; i < argc; i++) {
        const char *arg = argv[i];
        MO_ASSERT(arg != NULL);
        if (flags && strcmp(arg, "--") == 0) {
            flags = false;
            if (out->files != NULL)
                out->file_separator = out->file_count;
            continue;
        }
        if (flags && arg[0] == '-') {
            if (!option(out, error, arg, &count, &total))
                return false;
            unsigned flag;
            bool value = false;
            unsigned flag_count = (unsigned)(sizeof(flag_table) / sizeof(flag_table[0]));
            for (flag = 0; flag < flag_count; flag++)
                if (strcmp(arg, flag_table[flag].name) == 0)
                    break;
            if (strncmp(arg, "--emit=", 7) == 0)
                flag = FLAG_EMIT;
            else if (flag == flag_count)
                return refuse(out, error, "unknown flag", arg, NULL);
            if ((flag_table[flag].commands & (1u << (unsigned)out->command)) == 0)
                return refuse(out, error, "flag is not accepted by this command", arg, NULL);
            uint32_t bit = UINT32_C(1) << flag;
            if (flag != FLAG_ONLY && (seen & bit) != 0)
                return refuse(out, error, "flag may be given only once (except --only)", arg, NULL);
            seen |= bit;
            value = flag_table[flag].takes_value;
            const char *operand = NULL;
            if (value) {
                if (i + 1 == argc)
                    return refuse(out, error, "flag needs a nonempty value", arg, NULL);
                operand = argv[++i];
                MO_ASSERT(operand != NULL);
                if (!option(out, error, operand, &count, &total))
                    return false;
                if (operand[0] == '\0' ||
                    (operand[0] == '-' && (flag < FLAG_MAX_STACK || flag == FLAG_OUTPUT ||
                                           flag >= FLAG_DATA || operand[1] == '-')))
                    return refuse(out, error, "flag needs a nonempty value", arg, operand);
            }
            uint64_t n = 0;
            if (flag >= FLAG_MAX_STACK && flag <= FLAG_SECONDS &&
                !number(operand, flag == FLAG_MAX_STACK || flag == FLAG_MAX_HEAP,
                        flag == FLAG_MAX_STACK || flag == FLAG_MAX_HEAP ? SIZE_MAX : UINT32_MAX,
                        &n))
                return refuse(out, error,
                              "invalid limit (expected a positive decimal integer; bytes "
                              "may use B, KiB, MiB or GiB)",
                              arg, operand);
            switch (flag) {
            case FLAG_JSON:
                out->json = true;
                break;
            case FLAG_LIST:
                out->list = true;
                break;
            case FLAG_REFERENCE:
                out->reference = true;
                break;
            case FLAG_ONLY:
                out->only[out->only_count++] = operand;
                break;
            case FLAG_IN:
                out->module = operand;
                break;
            case FLAG_MAX_STACK:
                out->limits.stack_size = (size_t)n;
                break;
            case FLAG_MAX_DEPTH:
                out->limits.call_depth = (uint32_t)n;
                break;
            case FLAG_MAX_HEAP:
                out->limits.heap_size = (size_t)n;
                break;
            case FLAG_CALLS:
                out->calls = (uint32_t)n;
                break;
            case FLAG_SECONDS:
                out->seconds = (uint32_t)n;
                break;
            case FLAG_OUTPUT:
                out->output = operand;
                break;
            case FLAG_PORT:
                if (!port_number(operand, &out->port))
                    return refuse(out, error,
                                  "invalid port (expected a decimal integer from 0 to 65535)", arg,
                                  operand);
                break;
            case FLAG_DATA:
                out->data = operand;
                break;
            case FLAG_ALLOW_HOST:
                out->allow_host = operand;
                break;
            case FLAG_EMIT:
                if (strcmp(arg + 7, "core") == 0)
                    out->emit = MO_EMIT_CORE;
                else if (strcmp(arg + 7, "bytecode") == 0)
                    out->emit = MO_EMIT_BYTECODE;
                else
                    return refuse(out, error, "--emit must be core or bytecode", arg, NULL);
                break;
            default:
                MO_UNREACHABLE();
            }
            continue;
        }
        if (arg[0] == '\0')
            return refuse(out, error, "file name must not be empty", arg, NULL);
        if (out->command == MO_COMMAND_GUIDE)
            return refuse(out, error, "guide takes no files", arg, NULL);
        if (out->file_count != 0 &&
            (out->command == MO_COMMAND_CHECK || out->command == MO_COMMAND_TEST ||
             out->command == MO_COMMAND_EMIT || out->command == MO_COMMAND_SERVE))
            return refuse(out, error, "command takes exactly one file", arg, NULL);
        if (out->files == NULL)
            out->files = argv + i;
        out->file_count++;
        if (out->command == MO_COMMAND_RUN) {
            out->arguments = argv + i + 1;
            out->argument_count = (uint32_t)(argc - i - 1);
            break;
        }
    }
    if (out->command != MO_COMMAND_GUIDE && out->file_count == 0)
        return refuse(out, error, "command needs a file", command, NULL);
    if (out->command == MO_COMMAND_EMIT && out->output == NULL)
        return refuse(out, error, "emit needs -o FILE", command, NULL);
    if (out->command == MO_COMMAND_SERVE && (seen & (UINT32_C(1) << FLAG_PORT)) == 0)
        return refuse(out, error, "serve needs --port N", command, NULL);
    if (out->command == MO_COMMAND_SERVE && (seen & (UINT32_C(1) << FLAG_DATA)) == 0)
        return refuse(out, error, "serve needs --data DIR", command, NULL);
    if (out->json && out->emit != MO_EMIT_NONE)
        return refuse(out, error, "--json and --emit cannot be combined", "--json", "--emit");
    return true;
}

const char *mo_cli_file(const struct mo_cli *cli, uint32_t index) {
    MO_ASSERT(cli != NULL && index < cli->file_count);
    return cli->files[index + (uint32_t)(index >= cli->file_separator)];
}

const char *mo_version(void) {
    return MO_VERSION;
}
const char *mo_version_line(void) {
    return "mo " MO_VERSION;
}
