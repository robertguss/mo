#define MO_PROTOCOL_SERVER_FAULT_TEST
#include "test_protocol_server.c"

#include <errno.h>

static struct mo_protocol_server *fault_server;
static uint64_t admitted_before;
static bool exhaust_reply, record_resizes, fail_append;
static unsigned exhausted_calls, memory_stops, decode_after, decode_failures, append_failures;
static unsigned fail_call_after;
static bool scan_limit_zero;
static size_t resize_calls, resize_after, resize_failures, resize_sizes[4096];

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
        CHECK(mo_protocol_server_new(f.image, 0, f.store, f.listener, authorities, 1, &f.server) ==
              MO_PROTOCOL_SERVER_MEMORY);
        CHECK(f.server == NULL);
        CHECK(resize_after == 0);
    }
    CHECK(resize_failures == 2);
    MO_ASSERT(mo_protocol_server_new(f.image, 0, f.store, f.listener, authorities, 1, &f.server) ==
              MO_PROTOCOL_SERVER_OK);
    char out[4096];
    tool_request(&f, "read", "{}", "1", out, sizeof out);
    CHECK(strstr(out, "\"structuredContent\":{\"value\":\"0\"}") != NULL);
    finish(&f);
}

static void exhausted_fallback(void) {
    struct fixture_server f = start();
    char out[4096], id[6 * MO_LIMIT_MCP_ID_BYTES + 3];
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
    MO_ASSERT(mo_protocol_server_new(f.image, 0, f.store, f.listener, authorities, 1, &f.server) ==
              MO_PROTOCOL_SERVER_OK);
    tool_request(&f, "read", "{}", "5", out, sizeof out);
    CHECK(strstr(out, "\"structuredContent\":{\"value\":\"7\"}") != NULL);
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

static void run_faults(void *context) {
    wire(context);
    poll_failures();
    reserved_fallback();
    preauth_failure();
    exhausted_fallback();
    saved_copy_memory();
    store_failure();
}

int main(void) {
    MO_ASSERT(mo_with_stack(MO_SOURCE_STACK_SIZE, run_faults, NULL) == 0);
    return mo_test_report("protocol_server_faults");
}
