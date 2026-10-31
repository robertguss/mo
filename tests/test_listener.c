#define _POSIX_C_SOURCE 200809L
#include "host/host.h"
#include "test.h"

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static const char environment[] = "MO_TEST_LISTENER_BEARER";
static const char secret[] = "ListenerCanarySecret_7";

static struct mo_listener_time now(struct mo_listener *listener) {
    union mo_listener_result result;
    CHECK(mo_listener_call(listener, MO_LISTENER_NOW, NULL, &result) == MO_LISTENER_OK);
    return result.now;
}

static struct mo_listener_key accept_peer(struct mo_listener *listener) {
    union mo_listener_result result = {0};
    CHECK(mo_listener_call(listener, MO_LISTENER_ACCEPT, NULL, &result) == MO_LISTENER_OK);
    return result.accepted;
}

static enum mo_listener_status close_peer(struct mo_listener *listener,
                                          struct mo_listener_key connection) {
    union mo_listener_request request = {.connection = connection};
    union mo_listener_result result;
    return mo_listener_call(listener, MO_LISTENER_CLOSE, &request, &result);
}

static void credentials(void) {
    struct mo_listener *listener = NULL;
    struct mo_listener_config config = {.address = "localhost"};
    CHECK(unsetenv(environment) == 0);
    CHECK(mo_listener_open(&config, environment, &listener) == MO_LISTENER_CREDENTIAL);
    CHECK(listener == NULL);
    CHECK(mo_listener_fake(environment, &listener) == MO_LISTENER_CREDENTIAL);
    CHECK(listener == NULL);
    CHECK(setenv(environment, "bad token", 1) == 0);
    CHECK(mo_listener_fake(environment, &listener) == MO_LISTENER_CREDENTIAL);
    CHECK(setenv(environment, secret, 1) == 0);
    CHECK(mo_listener_fake(environment, &listener) == MO_LISTENER_OK);
    if (listener == NULL)
        return;
    CHECK(setenv(environment, "ChangedAfterOpen", 1) == 0);
    uint8_t candidate[sizeof secret + 4];
    memset(candidate, '!', sizeof candidate);
    memcpy(candidate + 2, secret, sizeof secret - 1);
    union mo_listener_request request = {
        .auth = {
            .bytes = candidate, .size = sizeof candidate, .start = 2, .length = sizeof secret - 1}};
    union mo_listener_result result;
    CHECK(mo_listener_call(listener, MO_LISTENER_AUTH, &request, &result) == MO_LISTENER_OK);
    CHECK(result.authenticated);
    request.auth.start = 1;
    CHECK(mo_listener_call(listener, MO_LISTENER_AUTH, &request, &result) == MO_LISTENER_OK);
    CHECK(!result.authenticated);
    request.auth = (struct mo_listener_auth){0};
    CHECK(mo_listener_call(listener, MO_LISTENER_AUTH, &request, &result) == MO_LISTENER_OK);
    CHECK(!result.authenticated);
    request.auth = (struct mo_listener_auth){
        .bytes = candidate, .size = sizeof candidate, .start = SIZE_MAX, .length = 2};
    CHECK(mo_listener_call(listener, MO_LISTENER_AUTH, &request, &result) == MO_LISTENER_CONFIG);
    CHECK(!result.authenticated);
    request.auth.start = 0;
    request.auth.length = sizeof candidate + 1;
    CHECK(mo_listener_call(listener, MO_LISTENER_AUTH, &request, &result) == MO_LISTENER_CONFIG);
    request.auth = (struct mo_listener_auth){.size = 1, .length = 1};
    CHECK(mo_listener_call(listener, MO_LISTENER_AUTH, &request, &result) == MO_LISTENER_CONFIG);
    mo_listener_close(listener);
    CHECK(setenv(environment, secret, 1) == 0);
}

static void configuration(void) {
    const char *invalid[] = {"localhost", "127.0.0.1.example", "", "::1%lo", "0.0.0.0", "::",
                             "192.0.2.1", "::ffff:127.0.0.1"};
    struct mo_listener *listener = NULL;
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        struct mo_listener_config config = {.address = invalid[i]};
        CHECK(mo_listener_open(&config, environment, &listener) == MO_LISTENER_CONFIG);
        CHECK(listener == NULL);
    }
    struct mo_listener_config config = {.address = "0.0.0.0", .expose = true};
    CHECK(mo_listener_open(&config, environment, &listener) == MO_LISTENER_OK);
    if (listener != NULL) {
        CHECK(mo_listener_port(listener) != 0);
        struct mo_listener *retained = listener;
        config.port = mo_listener_port(listener);
        CHECK(mo_listener_open(&config, environment, &listener) == MO_LISTENER_IO);
        CHECK(listener == retained);
        mo_listener_close(listener);
    }
    listener = NULL;
    CHECK(mo_listener_open(NULL, environment, &listener) == MO_LISTENER_CONFIG);
    CHECK(listener == NULL);
    mo_listener_close(NULL);
}

static void fake_io(void) {
    struct mo_listener *listener = NULL, *other = NULL;
    CHECK(mo_listener_fake(environment, &listener) == MO_LISTENER_OK);
    CHECK(mo_listener_fake(environment, &other) == MO_LISTENER_OK);
    if (listener == NULL || other == NULL) {
        mo_listener_close(listener);
        mo_listener_close(other);
        return;
    }
    CHECK(mo_listener_port(listener) == 0);
    uint8_t output[5] = {0}, input[4] = {0};
    struct mo_listener_fake_peer peer = {.input = (const uint8_t *)"abcdef",
                                         .input_size = 6,
                                         .output = output,
                                         .output_capacity = sizeof output,
                                         .read_quantum = 2,
                                         .write_quantum = 2,
                                         .readable = true,
                                         .writable = true};
    union mo_listener_request request = {0};
    union mo_listener_result result;
    CHECK(mo_listener_call(listener, MO_LISTENER_ACCEPT, NULL, &result) == MO_LISTENER_AGAIN);
    CHECK(mo_listener_fake_connect(listener, &peer) == MO_LISTENER_OK);
    CHECK(mo_listener_fake_connect(listener, &peer) == MO_LISTENER_CONFIG);
    struct mo_listener_key connection = accept_peer(listener);
    CHECK(close_peer(other, connection) == MO_LISTENER_STALE);
    request.read = (struct mo_listener_read){connection, input, sizeof input};
    CHECK(mo_listener_call(listener, MO_LISTENER_READ, &request, &result) == MO_LISTENER_OK);
    CHECK(result.transferred == 2 && memcmp(input, "ab", 2) == 0 && peer.input_offset == 2);
    peer.readable = false;
    CHECK(mo_listener_call(listener, MO_LISTENER_READ, &request, &result) == MO_LISTENER_AGAIN);
    CHECK(result.transferred == 0 && peer.input_offset == 2);
    peer.eof = true;
    request.wait = (struct mo_listener_wait){.until_us = 10, .count = 1};
    request.wait.interests[0] = (struct mo_listener_interest){connection, MO_LISTENER_READABLE};
    CHECK(mo_listener_call(listener, MO_LISTENER_WAIT, &request, &result) == MO_LISTENER_OK);
    CHECK(result.wait.events[0] == 0);
    peer.eof = false;
    request.read = (struct mo_listener_read){connection, input, sizeof input};
    peer.readable = true;
    peer.read_error = true;
    CHECK(mo_listener_call(listener, MO_LISTENER_READ, &request, &result) == MO_LISTENER_IO);
    peer.read_error = false;
    CHECK(mo_listener_call(listener, MO_LISTENER_READ, &request, &result) == MO_LISTENER_OK);
    CHECK(result.transferred == 2 && memcmp(input, "cd", 2) == 0);
    CHECK(mo_listener_call(listener, MO_LISTENER_READ, &request, &result) == MO_LISTENER_OK);
    CHECK(result.transferred == 2 && memcmp(input, "ef", 2) == 0);
    CHECK(mo_listener_call(listener, MO_LISTENER_READ, &request, &result) == MO_LISTENER_AGAIN);
    peer.eof = true;
    CHECK(mo_listener_call(listener, MO_LISTENER_READ, &request, &result) == MO_LISTENER_EOF);
    request.write = (struct mo_listener_write){connection, (const uint8_t *)"ABCDE", 5};
    peer.writable = false;
    CHECK(mo_listener_call(listener, MO_LISTENER_WRITE, &request, &result) == MO_LISTENER_AGAIN);
    peer.writable = true;
    peer.write_error = true;
    CHECK(mo_listener_call(listener, MO_LISTENER_WRITE, &request, &result) == MO_LISTENER_IO);
    peer.write_error = false;
    CHECK(mo_listener_call(listener, MO_LISTENER_WRITE, &request, &result) == MO_LISTENER_OK);
    CHECK(result.transferred == 2 && peer.output_size == 2 && memcmp(output, "AB", 2) == 0);
    request.write.bytes += 2;
    request.write.size -= 2;
    CHECK(mo_listener_call(listener, MO_LISTENER_WRITE, &request, &result) == MO_LISTENER_OK);
    CHECK(result.transferred == 2);
    request.write.bytes += 2;
    request.write.size -= 2;
    CHECK(mo_listener_call(listener, MO_LISTENER_WRITE, &request, &result) == MO_LISTENER_OK);
    CHECK(result.transferred == 1 && memcmp(output, "ABCDE", 5) == 0);
    CHECK(mo_listener_call(listener, MO_LISTENER_WRITE, &request, &result) == MO_LISTENER_AGAIN);
    request.connection = connection;
    CHECK(mo_listener_call(listener, MO_LISTENER_HALF_CLOSE, &request, &result) == MO_LISTENER_OK);
    CHECK(peer.half_closed && !peer.closed);
    request.write = (struct mo_listener_write){connection, (const uint8_t *)"x", 1};
    CHECK(mo_listener_call(listener, MO_LISTENER_WRITE, &request, &result) == MO_LISTENER_IO);
    request.read = (struct mo_listener_read){connection, input, sizeof input};
    CHECK(mo_listener_call(listener, MO_LISTENER_READ, &request, &result) == MO_LISTENER_EOF);
    CHECK(close_peer(listener, connection) == MO_LISTENER_OK);
    CHECK(peer.closed);
    CHECK(close_peer(listener, connection) == MO_LISTENER_STALE);
    CHECK(mo_listener_fake_connect(listener, &peer) == MO_LISTENER_OK);
    struct mo_listener_key replacement = accept_peer(listener);
    CHECK(replacement.slot == connection.slot && replacement.generation != connection.generation);
    CHECK(peer.input_offset == 0 && peer.output_size == 0 && !peer.closed && !peer.half_closed);
    CHECK(close_peer(listener, connection) == MO_LISTENER_STALE);
    CHECK(close_peer(listener, replacement) == MO_LISTENER_OK);
    for (unsigned i = 0; i < 32; i++) {
        CHECK(mo_listener_fake_connect(listener, &peer) == MO_LISTENER_OK);
        CHECK(close_peer(listener, accept_peer(listener)) == MO_LISTENER_OK);
    }
    mo_listener_close(other);
    CHECK(mo_listener_fake_connect(listener, &peer) == MO_LISTENER_OK);
    mo_listener_close(listener);
    CHECK(peer.closed);
}

static void fake_wait_and_limit(void) {
    struct mo_listener *listener = NULL;
    CHECK(mo_listener_fake(environment, &listener) == MO_LISTENER_OK);
    if (listener == NULL)
        return;
    struct mo_listener_fake_peer peers[MO_LIMIT_MCP_CONNECTIONS + 1] = {0};
    struct mo_listener_key connections[MO_LIMIT_MCP_CONNECTIONS];
    for (size_t i = 0; i < MO_LIMIT_MCP_CONNECTIONS; i++)
        CHECK(mo_listener_fake_connect(listener, &peers[i]) == MO_LISTENER_OK);
    CHECK(mo_listener_fake_connect(listener, &peers[MO_LIMIT_MCP_CONNECTIONS]) == MO_LISTENER_FULL);
    union mo_listener_request request = {.wait = {.until_us = 5000, .accept = true}};
    union mo_listener_result result;
    CHECK(mo_listener_call(listener, MO_LISTENER_WAIT, &request, &result) == MO_LISTENER_OK);
    CHECK(result.wait.accept && result.wait.now.monotonic_us == 0);
    for (size_t i = 0; i < MO_LIMIT_MCP_CONNECTIONS; i++)
        connections[i] = accept_peer(listener);
    CHECK(mo_listener_call(listener, MO_LISTENER_ACCEPT, NULL, &result) == MO_LISTENER_FULL);
    CHECK(now(listener).wall_us == 0);
    CHECK(mo_listener_fake_advance(listener, 1234) == MO_LISTENER_OK);
    CHECK(now(listener).monotonic_us == 1234 && now(listener).wall_us == 1234);
    request.wait = (struct mo_listener_wait){.until_us = 5000, .accept = true, .count = 1};
    request.wait.interests[0] = (struct mo_listener_interest){connections[0], MO_LISTENER_READABLE};
    CHECK(mo_listener_call(listener, MO_LISTENER_WAIT, &request, &result) == MO_LISTENER_OK);
    CHECK(!result.wait.accept && result.wait.events[0] == 0 &&
          result.wait.now.monotonic_us == 1234);
    peers[0].eof = true;
    CHECK(mo_listener_call(listener, MO_LISTENER_WAIT, &request, &result) == MO_LISTENER_OK);
    CHECK(result.wait.events[0] == MO_LISTENER_READABLE);
    peers[0].read_error = true;
    CHECK(mo_listener_call(listener, MO_LISTENER_WAIT, &request, &result) == MO_LISTENER_OK);
    CHECK((result.wait.events[0] & MO_LISTENER_ERROR) != 0);
    request.wait.count = MO_LIMIT_MCP_CONNECTIONS + 1;
    CHECK(mo_listener_call(listener, MO_LISTENER_WAIT, &request, &result) == MO_LISTENER_CONFIG);
    request.wait.count = 1;
    request.wait.interests[0].events = 128;
    CHECK(mo_listener_call(listener, MO_LISTENER_WAIT, &request, &result) == MO_LISTENER_CONFIG);
    CHECK(close_peer(listener, connections[0]) == MO_LISTENER_OK);
    request.wait.interests[0].events = MO_LISTENER_READABLE;
    CHECK(mo_listener_call(listener, MO_LISTENER_WAIT, &request, &result) == MO_LISTENER_STALE);
    CHECK(mo_listener_fake_advance(listener, UINT64_MAX) == MO_LISTENER_CONFIG);
    CHECK(now(listener).monotonic_us == 1234);
    mo_listener_close(listener);
    for (size_t i = 0; i < MO_LIMIT_MCP_CONNECTIONS; i++)
        CHECK(peers[i].closed);
}

static int client_socket(const char *address, uint16_t port) {
    int family = address == NULL ? AF_INET : AF_INET6;
    int fd = socket(family, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    if (fd < 0)
        return -1;
    struct timeval timeout = {.tv_sec = 5};
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) == 0);
    CHECK(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout) == 0);
    int connected;
    if (family == AF_INET) {
        struct sockaddr_in target = {.sin_family = AF_INET, .sin_port = htons(port)};
        CHECK(inet_pton(AF_INET, "127.0.0.1", &target.sin_addr) == 1);
        connected = connect(fd, (const struct sockaddr *)&target, sizeof target);
    } else {
        struct sockaddr_in6 target = {.sin6_family = AF_INET6, .sin6_port = htons(port)};
        CHECK(inet_pton(AF_INET6, address, &target.sin6_addr) == 1);
        connected = connect(fd, (const struct sockaddr *)&target, sizeof target);
    }
    CHECK(connected == 0);
    if (connected != 0) {
        CHECK(close(fd) == 0);
        return -1;
    }
    return fd;
}

static bool ipv6_available(void) {
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) {
        CHECK(errno == EAFNOSUPPORT || errno == EPROTONOSUPPORT);
        return false;
    }
    struct sockaddr_in6 address = {.sin6_family = AF_INET6, .sin6_addr = IN6ADDR_LOOPBACK_INIT};
    bool available = bind(fd, (const struct sockaddr *)&address, sizeof address) == 0;
    if (!available)
        CHECK(errno == EADDRNOTAVAIL || errno == EAFNOSUPPORT);
    CHECK(close(fd) == 0);
    return available;
}

static void real_io(const char *address) {
    struct sigaction before, after;
    CHECK(sigaction(SIGPIPE, NULL, &before) == 0);
    struct mo_listener_config config = {.address = address};
    struct mo_listener *listener = NULL;
    enum mo_listener_status status = mo_listener_open(&config, environment, &listener);
    CHECK(status == MO_LISTENER_OK);
    if (listener == NULL)
        return;
    CHECK(mo_listener_port(listener) != 0);
    CHECK(mo_listener_fake_advance(listener, 1) == MO_LISTENER_CONFIG);
    struct mo_listener_time first = now(listener);
    CHECK(first.monotonic_us > 0 && first.wall_us > 0);
    int client = client_socket(address, mo_listener_port(listener));
    if (client < 0) {
        mo_listener_close(listener);
        return;
    }
    union mo_listener_request request = {
        .wait = {.until_us = first.monotonic_us + 1000000, .accept = true}};
    union mo_listener_result result;
    CHECK(mo_listener_call(listener, MO_LISTENER_WAIT, &request, &result) == MO_LISTENER_OK);
    CHECK(result.wait.accept && result.wait.now.monotonic_us >= first.monotonic_us);
    struct mo_listener_key connection = accept_peer(listener);
    uint8_t input[8] = {0};
    request.read = (struct mo_listener_read){connection, input, sizeof input};
    CHECK(mo_listener_call(listener, MO_LISTENER_READ, &request, &result) == MO_LISTENER_AGAIN);
    CHECK(send(client, "abcdef", 6, 0) == 6);
    request.wait =
        (struct mo_listener_wait){.until_us = now(listener).monotonic_us + 1000000, .count = 1};
    request.wait.interests[0] = (struct mo_listener_interest){connection, MO_LISTENER_READABLE};
    CHECK(mo_listener_call(listener, MO_LISTENER_WAIT, &request, &result) == MO_LISTENER_OK);
    CHECK((result.wait.events[0] & MO_LISTENER_READABLE) != 0);
    request.read = (struct mo_listener_read){connection, input, 2};
    CHECK(mo_listener_call(listener, MO_LISTENER_READ, &request, &result) == MO_LISTENER_OK);
    CHECK(result.transferred == 2 && memcmp(input, "ab", 2) == 0);
    request.read.capacity = sizeof input;
    CHECK(mo_listener_call(listener, MO_LISTENER_READ, &request, &result) == MO_LISTENER_OK);
    CHECK(result.transferred == 4 && memcmp(input, "cdef", 4) == 0);
    request.write = (struct mo_listener_write){connection, (const uint8_t *)"reply", 5};
    CHECK(mo_listener_call(listener, MO_LISTENER_WRITE, &request, &result) == MO_LISTENER_OK);
    CHECK(result.transferred == 5);
    CHECK(recv(client, input, 5, MSG_WAITALL) == 5 && memcmp(input, "reply", 5) == 0);
    request.connection = connection;
    CHECK(mo_listener_call(listener, MO_LISTENER_HALF_CLOSE, &request, &result) == MO_LISTENER_OK);
    CHECK(recv(client, input, sizeof input, 0) == 0);
    CHECK(send(client, "drain", 5, 0) == 5);
    CHECK(shutdown(client, SHUT_WR) == 0);
    request.wait =
        (struct mo_listener_wait){.until_us = now(listener).monotonic_us + 1000000, .count = 1};
    request.wait.interests[0] = (struct mo_listener_interest){connection, MO_LISTENER_READABLE};
    CHECK(mo_listener_call(listener, MO_LISTENER_WAIT, &request, &result) == MO_LISTENER_OK);
    request.read = (struct mo_listener_read){connection, input, sizeof input};
    CHECK(mo_listener_call(listener, MO_LISTENER_READ, &request, &result) == MO_LISTENER_OK);
    CHECK(result.transferred == 5 && memcmp(input, "drain", 5) == 0);
    request.wait =
        (struct mo_listener_wait){.until_us = now(listener).monotonic_us + 1000000, .count = 1};
    request.wait.interests[0] = (struct mo_listener_interest){connection, MO_LISTENER_READABLE};
    CHECK(mo_listener_call(listener, MO_LISTENER_WAIT, &request, &result) == MO_LISTENER_OK);
    request.read = (struct mo_listener_read){connection, input, sizeof input};
    CHECK(mo_listener_call(listener, MO_LISTENER_READ, &request, &result) == MO_LISTENER_EOF);
    CHECK(close_peer(listener, connection) == MO_LISTENER_OK);
    CHECK(close_peer(listener, connection) == MO_LISTENER_STALE);
    CHECK(close(client) == 0);
    CHECK(sigaction(SIGPIPE, NULL, &after) == 0);
    CHECK(before.sa_handler == after.sa_handler);
    config.port = mo_listener_port(listener);
    mo_listener_close(listener);
    listener = NULL;
    CHECK(mo_listener_open(&config, environment, &listener) == MO_LISTENER_OK);
    CHECK(listener != NULL);
    mo_listener_close(listener);
}

static volatile sig_atomic_t sigpipes;

static void record_sigpipe(int signal_number) {
    (void)signal_number;
    sigpipes++;
}

static void real_limit_and_backpressure(void) {
    struct mo_listener_config config = {0};
    struct mo_listener *listener = NULL;
    CHECK(mo_listener_open(&config, environment, &listener) == MO_LISTENER_OK);
    if (listener == NULL)
        return;
    int clients[MO_LIMIT_MCP_CONNECTIONS + 1];
    struct mo_listener_key connections[MO_LIMIT_MCP_CONNECTIONS];
    union mo_listener_request request;
    union mo_listener_result result;
    for (size_t i = 0; i < MO_LIMIT_MCP_CONNECTIONS; i++) {
        clients[i] = client_socket(NULL, mo_listener_port(listener));
        request.wait = (struct mo_listener_wait){.until_us = now(listener).monotonic_us + 1000000,
                                                 .accept = true};
        CHECK(mo_listener_call(listener, MO_LISTENER_WAIT, &request, &result) == MO_LISTENER_OK);
        CHECK(result.wait.accept);
        connections[i] = accept_peer(listener);
    }
    clients[MO_LIMIT_MCP_CONNECTIONS] = client_socket(NULL, mo_listener_port(listener));
    CHECK(mo_listener_call(listener, MO_LISTENER_ACCEPT, NULL, &result) == MO_LISTENER_FULL);
    request.wait =
        (struct mo_listener_wait){.until_us = now(listener).monotonic_us, .accept = true};
    CHECK(mo_listener_call(listener, MO_LISTENER_WAIT, &request, &result) == MO_LISTENER_OK);
    CHECK(!result.wait.accept);
    CHECK(close_peer(listener, connections[0]) == MO_LISTENER_OK);
    CHECK(close(clients[0]) == 0);
    struct mo_listener_key replacement = accept_peer(listener);
    CHECK(replacement.slot == connections[0].slot &&
          replacement.generation != connections[0].generation);
    CHECK(close_peer(listener, connections[0]) == MO_LISTENER_STALE);
    static uint8_t payload[MO_LIMIT_MCP_REQUEST_MEMORY];
    request.write = (struct mo_listener_write){replacement, payload, sizeof payload};
    bool partial = false, blocked = false;
    for (unsigned i = 0; i < 128; i++) {
        enum mo_listener_status status =
            mo_listener_call(listener, MO_LISTENER_WRITE, &request, &result);
        if (status == MO_LISTENER_AGAIN) {
            blocked = true;
            CHECK(result.transferred == 0);
            break;
        }
        CHECK(status == MO_LISTENER_OK);
        CHECK(result.transferred > 0 && result.transferred <= sizeof payload);
        partial |= result.transferred < sizeof payload;
    }
    CHECK(partial);
    CHECK(blocked);
    struct linger reset = {.l_onoff = 1, .l_linger = 0};
    CHECK(setsockopt(clients[MO_LIMIT_MCP_CONNECTIONS], SOL_SOCKET, SO_LINGER, &reset,
                     sizeof reset) == 0);
    CHECK(close(clients[MO_LIMIT_MCP_CONNECTIONS]) == 0);
    request.wait =
        (struct mo_listener_wait){.until_us = now(listener).monotonic_us + 1000000, .count = 1};
    request.wait.interests[0] = (struct mo_listener_interest){replacement, MO_LISTENER_READABLE};
    CHECK(mo_listener_call(listener, MO_LISTENER_WAIT, &request, &result) == MO_LISTENER_OK);
    CHECK(result.wait.events[0] != 0);
    struct sigaction old, action = {.sa_handler = record_sigpipe};
    CHECK(sigemptyset(&action.sa_mask) == 0);
    CHECK(sigaction(SIGPIPE, &action, &old) == 0);
    request.write = (struct mo_listener_write){replacement, payload, 1};
    CHECK(mo_listener_call(listener, MO_LISTENER_WRITE, &request, &result) == MO_LISTENER_IO);
    CHECK(mo_listener_call(listener, MO_LISTENER_WRITE, &request, &result) == MO_LISTENER_IO);
    CHECK(sigpipes == 0);
    CHECK(sigaction(SIGPIPE, &old, NULL) == 0);
    CHECK(close_peer(listener, replacement) == MO_LISTENER_OK);
    mo_listener_close(listener);
    for (size_t i = 1; i < MO_LIMIT_MCP_CONNECTIONS; i++)
        CHECK(close(clients[i]) == 0);
}

int main(void) {
    credentials();
    configuration();
    fake_io();
    fake_wait_and_limit();
    real_io(NULL);
    if (ipv6_available())
        real_io("::1");
    else
        CHECK(printf("listener: IPv6 loopback unavailable\n") >= 0);
    real_limit_and_backpressure();
    CHECK(unsetenv(environment) == 0);
    return mo_test_report("listener");
}
