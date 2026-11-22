#include "tools/tools.h"

#include <string.h>

#ifndef MO_VERSION
#define MO_VERSION "0.3.0"
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

static bool worker_count(const char *text, uint32_t *out) {
    uint32_t value = 0;
    const char *p = text;
    for (; *p >= '0' && *p <= '9'; p++) {
        value = value * 10 + (uint32_t)(*p - '0');
        if (value > MO_LIMIT_API_WORKERS_MAX)
            return false;
    }
    if (p == text || *p != '\0' || value == 0)
        return false;
    *out = value;
    return true;
}

static bool ipv4_address(const char *text) {
    const char *p = text;
    for (unsigned part = 0; part < 4; part++) {
        if (part > 0) {
            if (*p != '.')
                return false;
            p++;
        }
        if (*p < '0' || *p > '9')
            return false;
        const char *start = p;
        uint32_t octet = 0;
        while (*p >= '0' && *p <= '9') {
            octet = octet * 10 + (uint32_t)(*p - '0');
            p++;
            if (octet > 255)
                return false;
        }
        if (p - start > 1 && *start == '0')
            return false;
    }
    return *p == '\0';
}

/* A retained failure's id: a positive decimal no larger than INT64_MAX, as the store keeps it. */
static bool failure_id(const char *text, uint64_t *out) {
    uint64_t id = 0;
    const char *p = text;
    if (*p == '\0')
        return false;
    for (; *p != '\0'; p++) {
        if (*p < '0' || *p > '9' || id > ((uint64_t)INT64_MAX - (unsigned)(*p - '0')) / 10)
            return false;
        id = id * 10 + (unsigned)(*p - '0');
    }
    *out = id;
    return id != 0;
}

/* One row per flag: its name, what value it takes, and the commands that accept it. A row's
 * index is its bit in the parse's seen set. */
enum flag_id {
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
    FLAG_EMIT,
    FLAG_FAILURE,
    FLAG_EXPORT,
    FLAG_FIX,
    FLAG_RECOVER,
    FLAG_MAX_WORK,
    FLAG_BUNDLE,
    FLAG_UPGRADE_FROM,
    FLAG_OLD_BUNDLE,
    FLAG_MIGRATE,
    FLAG_UPGRADE,
    FLAG_ADOPT,
    FLAG_UNCONFINED,
    FLAG_CONTEXT,
    FLAG_ARTIFACT,
    FLAG_CANDIDATE,
    FLAG_ADMIN_SOCKET,
    FLAG_BASE,
    FLAG_STRENGTH,
    FLAG_MUTANTS,
    FLAG_LIB,
    FLAG_ROOT,
    FLAG_STATE_DIR,
    FLAG_NAME,
    FLAG_BIN_DIR,
    FLAG_STDIN,
    FLAG_BIND,
    FLAG_WORKERS,
    FLAG_WATCH,
    FLAG_COUNT
};
_Static_assert(FLAG_COUNT <= 64, "seen bitset fits in 64 bits");
enum flag_value {
    VALUE_NONE,
    VALUE_TEXT,
    VALUE_BYTES,
    VALUE_COUNT,
    VALUE_PORT,
    VALUE_WORKERS,
    VALUE_ID
};
struct flag {
    enum flag_id id;
    const char *name;
    bool prefix; /* the value follows the name in the same word, as in --emit=core */
    bool repeated;
    enum flag_value value;
    uint64_t maximum; /* for a bytes or count value */
    uint32_t commands;
};
#define ON(name) (UINT32_C(1) << MO_COMMAND_##name)
static uint32_t command_bit(enum mo_command command) {
    return UINT32_C(1) << command;
}
static const struct flag flags_table[FLAG_COUNT] = {
    {FLAG_JSON, "--json", false, false, VALUE_NONE, 0,
     ON(CHECK) | ON(TEST) | ON(REPLAY) | ON(QUERY) | ON(HISTORY) | ON(REHEARSE) | ON(PUSH) |
         ON(DIFF)},
    {FLAG_LIST, "--list", false, false, VALUE_NONE, 0, ON(TEST)},
    {FLAG_REFERENCE, "--reference", false, false, VALUE_NONE, 0, ON(GUIDE)},
    {FLAG_ONLY, "--only", false, true, VALUE_TEXT, 0, ON(TEST)},
    {FLAG_IN, "--in", false, false, VALUE_TEXT, 0, ON(TEST)},
    {FLAG_MAX_STACK, "--max-stack", false, false, VALUE_BYTES, SIZE_MAX,
     ON(RUN) | ON(TEST) | ON(QUERY)},
    {FLAG_MAX_DEPTH, "--max-depth", false, false, VALUE_COUNT, UINT32_MAX,
     ON(RUN) | ON(TEST) | ON(QUERY)},
    {FLAG_MAX_HEAP, "--max-heap", false, false, VALUE_BYTES, SIZE_MAX,
     ON(RUN) | ON(TEST) | ON(QUERY)},
    {FLAG_CALLS, "--calls", false, false, VALUE_COUNT, UINT32_MAX, ON(TEST)},
    {FLAG_SECONDS, "--seconds", false, false, VALUE_COUNT, UINT32_MAX, ON(TEST)},
    {FLAG_OUTPUT, "-o", false, false, VALUE_TEXT, 0, ON(EMIT)},
    {FLAG_PORT, "--port", false, false, VALUE_PORT, 0, ON(SERVE)},
    {FLAG_DATA, "--data", false, false, VALUE_TEXT, 0, ON(SERVE) | ON(REPLAY) | ON(QUERY)},
    {FLAG_ALLOW_HOST, "--allow-host", false, false, VALUE_TEXT, 0, ON(SERVE)},
    {FLAG_EMIT, "--emit=", true, false, VALUE_NONE, 0, ON(CHECK)},
    {FLAG_FAILURE, "--failure", false, false, VALUE_ID, 0, ON(REPLAY) | ON(QUERY)},
    {FLAG_EXPORT, "--export", false, false, VALUE_TEXT, 0, ON(REPLAY) | ON(QUERY)},
    {FLAG_FIX, "--fix", false, false, VALUE_TEXT, 0, ON(REPLAY)},
    {FLAG_RECOVER, "--recover", false, false, VALUE_NONE, 0, ON(REPLAY) | ON(QUERY) | ON(HISTORY)},
    {FLAG_MAX_WORK, "--max-work", false, false, VALUE_COUNT, UINT64_MAX, ON(QUERY)},
    {FLAG_BUNDLE, "--bundle", false, false, VALUE_NONE, 0, ON(EMIT) | ON(TEST)},
    {FLAG_UPGRADE_FROM, "--upgrade-from", false, false, VALUE_TEXT, 0, ON(CHECK)},
    {FLAG_OLD_BUNDLE, "--old-bundle", false, false, VALUE_TEXT, 0,
     ON(EMIT) | ON(TEST) | ON(REHEARSE)},
    {FLAG_MIGRATE, "--migrate", false, false, VALUE_TEXT, 0,
     ON(CHECK) | ON(SERVE) | ON(EMIT) | ON(REHEARSE) | ON(PUSH)},
    {FLAG_UPGRADE, "--upgrade", false, false, VALUE_NONE, 0, ON(SERVE)},
    {FLAG_ADOPT, "--adopt", false, false, VALUE_NONE, 0, ON(SERVE)},
    {FLAG_UNCONFINED, "--unconfined", false, false, VALUE_NONE, 0, ON(SERVE)},
    {FLAG_CONTEXT, "--context", false, false, VALUE_TEXT, 0, ON(EMIT) | ON(TEST) | ON(REHEARSE)},
    {FLAG_ARTIFACT, "--artifact", false, false, VALUE_TEXT, 0, ON(REHEARSE)},
    {FLAG_CANDIDATE, "--candidate", false, false, VALUE_TEXT, 0, ON(REHEARSE)},
    {FLAG_ADMIN_SOCKET, "--admin-socket", false, false, VALUE_TEXT, 0,
     ON(SERVE) | ON(PUSH) | ON(QUERY)},
    {FLAG_BASE, "--base", false, false, VALUE_TEXT, 0, ON(PUSH)},
    {FLAG_STRENGTH, "--strength", false, false, VALUE_TEXT, 0, ON(TEST)},
    {FLAG_MUTANTS, "--mutants", false, false, VALUE_NONE, 0, ON(TEST)},
    {FLAG_LIB, "--lib", false, false, VALUE_TEXT, 0,
     ON(CHECK) | ON(RUN) | ON(TEST) | ON(FIX) | ON(EMIT) | ON(SERVE) | ON(REPLAY) | ON(QUERY) |
         ON(PUSH) | ON(DIFF) | ON(INSTALL)},
    {FLAG_ROOT, "--root", false, false, VALUE_TEXT, 0, ON(RUN) | ON(INSTALL)},
    {FLAG_STATE_DIR, "--state-dir", false, false, VALUE_TEXT, 0, ON(RUN) | ON(INSTALL)},
    {FLAG_NAME, "--name", false, false, VALUE_TEXT, 0, ON(INSTALL)},
    {FLAG_BIN_DIR, "--bin-dir", false, false, VALUE_TEXT, 0, ON(INSTALL)},
    {FLAG_STDIN, "--stdin", false, false, VALUE_NONE, 0, ON(FORMAT)},
    {FLAG_BIND, "--bind", false, false, VALUE_TEXT, 0, ON(SERVE)},
    {FLAG_WORKERS, "--workers", false, false, VALUE_WORKERS, 0, ON(SERVE)},
    {FLAG_WATCH, "--watch", false, false, VALUE_NONE, 0, ON(SERVE)},
};
#undef ON

bool mo_valid_install_name(const char *name) {
    if (name == NULL || name[0] == '\0')
        return false;
    char c0 = name[0];
    bool ok0 = (c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') || (c0 >= '0' && c0 <= '9') ||
               c0 == '_';
    if (!ok0)
        return false;
    size_t len = 1;
    for (const char *p = name + 1; *p != '\0'; p++, len++) {
        if (len >= MO_LIMIT_INSTALL_NAME)
            return false;
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-' || c == '.';
        if (!ok)
            return false;
    }
    return true;
}

static bool set_flag(struct mo_cli *out, const char **error, enum flag_id id, const char *arg,
                     const char *operand, uint64_t n) {
    switch (id) {
    case FLAG_ARTIFACT:
        out->artifact = operand;
        break;
    case FLAG_CANDIDATE:
        out->candidate = operand;
        break;
    case FLAG_ADMIN_SOCKET:
        out->admin_socket = operand;
        break;
    case FLAG_BASE:
        out->push_base = operand;
        break;
    case FLAG_MUTANTS:
        out->mutants = true;
        break;
    case FLAG_LIB:
        out->library = operand;
        break;
    case FLAG_ROOT:
        out->root = operand;
        break;
    case FLAG_STATE_DIR:
        out->state_dir = operand;
        break;
    case FLAG_NAME:
        if (!mo_valid_install_name(operand))
            return refuse(out, error,
                          "invalid tool name (expected ASCII letters, digits, '.', '_' or '-', "
                          "starting with a letter, digit or '_')",
                          arg, operand);
        out->install_name = operand;
        break;
    case FLAG_BIN_DIR:
        out->bin_dir = operand;
        break;
    case FLAG_STDIN:
        out->stdin_mode = true;
        break;
    case FLAG_BIND:
        if (!ipv4_address(operand))
            return refuse(
                out, error,
                "invalid bind address (expected an IPv4 address such as 127.0.0.1 or 0.0.0.0)", arg,
                operand);
        out->bind_host = operand;
        break;
    case FLAG_WORKERS:
        if (!worker_count(operand, &out->workers))
            return refuse(out, error,
                          "invalid worker count (expected a decimal integer from 1 to 64)", arg,
                          operand);
        out->has_workers = true;
        break;
    case FLAG_WATCH:
        out->watch = true;
        break;
    case FLAG_STRENGTH:
        out->strength = operand;
        break;
    case FLAG_CONTEXT:
        out->compile_context = operand;
        break;
    case FLAG_BUNDLE:
        out->source_bundle = true;
        break;
    case FLAG_UPGRADE_FROM:
        out->upgrade_from = operand;
        break;
    case FLAG_OLD_BUNDLE:
        out->old_bundle = operand;
        break;
    case FLAG_MIGRATE:
        out->migration = operand;
        break;
    case FLAG_UPGRADE:
        out->upgrade = true;
        break;
    case FLAG_ADOPT:
        out->adopt = true;
        break;
    case FLAG_UNCONFINED:
        out->unconfined = true;
        break;
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
            return refuse(out, error, "invalid port (expected a decimal integer from 0 to 65535)",
                          arg, operand);
        out->has_port = true;
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
    case FLAG_FAILURE:
        if (!failure_id(operand, &out->failure))
            return refuse(out, error, "failure id must be positive", arg, operand);
        break;
    case FLAG_EXPORT:
        out->artifact = operand;
        break;
    case FLAG_FIX:
        out->revised = operand;
        break;
    case FLAG_RECOVER:
        out->recover = true;
        break;
    case FLAG_MAX_WORK:
        out->max_work = n;
        break;
    case FLAG_COUNT:
        MO_UNREACHABLE();
    }
    return true;
}

bool mo_cli_parse(int argc, const char *const argv[], struct mo_cli *out, const char **error) {
    MO_ASSERT(out != NULL && error != NULL && argc >= 0);
    *out = (struct mo_cli){.limits = mo_limits_default(),
                           .calls = 200,
                           .seconds = 2,
                           .workers = MO_LIMIT_API_WORKERS,
                           .file_separator = UINT32_MAX,
                           .max_work = MO_LIMIT_QUERY_WORK};
    *error = NULL;
    if (argc < 2)
        return refuse(out, error,
                      "expected a command: check, run, test, format, fix, guide, emit, serve, "
                      "fetch, push, replay, query, rehearse, history, diff or install",
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
    if (strcmp(command, "fetch") == 0) {
        out->command = MO_COMMAND_FETCH;
        bool seen_socket = false, seen_export = false, seen_out = false;
        for (int i = 2; i < argc; i++) {
            const char *arg = argv[i];
            MO_ASSERT(arg != NULL);
            if (!option(out, error, arg, &count, &total))
                return false;
            if (strcmp(arg, "--admin-socket") == 0 && !seen_socket && i + 1 < argc) {
                seen_socket = true;
                out->admin_socket = argv[++i];
                if (!option(out, error, out->admin_socket, &count, &total))
                    return false;
                if (out->admin_socket[0] == '\0' || out->admin_socket[0] == '-')
                    return refuse(
                        out, error,
                        "usage: mo fetch --admin-socket PATH --export state|failure ID --out FILE",
                        NULL, NULL);
            } else if (strcmp(arg, "--out") == 0 && !seen_out && i + 1 < argc) {
                seen_out = true;
                out->output = argv[++i];
                if (!option(out, error, out->output, &count, &total))
                    return false;
                if (out->output[0] == '\0' || out->output[0] == '-')
                    return refuse(
                        out, error,
                        "usage: mo fetch --admin-socket PATH --export state|failure ID --out FILE",
                        NULL, NULL);
            } else if (strcmp(arg, "--export") == 0 && !seen_export && i + 1 < argc) {
                seen_export = true;
                const char *kind = argv[++i];
                if (!option(out, error, kind, &count, &total))
                    return false;
                if (strcmp(kind, "state") == 0) {
                    out->fetch_failure = 0;
                } else if (strcmp(kind, "failure") == 0 && i + 1 < argc) {
                    const char *id = argv[++i];
                    if (!option(out, error, id, &count, &total))
                        return false;
                    if (!failure_id(id, &out->fetch_failure))
                        return refuse(out, error, "failure id must be positive", NULL, NULL);
                } else {
                    return refuse(
                        out, error,
                        "usage: mo fetch --admin-socket PATH --export state|failure ID --out FILE",
                        NULL, NULL);
                }
            } else {
                return refuse(
                    out, error,
                    "usage: mo fetch --admin-socket PATH --export state|failure ID --out FILE",
                    NULL, NULL);
            }
        }
        if (!seen_socket || !seen_export || !seen_out)
            return refuse(
                out, error,
                "usage: mo fetch --admin-socket PATH --export state|failure ID --out FILE", NULL,
                NULL);
        return true;
    }
    static const struct {
        const char *name;
        enum mo_command command;
    } names[] = {{"check", MO_COMMAND_CHECK},       {"run", MO_COMMAND_RUN},
                 {"test", MO_COMMAND_TEST},         {"format", MO_COMMAND_FORMAT},
                 {"fix", MO_COMMAND_FIX},           {"guide", MO_COMMAND_GUIDE},
                 {"emit", MO_COMMAND_EMIT},         {"serve", MO_COMMAND_SERVE},
                 {"replay", MO_COMMAND_REPLAY},     {"query", MO_COMMAND_QUERY},
                 {"rehearse", MO_COMMAND_REHEARSE}, {"history", MO_COMMAND_HISTORY},
                 {"push", MO_COMMAND_PUSH},         {"diff", MO_COMMAND_DIFF},
                 {"install", MO_COMMAND_INSTALL}};
    unsigned c = 0;
    for (; c < sizeof(names) / sizeof(names[0]); c++)
        if (strcmp(command, names[c].name) == 0)
            break;
    if (c == sizeof(names) / sizeof(names[0]))
        return refuse(
            out, error,
            "unknown command (expected check, run, test, format, fix, guide, emit, serve, "
            "fetch, push, replay, query, rehearse, history, diff or install)",
            command, NULL);
    out->command = names[c].command;
    bool flags = true;
    uint64_t seen = 0;
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
            uint32_t row = 0;
            while (row < FLAG_COUNT &&
                   !(flags_table[row].prefix
                         ? strncmp(arg, flags_table[row].name, strlen(flags_table[row].name)) == 0
                         : strcmp(arg, flags_table[row].name) == 0))
                row++;
            if (row == FLAG_COUNT)
                return refuse(out, error, "unknown flag", arg, NULL);
            const struct flag *flag = &flags_table[row];
            MO_ASSERT(flag->id == (enum flag_id)row);
            if ((flag->commands & command_bit(out->command)) == 0)
                return refuse(out, error, "flag is not accepted by this command", arg, NULL);
            uint64_t bit = UINT64_C(1) << row;
            if (!flag->repeated && (seen & bit) != 0)
                return refuse(out, error, "flag may be given only once (except --only)", arg, NULL);
            seen |= bit;
            const char *operand = NULL;
            if (flag->value != VALUE_NONE) {
                if (i + 1 == argc)
                    return refuse(out, error, "flag needs a nonempty value", arg, NULL);
                operand = argv[++i];
                MO_ASSERT(operand != NULL);
                if (!option(out, error, operand, &count, &total))
                    return false;
                /* A number may look like a negative one, which its own check then refuses. */
                bool numeric = flag->value != VALUE_TEXT;
                if (operand[0] == '\0' || (operand[0] == '-' && (!numeric || operand[1] == '-')))
                    return refuse(out, error, "flag needs a nonempty value", arg, operand);
            }
            uint64_t n = 0;
            if ((flag->value == VALUE_BYTES || flag->value == VALUE_COUNT) &&
                !number(operand, flag->value == VALUE_BYTES, flag->maximum, &n))
                return refuse(out, error,
                              "invalid limit (expected a positive decimal integer; bytes "
                              "may use B, KiB, MiB or GiB)",
                              arg, operand);
            if (!set_flag(out, error, flag->id, arg, operand, n))
                return false;
            continue;
        }
        if (arg[0] == '\0')
            return refuse(out, error, "file name must not be empty", arg, NULL);
        if (out->command == MO_COMMAND_GUIDE)
            return refuse(out, error, "guide takes no files", arg, NULL);
        if (out->file_count != 0 &&
            (out->command == MO_COMMAND_CHECK || out->command == MO_COMMAND_TEST ||
             out->command == MO_COMMAND_EMIT || out->command == MO_COMMAND_SERVE ||
             out->command == MO_COMMAND_REPLAY || out->command == MO_COMMAND_PUSH ||
             out->command == MO_COMMAND_INSTALL))
            return refuse(out, error, "command takes exactly one file", arg, NULL);
        if (out->file_count == 2 && out->command == MO_COMMAND_DIFF)
            return refuse(out, error, "diff takes one or two files", arg, NULL);
        if (out->file_count != 0 && out->command == MO_COMMAND_HISTORY)
            return refuse(out, error, "history takes exactly one data folder", arg, NULL);
        if (out->file_count == 2 && out->command == MO_COMMAND_QUERY)
            return refuse(out, error, "query takes exactly one file and one function", arg, NULL);
        if (out->files == NULL)
            out->files = argv + i;
        out->file_count++;
        if (out->command == MO_COMMAND_RUN) {
            out->arguments = argv + i + 1;
            out->argument_count = (uint32_t)(argc - i - 1);
            break;
        }
    }
    if (out->command == MO_COMMAND_FORMAT && out->stdin_mode && out->file_count != 0)
        return refuse(out, error, "format --stdin takes no files", "--stdin", NULL);
    if (out->command == MO_COMMAND_HISTORY && out->file_count == 0)
        return refuse(out, error, "history needs a data folder", command, NULL);
    if (out->command != MO_COMMAND_GUIDE && out->command != MO_COMMAND_SERVE &&
        out->command != MO_COMMAND_REHEARSE && out->file_count == 0 &&
        !(out->command == MO_COMMAND_FORMAT && out->stdin_mode) &&
        !(out->command == MO_COMMAND_REPLAY && out->data != NULL))
        return refuse(out, error, "command needs a file", command, NULL);
    if (out->command == MO_COMMAND_QUERY && out->file_count != 2)
        return refuse(out, error, "query needs a file and a function", command, NULL);
    if (out->command == MO_COMMAND_QUERY)
        out->function = mo_cli_file(out, 1);
    bool replay = out->command == MO_COMMAND_REPLAY, query = out->command == MO_COMMAND_QUERY;
    if (replay && out->failure == 0)
        return refuse(out, error, "replay needs --failure ID", command, NULL);
    if (replay && (out->data == NULL) == (out->artifact == NULL))
        return refuse(out, error, "give exactly one of --data DIR and --export ARTIFACT", command,
                      NULL);
    uint32_t query_sources = (out->data != NULL ? 1u : 0u) + (out->artifact != NULL ? 1u : 0u) +
                             (out->admin_socket != NULL ? 1u : 0u);
    if (query && query_sources != 1)
        return refuse(out, error,
                      "give exactly one of --data DIR, --export ARTIFACT and --admin-socket PATH",
                      command, NULL);
    if (query && out->data != NULL && out->failure == 0)
        return refuse(out, error, "a query over a data folder needs --failure ID", command, NULL);
    if (out->recover && (out->artifact != NULL || out->admin_socket != NULL))
        return refuse(out, error, "--recover reads a data folder, not --export or --admin-socket",
                      "--recover", NULL);
    if (out->command == MO_COMMAND_EMIT && out->output == NULL)
        return refuse(out, error, "emit needs -o FILE", command, NULL);
    uint64_t planned = (UINT64_C(1) << FLAG_LIST) | (UINT64_C(1) << FLAG_ONLY) |
                       (UINT64_C(1) << FLAG_IN) | (UINT64_C(1) << FLAG_CALLS);
    uint64_t limited = (UINT64_C(1) << FLAG_ONLY) | (UINT64_C(1) << FLAG_IN) |
                       (UINT64_C(1) << FLAG_CALLS) | (UINT64_C(1) << FLAG_SECONDS) |
                       (UINT64_C(1) << FLAG_MAX_STACK) | (UINT64_C(1) << FLAG_MAX_DEPTH) |
                       (UINT64_C(1) << FLAG_MAX_HEAP);
    if (out->mutants && (out->source_bundle || out->strength != NULL || (seen & limited) != 0))
        return refuse(out, error,
                      "--mutants runs every check and tries every mutant; it takes no "
                      "--only, --in, --calls, --seconds, --max-stack, --max-depth, --max-heap, "
                      "--bundle or --strength",
                      "--mutants", NULL);
    bool checks = out->command == MO_COMMAND_TEST && out->source_bundle;
    if (checks && (out->json || out->strength != NULL ||
                   (seen & (planned | (UINT64_C(1) << FLAG_SECONDS))) != 0))
        return refuse(out, error,
                      "test --bundle runs upgrade checks over the whole bundle; "
                      "it takes no --list, --json, --only, --in, --calls, --seconds or --strength",
                      "--bundle", NULL);
    if (checks)
        out->seconds = (uint32_t)(MO_LIMIT_CHECK_WALL_US / UINT64_C(1000000));
    if (out->command == MO_COMMAND_SERVE && out->file_count == 0) {
        if ((seen & (UINT64_C(1) << FLAG_PORT)) == 0)
            return refuse(out, error, "serve needs --port N", command, NULL);
        if ((seen & (UINT64_C(1) << FLAG_DATA)) == 0)
            return refuse(out, error, "serve needs --data DIR", command, NULL);
    }
    if (out->command == MO_COMMAND_SERVE && out->file_count == 0 &&
        (out->upgrade || out->adopt || out->migration != NULL))
        return refuse(out, error, "upgrade or adoption needs a source file", command, NULL);
    if (out->command == MO_COMMAND_SERVE && out->file_count == 0 &&
        (out->bind_host != NULL || out->has_workers || out->watch))
        return refuse(out, error, "--bind, --workers and --watch apply only to api servers",
                      command, NULL);
    if (out->command == MO_COMMAND_PUSH && out->admin_socket == NULL)
        return refuse(out, error,
                      "usage: mo push FILE --admin-socket PATH [--base DIGEST] "
                      "[--migrate Module.function] [--json]",
                      command, NULL);
    if (out->migration != NULL && out->command != MO_COMMAND_PUSH &&
        !(out->upgrade || out->upgrade_from != NULL || out->old_bundle != NULL ||
          out->compile_context != NULL))
        return refuse(out, error, "--migrate needs --upgrade or an old program", "--migrate", NULL);
    if (out->command == MO_COMMAND_REHEARSE && (out->file_count != 0 || out->artifact == NULL ||
                                                out->old_bundle == NULL || out->candidate == NULL))
        return refuse(out, error,
                      "rehearse needs --artifact, --old-bundle and --candidate, and no files",
                      command, NULL);
    if (out->old_bundle != NULL && !out->source_bundle && out->command != MO_COMMAND_REHEARSE)
        return refuse(out, error, "--old-bundle needs --bundle", "--old-bundle", NULL);
    if (out->json && out->emit != MO_EMIT_NONE)
        return refuse(out, error, "--json and --emit cannot be combined", "--json", "--emit");
    if (out->strength != NULL && (seen & planned) != 0)
        return refuse(out, error,
                      "--strength runs a fixed plan over the whole program; it takes no --list, "
                      "--only, --in or --calls",
                      "--strength", NULL);
    out->door_plan = out->strength != NULL || checks;
    if (out->door_plan)
        out->calls = MO_DOOR_PLAN_INPUTS;
    out->sequences = out->door_plan ? MO_DOOR_PLAN_SEQUENCES : out->calls;
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
