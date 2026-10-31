#include "tools/protocol-server.h"

#include <string.h>

static bool byte_list(struct mo_protocol_server *s, struct protocol_connection *c,
                      const uint8_t *bytes, size_t size, struct mo_cell *out) {
    MO_ASSERT(size <= MO_LIMIT_MCP_HEADER_BYTES);
    if (!mo_protocol_charge(c, size))
        return false;
    struct mo_scratch scratch;
    struct mo_heap *heap = mo_process_heap(c->process);
    mo_scratch_init(&scratch, heap);
    struct mo_cell *cells = mo_scratch_resize(&scratch, NULL, 0, (size + 1) * sizeof *cells);
    bool ok = cells != NULL;
    if (ok) {
        for (size_t i = 0; i < size; i++)
            cells[i] = mo_cell_byte(bytes[i]);
        ok = mo_value_list(heap, mo_protocol_function(s, PF_SCAN)->parameters[0].type, cells,
                           (uint32_t)size, out);
    }
    mo_protocol_observe(s, c);
    mo_scratch_free(&scratch);
    return ok;
}

static bool headers(struct mo_protocol_server *s, struct protocol_connection *c,
                    struct mo_cell *bytes, struct mo_cell *head) {
    struct mo_heap *heap = mo_process_heap(c->process);
    const struct mo_bytecode_function *fn = mo_protocol_function(s, PF_HEADERS);
    struct mo_cell fields[] = {mo_cell_int(MO_LIMIT_MCP_HEADER_BYTES),
                               mo_cell_int(MO_LIMIT_MCP_LINE_BYTES),
                               mo_cell_int(MO_LIMIT_MCP_HEADER_FIELDS)};
    struct mo_cell args[2], value;
    if (!byte_list(s, c, c->input, c->header_size, &args[0]) ||
        !mo_value_case(heap, fn->parameters[1].type, 0, fields, 3, &args[1]))
        return false;
    if (!mo_protocol_invoke(s, c, PF_HEADERS, args, &value))
        return false;
    if (mo_cell_get_tag(value) != 0) {
        struct mo_cell problem = mo_protocol_field(s->protocol, fn->result_type, value, 0);
        mo_protocol_fail(s, c, mo_cell_get_tag(problem) == 2 ? 431 : 400);
        return false;
    }
    *bytes = args[0];
    *head = mo_protocol_field(s->protocol, fn->result_type, value, 0);
    return true;
}

static bool authorities(struct mo_protocol_server *s, struct protocol_connection *c,
                        struct mo_cell *out) {
    const struct mo_bytecode_function *fn = mo_protocol_function(s, PF_AUTHORITY);
    struct mo_heap *heap = mo_process_heap(c->process);
    uint32_t list = fn->parameters[0].type;
    uint32_t text = mo_image_types(s->protocol)->items[list].parameters[0];
    if (!mo_value_list(heap, list, NULL, 0, out))
        return false;
    for (uint32_t i = 0; i < mo_list_size(s->authorities); i++) {
        struct mo_cell item, copy;
        MO_ASSERT(mo_list_get(s->authorities, i, &item));
        size_t size;
        const uint8_t *bytes = mo_value_data(item, &size);
        if (!mo_protocol_charge(c, size) || !mo_value_text(heap, (const char *)bytes, size, &copy))
            return false;
        bool ok = mo_list_append(heap, list, out, copy);
        mo_value_drop(heap, text, copy);
        if (!ok)
            return false;
    }
    return true;
}

static bool authenticate(struct mo_protocol_server *s, struct protocol_connection *c,
                         struct mo_cell bytes, struct mo_cell head) {
    struct mo_cell args[3], value;
    args[0] = bytes;
    args[1] = head;
    if (!authorities(s, c, &args[2]) || !mo_protocol_invoke(s, c, PF_INSPECT, args, &value))
        return false;
    uint32_t type = mo_protocol_function(s, PF_INSPECT)->result_type;
    struct mo_cell part = mo_protocol_field(s->protocol, type, value, 0);
    if (mo_cell_get_tag(value) != 0) {
        mo_protocol_fail(s, c, (int)mo_cell_get_int(part));
        return false;
    }
    union mo_listener_request request = {.auth = {c->input, c->header_size, 0, 0}};
    if (mo_cell_get_tag(part) == 1) {
        uint32_t option = mo_image_types(s->protocol)->items[type].cases[0].fields[0].type;
        struct mo_cell span = mo_protocol_field(s->protocol, option, part, 0);
        int64_t start = mo_cell_get_int(mo_value_field(mo_cell_get_payload(span), 0));
        int64_t size = mo_cell_get_int(mo_value_field(mo_cell_get_payload(span), 1));
        MO_ASSERT(start >= 0 && size >= 0 && (uint64_t)start <= c->header_size &&
                  (uint64_t)size <= c->header_size - (size_t)start);
        request.auth.start = (size_t)start;
        request.auth.length = (size_t)size;
    }
    union mo_listener_result result;
    enum mo_listener_status status =
        mo_listener_call(s->listener, MO_LISTENER_AUTH, &request, &result);
    if (status != MO_LISTENER_OK || !result.authenticated) {
        mo_protocol_fail(s, c, 401);
        return false;
    }
    mo_host_wipe(c->input + request.auth.start, request.auth.length);
    memset(c->input + request.auth.start, 'x', request.auth.length);
    return true;
}

static bool plan(struct mo_protocol_server *s, struct protocol_connection *c, struct mo_cell bytes,
                 struct mo_cell head, bool *send_continue, bool *fixed, int64_t *length,
                 uint32_t *version_type, bool *version_some, uint8_t *version_bytes,
                 size_t *version_size) {
    struct mo_cell args[3], value;
    args[0] = bytes;
    args[1] = head;
    args[2] = mo_cell_int(MO_LIMIT_MCP_BODY_BYTES);
    if (!mo_protocol_invoke(s, c, PF_PLAN, args, &value))
        return false;
    uint32_t type = mo_protocol_function(s, PF_PLAN)->result_type;
    uint32_t tag = mo_cell_get_tag(value);
    if (tag == 0) {
        mo_protocol_fail(s, c,
                         (int)mo_cell_get_int(mo_protocol_field(s->protocol, type, value, 0)));
        return false;
    }
    uint32_t offset = tag == 1 ? 1U : 0U;
    *send_continue = mo_cell_get_bool(mo_protocol_field(s->protocol, type, value, offset));
    *fixed = tag == 1;
    *length = tag == 1 ? mo_cell_get_int(mo_protocol_field(s->protocol, type, value, 0)) : 0;
    *version_type = mo_image_types(s->protocol)->items[type].cases[tag].fields[offset + 1].type;
    struct mo_cell version = mo_protocol_field(s->protocol, type, value, offset + 1);
    *version_some = mo_cell_get_tag(version) != 0;
    *version_size = 0;
    if (*version_some) {
        struct mo_cell text = mo_protocol_field(s->protocol, *version_type, version, 0);
        const uint8_t *data = mo_value_data(text, version_size);
        MO_ASSERT(*version_size <= MO_LIMIT_MCP_HEADER_BYTES);
        memcpy(version_bytes, data, *version_size);
    }
    mo_protocol_drop(s, c, PF_PLAN, value);
    return true;
}

static bool promote(struct mo_protocol_server *s, struct protocol_connection *c, bool fixed,
                    int64_t length, uint32_t version_type, bool version_some,
                    const uint8_t *version_bytes, size_t version_size) {
    mo_protocol_observe(s, c);
    mo_protocol_dispose(s, c);
    if (!mo_protocol_request_process(s, c))
        return false;
    struct mo_heap *heap = mo_process_heap(c->process);
    if (version_some) {
        struct mo_cell text;
        if (!mo_value_text(heap, (const char *)version_bytes, version_size, &text) ||
            !mo_value_case(heap, version_type, 1, &text, 1, &c->version))
            return false;
        mo_value_drop(
            heap, mo_image_types(s->protocol)->items[version_type].cases[1].fields[0].type, text);
    } else if (!mo_value_case(heap, version_type, 0, NULL, 0, &c->version))
        return false;
    struct mo_cell size = mo_cell_int(length);
    return mo_protocol_invoke(s, c, fixed ? PF_FIXED : PF_CHUNKED, &size, &c->body_state);
}

bool mo_protocol_body_complete(const struct protocol_connection *c) {
    return mo_cell_get_tag(mo_value_field(mo_cell_get_payload(c->body_state), 0)) == 6;
}

static void feed(struct mo_protocol_server *s, struct protocol_connection *c, const uint8_t *bytes,
                 size_t size) {
    struct mo_heap *heap = mo_process_heap(c->process);
    const struct mo_bytecode_function *fn = mo_protocol_function(s, PF_FEED);
    struct mo_cell args[3], value;
    args[0] = c->body_state;
    struct mo_cell fields[] = {mo_cell_int(MO_LIMIT_MCP_BODY_BYTES),
                               mo_cell_int((int64_t)(MO_LIMIT_MCP_INGRESS_BYTES - c->header_size)),
                               mo_cell_int(MO_LIMIT_MCP_CHUNKS),
                               mo_cell_int(MO_LIMIT_MCP_LINE_BYTES),
                               mo_cell_int(MO_LIMIT_MCP_TRAILER_BYTES),
                               mo_cell_int(MO_LIMIT_MCP_TRAILER_FIELDS)};
    if (!byte_list(s, c, bytes, size, &args[1]) ||
        !mo_value_case(heap, fn->parameters[2].type, 0, fields, 6, &args[2]) ||
        !mo_protocol_invoke(s, c, PF_FEED, args, &value)) {
        mo_protocol_fail(s, c, 413);
        return;
    }
    struct mo_cell step = mo_protocol_field(s->protocol, fn->result_type, value, 0);
    if (mo_cell_get_tag(value) != 0) {
        mo_protocol_fail(s, c, (int)mo_cell_get_int(step));
        return;
    }
    struct mo_cell payload = mo_cell_get_payload(step);
    struct mo_cell spans = mo_value_field(payload, 2);
    int64_t consumed = mo_cell_get_int(mo_value_field(payload, 1));
    MO_ASSERT(consumed >= 0 && (uint64_t)consumed <= size);
    for (uint32_t i = 0; i < mo_list_size(spans); i++) {
        struct mo_cell span;
        MO_ASSERT(mo_list_get(spans, i, &span));
        int64_t start = mo_cell_get_int(mo_value_field(mo_cell_get_payload(span), 0));
        int64_t count = mo_cell_get_int(mo_value_field(mo_cell_get_payload(span), 1));
        MO_ASSERT(start >= 0 && count >= 0 && start <= consumed && count <= consumed - start &&
                  (uint64_t)count <= MO_LIMIT_MCP_BODY_BYTES - c->body_size);
        size_t added = (size_t)count;
        if (!mo_protocol_charge(c, added)) {
            mo_protocol_fail(s, c, 413);
            return;
        }
        uint8_t *body = mo_scratch_resize(&c->scratch, c->body, c->body_size, c->body_size + added);
        if (body == NULL) {
            mo_protocol_fail(s, c, 413);
            return;
        }
        c->body = body;
        memcpy(body + c->body_size, bytes + (size_t)start, added);
        c->body_size += added;
    }
    mo_value_drop(heap, fn->parameters[0].type, c->body_state);
    c->body_state = mo_value_dup(heap, fn->parameters[0].type, mo_value_field(payload, 0));
    mo_protocol_drop(s, c, PF_FEED, value);
    mo_value_drop(heap, fn->parameters[1].type, args[1]);
    mo_value_drop(heap, fn->parameters[2].type, args[2]);
    mo_protocol_observe(s, c);
    if (mo_protocol_body_complete(c))
        mo_protocol_receive(s, c);
}

static void head_complete(struct mo_protocol_server *s, struct protocol_connection *c,
                          size_t buffered) {
    struct mo_cell bytes, head;
    if (!headers(s, c, &bytes, &head)) {
        if (c->phase == PS_HEADERS)
            mo_protocol_fail(s, c, 413);
        return;
    }
    if (!authenticate(s, c, bytes, head)) {
        if (c->phase == PS_HEADERS)
            mo_protocol_fail(s, c, 413);
        return;
    }
    c->authenticated = true;
    bool send_continue, fixed, version_some;
    int64_t length;
    uint32_t version_type;
    uint8_t version_bytes[MO_LIMIT_MCP_HEADER_BYTES];
    size_t version_size;
    if (!plan(s, c, bytes, head, &send_continue, &fixed, &length, &version_type, &version_some,
              version_bytes, &version_size) ||
        !promote(s, c, fixed, length, version_type, version_some, version_bytes, version_size)) {
        if (c->phase == PS_HEADERS)
            mo_protocol_fail(s, c, 413);
        return;
    }
    c->deadline = s->now.monotonic_us > UINT64_MAX - MO_LIMIT_MCP_BODY_US
                      ? UINT64_MAX
                      : s->now.monotonic_us + MO_LIMIT_MCP_BODY_US;
    c->phase = PS_BODY;
    if (buffered != 0)
        feed(s, c, c->input + c->header_size, buffered);
    mo_host_wipe(c->input, sizeof c->input);
    if (c->phase != PS_BODY)
        return;
    if (send_continue) {
        for (size_t i = 0; i < s->error_count; i++)
            if (s->errors[i].status == 100) {
                c->output = s->errors[i].response;
                break;
            }
        MO_ASSERT(c->output.bytes != NULL);
        c->write_offset = 0;
        c->phase = PS_CONTINUE;
    } else if (mo_protocol_body_complete(c))
        mo_protocol_receive(s, c);
}

void mo_protocol_read(struct mo_protocol_server *s, struct protocol_connection *c) {
    size_t at = c->phase == PS_HEADERS ? c->header_size : 0;
    if (at == sizeof c->input) {
        mo_protocol_fail(s, c, 431);
        return;
    }
    union mo_listener_request request = {.read = {c->key, c->input + at, sizeof c->input - at}};
    union mo_listener_result result;
    enum mo_listener_status status =
        mo_listener_call(s->listener, MO_LISTENER_READ, &request, &result);
    if (status == MO_LISTENER_AGAIN)
        return;
    if (status != MO_LISTENER_OK) {
        if (status == MO_LISTENER_EOF)
            mo_protocol_fail(s, c, 400);
        else
            mo_protocol_close(s, c);
        return;
    }
    if (c->phase == PS_BODY) {
        size_t buffered = result.transferred;
        while (buffered < sizeof c->input) {
            request.read.bytes = c->input + buffered;
            request.read.capacity = sizeof c->input - buffered;
            status = mo_listener_call(s->listener, MO_LISTENER_READ, &request, &result);
            if (status != MO_LISTENER_OK)
                break;
            MO_ASSERT(result.transferred > 0);
            buffered += result.transferred;
        }
        feed(s, c, c->input, buffered);
        mo_host_wipe(c->input, sizeof c->input);
        return;
    }
    struct mo_cell args[3], scan;
    args[1] = c->scan;
    args[2] = mo_cell_int(MO_LIMIT_MCP_HEADER_BYTES);
    if (!byte_list(s, c, c->input + at, result.transferred, &args[0]) ||
        !mo_protocol_invoke(s, c, PF_SCAN, args, &scan)) {
        mo_protocol_fail(s, c, 413);
        return;
    }
    if (mo_cell_get_tag(scan) != 0) {
        mo_protocol_fail(s, c, 431);
        return;
    }
    const struct mo_bytecode_function *fn = mo_protocol_function(s, PF_SCAN);
    struct mo_cell state = mo_protocol_field(s->protocol, fn->result_type, scan, 0);
    struct mo_heap *heap = mo_process_heap(c->process);
    mo_value_drop(heap, fn->parameters[1].type, c->scan);
    c->scan = mo_value_dup(heap, fn->parameters[1].type, state);
    mo_value_drop(heap, fn->parameters[0].type, args[0]);
    mo_protocol_drop(s, c, PF_SCAN, scan);
    c->header_size = (size_t)mo_cell_get_int(mo_value_field(mo_cell_get_payload(c->scan), 1));
    if (mo_cell_get_bool(mo_value_field(mo_cell_get_payload(c->scan), 2)))
        head_complete(s, c, at + result.transferred - c->header_size);
}
