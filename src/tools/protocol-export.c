#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "base/binary.h"
#include "base/failure.h"
#include "base/runtime-identity.h"
#include "tools/protocol-server.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

void mo_protocol_hex(const uint8_t *bytes, size_t length, char *out) {
    const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < length; i++) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 15];
    }
    out[length * 2] = 0;
}

static bool add_text(struct mo_protocol_server *s, struct mo_heap *heap, uint32_t list,
                     struct mo_cell *values, const char *text) {
    struct mo_cell value;
    if (!mo_value_text(heap, text, strlen(text), &value))
        return false;
    bool ok = mo_list_append(heap, list, values, value);
    mo_value_drop(heap, mo_image_types(s->protocol)->items[list].parameters[0], value);
    return ok;
}

static bool add_number(struct mo_protocol_server *s, struct mo_heap *heap, uint32_t list,
                       struct mo_cell *values, uint64_t number) {
    char digits[21];
    size_t at = sizeof digits - 1;
    digits[at] = 0;
    do {
        digits[--at] = (char)('0' + number % 10);
        number /= 10;
    } while (number != 0);
    return add_text(s, heap, list, values, digits + at);
}

static bool add_signed(struct mo_protocol_server *s, struct mo_heap *heap, uint32_t list,
                       struct mo_cell *values, int64_t number) {
    char text[21];
    int length = snprintf(text, sizeof text, "%" PRId64, number);
    return length > 0 && (size_t)length < sizeof text && add_text(s, heap, list, values, text);
}

bool mo_protocol_export(struct mo_protocol_server *s, struct protocol_connection *c,
                        struct mo_cell *out, bool *refused) {
    *refused = false;
    struct mo_heap *heap = mo_process_heap(c->process);
    MO_ASSERT(mo_heap_limit(heap) <= MO_LIMIT_EXPORT_SCRATCH);
    struct mo_cell action = mo_cell_get_payload(c->action);
    struct mo_cell arguments[] = {mo_value_field(action, 2)}, requested;
    if (!mo_protocol_invoke(s, c, PF_EXPORT_ID, arguments, &requested))
        return false;
    size_t requested_size;
    const uint8_t *requested_bytes = mo_value_data(requested, &requested_size);
    struct mo_cell failure_value;
    if (!mo_protocol_invoke(s, c, PF_FAILURE_ID, arguments, &failure_value)) {
        mo_protocol_drop(s, c, PF_EXPORT_ID, requested);
        return false;
    }
    uint64_t failure_id = (uint64_t)mo_cell_get_int(failure_value);
    struct mo_export_pin *pin = failure_id == 0 ? &s->state_export : &s->failure_export;
    bool expired = mo_export_expire(pin, s->now.monotonic_us);
    (void)expired;
    const char *error = "";
    if (requested_size == 0) {
        struct mo_service_observation observed = mo_service_observe(s->service);
        const uint8_t *saved;
        size_t saved_size;
        struct mo_export_identity id = {.kind = failure_id == 0 ? 1 : 2,
                                        .label = mo_runtime_build_label(),
                                        .service = s->service_index,
                                        .sequence = observed.sequence,
                                        .encoded_sequence = observed.encoded_sequence,
                                        .created = s->now.wall_us,
                                        .program = mo_image_key(s->image, s->service_index) ^
                                                   ((uint64_t)s->service_index << 32)};
        if (!mo_runtime_identity(id.build) || getentropy(id.id, sizeof id.id) != 0 ||
            (pin->bytes != NULL && memcmp(id.id, pin->id, sizeof id.id) == 0))
            error = "identity_unavailable";
        else if (failure_id != 0) {
            if (!mo_service_pin_failure(s->service, failure_id, pin, &id, s->now.monotonic_us))
                error = "failure_unavailable";
        } else if (!mo_service_encoded_state(s->service, &saved, &saved_size))
            error = "state_unavailable";
        else {
            mo_export_release(pin);
            if (!mo_export_create(pin, &id, saved, saved_size, s->now.monotonic_us))
                error = "export_limit";
        }
    } else if (pin->bytes == NULL)
        error = "export_released";
    else {
        char current[33];
        mo_protocol_hex(pin->id, 16, current);
        if (requested_size != 32 || memcmp(requested_bytes, current, 32) != 0 ||
            pin->failure_id != failure_id)
            error = "export_replaced";
    }
    mo_protocol_drop(s, c, PF_EXPORT_ID, requested);
    int64_t offset = mo_cell_get_int(mo_value_field(action, 3));
    size_t length = 0, count = 0;
    if (*error == 0 && (offset < 0 || (uint64_t)offset > SIZE_MAX ||
                        !mo_export_page_bounds(pin->length, (size_t)offset, &length, &count)))
        error = "invalid_offset";
    *refused = *error != 0;
    const struct mo_bytecode_function *fn = mo_protocol_function(s, PF_EXPORT);
    struct mo_cell args[3];
    if (!mo_value_text(heap, error, strlen(error), &args[0]))
        return false;
    if (!mo_value_list(heap, fn->parameters[1].type, NULL, 0, &args[1])) {
        mo_value_drop(heap, fn->parameters[0].type, args[0]);
        return false;
    }
    bool ok = true;
    bool complete = false;
    if (*error == 0) {
        char id[33], checksum[17], digest[65];
        mo_protocol_hex(pin->id, 16, id);
        uint64_t hash = mo_binary_checksum(pin->bytes + (size_t)offset, length);
        uint8_t big[8];
        for (unsigned i = 0; i < 8; i++)
            big[i] = (uint8_t)(hash >> ((7 - i) * 8));
        mo_protocol_hex(big, 8, checksum);
        complete = length == pin->length - (size_t)offset;
        mo_protocol_hex(pin->digest, 32, digest);
        uint32_t list = fn->parameters[1].type;
        ok = add_text(s, heap, list, &args[1], id) &&
             add_number(s, heap, list, &args[1], pin->sequence) &&
             add_number(s, heap, list, &args[1], pin->encoded_sequence) &&
             add_number(s, heap, list, &args[1], (uint64_t)offset) &&
             add_number(s, heap, list, &args[1], pin->length) &&
             add_number(s, heap, list, &args[1], count) &&
             add_text(s, heap, list, &args[1], checksum) &&
             add_text(s, heap, list, &args[1], complete ? digest : "") &&
             add_text(s, heap, list, &args[1], failure_id == 0 ? "state" : "failure") &&
             add_number(s, heap, list, &args[1], failure_id);
    }
    bool copied = ok && mo_protocol_charge(c, length) &&
                  mo_value_bytes(heap, *error == 0 ? pin->bytes + (size_t)offset : NULL,
                                 *error == 0 ? length : 0, &args[2]);
    ok = copied && mo_protocol_invoke(s, c, PF_EXPORT, args, out);
    if (copied)
        mo_value_drop(heap, fn->parameters[2].type, args[2]);
    mo_value_drop(heap, fn->parameters[1].type, args[1]);
    mo_value_drop(heap, fn->parameters[0].type, args[0]);
    if (ok && *error == 0) {
        pin->touched = s->now.monotonic_us;
        if (complete)
            mo_export_release(pin);
    }
    return ok;
}

struct failure_view {
    struct mo_protocol_server *server;
    struct mo_heap *heap;
    struct protocol_connection *connection;
    uint32_t list;
    struct mo_cell *values;
};
/* An unavailable row: id 0, the reason, then the arrival time or "". */
static bool unavailable_row(struct failure_view *v, const char *reason, const char *arrival) {
    bool ok = add_number(v->server, v->heap, v->list, v->values, 0) &&
              add_text(v->server, v->heap, v->list, v->values, reason) &&
              add_text(v->server, v->heap, v->list, v->values, "") &&
              add_text(v->server, v->heap, v->list, v->values, arrival) &&
              add_number(v->server, v->heap, v->list, v->values, 0) &&
              add_number(v->server, v->heap, v->list, v->values, 0);
    for (unsigned j = 6; ok && j < 15; j++)
        ok = add_text(v->server, v->heap, v->list, v->values, "");
    return ok;
}
/* false only for resource refusal; a row that does not decode is reported unavailable. */
static bool failure_view_row(const struct mo_store_failure *f, void *context) {
    struct failure_view *v = context;
    struct mo_failure_record record;
    if (!mo_protocol_charge(v->connection, f->message.length + strlen(f->kind)))
        return false;
    if (!mo_failure_read(f->message.bytes, f->message.length, &record) || record.id != f->id) {
        char reason[48];
        int length = snprintf(reason, sizeof reason, "record %" PRIu64 " unreadable", f->id);
        return length > 0 && (size_t)length < sizeof reason && unavailable_row(v, reason, "");
    }
    char name[MO_LIMIT_FAILURE_KIND_BYTES + 1];
    memcpy(name, record.name.bytes, record.name.length);
    name[record.name.length] = 0;
    int64_t arrival;
    memcpy(&arrival, &record.arrival, sizeof arrival);
    char reason[MO_LIMIT_FAILURE_KIND_BYTES + 1], limit[MO_LIMIT_FAILURE_KIND_BYTES + 1];
    memcpy(reason, record.reason.bytes, record.reason.length);
    reason[record.reason.length] = 0;
    memcpy(limit, record.limit.bytes, record.limit.length);
    limit[record.limit.length] = 0;
    static const char *classes[] = {"defect", "work", "memory", "state_size"};
    return add_number(v->server, v->heap, v->list, v->values, f->id) &&
           add_text(v->server, v->heap, v->list, v->values, f->kind) &&
           add_text(v->server, v->heap, v->list, v->values, name) &&
           add_signed(v->server, v->heap, v->list, v->values, arrival) &&
           add_number(v->server, v->heap, v->list, v->values, f->state.length) &&
           add_number(v->server, v->heap, v->list, v->values, f->message.length) &&
           add_number(v->server, v->heap, v->list, v->values, record.signature_version) &&
           add_text(v->server, v->heap, v->list, v->values, classes[record.class]) &&
           add_text(v->server, v->heap, v->list, v->values, reason) &&
           add_text(v->server, v->heap, v->list, v->values, limit) &&
           add_number(v->server, v->heap, v->list, v->values, record.limit_value) &&
           add_number(v->server, v->heap, v->list, v->values, record.function) &&
           add_number(v->server, v->heap, v->list, v->values, record.file) &&
           add_number(v->server, v->heap, v->list, v->values, record.start) &&
           add_number(v->server, v->heap, v->list, v->values, record.end);
}
bool mo_protocol_failures(struct mo_protocol_server *s, struct protocol_connection *c,
                          struct mo_cell *out) {
    struct mo_heap *heap = mo_process_heap(c->process);
    const struct mo_bytecode_function *fn = mo_protocol_function(s, PF_FAILURE_VIEW);
    struct mo_cell args[2] = {mo_value_field(mo_cell_get_payload(c->action), 2)};
    if (!mo_value_list(heap, fn->parameters[1].type, NULL, 0, &args[1]))
        return false;
    struct failure_view v = {s, heap, c, fn->parameters[1].type, &args[1]};
    struct mo_cell after;
    uint32_t omitted = 0;
    bool ok = mo_protocol_invoke(s, c, PF_FAILURE_AFTER, args, &after) &&
              mo_service_failure_catalog(s->service, (uint64_t)mo_cell_get_int(after),
                                         failure_view_row, &v, &omitted) == MO_STORE_OK;
    if (ok && omitted != 0) {
        ok = add_text(s, heap, v.list, v.values, "-1") &&
             add_number(s, heap, v.list, v.values, omitted);
        for (unsigned i = 2; ok && i < 15; i++)
            ok = add_text(s, heap, v.list, v.values, "");
    }
    for (uint32_t i = 0; ok && i < MO_LIMIT_SERVICE_EVENTS; i++) {
        struct mo_service_event_summary event;
        if (!mo_service_event_entry(s->service, i, &event))
            break;
        if (event.record_unavailable == NULL)
            continue;
        char arrival[21];
        int length = snprintf(arrival, sizeof arrival, "%" PRId64, event.time.now);
        ok = length > 0 && (size_t)length < sizeof arrival &&
             unavailable_row(&v, event.record_unavailable, arrival);
    }
    ok = ok && mo_protocol_invoke(s, c, PF_FAILURE_VIEW, args, out);
    mo_value_drop(heap, fn->parameters[1].type, args[1]);
    return ok;
}
