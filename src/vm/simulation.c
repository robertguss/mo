#include "vm/internal.h"

#include "base/simops.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct mo_simulation {
    struct mo_arena arena;
    const struct mo_image *image;
    struct mo_store *store;
    struct mo_service_loop *loop;
    struct mo_service_time time;
    uint32_t descriptor;
    /* Set for the length of one operation. */
    const struct mo_simulation_hooks *hooks;
    struct mo_heap *heap;
    struct mo_simulation_reply *reply;
    uint64_t restarts; /* the supervisor's, a restart that stops included */
    bool replied, refused, delivered, stopped;
};

static void begin(struct mo_simulation *s, const struct mo_simulation_hooks *hooks) {
    MO_ASSERT(hooks != NULL && hooks->failure != NULL);
    s->hooks = hooks;
    s->stopped = false;
}

static void fail(struct mo_simulation *s, uint32_t function, int64_t now,
                 enum mo_service_status status, const struct mo_defect *defect) {
    MO_ASSERT(s->hooks != NULL);
    if (s->stopped)
        return;
    const struct mo_simulation_failure failure = {status, now, function, defect};
    s->delivered = true;
    s->stopped = !s->hooks->failure(&failure, s->hooks->context);
}

static struct mo_service_time clock_now(void *context) {
    return ((struct mo_simulation *)context)->time;
}

static void event(const struct mo_service_event *e, void *context) {
    struct mo_simulation *s = context;
    s->restarts += e->kind == MO_SERVICE_RESTART;
    if (e->function != MO_NO_INDEX || e->reason == MO_SERVICE_STORE ||
        e->reason == MO_SERVICE_SAVED_INVALID)
        fail(s, e->function, e->time.now, e->reason, e->defect);
}

static void reply(const struct mo_service_response *r, void *context) {
    struct mo_simulation *s = context;
    const struct mo_bytecode *file = mo_image_bytecode(s->image);
    const struct mo_bytecode_service *service = &file->services[s->descriptor];
    if (r->timer) {
        /* A failed commit was already seen when the store stopped the service. */
        if (r->status != MO_SERVICE_OK && r->status != MO_SERVICE_STORE)
            fail(s, service->timers[r->handler].function, s->time.now, r->status, r->defect);
        return;
    }
    MO_ASSERT(!s->replied && s->reply != NULL);
    s->replied = true;
    struct mo_simulation_reply *out = s->reply;
    out->status = r->status;
    out->updated = r->updated;
    out->type = r->type;
    if (r->status != MO_SERVICE_OK)
        return;
    size_t encoded_length;
    if (!mo_value_copy_saved(mo_service_heap(s->loop), s->heap, r->type, r->value, &out->reply,
                             &encoded_length, &out->refusal)) {
        out->reply = (struct mo_cell){0};
        s->refused = true;
        s->stopped = true;
    }
}

enum mo_store_status mo_simulation_new(const struct mo_image *image, uint32_t index, int64_t now,
                                       struct mo_simulation **out) {
    MO_ASSERT(image != NULL && out != NULL && index < mo_image_bytecode(image)->service_count);
    MO_ASSERT(now >= MO_TIME_MIN_US && now <= MO_TIME_MAX_US);
    struct mo_arena arena;
    mo_arena_init(&arena, sizeof(struct mo_simulation) + MO_ARENA_CHUNK_HEADER);
    struct mo_simulation *s = mo_arena_allocate(&arena, sizeof *s, _Alignof(struct mo_simulation));
    if (s == NULL) {
        mo_arena_free(&arena);
        return MO_STORE_MEMORY;
    }
    *s = (struct mo_simulation){
        .arena = arena, .image = image, .descriptor = index, .time = {.now = now}};
    enum mo_store_status status = mo_store_open(NULL, &s->store);
    if (status != MO_STORE_OK) {
        mo_arena_free(&arena);
        return status;
    }
    *out = s;
    return MO_STORE_OK;
}

void mo_simulation_free(struct mo_simulation *s) {
    if (s == NULL)
        return;
    mo_service_free(s->loop);
    mo_store_close(s->store);
    struct mo_arena arena = s->arena;
    mo_arena_free(&arena);
}

static enum mo_simulation_outcome drain(struct mo_simulation *s) {
    while (!s->stopped && mo_service_step(s->loop)) {
    }
    MO_ASSERT(s->stopped || mo_service_pending(s->loop) == 0);
    return s->stopped ? MO_SIMULATION_STOPPED : MO_SIMULATION_OK;
}

enum mo_simulation_outcome mo_simulation_open(struct mo_simulation *s,
                                              const struct mo_simulation_hooks *hooks) {
    MO_ASSERT(s != NULL);
    if (mo_simulation_running(s))
        return MO_SIMULATION_RUNNING;
    begin(s, hooks);
    mo_service_free(s->loop);
    s->loop = NULL;
    s->delivered = false;
    struct mo_service_hooks service = {clock_now, reply, event, s};
    enum mo_service_status status =
        mo_service_open(s->image, s->descriptor, &service, s->store, &s->loop);
    if (status != MO_SERVICE_OK) {
        s->loop = NULL;
        if (!s->delivered)
            fail(s, MO_NO_INDEX, s->time.now, status, NULL);
        return MO_SIMULATION_DOWN;
    }
    return MO_SIMULATION_OK;
}

enum mo_simulation_outcome mo_simulation_drain(struct mo_simulation *s,
                                               const struct mo_simulation_hooks *hooks) {
    MO_ASSERT(s != NULL);
    begin(s, hooks);
    return s->loop == NULL ? MO_SIMULATION_OK : drain(s);
}

enum mo_simulation_sent mo_simulation_send(struct mo_simulation *s,
                                           const struct mo_simulation_hooks *hooks,
                                           struct mo_heap *heap, uint32_t tool,
                                           const struct mo_cell *arguments,
                                           struct mo_simulation_reply *out) {
    MO_ASSERT(s != NULL && heap != NULL && out != NULL);
    const struct mo_bytecode_service *service =
        &mo_image_bytecode(s->image)->services[s->descriptor];
    MO_ASSERT(tool < service->tool_count);
    const struct mo_bytecode_service_tool *handler = &service->tools[tool];
    MO_ASSERT(handler->argument_count == 0 || arguments != NULL);
    *out = (struct mo_simulation_reply){.status = MO_SERVICE_DOWN};
    if (!mo_simulation_running(s))
        return MO_SENT_DOWN;
    begin(s, hooks);
    if (drain(s) != MO_SIMULATION_OK)
        return MO_SENT_STOPPED;
    struct mo_heap *service_heap = mo_service_heap(s->loop);
    struct mo_cell copied[MO_LIMIT_BYTECODE_PARAMETERS];
    uint32_t count = 0;
    uint64_t message_bytes = 25;
    enum mo_simulation_sent sent = MO_SENT_REPLIED;
    for (; count < handler->argument_count; count++) {
        size_t encoded_length;
        if (!mo_value_copy_saved(heap, service_heap, handler->argument_types[count],
                                 arguments[count], &copied[count], &encoded_length,
                                 &out->refusal)) {
            out->argument = count;
            sent = MO_SENT_ARGUMENT;
            break;
        }
        message_bytes += 4 + encoded_length;
    }
    if (sent == MO_SENT_REPLIED && !handler->read_only && message_bytes > MO_LIMIT_SAVED_BYTES) {
        out->bytes = message_bytes;
        sent = MO_SENT_TOO_LARGE;
    }
    if (sent == MO_SENT_REPLIED) {
        out->status = mo_service_send(s->loop, tool, copied, count, 0);
        if (out->status == MO_SERVICE_MEMORY)
            sent = MO_SENT_REFUSED;
    }
    for (uint32_t i = 0; i < count; i++)
        mo_value_drop(service_heap, handler->argument_types[i], copied[i]);
    if (sent != MO_SENT_REPLIED)
        return sent;
    if (out->status == MO_SERVICE_OK) {
        s->heap = heap;
        s->reply = out;
        s->replied = false;
        s->refused = false;
        enum mo_simulation_outcome drained = drain(s);
        s->heap = NULL;
        s->reply = NULL;
        if (s->refused)
            return MO_SENT_REPLY_COPY;
        if (drained != MO_SIMULATION_OK) {
            if (s->replied && out->status == MO_SERVICE_OK)
                mo_value_drop(heap, out->type, out->reply);
            return MO_SENT_STOPPED;
        }
        MO_ASSERT(s->replied);
    }
    switch (out->status) {
    case MO_SERVICE_OK:
        return out->updated || handler->read_only ? MO_SENT_REPLIED : MO_SENT_REJECTED;
    case MO_SERVICE_DOWN:
        return MO_SENT_DOWN;
    case MO_SERVICE_DEFECT:
    case MO_SERVICE_WORK:
    case MO_SERVICE_MEMORY:
        return MO_SENT_TOOL_FAILED;
    case MO_SERVICE_BUSY:
    case MO_SERVICE_INTERVAL:
    case MO_SERVICE_STORE:
    case MO_SERVICE_SAVED_INVALID:
    case MO_SERVICE_REMOVED:
        return MO_SENT_FAILED;
    }
    MO_UNREACHABLE();
}

bool mo_simulation_firings(const struct mo_simulation *s, int64_t by, uint64_t *out) {
    MO_ASSERT(s != NULL && out != NULL);
    if (by < 0 || (mo_i128)s->time.now + by > MO_TIME_MAX_US)
        return false;
    uint64_t next;
    *out = mo_simulation_running(s)
               ? mo_service_deadlines(s->loop, s->time.elapsed + (uint64_t)by, &next)
               : 0;
    return true;
}

enum mo_simulation_outcome mo_simulation_advance(struct mo_simulation *s,
                                                 const struct mo_simulation_hooks *hooks,
                                                 int64_t by, uint64_t *firings) {
    MO_ASSERT(s != NULL && firings != NULL);
    if (by < 0 || (mo_i128)s->time.now + by > MO_TIME_MAX_US)
        return MO_SIMULATION_DURATION;
    begin(s, hooks);
    uint64_t until = s->time.elapsed + (uint64_t)by;
    int64_t target = s->time.now + by;
    *firings = 0;
    if (mo_simulation_running(s)) {
        if (drain(s) != MO_SIMULATION_OK)
            return MO_SIMULATION_STOPPED;
        bool counted = mo_simulation_firings(s, by, firings);
        MO_ASSERT(counted);
        if (*firings > MO_LIMIT_SIMULATION_TICKS)
            return MO_SIMULATION_TICKS;
        uint64_t next;
        while (mo_service_deadlines(s->loop, until, &next) != 0) {
            MO_ASSERT(next >= s->time.elapsed);
            s->time.now += (int64_t)(next - s->time.elapsed);
            s->time.elapsed = next;
            if (drain(s) != MO_SIMULATION_OK)
                return MO_SIMULATION_STOPPED;
        }
    }
    s->time = (struct mo_service_time){target, until};
    return MO_SIMULATION_OK;
}

void mo_simulation_crash(struct mo_simulation *s) {
    MO_ASSERT(s != NULL);
    mo_service_free(s->loop);
    s->loop = NULL;
}

bool mo_simulation_running(const struct mo_simulation *s) {
    MO_ASSERT(s != NULL);
    return s->loop != NULL && mo_service_running(s->loop);
}

uint64_t mo_simulation_restarts(const struct mo_simulation *s) {
    MO_ASSERT(s != NULL);
    return s->restarts;
}

bool mo_simulation_encode(struct mo_simulation *s, struct mo_buffer *out) {
    MO_ASSERT(s != NULL && out != NULL);
    if (s->loop == NULL)
        return false;
    uint32_t type = mo_image_bytecode(s->image)->services[s->descriptor].state_type;
    return mo_value_encode(mo_service_heap(s->loop), type, mo_service_state(s->loop), out);
}

/* The Simulation capability: a thin wrapper that turns each outcome into its bug report. */
struct simulation {
    struct mo_simulation *simulation;
    const struct mo_image *image;
    uint32_t descriptor;
};

struct call {
    const struct simulation *s;
    struct mo_process *caller;
    uint32_t operation;
    bool failed;
};

static const char simulation_operations;

static void release(intptr_t handle) {
    mo_simulation_free(((struct simulation *)handle)->simulation);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
static void
report(struct call *c, const char *format, ...) {
    if (c->failed)
        return;
    va_list args;
    va_start(args, format);
    int written =
        vsnprintf(c->caller->simulation_report, sizeof c->caller->simulation_report, format, args);
    va_end(args);
    MO_ASSERT(written >= 0);
    c->failed = true;
}

static bool failure(const struct mo_simulation_failure *f, void *context) {
    static const char *const operations[] = {
#define HOSTOP(id, module, member, signature, params, bugs, spec_only)
#define SIMOP(id, module, member, spec_only) member,
#include "base/hostops.def"
    };
    struct call *c = context;
    uint32_t function = f->function;
    const struct mo_defect *defect = f->defect;
    const struct mo_bytecode *file = mo_image_bytecode(c->s->image);
    const struct mo_bytecode_service *service = &file->services[c->s->descriptor];
    uint32_t tool = service->tools[0].function;
    uint32_t time_type = file->functions[tool].parameters[1].type;
    struct mo_buffer time;
    mo_buffer_init(&time, 32);
    bool printed = mo_value_print(c->caller->heap, time_type, mo_cell_time(f->now), &time);
    const char *text = printed ? (const char *)time.bytes : "<time unavailable>";
    int length = printed ? (int)time.length : (int)strlen(text);
    if (function != MO_NO_INDEX) {
        MO_ASSERT(function < file->function_count);
        uint32_t timer = 0;
        while (timer < service->timer_count && service->timers[timer].interval != function)
            timer++;
        bool start = function == service->start;
        bool interval = timer < service->timer_count;
        if (interval)
            function = service->timers[timer].function;
        const struct mo_bytecode_string *name = &file->strings[file->functions[function].name];
        const char *reason = defect == NULL ? NULL : defect->reason;
        size_t detail_length = 0;
        const uint8_t *detail = defect != NULL && defect->has_detail
                                    ? mo_value_data(defect->detail, &detail_length)
                                    : (const uint8_t *)"";
        if (detail_length > sizeof c->caller->simulation_report)
            detail_length = sizeof c->caller->simulation_report;
        if (start)
            report(c, "Simulation.start at %.*s: start value: %s%s%s%s%.*s", length, text,
                   mo_service_status_text(f->status), reason == NULL ? "" : ": ",
                   reason == NULL ? "" : reason, detail_length == 0 ? "" : ": ", (int)detail_length,
                   detail);
        else if (interval)
            report(c, "Simulation.%s at %.*s: interval of timer %.*s: %s%s%s%s%.*s",
                   operations[c->operation], length, text, (int)name->length, name->bytes,
                   mo_service_status_text(f->status), reason == NULL ? "" : ": ",
                   reason == NULL ? "" : reason, detail_length == 0 ? "" : ": ", (int)detail_length,
                   detail);
        else
            report(c, "Simulation handler %.*s at %.*s: %s%s%s%s%.*s", (int)name->length,
                   name->bytes, length, text, mo_service_status_text(f->status),
                   reason == NULL ? "" : ": ", reason == NULL ? "" : reason,
                   detail_length == 0 ? "" : ": ", (int)detail_length, detail);
    } else
        report(c, "Simulation.%s at %.*s: %s", operations[c->operation], length, text,
               mo_service_status_text(f->status));
    mo_buffer_free(&time);
    return false;
}

static void start(struct call *c, const struct mo_simulation_hooks *hooks) {
    enum mo_simulation_outcome outcome = mo_simulation_open(c->s->simulation, hooks);
    if (outcome == MO_SIMULATION_OK)
        outcome = mo_simulation_drain(c->s->simulation, hooks);
    MO_ASSERT(outcome == MO_SIMULATION_OK || c->failed);
}

static void send(struct call *c, const struct mo_simulation_hooks *hooks, uint32_t descriptor,
                 uint32_t tool, const struct mo_cell *arguments, uint32_t result_type,
                 struct mo_cell *out) {
    struct mo_process *p = c->caller;
    const struct mo_bytecode_string *tool_name =
        &p->file->strings[p->file->services[descriptor].tools[tool].name];
    if (descriptor != c->s->descriptor) {
        const struct mo_bytecode_service *selected = &p->file->services[descriptor];
        const struct mo_bytecode_string *module =
            &p->file->strings[p->file->modules[selected->module].name];
        const struct mo_bytecode_string *actual =
            &p->file->strings[p->file->modules[p->file->services[c->s->descriptor].module].name];
        report(c,
               "Simulation.send selected %.*s.%.*s, but this simulation runs %.*s; "
               "nothing was queued or committed",
               (int)module->length, module->bytes, (int)tool_name->length, tool_name->bytes,
               (int)actual->length, actual->bytes);
        return;
    }
    struct mo_simulation_reply r;
    enum mo_simulation_sent sent =
        mo_simulation_send(c->s->simulation, hooks, p->heap, tool, arguments, &r);
    switch (sent) {
    case MO_SENT_STOPPED:
        return;
    case MO_SENT_ARGUMENT: {
        const struct mo_bytecode_string *name =
            &p->file->strings[p->file->services[descriptor].tools[tool].argument_names[r.argument]];
        report(c, "Simulation argument %.*s copy-in: %s; nothing was queued or committed",
               (int)name->length, name->bytes, r.refusal);
        return;
    }
    case MO_SENT_TOO_LARGE:
        report(c,
               "Simulation admission: aggregate saved message is %" PRIu64
               " bytes, exceeding MO_LIMIT_SAVED_BYTES (%u); nothing was queued or "
               "committed; no handler ran",
               r.bytes, MO_LIMIT_SAVED_BYTES);
        return;
    case MO_SENT_REFUSED:
        report(c, "Simulation admission refused; nothing was queued or committed; "
                  "no handler ran");
        return;
    case MO_SENT_REPLY_COPY:
        report(c, "Simulation reply copy-out: %s; tool %.*s executed%s; no rollback or retry",
               r.refusal, (int)tool_name->length, tool_name->bytes,
               r.updated ? "; its update was committed" : "");
        return;
    case MO_SENT_FAILED:
        report(c, "Simulation.send: %s", mo_service_status_text(r.status));
        return;
    case MO_SENT_REPLIED:
    case MO_SENT_REJECTED: {
        uint32_t reply_type = p->types->items[result_type].cases[0].fields[0].type;
        bool ok = mo_value_case(p->heap, result_type, 0, &r.reply, 1, out);
        mo_value_drop(p->heap, reply_type, r.reply);
        if (!ok)
            report(c,
                   "Simulation reply copy-out: allocation; tool %.*s executed%s; "
                   "no rollback or retry",
                   (int)tool_name->length, tool_name->bytes,
                   r.updated ? "; its update was committed" : "");
        return;
    }
    case MO_SENT_DOWN:
    case MO_SENT_TOOL_FAILED: {
        struct mo_cell error;
        uint32_t error_type = p->types->items[result_type].cases[1].fields[0].type;
        bool made =
            mo_value_case(p->heap, error_type, sent == MO_SENT_DOWN ? 0 : 1, NULL, 0, &error);
        if (!made || !mo_value_case(p->heap, result_type, 1, &error, 1, out)) {
            if (sent == MO_SENT_DOWN)
                report(c, "Simulation.send: allocation failed while returning ServiceDown; "
                          "nothing was queued or committed");
            else
                report(c,
                       "Simulation.send: allocation failed while returning ToolFailed; "
                       "tool %.*s failed and nothing was committed",
                       (int)tool_name->length, tool_name->bytes);
        }
        if (made)
            mo_value_drop(p->heap, error_type, error);
        return;
    }
    }
    MO_UNREACHABLE();
}

static void advance(struct call *c, const struct mo_simulation_hooks *hooks, int64_t by) {
    uint64_t firings;
    switch (mo_simulation_advance(c->s->simulation, hooks, by, &firings)) {
    case MO_SIMULATION_OK:
    case MO_SIMULATION_STOPPED:
        return;
    case MO_SIMULATION_DURATION:
        report(c, "Simulation.advance needs a nonnegative duration ending no later than year 9999");
        return;
    case MO_SIMULATION_TICKS:
        report(c,
               "Simulation.advance: %" PRIu64 " firings exceed MO_LIMIT_SIMULATION_TICKS "
               "(%u); no handler ran, the clock did not move and nothing was committed",
               firings, MO_LIMIT_SIMULATION_TICKS);
        return;
    case MO_SIMULATION_DOWN:
    case MO_SIMULATION_RUNNING:
        break;
    }
    MO_UNREACHABLE();
}

enum mo_outcome mo_vm_simulate(struct mo_process *p, uint32_t operation, uint32_t descriptor,
                               uint32_t tool, uint32_t result_type, const struct mo_cell *arguments,
                               struct mo_cell *out) {
    MO_ASSERT(operation < MO_SIM_COUNT);
    struct call c = {.caller = p, .operation = operation};
    const struct mo_simulation_hooks hooks = {failure, &c};
    if (operation == MO_SIM_START) {
        struct simulation initial = {.image = p->image, .descriptor = descriptor};
        enum mo_store_status status = mo_simulation_new(
            p->image, descriptor, mo_cell_get_time(arguments[0]), &initial.simulation);
        if (status != MO_STORE_OK)
            return mo_vm_stop(p,
                              status == MO_STORE_HANDLES || status == MO_STORE_MEMORY
                                  ? "Simulation.start: MO_LIMIT_STORES (8) or store memory limit"
                                  : "Simulation.start: fake store creation failed",
                              NULL, false, mo_cell_unit(), MO_STOPPED);
        const struct mo_capability_info info = {.operations = &simulation_operations,
                                                .release = release};
        if (!mo_value_capability_native(p->heap, result_type, &info, &initial, sizeof initial,
                                        out)) {
            mo_simulation_free(initial.simulation);
            return mo_vm_stop(p, "Simulation.start: handle allocation failed", NULL, false,
                              mo_cell_unit(), MO_STOPPED);
        }
        c.s = (const struct simulation *)mo_capability_get(*out)->handle;
        start(&c, &hooks);
    } else {
        const struct mo_capability_info *info = mo_capability_get(arguments[0]);
        MO_ASSERT(info->operations == &simulation_operations);
        c.s = (const struct simulation *)info->handle;
        MO_ASSERT(c.s->image == p->image);
        *out = mo_cell_unit();
        switch ((enum mo_sim_operation)operation) {
        case MO_SIM_START:
        case MO_SIM_COUNT:
            MO_UNREACHABLE();
        case MO_SIM_SEND:
            send(&c, &hooks, descriptor, tool, arguments + 1, result_type, out);
            break;
        case MO_SIM_ADVANCE:
            advance(&c, &hooks, mo_cell_get_duration(arguments[1]));
            break;
        case MO_SIM_CRASH:
            mo_simulation_crash(c.s->simulation);
            break;
        case MO_SIM_RESTART:
            if (mo_simulation_running(c.s->simulation))
                report(&c, "Simulation.restart needs a crashed or stopped service");
            else
                start(&c, &hooks);
            break;
        }
    }
    return c.failed ? mo_vm_stop(p, p->simulation_report, NULL, false, mo_cell_unit(), MO_STOPPED)
                    : MO_RETURNED;
}
