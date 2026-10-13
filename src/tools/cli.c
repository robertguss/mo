#include "tools/tools.h"

#include <string.h>

#ifndef MO_VERSION
#define MO_VERSION "0.0.0"
#endif

static bool refuse(const char **error, const char *message) {
    *error = message;
    return false;
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

bool mo_cli_parse(int argc, const char *const argv[], struct mo_cli *out, const char **error) {
    MO_ASSERT(out != NULL && error != NULL && argc >= 0);
    *out = (struct mo_cli){.limits = mo_limits_default(), .calls = 200, .seconds = 2};
    *error = NULL;
    if (argc > MO_CLI_ARGUMENTS)
        return refuse(error, "too many command arguments (maximum 256)");
    size_t total = 0;
    for (int i = 0; i < argc; i++) {
        MO_ASSERT(argv != NULL && argv[i] != NULL);
        size_t length = 0;
        while (length <= MO_CLI_ARGUMENT_SIZE && argv[i][length] != '\0')
            length++;
        if (length > MO_CLI_ARGUMENT_SIZE)
            return refuse(error, "command argument is too long (maximum 4096 bytes)");
        total += length + 1;
        if (total > MO_CLI_ARGUMENT_BYTES)
            return refuse(error, "command arguments are too large (maximum 65536 bytes)");
    }
    if (argc < 2)
        return refuse(error, "expected a command: check, run, test, format, fix or guide");
    const char *command = argv[1];
    if (strcmp(command, "--version") == 0 || strcmp(command, "-V") == 0) {
        out->command = MO_COMMAND_VERSION;
        return argc == 2 || refuse(error, "--version takes no other arguments");
    }
    const char *names[] = {"check", "run", "test", "format", "fix", "guide"};
    unsigned c = 0;
    for (; c < sizeof(names) / sizeof(names[0]); c++)
        if (strcmp(command, names[c]) == 0)
            break;
    if (c == sizeof(names) / sizeof(names[0]))
        return refuse(error, "unknown command (expected check, run, test, format, fix or guide)");
    out->command = (enum mo_command)c;
    bool flags = true;
    uint32_t seen = 0;
    for (int i = 2; i < argc; i++) {
        const char *arg = argv[i];
        if (flags && strcmp(arg, "--") == 0) {
            flags = false;
            continue;
        }
        if (flags && arg[0] == '-') {
            unsigned flag;
            bool value = false;
            const char *flag_names[] = {"--json",  "--list",      "--reference", "--only",
                                        "--in",    "--max-stack", "--max-depth", "--max-heap",
                                        "--calls", "--seconds"};
            for (flag = 0; flag < 10; flag++)
                if (strcmp(arg, flag_names[flag]) == 0)
                    break;
            if (strncmp(arg, "--emit=", 7) == 0)
                flag = 10;
            if (flag == 10 && strncmp(arg, "--emit=", 7) != 0)
                return refuse(error, "unknown flag");
            bool check = out->command == MO_COMMAND_CHECK;
            bool test = out->command == MO_COMMAND_TEST;
            bool run = out->command == MO_COMMAND_RUN;
            bool allowed = (flag == 0 && (check || test)) || (flag == 1 && test) ||
                           (flag == 2 && out->command == MO_COMMAND_GUIDE) ||
                           ((flag == 3 || flag == 4 || flag == 8 || flag == 9) && test) ||
                           (flag >= 5 && flag <= 7 && (run || test)) || (flag == 10 && check);
            if (!allowed)
                return refuse(error, "flag is not accepted by this command");
            uint32_t bit = UINT32_C(1) << flag;
            if (flag != 3 && (seen & bit) != 0)
                return refuse(error, "flag may be given only once (except --only)");
            seen |= bit;
            value = flag >= 3 && flag <= 9;
            const char *operand = NULL;
            if (value) {
                if (i + 1 == argc || argv[i + 1][0] == '-' || argv[i + 1][0] == '\0')
                    return refuse(error, "flag needs a nonempty value");
                operand = argv[++i];
            }
            uint64_t n = 0;
            if (flag >= 5 && flag <= 9 &&
                !number(operand, flag == 5 || flag == 7,
                        flag == 5 || flag == 7 ? SIZE_MAX : UINT32_MAX, &n))
                return refuse(error, "invalid limit (expected a positive decimal integer; bytes "
                                     "may use B, KiB, MiB or GiB)");
            switch (flag) {
            case 0:
                out->json = true;
                break;
            case 1:
                out->list = true;
                break;
            case 2:
                out->reference = true;
                break;
            case 3:
                out->only[out->only_count++] = operand;
                break;
            case 4:
                out->module = operand;
                break;
            case 5:
                out->limits.stack_size = (size_t)n;
                break;
            case 6:
                out->limits.call_depth = (uint32_t)n;
                break;
            case 7:
                out->limits.heap_size = (size_t)n;
                break;
            case 8:
                out->calls = (uint32_t)n;
                break;
            case 9:
                out->seconds = (uint32_t)n;
                break;
            case 10:
                if (strcmp(arg + 7, "core") == 0)
                    out->emit = MO_EMIT_CORE;
                else if (strcmp(arg + 7, "bytecode") == 0)
                    out->emit = MO_EMIT_BYTECODE;
                else
                    return refuse(error, "--emit must be core or bytecode");
                break;
            default:
                MO_UNREACHABLE();
            }
            continue;
        }
        if (arg[0] == '\0')
            return refuse(error, "file name must not be empty");
        if (out->command == MO_COMMAND_GUIDE)
            return refuse(error, "guide takes no files");
        out->files[out->file_count++] = arg;
        if (out->command == MO_COMMAND_RUN) {
            out->arguments = argv + i + 1;
            out->argument_count = (uint32_t)(argc - i - 1);
            break;
        }
    }
    if (out->command != MO_COMMAND_GUIDE && out->file_count == 0)
        return refuse(error, "command needs a file");
    if ((out->command == MO_COMMAND_CHECK || out->command == MO_COMMAND_TEST) &&
        out->file_count != 1)
        return refuse(error, "command takes exactly one file");
    if (out->json && out->emit != MO_EMIT_NONE)
        return refuse(error, "--json and --emit cannot be combined");
    return true;
}

const char *mo_version(void) {
    return MO_VERSION;
}
const char *mo_version_line(void) {
    return "mo " MO_VERSION;
}
