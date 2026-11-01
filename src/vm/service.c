#include "vm/internal.h"

#include <string.h>

#include "base/binary.h"

struct service_message {
    struct mo_cell arguments[MO_LIMIT_BYTECODE_PARAMETERS - 2];
    int64_t arrival;
    uint64_t id;
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
    uint64_t sequence, image_key;
    uint32_t head, count, failure_count, restart_count;
    bool running;
};

enum {
    LOG_TIMER_WIDTH = 1,
    LOG_HANDLER_WIDTH = 4,
    LOG_ARRIVAL_WIDTH = 8,
    LOG_COUNT_WIDTH = 4,
    LOG_CHECKSUM_WIDTH = 8,
    LOG_ARGUMENT_LENGTH_WIDTH = 4,
    MESSAGE_HEADER_BYTES = LOG_TIMER_WIDTH + LOG_HANDLER_WIDTH + LOG_ARRIVAL_WIDTH +
                           LOG_COUNT_WIDTH + LOG_CHECKSUM_WIDTH
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

static struct mo_service_time sample_clock(struct mo_service_loop *s) {
    struct mo_service_time time = s->hooks.clock(s->hooks.context);
    MO_ASSERT(time.elapsed >= s->last_clock);
    s->last_clock = time.elapsed;
    return time;
}

/* The source names tools and timers bare; a bytecode function name adds its
 * module and, when overloaded, `@Receiver`. An interval thunk reports as its timer. */
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
    struct mo_bytecode_string name = file->strings[file->functions[function].name];
    uint32_t start = 0, end = 0;
    while (end != name.length && name.bytes[end] != '@')
        if (name.bytes[end++] == '.')
            start = end;
    return (struct mo_bytecode_string){end - start, name.bytes + start};
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

static enum mo_service_status invoke(struct mo_service_loop *s, uint32_t function,
                                     const struct mo_cell *arguments, uint32_t count,
                                     struct mo_cell *out, struct mo_defect *defect) {
    uint64_t remaining = (uint64_t)MO_LIMIT_SERVICE_WORK + 1;
    enum mo_controlled_outcome outcome = mo_call_controlled(
        s->process, function, arguments, count, work_remaining, &remaining, out, defect);
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

static uint32_t message_function(const struct mo_service_loop *s, const struct service_message *m) {
    return m->timer ? s->descriptor->timers[m->handler].function
                    : s->descriptor->tools[m->handler].function;
}

static void call_message(struct mo_service_loop *s, const struct service_message *m,
                         struct mo_cell state, struct mo_service_response *reply,
                         struct mo_defect *defect) {
    uint32_t function = message_function(s, m);
    const struct mo_bytecode_function *fn = &s->process->file->functions[function];
    struct mo_cell arguments[MO_LIMIT_BYTECODE_PARAMETERS];
    arguments[0] = state;
    arguments[1] = mo_cell_time(m->arrival);
    for (uint32_t i = 2; i < fn->parameter_count; i++)
        arguments[i] = m->arguments[i - 2];
    for (uint32_t i = 0; i < fn->parameter_count; i++)
        if (fn->parameters[i].ownership == MO_PARAMETER_OWNED)
            arguments[i] = mo_value_dup(s->process->heap, fn->parameters[i].type, arguments[i]);
    *reply = (struct mo_service_response){
        .id = m->id, .handler = m->handler, .timer = m->timer, .type = fn->result_type};
    reply->status = invoke(s, function, arguments, fn->parameter_count, &reply->value, defect);
}

static bool replacement(struct mo_service_loop *s, const struct mo_service_response *reply,
                        struct mo_cell *out) {
    if (reply->status != MO_SERVICE_OK ||
        (!reply->timer && s->descriptor->tools[reply->handler].read_only))
        return false;
    struct mo_cell state = reply->value;
    if (!reply->timer) {
        if (s->process->types->items[reply->type].kind == MO_TYPE_CHOICE) {
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
static bool saved_copy(struct mo_service_loop *s, uint32_t type, struct mo_cell value,
                       struct mo_cell *out, struct mo_buffer *encoded) {
    if (s->store == NULL) {
        *out = mo_value_dup(s->process->heap, type, value);
        return true;
    }
    mo_buffer_clear(encoded);
    return mo_value_encode(s->process->heap, type, value, encoded) &&
           mo_value_decode(s->process->heap, type, encoded->bytes, encoded->length, out);
}

static bool snapshot(struct mo_service_loop *s, const uint8_t *bytes, size_t length) {
    struct mo_store_record record = {MO_STORE_STATE, s->sequence, s->image_key, bytes, length};
    return mo_store_call(s->store, MO_STORE_SNAPSHOT, &record, NULL, NULL) == MO_STORE_OK;
}

static bool snapshot_state(struct mo_service_loop *s, struct mo_cell state) {
    struct mo_buffer bytes;
    mo_buffer_init(&bytes, MO_LIMIT_SAVED_BYTES);
    bool ok = mo_value_encode(s->process->heap, s->descriptor->state_type, state, &bytes) &&
              snapshot(s, bytes.bytes, bytes.length);
    mo_buffer_free(&bytes);
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
        r->found = true;
        r->sequence = r->snapshot = record->sequence;
        return true;
    }
    struct mo_binary in = {record->bytes, record->length, 0};
    uint64_t timer, handler, arrival, count, expected;
    if (!r->found || record->sequence != r->sequence + 1 ||
        !mo_binary_uint(&in, LOG_TIMER_WIDTH, &timer) || timer > 1 ||
        !mo_binary_uint(&in, LOG_HANDLER_WIDTH, &handler) ||
        !mo_binary_uint(&in, LOG_ARRIVAL_WIDTH, &arrival) ||
        !mo_binary_uint(&in, LOG_COUNT_WIDTH, &count) ||
        !mo_binary_uint(&in, LOG_CHECKSUM_WIDTH, &expected))
        return false;
    struct service_message m = {.handler = (uint32_t)handler, .timer = timer != 0};
    memcpy(&m.arrival, &arrival, LOG_ARRIVAL_WIDTH);
    if (m.arrival < MO_TIME_MIN_US || m.arrival > MO_TIME_MAX_US ||
        handler >= (m.timer ? s->descriptor->timer_count : s->descriptor->tool_count))
        return false;
    const struct mo_bytecode_service_tool *tool = m.timer ? NULL : &s->descriptor->tools[handler];
    if (count != (m.timer ? 0 : tool->argument_count) || (!m.timer && tool->read_only))
        return false;
    uint32_t built = 0;
    bool ok = true;
    while (ok && built < count) {
        uint64_t length;
        const uint8_t *bytes;
        ok = mo_binary_uint(&in, LOG_ARGUMENT_LENGTH_WIDTH, &length) &&
             mo_binary_read(&in, (size_t)length, &bytes) &&
             mo_value_decode(heap, tool->argument_types[built], bytes, (size_t)length,
                             &m.arguments[built]);
        if (ok)
            built++;
    }
    ok = ok && in.position == in.length;
    if (ok) {
        struct mo_service_response reply;
        struct mo_defect defect = {0};
        call_message(s, &m, r->state, &reply, &defect);
        if (reply.status == MO_SERVICE_MEMORY || reply.status == MO_SERVICE_WORK)
            r->limit = reply.status;
        struct mo_cell state;
        struct mo_buffer encoded;
        mo_buffer_init(&encoded, MO_LIMIT_SAVED_BYTES);
        ok = replacement(s, &reply, &state) &&
             saved_copy(s, s->descriptor->state_type, state, &state, &encoded);
        if (ok && mo_binary_checksum(encoded.bytes, encoded.length) != expected) {
            mo_value_drop(heap, s->descriptor->state_type, state);
            ok = false;
        }
        mo_buffer_free(&encoded);
        if (ok) {
            mo_value_drop(heap, s->descriptor->state_type, r->state);
            r->state = state;
            r->sequence = record->sequence;
        } else
            r->failed_function = message_function(s, &m);
        if (reply.status == MO_SERVICE_OK)
            mo_value_drop(heap, reply.type, reply.value);
        finish_call(s->process);
    }
    for (uint32_t i = 0; i < built; i++)
        mo_value_drop(heap, tool->argument_types[i], m.arguments[i]);
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
    bool ok = mo_binary_write(&bytes, m->timer ? 1 : 0, LOG_TIMER_WIDTH) &&
              mo_binary_write(&bytes, m->handler, LOG_HANDLER_WIDTH) &&
              mo_binary_write(&bytes, (uint64_t)m->arrival, LOG_ARRIVAL_WIDTH) &&
              mo_binary_write(&bytes, count, LOG_COUNT_WIDTH) &&
              mo_binary_write(&bytes, mo_binary_checksum(state->bytes, state->length),
                              LOG_CHECKSUM_WIDTH);
    for (uint32_t i = 0; ok && i < count; i++) {
        mo_buffer_clear(&value);
        ok = mo_value_encode(s->process->heap, s->descriptor->tools[m->handler].argument_types[i],
                             m->arguments[i], &value) &&
             mo_binary_write(&bytes, value.length, LOG_ARGUMENT_LENGTH_WIDTH) &&
             mo_buffer_append(&bytes, value.bytes, value.length);
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
                                  .image_key = file->header.checksum ^ ((uint64_t)index << 32)};
    s->timers = mo_arena_allocate_array(&arena, s->descriptor->timer_count, sizeof *s->timers,
                                        _Alignof(struct service_timer));
    s->arena = arena;
    if (s->timers == NULL) {
        mo_service_free(s);
        return MO_SERVICE_MEMORY;
    }
    memset(s->timers, 0, s->descriptor->timer_count * sizeof *s->timers);
    struct mo_limits limits = mo_limits_default();
    limits.heap_size = MO_LIMIT_SERVICE_MESSAGE_MEMORY - limits.stack_size;
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
        status = invoke(s, s->descriptor->timers[i].interval, NULL, 0, &value, &defect);
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
        status = invoke(s, s->descriptor->start, NULL, 0, &s->state, &defect);
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
                              : snapshot_state(s, s->state)))
        status = MO_SERVICE_STORE;
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
    size_t payload = MESSAGE_HEADER_BYTES;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t type = handler->argument_types[i];
        if (handler->read_only)
            message.arguments[i] = mo_value_dup(s->process->heap, type, arguments[i]);
        else if (!saved_copy(s, type, arguments[i], &message.arguments[i], &encoded) ||
                 (payload += LOG_ARGUMENT_LENGTH_WIDTH + encoded.length) > MO_LIMIT_SAVED_BYTES) {
            uint32_t built = payload > MO_LIMIT_SAVED_BYTES ? i + 1 : i;
            for (uint32_t j = 0; j < built; j++)
                mo_value_drop(s->process->heap, handler->argument_types[j], message.arguments[j]);
            mo_buffer_free(&encoded);
            return MO_SERVICE_MEMORY;
        }
    }
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
    struct mo_defect defect = {0};
    struct mo_service_response reply;
    call_message(s, &message, s->state, &reply, &defect);
    bool returned = reply.status == MO_SERVICE_OK;
    uint32_t handler = message_function(s, &message);
    struct mo_service_time time = sample_clock(s);
    if (reply.status == MO_SERVICE_OK) {
        struct mo_cell state;
        struct mo_buffer encoded;
        mo_buffer_init(&encoded, MO_LIMIT_SAVED_BYTES);
        reply.updated = replacement(s, &reply, &state);
        if (reply.updated && !saved_copy(s, s->descriptor->state_type, state, &state, &encoded)) {
            reply.status = MO_SERVICE_MEMORY;
            reply.updated = false;
            log_event(s, MO_SERVICE_FAILURE, reply.status, time, NULL, handler, 0);
        } else if (reply.updated && !persist(s, &message, &encoded)) {
            mo_value_drop(s->process->heap, s->descriptor->state_type, state);
            reply.status = MO_SERVICE_STORE;
            reply.updated = false;
            s->running = false;
            log_event(s, MO_SERVICE_STOP, MO_SERVICE_STORE, time, NULL, MO_NO_INDEX, 0);
        }
        mo_buffer_free(&encoded);
        if (reply.updated) {
            mo_value_drop(s->process->heap, s->descriptor->state_type, s->state);
            s->state = state;
        }
    } else {
        reply.defect = defect.reason == NULL ? NULL : &defect;
        log_event(s, MO_SERVICE_FAILURE, reply.status, time, reply.defect, handler, 0);
    }
    struct mo_cell result = reply.value;
    if (returned && reply.status != MO_SERVICE_OK)
        reply.value = (struct mo_cell){0};
    s->hooks.reply(&reply, s->hooks.context);
    if (returned)
        mo_value_drop(s->process->heap, reply.type, result);
    finish_call(s->process);
    drop_message(s, &message);
    if (!returned || reply.status == MO_SERVICE_MEMORY)
        supervise(s, reply.status, time);
    if (!s->running)
        drain(s, false);
    if (s->running)
        poll_timers(s, time);
    return true;
}
