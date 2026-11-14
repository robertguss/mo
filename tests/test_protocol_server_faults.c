#define MO_PROTOCOL_SERVER_FAULT_TEST
#define MO_PROTOCOL_SERVER_UPGRADE_TEST
#include "test_protocol_server.c"

#include <errno.h>
#include <time.h>
#include <unistd.h>

static const uint8_t no_items[] = {'M', 'O', 'I', 'L', 1, 1, 0, 0, 0, 0};
static struct mo_protocol_server *fault_server;
static uint64_t admitted_before;
static bool exhaust_reply, record_resizes, fail_append;
static unsigned exhausted_calls, memory_stops, decode_after, decode_failures, append_failures;
static unsigned fail_call_after;
static bool scan_limit_zero;
static size_t resize_calls, resize_after, resize_failures, resize_sizes[4096];
/* 0 runs the real activation; otherwise its result without touching the store. */
static enum mo_store_activation_status activation_result;
static bool activation_faulted;

enum mo_outcome mo_protocol_server_fault_call(struct mo_process *process, uint32_t function,
                                              const struct mo_cell *arguments, uint32_t count,
                                              struct mo_cell *out, struct mo_defect *defect);
void *mo_protocol_server_fault_resize(struct mo_scratch *scratch, void *bytes, size_t old_size,
                                      size_t new_size);
bool mo_protocol_server_fault_decode(struct mo_heap *heap, uint32_t type, const uint8_t *bytes,
                                     size_t length, struct mo_cell *out);
enum mo_store_status mo_protocol_server_fault_store(
    struct mo_store *store, enum mo_store_operation operation, const struct mo_store_record *record,
    bool (*consume)(const struct mo_store_record *, void *), void *context);

enum mo_outcome mo_protocol_server_fault_call(struct mo_process *process, uint32_t function,
                                              const struct mo_cell *arguments, uint32_t count,
                                              struct mo_cell *out, struct mo_defect *defect) {
    if (fail_call_after != 0 && --fail_call_after == 0)
        return MO_STOPPED;
    bool exhaust = exhaust_reply && fault_server != NULL &&
                   mo_protocol_server_metrics(fault_server).admitted > admitted_before;
    if (exhaust) {
        exhaust_reply = false;
        exhausted_calls++;
        struct mo_heap *heap = mo_process_heap(process);
        mo_heap_set_limit(heap, mo_heap_used(heap));
    }
    struct mo_cell scan_args[3];
    if (scan_limit_zero) {
        MO_ASSERT(count == 3);
        memcpy(scan_args, arguments, sizeof scan_args);
        scan_args[2] = mo_cell_int(0);
        arguments = scan_args;
        scan_limit_zero = false;
    }
    enum mo_outcome outcome = mo_call(process, function, arguments, count, out, defect);
    if (exhaust) {
        CHECK(outcome == MO_STOPPED);
        if (outcome == MO_STOPPED) {
            CHECK(defect->memory);
            memory_stops += defect->memory ? 1u : 0u;
        }
    }
    return outcome;
}

void *mo_protocol_server_fault_resize(struct mo_scratch *scratch, void *bytes, size_t old_size,
                                      size_t new_size) {
    if (record_resizes) {
        MO_ASSERT(resize_calls < sizeof resize_sizes / sizeof *resize_sizes);
        resize_sizes[resize_calls++] = new_size;
    }
    if (resize_after != 0 && --resize_after == 0) {
        resize_failures++;
        errno = ENOMEM;
        return NULL;
    }
    return mo_scratch_resize(scratch, bytes, old_size, new_size);
}

bool mo_protocol_server_fault_decode(struct mo_heap *heap, uint32_t type, const uint8_t *bytes,
                                     size_t length, struct mo_cell *out) {
    if (decode_after != 0 && --decode_after == 0) {
        size_t limit = mo_heap_limit(heap);
        mo_heap_set_limit(heap, mo_heap_used(heap));
        bool ok = mo_value_decode(heap, type, bytes, length, out);
        mo_heap_set_limit(heap, limit);
        CHECK(!ok);
        decode_failures++;
        return ok;
    }
    return mo_value_decode(heap, type, bytes, length, out);
}

enum mo_store_status mo_protocol_server_fault_store(
    struct mo_store *store, enum mo_store_operation operation, const struct mo_store_record *record,
    bool (*consume)(const struct mo_store_record *, void *), void *context) {
    if (fail_append && operation == MO_STORE_APPEND) {
        append_failures++;
        return MO_STORE_IO;
    }
    return mo_store_call(store, operation, record, consume, context);
}

static bool fail_process_new;
static unsigned process_new_failures;

struct mo_process *mo_protocol_server_fault_process_new(const struct mo_image *image,
                                                        const struct mo_limits *limits);

struct mo_process *mo_protocol_server_fault_process_new(const struct mo_image *image,
                                                        const struct mo_limits *limits) {
    if (fail_process_new) {
        fail_process_new = false;
        process_new_failures++;
        return NULL;
    }
    return mo_process_new(image, limits);
}

static void accept_allocation_failure(void) {
    struct fixture_server f = start();
    struct mo_buffer input;
    input_request(&input, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}", auth);
    char out[4096];
    struct mo_listener_fake_peer peer = {.input = input.bytes,
                                         .input_size = input.length,
                                         .output = (uint8_t *)out,
                                         .output_capacity = sizeof out,
                                         .readable = true,
                                         .writable = true,
                                         .eof = true};
    MO_ASSERT(mo_listener_fake_connect(f.listener, &peer) == MO_LISTENER_OK);
    fail_process_new = true;
    for (unsigned i = 0; i < 8 && !peer.closed; i++)
        poll_server(&f);
    CHECK(!fail_process_new && process_new_failures == 1);
    CHECK(peer.closed && peer.output_size == 0);
    CHECK(mo_protocol_server_metrics(f.server).admitted == 0);
    tool_request(&f, "read", "{}", "2", out, sizeof out);
    CHECK(strstr(out, "HTTP/1.1 200 ") == out);
    CHECK(strstr(out, "\"structuredContent\":{\"value\":\"0\"}") != NULL);
    CHECK(mo_protocol_server_metrics(f.server).admitted == 1);
    mo_buffer_free(&input);
    finish(&f);
}

static void reserved_fallback(void) {
    resize_calls = 0;
    record_resizes = true;
    struct fixture_server f = start();
    record_resizes = false;
    size_t first = SIZE_MAX;
    for (size_t i = 0; i + 8 <= resize_calls; i++) {
        bool reserve = resize_sizes[i] > 6 * MO_LIMIT_MCP_ID_BYTES;
        for (size_t j = 1; j < 8; j++)
            reserve = reserve && resize_sizes[i + j] == resize_sizes[i];
        if (reserve) {
            CHECK(first == SIZE_MAX);
            first = i;
        }
    }
    MO_ASSERT(first != SIZE_MAX);
    mo_protocol_server_free(f.server);
    f.server = NULL;
    const char *authorities[] = {"localhost"};
    for (size_t i = 0; i < 8; i += 7) {
        resize_after = first + i + 1;
        CHECK(mo_protocol_server_new(f.image, 0, f.store, f.listener, authorities, 1, NULL,
                                     &f.server) == MO_PROTOCOL_SERVER_MEMORY);
        CHECK(f.server == NULL);
        CHECK(resize_after == 0);
    }
    CHECK(resize_failures == 2);
    MO_ASSERT(mo_protocol_server_new(f.image, 0, f.store, f.listener, authorities, 1, NULL,
                                     &f.server) == MO_PROTOCOL_SERVER_OK);
    char out[4096];
    tool_request(&f, "read", "{}", "1", out, sizeof out);
    CHECK(strstr(out, "\"structuredContent\":{\"value\":\"0\"}") != NULL);
    finish(&f);
}

static void exhausted_fallback(void) {
    struct fixture_server f = start();
    char out[4096], id[6 * 128 + 3];
    maximum_id(id);
    fault_server = f.server;
    for (unsigned updating = 0; updating < 2; updating++) {
        admitted_before = mo_protocol_server_metrics(f.server).admitted;
        exhaust_reply = true;
        tool_request(&f, updating == 0 ? "read" : "add",
                     updating == 0 ? "{}" : "{\"amount\":\"7\"}", id, out, sizeof out);
        CHECK(!exhaust_reply);
        CHECK(mo_protocol_server_metrics(f.server).admitted == admitted_before + 1);
        tool_error(out, "response_limit", updating == 0 ? "none" : "applied", id);
        tool_request(&f, "read", "{}", "2", out, sizeof out);
        CHECK(strstr(out, updating == 0 ? "\"structuredContent\":{\"value\":\"0\"}"
                                        : "\"structuredContent\":{\"value\":\"7\"}") != NULL);
    }
    CHECK(exhausted_calls == 2 && memory_stops == 2);
    fault_server = NULL;
    finish(&f);
}

static void saved_copy_memory(void) {
    struct fixture_server f = start();
    char out[4096];
    for (unsigned after = 1; after <= 2; after++) {
        uint64_t admitted = mo_protocol_server_metrics(f.server).admitted;
        decode_after = after;
        tool_request(&f, "add", "{\"amount\":\"7\"}", "1", out, sizeof out);
        tool_error(out, "memory_limit", "none", "1");
        CHECK(decode_after == 0);
        CHECK(mo_protocol_server_metrics(f.server).admitted == admitted + (after == 1 ? 0u : 1u));
        tool_request(&f, "read", "{}", "2", out, sizeof out);
        CHECK(strstr(out, "\"structuredContent\":{\"value\":\"0\"}") != NULL);
    }
    CHECK(decode_failures == 2);
    tool_request(&f, "add", "{\"amount\":\"3\"}", "3", out, sizeof out);
    CHECK(strstr(out, "\"structuredContent\":{\"value\":\"3\"}") != NULL);
    finish(&f);
}

static void store_failure(void) {
    struct fixture_server f = start();
    char out[4096];
    tool_request(&f, "add", "{\"amount\":\"7\"}", "1", out, sizeof out);
    CHECK(strstr(out, "\"structuredContent\":{\"value\":\"7\"}") != NULL);
    fail_append = true;
    tool_request(&f, "add", "{\"amount\":\"5\"}", "2", out, sizeof out);
    tool_error(out, "store_failed", "unknown", "2");
    fail_append = false;
    CHECK(append_failures == 1);
    CHECK(mo_protocol_server_metrics(f.server).admitted == 2);
    tool_request(&f, "read", "{}", "3", out, sizeof out);
    tool_error(out, "stopped", "none", "3");
    tool_request(&f, "add", "{\"amount\":\"9\"}", "4", out, sizeof out);
    tool_error(out, "stopped", "none", "4");
    CHECK(mo_protocol_server_metrics(f.server).admitted == 2);
    mo_protocol_server_free(f.server);
    f.server = NULL;
    const char *authorities[] = {"localhost"};
    MO_ASSERT(mo_protocol_server_new(f.image, 0, f.store, f.listener, authorities, 1, NULL,
                                     &f.server) == MO_PROTOCOL_SERVER_OK);
    tool_request(&f, "read", "{}", "5", out, sizeof out);
    CHECK(strstr(out, "\"structuredContent\":{\"value\":\"7\"}") != NULL);
    finish(&f);
}

static void store_stop_event(void) {
    struct event_log log = {.count = 0};
    struct fixture_server f = start_events(source, &log);
    log.server = &f.server;
    char out[4096];
    fail_append = true;
    tool_request(&f, "add", "{\"amount\":\"5\"}", "1", out, sizeof out);
    tool_error(out, "store_failed", "unknown", "1");
    fail_append = false;
    CHECK(log.count == 1);
    CHECK(log.events[0].kind == MO_SERVICE_STOP);
    CHECK(log.events[0].reason == MO_SERVICE_STORE);
    CHECK(log.events[0].defect == NULL);
    finish(&f);
}

static void preauth_failure(void) {
    struct fixture_server f = start();
    char out[4096];
    for (unsigned after = 1; after <= 4; after++) {
        fail_call_after = after;
        request(&f, "{}", auth, out, sizeof out, 0);
        CHECK(fail_call_after == 0);
        CHECK(strstr(out, "HTTP/1.1 413 ") == out);
        CHECK(strstr(out, "Content-Length: 0\r\n") != NULL);
        CHECK(mo_protocol_server_metrics(f.server).admitted == 0);
    }
    resize_after = 1;
    size_t failed_before = resize_failures;
    request(&f, "{}", auth, out, sizeof out, 0);
    CHECK(resize_after == 0 && resize_failures == failed_before + 1);
    CHECK(strstr(out, "HTTP/1.1 413 ") == out);
    CHECK(strstr(out, "Content-Length: 0\r\n") != NULL);
    CHECK(mo_protocol_server_metrics(f.server).admitted == 0);
    tool_request(&f, "read", "{}", "1", out, sizeof out);
    CHECK(strstr(out, "\"structuredContent\":{\"value\":\"0\"}") != NULL);
    finish(&f);
}

static struct mo_listener *wait_listener;
static uint64_t wait_advance;
static enum mo_listener_status write_status;
static bool fail_half_close;
static unsigned injected_waits, injected_writes;

enum mo_listener_status mo_protocol_server_fault_listener(struct mo_listener *listener,
                                                          enum mo_listener_operation operation,
                                                          const union mo_listener_request *request,
                                                          union mo_listener_result *result);

enum mo_listener_status mo_protocol_server_fault_listener(struct mo_listener *listener,
                                                          enum mo_listener_operation operation,
                                                          const union mo_listener_request *request,
                                                          union mo_listener_result *result) {
    if (operation == MO_LISTENER_WAIT && listener == wait_listener && wait_advance != 0) {
        MO_ASSERT(mo_listener_fake_advance(listener, wait_advance) == MO_LISTENER_OK);
        wait_advance = 0;
        injected_waits++;
    }
    if (operation == MO_LISTENER_WRITE && write_status != MO_LISTENER_OK) {
        enum mo_listener_status status = write_status;
        write_status = MO_LISTENER_OK;
        injected_writes++;
        return status;
    }
    if (operation == MO_LISTENER_HALF_CLOSE && fail_half_close) {
        fail_half_close = false;
        return MO_LISTENER_IO;
    }
    return mo_listener_call(listener, operation, request, result);
}

static void poll_failures(void) {
    struct fixture_server f = start();
    wait_listener = f.listener;
    char out[4096];
    struct mo_listener_fake_peer peer = {
        .output = (uint8_t *)out, .output_capacity = sizeof out, .writable = true};
    MO_ASSERT(mo_listener_fake_connect(f.listener, &peer) == MO_LISTENER_OK);
    poll_server(&f);
    wait_advance = 5000000;
    poll_server(&f);
    CHECK(peer.closed && peer.output_size == 0 && injected_waits == 1);
    peer = (struct mo_listener_fake_peer){.read_error = true};
    MO_ASSERT(mo_listener_fake_connect(f.listener, &peer) == MO_LISTENER_OK);
    poll_server(&f);
    poll_server(&f);
    CHECK(peer.closed && peer.output_size == 0);
    struct mo_buffer input;
    input_request(&input, "", auth);
    peer = (struct mo_listener_fake_peer){.input = input.bytes,
                                          .input_size = input.length,
                                          .output = (uint8_t *)out,
                                          .output_capacity = sizeof out - 1,
                                          .readable = true,
                                          .writable = true,
                                          .eof = true};
    MO_ASSERT(mo_listener_fake_connect(f.listener, &peer) == MO_LISTENER_OK);
    poll_server(&f);
    scan_limit_zero = true;
    for (unsigned i = 0; i < 8 && !peer.closed; i++)
        poll_server(&f);
    CHECK(peer.closed && peer.half_closed && !scan_limit_zero);
    out[peer.output_size] = 0;
    CHECK(strstr(out, "HTTP/1.1 431 ") == out);
    peer = (struct mo_listener_fake_peer){.input = input.bytes,
                                          .input_size = input.length,
                                          .output = (uint8_t *)out,
                                          .output_capacity = sizeof out,
                                          .readable = true,
                                          .writable = true};
    MO_ASSERT(mo_listener_fake_connect(f.listener, &peer) == MO_LISTENER_OK);
    poll_server(&f);
    poll_server(&f);
    write_status = MO_LISTENER_AGAIN;
    poll_server(&f);
    CHECK(!peer.closed && peer.output_size == 0 && injected_writes == 1);
    write_status = MO_LISTENER_IO;
    poll_server(&f);
    CHECK(peer.closed && peer.output_size == 0 && injected_writes == 2);
    uint64_t completed = mo_protocol_server_metrics(f.server).completed;
    peer = (struct mo_listener_fake_peer){.input = input.bytes,
                                          .input_size = input.length,
                                          .output = (uint8_t *)out,
                                          .output_capacity = sizeof out,
                                          .readable = true,
                                          .writable = true};
    MO_ASSERT(mo_listener_fake_connect(f.listener, &peer) == MO_LISTENER_OK);
    fail_half_close = true;
    for (unsigned i = 0; i < 8 && !peer.closed; i++)
        poll_server(&f);
    CHECK(peer.closed && !peer.half_closed && !fail_half_close);
    CHECK(peer.output_size > 0);
    CHECK(mo_protocol_server_metrics(f.server).completed == completed);
    wait_listener = NULL;
    mo_buffer_free(&input);
    finish(&f);
}

enum mo_store_activation_status
mo_protocol_server_fault_activate(struct mo_store *store, const struct mo_store_activation *a);
enum mo_store_activation_status
mo_protocol_server_fault_activate(struct mo_store *store, const struct mo_store_activation *a) {
    if (!activation_faulted)
        return mo_store_activate(store, a);
    return activation_result;
}

static void write_bytes(const char *path, const uint8_t *bytes, size_t length) {
    FILE *file = fopen(path, "wb");
    MO_ASSERT(file != NULL && fwrite(bytes, 1, length, file) == length && fclose(file) == 0);
}

static void read_bytes(const char *path, struct mo_buffer *out) {
    FILE *file = fopen(path, "rb");
    MO_ASSERT(file != NULL);
    uint8_t chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, file)) != 0)
        MO_ASSERT(mo_buffer_append(out, chunk, n));
    MO_ASSERT(fclose(file) == 0);
}

static void one_file_bundle(const char *text, struct mo_buffer *out) {
    MO_ASSERT(mo_buffer_append(out, "MOSB\1", 5) && mo_binary_write(out, 7, 4) &&
              mo_buffer_append(out, "main.mo", 7) && mo_binary_write(out, 1, 4) &&
              mo_binary_write(out, 7, 4) && mo_buffer_append(out, "main.mo", 7) &&
              mo_binary_write(out, strlen(text), 4) && mo_buffer_append(out, text, strlen(text)));
}

static void base64(const uint8_t *in, size_t length, char *out) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t at = 0;
    for (size_t i = 0; i < length; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < length ? (uint32_t)in[i + 1] << 8 : 0) |
                     (i + 2 < length ? in[i + 2] : 0);
        out[at++] = alphabet[v >> 18 & 63];
        out[at++] = alphabet[v >> 12 & 63];
        out[at++] = i + 1 < length ? alphabet[v >> 6 & 63] : '=';
        out[at++] = i + 2 < length ? alphabet[v & 63] : '=';
    }
    out[at] = 0;
}

/* A request whose reply may wait on the compiler child, so the poll runs in real time. */
static void slow_call(struct fixture_server *f, const char *tool, const char *arguments, char *out,
                      size_t capacity) {
    static char body[16384];
    int n = snprintf(body, sizeof body,
                     "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{"
                     "\"name\":\"%s\",\"arguments\":%s}}",
                     tool, arguments);
    MO_ASSERT(n > 0 && (size_t)n < sizeof body);
    struct mo_buffer input;
    input_request(&input, body, upgrade_auth);
    struct mo_listener_fake_peer peer = {.input = input.bytes,
                                         .input_size = input.length,
                                         .output = (uint8_t *)out,
                                         .output_capacity = capacity - 1,
                                         .readable = true,
                                         .writable = true,
                                         .eof = true};
    MO_ASSERT(mo_listener_fake_connect(f->listener, &peer) == MO_LISTENER_OK);
    for (unsigned i = 0; i < 6000 && !peer.closed; i++) {
        poll_server(f);
        struct timespec pause = {0, 5000000};
        (void)nanosleep(&pause, NULL);
    }
    CHECK(peer.closed);
    out[peer.output_size] = 0;
    mo_buffer_free(&input);
}

/* The server's side of an activation whose transaction is refused or whose outcome is unknown:
 * a rollback keeps the old program serving, an uncertain commit stops it with the store status. */
static void activation_outcomes(void) {
    mo_runtime_identity_init();
    char exe[4096];
    ssize_t length = readlink("/proc/self/exe", exe, sizeof exe - 8);
    MO_ASSERT(length > 0);
    exe[length] = 0;
    char *slash = strrchr(exe, '/');
    MO_ASSERT(slash != NULL);
    strcpy(slash, "/../mo");
    char dir[] = "/tmp/mo-activation-XXXXXX", path[256], image_path[300], command[8192];
    MO_ASSERT(mkdtemp(dir) != NULL);
    static const char next_text[] = "module Main\nservice\n state Int = 0\n tools read\n"
                                    " token from env \"MO_TEST_MCP_TOKEN\"\nend\n"
                                    "fn read(state: Int, _now: Time) : Text\n"
                                    " \"v2:#{state}\"\nend\n";
    struct mo_buffer old_bundle, next_bundle, wire, context;
    mo_buffer_init(&old_bundle, MO_LIMIT_BUNDLE_BYTES);
    mo_buffer_init(&next_bundle, MO_LIMIT_BUNDLE_BYTES);
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    mo_buffer_init(&context, MO_LIMIT_BUNDLE_BYTES);
    one_file_bundle(source_text_for_upgrade, &old_bundle);
    one_file_bundle(next_text, &next_bundle);
    MO_ASSERT(snprintf(path, sizeof path, "%s/old.mosb", dir) > 0);
    MO_ASSERT(snprintf(image_path, sizeof image_path, "%s/old.bc", dir) > 0);
    write_bytes(path, old_bundle.bytes, old_bundle.length);
    /* The recorded program and its context come from the same compiler child live upgrades use. */
    MO_ASSERT(snprintf(command, sizeof command, "'%s' emit '%s' -o '%s' --bundle", exe, path,
                       image_path) > 0);
    MO_ASSERT(system(command) == 0);
    read_bytes(image_path, &wire);
    MO_ASSERT(snprintf(command, sizeof command, "%s.mocc", image_path) > 0);
    read_bytes(command, &context);
    for (unsigned mode = 0; mode < 3; mode++) {
        struct mo_load_error error;
        struct fixture_server f = {.image = mo_load(wire.bytes, wire.length, &error)};
        MO_ASSERT(f.image != NULL);
        struct mo_limits limits = mo_limits_default();
        struct mo_defect defect;
        MO_ASSERT(mo_image_init(f.image, &limits, &defect) == MO_RETURNED);
        CHECK(setenv("MO_TEST_MCP_TOKEN", "McpSecretCanary42", 1) == 0);
        MO_ASSERT(mo_listener_fake("MO_TEST_MCP_TOKEN", &f.listener) == MO_LISTENER_OK);
        MO_ASSERT(mo_store_open(NULL, &f.store) == MO_STORE_OK);
        struct mo_limits service = mo_service_limits();
        struct mo_process *process = mo_process_new(f.image, &service);
        MO_ASSERT(process != NULL);
        uint32_t state_type = mo_image_bytecode(f.image)->services[0].state_type;
        struct mo_buffer saved;
        mo_buffer_init(&saved, MO_LIMIT_SAVED_BYTES);
        MO_ASSERT(mo_value_encode(mo_process_heap(process), state_type, mo_cell_int(4), &saved));
        struct mo_store_program program = {.image = mo_image_key(f.image, 0),
                                           .bundle = old_bundle.bytes,
                                           .bundle_size = old_bundle.length,
                                           .context = context.bytes,
                                           .context_size = context.length,
                                           .items = no_items,
                                           .items_size = sizeof no_items};
        MO_ASSERT(mo_type_schema_digest(mo_process_heap(process), state_type, program.schema));
        mo_process_free(process);
        struct mo_sha256 hash;
        mo_sha256_init(&hash);
        mo_sha256_update(&hash, old_bundle.bytes, old_bundle.length);
        mo_sha256_finish(&hash, program.source);
        mo_sha256_init(&hash);
        mo_sha256_update(&hash, context.bytes, context.length);
        mo_sha256_finish(&hash, program.context_digest);
        MO_ASSERT(mo_runtime_identity(program.build));
        struct mo_store_record first = {MO_STORE_STATE, 0, program.image, saved.bytes,
                                        saved.length};
        MO_ASSERT(mo_store_program_record(f.store, &program, &first) == MO_STORE_OK);
        mo_buffer_free(&saved);
        struct event_log log = {0};
        const char *authorities[] = {"localhost"};
        const struct mo_protocol_events events = {note_event, &log};
        MO_ASSERT(mo_protocol_server_new(f.image, 0, f.store, f.listener, authorities, 1, &events,
                                         &f.server) == MO_PROTOCOL_SERVER_OK);
        struct mo_bearer bearer = {.length = 23};
        memcpy(bearer.bytes, "UpgradeCanary0123456789", 23);
        CHECK(mo_protocol_server_upgrades(f.server, &bearer, NULL, exe) == MO_PROTOCOL_SERVER_OK);
        char base[65], candidate[65], arguments[16384], out[32768];
        hex64(program.source, base);
        uint8_t digest[32];
        mo_sha256_init(&hash);
        mo_sha256_update(&hash, next_bundle.bytes, next_bundle.length);
        mo_sha256_finish(&hash, digest);
        hex64(digest, candidate);
        int n = snprintf(arguments, sizeof arguments,
                         "{\"base\":\"%s\",\"candidate\":\"%s\",\"total\":%zu}", base, candidate,
                         next_bundle.length);
        MO_ASSERT(n > 0 && (size_t)n < sizeof arguments);
        upgrade_call(&f, upgrade_auth, "mo-upgrade-open", arguments, out, sizeof out);
        const char *slot_at = strstr(out, "\\\"slot\\\":\\\"");
        MO_ASSERT(slot_at != NULL);
        char slot[33] = {0}, text[MO_LIMIT_UPGRADE_PAGE_TEXT + 1];
        memcpy(slot, slot_at + strlen("\\\"slot\\\":\\\""), 32);
        base64(next_bundle.bytes, next_bundle.length, text);
        n = snprintf(arguments, sizeof arguments,
                     "{\"slot\":\"%s\",\"index\":0,\"offset\":0,\"total\":%zu,\"bytes\":\"%s\"}",
                     slot, next_bundle.length, text);
        MO_ASSERT(n > 0 && (size_t)n < sizeof arguments);
        upgrade_call(&f, upgrade_auth, "mo-upgrade-page", arguments, out, sizeof out);
        CHECK(strstr(out, "\\\"complete\\\":true") != NULL);
        activation_faulted = mode != 0;
        activation_result = mode == 1 ? MO_STORE_ACTIVATION_REFUSED : MO_STORE_ACTIVATION_UNSAFE;
        n = snprintf(arguments, sizeof arguments, "{\"slot\":\"%s\",\"base\":\"%s\"}", slot, base);
        MO_ASSERT(n > 0 && (size_t)n < sizeof arguments);
        slow_call(&f, "mo-upgrade-commit", arguments, out, sizeof out);
        activation_faulted = false;
        bool stopped = false;
        for (size_t i = 0; i < log.count; i++)
            stopped |=
                log.events[i].kind == MO_SERVICE_STOP && log.events[i].reason == MO_SERVICE_STORE;
        if (mode == 0) {
            CHECK(strstr(out, "\"status\":\"accepted\"") != NULL && !stopped);
            tool_request(&f, "read", "{}", "6", out, sizeof out);
            CHECK(strstr(out, "v2:4") != NULL);
        } else if (mode == 1) {
            CHECK(strstr(out, "\"code\":\"E1425\"") != NULL && strstr(out, "rolled back") != NULL);
            CHECK(!stopped);
            tool_request(&f, "read", "{}", "6", out, sizeof out);
            CHECK(strstr(out, "\"value\":\"4\"") != NULL);
        } else {
            CHECK(strstr(out, "\"code\":\"E1425\"") != NULL &&
                  strstr(out, "outcome is unknown") != NULL);
            CHECK(stopped);
            tool_request(&f, "read", "{}", "6", out, sizeof out);
            tool_error(out, "stopped", "none", "6");
        }
        finish(&f);
    }
    (void)unlink(path);
    (void)unlink(image_path);
    MO_ASSERT(snprintf(command, sizeof command, "%s.mocc", image_path) > 0);
    (void)unlink(command);
    (void)rmdir(dir);
    mo_buffer_free(&old_bundle);
    mo_buffer_free(&next_bundle);
    mo_buffer_free(&wire);
    mo_buffer_free(&context);
}

static void run_faults(void *context) {
    wire(context);
    poll_failures();
    reserved_fallback();
    accept_allocation_failure();
    preauth_failure();
    exhausted_fallback();
    saved_copy_memory();
    store_failure();
    store_stop_event();
    activation_outcomes();
}

int main(void) {
    MO_ASSERT(mo_with_stack(MO_SOURCE_STACK_SIZE, run_faults, NULL) == 0);
    return mo_test_report("protocol_server_faults");
}
