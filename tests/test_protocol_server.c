#define _POSIX_C_SOURCE 200809L
#include "base/runtime-identity.h"
#include "base/stack.h"
#include "service_compile.h"
#include "test.h"
#include "tools/protocol.h"

#include <stdlib.h>
#include <string.h>

static const char source[] =
    "module Main\n"
    "service\n state Int = 0\n tools add, read, reject, fail, large, grow, work\n"
    " token from env \"MO_TEST_MCP_TOKEN\"\nend\n"
    "fn add(state: Int, _now: Time, amount: Int) : (Int, Int)\n"
    " (state + amount, state + amount)\nend\n"
    "fn read(state: Int, _now: Time) : Text\n state.to_text\nend\n"
    "fn reject(_state: Int, _now: Time) : Result(Text, Text)\n Error(\"denied\")\nend\n"
    "fn fail(_state: Int, _now: Time) : Text\n stop \"never render this secret\"\nend\n"
    "fn large(_state: Int, _now: Time) : Text\n \"x\".repeat(32768)\nend\n"
    "fn grow(state: Int, _now: Time) : (Int, Text)\n"
    " (state + 1, \"x\".repeat(32768))\nend\n"
    "fn work(state: Int, now: Time) : (Int, Int)\n work(state + 1, now)\nend\n";

static const char auth[] = "Authorization: Bearer McpSecretCanary42\r\n";

struct fixture_server {
    struct mo_image *image;
    struct mo_store *store;
    struct mo_listener *listener;
    struct mo_protocol_server *server;
    uint64_t tick_us;
};

struct event_log {
    struct mo_service_event events[32];
    size_t count;
    struct mo_protocol_server **server;
    bool connected;
};

static void note_event(const struct mo_service_event *event, void *context) {
    struct event_log *log = context;
    MO_ASSERT(log->count < sizeof log->events / sizeof *log->events);
    log->events[log->count++] = *event;
    log->connected = mo_protocol_server_connected(*log->server);
}

static struct fixture_server start_events(const char *text, struct event_log *log) {
    struct fixture_server f = {.image = compile(text)};
    CHECK(setenv("MO_TEST_MCP_TOKEN", "McpSecretCanary42", 1) == 0);
    MO_ASSERT(mo_listener_fake("MO_TEST_MCP_TOKEN", &f.listener) == MO_LISTENER_OK);
    MO_ASSERT(mo_store_open(NULL, &f.store) == MO_STORE_OK);
    const char *authorities[] = {"localhost"};
    const struct mo_protocol_events events = {note_event, log};
    MO_ASSERT(mo_protocol_server_new(f.image, 0, f.store, f.listener, authorities, 1,
                                     log == NULL ? NULL : &events,
                                     &f.server) == MO_PROTOCOL_SERVER_OK);
    return f;
}

static struct fixture_server start_source(const char *text) {
    return start_events(text, NULL);
}

static struct fixture_server start(void) {
    return start_source(source);
}

static void finish(struct fixture_server *f) {
    mo_protocol_server_free(f->server);
    mo_listener_close(f->listener);
    mo_store_close(f->store);
    mo_image_free(f->image);
}

static void input_request(struct mo_buffer *input, const char *body, const char *fields) {
    mo_buffer_init(input, MO_LIMIT_MCP_INGRESS_BYTES);
    MO_ASSERT(mo_buffer_append_text(input, "POST /mcp HTTP/1.1\r\nHost: localhost\r\n"
                                           "Content-Type: application/json\r\n"
                                           "MCP-Protocol-Version: 2025-11-25\r\nContent-Length: "));
    MO_ASSERT(mo_buffer_append_uint(input, strlen(body)) && mo_buffer_append_text(input, "\r\n") &&
              mo_buffer_append_text(input, fields) && mo_buffer_append_text(input, "\r\n") &&
              mo_buffer_append_text(input, body));
}

static void poll_server(struct fixture_server *f) {
    if (f->tick_us != 0)
        MO_ASSERT(mo_listener_fake_advance(f->listener, f->tick_us) == MO_LISTENER_OK);
    CHECK(mo_protocol_server_poll(f->server, 0) == MO_PROTOCOL_SERVER_OK);
}

static void check_wire(const char *out) {
    CHECK(strstr(out, "McpSecretCanary42") == NULL);
    CHECK(strstr(out, "never render this secret") == NULL);
    CHECK(strstr(out, "Connection: close\r\n") != NULL);
    CHECK(strstr(out, "Cache-Control: no-store\r\n") != NULL);
    const char *length = strstr(out, "Content-Length: ");
    const char *body = strstr(out, "\r\n\r\n");
    CHECK(length != NULL && body != NULL);
    if (length != NULL && body != NULL)
        CHECK(strtoull(length + strlen("Content-Length: "), NULL, 10) == strlen(body + 4));
}

static void request(struct fixture_server *f, const char *body, const char *fields, char *out,
                    size_t capacity, size_t quantum) {
    struct mo_buffer input;
    input_request(&input, body, fields);
    struct mo_listener_fake_peer peer = {.input = input.bytes,
                                         .input_size = input.length,
                                         .output = (uint8_t *)out,
                                         .output_capacity = capacity - 1,
                                         .read_quantum = quantum,
                                         .write_quantum = quantum,
                                         .readable = true,
                                         .writable = true,
                                         .eof = true};
    MO_ASSERT(mo_listener_fake_connect(f->listener, &peer) == MO_LISTENER_OK);
    for (size_t i = 0; i < 2 * (input.length + capacity) && !peer.closed; i++)
        poll_server(f);
    CHECK(peer.closed && peer.half_closed);
    out[peer.output_size] = '\0';
    check_wire(out);
    mo_buffer_free(&input);
}

static void tool_request(struct fixture_server *f, const char *name, const char *arguments,
                         const char *id, char *out, size_t capacity) {
    char body[2048];
    int size = snprintf(body, sizeof body,
                        "{\"jsonrpc\":\"2.0\",\"id\":%s,\"method\":\"tools/call\","
                        "\"params\":{\"name\":\"%s\",\"arguments\":%s}}",
                        id, name, arguments);
    MO_ASSERT(size > 0 && (size_t)size < sizeof body);
    request(f, body, auth, out, capacity, 0);
}

static void tool_error(const char *out, const char *code, const char *effect, const char *id) {
    char expected[1024];
    CHECK(strstr(out, "HTTP/1.1 200 ") == out);
    CHECK(strstr(out, "\"isError\":true") != NULL);
    CHECK(strstr(out, "\"kind\":\"runtime\"") != NULL);
    int size = snprintf(expected, sizeof expected, "\"code\":\"%s\"", code);
    MO_ASSERT(size > 0 && (size_t)size < sizeof expected);
    CHECK(strstr(out, expected) != NULL);
    size = snprintf(expected, sizeof expected, "\"effect\":\"%s\"", effect);
    MO_ASSERT(size > 0 && (size_t)size < sizeof expected);
    CHECK(strstr(out, expected) != NULL);
    size = snprintf(expected, sizeof expected, "\"id\":%s", id);
    MO_ASSERT(size > 0 && (size_t)size < sizeof expected);
    CHECK(strstr(out, expected) != NULL);
}

static void maximum_id(char *out) {
    out[0] = '"';
    for (size_t i = 0; i < 128; i++)
        memcpy(out + 1 + 6 * i, "\\u0001", 6);
    out[1 + 6 * 128] = '"';
    out[2 + 6 * 128] = '\0';
}

static void empty_continue(struct fixture_server *f) {
    struct mo_buffer input;
    input_request(&input, "",
                  "Authorization: Bearer McpSecretCanary42\r\nExpect: 100-continue\r\n");
    char out[4096];
    struct mo_listener_fake_peer peer = {.input = input.bytes,
                                         .input_size = input.length,
                                         .output = (uint8_t *)out,
                                         .output_capacity = sizeof out - 1,
                                         .readable = true,
                                         .writable = true};
    uint64_t admitted = mo_protocol_server_metrics(f->server).admitted;
    MO_ASSERT(mo_listener_fake_connect(f->listener, &peer) == MO_LISTENER_OK);
    for (unsigned i = 0; i < 8 && !peer.half_closed; i++)
        poll_server(f);
    CHECK(peer.half_closed);
    out[peer.output_size] = '\0';
    const char interim[] = "HTTP/1.1 100 Continue\r\n\r\n";
    CHECK(strncmp(out, interim, strlen(interim)) == 0);
    const char *final = peer.output_size >= strlen(interim) ? out + strlen(interim) : out;
    CHECK(strstr(final, "HTTP/1.1 400 ") == final);
    CHECK(strstr(final, "\"code\":-32700") != NULL);
    CHECK(strstr(final, "\"message\":\"Parse error\"") != NULL);
    check_wire(final);
    CHECK(mo_protocol_server_metrics(f->server).admitted == admitted);
    peer.eof = true;
    for (unsigned i = 0; i < 8 && !peer.closed; i++)
        poll_server(f);
    CHECK(peer.closed);
    mo_buffer_free(&input);
}

static void wire(void *context) {
    (void)context;
    struct fixture_server f = start();
    char output[16384];
    request(&f,
            "{\"jsonrpc\":\"2.0\",\"id\":-0,\"method\":\"initialize\",\"params\":{"
            "\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{"
            "\"name\":\"test\",\"version\":\"1\"}}}",
            auth, output, sizeof output, 1);
    CHECK(strstr(output, "HTTP/1.1 200 ") == output);
    CHECK(strstr(output, "\"id\":-0") != NULL);
    CHECK(strstr(output, "\"protocolVersion\":\"2025-11-25\"") != NULL);
    CHECK(strstr(output, "\r\nMCP-Session-Id: ") != NULL);
    request(&f, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}", auth, output,
            sizeof output, 7);
    CHECK(strstr(output, "\"inputSchema\"") != NULL);
    CHECK(strstr(output, "\"name\":\"add\"") != NULL);
    request(&f,
            "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{"
            "\"name\":\"add\",\"arguments\":{\"amount\":7}}}",
            auth, output, sizeof output, 3);
    CHECK(strstr(output, "\"code\":\"invalid_arguments\"") != NULL);
    request(&f,
            "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{"
            "\"name\":\"add\",\"arguments\":{\"amount\":\"7\"}}}",
            auth, output, sizeof output, 3);
    CHECK(strstr(output, "\"structuredContent\":{\"value\":\"7\"}") != NULL);
    request(&f,
            "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{"
            "\"name\":\"read\"}}",
            auth, output, sizeof output, 0);
    CHECK(strstr(output, "\"structuredContent\":{\"value\":\"7\"}") != NULL);
    uint64_t admitted = mo_protocol_server_metrics(f.server).admitted;
    request(&f, "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"params\":[1]}", auth, output,
            sizeof output, 1);
    CHECK(strstr(output, "HTTP/1.1 202 ") == output);
    CHECK(strstr(output, "Content-Length: 0\r\n") != NULL);
    CHECK(mo_protocol_server_metrics(f.server).admitted == admitted);
    request(&f, "not JSON", "", output, sizeof output, 1);
    CHECK(strstr(output, "HTTP/1.1 401 ") == output);
    CHECK(strstr(output, "WWW-Authenticate: Bearer\r\n") != NULL);
    CHECK(strstr(output, "Parse error") == NULL);
    request(&f,
            "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{"
            "\"name\":\"reject\"}}",
            auth, output, sizeof output, 0);
    CHECK(strstr(output, "\"kind\":\"application\"") != NULL);
    CHECK(strstr(output, "\"value\":\"denied\"") != NULL);
    request(&f,
            "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{"
            "\"name\":\"fail\"}}",
            auth, output, sizeof output, 0);
    CHECK(strstr(output, "\"code\":\"handler_failed\"") != NULL);
    CHECK(strstr(output, "\"effect\":\"none\"") != NULL);
    tool_request(&f, "work", "{}", "6", output, sizeof output);
    tool_error(output, "work_limit", "none", "6");
    tool_request(&f, "read", "{}", "7", output, sizeof output);
    CHECK(strstr(output, "\"structuredContent\":{\"value\":\"7\"}") != NULL);
    empty_continue(&f);
    CHECK(mo_protocol_server_metrics(f.server).request_memory <= MO_LIMIT_MCP_REQUEST_MEMORY);
    finish(&f);
}

#ifndef MO_PROTOCOL_SERVER_FAULT_TEST
static void rate(void) {
    struct fixture_server f = start();
    char out[4096];
    for (unsigned i = 0; i < 45; i++) {
        tool_request(&f, "unknown", "{}", "1", out, sizeof out);
        CHECK(strstr(out, "\"code\":-32602") != NULL);
        request(&f, "{", auth, out, sizeof out, 0);
        CHECK(strstr(out, "\"code\":-32700") != NULL);
        request(&f, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":[]}", auth,
                out, sizeof out, 0);
        CHECK(strstr(out, "\"code\":-32602") != NULL);
    }
    CHECK(mo_protocol_server_metrics(f.server).admitted == 0);
    for (unsigned i = 0; i < 40; i++) {
        if (i % 2 == 0) {
            tool_request(&f, "add", "{\"amount\":7}", "1", out, sizeof out);
            tool_error(out, "invalid_arguments", "none", "1");
        } else {
            tool_request(&f, "read", "{}", "1", out, sizeof out);
            CHECK(strstr(out, "\"structuredContent\":{\"value\":\"0\"}") != NULL);
        }
    }
    CHECK(mo_protocol_server_metrics(f.server).admitted == 20);
    tool_request(&f, "read", "{}", "2", out, sizeof out);
    tool_error(out, "rate_limited", "none", "2");
    tool_request(&f, "mo-state", "{}", "2", out, sizeof out);
    tool_error(out, "rate_limited", "none", "2");
    tool_request(&f, "mo-export", "{\"kind\":\"state\"}", "2", out, sizeof out);
    tool_error(out, "rate_limited", "none", "2");
    MO_ASSERT(mo_listener_fake_advance(f.listener, 49999) == MO_LISTENER_OK);
    tool_request(&f, "read", "{}", "3", out, sizeof out);
    tool_error(out, "rate_limited", "none", "3");
    MO_ASSERT(mo_listener_fake_advance(f.listener, 1) == MO_LISTENER_OK);
    tool_request(&f, "read", "{}", "4", out, sizeof out);
    CHECK(strstr(out, "\"structuredContent\":{\"value\":\"0\"}") != NULL);
    tool_request(&f, "read", "{}", "5", out, sizeof out);
    tool_error(out, "rate_limited", "none", "5");
    CHECK(mo_protocol_server_metrics(f.server).admitted == 21);
    finish(&f);
}

static void stopped(void) {
    struct fixture_server f = start();
    char out[4096];
    for (unsigned i = 0; i < 40; i++) {
        tool_request(&f, "add", "{\"amount\":7}", "1", out, sizeof out);
        tool_error(out, "invalid_arguments", "none", "1");
    }
    for (unsigned i = 0; i < 15; i++) {
        MO_ASSERT(mo_listener_fake_advance(f.listener, 100000) == MO_LISTENER_OK);
        tool_request(&f, "fail", "{}", "1", out, sizeof out);
        tool_error(out, "handler_failed", "none", "1");
        tool_request(&f, "read", "{}", "2", out, sizeof out);
        if (i == 14)
            tool_error(out, "stopped", "none", "2");
        else
            CHECK(strstr(out, "\"structuredContent\":{\"value\":\"0\"}") != NULL);
    }
    CHECK(mo_protocol_server_metrics(f.server).admitted == 29);
    finish(&f);
}

static void events_reach_the_sink(void) {
    struct event_log log = {.count = 0};
    struct fixture_server f = start_events(source, &log);
    log.server = &f.server;
    char out[4096];
    CHECK(log.count == 0);
    for (unsigned i = 0; i < 15; i++) {
        MO_ASSERT(mo_listener_fake_advance(f.listener, 100000) == MO_LISTENER_OK);
        tool_request(&f, "fail", "{}", "1", out, sizeof out);
        tool_error(out, "handler_failed", "none", "1");
    }
    static const enum mo_service_event_kind expected[] = {
        MO_SERVICE_FAILURE, MO_SERVICE_FAILURE, MO_SERVICE_FAILURE, MO_SERVICE_FAILURE,
        MO_SERVICE_FAILURE, MO_SERVICE_RESTART, MO_SERVICE_FAILURE, MO_SERVICE_FAILURE,
        MO_SERVICE_FAILURE, MO_SERVICE_FAILURE, MO_SERVICE_FAILURE, MO_SERVICE_RESTART,
        MO_SERVICE_FAILURE, MO_SERVICE_FAILURE, MO_SERVICE_FAILURE, MO_SERVICE_FAILURE,
        MO_SERVICE_FAILURE, MO_SERVICE_RESTART, MO_SERVICE_STOP};
    static const uint32_t restarts[sizeof expected / sizeof *expected] = {
        [5] = 1, [11] = 2, [17] = 3};
    CHECK(log.count == sizeof expected / sizeof *expected);
    for (size_t i = 0; i < log.count && i < sizeof expected / sizeof *expected; i++) {
        CHECK(log.events[i].kind == expected[i]);
        CHECK(log.events[i].reason == MO_SERVICE_DEFECT);
        CHECK((log.events[i].defect != NULL) == (expected[i] == MO_SERVICE_FAILURE));
        CHECK(expected[i] == MO_SERVICE_FAILURE ? named(log.events[i].handler, "fail")
                                                : log.events[i].handler.bytes == NULL);
        CHECK(log.events[i].restart == restarts[i]);
    }
    finish(&f);
}

static void connected(void) {
    struct event_log log = {.count = 0};
    struct fixture_server f = start_events(source, &log);
    log.server = &f.server;
    CHECK(!mo_protocol_server_connected(f.server));
    struct mo_buffer input;
    input_request(&input,
                  "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                  "\"params\":{\"name\":\"fail\"}}",
                  auth);
    char out[4096];
    struct mo_listener_fake_peer peer = {.input = input.bytes,
                                         .input_size = input.length - 1,
                                         .output = (uint8_t *)out,
                                         .output_capacity = sizeof out,
                                         .readable = true};
    MO_ASSERT(mo_listener_fake_connect(f.listener, &peer) == MO_LISTENER_OK);
    for (unsigned i = 0; i < 8; i++)
        poll_server(&f);
    CHECK(peer.input_offset == input.length - 1 && log.count == 0);
    CHECK(mo_protocol_server_connected(f.server));
    mo_protocol_server_stop_accepting(f.server);
    char late_out[64];
    struct mo_listener_fake_peer late = {.output = (uint8_t *)late_out,
                                         .output_capacity = sizeof late_out,
                                         .readable = true,
                                         .writable = true};
    MO_ASSERT(mo_listener_fake_connect(f.listener, &late) == MO_LISTENER_OK);
    peer.input_size = input.length;
    for (unsigned i = 0; i < 8; i++)
        poll_server(&f);
    CHECK(log.count == 1 && log.connected);
    CHECK(peer.output_size == 0 && mo_protocol_server_connected(f.server));
    peer.writable = true;
    for (unsigned i = 0; i < 8 && !peer.half_closed; i++)
        poll_server(&f);
    CHECK(peer.half_closed && !peer.closed);
    CHECK(mo_protocol_server_connected(f.server));
    peer.eof = true;
    for (unsigned i = 0; i < 8 && !peer.closed; i++)
        poll_server(&f);
    CHECK(peer.closed && !mo_protocol_server_connected(f.server));
    CHECK(!late.closed && late.output_size == 0);
    mo_buffer_free(&input);
    finish(&f);
}

static void response_limit(void) {
    struct fixture_server f = start();
    char out[4096], id[6 * 128 + 3];
    maximum_id(id);
    tool_request(&f, "large", "{}", id, out, sizeof out);
    tool_error(out, "response_limit", "none", id);
    tool_request(&f, "read", "{}", "2", out, sizeof out);
    CHECK(strstr(out, "\"structuredContent\":{\"value\":\"0\"}") != NULL);
    tool_request(&f, "grow", "{}", id, out, sizeof out);
    tool_error(out, "response_limit", "applied", id);
    tool_request(&f, "read", "{}", "3", out, sizeof out);
    CHECK(strstr(out, "\"structuredContent\":{\"value\":\"1\"}") != NULL);
    CHECK(mo_protocol_server_metrics(f.server).admitted == 4);
    finish(&f);
}

static void busy(void) {
    struct mo_buffer text;
    mo_buffer_init(&text, MO_LIMIT_FILE_SIZE);
    MO_ASSERT(mo_buffer_append_text(&text,
                                    "module Main\nstruct Secret\n value: Int\nend\nservice\nstate "
                                    "Secret = Secret(value: 0)\ntools read\n"));
    for (unsigned i = 0; i <= MO_LIMIT_MCP_VIEW_TIMERS; i++) {
        MO_ASSERT(mo_buffer_append_text(&text, "every 1.second tick_") &&
                  mo_buffer_append_uint(&text, i) && mo_buffer_append_text(&text, "\n"));
    }
    MO_ASSERT(mo_buffer_append_text(
        &text, "token from env \"MO_TEST_MCP_TOKEN\"\nend\n"
               "fn read(state: Secret, _now: Time) : Text\nstate.value.to_text\nend\n"));
    for (unsigned i = 0; i <= MO_LIMIT_MCP_VIEW_TIMERS; i++) {
        MO_ASSERT(
            mo_buffer_append_text(&text, "fn tick_") && mo_buffer_append_uint(&text, i) &&
            mo_buffer_append_text(
                &text,
                "(state: Secret, _now: Time) : Secret\nSecret(value: state.value + 1)\nend\n"));
    }
    MO_ASSERT(mo_buffer_append_byte(&text, 0));
    struct fixture_server f = start_source((const char *)text.bytes);
    struct mo_buffer input;
    input_request(&input,
                  "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                  "\"params\":{\"name\":\"read\"}}",
                  auth);
    char out[8][4096];
    struct mo_listener_fake_peer peers[8];
    for (size_t i = 0; i < 8; i++) {
        peers[i] = (struct mo_listener_fake_peer){.input = input.bytes,
                                                  .input_size = input.length,
                                                  .output = (uint8_t *)out[i],
                                                  .output_capacity = sizeof out[i] - 1,
                                                  .readable = true,
                                                  .writable = true,
                                                  .eof = true};
        MO_ASSERT(mo_listener_fake_connect(f.listener, &peers[i]) == MO_LISTENER_OK);
    }
    f.tick_us = 1000000;
    for (unsigned i = 0; i < 32; i++)
        poll_server(&f);
    for (size_t i = 0; i < 8; i++) {
        CHECK(peers[i].closed && peers[i].half_closed);
        out[i][peers[i].output_size] = '\0';
        check_wire(out[i]);
        tool_error(out[i], "busy", "none", "1");
    }
    CHECK(mo_protocol_server_metrics(f.server).admitted == 0);
    CHECK(mo_protocol_server_metrics(f.server).completed == 8);
    char view[32768];
    tool_request(&f, "mo-mailbox", "{}", "1", view, sizeof view);
    CHECK(strstr(view, "\"complete\":false") != NULL);
    CHECK(strstr(view, "\"truncated\":") != NULL);
    CHECK(strstr(view, "\"isError\":false") != NULL);
    CHECK(mo_protocol_server_metrics(f.server).admitted == 0);
    tool_request(&f, "mo-timers", "{}", "1", view, sizeof view);
    CHECK(strstr(view, "\"complete\":false") != NULL);
    size_t included = 0;
    const char *entry = view;
    while ((entry = strstr(entry, "\"interval_us\":")) != NULL) {
        included++;
        entry++;
    }
    CHECK(included > 0 && included < MO_LIMIT_MCP_VIEW_TIMERS);
    char omitted[64];
    int written = snprintf(omitted, sizeof omitted, "\"truncated\":%zu",
                           (size_t)MO_LIMIT_MCP_VIEW_TIMERS + 1 - included);
    MO_ASSERT(written > 0 && (size_t)written < sizeof omitted);
    CHECK(strstr(view, omitted) != NULL);
    written = snprintf(omitted, sizeof omitted, "\"next\":{\"offset\":%zu}", included);
    MO_ASSERT(written > 0 && (size_t)written < sizeof omitted);
    CHECK(strstr(view, omitted) != NULL);
    char arguments[64];
    written = snprintf(arguments, sizeof arguments, "{\"offset\":%u}", MO_LIMIT_MCP_VIEW_TIMERS);
    MO_ASSERT(written > 0 && (size_t)written < sizeof arguments);
    tool_request(&f, "mo-timers", arguments, "1", view, sizeof view);
    written = snprintf(omitted, sizeof omitted, "\"name\":\"tick_%u\"", MO_LIMIT_MCP_VIEW_TIMERS);
    MO_ASSERT(written > 0 && (size_t)written < sizeof omitted);
    CHECK(strstr(view, omitted) != NULL);
    CHECK(strstr(view, "\"complete\":true") != NULL);
    tool_request(&f, "mo-state", "{\"path\":[\"value\"]}", "1", view, sizeof view);
    CHECK(strstr(view, "\"complete\":true") != NULL);
    CHECK(strstr(view, "\"state\":") != NULL);
    tool_request(&f, "mo-export", "{\"kind\":\"state\"}", "1", view, sizeof view);
    CHECK(strstr(view, "\"kind\":\"state\"") != NULL);
    CHECK(strstr(view, "\"base64\":") != NULL);
    CHECK(mo_protocol_server_metrics(f.server).admitted == 0);
    f.tick_us = 0;
    for (unsigned i = 0; i < 2 * (MO_LIMIT_MCP_VIEW_TIMERS + 1); i++)
        poll_server(&f);
    tool_request(&f, "read", "{}", "2", out[0], sizeof out[0]);
    CHECK(strstr(out[0], "\"structuredContent\":{\"value\":") != NULL);
    CHECK(mo_protocol_server_metrics(f.server).admitted == 1);
    mo_buffer_free(&input);
    mo_buffer_free(&text);
    finish(&f);
}

static void small_body_reads(void) {
    struct fixture_server f = start();
    char body[MO_LIMIT_MCP_BODY_BYTES + 1];
    const char *ping = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}";
    size_t size = strlen(ping);
    memcpy(body, ping, size);
    memset(body + size, ' ', MO_LIMIT_MCP_BODY_BYTES - size);
    body[MO_LIMIT_MCP_BODY_BYTES] = '\0';
    for (size_t quantum = 1; quantum <= 2; quantum++) {
        char out[4096];
        request(&f, body, auth, out, sizeof out, quantum);
        CHECK(strstr(out, "HTTP/1.1 200 ") == out);
        CHECK(strstr(out, "\"id\":1,\"result\":{}") != NULL);
        CHECK(mo_protocol_server_metrics(f.server).work <= UINT64_C(800000));
    }
    finish(&f);
}

static void raw_status(struct fixture_server *f, const char *input, int status) {
    char out[4096], expected[32];
    struct mo_listener_fake_peer peer = {.input = (const uint8_t *)input,
                                         .input_size = strlen(input),
                                         .output = (uint8_t *)out,
                                         .output_capacity = sizeof out - 1,
                                         .readable = true,
                                         .writable = true,
                                         .eof = true};
    MO_ASSERT(mo_listener_fake_connect(f->listener, &peer) == MO_LISTENER_OK);
    for (unsigned i = 0; i < 100 && !peer.closed; i++)
        poll_server(f);
    CHECK(peer.closed);
    out[peer.output_size] = 0;
    int n = snprintf(expected, sizeof expected, "HTTP/1.1 %d ", status);
    MO_ASSERT(n > 0 && (size_t)n < sizeof expected);
    CHECK(strstr(out, expected) == out);
    check_wire(out);
}

static void literal_boundaries(void) {
    CHECK(MO_LIMIT_MCP_ID_BYTES == 128);
    struct fixture_server f = start();
    char id[132], body[256], out[4096];
    for (size_t size = 128; size <= 129; size++) {
        memset(id, 'a', size);
        id[size] = 0;
        int n = snprintf(body, sizeof body,
                         "{\"jsonrpc\":\"2.0\",\"id\":\"%s\",\"method\":\"ping\"}", id);
        MO_ASSERT(n > 0 && (size_t)n < sizeof body);
        request(&f, body, auth, out, sizeof out, 0);
        CHECK(strstr(out, size == 128 ? "HTTP/1.1 200 " : "HTTP/1.1 400 ") == out);
        if (size == 128)
            CHECK(strstr(out, id) != NULL && strstr(out, "\"result\":{}") != NULL);
        else
            CHECK(strstr(out, "\"code\":-32600") != NULL);
    }
    for (size_t size = 16384; size <= 16385; size++) {
        struct mo_buffer input;
        mo_buffer_init(&input, MO_LIMIT_MCP_INGRESS_BYTES);
        MO_ASSERT(mo_buffer_append_text(
            &input, "POST /mcp HTTP/1.1\r\nHost: localhost\r\n"
                    "Content-Type: application/json\r\nTransfer-Encoding: chunked\r\n"
                    "MCP-Protocol-Version: 2025-11-25\r\n"));
        MO_ASSERT(mo_buffer_append_text(&input, auth));
        MO_ASSERT(mo_buffer_append_text(
            &input, "\r\n28\r\n{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}\r\n0\r\n"));
        const size_t lines[] = {6000, 6000, size - 12002};
        for (size_t i = 0; i < 3; i++) {
            MO_ASSERT(mo_buffer_append_text(&input, "X: "));
            for (size_t j = 0; j < lines[i] - 5; j++)
                MO_ASSERT(mo_buffer_append_byte(&input, 'a'));
            MO_ASSERT(mo_buffer_append_text(&input, "\r\n"));
        }
        MO_ASSERT(mo_buffer_append_text(&input, "\r\n") && mo_buffer_append_byte(&input, 0));
        raw_status(&f, (const char *)input.bytes, size == 16384 ? 200 : 413);
        mo_buffer_free(&input);
    }
    finish(&f);
}

static void service_hangup(void) {
    for (unsigned mode = 0; mode < 2; mode++) {
        struct fixture_server f = start();
        struct mo_buffer input;
        input_request(&input,
                      "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                      "\"params\":{\"name\":\"read\"}}",
                      auth);
        char out[8][4096];
        struct mo_listener_fake_peer peers[8];
        for (size_t i = 0; i < 8; i++) {
            peers[i] = (struct mo_listener_fake_peer){.input = input.bytes,
                                                      .input_size = input.length - 1,
                                                      .output = (uint8_t *)out[i],
                                                      .output_capacity = sizeof out[i] - 1,
                                                      .readable = true,
                                                      .writable = true};
            MO_ASSERT(mo_listener_fake_connect(f.listener, &peers[i]) == MO_LISTENER_OK);
        }
        for (unsigned n = 0; n < 16; n++)
            poll_server(&f);
        for (size_t i = 0; i < 8; i++) {
            CHECK(peers[i].input_offset == input.length - 1);
            peers[i].input_size = input.length;
        }
        poll_server(&f);
        CHECK(mo_protocol_server_metrics(f.server).admitted == 8);
        CHECK(mo_protocol_server_metrics(f.server).completed == 0);
        for (size_t i = 0; i < 8; i++) {
            CHECK(!peers[i].closed && peers[i].output_size == 0);
            peers[i].eof = true;
            peers[i].read_error = mode == 0;
            peers[i].write_error = mode == 0;
        }
        poll_server(&f);
        for (size_t i = 1; i < 8; i++)
            CHECK(!peers[i].closed && peers[i].output_size == 0);
        for (unsigned n = 0; n < 32; n++)
            poll_server(&f);
        for (size_t i = 0; i < 8; i++) {
            CHECK(peers[i].closed);
            out[i][peers[i].output_size] = 0;
            if (mode == 1) {
                CHECK(peers[i].half_closed);
                CHECK(strstr(out[i], "HTTP/1.1 200 ") == out[i]);
                CHECK(strstr(out[i], "\"structuredContent\":{\"value\":\"0\"}") != NULL);
                check_wire(out[i]);
            } else
                CHECK(peers[i].output_size == 0);
        }
        tool_request(&f, "read", "{}", "2", out[0], sizeof out[0]);
        CHECK(strstr(out[0], "\"structuredContent\":{\"value\":\"0\"}") != NULL);
        CHECK(mo_protocol_server_metrics(f.server).admitted == 9);
        mo_buffer_free(&input);
        finish(&f);
    }
}

static void rejection_paths(void) {
    struct fixture_server f = start();
    raw_status(&f, "POST /mcp HTTP/1.1\r\nBad Header: x\r\n\r\n", 400);
    raw_status(&f, "POST /mcp HTTP/1.1\r\nHost: other\r\n\r\n", 421);
    raw_status(&f, "POST /mcp HTTP/1.1\r\nHost: localhost:99999\r\n\r\n", 400);
    raw_status(&f,
               "POST /mcp HTTP/1.1\r\nHost: localhost\r\n"
               "Authorization: Bearer McpSecretCanary42\r\n"
               "Content-Type: text/plain\r\nContent-Length: 0\r\n\r\n",
               415);
    raw_status(&f,
               "POST /mcp HTTP/1.1\r\nHost: localhost\r\n"
               "Authorization: Bearer McpSecretCanary42\r\n"
               "Content-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n"
               "z\r\n",
               400);
    raw_status(&f, "POST /mcp HTTP/1.1\r\nHost:", 400);
    raw_status(&f,
               "POST /mcp HTTP/1.1\r\nHost: localhost\r\n"
               "Authorization: Bearer McpSecretCanary42\r\n"
               "Content-Type: application/json\r\nContent-Length: 10\r\n\r\n{",
               400);
    char incomplete[1025];
    memset(incomplete, 'x', 1024);
    incomplete[1024] = 0;
    raw_status(&f, incomplete, 431);
    char out[4096];
    request(&f, "", auth, out, sizeof out, 0);
    CHECK(strstr(out, "HTTP/1.1 400 ") == out);
    CHECK(strstr(out, "\"code\":-32700") != NULL);
    request(&f, "\xff", auth, out, sizeof out, 0);
    CHECK(strstr(out, "HTTP/1.1 400 ") == out);
    CHECK(strstr(out, "\"code\":-32700") != NULL);
    struct mo_protocol_server *invalid = NULL;
    const char *authorities[] = {NULL};
    CHECK(mo_protocol_server_new(f.image, 0, f.store, f.listener, authorities, 1, NULL, &invalid) ==
          MO_PROTOCOL_SERVER_CONFIG);
    CHECK(invalid == NULL);
    finish(&f);
}

static void result_updates(void) {
    const char *text = "module Main\nenum QErr\nNotLeased\nNoSuchJob(id: Int)\nend\n"
                       "struct Job\nid: Int\npayload: Text\nend\n"
                       "service\nstate Int = 0\ntools ack, lease, read\n"
                       "token from env \"MO_TEST_MCP_TOKEN\"\nend\n"
                       "fn ack(state: Int, _now: Time, id: Int) : Result((Int, Unit), QErr)\n"
                       "if id == 1\nOk((state + 1, Unit))\nelse if id == 2\nError(NotLeased)\n"
                       "else\nError(NoSuchJob(id: id))\nend\nend\n"
                       "fn lease(state: Int, _now: Time) : Result((Int, Option(Job)), QErr)\n"
                       "Ok((state + 1, Some(Job(id: state, payload: \"p\"))))\nend\n"
                       "fn read(state: Int, _now: Time) : Text\nstate.to_text\nend\n";
    struct fixture_server f = start_source(text);
    char out[8192];
    tool_request(&f, "ack", "{\"id\":\"1\"}", "1", out, sizeof out);
    CHECK(strstr(out, "\"structuredContent\":{\"value\":null}") != NULL);
    tool_request(&f, "ack", "{\"id\":\"2\"}", "2", out, sizeof out);
    CHECK(strstr(out, "\"kind\":\"application\"") != NULL);
    CHECK(strstr(out, "\"case\":\"NotLeased\"") != NULL);
    tool_request(&f, "ack", "{\"id\":\"3\"}", "3", out, sizeof out);
    CHECK(strstr(out, "\"case\":\"NoSuchJob\",\"fields\":{\"id\":\"3\"}") != NULL);
    tool_request(&f, "lease", "{}", "4", out, sizeof out);
    CHECK(strstr(out, "\"case\":\"Some\",\"value\":{\"id\":\"1\",\"payload\":\"p\"}") != NULL);
    tool_request(&f, "read", "{}", "5", out, sizeof out);
    CHECK(strstr(out, "\"structuredContent\":{\"value\":\"2\"}") != NULL);
    CHECK(mo_protocol_server_metrics(f.server).admitted == 5);
    finish(&f);
}

static uint64_t uptime(const char *out) {
    const char *field = strstr(out, "\"uptime_us\":\"");
    CHECK(field != NULL);
    return field == NULL ? 0 : strtoull(field + strlen("\"uptime_us\":\""), NULL, 10);
}

static void operator_views(void) {
    struct fixture_server f = start();
    char out[32768];
    request(&f, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}", auth, out, sizeof out,
            0);
    char depth[32];
    int written = snprintf(depth, sizeof depth, "\"maxItems\":%d", MO_LIMIT_SAVED_DEPTH);
    MO_ASSERT(written > 0 && (size_t)written < sizeof depth);
    CHECK(strstr(out, depth) != NULL);
    struct mo_protocol_metrics before = mo_protocol_server_metrics(f.server);
    tool_request(&f, "mo-overview", "{}", "1", out, sizeof out);
    CHECK(strstr(out, "\"recovery_sequence\":\"0\"") != NULL);
    CHECK(strstr(out, "McpSecretCanary42") == NULL);
    tool_request(&f, "mo-state", "{}", "2", out, sizeof out);
    CHECK(strstr(out, "\"complete\":true") != NULL);
    CHECK(strstr(out, "\"state\":\"0\"") != NULL);
    CHECK(mo_protocol_server_metrics(f.server).admitted == before.admitted);
    tool_request(&f, "fail", "{}", "3", out, sizeof out);
    tool_error(out, "handler_failed", "none", "3");
    tool_request(&f, "fail", "{}", "4", out, sizeof out);
    tool_error(out, "handler_failed", "none", "4");
    tool_request(&f, "mo-supervisor", "{}", "5", out, sizeof out);
    const char *first = strstr(out, "\"number\":\"2\"");
    const char *second = strstr(out, "\"number\":\"1\"");
    CHECK(first != NULL && second != NULL && first < second);
    CHECK(strstr(out, "never render this secret") == NULL);
    tool_request(&f, "mo-supervisor", "{\"after\":1}", "6", out, sizeof out);
    CHECK(strstr(out, "\"number\":\"2\"") != NULL);
    CHECK(strstr(out, "\"number\":\"1\"") == NULL);
    CHECK(mo_protocol_server_metrics(f.server).admitted == before.admitted + 2);
    for (unsigned i = 2; i < MO_LIMIT_SERVICE_FAILURES * MO_LIMIT_SERVICE_RESTARTS; i++) {
        tool_request(&f, "fail", "{}", "7", out, sizeof out);
        CHECK(strstr(out, "\"isError\":true") != NULL);
    }
    tool_request(&f, "mo-overview", "{}", "8", out, sizeof out);
    CHECK(strstr(out, "\"running\":false") != NULL);
    uint64_t stopped = uptime(out);
    MO_ASSERT(mo_listener_fake_advance(f.listener, 7000000) == MO_LISTENER_OK);
    tool_request(&f, "mo-overview", "{}", "9", out, sizeof out);
    CHECK(uptime(out) >= stopped + 7000000);
    finish(&f);
}

static void private_views(void) {
    const char private_source[] =
        "module Main\nstruct Secret\n answer: Int\n text: Text\n items: List(Int)\nend\n"
        "service\n state Secret = Secret(answer: 42, text: \"private\", items: [7, 9])\n"
        " tools mo_state\n every 1.hour tick\n token from env \"MO_TEST_MCP_TOKEN\"\nend\n"
        "fn mo_state(_state: Secret, _now: Time) : Int\n 777\nend\n"
        "fn tick(state: Secret, _now: Time) : Secret\n state\nend\n";
    struct fixture_server f = start_source(private_source);
    char out[32768];
    tool_request(&f, "mo-state", "{}", "1", out, sizeof out);
    CHECK(strstr(out, "\"type\":\"Main.Secret\"") != NULL);
    CHECK(strstr(out, "\"answer\":\"42\"") != NULL);
    CHECK(strstr(out, "\"items\":[\"7\",\"9\"]") != NULL);
    tool_request(&f, "mo-state", "{\"path\":[\"items\",1]}", "2", out, sizeof out);
    CHECK(strstr(out, "\"state\":\"9\"") != NULL);
    tool_request(&f, "mo-state", "{\"path\":[-1]}", "3", out, sizeof out);
    CHECK(strstr(out, "\"code\":-32602") != NULL);
    tool_request(&f, "mo-export", "{}", "4", out, sizeof out);
    CHECK(strstr(out, "\"code\":-32602") != NULL);
    tool_request(&f, "mo-failures", "{}", "5", out, sizeof out);
    CHECK(strstr(out, "\"records\":[]") != NULL);
    CHECK(mo_protocol_server_metrics(f.server).admitted == 0);
    tool_request(&f, "mo_state", "{}", "6", out, sizeof out);
    CHECK(strstr(out, "\"value\":\"777\"") != NULL);
    CHECK(mo_protocol_server_metrics(f.server).admitted == 1);
    finish(&f);
}

/* Export model traces: create/read, commit/read, same-sequence replacement,
 * expiry, final release and restart. These go through authenticated native dispatch. */
static void export_generations(void) {
    const char text[] =
        "module Main\nservice\n state Text = \"a\".repeat(20000)\n tools change, fail\n"
        " token from env \"MO_TEST_MCP_TOKEN\"\nend\n"
        "fn change(_state: Text, _now: Time) : (Text, Int)\n"
        " (\"b\".repeat(20000), 1)\nend\n"
        "fn fail(_state: Text, _now: Time) : Int\n stop \"restart export fixture\"\nend\n";
    struct fixture_server f = start_source(text);
    f.tick_us = 100000;
    char out[32768], first[33], second[33], args[160];
    tool_request(&f, "mo-export", "{\"kind\":\"state\"}", "1", out, sizeof out);
    const char *id = strstr(out, "\"export\":\"");
    CHECK(id != NULL);
    if (id == NULL) {
        finish(&f);
        return;
    }
    memcpy(first, id + 10, 32);
    first[32] = 0;
    CHECK(strstr(out, "\"length\":8192") != NULL);
    CHECK(strstr(out, "\"isError\":false") != NULL);
    CHECK(strstr(out, "\"sequence\":\"0\"") != NULL);
    CHECK(strstr(out, "\"pages\":3") != NULL);
    CHECK(mo_protocol_server_metrics(f.server).admitted == 0);
    tool_request(&f, "change", "{}", "2", out, sizeof out);
    int n = snprintf(args, sizeof args, "{\"kind\":\"state\",\"export\":\"%s\",\"offset\":8192}",
                     first);
    MO_ASSERT(n > 0 && (size_t)n < sizeof args);
    tool_request(&f, "mo-export", args, "3", out, sizeof out);
    CHECK(strstr(out, "\"sequence\":\"0\"") != NULL);
    CHECK(strstr(out, "\"encoded_sequence\":\"0\"") != NULL);
    CHECK(strstr(out, "YWFhYWFh") != NULL); /* copied old 'a' bytes, not current 'b' */
    tool_request(&f, "mo-export", "{\"kind\":\"state\"}", "4", out, sizeof out);
    id = strstr(out, "\"export\":\"");
    MO_ASSERT(id != NULL);
    memcpy(second, id + 10, 32);
    second[32] = 0;
    CHECK(strcmp(first, second) != 0);
    tool_request(&f, "mo-export", args, "5", out, sizeof out);
    CHECK(strstr(out, "export_replaced") != NULL);
    CHECK(strstr(out, "\"isError\":true") != NULL);
    memcpy(first, second, sizeof first);
    tool_request(&f, "mo-export", "{\"kind\":\"state\"}", "6", out, sizeof out);
    id = strstr(out, "\"export\":\"");
    MO_ASSERT(id != NULL);
    memcpy(second, id + 10, 32);
    second[32] = 0;
    CHECK(strcmp(first, second) != 0); /* no intervening commit */
    n = snprintf(args, sizeof args, "{\"kind\":\"state\",\"export\":\"%s\",\"offset\":8192}",
                 first);
    MO_ASSERT(n > 0 && (size_t)n < sizeof args);
    tool_request(&f, "mo-export", args, "7", out, sizeof out);
    CHECK(strstr(out, "export_replaced") != NULL);
    CHECK(strstr(out, "\"isError\":true") != NULL);
    n = snprintf(args, sizeof args, "{\"kind\":\"state\",\"export\":\"%s\",\"offset\":16384}",
                 second);
    MO_ASSERT(n > 0 && (size_t)n < sizeof args);
    tool_request(&f, "mo-export", args, "8", out, sizeof out);
    CHECK(strstr(out, "\"next\":null") != NULL);
    tool_request(&f, "mo-export", args, "9", out, sizeof out);
    CHECK(strstr(out, "export_released") != NULL);
    CHECK(strstr(out, "\"isError\":true") != NULL);
    tool_request(&f, "mo-memory", "{}", "10", out, sizeof out);
    CHECK(strstr(out, "\"pinned_export_bytes\":0") != NULL);
    tool_request(&f, "mo-export", "{\"kind\":\"state\"}", "11", out, sizeof out);
    id = strstr(out, "\"export\":\"");
    MO_ASSERT(id != NULL);
    memcpy(second, id + 10, 32);
    second[32] = 0;
    n = snprintf(args, sizeof args, "{\"kind\":\"state\",\"export\":\"%s\",\"offset\":8192}",
                 second);
    MO_ASSERT(n > 0 && (size_t)n < sizeof args);
    MO_ASSERT(mo_listener_fake_advance(f.listener, MO_LIMIT_EXPORT_IDLE_US) == MO_LISTENER_OK);
    tool_request(&f, "mo-export", args, "12", out, sizeof out);
    CHECK(strstr(out, "export_released") != NULL);
    CHECK(strstr(out, "\"isError\":true") != NULL);
    CHECK(mo_protocol_server_metrics(f.server).admitted == 1);
    finish(&f);
    f = start_source(text);
    tool_request(&f, "mo-export", args, "13", out, sizeof out);
    CHECK(strstr(out, "export_released") != NULL);
    CHECK(strstr(out, "\"isError\":true") != NULL);
    tool_request(&f, "mo-export", "{\"kind\":\"state\",\"offset\":1}", "14", out, sizeof out);
    CHECK(strstr(out, "\"code\":-32602") != NULL);
    tool_request(&f, "mo-export", "{\"kind\":\"failure\"}", "15", out, sizeof out);
    CHECK(strstr(out, "\"code\":-32602") != NULL);
    f.tick_us = 100000;
    tool_request(&f, "mo-export", "{\"kind\":\"state\"}", "16", out, sizeof out);
    id = strstr(out, "\"export\":\"");
    MO_ASSERT(id != NULL);
    memcpy(second, id + 10, 32);
    second[32] = 0;
    n = snprintf(args, sizeof args, "{\"kind\":\"state\",\"export\":\"%s\",\"offset\":8192}",
                 second);
    MO_ASSERT(n > 0 && (size_t)n < sizeof args);
    for (unsigned i = 0; i < MO_LIMIT_SERVICE_FAILURES; i++) {
        tool_request(&f, "fail", "{}", "17", out, sizeof out);
        tool_error(out, "handler_failed", "none", "17");
    }
    tool_request(&f, "mo-export", args, "18", out, sizeof out);
    CHECK(strstr(out, "export_released") != NULL);
    CHECK(strstr(out, "\"isError\":true") != NULL);

    finish(&f);
}

static void export_expiry_refresh(void) {
    const char text[] = "module Main\nservice\n state Text = \"a\".repeat(20000)\n tools count\n"
                        " token from env \"MO_TEST_MCP_TOKEN\"\nend\n"
                        "fn count(state: Text, _now: Time) : Int\n state.byte_count\nend\n";
    struct fixture_server f = start_source(text);
    char out[32768], id[33], args[160], body[320];
    tool_request(&f, "mo-export", "{\"kind\":\"state\"}", "1", out, sizeof out);
    const char *field = strstr(out, "\"export\":\"");
    MO_ASSERT(field != NULL);
    memcpy(id, field + 10, 32);
    id[32] = 0;
    int n =
        snprintf(args, sizeof args, "{\"kind\":\"state\",\"export\":\"%s\",\"offset\":8192}", id);
    MO_ASSERT(n > 0 && (size_t)n < sizeof args);
    MO_ASSERT(mo_listener_fake_advance(f.listener, MO_LIMIT_EXPORT_IDLE_US - 1) == MO_LISTENER_OK);
    tool_request(&f, "mo-export", args, "2", out, sizeof out);
    CHECK(strstr(out, "\"offset\":8192") != NULL);
    CHECK(strstr(out, "\"length\":8192") != NULL);
    CHECK(strstr(out, "\"isError\":false") != NULL);
    MO_ASSERT(mo_listener_fake_advance(f.listener, 1) == MO_LISTENER_OK);
    tool_request(&f, "mo-memory", "{}", "3", out, sizeof out);
    field = strstr(out, "\"pinned_export_bytes\":");
    MO_ASSERT(field != NULL);
    CHECK(strtoull(field + strlen("\"pinned_export_bytes\":"), NULL, 10) > 0);
    /* One microsecond before the refreshed deadline, no refused read may refresh again. */
    MO_ASSERT(mo_listener_fake_advance(f.listener, MO_LIMIT_EXPORT_IDLE_US - 2) == MO_LISTENER_OK);
    n = snprintf(body, sizeof body, "{\"kind\":\"state\",\"export\":\"%s\",\"offset\":32768}", id);
    MO_ASSERT(n > 0 && (size_t)n < sizeof body);
    tool_request(&f, "mo-export", body, "4", out, sizeof out);
    CHECK(strstr(out, "invalid_offset") != NULL);
    CHECK(strstr(out, "\"isError\":true") != NULL);
    char wrong[33];
    memcpy(wrong, id, sizeof wrong);
    wrong[0] = wrong[0] == '0' ? '1' : '0';
    n = snprintf(body, sizeof body, "{\"kind\":\"state\",\"export\":\"%s\",\"offset\":8192}",
                 wrong);
    MO_ASSERT(n > 0 && (size_t)n < sizeof body);
    tool_request(&f, "mo-export", body, "5", out, sizeof out);
    CHECK(strstr(out, "export_replaced") != NULL);
    CHECK(strstr(out, "\"isError\":true") != NULL);
    n = snprintf(body, sizeof body,
                 "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"mo-export\",\"arguments\":%s}}",
                 args);
    MO_ASSERT(n > 0 && (size_t)n < sizeof body);
    request(&f, body, "Authorization: Bearer WrongExportCanary\r\n", out, sizeof out, 0);
    CHECK(strstr(out, "HTTP/1.1 401") != NULL);
    MO_ASSERT(mo_listener_fake_advance(f.listener, 1) == MO_LISTENER_OK);
    tool_request(&f, "mo-export", args, "7", out, sizeof out);
    CHECK(strstr(out, "export_released") != NULL);
    CHECK(strstr(out, "\"isError\":true") != NULL);
    CHECK(mo_protocol_server_metrics(f.server).admitted == 0);
    finish(&f);
}

static void export_id(const char *out, char id[33]) {
    const char *at = strstr(out, "\"export\":\"");
    CHECK(at != NULL);
    memcpy(id, at == NULL ? "" : at + 10, at == NULL ? 1 : 32);
    id[at == NULL ? 0 : 32] = 0;
}

/* Text arguments with NUL or newline still produce readable, pinnable records. */
static void failure_text_arguments(void) {
    struct fixture_server f = start_source(
        "module Main\nservice\n state Int = 0\n tools echo\n"
        " token from env \"MO_TEST_MCP_TOKEN\"\nend\n"
        "fn echo(_state: Int, _now: Time, _text: Text) : Bool\n stop \"echo fixture\"\nend\n");
    char out[32768];
    tool_request(&f, "echo", "{\"_text\":\"a\\u0000b\"}", "1", out, sizeof out);
    tool_error(out, "handler_failed", "none", "1");
    tool_request(&f, "echo", "{\"_text\":\"x\\nframe 99 file 0 span 0..0\"}", "2", out, sizeof out);
    tool_error(out, "handler_failed", "none", "2");
    tool_request(&f, "mo-failures", "{}", "3", out, sizeof out);
    CHECK(strstr(out, "\"isError\":false") != NULL);
    CHECK(strstr(out, "\"records\":[{\"id\":\"1\"") != NULL);
    CHECK(strstr(out, "{\"id\":\"2\"") != NULL);
    CHECK(strstr(out, "\"unavailable\":[]") != NULL);
    tool_request(&f, "mo-export", "{\"kind\":\"failure\",\"failure\":1}", "4", out, sizeof out);
    CHECK(strstr(out, "\"isError\":false") != NULL && strstr(out, "\"export\":\"") != NULL);
    finish(&f);
}

/* A stored row whose message does not decode is reported unavailable; the view still answers. */
static void failure_view_unreadable(void) {
    struct fixture_server f = start();
    const uint8_t a = 42, b = 43;
    struct mo_store_record state = {MO_STORE_FAILURE_STATE, 0, 1, &a, 1};
    struct mo_store_record message = {MO_STORE_FAILURE_MESSAGE, 0, 1, &b, 1};
    uint64_t id = 0;
    CHECK(mo_store_capture(f.store, "junk", &state, &message, &id) == MO_STORE_CAPTURE_OK);
    CHECK(id == 1);
    char out[32768];
    tool_request(&f, "fail", "{}", "1", out, sizeof out);
    tool_error(out, "handler_failed", "none", "1");
    tool_request(&f, "mo-failures", "{}", "2", out, sizeof out);
    CHECK(strstr(out, "\"isError\":false") != NULL);
    CHECK(strstr(out, "\"records\":[{\"id\":\"2\"") != NULL);
    CHECK(strstr(out, "{\"id\":\"1\"") == NULL);
    CHECK(strstr(out, "\"reason\":\"record 1 unreadable\"") != NULL);
    finish(&f);
}

/* Asking for a failure that does not exist leaves the live failure export readable. */
static void failure_export_bad_id(void) {
    struct fixture_server f =
        start_source("module Main\nservice\n state Text = \"a\".repeat(20000)\n tools fail\n"
                     " token from env \"MO_TEST_MCP_TOKEN\"\nend\n"
                     "fn fail(_state: Text, _now: Time) : Int\n stop \"export fixture\"\nend\n");
    char out[32768], id[33], args[192];
    tool_request(&f, "fail", "{}", "1", out, sizeof out);
    tool_error(out, "handler_failed", "none", "1");
    tool_request(&f, "mo-export", "{\"kind\":\"failure\",\"failure\":1}", "2", out, sizeof out);
    CHECK(strstr(out, "\"isError\":false") != NULL && strstr(out, "\"pages\":3") != NULL);
    export_id(out, id);
    tool_request(&f, "mo-export", "{\"kind\":\"failure\",\"failure\":99}", "3", out, sizeof out);
    CHECK(strstr(out, "failure_unavailable") != NULL);
    int n = snprintf(args, sizeof args,
                     "{\"kind\":\"failure\",\"failure\":1,\"export\":\"%s\",\"offset\":8192}", id);
    MO_ASSERT(n > 0 && (size_t)n < sizeof args);
    tool_request(&f, "mo-export", args, "4", out, sizeof out);
    CHECK(strstr(out, "\"isError\":false") != NULL);
    CHECK(strstr(out, "export_released") == NULL);
    finish(&f);
}

static uint64_t export_poll_wait(struct fixture_server *f, uint64_t until_us) {
    CHECK(!mo_protocol_server_connected(f->server));
    CHECK(mo_protocol_server_poll(f->server, until_us) == MO_PROTOCOL_SERVER_OK);
    uint64_t waited = 0;
    CHECK(mo_listener_fake_last_wait(f->listener, &waited) == MO_LISTENER_OK);
    return waited;
}

/* WAIT until_us is the pin idle deadline, not the caller's later bound. */
static void export_idle_wait(void) {
    const char text[] = "module Main\nservice\n state Text = \"a\".repeat(20000)\n tools fail\n"
                        " token from env \"MO_TEST_MCP_TOKEN\"\nend\n"
                        "fn fail(_state: Text, _now: Time) : Int\n stop \"export fixture\"\nend\n";
    char out[32768];
    uint64_t idle = MO_LIMIT_EXPORT_IDLE_US, far = 2 * idle;
    struct fixture_server f = start_source(text);
    CHECK(export_poll_wait(&f, far) == far);
    finish(&f);
    f = start_source(text);
    tool_request(&f, "mo-export", "{\"kind\":\"state\"}", "1", out, sizeof out);
    CHECK(strstr(out, "\"isError\":false") != NULL && strstr(out, "\"next\":null") == NULL);
    CHECK(export_poll_wait(&f, far) == idle);
    finish(&f);
    f = start_source(text);
    tool_request(&f, "fail", "{}", "1", out, sizeof out);
    tool_error(out, "handler_failed", "none", "1");
    tool_request(&f, "mo-export", "{\"kind\":\"failure\",\"failure\":1}", "2", out, sizeof out);
    CHECK(strstr(out, "\"isError\":false") != NULL && strstr(out, "\"next\":null") == NULL);
    CHECK(export_poll_wait(&f, far) == idle);
    finish(&f);
    f = start_source(text);
    tool_request(&f, "fail", "{}", "1", out, sizeof out);
    tool_error(out, "handler_failed", "none", "1");
    tool_request(&f, "mo-export", "{\"kind\":\"failure\",\"failure\":1}", "2", out, sizeof out);
    CHECK(strstr(out, "\"isError\":false") != NULL);
    MO_ASSERT(mo_listener_fake_advance(f.listener, 1) == MO_LISTENER_OK);
    tool_request(&f, "mo-export", "{\"kind\":\"state\"}", "3", out, sizeof out);
    CHECK(strstr(out, "\"isError\":false") != NULL && strstr(out, "\"next\":null") == NULL);
    CHECK(export_poll_wait(&f, 1 + far) == idle);
    finish(&f);
}

static void session_of(const char *out, char id[MO_LIMIT_MCP_SESSION_ID_BYTES + 1]) {
    const char *at = strstr(out, "\r\nMCP-Session-Id: ");
    id[0] = 0;
    CHECK(at != NULL);
    if (at == NULL)
        return;
    at += strlen("\r\nMCP-Session-Id: ");
    size_t n = strcspn(at, "\r");
    CHECK(n == MO_LIMIT_MCP_SESSION_ID_BYTES && strspn(at, "0123456789abcdef") == n);
    if (n == MO_LIMIT_MCP_SESSION_ID_BYTES) {
        memcpy(id, at, n);
        id[n] = 0;
    }
}

static void initialize(struct fixture_server *f, char id[MO_LIMIT_MCP_SESSION_ID_BYTES + 1]) {
    char out[4096];
    request(f,
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
            "\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{"
            "\"name\":\"test\",\"version\":\"1\"}}}",
            auth, out, sizeof out, 0);
    CHECK(strstr(out, "\"listChanged\":true") != NULL);
    session_of(out, id);
}

struct stream_peer {
    char input[512], output[4096];
    struct mo_listener_fake_peer peer;
};

static const char stream_head[] = "HTTP/1.1 200 OK\r\nConnection: close\r\nCache-Control: "
                                  "no-store\r\nContent-Type: text/event-stream\r\n\r\n";

/* A GET that stays connected; the caller polls and inspects peer state. */
static void stream_connect(struct fixture_server *f, struct stream_peer *p, const char *fields,
                           bool writable) {
    int n = snprintf(p->input, sizeof p->input, "GET /mcp HTTP/1.1\r\nHost: localhost\r\n%s\r\n",
                     fields);
    MO_ASSERT(n > 0 && (size_t)n < sizeof p->input);
    p->peer = (struct mo_listener_fake_peer){.input = (const uint8_t *)p->input,
                                             .input_size = (size_t)n,
                                             .output = (uint8_t *)p->output,
                                             .output_capacity = sizeof p->output - 1,
                                             .readable = true,
                                             .writable = writable};
    MO_ASSERT(mo_listener_fake_connect(f->listener, &p->peer) == MO_LISTENER_OK);
    for (unsigned i = 0; i < 16 && p->peer.output_size < strlen(stream_head) && !p->peer.closed;
         i++)
        poll_server(f);
    p->output[p->peer.output_size] = 0;
}

static void stream_fields(char *out, size_t size, const char *session) {
    int n = snprintf(out, size,
                     "%sAccept: text/event-stream\r\nMCP-Protocol-Version: 2025-11-25\r\n"
                     "MCP-Session-Id: %s\r\n",
                     auth, session);
    MO_ASSERT(n > 0 && (size_t)n < size);
}

static void streams(void) {
    struct fixture_server f = start();
    char session[MO_LIMIT_MCP_SESSION_ID_BYTES + 1], other[MO_LIMIT_MCP_SESSION_ID_BYTES + 1];
    initialize(&f, session);
    initialize(&f, other);
    CHECK(strcmp(session, other) != 0);
    char fields[512], out[4096];
    stream_fields(fields, sizeof fields, session);
    struct stream_peer first, second, third;
    stream_connect(&f, &first, fields, true);
    CHECK(strcmp(first.output, stream_head) == 0 && !first.peer.closed);
    for (unsigned i = 0; i < 8; i++)
        poll_server(&f);
    CHECK(!first.peer.closed && mo_protocol_server_connected(f.server));
    /* A reconnect in the same session replaces its stream, so nothing reaches both. */
    stream_connect(&f, &second, fields, true);
    CHECK(strcmp(second.output, stream_head) == 0 && first.peer.closed && !second.peer.closed);
    /* Calls keep working with or without the session header. */
    char with_session[256];
    int n = snprintf(with_session, sizeof with_session, "%sMCP-Session-Id: %s\r\n", auth, session);
    MO_ASSERT(n > 0 && (size_t)n < sizeof with_session);
    request(&f, "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"ping\"}", with_session, out, sizeof out,
            0);
    CHECK(strstr(out, "HTTP/1.1 200 ") == out && strstr(out, "\"id\":9,\"result\":{}") != NULL);
    n = snprintf(with_session, sizeof with_session,
                 "%sMCP-Session-Id: 00000000000000000000000000000000\r\n", auth);
    MO_ASSERT(n > 0 && (size_t)n < sizeof with_session);
    request(&f, "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"ping\"}", with_session, out, sizeof out,
            0);
    CHECK(strstr(out, "HTTP/1.1 404 ") == out);
    /* The client's end closes the stream and frees its slot. */
    second.peer.eof = true;
    for (unsigned i = 0; i < 8 && !second.peer.closed; i++)
        poll_server(&f);
    CHECK(second.peer.closed && !mo_protocol_server_connected(f.server));
    const struct {
        const char *fields;
        int status;
    } refusals[] = {
        {"Accept: text/event-stream\r\nMCP-Session-Id: 0\r\n", 401},
        {"Authorization: Bearer McpSecretCanary42\r\nAccept: text/event-stream\r\n", 400},
        {"Authorization: Bearer McpSecretCanary42\r\nAccept: text/event-stream\r\n"
         "MCP-Session-Id: ffffffffffffffffffffffffffffffff\r\n",
         404},
        {"Authorization: Bearer McpSecretCanary42\r\nAccept: application/json\r\n", 406},
    };
    for (size_t i = 0; i < sizeof refusals / sizeof *refusals; i++) {
        char raw[512];
        n = snprintf(raw, sizeof raw, "GET /mcp HTTP/1.1\r\nHost: localhost\r\n%s\r\n",
                     refusals[i].fields);
        MO_ASSERT(n > 0 && (size_t)n < sizeof raw);
        raw_status(&f, raw, refusals[i].status);
    }
    raw_status(&f, "DELETE /mcp HTTP/1.1\r\nHost: localhost\r\n\r\n", 405);
    /* A consumer that never reads is disconnected, never waited for. */
    stream_fields(fields, sizeof fields, other);
    stream_connect(&f, &third, fields, true);
    CHECK(!third.peer.closed);
    third.peer.writable = false;
    struct stream_peer slow;
    stream_fields(fields, sizeof fields, session);
    stream_connect(&f, &slow, fields, false);
    CHECK(slow.peer.output_size == 0 && !slow.peer.closed);
    MO_ASSERT(mo_listener_fake_advance(f.listener, MO_LIMIT_MCP_WRITE_US - 1) == MO_LISTENER_OK);
    poll_server(&f);
    CHECK(!slow.peer.closed);
    MO_ASSERT(mo_listener_fake_advance(f.listener, 1) == MO_LISTENER_OK);
    poll_server(&f);
    CHECK(slow.peer.closed && !third.peer.closed);
    finish(&f);
    f = start();
    struct stream_peer open[MO_LIMIT_MCP_STREAMS + 1];
    for (size_t i = 0; i <= MO_LIMIT_MCP_STREAMS; i++) {
        initialize(&f, session);
        stream_fields(fields, sizeof fields, session);
        if (i < MO_LIMIT_MCP_STREAMS) {
            stream_connect(&f, &open[i], fields, true);
            CHECK(strcmp(open[i].output, stream_head) == 0 && !open[i].peer.closed);
            continue;
        }
        char raw[512];
        n = snprintf(raw, sizeof raw, "GET /mcp HTTP/1.1\r\nHost: localhost\r\n%s\r\n", fields);
        MO_ASSERT(n > 0 && (size_t)n < sizeof raw);
        raw_status(&f, raw, 503);
    }
    mo_protocol_server_stop_accepting(f.server);
    for (unsigned i = 0; i < 4 && mo_protocol_server_connected(f.server); i++)
        poll_server(&f);
    CHECK(!mo_protocol_server_connected(f.server));
    for (size_t i = 0; i < MO_LIMIT_MCP_STREAMS; i++)
        CHECK(open[i].peer.closed);
    finish(&f);
}

static void run(void *context) {
    failure_text_arguments();
    failure_view_unreadable();
    failure_export_bad_id();
    export_idle_wait();
    export_expiry_refresh();
    export_generations();
    operator_views();
    private_views();
    wire(context);
    literal_boundaries();
    service_hangup();
    rejection_paths();
    result_updates();
    small_body_reads();
    rate();
    stopped();
    events_reach_the_sink();
    connected();
    response_limit();
    busy();
    streams();
}

int main(void) {
    mo_runtime_identity_init();
    MO_ASSERT(mo_with_stack(MO_SOURCE_STACK_SIZE, run, NULL) == 0);
    return mo_test_report("protocol_server");
}
#endif
