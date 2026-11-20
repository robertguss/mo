#include "host/network.h"

#include <stdio.h>
#include <string.h>

enum { REPLIES, SENT, POSITION };

struct reason {
    char text[3 * (4 * MO_LIMIT_FAKE_QUOTE + 16) + 64];
    size_t length;
};
struct line {
    const uint8_t *bytes;
    size_t length;
    bool present;
};

static void put(struct reason *reason, const char *text) {
    size_t length = strlen(text);
    MO_ASSERT(length < sizeof reason->text - reason->length);
    memcpy(reason->text + reason->length, text, length + 1);
    reason->length += length;
}

static void quote(struct reason *reason, struct line line) {
    if (!line.present) {
        put(reason, "nothing more");
        return;
    }
    put(reason, "\"");
    for (size_t i = 0; i < line.length && i < MO_LIMIT_FAKE_QUOTE; i++) {
        uint8_t byte = line.bytes[i];
        char shown[5] = {'\\', (char)byte};
        if (byte == '\r' || byte == '\n')
            shown[1] = byte == '\r' ? 'r' : 'n';
        else if (byte < 0x20 || byte > 0x7e) {
            static const char digits[] = "0123456789abcdef";
            shown[1] = 'x';
            shown[2] = digits[byte >> 4];
            shown[3] = digits[byte & 15];
        } else if (byte != '"' && byte != '\\') {
            shown[0] = (char)byte;
            shown[1] = 0;
        }
        put(reason, shown);
    }
    put(reason, line.length > MO_LIMIT_FAKE_QUOTE ? "...\"" : "\"");
}

static MO_MUST_USE struct line next_line(const uint8_t *bytes, size_t length, size_t *at) {
    if (*at >= length)
        return (struct line){0};
    const uint8_t *end = memchr(bytes + *at, '\n', length - *at);
    size_t stop = end == NULL ? length : (size_t)(end - bytes) + 1;
    struct line line = {bytes + *at, stop - *at, true};
    *at = stop;
    return line;
}

static MO_MUST_USE bool same(struct line a, struct line b) {
    return a.present && b.present && a.length == b.length &&
           memcmp(a.bytes, b.bytes, a.length) == 0;
}

static void describe(struct mo_cell replies, struct mo_cell sent, struct reason *reason) {
    size_t length, at = 0;
    const uint8_t *bytes = mo_value_data(sent, &length);
    struct line first = next_line(bytes, length, &at);
    if (!first.present) {
        put(reason, "no reply for a connection that sent nothing");
        return;
    }
    while (first.length != 0 &&
           (first.bytes[first.length - 1] == '\n' || first.bytes[first.length - 1] == '\r'))
        first.length--;
    put(reason, "no reply for ");
    quote(reason, first);
    uint32_t position = 0, most = 0;
    struct mo_cell key, value;
    struct line was = {0}, wanted = {0};
    while (mo_table_next(replies, &position, &key, &value)) {
        size_t held_length, mine = 0, theirs = 0;
        const uint8_t *held = mo_value_data(key, &held_length);
        uint32_t shared = 0;
        struct line a = next_line(bytes, length, &mine), b = next_line(held, held_length, &theirs);
        while (same(a, b)) {
            shared++;
            a = next_line(bytes, length, &mine);
            b = next_line(held, held_length, &theirs);
        }
        if (shared > most) {
            most = shared;
            was = a;
            wanted = b;
        }
    }
    if (most == 0)
        return;
    put(reason, ": sent ");
    quote(reason, was);
    put(reason, ", expected ");
    quote(reason, wanted);
}

static MO_MUST_USE enum mo_host_outcome fake_connect(struct mo_host *host, struct mo_heap *heap,
                                                     uint32_t type, const struct mo_cell *args,
                                                     struct mo_cell *out,
                                                     struct mo_host_error *error) {
    size_t length;
    const uint8_t *name = mo_value_data(args[1], &length);
    int64_t port = mo_cell_get_int(args[2]);
    struct mo_cell replies;
    bool found = false;
    if (mo_host_named(name, length)) {
        char key[MO_LIMIT_HOST_NAME + 7];
        memcpy(key, name, length);
        int digits = snprintf(key + length, 7, ":%d", (int)port);
        MO_ASSERT(digits > 1 && digits < 7);
        struct mo_cell text;
        if (!mo_value_text(heap, key, length + (size_t)digits, &text))
            return MO_HOST_OUT_OF_MEMORY;
        bool ok = mo_table_get(heap, host->types->peers,
                               mo_state_get(mo_capability_get(args[0])->state, 0), text, &found,
                               &replies);
        mo_value_drop(heap, host->types->text, text);
        if (!ok)
            return MO_HOST_OUT_OF_MEMORY;
    }
    if (!found)
        return mo_host_network_error(host, heap, type,
                                     (struct mo_network_error){.status = NETWORK_UNKNOWN_HOST},
                                     args[1], port, out, error);
    struct mo_cell sent, value;
    if (!mo_value_bytes(heap, NULL, 0, &sent))
        return MO_HOST_OUT_OF_MEMORY;
    uint32_t types[] = {host->types->replies, host->types->bytes, host->types->integer};
    struct mo_cell values[] = {replies, sent, mo_cell_int(-1)};
    enum mo_host_outcome result = mo_host_state_cap(
        host, heap, mo_host_network_payload(host, type, "Ok"), true, types, values, 3, &value);
    mo_value_drop(heap, host->types->bytes, sent);
    if (result != MO_HOST_RETURNED)
        return result;
    return mo_host_network_ok(host, heap, type, value, out);
}

static MO_MUST_USE enum mo_host_outcome fake_send(struct mo_host *host, struct mo_heap *heap,
                                                  uint32_t type, const struct mo_cell *args,
                                                  struct mo_cell *out,
                                                  struct mo_host_error *error) {
    struct mo_state *state = mo_capability_get(args[0])->state;
    if (mo_cell_get_int(mo_state_get(state, POSITION)) >= 0)
        return mo_host_network_error(host, heap, type,
                                     (struct mo_network_error){.status = NETWORK_CLOSED},
                                     mo_cell_unit(), 0, out, error);
    size_t had, more;
    const uint8_t *before = mo_value_data(mo_state_get(state, SENT), &had);
    const uint8_t *added = mo_value_data(args[1], &more);
    if (more == 0)
        return mo_host_network_ok(host, heap, type, mo_cell_unit(), out);
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, heap);
    uint8_t *joined = mo_scratch_resize(&scratch, NULL, 0, had + more);
    if (joined == NULL)
        return MO_HOST_OUT_OF_MEMORY;
    if (had != 0)
        memcpy(joined, before, had);
    memcpy(joined + had, added, more);
    struct mo_cell sent;
    bool ok = mo_value_bytes(heap, joined, had + more, &sent);
    mo_scratch_free(&scratch);
    if (!ok)
        return MO_HOST_OUT_OF_MEMORY;
    enum mo_host_outcome result = mo_host_network_ok(host, heap, type, mo_cell_unit(), out);
    if (result == MO_HOST_RETURNED)
        mo_state_set(heap, state, SENT, sent);
    mo_value_drop(heap, host->types->bytes, sent);
    return result;
}

static MO_MUST_USE enum mo_host_outcome fake_receive(struct mo_host *host, struct mo_heap *heap,
                                                     uint32_t type, const struct mo_cell *args,
                                                     struct mo_cell *out,
                                                     struct mo_host_error *error) {
    struct mo_state *state = mo_capability_get(args[0])->state;
    struct mo_cell replies = mo_state_get(state, REPLIES), sent = mo_state_get(state, SENT), reply;
    int64_t position = mo_cell_get_int(mo_state_get(state, POSITION));
    if (position < 0)
        position = 0;
    bool found;
    if (!mo_table_get(heap, host->types->replies, replies, sent, &found, &reply))
        return MO_HOST_OUT_OF_MEMORY;
    enum mo_host_outcome result;
    if (found) {
        size_t length, most = (size_t)mo_cell_get_int(args[1]);
        const uint8_t *bytes = mo_value_data(reply, &length);
        MO_ASSERT((uint64_t)position <= length);
        size_t left = length - (size_t)position;
        struct mo_cell value;
        if (left > most)
            left = most;
        if (!mo_value_bytes(heap, left == 0 ? NULL : bytes + position, left, &value))
            return MO_HOST_OUT_OF_MEMORY;
        position += (int64_t)left;
        result = mo_host_network_ok(host, heap, type, value, out);
    } else {
        struct reason reason = {.length = 0};
        describe(replies, sent, &reason);
        result = mo_host_network_error(
            host, heap, type,
            (struct mo_network_error){.status = NETWORK_BROKEN, .reason = reason.text},
            mo_cell_unit(), 0, out, error);
    }
    if (result == MO_HOST_RETURNED)
        mo_state_set(heap, state, POSITION, mo_cell_int(position));
    return result;
}

enum mo_host_outcome mo_host_fake_network_call(struct mo_host *host, struct mo_heap *heap,
                                               uint32_t operation, uint32_t type,
                                               const struct mo_cell *args, struct mo_cell *out,
                                               struct mo_host_error *error) {
    if (operation == MO_HOST_NETWORK_CONNECT || operation == MO_HOST_NETWORK_CONNECT_SECURE)
        return fake_connect(host, heap, type, args, out, error);
    if (operation == MO_HOST_CONNECTION_SEND)
        return fake_send(host, heap, type, args, out, error);
    MO_ASSERT(operation == MO_HOST_CONNECTION_RECEIVE);
    return fake_receive(host, heap, type, args, out, error);
}
