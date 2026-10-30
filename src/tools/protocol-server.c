#include "tools/protocol-server.h"

#include "tools/tools.h"

#include <string.h>

static const char *const function_names[PF_COUNT] = {
    "Http.scan_headers",      "Http.parse_headers",    "HttpAuthority.check",
    "HttpPolicy.inspect",     "HttpPolicy.body_plan",  "HttpBody.fixed",
    "HttpBody.chunked",       "HttpBody.feed",         "Mcp.prepare",
    "McpTransport.receive",   "McpValue.arguments",    "McpValue.encode",
    "Mcp.tool_reply",         "McpTransport.fallback", "McpTransport.http",
    "McpTransport.http_head", "McpTransport.http100"};

static uint64_t deadline(uint64_t now, uint64_t interval) {
    return interval > UINT64_MAX - now ? UINT64_MAX : now + interval;
}

const struct mo_bytecode_function *mo_protocol_function(const struct mo_protocol_server *s,
                                                        enum protocol_function function) {
    return &mo_image_bytecode(s->protocol)->functions[s->functions[function]];
}

struct mo_cell mo_protocol_field(const struct mo_image *image, uint32_t type, struct mo_cell value,
                                 uint32_t field) {
    struct mo_cell payload = mo_cell_get_payload(value);
    return mo_case_shape(mo_image_types(image), type, mo_cell_get_tag(value)) == MO_CASE_BOXED
               ? mo_value_field(payload, field)
               : payload;
}

bool mo_protocol_charge(struct protocol_connection *c, size_t amount) {
    if (amount > c->work_left || (!c->authenticated && amount > c->preauth_left)) {
        c->work_left = 0;
        return false;
    }
    c->work_left -= amount;
    if (!c->authenticated)
        c->preauth_left -= amount;
    return true;
}

static struct mo_service_time clock_now(void *context) {
    struct mo_protocol_server *s = context;
    union mo_listener_result result;
    if (mo_listener_call(s->listener, MO_LISTENER_NOW, NULL, &result) == MO_LISTENER_OK)
        s->now = result.now;
    else
        s->io_failed = true;
    return (struct mo_service_time){s->now.wall_us, s->now.monotonic_us};
}

bool mo_protocol_invoke(struct mo_protocol_server *s, struct protocol_connection *c,
                        enum protocol_function function, const struct mo_cell *args,
                        struct mo_cell *out) {
    struct mo_process *process = c == NULL ? s->catalog_process : c->process;
    const struct mo_bytecode_function *fn = mo_protocol_function(s, function);
    struct mo_heap *heap = mo_process_heap(process);
    struct mo_cell owned[5];
    MO_ASSERT(fn->parameter_count <= sizeof owned / sizeof *owned);
    uint64_t budget = c == NULL ? UINT64_MAX : c->work_left;
    if (c != NULL && !c->authenticated && c->preauth_left < budget)
        budget = c->preauth_left;
    mo_process_instruction_limit(process, budget);
    for (uint32_t i = 0; i < fn->parameter_count; i++)
        owned[i] = fn->parameters[i].ownership == MO_PARAMETER_OWNED
                       ? mo_value_dup(heap, fn->parameters[i].type, args[i])
                       : args[i];
    uint64_t before = clock_now(s).elapsed;
    struct mo_defect defect;
    enum mo_outcome outcome =
        mo_call(process, s->functions[function], owned, fn->parameter_count, out, &defect);
    uint64_t after = clock_now(s).elapsed;
    if (after - before > s->metrics.stage_us)
        s->metrics.stage_us = after - before;
    if (c != NULL) {
        uint64_t used = budget - mo_process_instructions_remaining(process);
        c->work_left -= used;
        if (!c->authenticated)
            c->preauth_left -= used;
        mo_protocol_observe(s, c);
    }
    return outcome == MO_RETURNED && !s->io_failed;
}

void mo_protocol_drop(struct mo_protocol_server *s, struct protocol_connection *c,
                      enum protocol_function function, struct mo_cell value) {
    mo_value_drop(mo_process_heap(c == NULL ? s->catalog_process : c->process),
                  mo_protocol_function(s, function)->result_type, value);
}

void mo_protocol_observe(struct mo_protocol_server *s, struct protocol_connection *c) {
    size_t resident = sizeof *c + c->fallback_capacity;
    size_t memory = resident, native = resident;
    if (c->process != NULL) {
        memory += mo_process_memory_peak(c->process);
        native += mo_heap_scratch_peak(mo_process_heap(c->process));
    }
    if (memory > c->peak_memory)
        c->peak_memory = memory;
    if (native > c->peak_native)
        c->peak_native = native;
    if (c->peak_memory > s->metrics.request_memory)
        s->metrics.request_memory = c->peak_memory;
    if (c->peak_native > s->metrics.native_memory)
        s->metrics.native_memory = c->peak_native;
    uint64_t work = MO_LIMIT_MCP_REQUEST_WORK - c->work_left;
    if (work > s->metrics.work)
        s->metrics.work = work;
    if (!c->authenticated) {
        uint64_t preauth_work = MO_LIMIT_MCP_HEADER_WORK - c->preauth_left;
        if (preauth_work > s->metrics.preauth_work)
            s->metrics.preauth_work = preauth_work;
        if (memory > s->metrics.preauth_memory)
            s->metrics.preauth_memory = memory;
    }
    size_t concurrent = 0;
    for (size_t i = 0; i < MO_LIMIT_MCP_CONNECTIONS; i++) {
        const struct protocol_connection *other = &s->connections[i];
        if (other->phase != PS_EMPTY) {
            size_t peak = sizeof *other + other->fallback_capacity;
            if (other->process != NULL)
                peak += mo_process_memory_peak(other->process);
            if (peak < other->peak_memory)
                peak = other->peak_memory;
            concurrent += peak;
        }
    }
    if (concurrent > s->metrics.concurrent_memory)
        s->metrics.concurrent_memory = concurrent;
}

void mo_protocol_dispose(struct mo_protocol_server *s, struct protocol_connection *c) {
    if (c->process != NULL) {
        mo_protocol_observe(s, c);
        mo_scratch_free(&c->scratch);
        mo_process_free(c->process);
        c->process = NULL;
        c->body = NULL;
    }
}

void mo_protocol_close(struct mo_protocol_server *s, struct protocol_connection *c) {
    if (c->phase == PS_EMPTY)
        return;
    if (c->retained)
        mo_value_drop(s->service_heap, c->reply.type, c->reply.value);
    union mo_listener_request request = {.connection = c->key};
    union mo_listener_result result;
    enum mo_listener_status status =
        mo_listener_call(s->listener, MO_LISTENER_CLOSE, &request, &result);
    MO_ASSERT(status == MO_LISTENER_OK || status == MO_LISTENER_STALE);
    mo_protocol_dispose(s, c);
    uint8_t *fallback = c->fallback;
    size_t capacity = c->fallback_capacity;
    mo_host_wipe(fallback, capacity);
    mo_host_wipe(c, sizeof *c);
    c->fallback = fallback;
    c->fallback_capacity = capacity;
}

void mo_protocol_start_write(struct mo_protocol_server *s, struct protocol_connection *c,
                             struct protocol_bytes bytes) {
    c->output = bytes;
    c->write_offset = 0;
    c->deadline = deadline(s->now.monotonic_us, MO_LIMIT_MCP_WRITE_US);
    c->phase = PS_WRITE;
    mo_protocol_observe(s, c);
}

void mo_protocol_fail(struct mo_protocol_server *s, struct protocol_connection *c, int status) {
    MO_ASSERT(status >= 0 && (size_t)status < sizeof s->errors / sizeof *s->errors);
    struct protocol_bytes response = s->errors[status];
    MO_ASSERT(response.bytes != NULL);
    mo_protocol_dispose(s, c);
    mo_host_wipe(c->input, sizeof c->input);
    mo_protocol_start_write(s, c, response);
}

void mo_protocol_fallback(struct mo_protocol_server *s, struct protocol_connection *c,
                          enum protocol_error error, enum protocol_effect effect) {
    const struct protocol_fallback *form = &s->fallbacks[error][effect];
    MO_ASSERT(form->prefix.bytes != NULL && c->id_size != 0);
    size_t body_size = form->prefix.size + c->id_size + form->suffix.size;
    MO_ASSERT(body_size <= s->fallback_body_max);
    const struct protocol_bytes *header = &s->fallback_headers[body_size];
    MO_ASSERT(header->size + body_size <= c->fallback_capacity);
    size_t at = 0;
    const struct protocol_bytes parts[] = {
        *header, form->prefix, {c->escaped_id, c->id_size}, form->suffix};
    for (size_t i = 0; i < sizeof parts / sizeof *parts; i++) {
        memcpy(c->fallback + at, parts[i].bytes, parts[i].size);
        at += parts[i].size;
    }
    mo_protocol_dispose(s, c);
    mo_protocol_start_write(s, c, (struct protocol_bytes){c->fallback, at});
}

bool mo_protocol_request_process(struct mo_protocol_server *s, struct protocol_connection *c) {
    struct mo_limits limits = mo_limits_default();
    size_t allowance = c->authenticated ? MO_LIMIT_MCP_REQUEST_MEMORY : MO_LIMIT_MCP_HEADER_HEAP;
    size_t resident = sizeof *c + c->fallback_capacity;
    MO_ASSERT(resident < allowance);
    limits.heap_size = allowance - resident;
    c->process = mo_process_new(s->protocol, &limits);
    if (c->process == NULL)
        return false;
    mo_process_confidential(c->process);
    mo_process_account_stack(c->process);
    mo_scratch_init(&c->scratch, mo_process_heap(c->process));
    return true;
}

static void replied(const struct mo_service_response *reply, void *context) {
    struct mo_protocol_server *s = context;
    if (reply->timer)
        return;
    for (size_t i = 0; i < MO_LIMIT_MCP_CONNECTIONS; i++) {
        struct protocol_connection *c = &s->connections[i];
        if ((c->phase != PS_SERVICE && c->phase != PS_ABANDONED) || c->id != reply->id)
            continue;
        c->reply = *reply;
        c->reply.defect = NULL;
        c->reply_ready = true;
        if (reply->status == MO_SERVICE_OK && c->phase != PS_ABANDONED) {
            c->reply.value = mo_value_dup(s->service_heap, reply->type, reply->value);
            c->retained = true;
        }
        return;
    }
}

static void event(const struct mo_service_event *value, void *context) {
    (void)value;
    (void)context;
}

static bool catalog_copy(struct mo_protocol_server *s, struct mo_cell value,
                         struct protocol_bytes *out) {
    size_t size;
    const uint8_t *bytes = mo_value_data(value, &size);
    uint8_t *copy = mo_scratch_resize(&s->catalog_scratch, NULL, 0, size == 0 ? 1 : size);
    if (copy == NULL)
        return false;
    memcpy(copy, bytes, size);
    *out = (struct protocol_bytes){copy, size};
    return true;
}

static bool templates(struct mo_protocol_server *s) {
    static const char *const codes[] = {
        "invalid_arguments", "busy",         "stopped",      "memory_limit",   "work_limit",
        "handler_failed",    "store_failed", "rate_limited", "response_limit", "internal_error"};
    static const char *const effects[] = {"none", "applied", "unknown"};
    struct mo_heap *heap = mo_process_heap(s->catalog_process);
    uint32_t text_type = mo_protocol_function(s, PF_FALLBACK)->parameters[0].type;
    size_t max_body = 0;
    for (unsigned code = 0; code < PE_COUNT; code++) {
        for (unsigned effect = 0; effect < EFFECT_COUNT; effect++) {
            bool valid = code == PE_INTERNAL || (code == PE_RESPONSE && effect != EFFECT_UNKNOWN) ||
                         (code == PE_STORE ? effect == EFFECT_UNKNOWN : effect == EFFECT_NONE);
            if (!valid)
                continue;
            struct mo_cell args[2], value;
            if (!mo_value_text(heap, codes[code], strlen(codes[code]), &args[0]))
                return false;
            if (!mo_value_text(heap, effects[effect], strlen(effects[effect]), &args[1])) {
                mo_value_drop(heap, text_type, args[0]);
                return false;
            }
            bool ok = mo_protocol_invoke(s, NULL, PF_FALLBACK, args, &value);
            mo_value_drop(heap, text_type, args[0]);
            mo_value_drop(heap, text_type, args[1]);
            if (!ok)
                return false;
            struct protocol_fallback *form = &s->fallbacks[code][effect];
            ok = catalog_copy(s, mo_value_field(value, 0), &form->prefix) &&
                 catalog_copy(s, mo_value_field(value, 1), &form->suffix);
            mo_protocol_drop(s, NULL, PF_FALLBACK, value);
            if (!ok)
                return false;
            size_t size = form->prefix.size + form->suffix.size;
            if (size > max_body)
                max_body = size;
        }
    }
    s->fallback_body_max = max_body + sizeof s->connections[0].escaped_id;
    s->fallback_headers = mo_scratch_resize(
        &s->catalog_scratch, NULL, 0, (s->fallback_body_max + 1) * sizeof *s->fallback_headers);
    if (s->fallback_headers == NULL)
        return false;
    size_t max_header = 0;
    for (size_t size = 0; size <= s->fallback_body_max; size++) {
        struct mo_cell args[] = {mo_cell_int(200), mo_cell_int((int64_t)size)}, value;
        if (!mo_protocol_invoke(s, NULL, PF_HTTP_HEAD, args, &value))
            return false;
        bool ok = catalog_copy(s, value, &s->fallback_headers[size]);
        mo_protocol_drop(s, NULL, PF_HTTP_HEAD, value);
        if (!ok)
            return false;
        if (s->fallback_headers[size].size > max_header)
            max_header = s->fallback_headers[size].size;
    }
    for (size_t i = 0; i < MO_LIMIT_MCP_CONNECTIONS; i++) {
        struct protocol_connection *c = &s->connections[i];
        c->fallback_capacity = s->fallback_body_max + max_header;
        c->fallback = mo_scratch_resize(&s->catalog_scratch, NULL, 0, c->fallback_capacity);
        if (c->fallback == NULL)
            return false;
    }
    static const int statuses[] = {100, 202, 400, 401, 403, 404, 405, 406,
                                   413, 415, 417, 421, 431, 501, 505};
    for (size_t i = 0; i < sizeof statuses / sizeof *statuses; i++) {
        struct mo_cell args[] = {mo_cell_int(statuses[i]), mo_cell_int(0)}, value;
        if (!mo_protocol_invoke(s, NULL, PF_HTTP_HEAD, args, &value))
            return false;
        bool ok = catalog_copy(s, value, &s->errors[statuses[i]]);
        mo_protocol_drop(s, NULL, PF_HTTP_HEAD, value);
        if (!ok)
            return false;
    }
    return true;
}

enum mo_protocol_server_status
mo_protocol_server_new(const struct mo_image *image, uint32_t index, struct mo_store *store,
                       struct mo_listener *listener, const char *const *authorities,
                       size_t authority_count, struct mo_protocol_server **out) {
    if (image == NULL || listener == NULL || store == NULL || out == NULL ||
        index >= mo_image_bytecode(image)->service_count || authorities == NULL ||
        authority_count == 0 || authority_count > UINT32_MAX)
        return MO_PROTOCOL_SERVER_CONFIG;
    struct mo_arena arena;
    mo_arena_init_confidential(&arena, MO_LIMIT_MCP_CATALOG_MEMORY);
    struct mo_protocol_server *s =
        mo_arena_allocate(&arena, sizeof *s, _Alignof(struct mo_protocol_server));
    if (s == NULL) {
        mo_arena_free(&arena);
        return MO_PROTOCOL_SERVER_MEMORY;
    }
    *s = (struct mo_protocol_server){.arena = arena,
                                     .image = image,
                                     .listener = listener,
                                     .descriptor = &mo_image_bytecode(image)->services[index],
                                     .service_index = index,
                                     .rate_credit = MO_LIMIT_MCP_BURST * UINT64_C(1000000)};
    enum mo_protocol_server_status status = MO_PROTOCOL_SERVER_PROTOCOL;
    s->protocol = mo_protocol_build();
    if (s->protocol == NULL)
        goto fail;
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    if (mo_image_init(s->protocol, &limits, &defect) != MO_RETURNED)
        goto fail;
    const struct mo_bytecode *file = mo_image_bytecode(s->protocol);
    for (unsigned i = 0; i < PF_COUNT; i++) {
        s->functions[i] = MO_NO_INDEX;
        for (uint32_t j = 0; j < file->function_count; j++) {
            const struct mo_bytecode_string *name = &file->strings[file->functions[j].name];
            if (name->length == strlen(function_names[i]) &&
                memcmp(name->bytes, function_names[i], name->length) == 0)
                s->functions[i] = j;
        }
        MO_ASSERT(s->functions[i] != MO_NO_INDEX);
    }
    status = MO_PROTOCOL_SERVER_MEMORY;
    limits.heap_size = MO_LIMIT_MCP_CATALOG_MEMORY - s->arena.used;
    s->catalog_process = mo_process_new(s->protocol, &limits);
    if (s->catalog_process == NULL)
        goto fail;
    mo_process_confidential(s->catalog_process);
    mo_process_account_stack(s->catalog_process);
    struct mo_heap *heap = mo_process_heap(s->catalog_process);
    mo_scratch_init(&s->catalog_scratch, heap);
    uint32_t list_type = mo_protocol_function(s, PF_AUTHORITY)->parameters[0].type;
    if (!mo_value_list(heap, list_type, NULL, 0, &s->authorities))
        goto fail;
    for (size_t i = 0; i < authority_count; i++) {
        struct mo_cell text;
        if (authorities[i] == NULL ||
            !mo_value_text(heap, authorities[i], strlen(authorities[i]), &text))
            goto fail;
        bool ok = mo_list_append(heap, list_type, &s->authorities, text);
        mo_value_drop(heap, mo_image_types(s->protocol)->items[list_type].parameters[0], text);
        if (!ok)
            goto fail;
    }
    struct mo_cell checked;
    if (!mo_protocol_invoke(s, NULL, PF_AUTHORITY, &s->authorities, &checked))
        goto fail;
    if (!mo_cell_get_bool(checked)) {
        status = MO_PROTOCOL_SERVER_CONFIG;
        goto fail;
    }
    if (!mo_value_make_immortal(heap, list_type, s->authorities) || !mo_protocol_catalog(s, NULL) ||
        !templates(s))
        goto fail;
    s->metrics.catalog_memory = s->arena.used + mo_process_memory_peak(s->catalog_process);
    struct mo_service_hooks hooks = {clock_now, replied, event, s};
    enum mo_service_status service = mo_service_open(image, index, &hooks, store, &s->service);
    if (service != MO_SERVICE_OK || s->io_failed) {
        status = s->io_failed ? MO_PROTOCOL_SERVER_IO : MO_PROTOCOL_SERVER_SERVICE;
        goto fail;
    }
    s->service_heap = mo_service_heap(s->service);
    s->rate_at = s->now.monotonic_us;
    *out = s;
    return MO_PROTOCOL_SERVER_OK;
fail:
    mo_protocol_server_free(s);
    return status;
}

void mo_protocol_server_free(struct mo_protocol_server *s) {
    if (s == NULL)
        return;
    for (size_t i = 0; i < MO_LIMIT_MCP_CONNECTIONS; i++)
        mo_protocol_close(s, &s->connections[i]);
    mo_service_free(s->service);
    if (s->catalog_process != NULL) {
        mo_scratch_free(&s->catalog_scratch);
        mo_process_free(s->catalog_process);
    }
    mo_image_free(s->protocol);
    struct mo_arena arena = s->arena;
    mo_host_wipe(s, sizeof *s);
    mo_arena_free(&arena);
}

static void write_ready(struct mo_protocol_server *s, struct protocol_connection *c) {
    union mo_listener_request request = {
        .write = {c->key, c->output.bytes + c->write_offset, c->output.size - c->write_offset}};
    union mo_listener_result result;
    enum mo_listener_status status =
        mo_listener_call(s->listener, MO_LISTENER_WRITE, &request, &result);
    if (status == MO_LISTENER_AGAIN)
        return;
    if (status != MO_LISTENER_OK) {
        mo_protocol_close(s, c);
        return;
    }
    c->write_offset += result.transferred;
    if (c->write_offset != c->output.size)
        return;
    if (c->phase == PS_CONTINUE) {
        c->phase = PS_BODY;
        if (mo_protocol_body_complete(c))
            mo_protocol_receive(s, c);
        return;
    }
    request.connection = c->key;
    if (mo_listener_call(s->listener, MO_LISTENER_HALF_CLOSE, &request, &result) !=
        MO_LISTENER_OK) {
        mo_protocol_close(s, c);
        return;
    }
    s->metrics.completed++;
    mo_protocol_dispose(s, c);
    mo_host_wipe(c->input, sizeof c->input);
    c->phase = PS_DRAIN;
    c->deadline = deadline(s->now.monotonic_us, MO_LIMIT_MCP_DRAIN_US);
}

static void drain_ready(struct mo_protocol_server *s, struct protocol_connection *c) {
    size_t capacity = MO_LIMIT_MCP_DRAIN_BYTES - c->drained;
    if (capacity > sizeof c->input)
        capacity = sizeof c->input;
    union mo_listener_request request = {.read = {c->key, c->input, capacity}};
    union mo_listener_result result;
    enum mo_listener_status status =
        mo_listener_call(s->listener, MO_LISTENER_READ, &request, &result);
    if (status == MO_LISTENER_AGAIN)
        return;
    c->drained += result.transferred;
    mo_host_wipe(c->input, sizeof c->input);
    if (status != MO_LISTENER_OK || c->drained == MO_LIMIT_MCP_DRAIN_BYTES)
        mo_protocol_close(s, c);
}

enum mo_protocol_server_status mo_protocol_server_poll(struct mo_protocol_server *s,
                                                       uint64_t until_us) {
    MO_ASSERT(s != NULL);
    (void)clock_now(s);
    if (s->io_failed)
        return MO_PROTOCOL_SERVER_IO;
    union mo_listener_request request = {.wait = {.until_us = until_us, .accept = true}};
    size_t slots[MO_LIMIT_MCP_CONNECTIONS];
    uint64_t next_service;
    if (mo_service_next_deadline(s->service, &next_service) && next_service < request.wait.until_us)
        request.wait.until_us = next_service;
    for (size_t n = 0; n < MO_LIMIT_MCP_CONNECTIONS; n++) {
        size_t slot = (s->next_slot + n) % MO_LIMIT_MCP_CONNECTIONS;
        struct protocol_connection *c = &s->connections[slot];
        if (c->phase == PS_EMPTY)
            continue;
        if (c->phase != PS_SERVICE && c->phase != PS_ABANDONED &&
            s->now.monotonic_us >= c->deadline) {
            mo_protocol_close(s, c);
            continue;
        }
        if (c->deadline < request.wait.until_us && c->phase != PS_SERVICE &&
            c->phase != PS_ABANDONED)
            request.wait.until_us = c->deadline;
        if (c->phase == PS_SERVICE || c->phase == PS_ABANDONED)
            continue;
        unsigned events = c->phase == PS_WRITE || c->phase == PS_CONTINUE ? MO_LISTENER_WRITABLE
                                                                          : MO_LISTENER_READABLE;
        slots[request.wait.count] = slot;
        request.wait.interests[request.wait.count++] =
            (struct mo_listener_interest){c->key, events};
    }
    union mo_listener_result result;
    enum mo_listener_status status =
        mo_listener_call(s->listener, MO_LISTENER_WAIT, &request, &result);
    if (status == MO_LISTENER_AGAIN)
        return MO_PROTOCOL_SERVER_OK;
    if (status != MO_LISTENER_OK)
        return MO_PROTOCOL_SERVER_IO;
    s->now = result.wait.now;
    for (size_t i = 0; i < request.wait.count; i++) {
        struct protocol_connection *c = &s->connections[slots[i]];
        unsigned events = result.wait.events[i];
        if (c->phase == PS_EMPTY)
            continue;
        if (s->now.monotonic_us >= c->deadline) {
            mo_protocol_close(s, c);
            continue;
        }
        if ((events & MO_LISTENER_ERROR) != 0) {
            if (c->phase == PS_SERVICE)
                c->phase = PS_ABANDONED;
            else if (c->phase != PS_ABANDONED)
                mo_protocol_close(s, c);
        } else if ((events & MO_LISTENER_WRITABLE) != 0)
            write_ready(s, c);
        else if ((events & MO_LISTENER_READABLE) != 0) {
            if (c->phase == PS_DRAIN)
                drain_ready(s, c);
            else if (c->phase != PS_ABANDONED)
                mo_protocol_read(s, c);
        }
    }
    uint64_t before = clock_now(s).elapsed;
    bool stepped = mo_service_step(s->service);
    uint64_t elapsed = clock_now(s).elapsed - before;
    if (elapsed > s->metrics.stage_us)
        s->metrics.stage_us = elapsed;
    (void)stepped;
    for (size_t i = 0; i < MO_LIMIT_MCP_CONNECTIONS; i++)
        if (s->connections[i].reply_ready)
            mo_protocol_complete(s, &s->connections[i]);
    if (result.wait.accept) {
        status = mo_listener_call(s->listener, MO_LISTENER_ACCEPT, NULL, &result);
        if (status == MO_LISTENER_OK) {
            struct protocol_connection *c = &s->connections[result.accepted.slot];
            MO_ASSERT(c->phase == PS_EMPTY);
            c->key = result.accepted;
            c->phase = PS_HEADERS;
            c->deadline = deadline(s->now.monotonic_us, MO_LIMIT_MCP_HEADER_US);
            c->work_left = MO_LIMIT_MCP_REQUEST_WORK;
            c->preauth_left = MO_LIMIT_MCP_HEADER_WORK;
            if (!mo_protocol_request_process(s, c))
                mo_protocol_close(s, c);
            else {
                struct mo_cell fields[] = {mo_cell_int(0), mo_cell_int(0), mo_cell_bool(false)};
                uint32_t scan_type = mo_protocol_function(s, PF_SCAN)->parameters[1].type;
                if (!mo_value_case(mo_process_heap(c->process), scan_type, 0, fields, 3, &c->scan))
                    mo_protocol_close(s, c);
            }
        } else if (status != MO_LISTENER_AGAIN && status != MO_LISTENER_FULL)
            return MO_PROTOCOL_SERVER_IO;
    }
    s->next_slot = (s->next_slot + 1) % MO_LIMIT_MCP_CONNECTIONS;
    return s->io_failed ? MO_PROTOCOL_SERVER_IO : MO_PROTOCOL_SERVER_OK;
}

struct mo_protocol_metrics mo_protocol_server_metrics(const struct mo_protocol_server *s) {
    MO_ASSERT(s != NULL);
    return s->metrics;
}
