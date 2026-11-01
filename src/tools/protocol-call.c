#include "tools/protocol-server.h"

#include "base/utf8.h"
#include "tools/tools.h"

#include <string.h>

bool mo_protocol_catalog(struct mo_protocol_server *s) {
    struct mo_heap *heap = mo_process_heap(s->catalog_process);
    struct mo_protocol_descriptor descriptor;
    if (!mo_protocol_descriptor(s->protocol, heap, s->image, s->service_index, &descriptor))
        return false;
    uint64_t key = mo_image_key(s->image, s->service_index);
    char hex[16];
    const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; i++)
        hex[i] = digits[(key >> ((15 - i) * 4)) & 15];
    struct mo_cell args[3], catalog;
    args[0] = descriptor.value;
    if (!mo_value_text(heap, hex, sizeof hex, &args[1]))
        return false;
    args[2] = mo_cell_int(MO_LIMIT_MCP_RESPONSE_BYTES);
    if (!mo_protocol_invoke(s, NULL, PF_PREPARE, args, &catalog))
        return false;
    if (mo_cell_get_tag(catalog) != 1)
        return false;
    uint32_t catalog_type = mo_protocol_function(s, PF_RECEIVE)->parameters[3].type;
    struct mo_cell prepared = mo_protocol_field(
        s->protocol, mo_protocol_function(s, PF_PREPARE)->result_type, catalog, 0);
    if (!mo_value_make_immortal(heap, descriptor.type, descriptor.value) ||
        !mo_value_make_immortal(heap, catalog_type, prepared))
        return false;
    s->cached_descriptor = descriptor.value;
    s->cached_catalog = prepared;
    mo_protocol_drop(s, NULL, PF_PREPARE, catalog);
    mo_value_drop(heap, mo_protocol_function(s, PF_PREPARE)->parameters[1].type, args[1]);
    return true;
}

static enum protocol_error failure(struct protocol_connection *c) {
    return c->work_left == 0 ? PE_WORK : PE_MEMORY;
}

static enum protocol_error service_error(enum mo_service_status status) {
    switch (status) {
    case MO_SERVICE_OK:
        return PE_INTERNAL;
    case MO_SERVICE_BUSY:
        return PE_BUSY;
    case MO_SERVICE_DOWN:
        return PE_STOPPED;
    case MO_SERVICE_MEMORY:
        return PE_MEMORY;
    case MO_SERVICE_WORK:
        return PE_WORK;
    case MO_SERVICE_DEFECT:
        return PE_HANDLER;
    case MO_SERVICE_STORE:
        return PE_STORE;
    case MO_SERVICE_INTERVAL:
    case MO_SERVICE_SAVED_INVALID:
        return PE_INTERNAL;
    }
    MO_UNREACHABLE();
}

static void respond(struct mo_protocol_server *s, struct protocol_connection *c, int status,
                    struct mo_cell body, enum protocol_effect effect) {
    size_t size;
    const uint8_t *bytes = mo_value_data(body, &size);
    if (size > MO_LIMIT_MCP_RESPONSE_BYTES) {
        if (c->id_size != 0)
            mo_protocol_fallback(s, c, PE_RESPONSE, effect);
        else
            mo_protocol_fail(s, c, 413);
        return;
    }
    struct mo_cell args[] = {mo_cell_int(status), body};
    if (!mo_protocol_invoke(s, c, PF_HTTP, args, &c->response)) {
        if (c->id_size != 0)
            mo_protocol_fallback(s, c, PE_RESPONSE, effect);
        else
            mo_protocol_fail(s, c, 413);
        return;
    }
    bytes = mo_value_data(c->response, &size);
    mo_protocol_start_write(s, c, (struct protocol_bytes){bytes, size});
}

static bool token(struct mo_protocol_server *s) {
    uint64_t elapsed = s->now.monotonic_us - s->rate_at;
    uint64_t full = MO_LIMIT_MCP_BURST * UINT64_C(1000000);
    s->rate_at = s->now.monotonic_us;
    if (elapsed >= (full - s->rate_credit + MO_LIMIT_MCP_RATE - 1) / MO_LIMIT_MCP_RATE)
        s->rate_credit = full;
    else
        s->rate_credit += elapsed * MO_LIMIT_MCP_RATE;
    if (s->rate_credit < UINT64_C(1000000))
        return false;
    s->rate_credit -= UINT64_C(1000000);
    return true;
}

static struct mo_protocol_values bridge(struct mo_protocol_server *s,
                                        struct protocol_connection *c) {
    uint32_t list = mo_protocol_function(s, PF_ARGUMENTS)->result_type;
    list = mo_image_types(s->protocol)->items[list].cases[0].fields[0].type;
    return (struct mo_protocol_values){.protocol = mo_image_types(s->protocol),
                                       .service = mo_image_types(s->image),
                                       .descriptor = s->descriptor,
                                       .value =
                                           mo_image_types(s->protocol)->items[list].parameters[0],
                                       .work = &c->work_left};
}

static void call_tool(struct mo_protocol_server *s, struct protocol_connection *c) {
    struct mo_cell fields = mo_cell_get_payload(c->action);
    struct mo_cell request = mo_value_field(fields, 0);
    uint32_t request_type = mo_image_types(s->protocol)
                                ->items[mo_protocol_function(s, PF_RECEIVE)->result_type]
                                .cases[1]
                                .fields[0]
                                .type;
    struct mo_cell text = mo_protocol_field(s->protocol, request_type, request, 1);
    size_t size;
    const uint8_t *bytes = mo_value_data(text, &size);
    MO_ASSERT(size <= sizeof c->escaped_id);
    memcpy(c->escaped_id, bytes, size);
    c->id_size = size;
    if (!token(s)) {
        mo_protocol_fallback(s, c, PE_RATE, EFFECT_NONE);
        return;
    }
    int64_t index = mo_cell_get_int(mo_value_field(fields, 1));
    MO_ASSERT(index >= 0 && (uint64_t)index < s->descriptor->tool_count);
    const struct mo_bytecode_service_tool *tool = &s->descriptor->tools[index];
    struct mo_cell tools = mo_value_field(mo_cell_get_payload(c->descriptor), 1), descriptor;
    MO_ASSERT(mo_list_get(tools, (uint32_t)index, &descriptor));
    struct mo_cell args[] = {mo_value_field(mo_cell_get_payload(c->descriptor), 0),
                             mo_value_field(mo_cell_get_payload(descriptor), 3),
                             mo_value_field(fields, 2), mo_cell_int(MO_LIMIT_MCP_VALUE_DEPTH),
                             mo_cell_int(MO_LIMIT_MCP_BIGINT_DIGITS)};
    struct mo_cell decoded;
    if (!mo_protocol_invoke(s, c, PF_ARGUMENTS, args, &decoded)) {
        mo_protocol_fallback(s, c, failure(c), EFFECT_NONE);
        return;
    }
    if (mo_cell_get_tag(decoded) != 0) {
        mo_protocol_fallback(s, c, PE_ARGUMENTS, EFFECT_NONE);
        return;
    }
    struct mo_cell values = mo_protocol_field(
        s->protocol, mo_protocol_function(s, PF_ARGUMENTS)->result_type, decoded, 0);
    MO_ASSERT(mo_list_size(values) == tool->argument_count);
    struct mo_cell arguments[MO_LIMIT_BYTECODE_PARAMETERS - 2];
    struct mo_protocol_values types = bridge(s, c);
    struct mo_heap *protocol_heap = mo_process_heap(c->process);
    size_t available = mo_heap_limit(protocol_heap) - mo_heap_used(protocol_heap);
    size_t before = mo_heap_used(s->service_heap);
    size_t old_limit = mo_heap_limit(s->service_heap);
    size_t decode_limit = available > old_limit - before ? old_limit : before + available;
    mo_heap_set_limit(s->service_heap, decode_limit);
    uint32_t built = 0;
    enum mo_protocol_value_status status = MO_PROTOCOL_VALUE_OK;
    for (; built < tool->argument_count; built++) {
        struct mo_cell value;
        MO_ASSERT(mo_list_get(values, built, &value));
        status = mo_protocol_value_unpack(&types, s->service_heap, tool->argument_types[built],
                                          value, MO_LIMIT_MCP_BODY_BYTES, &arguments[built]);
        if (status != MO_PROTOCOL_VALUE_OK)
            break;
    }
    size_t unpack_peak = mo_heap_peak(s->service_heap);
    if (unpack_peak > decode_limit)
        unpack_peak = decode_limit;
    unpack_peak -= before;
    size_t memory =
        sizeof *c + c->fallback_capacity + mo_process_memory_used(c->process) + unpack_peak;
    if (memory > c->peak_memory)
        c->peak_memory = memory;
    size_t native = mo_heap_scratch_peak(s->service_heap);
    if (native > decode_limit - before)
        native = decode_limit - before;
    native += sizeof *c + c->fallback_capacity + mo_heap_scratch_peak(protocol_heap);
    if (native > c->peak_native)
        c->peak_native = native;
    mo_protocol_observe(s, c);
    mo_heap_set_limit(s->service_heap, old_limit);
    enum mo_service_status sent = MO_SERVICE_MEMORY;
    if (status == MO_PROTOCOL_VALUE_OK && s->sequence != UINT64_MAX) {
        c->id = ++s->sequence;
        sent = mo_service_send(s->service, (uint32_t)index, arguments, built, c->id);
    }
    for (uint32_t i = 0; i < built; i++)
        mo_value_drop(s->service_heap, tool->argument_types[i], arguments[i]);
    mo_protocol_drop(s, c, PF_ARGUMENTS, decoded);
    if (status != MO_PROTOCOL_VALUE_OK) {
        mo_protocol_fallback(s, c, status == MO_PROTOCOL_VALUE_INVALID ? PE_ARGUMENTS : failure(c),
                             EFFECT_NONE);
    } else if (sent != MO_SERVICE_OK)
        mo_protocol_fallback(s, c, service_error(sent), EFFECT_NONE);
    else {
        c->phase = PS_SERVICE;
        s->metrics.admitted++;
    }
}

void mo_protocol_receive(struct mo_protocol_server *s, struct protocol_connection *c) {
    if (s->now.monotonic_us >= c->deadline) {
        mo_protocol_close(s, c);
        return;
    }
    size_t marks;
    if (!mo_protocol_charge(c, c->body_size) ||
        !mo_protocol_body_marks(c->body, c->body_size, &marks) ||
        marks >= MO_LIMIT_MCP_PAYLOAD_NODES + MO_LIMIT_MCP_ENVELOPE_NODES) {
        mo_protocol_fail(s, c, 413);
        return;
    }
    c->descriptor = s->cached_descriptor;
    c->catalog = s->cached_catalog;
    struct mo_heap *heap = mo_process_heap(c->process);
    struct mo_cell args[5];
    const uint8_t *body = c->body;
    size_t size = c->body_size;
    if (!mo_utf8_valid(body, size)) {
        body = (const uint8_t *)"{";
        size = 1;
    }
    if (!mo_protocol_charge(c, size) || !mo_value_text(heap, (const char *)body, size, &args[0]) ||
        !mo_value_text(heap, mo_version(), strlen(mo_version()), &args[4])) {
        mo_protocol_fail(s, c, 413);
        return;
    }
    args[1] = c->version;
    args[2] = c->descriptor;
    args[3] = c->catalog;
    if (!mo_protocol_invoke(s, c, PF_RECEIVE, args, &c->action)) {
        mo_protocol_fail(s, c, 413);
        return;
    }
    if (s->now.monotonic_us >= c->deadline) {
        mo_protocol_close(s, c);
        return;
    }
    if (mo_cell_get_tag(c->action) == 1)
        call_tool(s, c);
    else {
        struct mo_cell fields = mo_cell_get_payload(c->action);
        respond(s, c, (int)mo_cell_get_int(mo_value_field(fields, 0)), mo_value_field(fields, 1),
                EFFECT_NONE);
    }
}

void mo_protocol_complete(struct mo_protocol_server *s, struct protocol_connection *c) {
    c->reply_ready = false;
    enum protocol_effect effect = c->reply.updated ? EFFECT_APPLIED : EFFECT_NONE;
    if (c->reply.status != MO_SERVICE_OK) {
        if (c->reply.status == MO_SERVICE_STORE || c->reply.status == MO_SERVICE_INTERVAL ||
            c->reply.status == MO_SERVICE_SAVED_INVALID)
            effect = EFFECT_UNKNOWN;
        mo_protocol_fallback(s, c, service_error(c->reply.status), effect);
        return;
    }
    struct mo_cell value = c->reply.value;
    uint32_t type = c->reply.type;
    const struct mo_types *types = mo_image_types(s->image);
    const struct mo_bytecode_service_tool *tool = &s->descriptor->tools[c->reply.handler];
    bool application = types->items[type].kind == MO_TYPE_CHOICE &&
                       strncmp(types->items[type].name, "Result(", 7) == 0;
    bool failed = application && mo_cell_get_tag(value) == 1;
    if (application) {
        uint32_t tag = mo_cell_get_tag(value);
        value = mo_protocol_field(s->image, type, value, 0);
        type = types->items[type].cases[tag].fields[0].type;
    }
    if (!failed && !tool->read_only) {
        value = mo_value_field(value, 1);
        type = types->items[type].parameters[1];
    }
    struct mo_cell tool_fields;
    MO_ASSERT(mo_list_get(mo_value_field(mo_cell_get_payload(c->descriptor), 1), c->reply.handler,
                          &tool_fields));
    struct mo_cell declared = mo_value_field(mo_cell_get_payload(tool_fields), failed ? 5 : 4);
    if (failed)
        declared = mo_cell_get_payload(declared);
    struct mo_protocol_values transfer = bridge(s, c);
    transfer.node_limit = MO_LIMIT_MCP_RESPONSE_NODES;
    struct mo_heap *heap = mo_process_heap(c->process);
    struct mo_cell packed;
    enum mo_protocol_value_status copied =
        mo_protocol_value_pack(&transfer, heap, type, value, MO_LIMIT_MCP_RESPONSE_BYTES, &packed);
    mo_value_drop(s->service_heap, c->reply.type, c->reply.value);
    c->retained = false;
    if (copied != MO_PROTOCOL_VALUE_OK) {
        mo_protocol_fallback(s, c, PE_RESPONSE, effect);
        return;
    }
    struct mo_cell args[] = {mo_value_field(mo_cell_get_payload(c->descriptor), 0), declared,
                             packed, mo_cell_int(MO_LIMIT_MCP_VALUE_DEPTH)};
    struct mo_cell encoded, rendered;
    if (!mo_protocol_invoke(s, c, PF_ENCODE, args, &encoded) || mo_cell_get_tag(encoded) != 0) {
        mo_protocol_fallback(s, c, PE_RESPONSE, effect);
        return;
    }
    struct mo_cell json =
        mo_protocol_field(s->protocol, mo_protocol_function(s, PF_ENCODE)->result_type, encoded, 0);
    struct mo_cell request = mo_value_field(mo_cell_get_payload(c->action), 0);
    uint32_t request_type = mo_image_types(s->protocol)
                                ->items[mo_protocol_function(s, PF_RECEIVE)->result_type]
                                .cases[1]
                                .fields[0]
                                .type;
    struct mo_cell result[] = {mo_protocol_field(s->protocol, request_type, request, 0), json,
                               mo_cell_bool(failed), mo_cell_int(MO_LIMIT_MCP_RESPONSE_BYTES)};
    if (!mo_protocol_invoke(s, c, PF_TOOL_REPLY, result, &rendered) ||
        mo_cell_get_tag(rendered) != 1) {
        mo_protocol_fallback(s, c, PE_RESPONSE, effect);
        return;
    }
    struct mo_cell body = mo_protocol_field(
        s->protocol, mo_protocol_function(s, PF_TOOL_REPLY)->result_type, rendered, 0);
    respond(s, c, 200, body, effect);
}
