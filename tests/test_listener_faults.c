#define _GNU_SOURCE
#define MO_PROTOCOL_SERVER_FAULT_TEST
#include "test_protocol_server.c"

#include <arpa/inet.h>
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

static int accept_error;

#if defined(__linux__)
int mo_listener_fault_accept4(int fd, struct sockaddr *address, socklen_t *length, int flags);
#endif
int mo_listener_fault_accept(int fd, struct sockaddr *address, socklen_t *length);

#if defined(__linux__)
int mo_listener_fault_accept4(int fd, struct sockaddr *address, socklen_t *length, int flags) {
    if (accept_error != 0) {
        errno = accept_error;
        accept_error = 0;
        return -1;
    }
    return accept4(fd, address, length, flags);
}

#endif
int mo_listener_fault_accept(int fd, struct sockaddr *address, socklen_t *length) {
    if (accept_error != 0) {
        errno = accept_error;
        accept_error = 0;
        return -1;
    }
    return accept(fd, address, length);
}

static void retry_accept(void *context) {
    wire(context);
    char id[6 * MO_LIMIT_MCP_ID_BYTES + 3];
    maximum_id(id);
    struct fixture_server f = start();
    mo_protocol_server_free(f.server);
    mo_listener_close(f.listener);
    struct mo_listener_config config = {0};
    MO_ASSERT(mo_listener_open(&config, "MO_TEST_MCP_TOKEN", &f.listener) == MO_LISTENER_OK);
    const char *authorities[] = {"localhost"};
    MO_ASSERT(mo_protocol_server_new(f.image, 0, f.store, f.listener, authorities, 1, &f.server) ==
              MO_PROTOCOL_SERVER_OK);
    const int errors[] = {
        ECONNABORTED,
        EPROTO,
        ENETDOWN,
        ENOPROTOOPT,
#if defined(EHOSTDOWN)
        EHOSTDOWN,
#endif
#if defined(ENONET)
        ENONET,
#endif
        EHOSTUNREACH,
        EOPNOTSUPP,
        ENETUNREACH,
        ENOBUFS,
        ENOMEM,
        EINTR
    };
    for (size_t i = 0; i < sizeof errors / sizeof *errors; i++) {
        int client = socket(AF_INET, SOCK_STREAM, 0);
        MO_ASSERT(client >= 0);
        struct sockaddr_in target = {.sin_family = AF_INET,
                                     .sin_port = htons(mo_listener_port(f.listener))};
        MO_ASSERT(inet_pton(AF_INET, "127.0.0.1", &target.sin_addr) == 1);
        MO_ASSERT(connect(client, (const struct sockaddr *)&target, sizeof target) == 0);
        struct mo_buffer input;
        input_request(&input, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}", auth);
        CHECK(send(client, input.bytes, input.length, 0) == (ssize_t)input.length);
        CHECK(shutdown(client, SHUT_WR) == 0);
        accept_error = errors[i];
        CHECK(mo_protocol_server_poll(f.server, 0) == MO_PROTOCOL_SERVER_OK);
        CHECK(accept_error == 0);
        for (unsigned n = 0; n < 100; n++)
            CHECK(mo_protocol_server_poll(f.server, 0) == MO_PROTOCOL_SERVER_OK);
        char out[4096] = {0};
        ssize_t count = recv(client, out, sizeof out - 1, MSG_DONTWAIT);
        CHECK(count > 0);
        CHECK(strstr(out, "HTTP/1.1 200 ") == out);
        CHECK(strstr(out, "\"id\":1,\"result\":{}") != NULL);
        CHECK(close(client) == 0);
        mo_buffer_free(&input);
    }
    int client = socket(AF_INET, SOCK_STREAM, 0);
    MO_ASSERT(client >= 0);
    struct sockaddr_in target = {.sin_family = AF_INET,
                                 .sin_port = htons(mo_listener_port(f.listener))};
    MO_ASSERT(inet_pton(AF_INET, "127.0.0.1", &target.sin_addr) == 1);
    MO_ASSERT(connect(client, (const struct sockaddr *)&target, sizeof target) == 0);
    accept_error = EBADF;
    CHECK(mo_protocol_server_poll(f.server, 0) == MO_PROTOCOL_SERVER_IO);
    CHECK(accept_error == 0);
    CHECK(close(client) == 0);
    finish(&f);
}

int main(void) {
    MO_ASSERT(mo_with_stack(MO_SOURCE_STACK_SIZE, retry_accept, NULL) == 0);
    return mo_test_report("listener_faults");
}
