#define _POSIX_C_SOURCE 200809L
#include "../tests/host_fixture.h"

#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static int listening = -1;
    static uint16_t port;
    if (listening < 0) {
        listening = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in address = {.sin_family = AF_INET,
                                      .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        socklen_t length = sizeof address;
        MO_ASSERT(listening >= 0 &&
                  bind(listening, (const struct sockaddr *)&address, length) == 0 &&
                  listen(listening, 16) == 0 &&
                  getsockname(listening, (struct sockaddr *)&address, &length) == 0);
        port = ntohs(address.sin_port);
    }
    int64_t most = 1;
    if (size >= 2) {
        most += data[0] | data[1] << 8;
        data += 2;
        size -= 2;
    }
    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    MO_ASSERT(mo_host_start(&f.host) == NULL);
    struct mo_cell network, result;
    struct mo_host_error error;
    MO_ASSERT(mo_host_real(&f.host, f.heap, f.capabilities[MO_CAP_NETWORK], NULL, -1, &network,
                           &error) == MO_HOST_RETURNED);
    struct mo_cell dialing[] = {network, host_text(&f, "127.0.0.1"), mo_cell_int(port),
                                mo_cell_duration(30000000)};
    MO_ASSERT(mo_host_call(&f.host, f.heap, MO_HOST_NETWORK_CONNECT, f.result_connection, dialing,
                           &result, &error) == MO_HOST_RETURNED &&
              mo_cell_get_tag(result) == 0);
    struct mo_cell inside = mo_cell_get_payload(result);
    if (mo_case_shape(&f.types.types, f.result_connection, 0) == MO_CASE_BOXED)
        inside = mo_value_field(inside, 0);
    struct mo_cell receiving[] = {inside, mo_cell_int(most)};
    int peer = accept(listening, NULL, NULL);
    MO_ASSERT(peer >= 0);
    size_t written = 0, received = 0;
    for (;;) {
        if (peer >= 0 && written < size) {
            ssize_t n = send(peer, data + written, size - written, MSG_DONTWAIT);
            MO_ASSERT(n > 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)));
            written += n > 0 ? (size_t)n : 0;
        }
        if (peer >= 0 && written == size) {
            MO_ASSERT(close(peer) == 0);
            peer = -1;
        }
        struct mo_cell bytes;
        MO_ASSERT(mo_host_call(&f.host, f.heap, MO_HOST_CONNECTION_RECEIVE, f.result_received,
                               receiving, &bytes, &error) == MO_HOST_RETURNED &&
                  mo_cell_get_tag(bytes) == 0);
        struct mo_cell value = mo_cell_get_payload(bytes);
        if (mo_case_shape(&f.types.types, f.result_received, 0) == MO_CASE_BOXED)
            value = mo_value_field(value, 0);
        size_t count;
        const uint8_t *got = mo_value_data(value, &count);
        MO_ASSERT(count <= (size_t)most && count <= written - received &&
                  (count == 0 || memcmp(got, data + received, count) == 0));
        received += count;
        mo_value_drop(f.heap, f.result_received, bytes);
        if (count == 0)
            break;
    }
    MO_ASSERT(received == size && peer < 0);
    mo_value_drop(f.heap, f.result_connection, result);
    mo_value_drop(f.heap, f.types.text, dialing[1]);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_NETWORK], network);
    MO_ASSERT(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
    return 0;
}
