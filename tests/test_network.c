#define _POSIX_C_SOURCE 200809L
#include "host_fixture.h"
#include "test.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int listener(uint16_t *port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    MO_ASSERT(fd >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t size = sizeof address;
    MO_ASSERT(bind(fd, (const struct sockaddr *)&address, size) == 0 && listen(fd, 128) == 0 &&
              getsockname(fd, (struct sockaddr *)&address, &size) == 0);
    *port = ntohs(address.sin_port);
    return fd;
}

static struct mo_cell network(struct host_fixture *f) {
    struct mo_cell out;
    struct mo_host_error error;
    MO_ASSERT(mo_host_start(&f->host) == NULL);
    MO_ASSERT(mo_host_real(&f->host, f->heap, f->capabilities[MO_CAP_NETWORK], NULL, -1, &out,
                           &error) == MO_HOST_RETURNED);
    return out;
}

static struct mo_cell call(struct host_fixture *f, uint32_t operation,
                           const struct mo_cell *arguments) {
    struct mo_cell out;
    struct mo_host_error error;
    MO_ASSERT(mo_host_call(&f->host, f->heap, operation, host_result_type(f, operation), arguments,
                           &out, &error) == MO_HOST_RETURNED);
    return out;
}

static struct mo_cell dial(struct host_fixture *f, struct mo_cell capability, const char *host,
                           size_t length, int64_t port, int64_t within) {
    struct mo_cell arguments[] = {capability, mo_cell_unit(), mo_cell_int(port),
                                  mo_cell_duration(within)};
    MO_ASSERT(mo_value_text(f->heap, host, length, &arguments[1]));
    struct mo_cell out = call(f, MO_HOST_NETWORK_CONNECT, arguments);
    mo_value_drop(f->heap, f->types.text, arguments[1]);
    return out;
}

static void shows(struct host_fixture *f, uint32_t type, struct mo_cell value,
                  const char *expected) {
    struct mo_buffer printed;
    mo_buffer_init(&printed, MO_LIMIT_STRING_SIZE);
    CHECK(mo_value_print(f->heap, type, value, &printed));
    bool equal =
        printed.length == strlen(expected) && memcmp(printed.bytes, expected, printed.length) == 0;
    if (!equal)
        CHECK(fprintf(stderr, "expected %s, got %.*s\n", expected, (int)printed.length,
                      printed.bytes) >= 0);
    CHECK(equal);
    mo_buffer_free(&printed);
    mo_value_drop(f->heap, type, value);
}

static struct mo_cell opened(struct host_fixture *f, struct mo_cell result) {
    uint32_t type = f->result_connection;
    MO_ASSERT(mo_cell_get_tag(result) == 0);
    struct mo_cell inside = mo_cell_get_payload(result);
    if (mo_case_shape(&f->types.types, type, 0) == MO_CASE_BOXED)
        inside = mo_value_field(inside, 0);
    struct mo_cell out = mo_value_dup(f->heap, f->capabilities[MO_CAP_CONNECTION], inside);
    mo_value_drop(f->heap, type, result);
    return out;
}

static struct mo_cell connected(struct host_fixture *f, struct mo_cell capability, uint16_t port,
                                int64_t within) {
    return opened(f, dial(f, capability, "127.0.0.1", 9, port, within));
}

static void sends(struct host_fixture *f, struct mo_cell connection, const char *bytes,
                  const char *expected) {
    struct mo_cell arguments[] = {connection, mo_cell_unit()};
    MO_ASSERT(mo_value_bytes(f->heap, (const uint8_t *)bytes, strlen(bytes), &arguments[1]));
    shows(f, f->result_sent, call(f, MO_HOST_CONNECTION_SEND, arguments), expected);
    mo_value_drop(f->heap, f->bytes, arguments[1]);
}

static void receives(struct host_fixture *f, struct mo_cell connection, int64_t most,
                     const char *expected) {
    struct mo_cell arguments[] = {connection, mo_cell_int(most)};
    shows(f, f->result_received, call(f, MO_HOST_CONNECTION_RECEIVE, arguments), expected);
}

static void drop_connection(struct host_fixture *f, struct mo_cell connection) {
    mo_value_drop(f->heap, f->capabilities[MO_CAP_CONNECTION], connection);
}

static void finish(struct host_fixture *f, struct mo_cell capability) {
    mo_value_drop(f->heap, f->capabilities[MO_CAP_NETWORK], capability);
    CHECK(mo_heap_objects(f->heap) == 0);
    host_fixture_free(f);
}

static uint64_t milliseconds(void) {
    struct timespec now;
    MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void test_round_trip(void) {
    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    struct mo_cell capability = network(&f);
    uint16_t port;
    int listening = listener(&port);
    struct mo_cell connection = connected(&f, capability, port, 5000000);
    CHECK(f.host.open_connections == 1);
    int peer = accept(listening, NULL, NULL);
    MO_ASSERT(peer >= 0);
    sends(&f, connection, "ping", "Ok(Unit)");
    sends(&f, connection, "", "Ok(Unit)");
    char seen[8];
    CHECK(recv(peer, seen, sizeof seen, 0) == 4 && memcmp(seen, "ping", 4) == 0);
    CHECK(send(peer, "abcdefghij", 10, 0) == 10 && close(peer) == 0);
    receives(&f, connection, 4, "Ok(bytes[97, 98, 99, 100])");
    receives(&f, connection, MO_LIMIT_RECEIVE_BYTES, "Ok(bytes[101, 102, 103, 104, 105, 106])");
    receives(&f, connection, 4, "Ok(bytes[])");
    receives(&f, connection, 4, "Ok(bytes[])");
    drop_connection(&f, connection);
    CHECK(f.host.open_connections == 0);

    connection = connected(&f, capability, port, 5000000);
    peer = accept(listening, NULL, NULL);
    MO_ASSERT(peer >= 0);
    drop_connection(&f, connection);
    CHECK(recv(peer, seen, sizeof seen, 0) == 0 && close(peer) == 0);

    uint16_t named;
    int by_name = listener(&named);
    connection = opened(&f, dial(&f, capability, "localhost", 9, named, 5000000));
    drop_connection(&f, connection);
    CHECK(close(by_name) == 0 && close(listening) == 0);
    finish(&f, capability);
}

static void test_errors(void) {
    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    struct mo_cell capability = network(&f);
    uint16_t port;
    int listening = listener(&port);
    CHECK(close(listening) == 0);
    char expected[96];
    CHECK(snprintf(expected, sizeof expected, "Error(Refused(host: \"127.0.0.1\", port: %u))",
                   (unsigned)port) > 0);
    shows(&f, f.result_connection, dial(&f, capability, "127.0.0.1", 9, port, 5000000), expected);

    shows(&f, f.result_connection, dial(&f, capability, "", 0, 80, 5000000),
          "Error(UnknownHost(host: \"\"))");
    shows(&f, f.result_connection, dial(&f, capability, "a\0b", 3, 80, 5000000),
          "Error(UnknownHost(host: \"a\\u{0}b\"))");
    char longest[MO_LIMIT_HOST_NAME + 2], shown[MO_LIMIT_HOST_NAME + 40];
    memset(longest, 'a', sizeof longest - 1);
    longest[sizeof longest - 1] = '\0';
    CHECK(snprintf(shown, sizeof shown, "Error(UnknownHost(host: \"%s\"))", longest) > 0);
    shows(&f, f.result_connection,
          dial(&f, capability, longest, MO_LIMIT_HOST_NAME + 1, 80, 5000000), shown);

    listening = listener(&port);
    shows(&f, f.result_connection, dial(&f, capability, "127.0.0.1", 9, port, 0),
          "Error(TimedOut)");
    shows(&f, f.result_connection, dial(&f, capability, "127.0.0.1", 9, port, -1),
          "Error(TimedOut)");
    CHECK(f.host.open_connections == 0 && close(listening) == 0);
    finish(&f, capability);
}

static void test_deadline(void) {
    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    struct mo_cell capability = network(&f);
    uint16_t port;
    int listening = listener(&port);
    uint64_t start = milliseconds();
    struct mo_cell connection = connected(&f, capability, port, 300000);
    int peer = accept(listening, NULL, NULL);
    MO_ASSERT(peer >= 0);
    receives(&f, connection, 16, "Error(TimedOut)");
    uint64_t waited = milliseconds() - start;
    CHECK(waited >= 300 && waited < 3000);
    CHECK(send(peer, "late", 4, 0) == 4);
    receives(&f, connection, 16, "Error(TimedOut)");
    sends(&f, connection, "late", "Error(TimedOut)");
    drop_connection(&f, connection);
    CHECK(close(peer) == 0);

    connection = connected(&f, capability, port, 300000);
    struct mo_cell arguments[] = {connection, mo_cell_unit()};
    size_t size = (size_t)64 << 20;
    uint8_t *flood = calloc(size, 1);
    MO_ASSERT(flood != NULL && mo_value_bytes(f.heap, flood, size, &arguments[1]));
    free(flood);
    start = milliseconds();
    shows(&f, f.result_sent, call(&f, MO_HOST_CONNECTION_SEND, arguments), "Error(TimedOut)");
    waited = milliseconds() - start;
    CHECK(waited >= 200 && waited < 3000);
    mo_value_drop(f.heap, f.bytes, arguments[1]);
    drop_connection(&f, connection);
    CHECK(close(listening) == 0);
    finish(&f, capability);
}

static void test_reset(void) {
    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    struct mo_cell capability = network(&f);
    uint16_t port;
    int listening = listener(&port);
    for (unsigned sending = 0; sending < 2; sending++) {
        struct mo_cell connection = connected(&f, capability, port, 5000000);
        int peer = accept(listening, NULL, NULL);
        struct linger now = {.l_onoff = 1};
        MO_ASSERT(peer >= 0 && setsockopt(peer, SOL_SOCKET, SO_LINGER, &now, sizeof now) == 0);
        CHECK(close(peer) == 0);
        if (sending)
            sends(&f, connection, "x", "Error(Closed)");
        else
            receives(&f, connection, 16, "Error(Closed)");
        drop_connection(&f, connection);
    }
    CHECK(close(listening) == 0);
    finish(&f, capability);
}

static void refuses(struct host_fixture *f, uint32_t operation, const struct mo_cell *arguments,
                    const char *reason) {
    struct mo_cell out = mo_cell_int(123);
    struct mo_host_error error;
    CHECK(mo_host_call(&f->host, f->heap, operation, host_result_type(f, operation), arguments,
                       &out, &error) == MO_HOST_BUG);
    CHECK(mo_cell_get_int(out) == 123 && strcmp(error.reason, reason) == 0);
}

static void test_bugs(void) {
    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    struct mo_cell capability = network(&f);
    uint16_t port;
    int listening = listener(&port);
    struct mo_cell dialing[] = {capability, host_text(&f, "127.0.0.1"), mo_cell_int(0),
                                mo_cell_duration(5000000)};
    refuses(&f, MO_HOST_NETWORK_CONNECT, dialing, "port outside 1 to 65535");
    dialing[2] = mo_cell_int(65536);
    refuses(&f, MO_HOST_NETWORK_CONNECT, dialing, "port outside 1 to 65535");

    struct mo_cell connections[MO_LIMIT_OPEN_CONNECTIONS];
    for (unsigned i = 0; i < MO_LIMIT_OPEN_CONNECTIONS; i++)
        connections[i] = connected(&f, capability, port, 5000000);
    CHECK(f.host.open_connections == MO_LIMIT_OPEN_CONNECTIONS);
    dialing[2] = mo_cell_int(port);
    refuses(&f, MO_HOST_NETWORK_CONNECT, dialing, "too many connections open at once");
    drop_connection(&f, connections[0]);
    connections[0] = connected(&f, capability, port, 5000000);

    struct mo_cell receiving[] = {connections[0], mo_cell_int(0)};
    refuses(&f, MO_HOST_CONNECTION_RECEIVE, receiving, "most below 1 or above the receive limit");
    receiving[1] = mo_cell_int((int64_t)MO_LIMIT_RECEIVE_BYTES + 1);
    refuses(&f, MO_HOST_CONNECTION_RECEIVE, receiving, "most below 1 or above the receive limit");
    for (unsigned i = 0; i < MO_LIMIT_OPEN_CONNECTIONS; i++)
        drop_connection(&f, connections[i]);
    mo_value_drop(f.heap, f.types.text, dialing[1]);
    CHECK(close(listening) == 0);
    finish(&f, capability);
}

struct drain {
    int listening;
    size_t count;
    uint32_t sum;
};

static void *drain(void *argument) {
    struct drain *state = argument;
    int peer = accept(state->listening, NULL, NULL);
    MO_ASSERT(peer >= 0);
    uint8_t bytes[4096];
    for (;;) {
        ssize_t n = recv(peer, bytes, sizeof bytes, 0);
        MO_ASSERT(n >= 0);
        if (n == 0)
            break;
        for (ssize_t i = 0; i < n; i++)
            state->sum = state->sum * 31u + bytes[i];
        state->count += (size_t)n;
    }
    MO_ASSERT(close(peer) == 0);
    return NULL;
}

static void test_large_send(void) {
    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    struct mo_cell capability = network(&f);
    uint16_t port;
    struct drain state = {.listening = listener(&port)};
    pthread_t reader;
    MO_ASSERT(pthread_create(&reader, NULL, drain, &state) == 0);
    struct mo_cell connection = connected(&f, capability, port, 30000000);
    size_t size = (size_t)8 << 20;
    uint8_t *bytes = malloc(size);
    MO_ASSERT(bytes != NULL);
    uint32_t sum = 0;
    for (size_t i = 0; i < size; i++) {
        bytes[i] = (uint8_t)(i * 7u + i / 251u);
        sum = sum * 31u + bytes[i];
    }
    struct mo_cell arguments[] = {connection, mo_cell_unit()};
    MO_ASSERT(mo_value_bytes(f.heap, bytes, size, &arguments[1]));
    free(bytes);
    shows(&f, f.result_sent, call(&f, MO_HOST_CONNECTION_SEND, arguments), "Ok(Unit)");
    mo_value_drop(f.heap, f.bytes, arguments[1]);
    drop_connection(&f, connection);
    MO_ASSERT(pthread_join(reader, NULL) == 0);
    CHECK(state.count == size && state.sum == sum && close(state.listening) == 0);
    finish(&f, capability);
}

#define ASKED "GET /a HTTP/1.1\r\nAccept: a\r\n\r\n"
#define NO_REPLY "Error(Broken(reason: \"no reply for "

static struct mo_cell fake_network(struct host_fixture *f, const char *peer,
                                   const char *const *exchanges, uint32_t count) {
    struct mo_cell keys[2], values[2], replies, peers, name = host_text(f, peer);
    MO_ASSERT(count <= 2);
    for (uint32_t i = 0; i < count; i++) {
        const char *request = exchanges[2 * i], *reply = exchanges[2 * i + 1];
        MO_ASSERT(mo_value_bytes(f->heap, (const uint8_t *)request, strlen(request), &keys[i]) &&
                  mo_value_bytes(f->heap, (const uint8_t *)reply, strlen(reply), &values[i]));
    }
    MO_ASSERT(mo_value_map(f->heap, f->types.replies, keys, values, count, &replies) &&
              mo_value_map(f->heap, f->types.peers, &name, &replies, 1, &peers));
    for (uint32_t i = 0; i < count; i++) {
        mo_value_drop(f->heap, f->bytes, keys[i]);
        mo_value_drop(f->heap, f->bytes, values[i]);
    }
    mo_value_drop(f->heap, f->types.replies, replies);
    mo_value_drop(f->heap, f->types.text, name);
    return call(f, MO_HOST_NETWORK_FAKE, &peers);
}

static struct mo_cell asked(struct host_fixture *f, struct mo_cell capability) {
    return opened(f, dial(f, capability, "example.org", 11, 80, 0));
}

static void test_fake_reply(void) {
    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    static const char *const held[] = {ASKED, "0123456789"}, *const other[] = {ASKED, "zz"};
    struct mo_cell capability = fake_network(&f, "example.org:80", held, 1);
    struct mo_cell apart = fake_network(&f, "example.org:80", other, 1);
    shows(&f, f.result_connection, dial(&f, capability, "example.org", 11, 81, 0),
          "Error(UnknownHost(host: \"example.org\"))");
    shows(&f, f.result_connection, dial(&f, capability, "example.com", 11, 80, 0),
          "Error(UnknownHost(host: \"example.com\"))");
    shows(&f, f.result_connection, dial(&f, capability, "", 0, 80, 0),
          "Error(UnknownHost(host: \"\"))");
    struct mo_cell first = asked(&f, capability), second = asked(&f, capability),
                   third = asked(&f, apart);
    CHECK(f.host.open_connections == 0);
    sends(&f, first, "GET /a HTTP/1.1\r\n", "Ok(Unit)");
    sends(&f, first, "", "Ok(Unit)");
    sends(&f, first, "Accept: a\r\n\r\n", "Ok(Unit)");
    receives(&f, first, 4, "Ok(bytes[48, 49, 50, 51])");
    sends(&f, first, "more", "Error(Closed)");
    sends(&f, second, ASKED, "Ok(Unit)");
    receives(&f, second, 4, "Ok(bytes[48, 49, 50, 51])");
    receives(&f, first, 4, "Ok(bytes[52, 53, 54, 55])");
    receives(&f, first, 4, "Ok(bytes[56, 57])");
    receives(&f, first, 4, "Ok(bytes[])");
    receives(&f, first, 4, "Ok(bytes[])");
    receives(&f, second, MO_LIMIT_RECEIVE_BYTES, "Ok(bytes[52, 53, 54, 55, 56, 57])");
    sends(&f, third, ASKED, "Ok(Unit)");
    receives(&f, third, 4, "Ok(bytes[122, 122])");
    drop_connection(&f, first);
    drop_connection(&f, second);
    drop_connection(&f, third);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_NETWORK], apart);
    finish(&f, capability);
}

static void unanswered(struct host_fixture *f, struct mo_cell capability, const char *sent,
                       const char *expected) {
    struct mo_cell connection = asked(f, capability);
    if (sent != NULL)
        sends(f, connection, sent, "Ok(Unit)");
    receives(f, connection, 4, expected);
    sends(f, connection, ASKED, "Error(Closed)");
    receives(f, connection, 4, expected);
    drop_connection(f, connection);
}

static void test_fake_no_reply(void) {
    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    static const char *const held[] = {ASKED, "0123456789",
                                       "GET /a HTTP/1.1\r\nAccept: b\r\n\r\none", "1"};
    struct mo_cell capability = fake_network(&f, "example.org:80", held, 2);
    unanswered(&f, capability, NULL, NO_REPLY "a connection that sent nothing\"))");
    unanswered(&f, capability, "GET /b HTTP/1.1\r\n\r\n", NO_REPLY "\\\"GET /b HTTP/1.1\\\"\"))");
    unanswered(&f, capability, "GET /a HTTP/1.1\r\nAccept: c\r\n\r\n",
               NO_REPLY "\\\"GET /a HTTP/1.1\\\": sent \\\"Accept: c\\\\r\\\\n\\\", "
                        "expected \\\"Accept: a\\\\r\\\\n\\\"\"))");
    unanswered(&f, capability, "GET /a HTTP/1.1\r\nAccept: b\r\n\r\ntwo",
               NO_REPLY "\\\"GET /a HTTP/1.1\\\": sent \\\"two\\\", expected \\\"one\\\"\"))");
    unanswered(&f, capability, "GET /a HTTP/1.1\r\nAccept: a\r\n",
               NO_REPLY "\\\"GET /a HTTP/1.1\\\": sent nothing more, "
                        "expected \\\"\\\\r\\\\n\\\"\"))");
    unanswered(&f, capability, ASKED "\x01\"\\\xc3\xa9",
               NO_REPLY
               "\\\"GET /a HTTP/1.1\\\": sent \\\"\\\\x01\\\\\\\"\\\\\\\\\\\\xc3\\\\xa9\\\", "
               "expected nothing more\"))");
    char line[MO_LIMIT_FAKE_QUOTE + 2], expected[MO_LIMIT_FAKE_QUOTE + 64];
    memset(line, 'x', sizeof line - 1);
    line[sizeof line - 1] = 0;
    CHECK(snprintf(expected, sizeof expected, NO_REPLY "\\\"%.*s...\\\"\"))", MO_LIMIT_FAKE_QUOTE,
                   line) < (int)sizeof expected);
    unanswered(&f, capability, line, expected);
    line[MO_LIMIT_FAKE_QUOTE] = 0;
    CHECK(snprintf(expected, sizeof expected, NO_REPLY "\\\"%s\\\"\"))", line) <
          (int)sizeof expected);
    unanswered(&f, capability, line, expected);
    finish(&f, capability);
}

static void test_fake_snapshot(void) {
    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    static const char *const held[] = {ASKED, "0123456789"};
    struct mo_cell capability = fake_network(&f, "example.org:80", held, 1), copy;
    struct mo_host_fake_state state;
    struct mo_host_error error;
    MO_ASSERT(mo_host_fake_snapshot(&f.host, f.heap, capability, &state));
    CHECK(state.kind == MO_CAP_NETWORK);
    MO_ASSERT(mo_host_fake_restore(&f.host, f.heap, f.capabilities[MO_CAP_NETWORK], &state, &copy,
                                   &error) == MO_HOST_RETURNED);
    mo_host_fake_state_drop(&f.host, f.heap, &state);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_NETWORK], capability);
    struct mo_cell connection = asked(&f, copy);
    sends(&f, connection, ASKED, "Ok(Unit)");
    receives(&f, connection, 10, "Ok(bytes[48, 49, 50, 51, 52, 53, 54, 55, 56, 57])");
    shows(&f, f.result_connection, dial(&f, copy, "example.org", 11, 81, 0),
          "Error(UnknownHost(host: \"example.org\"))");
    drop_connection(&f, connection);
    finish(&f, copy);
}

static void test_fake_bugs(void) {
    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    static const char *const held[] = {ASKED, "0123456789"};
    struct mo_cell capability = fake_network(&f, "example.org:80", held, 1);
    struct mo_cell dialing[] = {capability, host_text(&f, "example.org"), mo_cell_int(0),
                                mo_cell_duration(0)};
    refuses(&f, MO_HOST_NETWORK_CONNECT, dialing, "port outside 1 to 65535");
    dialing[2] = mo_cell_int(65536);
    refuses(&f, MO_HOST_NETWORK_CONNECT, dialing, "port outside 1 to 65535");
    mo_value_drop(f.heap, f.types.text, dialing[1]);
    struct mo_cell receiving[] = {asked(&f, capability), mo_cell_int(0)};
    refuses(&f, MO_HOST_CONNECTION_RECEIVE, receiving, "most below 1 or above the receive limit");
    receiving[1] = mo_cell_int((int64_t)MO_LIMIT_RECEIVE_BYTES + 1);
    refuses(&f, MO_HOST_CONNECTION_RECEIVE, receiving, "most below 1 or above the receive limit");
    drop_connection(&f, receiving[0]);
    finish(&f, capability);
}

int main(void) {
    test_round_trip();
    test_errors();
    test_deadline();
    test_reset();
    test_bugs();
    test_large_send();
    test_fake_reply();
    test_fake_no_reply();
    test_fake_snapshot();
    test_fake_bugs();
    return mo_test_report("test_network");
}
