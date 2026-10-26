#include "vm/internal.h"

#include <string.h>

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
    uint32_t head, count, failure_count, restart_count;
    bool running;
};

static struct mo_service_time sample_clock(struct mo_service_loop *s) {
    struct mo_service_time time = s->hooks.clock(s->hooks.context);
    MO_ASSERT(time.elapsed >= s->last_clock);
    s->last_clock = time.elapsed;
    return time;
}

static void log_event(struct mo_service_loop *s, enum mo_service_event_kind kind,
                      enum mo_service_status reason, struct mo_service_time time,
                      const struct mo_defect *defect) {
    struct mo_service_event event = {kind, reason, time, defect};
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
    /* Controlled calls poll once at entry, then once before each instruction. */
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
        return strncmp(defect->reason, "running out of memory", 21) == 0 ||
                       (defect->limit_name != NULL &&
                        strcmp(defect->limit_name, "--max-stack") == 0)
                   ? MO_SERVICE_MEMORY
                   : MO_SERVICE_DEFECT;
    }
    MO_UNREACHABLE();
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
    *s = (struct mo_service_loop){.descriptor = &file->services[index], .hooks = *hooks};
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
    for (uint32_t i = 0; i < s->descriptor->timer_count; i++) {
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
        status = invoke(s, s->descriptor->start, NULL, 0, &s->state, &defect);
    struct mo_service_time time = sample_clock(s);
    if (status != MO_SERVICE_OK) {
        log_event(s, MO_SERVICE_FAILURE, status, time, defect.reason == NULL ? NULL : &defect);
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
    struct service_message *message = append(s);
    *message = (struct service_message){.arrival = time.now, .handler = tool, .id = id};
    for (uint32_t i = 0; i < count; i++)
        message->arguments[i] =
            mo_value_dup(s->process->heap, handler->argument_types[i], arguments[i]);
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

static void supervise(struct mo_service_loop *s, enum mo_service_status reason,
                      struct mo_service_time time) {
    s->failure_count =
        record(s->failures, s->failure_count, time.elapsed, MO_LIMIT_SERVICE_FAILURE_WINDOW_US);
    if (s->failure_count != MO_LIMIT_SERVICE_FAILURES)
        return;
    s->failure_count = 0;
    s->restart_count =
        record(s->restarts, s->restart_count, time.elapsed, MO_LIMIT_SERVICE_RESTART_WINDOW_US);
    log_event(s, MO_SERVICE_RESTART, reason, time, NULL);
    s->running = s->restart_count != MO_LIMIT_SERVICE_RESTARTS;
    if (!s->running)
        log_event(s, MO_SERVICE_STOP, reason, time, NULL);
    uint32_t old_count = s->count;
    s->count = 0;
    for (uint32_t i = 0; i < old_count; i++) {
        struct service_message message = s->mailbox[(s->head + i) % MO_LIMIT_SERVICE_MAILBOX];
        if (message.timer)
            continue;
        if (s->running)
            *append(s) = message;
        else {
            struct mo_service_response reply = {
                .id = message.id, .handler = message.handler, .status = MO_SERVICE_DOWN};
            s->hooks.reply(&reply, s->hooks.context);
            drop_message(s, &message);
        }
    }
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
    const struct mo_bytecode_service_tool *tool =
        message.timer ? NULL : &s->descriptor->tools[message.handler];
    uint32_t function =
        message.timer ? s->descriptor->timers[message.handler].function : tool->function;
    if (message.timer)
        s->timers[message.handler].waiting = false;
    const struct mo_bytecode_function *fn = &s->process->file->functions[function];
    struct mo_cell arguments[MO_LIMIT_BYTECODE_PARAMETERS];
    arguments[0] = s->state;
    arguments[1] = mo_cell_time(message.arrival);
    for (uint32_t i = 2; i < fn->parameter_count; i++)
        arguments[i] = message.arguments[i - 2];
    for (uint32_t i = 0; i < fn->parameter_count; i++)
        if (fn->parameters[i].ownership == MO_PARAMETER_OWNED)
            arguments[i] = mo_value_dup(s->process->heap, fn->parameters[i].type, arguments[i]);
    struct mo_defect defect = {0};
    struct mo_service_response reply = {.id = message.id,
                                        .handler = message.handler,
                                        .timer = message.timer,
                                        .type = fn->result_type};
    reply.status = invoke(s, function, arguments, fn->parameter_count, &reply.value, &defect);
    struct mo_service_time time = sample_clock(s);
    if (reply.status == MO_SERVICE_OK) {
        struct mo_cell state = reply.value;
        reply.updated = message.timer || !tool->read_only;
        if (!message.timer && reply.updated) {
            const struct mo_type *type = &s->process->types->items[reply.type];
            if (type->kind == MO_TYPE_CHOICE) {
                reply.updated = mo_cell_get_tag(reply.value) == 0;
                state = mo_cell_get_payload(reply.value);
            }
            if (reply.updated)
                state = mo_value_field(state, 0);
        }
        if (reply.updated) {
            state = mo_value_dup(s->process->heap, s->descriptor->state_type, state);
            mo_value_drop(s->process->heap, s->descriptor->state_type, s->state);
            s->state = state;
        }
    } else {
        reply.defect = defect.reason == NULL ? NULL : &defect;
        log_event(s, MO_SERVICE_FAILURE, reply.status, time, reply.defect);
    }
    s->hooks.reply(&reply, s->hooks.context);
    if (reply.status == MO_SERVICE_OK)
        mo_value_drop(s->process->heap, reply.type, reply.value);
    finish_call(s->process);
    drop_message(s, &message);
    if (reply.status != MO_SERVICE_OK)
        supervise(s, reply.status, time);
    if (s->running)
        poll_timers(s, time);
    return true;
}
