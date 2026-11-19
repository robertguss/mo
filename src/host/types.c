#include "host/host.h"

#include <string.h>
#include <unistd.h>

static MO_MUST_USE uint32_t add(struct mo_host_types *out, enum mo_type_kind kind,
                                const uint32_t *parameters, uint32_t count) {
    for (uint32_t i = 0; i < out->types.count; i++) {
        const struct mo_type *t = &out->types.items[i];
        if (t->kind == kind && t->parameter_count == count &&
            (count == 0 || memcmp(t->parameters, parameters, count * sizeof *parameters) == 0))
            return i;
    }
    uint32_t index = out->types.count++;
    out->types.items[index] = (struct mo_type){.kind = kind,
                                               .parameters = parameters,
                                               .parameter_count = count,
                                               .reference = kind >= MO_TYPE_TEXT,
                                               .defined = true};
    return index;
}

void mo_host_prepare_types(struct mo_host_types *out, const struct mo_types *source,
                           struct mo_type *storage) {
    MO_ASSERT(out != NULL && source != NULL && source->finished && storage != NULL);
    memset(out, 0, sizeof *out);
    out->types = *source;
    if (source->count != 0)
        memcpy(storage, source->items, (size_t)source->count * sizeof *storage);
    out->types.items = storage;
    out->types.capacity = source->count + MO_HOST_PRIVATE_TYPES;
    out->types.arena = NULL;
    out->types.type_slots = NULL;
    out->types.type_slot_count = 0;
    out->types.layout_slots = NULL;
    out->types.layout_slot_count = 0;
    for (unsigned i = 0; i <= MO_CAP_CONNECTION; i++)
        out->capabilities[i] = MO_NO_INDEX;
    for (uint32_t i = 0; i < source->count; i++) {
        if (source->items[i].kind == MO_TYPE_CAPABILITY &&
            source->items[i].capability != MO_CAP_SIMULATION)
            out->capabilities[source->items[i].capability] = i;
    }
    out->integer = add(out, MO_TYPE_INT, NULL, 0);
    out->time = add(out, MO_TYPE_TIME, NULL, 0);
    out->text = add(out, MO_TYPE_TEXT, NULL, 0);
    out->bytes = add(out, MO_TYPE_BYTES, NULL, 0);
    uint32_t *parameters = out->parameters;
    parameters[0] = parameters[1] = parameters[4] = out->text;
    parameters[2] = parameters[3] = out->bytes;
    out->texts = add(out, MO_TYPE_LIST, parameters, 1);
    out->folders = add(out, MO_TYPE_SET, parameters, 1);
    out->files = add(out, MO_TYPE_MAP, parameters, 2);
    out->replies = add(out, MO_TYPE_MAP, parameters + 2, 2);
    parameters[5] = out->replies;
    out->peers = add(out, MO_TYPE_MAP, parameters + 4, 2);
}

void mo_host_init(struct mo_host *host, const struct mo_host_types *types) {
    MO_ASSERT(host != NULL && types != NULL);
    *host = (struct mo_host){
        .types = types, .input = STDIN_FILENO, .output = STDOUT_FILENO, .errors = STDERR_FILENO};
}
