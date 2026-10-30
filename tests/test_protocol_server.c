#define _POSIX_C_SOURCE 200809L
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

static struct fixture_server start_source(const char *text) {
    struct fixture_server f = {.image = compile(text)};
    CHECK(setenv("MO_TEST_MCP_TOKEN", "McpSecretCanary42", 1) == 0);
    MO_ASSERT(mo_listener_fake("MO_TEST_MCP_TOKEN", &f.listener) == MO_LISTENER_OK);
    MO_ASSERT(mo_store_open(NULL, &f.store) == MO_STORE_OK);
    const char *authorities[] = {"localhost"};
    MO_ASSERT(mo_protocol_server_new(f.image, 0, f.store, f.listener, authorities, 1, &f.server) ==
              MO_PROTOCOL_SERVER_OK);
    return f;
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
    MO_ASSERT(mo_buffer_append_text(&text, "module Main\nservice\nstate Int = 0\ntools read\n"));
    for (unsigned i = 0; i <= 64; i++) {
        MO_ASSERT(mo_buffer_append_text(&text, "every 1.second tick_") &&
                  mo_buffer_append_uint(&text, i) && mo_buffer_append_text(&text, "\n"));
    }
    MO_ASSERT(mo_buffer_append_text(
        &text, "token from env \"MO_TEST_MCP_TOKEN\"\nend\n"
               "fn read(state: Int, _now: Time) : Text\nstate.to_text\nend\n"));
    for (unsigned i = 0; i <= 64; i++) {
        MO_ASSERT(mo_buffer_append_text(&text, "fn tick_") && mo_buffer_append_uint(&text, i) &&
                  mo_buffer_append_text(&text, "(state: Int, _now: Time) : Int\nstate + 1\nend\n"));
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
    f.tick_us = 0;
    for (unsigned i = 0; i < 64; i++)
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
    CHECK(mo_protocol_server_new(f.image, 0, f.store, f.listener, authorities, 1, &invalid) ==
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

static void run(void *context) {
    wire(context);
    literal_boundaries();
    service_hangup();
    rejection_paths();
    result_updates();
    small_body_reads();
    rate();
    stopped();
    response_limit();
    busy();
}

int main(void) {
    MO_ASSERT(mo_with_stack(MO_SOURCE_STACK_SIZE, run, NULL) == 0);
    return mo_test_report("protocol_server");
}
#endif
