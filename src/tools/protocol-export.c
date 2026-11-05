#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "base/binary.h"
#include "base/runtime-identity.h"
#include "tools/protocol-server.h"
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

static void hex(const uint8_t *bytes, size_t length, char *out) {
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
    struct mo_export_pin *pin = &s->state_export;
    bool expired = mo_export_expire(pin, s->now.monotonic_us);
    (void)expired;
    const char *error = "";
    if (requested_size == 0) {
        struct mo_service_observation observed = mo_service_observe(s->service);
        const uint8_t *saved;
        size_t saved_size;
        struct mo_export_identity id = {.kind = 1,
                                        .label = mo_runtime_build_label(),
                                        .service = s->service_index,
                                        .sequence = observed.sequence,
                                        .encoded_sequence = observed.encoded_sequence,
                                        .created = s->now.wall_us,
                                        .program = mo_image_key(s->image, s->service_index) ^
                                                   ((uint64_t)s->service_index << 32)};
        if (!mo_service_encoded_state(s->service, &saved, &saved_size))
            error = "state_unavailable";
        else if (!mo_runtime_identity(id.build) || getentropy(id.id, sizeof id.id) != 0 ||
                 (pin->bytes != NULL && memcmp(id.id, pin->id, sizeof id.id) == 0))
            error = "identity_unavailable";
        else {
            /* Release before allocation: replacement cannot double the per-kind budget. */
            mo_export_release(pin);
            if (!mo_export_create(pin, &id, saved, saved_size, s->now.monotonic_us))
                error = "export_limit";
        }
    } else if (pin->bytes == NULL)
        error = "export_released";
    else {
        char current[33];
        hex(pin->id, 16, current);
        if (requested_size != 32 || memcmp(requested_bytes, current, 32) != 0)
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
        hex(pin->id, 16, id);
        uint64_t hash = mo_binary_checksum(pin->bytes + (size_t)offset, length);
        uint8_t big[8];
        for (unsigned i = 0; i < 8; i++)
            big[i] = (uint8_t)(hash >> ((7 - i) * 8));
        hex(big, 8, checksum);
        complete = length == pin->length - (size_t)offset;
        hex(pin->digest, 32, digest);
        uint32_t list = fn->parameters[1].type;
        ok = add_text(s, heap, list, &args[1], id) &&
             add_number(s, heap, list, &args[1], pin->sequence) &&
             add_number(s, heap, list, &args[1], pin->encoded_sequence) &&
             add_number(s, heap, list, &args[1], (uint64_t)offset) &&
             add_number(s, heap, list, &args[1], pin->length) &&
             add_number(s, heap, list, &args[1], count) &&
             add_text(s, heap, list, &args[1], checksum) &&
             add_text(s, heap, list, &args[1], complete ? digest : "");
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
