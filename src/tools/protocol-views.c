#include "tools/protocol-server.h"

#include "base/runtime-identity.h"

#include <stdio.h>
#include <string.h>

struct view_builder {
    struct mo_protocol_server *server;
    struct mo_heap *heap;
    struct protocol_connection *connection;
    const struct mo_types *types;
};

static uint32_t field_type(const struct view_builder *b, uint32_t type, uint32_t field) {
    return b->types->items[type].cases[0].fields[field].type;
}

/* Builders own their fields; the protocol heap is separate from the service. */
static bool record(struct view_builder *b, uint32_t type, struct mo_cell *fields, uint32_t count,
                   struct mo_cell *out) {
    bool ok = mo_value_case(b->heap, type, 0, fields, count, out);
    for (uint32_t i = 0; i < count; i++)
        mo_value_drop(b->heap, field_type(b, type, i), fields[i]);
    return ok;
}

static bool text(struct view_builder *b, const char *value, struct mo_cell *out) {
    return mo_protocol_charge(b->connection, strlen(value)) &&
           mo_value_text(b->heap, value, strlen(value), out);
}

static bool number(struct view_builder *b, mo_u128 value, struct mo_cell *out) {
    char digits[40];
    size_t at = sizeof digits;
    do {
        digits[--at] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    return mo_value_text(b->heap, digits + at, sizeof digits - at, out);
}

const struct mo_image *mo_protocol_epoch_image(const struct mo_protocol_server *s, uint64_t epoch) {
    if (epoch == s->epoch)
        return s->image;
    return s->previous.image != NULL && epoch == s->previous.epoch ? s->previous.image : NULL;
}

static bool name_in(struct view_builder *b, const struct mo_bytecode *file, uint32_t index,
                    struct mo_cell *out) {
    if (index == MO_NO_INDEX)
        return text(b, "", out);
    const struct mo_bytecode_string *value = &file->strings[index];
    if (value->length > MO_LIMIT_MCP_VIEW_NAME_BYTES)
        return text(b, "<name unavailable: exceeds inspection byte limit>", out);
    return mo_protocol_charge(b->connection, value->length) &&
           mo_value_text(b->heap, (const char *)value->bytes, value->length, out);
}

static bool name(struct view_builder *b, uint32_t index, struct mo_cell *out) {
    return name_in(b, mo_image_bytecode(b->server->image), index, out);
}

static bool upgrade_reason(struct view_builder *b, const struct mo_service_upgrade *u,
                           struct mo_cell *out) {
    char from[65], to[65], reason[256];
    mo_protocol_hex(u->old_source, 32, from);
    mo_protocol_hex(u->new_source, 32, to);
    int n = u->accepted ? snprintf(reason, sizeof reason,
                                   "accepted ordinal %llu from %s to %s; answered %u; dropped %u",
                                   (unsigned long long)u->ordinal, from, to, (unsigned)u->answered,
                                   (unsigned)u->dropped)
                        : snprintf(reason, sizeof reason, "refused E%04u from %s to %s",
                                   (unsigned)u->code, from, to);
    MO_ASSERT(n > 0 && (size_t)n < sizeof reason);
    return text(b, reason, out);
}

static uint32_t handler(const struct view_builder *b, bool timer, uint32_t index) {
    return timer ? mo_image_bytecode(b->server->image)
                       ->functions[b->server->descriptor->timers[index].function]
                       .name
                 : b->server->descriptor->tools[index].name;
}

static bool messages(struct view_builder *b, uint32_t type, bool log, struct mo_cell *out) {
    if (!mo_value_list(b->heap, type, NULL, 0, out))
        return false;
    uint32_t element = b->types->items[type].parameters[0];
    uint32_t limit = log ? MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES : MO_LIMIT_SERVICE_MAILBOX;
    for (uint32_t i = 0; i < limit; i++) {
        struct mo_service_message_summary item;
        bool found = log ? mo_service_log_entry(b->server->service, i, &item)
                         : mo_service_mailbox_entry(b->server->service, i, &item);
        if (!found)
            break;
        struct mo_cell fields[6], value;
        if (!name(b, handler(b, item.timer, item.handler), &fields[0]))
            goto fail;
        fields[1] = mo_cell_bool(item.timer);
        fields[2] = mo_cell_int(item.arrival);
        if (!number(b, item.id, &fields[3])) {
            mo_value_drop(b->heap, field_type(b, element, 0), fields[0]);
            goto fail;
        }
        fields[4] = mo_cell_int((int64_t)item.argument_bytes);
        fields[5] = mo_cell_bool(item.argument_bytes_available);
        if (!record(b, element, fields, 6, &value))
            goto fail;
        bool ok = mo_list_append(b->heap, type, out, value);
        mo_value_drop(b->heap, element, value);
        if (!ok)
            goto fail;
    }
    return true;
fail:
    mo_value_drop(b->heap, type, *out);
    return false;
}

/* One page of at most MO_LIMIT_MCP_VIEW_TIMERS timers, from the offset the transport parsed. */
static bool timers(struct view_builder *b, uint32_t type, int64_t offset, struct mo_cell *out) {
    if (!mo_value_list(b->heap, type, NULL, 0, out))
        return false;
    uint32_t element = b->types->items[type].parameters[0];
    uint32_t count = b->server->descriptor->timer_count;
    MO_ASSERT(offset >= 0);
    uint32_t first = (uint64_t)offset < count ? (uint32_t)offset : count;
    uint32_t last =
        count - first < MO_LIMIT_MCP_VIEW_TIMERS ? count : first + MO_LIMIT_MCP_VIEW_TIMERS;
    for (uint32_t i = first; i < last; i++) {
        struct mo_service_timer_summary item;
        MO_ASSERT(mo_service_timer_entry(b->server->service, i, &item));
        struct mo_cell fields[4], value;
        uint32_t built = 0;
        if (name(b, handler(b, true, i), &fields[built]))
            built++;
        if (built == 1 && number(b, item.interval, &fields[built]))
            built++;
        if (built == 2 && number(b, item.next, &fields[built]))
            built++;
        if (built != 3) {
            for (uint32_t j = 0; j < built; j++)
                mo_value_drop(b->heap, field_type(b, element, j), fields[j]);
            goto fail;
        }
        fields[3] = mo_cell_bool(item.waiting);
        if (!record(b, element, fields, 4, &value))
            goto fail;
        bool ok = mo_list_append(b->heap, type, out, value);
        mo_value_drop(b->heap, element, value);
        if (!ok)
            goto fail;
    }
    return true;
fail:
    mo_value_drop(b->heap, type, *out);
    return false;
}

static bool events(struct view_builder *b, uint32_t type, struct mo_cell *out) {
    if (!mo_value_list(b->heap, type, NULL, 0, out))
        return false;
    uint32_t element = b->types->items[type].parameters[0];
    for (uint32_t i = 0; i < MO_LIMIT_SERVICE_EVENTS; i++) {
        struct mo_service_event_summary item;
        if (!mo_service_event_entry(b->server->service, i, &item))
            break;
        const char *kind = "";
        switch (item.kind) {
        case MO_SERVICE_FAILURE:
            kind = "failure";
            break;
        case MO_SERVICE_RESTART:
            kind = "restart";
            break;
        case MO_SERVICE_STOP:
            kind = "stop";
            break;
        case MO_SERVICE_UPGRADE:
            kind = "upgrade";
            break;
        }
        struct mo_cell fields[8], value;
        uint32_t built = 0;
        if (number(b, item.number, &fields[built]))
            built++;
        if (built == 1 && text(b, kind, &fields[built]))
            built++;
        if (built == 2 && (item.kind == MO_SERVICE_UPGRADE
                               ? upgrade_reason(b, &item.upgrade, &fields[built])
                               : text(b, mo_service_status_text(item.reason), &fields[built])))
            built++;
        /* Handler indexes belong to the program the event ran under, which may be gone. */
        const struct mo_image *image = mo_protocol_epoch_image(b->server, item.epoch);
        const struct mo_bytecode *file = image == NULL ? NULL : mo_image_bytecode(image);
        bool known =
            item.function == MO_NO_INDEX || (file != NULL && item.function < file->function_count);
        if (built == 3 &&
            (known ? name_in(b, file == NULL ? mo_image_bytecode(b->server->image) : file,
                             item.function == MO_NO_INDEX ? MO_NO_INDEX
                                                          : file->functions[item.function].name,
                             &fields[built])
                   : text(b, "<program not retained>", &fields[built])))
            built++;
        if (built == 4)
            fields[built++] = mo_cell_int(item.time.now);
        if (built == 5 && number(b, item.time.elapsed, &fields[built]))
            built++;
        if (built == 6 && number(b, item.record_id, &fields[built]))
            built++;
        if (built == 7 &&
            text(b, item.record_unavailable == NULL ? "" : item.record_unavailable, &fields[built]))
            built++;
        if (built != 8) {
            for (uint32_t j = 0; j < built; j++)
                mo_value_drop(b->heap, field_type(b, element, j), fields[j]);
            goto fail;
        }
        if (!record(b, element, fields, 8, &value))
            goto fail;
        bool ok = mo_list_append(b->heap, type, out, value);
        mo_value_drop(b->heap, element, value);
        if (!ok)
            goto fail;
    }
    return true;
fail:
    mo_value_drop(b->heap, type, *out);
    return false;
}

bool mo_protocol_view(struct mo_protocol_server *s, struct protocol_connection *c,
                      struct mo_cell *out) {
    struct view_builder b = {s, mo_process_heap(c->process), c, mo_image_types(s->protocol)};
    const struct mo_bytecode_function *fn = mo_protocol_function(s, PF_VIEW);
    uint32_t type = fn->parameters[2].type;
    struct mo_service_observation observed = mo_service_observe(s->service);
    struct mo_cell fields[10], observation;
    uint32_t built = 0;
    if (name(&b, mo_image_bytecode(s->image)->modules[s->descriptor->module].name, &fields[0]))
        built++;
    uint64_t program =
        mo_image_key(s->image, s->service_index) ^ ((uint64_t)s->service_index << 32);
    if (built == 1 && number(&b, program, &fields[1]))
        built++;
    uint8_t digest[32];
    char hex[65] = {0};
    if (mo_runtime_identity(digest)) {
        const char digits[] = "0123456789abcdef";
        for (size_t i = 0; i < sizeof digest; i++) {
            hex[2 * i] = digits[digest[i] >> 4];
            hex[2 * i + 1] = digits[digest[i] & 15];
        }
    }
    if (built == 2 && text(&b, hex, &fields[2]))
        built++;
    if (built == 3 && text(&b, mo_runtime_build_label(), &fields[3]))
        built++;
    struct mo_cell stats[] = {
        mo_cell_int((int64_t)observed.sequence),
        mo_cell_int((int64_t)observed.snapshot_sequence),
        mo_cell_int((int64_t)observed.encoded_sequence),
        mo_cell_unit(),
        mo_cell_int((int64_t)observed.heap_used),
        mo_cell_int((int64_t)observed.heap_peak),
        mo_cell_int((int64_t)mo_heap_limit(s->service_heap)),
        mo_cell_int((int64_t)observed.stack_capacity),
        mo_cell_int((int64_t)observed.encoded_bytes),
        mo_cell_int((int64_t)s->view_scratch_peak),
        mo_cell_int(observed.failures),
        mo_cell_int(observed.restarts),
        mo_cell_bool(observed.running),
        mo_cell_bool(observed.persistent),
        mo_cell_int((int64_t)MO_LIMIT_SERVICE_FAILURE_WINDOW_US),
        mo_cell_int((int64_t)MO_LIMIT_SERVICE_RESTART_WINDOW_US),
        mo_cell_int(observed.timers),
        mo_cell_int((int64_t)observed.stack_limit),
        mo_cell_int((int64_t)(s->state_export.arena.used + s->failure_export.arena.used)),
        mo_cell_int(observed.retained_available ? (int64_t)observed.retained_failures : -1),
        mo_cell_unit()};
    if (built == 4 && number(&b, observed.observed_elapsed, &stats[3])) {
        if (!number(&b, observed.evicted_kinds, &stats[20]))
            mo_value_drop(b.heap, field_type(&b, field_type(&b, type, 4), 3), stats[3]);
        else if (record(&b, field_type(&b, type, 4), stats, 21, &fields[4]))
            built++;
    }
    if (built == 5 && messages(&b, field_type(&b, type, 5), false, &fields[5]))
        built++;
    struct mo_cell action = mo_cell_get_payload(c->action);
    if (built == 6 &&
        timers(&b, field_type(&b, type, 6), mo_cell_get_int(mo_value_field(action, 3)), &fields[6]))
        built++;
    if (built == 7 && events(&b, field_type(&b, type, 7), &fields[7]))
        built++;
    if (built == 8 && messages(&b, field_type(&b, type, 8), true, &fields[8]))
        built++;
    MO_ASSERT(s->now.monotonic_us >= s->started_at);
    if (built == 9 && number(&b, s->now.monotonic_us - s->started_at, &fields[9]))
        built++;
    if (built != 10) {
        for (uint32_t i = 0; i < built; i++)
            mo_value_drop(b.heap, field_type(&b, type, i), fields[i]);
        return false;
    }
    if (!record(&b, type, fields, 10, &observation))
        return false;
    const uint8_t *bytes = NULL;
    size_t length = 0;
    if (mo_cell_get_int(mo_value_field(action, 1)) == 1 &&
        !mo_service_encoded_state(s->service, &bytes, &length)) {
        bytes = NULL;
        length = 0;
    }
    size_t old_limit = mo_heap_limit(b.heap);
    mo_heap_set_limit(b.heap, old_limit + MO_LIMIT_MCP_VIEW_MEMORY - MO_LIMIT_MCP_REQUEST_MEMORY);
    struct mo_cell saved;
    bool ok = mo_protocol_charge(c, length) && mo_value_bytes(b.heap, bytes, length, &saved);
    if (ok) {
        struct mo_cell args[] = {mo_value_field(action, 1), mo_value_field(action, 2), observation,
                                 saved, mo_cell_int(MO_LIMIT_MCP_VIEW_BYTES)};
        ok = mo_protocol_invoke(s, c, PF_VIEW, args, out);
        mo_value_drop(b.heap, fn->parameters[3].type, saved);
    }
    size_t scratch_peak = mo_heap_scratch_peak(b.heap);
    if (scratch_peak > s->view_scratch_peak)
        s->view_scratch_peak = scratch_peak;
    mo_value_drop(b.heap, type, observation);
    mo_heap_set_limit(b.heap, old_limit);
    mo_protocol_observe(s, c);
    return ok;
}
