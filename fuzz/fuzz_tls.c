#define _POSIX_C_SOURCE 200809L
#include "../tests/host_fixture.h"

#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    int listening;
    uint16_t port;
    const uint8_t *bytes;
    size_t size;
    bool pending;
    bool finished;
} peer_state = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .changed = PTHREAD_COND_INITIALIZER,
    .listening = -1,
};

static void *serve_raw_tls(void *unused) {
    (void)unused;
    for (;;) {
        MO_ASSERT(pthread_mutex_lock(&peer_state.lock) == 0);
        while (!peer_state.pending)
            MO_ASSERT(pthread_cond_wait(&peer_state.changed, &peer_state.lock) == 0);
        const uint8_t *bytes = peer_state.bytes;
        size_t size = peer_state.size;
        peer_state.pending = false;
        MO_ASSERT(pthread_mutex_unlock(&peer_state.lock) == 0);

        int fd = accept(peer_state.listening, NULL, NULL);
        MO_ASSERT(fd >= 0);
        struct pollfd in = {.fd = fd, .events = POLLIN};
        if (poll(&in, 1, 50) > 0) {
            uint8_t client_hello[1024];
            (void)recv(fd, client_hello, sizeof client_hello, MSG_DONTWAIT);
        }
        size_t written = 0;
        while (written < size) {
            ssize_t n = send(fd, bytes + written, size - written, MSG_DONTWAIT);
            if (n > 0) {
                written += (size_t)n;
                continue;
            }
            if (errno == EPIPE || errno == ECONNRESET)
                break;
            MO_ASSERT(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
            struct pollfd out = {.fd = fd, .events = POLLOUT};
            if (poll(&out, 1, 50) <= 0)
                break;
        }
        MO_ASSERT(close(fd) == 0);

        MO_ASSERT(pthread_mutex_lock(&peer_state.lock) == 0);
        peer_state.finished = true;
        MO_ASSERT(pthread_cond_signal(&peer_state.changed) == 0);
        MO_ASSERT(pthread_mutex_unlock(&peer_state.lock) == 0);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (peer_state.listening < 0) {
        peer_state.listening = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in address = {.sin_family = AF_INET,
                                      .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        socklen_t length = sizeof address;
        MO_ASSERT(peer_state.listening >= 0 &&
                  bind(peer_state.listening, (const struct sockaddr *)&address, length) == 0 &&
                  listen(peer_state.listening, 16) == 0 &&
                  getsockname(peer_state.listening, (struct sockaddr *)&address, &length) == 0);
        peer_state.port = ntohs(address.sin_port);
        pthread_t thread;
        MO_ASSERT(pthread_create(&thread, NULL, serve_raw_tls, NULL) == 0);
        MO_ASSERT(pthread_detach(thread) == 0);
    }

    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    MO_ASSERT(mo_host_start(&f.host) == NULL);
    struct mo_cell network, result;
    struct mo_host_error error;
    MO_ASSERT(mo_host_real(&f.host, f.heap, f.capabilities[MO_CAP_NETWORK], NULL, -1, &network,
                           &error) == MO_HOST_RETURNED);

    MO_ASSERT(pthread_mutex_lock(&peer_state.lock) == 0);
    peer_state.bytes = data;
    peer_state.size = size;
    peer_state.pending = true;
    peer_state.finished = false;
    MO_ASSERT(pthread_cond_signal(&peer_state.changed) == 0);
    MO_ASSERT(pthread_mutex_unlock(&peer_state.lock) == 0);

    struct mo_cell dialing[] = {network, host_text(&f, "127.0.0.1"), mo_cell_int(peer_state.port),
                                mo_cell_duration(200000)};
    MO_ASSERT(mo_host_call(&f.host, f.heap, MO_HOST_NETWORK_CONNECT_SECURE, f.result_connection,
                           dialing, &result, &error) == MO_HOST_RETURNED);

    MO_ASSERT(pthread_mutex_lock(&peer_state.lock) == 0);
    while (!peer_state.finished)
        MO_ASSERT(pthread_cond_wait(&peer_state.changed, &peer_state.lock) == 0);
    MO_ASSERT(pthread_mutex_unlock(&peer_state.lock) == 0);

    mo_value_drop(f.heap, f.result_connection, result);
    mo_value_drop(f.heap, f.types.text, dialing[1]);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_NETWORK], network);
    MO_ASSERT(mo_heap_objects(f.heap) == 0 && f.host.open_connections == 0);
    host_fixture_free(&f);
    return 0;
}
