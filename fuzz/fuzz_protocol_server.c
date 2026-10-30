#define _POSIX_C_SOURCE 200809L
#include "../tests/service_compile.h"
#include "base/stack.h"
#include "tools/protocol.h"

#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static struct mo_image *image;

struct input {
    const uint8_t *bytes;
    size_t size;
};

static void run(void *context) {
    const struct input *input = context;
    if (image == NULL) {
        MO_ASSERT(setenv("MO_FUZZ_MCP_TOKEN", "fuzz-token", 1) == 0);
        image = compile(
            "module Main\nservice\nstate Int = 0\ntools add, read, reject\n"
            "token from env \"MO_FUZZ_MCP_TOKEN\"\nend\n"
            "fn add(state: Int, _now: Time, amount: Int) : (Int, Int)\n"
            "(state + amount, state)\nend\n"
            "fn read(state: Int, _now: Time) : Text\nstate.to_text\nend\n"
            "fn reject(state: Int, _now: Time, ok: Bool) : Result((Int, Unit), Text)\n"
            "if ok\nOk((state + 1, Unit))\nelse\nError(\"denied\")\nend\nend\n");
    }
    struct mo_listener *listener;
    struct mo_store *store;
    struct mo_protocol_server *server;
    MO_ASSERT(mo_listener_fake("MO_FUZZ_MCP_TOKEN", &listener) == MO_LISTENER_OK);
    MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
    const char *authorities[] = {"localhost"};
    MO_ASSERT(mo_protocol_server_new(image, 0, store, listener, authorities, 1, &server) ==
              MO_PROTOCOL_SERVER_OK);
    uint8_t out[MO_LIMIT_MCP_RESPONSE_BYTES + 1024];
    uint8_t control = input->bytes[0];
    struct mo_listener_fake_peer peer = {.input = input->bytes + 1,
                                         .input_size = input->size - 1,
                                         .output = out,
                                         .output_capacity = sizeof out,
                                         .read_quantum = control & 31U,
                                         .write_quantum = control >> 5,
                                         .readable = true,
                                         .writable = true,
                                         .eof = (control & 64U) == 0,
                                         .read_error = control == 254,
                                         .write_error = control == 255};
    MO_ASSERT(mo_listener_fake_connect(listener, &peer) == MO_LISTENER_OK);
    for (size_t i = 0; i < 2 * input->size + 1024 && !peer.closed; i++) {
        MO_ASSERT(mo_protocol_server_poll(server, 0) == MO_PROTOCOL_SERVER_OK);
        if (i % 512 == 511)
            MO_ASSERT(mo_listener_fake_advance(listener, 300000) == MO_LISTENER_OK);
    }
    MO_ASSERT(mo_listener_fake_advance(listener, 30000000) == MO_LISTENER_OK);
    for (unsigned i = 0; i < 4 && !peer.closed; i++)
        MO_ASSERT(mo_protocol_server_poll(server, 0) == MO_PROTOCOL_SERVER_OK);
    MO_ASSERT(peer.closed);
    mo_protocol_server_free(server);
    mo_listener_close(listener);
    mo_store_close(store);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0 || size > MO_LIMIT_MCP_INGRESS_BYTES + 2)
        return 0;
    struct input input = {data, size};
    MO_ASSERT(mo_with_stack(MO_SOURCE_STACK_SIZE, run, &input) == 0);
    return 0;
}
