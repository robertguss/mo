#include "vm/internal.h"

#include <string.h>

#include "base/binary.h"
#include "base/export.h"
#include "base/failure.h"
#include "base/log-record.h"
#include "base/runtime-identity.h"

struct service_message {
    struct mo_cell arguments[MO_LIMIT_BYTECODE_PARAMETERS - 2];
    int64_t arrival;
    uint64_t id;
    size_t argument_bytes;
    bool argument_bytes_available;
    uint32_t handler;
    bool timer;
};

struct service_timer {
    mo_u128 next;
    uint64_t interval;
    bool waiting;
};

struct mo_service_loop {
    struct mo_arena arena;
    struct mo_process *process;
    const struct mo_bytecode_service *descriptor;
    struct mo_service_hooks hooks;
    struct mo_cell state;
    struct service_message mailbox[MO_LIMIT_SERVICE_MAILBOX];
    struct service_timer *timers;
    uint64_t failures[MO_LIMIT_SERVICE_FAILURES];
    uint64_t restarts[MO_LIMIT_SERVICE_RESTARTS];
    uint64_t last_clock;
    struct mo_store *store;
    uint64_t sequence, image_key, snapshot_sequence, encoded_sequence;
    struct mo_buffer current_encoding;
    struct mo_service_message_summary log[MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES];
    uint32_t log_count;
    struct mo_service_event_summary events[MO_LIMIT_SERVICE_EVENTS];
    uint64_t event_number;
    uint32_t event_head, event_count;
    uint32_t head, count, failure_count, restart_count;
    uint64_t capture_id;
    const char *capture_refusal;
    bool capture_unsafe;
    bool running;
};

const char *mo_service_status_text(enum mo_service_status status) {
    switch (status) {
    case MO_SERVICE_OK:
        return "service message completed";
    case MO_SERVICE_BUSY:
        return "service mailbox is full";
    case MO_SERVICE_DOWN:
        return "service is stopped";
    case MO_SERVICE_MEMORY:
        return "service memory limit exceeded";
    case MO_SERVICE_WORK:
        return "service work limit exceeded";
    case MO_SERVICE_DEFECT:
        return "service handler failed";
    case MO_SERVICE_INTERVAL:
        return "service timer interval must be positive";
    case MO_SERVICE_STORE:
        return mo_store_status_text(MO_STORE_IO);
    case MO_SERVICE_SAVED_INVALID:
        return mo_store_status_text(MO_STORE_CORRUPT);
    }
    MO_UNREACHABLE();
}

struct mo_limits mo_service_limits(void) {
    struct mo_limits limits = mo_limits_default();
    limits.heap_size = MO_LIMIT_SERVICE_MESSAGE_MEMORY - limits.stack_size;
    return limits;
}

bool mo_service_budgets_match(const struct mo_failure_record *record,
                              struct mo_service_budget *differs) {
    MO_ASSERT(record != NULL && differs != NULL);
    struct mo_limits limits = mo_service_limits();
    const struct mo_service_budget budgets[] = {
        {"work", record->work, MO_LIMIT_SERVICE_WORK},
        {"heap", record->heap, limits.heap_size},
        {"stack", record->stack, limits.stack_size},
        {"depth", record->depth, limits.call_depth},
        {"saved bytes", record->saved, MO_LIMIT_SAVED_BYTES}};
    for (size_t i = 0; i < sizeof budgets / sizeof budgets[0]; i++)
        if (budgets[i].recorded != budgets[i].expected) {
            *differs = budgets[i];
            return false;
        }
    return true;
}

static struct mo_service_time sample_clock(struct mo_service_loop *s) {
    struct mo_service_time time = s->hooks.clock(s->hooks.context);
    MO_ASSERT(time.elapsed >= s->last_clock);
    s->last_clock = time.elapsed;
    return time;
}

static struct mo_bytecode_string without_module_or_overload(struct mo_bytecode_string name) {
    uint32_t start = 0, end = 0;
    while (end != name.length && name.bytes[end] != '@')
        if (name.bytes[end++] == '.')
            start = end;
    return (struct mo_bytecode_string){end - start, name.bytes + start};
}

static struct mo_bytecode_string handler_name(const struct mo_service_loop *s, uint32_t function) {
    const struct mo_bytecode_service *service = s->descriptor;
    if (function == MO_NO_INDEX || function == service->start)
        return (struct mo_bytecode_string){0};
    for (uint32_t i = 0; i < service->timer_count; i++)
        if (service->timers[i].interval == function) {
            function = service->timers[i].function;
            break;
        }
    const struct mo_bytecode *file = s->process->file;
    MO_ASSERT(function < file->function_count);
    return without_module_or_overload(file->strings[file->functions[function].name]);
}

static void log_event(struct mo_service_loop *s, enum mo_service_event_kind kind,
                      enum mo_service_status reason, struct mo_service_time time,
                      const struct mo_defect *defect, uint32_t handler, uint32_t restart) {
    struct mo_service_event event = {.kind = kind,
                                     .reason = reason,
                                     .time = time,
                                     .defect = defect,
                                     .function = MO_NO_INDEX,
                                     .handler = handler_name(s, handler),
                                     .restart = restart};
    if (kind == MO_SERVICE_FAILURE) {
        event.record_id = s->capture_id;
        event.record_unavailable = s->capture_refusal;
    }
    uint32_t slot = (s->event_head + s->event_count) % MO_LIMIT_SERVICE_EVENTS;
    if (s->event_count == MO_LIMIT_SERVICE_EVENTS)
        s->event_head = (s->event_head + 1) % MO_LIMIT_SERVICE_EVENTS;
    else
        s->event_count++;
    s->events[slot] =
        (struct mo_service_event_summary){.number = ++s->event_number,
                                          .kind = kind,
                                          .reason = reason,
                                          .time = time,
                                          .function = handler,
                                          .restart = restart,
                                          .record_id = event.record_id,
                                          .record_unavailable = event.record_unavailable};
    s->hooks.event(&event, s->hooks.context);
}

/* Service processes have no external capabilities. Every transferred value is
 * either still owned by a frame or already dropped by the interpreter. The
 * service retains its own state/argument references across this unwind. */
static void finish_call(struct mo_process *p) {
    while (p->depth != 0) {
        struct mo_vm_frame *frame = mo_vm_frame(p);
        const struct mo_bytecode_function *fn = &p->file->functions[frame->function];
        struct mo_cell *cells = mo_vm_cells(frame);
        uint8_t *states = mo_vm_states(p, frame);
        for (uint32_t i = 0; i < fn->register_count; i++) {
            if (states[i] == VM_OWNED)
                mo_value_drop(p->heap, fn->register_types[i], cells[i]);
            else if (states[i] == VM_TOKEN) {
                struct mo_token token;
                memcpy(&token, &cells[i], sizeof token);
                mo_token_free(p->heap, &token);
            }
        }
        p->used = p->top;
        p->top = frame->previous;
        p->depth--;
    }
    p->terminal = false;
    p->cancelled = false;
    p->defect = (struct mo_defect){0};
}

static bool work_remaining(void *context) {
    uint64_t *remaining = context;
    if (*remaining == 0)
        return false;
    (*remaining)--;
    return true;
}

/* The work budget: MO_LIMIT_SERVICE_WORK instructions run, and the poll after them cancels. */
static enum mo_service_status invoke(struct mo_process *p, uint32_t function,
                                     const struct mo_cell *arguments, uint32_t count,
                                     struct mo_cell *out, struct mo_defect *defect) {
    uint64_t remaining = (uint64_t)MO_LIMIT_SERVICE_WORK + 1;
    enum mo_controlled_outcome outcome =
        mo_call_controlled(p, function, arguments, count, work_remaining, &remaining, out, defect);
    switch (outcome) {
    case MO_CONTROL_RETURNED:
        return MO_SERVICE_OK;
    case MO_CONTROL_CANCELLED:
        return MO_SERVICE_WORK;
    case MO_CONTROL_STOPPED:
    case MO_CONTROL_FAILED:
        return defect->memory ? MO_SERVICE_MEMORY : MO_SERVICE_DEFECT;
    }
    MO_UNREACHABLE();
}

static uint32_t handler_function(const struct mo_bytecode_service *service, bool timer,
                                 uint32_t handler) {
    MO_ASSERT(handler < (timer ? service->timer_count : service->tool_count));
    return timer ? service->timers[handler].function : service->tools[handler].function;
}

/* An updating tool's Ok carries its new state first; a timer's result is the new state. */
static bool replacement(const struct mo_process *p, const struct mo_bytecode_service *service,
                        bool timer, uint32_t handler, uint32_t type, struct mo_cell value,
                        struct mo_cell *out) {
    if (!timer && service->tools[handler].read_only)
        return false;
    struct mo_cell state = value;
    if (!timer) {
        if (p->types->items[type].kind == MO_TYPE_CHOICE) {
            if (mo_cell_get_tag(state) != 0)
                return false;
            state = mo_cell_get_payload(state);
        }
        state = mo_value_field(state, 0);
    }
    *out = state;
    return true;
}

/* Copies through the saved codec, so the live value is as unshared as the one
 * recovery decodes and never larger than a record payload. */
static bool encoded_copy(struct mo_heap *heap, uint32_t type, struct mo_cell value,
                         struct mo_cell *out, struct mo_buffer *encoded,
                         enum mo_saved_status *status) {
    mo_buffer_clear(encoded);
    *status = mo_value_encode_status(heap, type, value, encoded);
    return *status == MO_SAVED_OK &&
           mo_value_decode(heap, type, encoded->bytes, encoded->length, out);
}

static bool saved_copy(struct mo_service_loop *s, uint32_t type, struct mo_cell value,
                       struct mo_cell *out, struct mo_buffer *encoded) {
    if (s->store == NULL) {
        *out = mo_value_dup(s->process->heap, type, value);
        return true;
    }
    enum mo_saved_status status;
    return encoded_copy(s->process->heap, type, value, out, encoded, &status);
}

void mo_service_call_handler(struct mo_process *p, const struct mo_bytecode_service *service,
                             bool timer, uint32_t handler, struct mo_cell state, int64_t arrival,
                             const struct mo_cell *arguments, struct mo_buffer *encoded,
                             struct mo_service_call *call) {
    MO_ASSERT(p != NULL && service != NULL && call != NULL);
    MO_ASSERT(service >= p->file->services && service < p->file->services + p->file->service_count);
    uint32_t function = handler_function(service, timer, handler);
    const struct mo_bytecode_function *fn = &p->file->functions[function];
    MO_ASSERT(fn->parameter_count >= 2 && (fn->parameter_count == 2 || arguments != NULL));
    struct mo_cell values[MO_LIMIT_BYTECODE_PARAMETERS];
    values[0] = state;
    values[1] = mo_cell_time(arrival);
    for (uint32_t i = 2; i < fn->parameter_count; i++)
        values[i] = arguments[i - 2];
    for (uint32_t i = 0; i < fn->parameter_count; i++)
        if (fn->parameters[i].ownership == MO_PARAMETER_OWNED)
            values[i] = mo_value_dup(p->heap, fn->parameters[i].type, values[i]);
    *call = (struct mo_service_call){
        .function = function, .type = fn->result_type, .saved = MO_SAVED_OK};
    call->status = invoke(p, function, values, fn->parameter_count, &call->value, &call->defect);
    call->returned = call->status == MO_SERVICE_OK;
    struct mo_cell next;
    if (!call->returned || !replacement(p, service, timer, handler, call->type, call->value, &next))
        return;
    uint32_t type = service->state_type;
    if (encoded == NULL)
        call->state = mo_value_dup(p->heap, type, next);
    else if (!encoded_copy(p->heap, type, next, &call->state, encoded, &call->saved)) {
        call->status = MO_SERVICE_MEMORY;
        return;
    }
    call->updated = true;
}

static void replace_encoding(struct mo_service_loop *s, struct mo_buffer *encoded,
                             uint64_t sequence) {
    s->encoded_sequence = sequence;
    mo_buffer_free(&s->current_encoding);
    s->current_encoding = *encoded;
    mo_buffer_init(encoded, MO_LIMIT_SAVED_BYTES);
}

static bool snapshot(struct mo_service_loop *s, const uint8_t *bytes, size_t length) {
    struct mo_store_record record = {MO_STORE_STATE, s->sequence, s->image_key, bytes, length};
    bool ok = mo_store_call(s->store, MO_STORE_SNAPSHOT, &record, NULL, NULL) == MO_STORE_OK;
    if (ok) {
        s->snapshot_sequence = s->sequence;
        s->log_count = 0;
    }
    return ok;
}

struct recovery {
    struct mo_service_loop *service;
    struct mo_cell state;
    uint64_t sequence, snapshot;
    enum mo_service_status limit;
    uint32_t failed_function;
    bool found;
};

static bool restore_record(const struct mo_store_record *record, void *context) {
    struct recovery *r = context;
    struct mo_service_loop *s = r->service;
    struct mo_heap *heap = s->process->heap;
    if (record->image != s->image_key)
        return false;
    if (record->kind == MO_STORE_STATE) {
        if (r->found || !mo_value_decode(heap, s->descriptor->state_type, record->bytes,
                                         record->length, &r->state))
            return false;
        if (!mo_buffer_append(&s->current_encoding, record->bytes, record->length)) {
            mo_value_drop(heap, s->descriptor->state_type, r->state);
            r->limit = MO_SERVICE_MEMORY;
            return false;
        }
        r->found = true;
        r->sequence = r->snapshot = record->sequence;
        s->encoded_sequence = record->sequence;
        return true;
    }
    struct mo_binary in = {record->bytes, record->length, 0};
    struct mo_log_record_header header;
    if (!r->found || record->sequence != r->sequence + 1 ||
        !mo_log_record_header_read(&in, &header))
        return false;
    uint64_t expected = header.state_checksum;
    struct service_message m = {
        .arrival = header.arrival, .handler = header.handler, .timer = header.timer};
    if (m.arrival < MO_TIME_MIN_US || m.arrival > MO_TIME_MAX_US ||
        m.handler >= (m.timer ? s->descriptor->timer_count : s->descriptor->tool_count))
        return false;
    const struct mo_bytecode_service_tool *tool = m.timer ? NULL : &s->descriptor->tools[m.handler];
    if (header.count != (m.timer ? 0 : tool->argument_count) || (!m.timer && tool->read_only))
        return false;
    uint32_t built = 0;
    bool ok = true;
    while (ok && built < header.count) {
        size_t length;
        const uint8_t *bytes;
        ok = mo_log_record_argument_read(&in, &bytes, &length) &&
             mo_value_decode(heap, tool->argument_types[built], bytes, length, &m.arguments[built]);
        if (ok) {
            m.argument_bytes += length;
            built++;
        }
    }
    ok = ok && in.position == in.length;
    if (ok) {
        struct mo_service_call call;
        struct mo_buffer encoded;
        mo_buffer_init(&encoded, MO_LIMIT_SAVED_BYTES);
        mo_service_call_handler(s->process, s->descriptor, m.timer, m.handler, r->state, m.arrival,
                                m.arguments, &encoded, &call);
        if (!call.returned && (call.status == MO_SERVICE_MEMORY || call.status == MO_SERVICE_WORK))
            r->limit = call.status;
        ok = call.updated;
        if (ok && mo_binary_checksum(encoded.bytes, encoded.length) != expected) {
            mo_value_drop(heap, s->descriptor->state_type, call.state);
            ok = false;
        }
        if (ok) {
            replace_encoding(s, &encoded, record->sequence);
        }
        mo_buffer_free(&encoded);
        if (ok) {
            mo_value_drop(heap, s->descriptor->state_type, r->state);
            r->state = call.state;
            r->sequence = record->sequence;
        } else
            r->failed_function = call.function;
        if (call.returned)
            mo_value_drop(heap, call.type, call.value);
        finish_call(s->process);
    }
    for (uint32_t i = 0; i < built; i++)
        mo_value_drop(heap, tool->argument_types[i], m.arguments[i]);
    if (ok) {
        MO_ASSERT(s->log_count < MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES);
        s->log[s->log_count++] =
            (struct mo_service_message_summary){.arrival = m.arrival,
                                                .id = record->sequence,
                                                .argument_bytes = m.argument_bytes,
                                                .handler = m.handler,
                                                .timer = m.timer,
                                                .argument_bytes_available = true};
    }
    return ok;
}

static enum mo_service_status recover(struct mo_service_loop *s, bool *found, struct mo_cell *state,
                                      bool *snapshot_due, uint32_t *failed_function) {
    struct recovery r = {.service = s, .limit = MO_SERVICE_OK, .failed_function = MO_NO_INDEX};
    enum mo_store_status status = mo_store_call(s->store, MO_STORE_LOAD, NULL, restore_record, &r);
    if (status != MO_STORE_OK) {
        *failed_function = r.failed_function;
        if (r.found)
            mo_value_drop(s->process->heap, s->descriptor->state_type, r.state);
        if (r.limit != MO_SERVICE_OK)
            return r.limit;
        return status == MO_STORE_CORRUPT ? MO_SERVICE_SAVED_INVALID : MO_SERVICE_STORE;
    }
    *found = r.found;
    if (r.found) {
        *state = r.state;
        s->sequence = r.sequence;
        s->snapshot_sequence = r.snapshot;
        *snapshot_due =
            r.sequence != r.snapshot && r.sequence % MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES == 0;
    }
    return MO_SERVICE_OK;
}

static bool persist(struct mo_service_loop *s, const struct service_message *m,
                    const struct mo_buffer *state) {
    if (s->store == NULL)
        return true;
    struct mo_buffer bytes, value;
    mo_buffer_init(&bytes, MO_LIMIT_SAVED_BYTES);
    mo_buffer_init(&value, MO_LIMIT_SAVED_BYTES);
    uint32_t count = m->timer ? 0 : s->descriptor->tools[m->handler].argument_count;
    struct mo_log_record_header header = {.state_checksum =
                                              mo_binary_checksum(state->bytes, state->length),
                                          .arrival = m->arrival,
                                          .handler = m->handler,
                                          .count = count,
                                          .timer = m->timer};
    bool ok = mo_log_record_header_write(&bytes, &header);
    for (uint32_t i = 0; ok && i < count; i++) {
        mo_buffer_clear(&value);
        ok = mo_value_encode(s->process->heap, s->descriptor->tools[m->handler].argument_types[i],
                             m->arguments[i], &value) &&
             mo_log_record_argument_write(&bytes, value.bytes, value.length);
    }
    if (ok) {
        struct mo_store_record record = {MO_STORE_LOG, s->sequence + 1, s->image_key, bytes.bytes,
                                         bytes.length};
        ok = mo_store_call(s->store, MO_STORE_APPEND, &record, NULL, NULL) == MO_STORE_OK;
    }
    mo_buffer_free(&value);
    mo_buffer_free(&bytes);
    if (ok) {
        s->sequence++;
        MO_ASSERT(s->log_count < MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES);
        s->log[s->log_count++] =
            (struct mo_service_message_summary){.arrival = m->arrival,
                                                .id = s->sequence,
                                                .argument_bytes = m->argument_bytes,
                                                .handler = m->handler,
                                                .timer = m->timer,
                                                .argument_bytes_available = true};
        if (s->sequence % MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES == 0)
            ok = snapshot(s, state->bytes, state->length);
    }
    return ok;
}

static struct service_message *append(struct mo_service_loop *s) {
    MO_ASSERT(s->count < MO_LIMIT_SERVICE_MAILBOX);
    uint32_t index = (s->head + s->count++) % MO_LIMIT_SERVICE_MAILBOX;
    return &s->mailbox[index];
}

static void advance_timer(struct service_timer *timer, uint64_t elapsed) {
    timer->next += (((mo_u128)elapsed - timer->next) / timer->interval + 1) * timer->interval;
}

static void poll_timers(struct mo_service_loop *s, struct mo_service_time time) {
    for (uint32_t i = 0; i < s->descriptor->timer_count; i++) {
        struct service_timer *timer = &s->timers[i];
        if (timer->waiting && timer->next <= time.elapsed)
            advance_timer(timer, time.elapsed);
    }
    /* Admission is bounded by mailbox capacity. Keep older, unqueued ticks
     * ahead of newly due ones, or a full mailbox can starve startup timers. */
    while (s->count < MO_LIMIT_SERVICE_MAILBOX) {
        uint32_t first = MO_NO_INDEX;
        for (uint32_t i = 0; i < s->descriptor->timer_count; i++) {
            const struct service_timer *timer = &s->timers[i];
            if (!timer->waiting && timer->next <= time.elapsed &&
                (first == MO_NO_INDEX || timer->next < s->timers[first].next))
                first = i;
        }
        if (first == MO_NO_INDEX)
            break;
        *append(s) = (struct service_message){.arrival = time.now, .handler = first, .timer = true};
        s->timers[first].waiting = true;
        advance_timer(&s->timers[first], time.elapsed);
    }
}

static void start_timers(struct mo_service_loop *s, struct mo_service_time time) {
    for (uint32_t i = 0; i < s->descriptor->timer_count; i++) {
        s->timers[i].waiting = false;
        s->timers[i].next = time.elapsed;
    }
    poll_timers(s, time);
}

enum mo_service_status mo_service_new(const struct mo_image *image, uint32_t index,
                                      const struct mo_service_hooks *hooks,
                                      struct mo_service_loop **out) {
    return mo_service_open(image, index, hooks, NULL, out);
}

enum mo_service_status mo_service_open(const struct mo_image *image, uint32_t index,
                                       const struct mo_service_hooks *hooks, struct mo_store *store,
                                       struct mo_service_loop **out) {
    MO_ASSERT(image != NULL && hooks != NULL && hooks->clock != NULL && hooks->reply != NULL &&
              hooks->event != NULL && out != NULL);
    const struct mo_bytecode *file = mo_image_bytecode(image);
    MO_ASSERT(index < file->service_count);
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_SERVICE_METADATA);
    struct mo_service_loop *s =
        mo_arena_allocate(&arena, sizeof *s, _Alignof(struct mo_service_loop));
    if (s == NULL)
        return MO_SERVICE_MEMORY;
    *s = (struct mo_service_loop){.descriptor = &file->services[index],
                                  .hooks = *hooks,
                                  .store = store,
                                  .image_key = mo_image_key(image, index)};
    mo_buffer_init(&s->current_encoding, MO_LIMIT_SAVED_BYTES);
    s->timers = mo_arena_allocate_array(&arena, s->descriptor->timer_count, sizeof *s->timers,
                                        _Alignof(struct service_timer));
    s->arena = arena;
    if (s->timers == NULL) {
        mo_service_free(s);
        return MO_SERVICE_MEMORY;
    }
    memset(s->timers, 0, s->descriptor->timer_count * sizeof *s->timers);
    struct mo_limits limits = mo_service_limits();
    s->process = mo_process_new(image, &limits);
    if (s->process == NULL) {
        mo_service_free(s);
        return MO_SERVICE_MEMORY;
    }
    struct mo_cell value;
    struct mo_defect defect = {0};
    enum mo_service_status status = MO_SERVICE_OK;
    uint32_t failed_function = s->descriptor->start;
    for (uint32_t i = 0; i < s->descriptor->timer_count; i++) {
        failed_function = s->descriptor->timers[i].interval;
        status = invoke(s->process, s->descriptor->timers[i].interval, NULL, 0, &value, &defect);
        if (status != MO_SERVICE_OK)
            break;
        int64_t interval = mo_cell_get_duration(value);
        if (interval <= 0) {
            status = MO_SERVICE_INTERVAL;
            break;
        }
        s->timers[i].interval = (uint64_t)interval;
    }
    if (status == MO_SERVICE_OK)
        failed_function = MO_NO_INDEX;
    bool found = false, snapshot_due = false;
    struct mo_buffer encoded;
    mo_buffer_init(&encoded, MO_LIMIT_SAVED_BYTES);
    if (status == MO_SERVICE_OK && store != NULL)
        status = recover(s, &found, &s->state, &snapshot_due, &failed_function);
    if (status == MO_SERVICE_OK && !found) {
        failed_function = s->descriptor->start;
        status = invoke(s->process, s->descriptor->start, NULL, 0, &s->state, &defect);
    }
    if (status == MO_SERVICE_OK && !found && store != NULL) {
        struct mo_cell original = s->state;
        if (!saved_copy(s, s->descriptor->state_type, original, &s->state, &encoded))
            status = MO_SERVICE_MEMORY;
        else
            mo_value_drop(s->process->heap, s->descriptor->state_type, original);
        snapshot_due = true;
    }
    /* Recovery may stop between an append and its due snapshot; finish it now. */
    if (status == MO_SERVICE_OK)
        failed_function = MO_NO_INDEX;
    if (status == MO_SERVICE_OK && snapshot_due &&
        !(encoded.length != 0 ? snapshot(s, encoded.bytes, encoded.length)
                              : snapshot(s, s->current_encoding.bytes, s->current_encoding.length)))
        status = MO_SERVICE_STORE;
    if (status == MO_SERVICE_OK && encoded.length != 0) {
        replace_encoding(s, &encoded, s->sequence);
    }
    mo_buffer_free(&encoded);
    struct mo_service_time time = sample_clock(s);
    if (status != MO_SERVICE_OK) {
        const struct mo_service_event event = {.kind = MO_SERVICE_FAILURE,
                                               .reason = status,
                                               .time = time,
                                               .defect = defect.reason == NULL ? NULL : &defect,
                                               .function = failed_function,
                                               .handler = handler_name(s, failed_function)};
        s->hooks.event(&event, s->hooks.context);
        mo_service_free(s);
        return status;
    }
    s->running = true;
    start_timers(s, time);
    *out = s;
    return MO_SERVICE_OK;
}

void mo_service_free(struct mo_service_loop *s) {
    if (s != NULL) {
        struct mo_arena arena = s->arena;
        mo_buffer_free(&s->current_encoding);
        mo_process_free(s->process);
        mo_arena_free(&arena);
    }
}

struct mo_heap *mo_service_heap(struct mo_service_loop *s) {
    MO_ASSERT(s != NULL);
    return s->process->heap;
}

struct mo_cell mo_service_state(const struct mo_service_loop *s) {
    MO_ASSERT(s != NULL);
    return s->state;
}

uint32_t mo_service_pending(const struct mo_service_loop *s) {
    MO_ASSERT(s != NULL);
    return s->count;
}

bool mo_service_running(const struct mo_service_loop *s) {
    MO_ASSERT(s != NULL);
    return s->running;
}

bool mo_service_next_deadline(const struct mo_service_loop *s, uint64_t *out) {
    MO_ASSERT(s != NULL && out != NULL);
    if (!s->running)
        return false;
    if (s->count != 0) {
        *out = 0;
        return true;
    }
    mo_u128 first = (mo_u128)UINT64_MAX + 1;
    for (uint32_t i = 0; i < s->descriptor->timer_count; i++)
        if (!s->timers[i].waiting && s->timers[i].next < first)
            first = s->timers[i].next;
    if (first > UINT64_MAX)
        return false;
    *out = (uint64_t)first;
    return true;
}

uint64_t mo_service_deadlines(const struct mo_service_loop *s, uint64_t until, uint64_t *next) {
    MO_ASSERT(s != NULL && next != NULL && s->count == 0);
    mo_u128 count = 0, first = (mo_u128)UINT64_MAX + 1;
    if (!s->running)
        return 0;
    for (uint32_t i = 0; i < s->descriptor->timer_count; i++) {
        const struct service_timer *timer = &s->timers[i];
        MO_ASSERT(!timer->waiting && timer->interval != 0);
        if (timer->next > until)
            continue;
        count += ((mo_u128)until - timer->next) / timer->interval + 1;
        if (timer->next < first)
            first = timer->next;
    }
    if (count != 0)
        *next = (uint64_t)first;
    return count > UINT64_MAX ? UINT64_MAX : (uint64_t)count;
}

enum mo_service_status mo_service_send(struct mo_service_loop *s, uint32_t tool,
                                       const struct mo_cell *arguments, uint32_t count,
                                       uint64_t id) {
    MO_ASSERT(s != NULL && tool < s->descriptor->tool_count);
    const struct mo_bytecode_service_tool *handler = &s->descriptor->tools[tool];
    MO_ASSERT(count == handler->argument_count && (count == 0 || arguments != NULL));
    if (!s->running)
        return MO_SERVICE_DOWN;
    if (s->count == MO_LIMIT_SERVICE_MAILBOX)
        return MO_SERVICE_BUSY;
    struct mo_service_time time = sample_clock(s);
    struct service_message message = {.arrival = time.now, .handler = tool, .id = id};
    struct mo_buffer encoded;
    mo_buffer_init(&encoded, MO_LIMIT_SAVED_BYTES);
    size_t payload = MO_LOG_RECORD_HEADER_BYTES;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t type = handler->argument_types[i];
        if (handler->read_only)
            message.arguments[i] = mo_value_dup(s->process->heap, type, arguments[i]);
        else if (!saved_copy(s, type, arguments[i], &message.arguments[i], &encoded) ||
                 (payload += MO_LOG_RECORD_LENGTH_BYTES + encoded.length) > MO_LIMIT_SAVED_BYTES) {
            uint32_t built = payload > MO_LIMIT_SAVED_BYTES ? i + 1 : i;
            for (uint32_t j = 0; j < built; j++)
                mo_value_drop(s->process->heap, handler->argument_types[j], message.arguments[j]);
            mo_buffer_free(&encoded);
            return MO_SERVICE_MEMORY;
        }
    }
    message.argument_bytes_available = count == 0 || (s->store != NULL && !handler->read_only);
    message.argument_bytes =
        message.argument_bytes_available
            ? payload - MO_LOG_RECORD_HEADER_BYTES - count * MO_LOG_RECORD_LENGTH_BYTES
            : 0;
    *append(s) = message;
    mo_buffer_free(&encoded);
    return MO_SERVICE_OK;
}

static void drop_message(struct mo_service_loop *s, const struct service_message *message) {
    if (message->timer)
        return;
    const struct mo_bytecode_service_tool *tool = &s->descriptor->tools[message->handler];
    for (uint32_t i = 0; i < tool->argument_count; i++)
        mo_value_drop(s->process->heap, tool->argument_types[i], message->arguments[i]);
}

static uint32_t record(uint64_t *times, uint32_t count, uint64_t now, uint64_t window) {
    uint32_t keep = 0;
    for (uint32_t i = 0; i < count; i++)
        if (now - times[i] < window)
            times[keep++] = times[i];
    times[keep++] = now;
    return keep;
}

static void drain(struct mo_service_loop *s, bool keep) {
    uint32_t old_count = s->count, head = s->head;
    s->count = 0;
    for (uint32_t i = 0; i < old_count; i++) {
        struct service_message message = s->mailbox[(head + i) % MO_LIMIT_SERVICE_MAILBOX];
        if (keep && !message.timer) {
            *append(s) = message;
            continue;
        }
        if (!message.timer) {
            struct mo_service_response reply = {
                .id = message.id, .handler = message.handler, .status = MO_SERVICE_DOWN};
            s->hooks.reply(&reply, s->hooks.context);
        }
        drop_message(s, &message);
    }
    if (!keep)
        s->head = (head + old_count) % MO_LIMIT_SERVICE_MAILBOX;
}

static bool capture_string(struct mo_buffer *out, const char *text) {
    return mo_failure_text(out, text, strlen(text));
}

struct captured_variables {
    struct mo_buffer *context;
    struct mo_heap *heap;
    const struct mo_defect_frame *top;
    bool unavailable;
};
static bool capture_variable(const struct mo_defect_variable *v, void *context) {
    struct captured_variables *c = context;
    if (v->frame != c->top)
        return false;
    struct mo_buffer value;
    mo_buffer_init(&value, MO_LIMIT_FAILURE_VALUE_BYTES);
    bool rendered = mo_value_print_literal(c->heap, v->type, v->value, &value);
    bool ok = mo_buffer_append_text(c->context, "variable ") &&
              mo_buffer_append(c->context, v->name, v->name_length) &&
              mo_buffer_append_text(c->context, rendered ? " available: " : " unavailable: ") &&
              (!rendered || mo_buffer_append(c->context, value.bytes, value.length)) &&
              mo_buffer_append_byte(c->context, '\n');
    c->unavailable = c->unavailable || !rendered;
    mo_buffer_free(&value);
    return ok;
}

/* The store binds the id; the payload must decode once it does. */
static bool capture_readable(struct mo_buffer *message) {
    struct mo_failure_record record;
    message->bytes[MO_FAILURE_ID_OFFSET] = 1;
    bool readable = mo_failure_read(message->bytes, message->length, &record);
    message->bytes[MO_FAILURE_ID_OFFSET] = 0;
    return readable;
}

static struct mo_failure_slice slice(const char *text) {
    return (struct mo_failure_slice){(const uint8_t *)text, strlen(text)};
}

struct mo_signature mo_service_signature(const struct mo_service_call *call) {
    MO_ASSERT(call->status == MO_SERVICE_MEMORY || call->status == MO_SERVICE_WORK ||
              call->status == MO_SERVICE_DEFECT);
    const struct mo_defect *defect = &call->defect;
    /* A returned handler failed only in its new state's saved copy; the size limits are named. */
    bool state_size =
        call->returned && call->saved != MO_SAVED_OK && call->saved != MO_SAVED_MEMORY;
    bool work = call->status == MO_SERVICE_WORK;
    struct mo_signature signature = {
        .function = defect->frames == NULL ? call->function : defect->frames->function,
        .file = defect->reason == NULL ? MO_NO_INDEX : defect->span.file,
        .start = defect->reason == NULL ? 0 : defect->span.start,
        .end = defect->reason == NULL ? 0 : defect->span.end};
    if (state_size) {
        signature.class = MO_SIGNATURE_STATE_SIZE;
        signature.reason = slice("saved state size");
        bool depth = call->saved == MO_SAVED_DEPTH, nodes = call->saved == MO_SAVED_NODES;
        signature.limit = slice(depth   ? "MO_LIMIT_SAVED_DEPTH"
                                : nodes ? "MO_LIMIT_SAVED_NODES"
                                        : "MO_LIMIT_SAVED_BYTES");
        signature.limit_value = depth   ? MO_LIMIT_SAVED_DEPTH
                                : nodes ? MO_LIMIT_SAVED_NODES
                                        : MO_LIMIT_SAVED_BYTES;
    } else if (work) {
        signature.class = MO_SIGNATURE_WORK;
        signature.reason = slice("work limit");
        signature.limit = slice("MO_LIMIT_SERVICE_WORK");
        signature.limit_value = MO_LIMIT_SERVICE_WORK;
    } else {
        bool memory = call->status == MO_SERVICE_MEMORY || defect->limit_name != NULL;
        signature.class = memory ? MO_SIGNATURE_MEMORY : MO_SIGNATURE_DEFECT;
        signature.reason = slice(defect->reason == NULL ? "native memory" : defect->reason);
        signature.limit = slice(defect->limit_name == NULL
                                    ? (call->status == MO_SERVICE_MEMORY ? "native allocation" : "")
                                    : defect->limit_name);
        signature.limit_value = defect->limit;
    }
    return signature;
}

struct mo_frame_walk mo_process_frames(const struct mo_process *process) {
    MO_ASSERT(process != NULL);
    return (struct mo_frame_walk){process, process->depth == 0 ? SIZE_MAX : process->top};
}

bool mo_process_frame_next(struct mo_frame_walk *walk, struct mo_defect_frame *out) {
    MO_ASSERT(walk != NULL && out != NULL);
    if (walk->offset == SIZE_MAX)
        return false;
    const struct mo_process *p = walk->process;
    MO_ASSERT(walk->offset < p->capacity);
    const struct mo_vm_frame *f = (const struct mo_vm_frame *)(p->stack + walk->offset);
    *out = (struct mo_defect_frame){
        .function = f->function, .pc = f->pc, .span = mo_vm_span(p->file, f->function, f->pc)};
    walk->offset = f->previous;
    return true;
}

enum mo_failure_context mo_service_context(struct mo_process *p, const struct mo_defect *defect,
                                           struct mo_buffer *out) {
    enum mo_failure_context status = MO_FAILURE_CONTEXT_COMPLETE;
    /* Walk live frames before normal cleanup, including cancelled calls. */
    struct mo_frame_walk walk = mo_process_frames(p);
    struct mo_defect_frame frame;
    unsigned frames = 0;
    bool truncated = false;
    while (mo_process_frame_next(&walk, &frame)) {
        if (frames == MO_LIMIT_FAILURE_FRAMES) {
            truncated = true;
            break;
        }
        bool ok =
            mo_buffer_append_text(out, "frame ") && mo_buffer_append_uint(out, frame.function) &&
            mo_buffer_append_text(out, " file ") && mo_buffer_append_uint(out, frame.span.file) &&
            mo_buffer_append_text(out, " span ") && mo_buffer_append_uint(out, frame.span.start) &&
            mo_buffer_append_text(out, "..") && mo_buffer_append_uint(out, frame.span.end) &&
            mo_buffer_append_byte(out, '\n');
        if (!ok) {
            status = MO_FAILURE_CONTEXT_UNAVAILABLE;
            break;
        }
        frames++;
    }
    if (defect->frames != NULL && defect->process != NULL) {
        struct captured_variables variables = {out, p->heap, defect->frames, false};
        enum mo_defect_variables_result visited =
            mo_visit_defect_variables(p, defect, MO_LIMIT_FAILURE_VARIABLES,
                                      MO_LIMIT_FAILURE_CONTEXT_WORK, capture_variable, &variables);
        truncated = truncated || visited != MO_DEFECT_VARIABLES_COMPLETE || variables.unavailable;
    } else if (!mo_buffer_append_text(out, "variables unavailable\n"))
        status = MO_FAILURE_CONTEXT_UNAVAILABLE;
    else
        truncated = true;
    /* Unavailable is never downgraded to truncated. */
    if (truncated && status == MO_FAILURE_CONTEXT_COMPLETE)
        status = MO_FAILURE_CONTEXT_TRUNCATED;
    return status;
}

static void capture_failure(struct mo_service_loop *s, const struct service_message *m,
                            const struct mo_service_call *call) {
    s->capture_id = 0;
    s->capture_unsafe = false;
    s->capture_refusal = "out of memory for the record";
    if (s->store == NULL || s->current_encoding.length == 0) {
        s->capture_refusal = "persistent state unavailable";
        return;
    }
    struct mo_buffer metadata, context, kind;
    mo_buffer_init(&metadata, MO_LIMIT_FAILURE_MESSAGE_BYTES);
    mo_buffer_init(&context, MO_LIMIT_FAILURE_CONTEXT_BYTES);
    /* An over-long kind still fits here, so its refusal names the kind, not memory. */
    mo_buffer_init(&kind, MO_LIMIT_FAILURE_MESSAGE_BYTES);
    struct mo_bytecode_string name = handler_name(s, call->function);
    uint32_t count = m->timer ? 0 : s->descriptor->tools[m->handler].argument_count;
    bool readonly = !m->timer && s->descriptor->tools[m->handler].read_only;
    struct mo_signature signature = mo_service_signature(call);
    uint8_t class = (uint8_t)signature.class;
    enum mo_failure_context context_status =
        mo_service_context(s->process, &call->defect, &context);
    bool ok = mo_binary_write(&metadata, MO_FAILURE_VERSION, 1) &&
              mo_binary_write(&metadata, 0, MO_FAILURE_ID_BYTES) &&
              mo_binary_write(&metadata, s->sequence, 8) &&
              mo_binary_write(&metadata, (uint64_t)m->arrival, 8) &&
              mo_binary_write(&metadata, m->timer, 1) &&
              mo_binary_write(&metadata, m->handler, 4) &&
              mo_failure_text(&metadata, name.bytes, name.length) &&
              mo_binary_write(&metadata, readonly, 1) && mo_binary_write(&metadata, count, 4);
    for (uint32_t i = 0; ok && i < count; i++) {
        size_t at = metadata.length;
        ok = mo_binary_write(&metadata, 0, 4);
        size_t start = metadata.length;
        if (ok) {
            enum mo_saved_status encoded = mo_value_encode_diagnostic(
                s->process->heap, s->descriptor->tools[m->handler].argument_types[i],
                m->arguments[i], &metadata);
            if (encoded != MO_SAVED_OK) {
                s->capture_refusal = encoded == MO_SAVED_MEMORY ? "out of memory for the record"
                                     : readonly                 ? "arguments could not be encoded"
                                                                : "too large to record";
                ok = false;
            }
        }
        if (ok)
            for (unsigned j = 0; j < 4; j++)
                metadata.bytes[at + j] = (uint8_t)((metadata.length - start) >> (8 * j));
    }
    size_t tail = 10 + 4 + signature.reason.length + 4 + signature.limit.length + 16 + 48 + 32 + 4 +
                  strlen(mo_runtime_build_label()) + 32 + 1 + 4 + context.length;
    if (ok && tail > metadata.limit - metadata.length) {
        s->capture_refusal = "too large to record";
        ok = false;
    }
    uint8_t build[32], schema[32];
    if (ok && (!mo_runtime_identity(build) ||
               !mo_export_schema(s->current_encoding.bytes, s->current_encoding.length, schema))) {
        s->capture_refusal = "runtime identity unavailable";
        ok = false;
    }
    ok = ok && mo_binary_write(&metadata, 1, 1) && mo_binary_write(&metadata, class, 1) &&
         mo_failure_text(&metadata, signature.reason.bytes, signature.reason.length) &&
         mo_failure_text(&metadata, signature.limit.bytes, signature.limit.length) &&
         mo_binary_write(&metadata, signature.limit_value, 8) &&
         mo_binary_write(&metadata, signature.function, 4) &&
         mo_binary_write(&metadata, signature.file, 4) &&
         mo_binary_write(&metadata, signature.start, 4) &&
         mo_binary_write(&metadata, signature.end, 4) &&
         mo_binary_write(&metadata, MO_LIMIT_SERVICE_WORK, 8) &&
         mo_binary_write(&metadata, s->process->limits.heap_size, 8) &&
         mo_binary_write(&metadata, s->process->limits.stack_size, 8) &&
         mo_binary_write(&metadata, s->process->limits.call_depth, 8) &&
         mo_binary_write(&metadata, MO_LIMIT_SAVED_BYTES, 8) &&
         mo_binary_write(&metadata, s->image_key, 8) && mo_buffer_append(&metadata, build, 32) &&
         capture_string(&metadata, mo_runtime_build_label()) &&
         mo_buffer_append(&metadata, schema, 32) &&
         mo_binary_write(&metadata, (uint64_t)context_status, 1) &&
         mo_failure_text(&metadata, context.bytes, context.length);
    ok = ok && mo_buffer_append(&kind, name.bytes, name.length) &&
         mo_buffer_append_byte(&kind, ':') && mo_buffer_append_uint(&kind, class) &&
         mo_buffer_append_byte(&kind, ':') &&
         mo_buffer_append(&kind, signature.reason.bytes, signature.reason.length) &&
         mo_buffer_append_byte(&kind, ':') &&
         mo_buffer_append(&kind, signature.limit.bytes, signature.limit.length);
    if (ok && class == 0)
        ok = mo_buffer_append_byte(&kind, ':') &&
             mo_buffer_append_uint(&kind, signature.function) &&
             mo_buffer_append_byte(&kind, ':') && mo_buffer_append_uint(&kind, signature.file) &&
             mo_buffer_append_byte(&kind, ':') && mo_buffer_append_uint(&kind, signature.start) &&
             mo_buffer_append_byte(&kind, ':') && mo_buffer_append_uint(&kind, signature.end);
    ok = ok && mo_buffer_append_byte(&kind, 0);
    if (ok && kind.length - 1 > MO_LIMIT_FAILURE_KIND_BYTES) {
        s->capture_refusal = "failure kind too long";
        ok = false;
    }
    if (ok && metadata.length > MO_LIMIT_FAILURE_RECORD_BYTES - MO_LIMIT_FAILURE_ENVELOPE_BYTES -
                                    s->current_encoding.length) {
        s->capture_refusal = "too large to record";
        ok = false;
    }
    if (ok && !capture_readable(&metadata)) {
        s->capture_refusal = "record failed validation";
        ok = false;
    }
    if (ok) {
        struct mo_store_record state = {MO_STORE_FAILURE_STATE, s->sequence, s->image_key,
                                        s->current_encoding.bytes, s->current_encoding.length};
        struct mo_store_record message = {MO_STORE_FAILURE_MESSAGE, s->sequence, s->image_key,
                                          metadata.bytes, metadata.length};
        enum mo_store_capture_status result =
            mo_store_capture(s->store, (const char *)kind.bytes, &state, &message, &s->capture_id);
        s->capture_refusal = result == MO_STORE_CAPTURE_OK        ? NULL
                             : result == MO_STORE_CAPTURE_DAMAGED ? "diagnostic records damaged"
                             : result == MO_STORE_CAPTURE_UNSAFE  ? "store outcome uncertain"
                                                                  : "the store refused the record";
        s->capture_unsafe = result == MO_STORE_CAPTURE_UNSAFE;
    }
    mo_buffer_free(&metadata);
    mo_buffer_free(&context);
    mo_buffer_free(&kind);
}

static void supervise(struct mo_service_loop *s, enum mo_service_status reason,
                      struct mo_service_time time) {
    s->failure_count =
        record(s->failures, s->failure_count, time.elapsed, MO_LIMIT_SERVICE_FAILURE_WINDOW_US);
    if (s->failure_count != MO_LIMIT_SERVICE_FAILURES)
        return;
    s->failure_count = 0;
    s->restart_count =
        record(s->restarts, s->restart_count, time.elapsed, MO_LIMIT_SERVICE_RESTART_WINDOW_US);
    log_event(s, MO_SERVICE_RESTART, reason, time, NULL, MO_NO_INDEX, s->restart_count);
    s->running = s->restart_count != MO_LIMIT_SERVICE_RESTARTS;
    if (!s->running)
        log_event(s, MO_SERVICE_STOP, reason, time, NULL, MO_NO_INDEX, 0);
    drain(s, s->running);
    if (s->running)
        start_timers(s, time);
}

bool mo_service_step(struct mo_service_loop *s) {
    MO_ASSERT(s != NULL);
    if (!s->running)
        return false;
    poll_timers(s, sample_clock(s));
    if (s->count == 0)
        return false;
    struct service_message message = s->mailbox[s->head];
    s->head = (s->head + 1) % MO_LIMIT_SERVICE_MAILBOX;
    s->count--;
    if (message.timer)
        s->timers[message.handler].waiting = false;
    struct mo_buffer encoded;
    mo_buffer_init(&encoded, MO_LIMIT_SAVED_BYTES);
    struct mo_service_call call;
    mo_service_call_handler(s->process, s->descriptor, message.timer, message.handler, s->state,
                            message.arrival, message.arguments, s->store == NULL ? NULL : &encoded,
                            &call);
    struct mo_service_response reply = {.id = message.id,
                                        .handler = message.handler,
                                        .timer = message.timer,
                                        .updated = call.updated,
                                        .status = call.status,
                                        .type = call.type,
                                        .value = call.value};
    bool returned = call.returned;
    s->capture_id = 0;
    s->capture_refusal = NULL;
    s->capture_unsafe = false;
    uint32_t handler = call.function;
    struct mo_service_time time = sample_clock(s);
    if (returned) {
        if (reply.status != MO_SERVICE_OK) {
            capture_failure(s, &message, &call);
            log_event(s, MO_SERVICE_FAILURE, reply.status, time, NULL, handler, 0);
        } else if (reply.updated && !persist(s, &message, &encoded)) {
            mo_value_drop(s->process->heap, s->descriptor->state_type, call.state);
            reply.status = MO_SERVICE_STORE;
            reply.updated = false;
            s->running = false;
            log_event(s, MO_SERVICE_STOP, MO_SERVICE_STORE, time, NULL, MO_NO_INDEX, 0);
        }
        if (reply.updated && s->store != NULL) {
            replace_encoding(s, &encoded, s->sequence);
        }
        if (reply.updated) {
            mo_value_drop(s->process->heap, s->descriptor->state_type, s->state);
            s->state = call.state;
        }
    } else {
        capture_failure(s, &message, &call);
        reply.defect = call.defect.reason == NULL ? NULL : &call.defect;
        log_event(s, MO_SERVICE_FAILURE, reply.status, time, reply.defect, handler, 0);
    }
    mo_buffer_free(&encoded);
    if (s->capture_unsafe) {
        s->running = false;
        log_event(s, MO_SERVICE_STOP, MO_SERVICE_STORE, time, NULL, MO_NO_INDEX, 0);
    }
    struct mo_cell result = reply.value;
    if (returned && reply.status != MO_SERVICE_OK)
        reply.value = (struct mo_cell){0};
    s->hooks.reply(&reply, s->hooks.context);
    if (returned)
        mo_value_drop(s->process->heap, reply.type, result);
    finish_call(s->process);
    drop_message(s, &message);
    if (!returned || reply.status == MO_SERVICE_MEMORY) {
        if (s->capture_unsafe)
            s->failure_count = record(s->failures, s->failure_count, time.elapsed,
                                      MO_LIMIT_SERVICE_FAILURE_WINDOW_US);
        else
            supervise(s, reply.status, time);
    }
    if (!s->running)
        drain(s, false);
    if (s->running)
        poll_timers(s, time);
    return true;
}

struct mo_service_observation mo_service_observe(const struct mo_service_loop *s) {
    MO_ASSERT(s != NULL);
    struct mo_service_observation observation = {.sequence = s->sequence,
                                                 .snapshot_sequence = s->snapshot_sequence,
                                                 .encoded_sequence = s->encoded_sequence,
                                                 .running = s->running,
                                                 .persistent = s->store != NULL,
                                                 .pending = s->count,
                                                 .failures = s->failure_count,
                                                 .restarts = s->restart_count,
                                                 .timers = s->descriptor->timer_count,
                                                 .events = s->event_count,
                                                 .heap_used = mo_heap_used(s->process->heap),
                                                 .heap_peak = mo_heap_peak(s->process->heap),
                                                 .stack_capacity = s->process->capacity,
                                                 .stack_limit = s->process->limits.stack_size,
                                                 .encoded_bytes = s->current_encoding.length,
                                                 .encoded_capacity = s->current_encoding.capacity};
    observation.observed_elapsed = s->last_clock;
    observation.retained_available =
        s->store != NULL && mo_store_failure_stats(s->store, &observation.retained_failures,
                                                   &observation.evicted_kinds) == MO_STORE_OK;
    memcpy(observation.failure_times, s->failures, sizeof s->failures);
    memcpy(observation.restart_times, s->restarts, sizeof s->restarts);
    return observation;
}

bool mo_service_encoded_state(const struct mo_service_loop *s, const uint8_t **bytes,
                              size_t *length) {
    MO_ASSERT(s != NULL && bytes != NULL && length != NULL);
    *bytes = s->current_encoding.bytes;
    *length = s->current_encoding.length;
    return s->store != NULL;
}

bool mo_service_mailbox_entry(const struct mo_service_loop *s, uint32_t index,
                              struct mo_service_message_summary *out) {
    MO_ASSERT(s != NULL && out != NULL);
    if (index >= s->count)
        return false;
    const struct service_message *m = &s->mailbox[(s->head + index) % MO_LIMIT_SERVICE_MAILBOX];
    *out = (struct mo_service_message_summary){.arrival = m->arrival,
                                               .id = m->id,
                                               .handler = m->handler,
                                               .timer = m->timer,
                                               .argument_bytes = m->argument_bytes,
                                               .argument_bytes_available =
                                                   m->timer || m->argument_bytes_available};
    return true;
}

bool mo_service_timer_entry(const struct mo_service_loop *s, uint32_t index,
                            struct mo_service_timer_summary *out) {
    MO_ASSERT(s != NULL && out != NULL);
    if (index >= s->descriptor->timer_count)
        return false;
    const struct service_timer *t = &s->timers[index];
    *out = (struct mo_service_timer_summary){
        .next = t->next, .interval = t->interval, .waiting = t->waiting};
    return true;
}

bool mo_service_event_entry(const struct mo_service_loop *s, uint32_t index,
                            struct mo_service_event_summary *out) {
    MO_ASSERT(s != NULL && out != NULL);
    if (index >= s->event_count)
        return false;
    *out = s->events[(s->event_head + index) % MO_LIMIT_SERVICE_EVENTS];
    return true;
}

bool mo_service_log_entry(const struct mo_service_loop *s, uint32_t index,
                          struct mo_service_message_summary *out) {
    MO_ASSERT(s != NULL && out != NULL);
    if (index >= s->log_count)
        return false;
    *out = s->log[index];
    return true;
}

enum mo_store_status mo_service_failures(const struct mo_service_loop *s,
                                         bool (*consume)(const struct mo_store_failure *, void *),
                                         void *context) {
    return s->store == NULL ? MO_STORE_IO : mo_store_failures(s->store, consume, context);
}

enum mo_store_status mo_service_failure_catalog(const struct mo_service_loop *s, uint64_t after,
                                                bool (*consume)(const struct mo_store_failure *,
                                                                void *),
                                                void *context, uint32_t *omitted) {
    return s->store == NULL ? MO_STORE_IO
                            : mo_store_failure_catalog(s->store, after, consume, context, omitted);
}

bool mo_service_failure_export(const struct mo_service_loop *s, uint64_t id,
                               struct mo_buffer *out) {
    return s->store != NULL && mo_store_failure_export(s->store, id, out);
}

struct failure_pin {
    uint64_t failure, now;
    struct mo_export_pin *pin;
    struct mo_export_identity identity;
    bool found;
};
static bool pin_failure(const struct mo_store_failure *record, void *context) {
    struct failure_pin *p = context;
    if (record->id != p->failure)
        return true;
    struct mo_failure_record metadata;
    if (!mo_failure_read(record->message.bytes, record->message.length, &metadata) ||
        metadata.id != record->id)
        return false;
    char label[MO_LIMIT_EXPORT_LABEL_BYTES + 1];
    memcpy(label, metadata.label.bytes, metadata.label.length);
    label[metadata.label.length] = 0;
    p->identity.label = label;
    memcpy(p->identity.build, metadata.build, 32);
    p->identity.program = metadata.program ^ ((uint64_t)p->identity.service << 32);
    p->identity.sequence = record->state.sequence;
    p->identity.encoded_sequence = record->state.sequence;
    /* One failure pin at a time: release the old one only once its replacement is found. */
    mo_export_release(p->pin);
    p->found =
        mo_export_create_failure(p->pin, &p->identity, record->state.bytes, record->state.length,
                                 record->message.bytes, record->message.length, p->now);
    return p->found;
}
bool mo_service_pin_failure(const struct mo_service_loop *s, uint64_t failure,
                            struct mo_export_pin *pin, const struct mo_export_identity *identity,
                            uint64_t now) {
    struct failure_pin context = {failure, now, pin, *identity, false};
    return s->store != NULL &&
           mo_store_failure_get(s->store, failure, pin_failure, &context) == MO_STORE_OK &&
           context.found;
}
