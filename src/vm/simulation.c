#include "vm/internal.h"

#include "base/simops.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>

struct simulation {
    const struct mo_image *image;
    struct mo_store *store;
    struct mo_service_loop *loop;
    struct mo_service_time time;
    uint32_t descriptor;
    struct mo_process *caller;
    struct mo_cell reply;
    enum mo_service_status status;
    bool replied, updated, failed;
};

static const char simulation_operations;

static void release(intptr_t handle) {
    struct simulation *s = (struct simulation *)handle;
    mo_service_free(s->loop);
    mo_store_close(s->store);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
static void
report(struct simulation *s, const char *format, ...) {
    if (s->failed)
        return;
    va_list args;
    va_start(args, format);
    int written =
        vsnprintf(s->caller->simulation_report, sizeof s->caller->simulation_report, format, args);
    va_end(args);
    MO_ASSERT(written >= 0);
    s->failed = true;
}

static void handler_failure(struct simulation *s, uint32_t function,
                            enum mo_service_status status) {
    const struct mo_bytecode *file = mo_image_bytecode(s->image);
    MO_ASSERT(function < file->function_count);
    const struct mo_bytecode_string *name = &file->strings[file->functions[function].name];
    report(s, "Simulation handler %.*s at Time(%" PRId64 "): %s", (int)name->length, name->bytes,
           s->time.now, mo_service_status_text(status));
}

static struct mo_service_time clock_now(void *context) {
    return ((struct simulation *)context)->time;
}

static void event(const struct mo_service_event *e, void *context) {
    struct simulation *s = context;
    if (e->function != MO_NO_INDEX)
        handler_failure(s, e->function, e->reason);
    else if (e->reason == MO_SERVICE_STORE || e->reason == MO_SERVICE_SAVED_INVALID)
        report(s, "Simulation at Time(%" PRId64 "): %s", e->time.now,
               mo_service_status_text(e->reason));
}

static void reply(const struct mo_service_response *r, void *context) {
    struct simulation *s = context;
    const struct mo_bytecode *file = mo_image_bytecode(s->image);
    const struct mo_bytecode_service *service = &file->services[s->descriptor];
    if (r->timer) {
        if (r->status != MO_SERVICE_OK)
            handler_failure(s, service->timers[r->handler].function, r->status);
        return;
    }
    MO_ASSERT(!s->replied);
    s->replied = true;
    s->updated = r->updated;
    s->status = r->status;
    if (r->status != MO_SERVICE_OK)
        return;
    const char *failure;
    size_t encoded_length;
    if (!mo_value_copy_saved(mo_service_heap(s->loop), s->caller->heap, r->type, r->value,
                             &s->reply, &encoded_length, &failure)) {
        const struct mo_bytecode_string *name = &file->strings[service->tools[r->handler].name];
        report(s, "Simulation reply copy-out: %s; tool %.*s executed%s; no rollback or retry",
               failure, (int)name->length, name->bytes,
               r->updated ? "; its update was committed" : "");
    }
}

static void drain(struct simulation *s) {
    while (!s->failed && mo_service_step(s->loop)) {
    }
    MO_ASSERT(s->failed || mo_service_pending(s->loop) == 0);
}

static void open_loop(struct simulation *s) {
    struct mo_service_hooks hooks = {clock_now, reply, event, s};
    enum mo_service_status status =
        mo_service_open(s->image, s->descriptor, &hooks, s->store, &s->loop);
    if (status != MO_SERVICE_OK) {
        report(s, "Simulation restart/start at Time(%" PRId64 "): %s", s->time.now,
               mo_service_status_text(status));
        return;
    }
    drain(s);
}

static void send(struct simulation *s, uint32_t descriptor, uint32_t tool,
                 const struct mo_cell *arguments, uint32_t result_type, struct mo_cell *out) {
    if (descriptor != s->descriptor) {
        report(s, "Simulation.send selected a tool from a different service");
        return;
    }
    if (s->loop == NULL || !mo_service_running(s->loop))
        s->status = MO_SERVICE_DOWN;
    else {
        drain(s);
        if (s->failed)
            return;
        const struct mo_bytecode_service_tool *handler =
            &s->caller->file->services[descriptor].tools[tool];
        struct mo_heap *heap = mo_service_heap(s->loop);
        struct mo_cell copied[MO_LIMIT_BYTECODE_PARAMETERS];
        uint32_t count = 0;
        uint64_t message_bytes = 25;
        for (; count < handler->argument_count; count++) {
            const char *failure;
            size_t encoded_length;
            if (!mo_value_copy_saved(s->caller->heap, heap, handler->argument_types[count],
                                     arguments[count], &copied[count], &encoded_length, &failure)) {
                const struct mo_bytecode_string *name =
                    &s->caller->file->strings[handler->argument_names[count]];
                report(s, "Simulation argument %.*s copy-in: %s; nothing was queued or committed",
                       (int)name->length, name->bytes, failure);
                break;
            }
            message_bytes += 4 + encoded_length;
        }
        if (!s->failed && !handler->read_only && message_bytes > MO_LIMIT_SAVED_BYTES)
            report(s,
                   "Simulation admission: aggregate saved message is %" PRIu64
                   " bytes, exceeding MO_LIMIT_SAVED_BYTES (%u); nothing was queued or "
                   "committed; no handler ran",
                   message_bytes, MO_LIMIT_SAVED_BYTES);
        if (!s->failed) {
            s->status = mo_service_send(s->loop, tool, copied, count, 0);
            if (s->status == MO_SERVICE_MEMORY)
                report(s, "Simulation admission refused; nothing was queued or committed; "
                          "no handler ran");
        }
        for (uint32_t i = 0; i < count; i++)
            mo_value_drop(heap, handler->argument_types[i], copied[i]);
        if (s->failed)
            return;
        if (s->status == MO_SERVICE_OK) {
            drain(s);
            if (s->failed)
                return;
            MO_ASSERT(s->replied);
        }
    }
    struct mo_process *p = s->caller;
    if (s->status == MO_SERVICE_OK) {
        uint32_t reply_type = p->types->items[result_type].cases[0].fields[0].type;
        bool ok = mo_value_case(p->heap, result_type, 0, &s->reply, 1, out);
        mo_value_drop(p->heap, reply_type, s->reply);
        if (!ok) {
            const struct mo_bytecode_string *name =
                &p->file->strings[p->file->services[descriptor].tools[tool].name];
            report(s,
                   "Simulation reply copy-out: allocation; tool %.*s executed%s; "
                   "no rollback or retry",
                   (int)name->length, name->bytes, s->updated ? "; its update was committed" : "");
        }
    } else if (s->status == MO_SERVICE_DOWN || s->status == MO_SERVICE_DEFECT ||
               s->status == MO_SERVICE_WORK || s->status == MO_SERVICE_MEMORY) {
        struct mo_cell error;
        uint32_t error_type = p->types->items[result_type].cases[1].fields[0].type;
        bool made = mo_value_case(p->heap, error_type, s->status == MO_SERVICE_DOWN ? 0 : 1, NULL,
                                  0, &error);
        if (!made || !mo_value_case(p->heap, result_type, 1, &error, 1, out))
            report(s, "Simulation error reply copy-out: allocation");
        if (made)
            mo_value_drop(p->heap, error_type, error);
    } else
        report(s, "Simulation.send: %s", mo_service_status_text(s->status));
}

static void advance(struct simulation *s, int64_t by) {
    if (by < 0 || (mo_i128)s->time.now + by > MO_TIME_MAX_US) {
        report(s, "Simulation.advance needs a nonnegative duration ending no later than year 9999");
        return;
    }
    uint64_t until = s->time.elapsed + (uint64_t)by;
    int64_t target = s->time.now + by;
    if (s->loop != NULL && mo_service_running(s->loop)) {
        uint64_t next;
        uint64_t count = mo_service_deadlines(s->loop, until, &next);
        if (count > MO_LIMIT_SIMULATION_TICKS) {
            report(s,
                   "Simulation.advance: %" PRIu64 " firings exceed MO_LIMIT_SIMULATION_TICKS "
                   "(%u); no handlers ran, clock moved or updates committed",
                   count, MO_LIMIT_SIMULATION_TICKS);
            return;
        }
        while (mo_service_deadlines(s->loop, until, &next) != 0) {
            MO_ASSERT(next >= s->time.elapsed);
            s->time.now += (int64_t)(next - s->time.elapsed);
            s->time.elapsed = next;
            drain(s);
            if (s->failed)
                return;
        }
    }
    s->time = (struct mo_service_time){target, until};
}

enum mo_outcome mo_vm_simulate(struct mo_process *p, uint32_t operation, uint32_t descriptor,
                               uint32_t tool, uint32_t result_type, const struct mo_cell *arguments,
                               struct mo_cell *out) {
    MO_ASSERT(operation < MO_SIM_COUNT);
    struct simulation *s;
    if (operation == MO_SIM_START) {
        struct simulation initial = {.image = p->image,
                                     .descriptor = descriptor,
                                     .caller = p,
                                     .time = {.now = mo_cell_get_time(arguments[0])}};
        enum mo_store_status status = mo_store_open(NULL, &initial.store);
        if (status != MO_STORE_OK)
            return mo_vm_stop(p,
                              status == MO_STORE_LIMIT
                                  ? "Simulation.start: MO_LIMIT_STORES (8) or store memory limit"
                                  : "Simulation.start: fake store creation failed",
                              NULL, false, mo_cell_unit(), MO_STOPPED);
        const struct mo_capability_info info = {.operations = &simulation_operations,
                                                .release = release};
        if (!mo_value_capability_native(p->heap, result_type, &info, &initial, sizeof initial,
                                        out)) {
            mo_store_close(initial.store);
            return mo_vm_stop(p, "Simulation.start: handle allocation failed", NULL, false,
                              mo_cell_unit(), MO_STOPPED);
        }
        s = (struct simulation *)mo_capability_get(*out)->handle;
        open_loop(s);
    } else {
        const struct mo_capability_info *info = mo_capability_get(arguments[0]);
        MO_ASSERT(info->operations == &simulation_operations);
        s = (struct simulation *)info->handle;
        MO_ASSERT(s->image == p->image);
        s->caller = p;
        s->failed = false;
        s->replied = false;
        *out = mo_cell_unit();
        switch ((enum mo_sim_operation)operation) {
        case MO_SIM_START:
        case MO_SIM_COUNT:
            MO_UNREACHABLE();
        case MO_SIM_SEND:
            send(s, descriptor, tool, arguments + 1, result_type, out);
            break;
        case MO_SIM_ADVANCE:
            advance(s, mo_cell_get_duration(arguments[1]));
            break;
        case MO_SIM_CRASH:
            mo_service_free(s->loop);
            s->loop = NULL;
            break;
        case MO_SIM_RESTART:
            if (s->loop != NULL && mo_service_running(s->loop))
                report(s, "Simulation.restart needs a crashed or stopped service");
            else {
                mo_service_free(s->loop);
                s->loop = NULL;
                open_loop(s);
            }
            break;
        }
    }
    s->caller = NULL;
    return s->failed ? mo_vm_stop(p, p->simulation_report, NULL, false, mo_cell_unit(), MO_STOPPED)
                     : MO_RETURNED;
}
