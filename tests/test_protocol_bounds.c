#define _POSIX_C_SOURCE 200809L
#include "base/stack.h"
#include "service_compile.h"
#include "test.h"
#include "tools/protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char source[] =
    "module Main\n"
    "service\n state Int = 0\n tools sized, grow, read\n"
    " token from env \"MO_TEST_MCP_TOKEN\"\nend\n"
    "fn sized(_state: Int, _now: Time, count: Int) : Text\n \"x\".repeat(count)\nend\n"
    "fn grow(state: Int, _now: Time, count: Int) : (Int, Text)\n"
    " (state + 1, \"x\".repeat(count))\nend\n"
    "fn read(state: Int, _now: Time) : Text\n state.to_text\nend\n";

static const char *const base = "POST /mcp HTTP/1.1\r\nHost: localhost\r\n"
                                "Content-Type: application/json\r\n"
                                "MCP-Protocol-Version: 2025-11-25\r\n"
                                "Authorization: Bearer McpSecretCanary42\r\n";
static const char ping[] = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}";

struct fx {
    struct mo_image *image;
    struct mo_store *store;
    struct mo_listener *listener;
    struct mo_protocol_server *server;
};

static struct fx up(void) {
    struct fx f = {.image = compile(source)};
    MO_ASSERT(mo_listener_fake("MO_TEST_MCP_TOKEN", &f.listener) == MO_LISTENER_OK);
    MO_ASSERT(mo_store_open(NULL, &f.store) == MO_STORE_OK);
    const char *a[] = {"localhost"};
    MO_ASSERT(mo_protocol_server_new(f.image, 0, f.store, f.listener, a, 1, NULL, &f.server) ==
              MO_PROTOCOL_SERVER_OK);
    return f;
}

static void down(struct fx *f) {
    mo_protocol_server_free(f->server);
    mo_listener_close(f->listener);
    mo_store_close(f->store);
    mo_image_free(f->image);
}

static char output[128 * 1024];
static char in[96 * 1024];
static size_t out_size;

static void drive(struct fx *f, size_t size) {
    struct mo_listener_fake_peer p = {.input = (const uint8_t *)in,
                                      .input_size = size,
                                      .output = (uint8_t *)output,
                                      .output_capacity = sizeof output - 1,
                                      .readable = true,
                                      .writable = true,
                                      .eof = true};
    MO_ASSERT(mo_listener_fake_connect(f->listener, &p) == MO_LISTENER_OK);
    for (int i = 0; i < 400 && !p.closed; i++) {
        MO_ASSERT(mo_protocol_server_poll(f->server, 0) == MO_PROTOCOL_SERVER_OK);
    }
    MO_ASSERT(p.closed);
    MO_ASSERT(mo_listener_fake_advance(f->listener, 100000) == MO_LISTENER_OK);
    out_size = p.output_size;
    output[p.output_size] = 0;
}

static int status(void) {
    return out_size > 12 ? atoi(output + 9) : 0;
}

static size_t body_bytes(void) {
    const char *b = strstr(output, "\r\n\r\n");
    return b == NULL ? 0 : out_size - (size_t)(b + 4 - output);
}

static void expect(const char *label, int want) {
    if (status() != want)
        (void)fprintf(stderr, "%s: got %d, expected %d\n", label, status(), want);
    CHECK(status() == want);
}

static size_t head_of(size_t head, const char *extra) {
    size_t used = (size_t)sprintf(in, "%s%sContent-Length: %zu\r\n", base, extra, strlen(ping));
    size_t pad = head - used - strlen("X: \r\n\r\n");
    used += (size_t)sprintf(in + used, "X: ");
    memset(in + used, 'y', pad);
    used += pad;
    used += (size_t)sprintf(in + used, "\r\n\r\n");
    MO_ASSERT(used == head);
    memcpy(in + used, ping, strlen(ping));
    return used + strlen(ping);
}

static void header_bytes(struct fx *f) {
    drive(f, head_of(1024, ""));
    expect("header block 1024", 200);
    drive(f, head_of(1025, ""));
    expect("header block 1025", 431);
}

static void header_fields(struct fx *f) {
    for (int count = 64; count <= 65; count++) {
        int n = sprintf(in, "%sContent-Length: %zu\r\n", base, strlen(ping));
        for (int i = 5; i < count; i++)
            n += sprintf(in + n, "a:\r\n");
        n += sprintf(in + n, "\r\n%s", ping);
        drive(f, (size_t)n);
        expect(count == 64 ? "header fields 64" : "header fields 65", count == 64 ? 200 : 431);
    }
}

static size_t with_body(const char *body, size_t size) {
    int n = sprintf(in, "%sContent-Length: %zu\r\n\r\n", base, size);
    memcpy(in + n, body, size);
    return (size_t)n + size;
}

static size_t call(const char *tool, size_t n, const char *id) {
    static char body[256];
    int size = sprintf(body,
                       "{\"jsonrpc\":\"2.0\",\"id\":%s,\"method\":\"tools/call\",\"params\":"
                       "{\"name\":\"%s\",\"arguments\":{\"count\":\"%zu\"}}}",
                       id, tool, n);
    return with_body(body, (size_t)size);
}

static void body_ceiling(struct fx *f) {
    static char body[20000];
    memset(body, ' ', sizeof body);
    memcpy(body, ping, strlen(ping));
    drive(f, with_body(body, 16384));
    expect("decoded body 16384 (trailing JSON space)", 200);
    drive(f, with_body(body, 16385));
    expect("decoded body 16385", 413);
    int n = sprintf(in, "%sContent-Length: 16385\r\n\r\n", base);
    drive(f, (size_t)n);
    expect("Content-Length 16385 without body", 413);
    n = sprintf(in, "%sContent-Length: 99999999999999999999999\r\n\r\n", base);
    drive(f, (size_t)n);
    expect("Content-Length overflow", 400);
}

static size_t chunked(size_t chunks, size_t total_ingress) {
    int n = sprintf(in, "%sTransfer-Encoding: chunked\r\n\r\n", base);
    size_t at = (size_t)n;
    static char body[20000];
    memset(body, ' ', sizeof body);
    memcpy(body, ping, strlen(ping));
    size_t each = 1;

    size_t tail = chunks * strlen("1\r\nX\r\n") + strlen("0\r\n\r\n");
    MO_ASSERT(total_ingress >= at + tail);
    size_t pad = total_ingress - at - tail;
    size_t i = 0;
    for (size_t c = 0; c < chunks; c++) {
        size_t take = each;
        if (pad != 0) {
            size_t chunk_pad = pad > 8192 - 3 ? 8192 - 3 : pad;
            if (chunk_pad == 1) {
                at += (size_t)sprintf(in + at, "01\r\n");
            } else if (chunk_pad == 2) {
                at += (size_t)sprintf(in + at, "001\r\n");
            } else {
                at += (size_t)sprintf(in + at, "1;");
                memset(in + at, 'e', chunk_pad - 1);
                at += chunk_pad - 1;
                at += (size_t)sprintf(in + at, "\r\n");
            }
            pad -= chunk_pad;
        } else
            at += (size_t)sprintf(in + at, "1\r\n");
        in[at++] = body[i];
        i += take;
        at += (size_t)sprintf(in + at, "\r\n");
    }
    at += (size_t)sprintf(in + at, "0\r\n\r\n");
    MO_ASSERT(pad == 0);
    return at;
}

static void chunk_ceilings(struct fx *f) {
    size_t size = chunked(1024, 24576);
    MO_ASSERT(size == 24576);
    drive(f, size);
    expect("chunked ingress 24576, 1024 chunks", 200);
    size = chunked(1024, 24577);
    MO_ASSERT(size == 24577);
    drive(f, size);
    expect("chunked ingress 24577, 1024 chunks", 413);
    size = chunked(1025, 8192);
    drive(f, size);
    expect("1025 chunks", 413);

    for (size_t len = 8192; len <= 8193; len++) {
        int n = sprintf(in, "%sTransfer-Encoding: chunked\r\n\r\n", base);
        size_t at = (size_t)n;
        at += (size_t)sprintf(in + at, "%zx;", strlen(ping));
        memset(in + at, 'e', len - (size_t)snprintf(NULL, 0, "%zx;", strlen(ping)));
        at += len - (size_t)snprintf(NULL, 0, "%zx;", strlen(ping));
        at += (size_t)sprintf(in + at, "\r\n%s\r\n0\r\n\r\n", ping);
        drive(f, at);
        expect(len == 8192 ? "chunk size line 8192" : "chunk size line 8193",
               len == 8192 ? 200 : 413);
    }
    for (int fields = 64; fields <= 65; fields++) {
        int n = sprintf(in, "%sTransfer-Encoding: chunked\r\n\r\n", base);
        size_t at = (size_t)n;
        at += (size_t)sprintf(in + at, "%zx\r\n%s\r\n0\r\n", strlen(ping), ping);
        for (int i = 0; i < fields; i++)
            at += (size_t)sprintf(in + at, "a:\r\n");
        at += (size_t)sprintf(in + at, "\r\n");
        drive(f, at);
        expect(fields == 64 ? "trailer fields 64" : "trailer fields 65", fields == 64 ? 200 : 413);
    }
}

static void response_ceiling(struct fx *f) {
    drive(f, call("sized", 1, "1"));
    size_t at_one = body_bytes();
    size_t per = 2;

    const char *id = (32768 - at_one) % 2 == 0 ? "1" : "12";
    size_t base_size = at_one + strlen(id) - 1;
    size_t n = 1 + (32768 - base_size) / per;
    drive(f, call("sized", n, id));
    CHECK(status() == 200);
    CHECK(body_bytes() == 32768);
    CHECK(strstr(output, "response_limit") == NULL);
    const char *longer_id = strlen(id) == 1 ? "12" : "123";
    drive(f, call("sized", n, longer_id));
    CHECK(status() == 200);
    CHECK(strstr(output, "response_limit") != NULL);
    drive(f, call("grow", n, longer_id));
    CHECK(strstr(output, "response_limit") != NULL);
    CHECK(strstr(output, "\"effect\":\"applied\"") != NULL);
}

static void run(void *context) {
    (void)context;
    MO_ASSERT(setenv("MO_TEST_MCP_TOKEN", "McpSecretCanary42", 1) == 0);
    struct fx f = up();
    header_bytes(&f);
    header_fields(&f);
    body_ceiling(&f);
    chunk_ceilings(&f);
    down(&f);
    f = up();
    response_ceiling(&f);
    down(&f);
}

int main(void) {
    MO_ASSERT(mo_with_stack(MO_SOURCE_STACK_SIZE, run, NULL) == 0);
    return mo_test_report("protocol_bounds");
}
