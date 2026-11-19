#include "../tests/host_fixture.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static struct mo_cell call(struct host_fixture *f, uint32_t operation,
                           const struct mo_cell *arguments) {
    struct mo_cell out;
    struct mo_host_error error;
    MO_ASSERT(mo_host_call(&f->host, f->heap, operation, host_result_type(f, operation), arguments,
                           &out, &error) == MO_HOST_RETURNED);
    return out;
}

static struct mo_cell inside(struct host_fixture *f, uint32_t type, struct mo_cell result) {
    struct mo_cell value = mo_cell_get_payload(result);
    if (mo_case_shape(&f->types.types, type, 0) == MO_CASE_BOXED)
        value = mo_value_field(value, 0);
    return value;
}

static void sends(struct host_fixture *f, struct mo_cell connection, const uint8_t *bytes,
                  size_t size, uint32_t tag) {
    struct mo_cell arguments[] = {connection, mo_cell_unit()};
    MO_ASSERT(mo_value_bytes(f->heap, bytes, size, &arguments[1]));
    struct mo_cell result = call(f, MO_HOST_CONNECTION_SEND, arguments);
    MO_ASSERT(mo_cell_get_tag(result) == tag);
    mo_value_drop(f->heap, f->result_sent, result);
    mo_value_drop(f->heap, f->bytes, arguments[1]);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 6)
        return 0;
    int64_t most = 1 + (data[0] | data[1] << 8);
    size_t cut = (size_t)(data[2] | data[3] << 8), split = (size_t)(data[4] | data[5] << 8);
    data += 6;
    size -= 6;
    size_t held_size = split % (size + 1), sent_size = size - held_size;
    const uint8_t *sent = data + held_size;
    cut %= sent_size + 1;
    bool held = held_size == sent_size && memcmp(data, sent, sent_size) == 0;

    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    struct mo_cell request, reply, replies, peers, name = host_text(&f, "peer:1");
    MO_ASSERT(mo_value_bytes(f.heap, data, held_size, &request) &&
              mo_value_bytes(f.heap, data, size, &reply) &&
              mo_value_map(f.heap, f.types.replies, &request, &reply, 1, &replies) &&
              mo_value_map(f.heap, f.types.peers, &name, &replies, 1, &peers));
    mo_value_drop(f.heap, f.bytes, request);
    mo_value_drop(f.heap, f.bytes, reply);
    mo_value_drop(f.heap, f.types.replies, replies);
    mo_value_drop(f.heap, f.types.text, name);
    struct mo_cell network = call(&f, MO_HOST_NETWORK_FAKE, &peers);
    struct mo_cell dialing[] = {network, host_text(&f, "peer"), mo_cell_int(1),
                                mo_cell_duration(0)};
    struct mo_cell opened = call(&f, MO_HOST_NETWORK_CONNECT, dialing);
    MO_ASSERT(mo_cell_get_tag(opened) == 0);
    struct mo_cell connection = inside(&f, f.result_connection, opened);
    sends(&f, connection, sent, cut, 0);
    sends(&f, connection, sent + cut, sent_size - cut, 0);

    struct mo_cell receiving[] = {connection, mo_cell_int(most)};
    size_t received = 0;
    for (unsigned turn = 0;; turn++) {
        struct mo_cell result = call(&f, MO_HOST_CONNECTION_RECEIVE, receiving);
        MO_ASSERT(mo_cell_get_tag(result) == (held ? 0u : 1u));
        size_t count = 0;
        if (held) {
            const uint8_t *got = mo_value_data(inside(&f, f.result_received, result), &count);
            MO_ASSERT(count <= (size_t)most && count <= size - received &&
                      (count == (size_t)most || count == size - received) &&
                      (count == 0 || memcmp(got, data + received, count) == 0));
            received += count;
        }
        mo_value_drop(f.heap, f.result_received, result);
        if (held ? count == 0 : turn == 1)
            break;
    }
    MO_ASSERT(!held || received == size);
    sends(&f, connection, sent, sent_size, 1);
    MO_ASSERT(f.host.open_connections == 0);
    mo_value_drop(f.heap, f.result_connection, opened);
    mo_value_drop(f.heap, f.types.text, dialing[1]);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_NETWORK], network);
    MO_ASSERT(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
    return 0;
}
