#include "vm/service-internal.h"

#include <string.h>

static struct mo_bytecode_string function_name(const struct mo_bytecode *file, uint32_t function) {
    MO_ASSERT(function < file->function_count);
    return mo_service_short_name(file->strings[file->functions[function].name]);
}

static bool same_name(struct mo_bytecode_string a, struct mo_bytecode_string b) {
    return a.length == b.length && memcmp(a.bytes, b.bytes, a.length) == 0;
}

static uint32_t timer_named(const struct mo_bytecode *file, const struct mo_bytecode_service *d,
                            struct mo_bytecode_string name) {
    for (uint32_t i = 0; i < d->timer_count; i++)
        if (same_name(function_name(file, d->timers[i].function), name))
            return i;
    return MO_NO_INDEX;
}

static uint32_t tool_named(const struct mo_bytecode *file, const struct mo_bytecode_service *d,
                           struct mo_bytecode_string name) {
    for (uint32_t i = 0; i < d->tool_count; i++)
        if (same_name(file->strings[d->tools[i].name], name))
            return i;
    return MO_NO_INDEX;
}

/* A queued call keeps its place when the new program has a tool of the same name whose
 * argument types accept the saved encoding of each argument; otherwise it is answered later. */
static enum mo_service_swap_status readmit(struct mo_service_loop *old,
                                           struct mo_service_loop *next,
                                           const struct service_message *m) {
    const struct mo_bytecode *from = old->process->file, *to = next->process->file;
    if (m->timer) {
        uint32_t timer =
            timer_named(to, next->descriptor,
                        function_name(from, old->descriptor->timers[m->handler].function));
        if (timer == MO_NO_INDEX) {
            next->dropped++;
            return MO_SERVICE_SWAP_READY;
        }
        *mo_service_append(next) = (struct service_message){.arrival = m->arrival,
                                                            .handler = timer,
                                                            .timer = true,
                                                            .argument_bytes_available = true};
        next->timers[timer].waiting = true;
        return MO_SERVICE_SWAP_READY;
    }
    const struct mo_bytecode_service_tool *was = &old->descriptor->tools[m->handler];
    uint32_t tool = tool_named(to, next->descriptor, from->strings[was->name]);
    const struct mo_bytecode_service_tool *now =
        tool == MO_NO_INDEX ? NULL : &next->descriptor->tools[tool];
    struct service_message moved = *m;
    moved.handler = tool;
    bool kept = now != NULL && now->argument_count == was->argument_count;
    uint32_t decoded = 0;
    struct mo_buffer encoded;
    mo_buffer_init(&encoded, MO_LIMIT_SAVED_BYTES);
    enum mo_service_swap_status status = MO_SERVICE_SWAP_READY;
    while (kept && decoded < was->argument_count) {
        mo_buffer_clear(&encoded);
        if (!mo_value_encode(old->process->heap, was->argument_types[decoded],
                             m->arguments[decoded], &encoded)) {
            status = MO_SERVICE_SWAP_MEMORY;
            break;
        }
        kept = mo_value_decode(next->process->heap, now->argument_types[decoded], encoded.bytes,
                               encoded.length, &moved.arguments[decoded]);
        decoded += kept;
    }
    mo_buffer_free(&encoded);
    if (kept && status == MO_SERVICE_SWAP_READY) {
        *mo_service_append(next) = moved;
        return status;
    }
    for (uint32_t i = 0; i < decoded; i++)
        mo_value_drop(next->process->heap, now->argument_types[i], moved.arguments[i]);
    if (status == MO_SERVICE_SWAP_READY)
        next->removed[next->removed_count++] = m->id;
    return status;
}

enum mo_service_swap_status mo_service_swap_prepare(struct mo_service_loop *old,
                                                    const struct mo_image *image, uint32_t index,
                                                    const uint8_t *state, size_t length,
                                                    struct mo_service_loop **out) {
    MO_ASSERT(old != NULL && image != NULL && out != NULL && (state != NULL || length == 0));
    *out = NULL;
    if (!old->running)
        return MO_SERVICE_SWAP_STOPPED;
    const struct mo_bytecode *file = mo_image_bytecode(image);
    MO_ASSERT(index < file->service_count);
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_SERVICE_METADATA);
    struct mo_service_loop *s =
        mo_arena_allocate(&arena, sizeof *s, _Alignof(struct mo_service_loop));
    if (s == NULL) {
        mo_arena_free(&arena);
        return MO_SERVICE_SWAP_MEMORY;
    }
    *s = (struct mo_service_loop){.descriptor = &file->services[index],
                                  .hooks = old->hooks,
                                  .store = old->store,
                                  .image_key = mo_image_key(image, index),
                                  .restore_key = mo_image_key(image, index),
                                  .sequence = old->sequence,
                                  .snapshot_sequence = old->sequence,
                                  .encoded_sequence = old->sequence,
                                  .last_clock = old->last_clock,
                                  .event_number = old->event_number,
                                  .event_head = old->event_head,
                                  .event_count = old->event_count,
                                  .failure_count = old->failure_count,
                                  .restart_count = old->restart_count,
                                  .epoch = old->epoch + 1};
    memcpy(s->events, old->events, sizeof s->events);
    memcpy(s->failures, old->failures, sizeof s->failures);
    memcpy(s->restarts, old->restarts, sizeof s->restarts);
    mo_buffer_init(&s->current_encoding, MO_LIMIT_SAVED_BYTES);
    s->timers = mo_arena_allocate_array(&arena, s->descriptor->timer_count, sizeof *s->timers,
                                        _Alignof(struct service_timer));
    s->arena = arena;
    struct mo_limits limits = mo_service_limits();
    if (s->timers == NULL || (s->process = mo_process_new(image, &limits)) == NULL) {
        mo_service_free(s);
        return MO_SERVICE_SWAP_MEMORY;
    }
    memset(s->timers, 0, s->descriptor->timer_count * sizeof *s->timers);
    enum mo_service_swap_status status = MO_SERVICE_SWAP_READY;
    for (uint32_t i = 0; status == MO_SERVICE_SWAP_READY && i < s->descriptor->timer_count; i++) {
        struct mo_cell value;
        struct mo_defect defect;
        enum mo_service_status called = mo_service_invoke(
            s->process, s->descriptor->timers[i].interval, NULL, 0, &value, &defect);
        int64_t interval = called == MO_SERVICE_OK ? mo_cell_get_duration(value) : 0;
        if (called != MO_SERVICE_OK)
            mo_service_finish_call(s->process);
        if (interval <= 0)
            status = MO_SERVICE_SWAP_INTERVAL;
        else {
            s->timers[i].interval = (uint64_t)interval;
            /* A kept timer keeps its next due time; a new one is due now, as at a start. */
            uint32_t was = timer_named(old->process->file, old->descriptor,
                                       function_name(file, s->descriptor->timers[i].function));
            s->timers[i].next = was == MO_NO_INDEX ? old->last_clock : old->timers[was].next;
        }
    }
    if (status == MO_SERVICE_SWAP_READY &&
        !mo_value_decode(s->process->heap, s->descriptor->state_type, state, length, &s->state))
        status = MO_SERVICE_SWAP_STATE;
    else if (status == MO_SERVICE_SWAP_READY)
        s->running = true;
    if (status == MO_SERVICE_SWAP_READY && s->store != NULL &&
        !mo_buffer_append(&s->current_encoding, state, length))
        status = MO_SERVICE_SWAP_MEMORY;
    for (uint32_t i = 0; status == MO_SERVICE_SWAP_READY && i < old->count; i++)
        status = readmit(old, s, &old->mailbox[(old->head + i) % MO_LIMIT_SERVICE_MAILBOX]);
    if (status != MO_SERVICE_SWAP_READY) {
        mo_service_free(s);
        return status;
    }
    *out = s;
    return MO_SERVICE_SWAP_READY;
}

void mo_service_swap_finish(struct mo_service_loop *old, struct mo_service_loop *next,
                            const struct mo_service_upgrade *upgrade) {
    MO_ASSERT(old != NULL && next != NULL && upgrade != NULL && next->epoch == old->epoch + 1);
    struct mo_service_upgrade note = *upgrade;
    note.accepted = true;
    note.code = 0;
    note.answered = next->removed_count;
    note.dropped = next->dropped;
    for (uint32_t i = 0; i < next->removed_count; i++) {
        struct mo_service_response reply = {.id = next->removed[i], .status = MO_SERVICE_REMOVED};
        next->hooks.reply(&reply, next->hooks.context);
    }
    next->removed_count = 0;
    next->dropped = 0;
    mo_service_record_event(next, MO_SERVICE_UPGRADE, MO_SERVICE_OK, mo_service_clock(next), NULL,
                            MO_NO_INDEX, 0, &note);
    mo_service_free(old);
}

void mo_service_swap_abandon(struct mo_service_loop *next) {
    mo_service_free(next);
}

void mo_service_note_upgrade(struct mo_service_loop *s, const struct mo_service_upgrade *upgrade) {
    MO_ASSERT(s != NULL && upgrade != NULL && !upgrade->accepted && upgrade->code != 0);
    mo_service_record_event(s, MO_SERVICE_UPGRADE, MO_SERVICE_OK, mo_service_clock(s), NULL,
                            MO_NO_INDEX, 0, upgrade);
}
