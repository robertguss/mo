#include "host/host.h"

#include <string.h>
#include <unistd.h>

static MO_MUST_USE uint32_t add(struct mo_host_types *out, enum mo_type_kind kind,
                                uint32_t parameters) {
    for (uint32_t i = 0; i < out->types.count; i++) {
        const struct mo_type *t = &out->types.items[i];
        if (t->kind != kind || t->parameter_count != parameters)
            continue;
        if (parameters != 0 && t->parameters[0] != out->text)
            continue;
        if (parameters == 2 && t->parameters[1] != out->text)
            continue;
        return i;
    }
    uint32_t index = out->types.count++;
    out->types.items[index] =
        (struct mo_type){.kind = kind,
                         .parameters = parameters == 0 ? NULL : out->parameters,
                         .parameter_count = parameters,
                         .reference = kind != MO_TYPE_INT,
                         .defined = true};
    return index;
}

void mo_host_prepare_types(struct mo_host_types *out, const struct mo_types *source,
                           struct mo_type *storage) {
    MO_ASSERT(out != NULL && source != NULL && source->finished && storage != NULL);
    memset(out, 0, sizeof *out);
    out->types = *source;
    memcpy(storage, source->items, (size_t)source->count * sizeof *storage);
    out->types.items = storage;
    out->types.capacity = source->count + MO_HOST_PRIVATE_TYPES;
    out->types.arena = NULL;
    out->types.type_slots = NULL;
    out->types.type_slot_count = 0;
    out->types.layout_slots = NULL;
    out->types.layout_slot_count = 0;
    out->integer = add(out, MO_TYPE_INT, 0);
    out->text = add(out, MO_TYPE_TEXT, 0);
    out->parameters[0] = out->parameters[1] = out->text;
    out->texts = add(out, MO_TYPE_LIST, 1);
    out->folders = add(out, MO_TYPE_SET, 1);
    out->files = add(out, MO_TYPE_MAP, 2);
}

void mo_host_init(struct mo_host *host, const struct mo_host_types *types) {
    MO_ASSERT(host != NULL && types != NULL);
    *host = (struct mo_host){
        .types = types, .input = STDIN_FILENO, .output = STDOUT_FILENO, .errors = STDERR_FILENO};
}
