#define _POSIX_C_SOURCE 200809L

#include "tools/run.h"

#include "base/codes.h"
#include "tools/protocol.h"
#include "vm/vm.h"

#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/stat.h>

#define DATA_FILE "service.sqlite"

static atomic_bool signalled;

static void note_signal(int number) {
    (void)number;
    atomic_store(&signalled, true);
}

static bool watch_signals(void) {
    struct sigaction action = {.sa_handler = note_signal};
    return sigemptyset(&action.sa_mask) == 0 && sigaction(SIGTERM, &action, NULL) == 0 &&
           sigaction(SIGINT, &action, NULL) == 0;
}

struct serve_log {
    FILE *errors;
    bool failed, stopped;
    enum mo_service_status failure, stop;
};

static void log_line(FILE *errors, const char *kind, const char *text) {
    (void)fprintf(errors, "mo serve: %s: %s\n", kind, text);
    (void)fflush(errors);
}

static void log_event(const struct mo_service_event *event, void *context) {
    struct serve_log *log = context;
    const char *text = mo_service_status_text(event->reason);
    switch (event->kind) {
    case MO_SERVICE_FAILURE:
        log->failed = true;
        log->failure = event->reason;
        log_line(log->errors, "failure", text);
        break;
    case MO_SERVICE_RESTART:
        log_line(log->errors, "restart", text);
        break;
    case MO_SERVICE_STOP:
        log->stopped = true;
        log->stop = event->reason;
        log_line(log->errors, "stop", text);
        break;
    }
    if (event->defect != NULL &&
        !mo_write_defect(log->errors, event->defect->process, event->defect))
        (void)fputs("mo: could not write the defect report\n", log->errors);
}

static void refuse(struct mo_diags *diags, const char *what, const char *subject,
                   const char *reason) {
    char message[MO_CLI_ARGUMENT_SIZE + 1024];
    int written = subject == NULL
                      ? snprintf(message, sizeof message, "%s: %s", what, reason)
                      : snprintf(message, sizeof message, "%s %.*s: %s", what,
                                 (int)(MO_CLI_ARGUMENT_SIZE + sizeof DATA_FILE), subject, reason);
    MO_ASSERT(written > 0);
    struct mo_diag diag = {.code = MO_CODE_TOOL_SERVICE_START,
                           .message = message,
                           .primary = {.span = {.file = UINT32_MAX}}};
    bool added = mo_diags_add(diags, &diag);
    if (!added)
        MO_ASSERT(mo_diags_any(diags));
}

static const char *server_refusal(enum mo_protocol_server_status status,
                                  const struct serve_log *log) {
    if (log->failed)
        return mo_service_status_text(log->failure);
    switch (status) {
    case MO_PROTOCOL_SERVER_OK:
        break;
    case MO_PROTOCOL_SERVER_MEMORY:
        return "the server ran out of memory";
    case MO_PROTOCOL_SERVER_CONFIG:
        return "the server configuration was refused";
    case MO_PROTOCOL_SERVER_PROTOCOL:
        return "the embedded protocol failed to start";
    case MO_PROTOCOL_SERVER_SERVICE:
        return "the service failed to start";
    case MO_PROTOCOL_SERVER_IO:
        return "the clock could not be read";
    }
    MO_UNREACHABLE();
}

static bool serve_requests(struct mo_protocol_server *server, struct mo_listener *listener,
                           const struct serve_log *log) {
    for (;;) {
        if (atomic_load(&signalled) || log->stopped) {
            mo_protocol_server_stop_accepting(server);
            if (!mo_protocol_server_connected(server))
                return true;
        }
        union mo_listener_result clock;
        if (mo_listener_call(listener, MO_LISTENER_NOW, NULL, &clock) != MO_LISTENER_OK ||
            mo_protocol_server_poll(server, clock.now.monotonic_us + MO_CLI_SERVE_TICK_US) !=
                MO_PROTOCOL_SERVER_OK) {
            log_line(log->errors, "stop", mo_listener_status_text(MO_LISTENER_IO));
            return false;
        }
    }
}

static int exit_status(const struct serve_log *log, bool served) {
    if (log->stopped)
        return log->stop == MO_SERVICE_STORE ? 70 : MO_EXIT_SERVICE_STOPPED;
    if (!served)
        return 70;
    log_line(log->errors, "stop", "a signal asked the service to stop");
    return 0;
}

static void refuse_listener(struct mo_diags *diags, enum mo_listener_status status, int reason,
                            const char *name, uint16_t port) {
    if (status == MO_LISTENER_CREDENTIAL) {
        refuse(diags, "mo serve could not read the token from environment variable", name,
               "it is missing or invalid");
        return;
    }
    char number[sizeof "65535"];
    int written = snprintf(number, sizeof number, "%u", (unsigned)port);
    MO_ASSERT(written > 0 && (size_t)written < sizeof number);
    refuse(diags, "mo serve could not listen on port", number,
           status == MO_LISTENER_IO ? strerror(reason) : mo_listener_status_text(status));
}

int mo_serve_image(const struct mo_cli *cli, const struct mo_image *image, uint32_t service,
                   FILE *output, FILE *errors, struct mo_diags *diags) {
    const struct mo_bytecode *file = mo_image_bytecode(image);
    MO_ASSERT(service < file->service_count);
    const struct mo_bytecode_string *token = &file->strings[file->services[service].token_env];
    struct serve_log log = {.errors = errors};
    struct mo_listener *listener = NULL;
    struct mo_store *store = NULL;
    struct mo_protocol_server *server = NULL;
    struct mo_buffer name;
    mo_buffer_init(&name, MO_LIMIT_STRING_SIZE + 1);
    int status = 1;
    if (!watch_signals()) {
        refuse(diags, "mo serve could not watch for stop signals", NULL, strerror(errno));
        goto done;
    }
    struct stat data;
    int found = stat(cli->data, &data);
    if (found != 0 || !S_ISDIR(data.st_mode)) {
        refuse(diags, "mo serve can't use the data folder", cli->data,
               strerror(found != 0 ? errno : ENOTDIR));
        goto done;
    }
    if (!mo_buffer_append(&name, token->bytes, token->length) || !mo_buffer_append_byte(&name, 0)) {
        refuse(diags, "mo serve could not read the token's variable name", NULL, "out of memory");
        goto done;
    }
    struct mo_listener_config config = {.port = cli->port};
    enum mo_listener_status opened = mo_listener_open(&config, (const char *)name.bytes, &listener);
    if (opened != MO_LISTENER_OK) {
        refuse_listener(diags, opened, errno, (const char *)name.bytes, cli->port);
        goto done;
    }
    char path[MO_CLI_ARGUMENT_SIZE + sizeof "/" DATA_FILE];
    int length = snprintf(path, sizeof path, "%s/" DATA_FILE, cli->data);
    MO_ASSERT(length > 0 && (size_t)length < sizeof path);
    mode_t mask = umask(S_IRWXG | S_IRWXO);
    enum mo_store_status stored = mo_store_open(path, &store);
    (void)umask(mask);
    if (stored != MO_STORE_OK) {
        refuse(diags, "mo serve could not open saved state at", path, mo_store_status_text(stored));
        goto done;
    }
    uint16_t port = mo_listener_port(listener);
    char numeric[sizeof "127.0.0.1:65535"], named[sizeof "localhost:65535"];
    int first = snprintf(numeric, sizeof numeric, "127.0.0.1:%u", (unsigned)port);
    int second = snprintf(named, sizeof named, "localhost:%u", (unsigned)port);
    MO_ASSERT(first > 0 && (size_t)first < sizeof numeric);
    MO_ASSERT(second > 0 && (size_t)second < sizeof named);
    const char *authorities[] = {numeric, named};
    const struct mo_protocol_events events = {log_event, &log};
    enum mo_protocol_server_status built =
        mo_protocol_server_new(image, service, store, listener, authorities, 2, &events, &server);
    if (built != MO_PROTOCOL_SERVER_OK) {
        refuse(diags, "mo serve could not start the service", NULL, server_refusal(built, &log));
        goto done;
    }
    if (fprintf(output, "mo serve: listening on http://%s/mcp\n", numeric) < 0 ||
        fflush(output) != 0) {
        refuse(diags, "mo serve could not write its listening line", NULL, strerror(errno));
        goto done;
    }
    status = exit_status(&log, serve_requests(server, listener, &log));
done:
    mo_protocol_server_free(server);
    mo_store_close(store);
    mo_listener_close(listener);
    mo_buffer_free(&name);
    return status;
}
