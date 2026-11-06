#define _POSIX_C_SOURCE 200809L
#include "tools/offline.h"

#include "base/runtime-identity.h"
#include "tools/protocol.h"
#include "vm/vm.h"

#include <string.h>

bool mo_offline_renderer_open(struct mo_offline_renderer *renderer) {
    *renderer = (struct mo_offline_renderer){.function = MO_NO_INDEX};
    renderer->image = mo_protocol_build();
    if (renderer->image == NULL)
        return false;
    struct mo_limits limits = mo_limits_default();
    limits.heap_size = MO_LIMIT_OFFLINE_RENDER_MEMORY;
    struct mo_defect initialization;
    if (mo_image_init(renderer->image, &limits, &initialization) != MO_RETURNED) {
        mo_offline_renderer_close(renderer);
        return false;
    }
    const struct mo_bytecode *file = mo_image_bytecode(renderer->image);
    static const char name[] = "McpViews.offline_value";
    for (uint32_t i = 0; i < file->function_count; i++) {
        const struct mo_bytecode_string *candidate = &file->strings[file->functions[i].name];
        if (candidate->length == sizeof name - 1 &&
            memcmp(candidate->bytes, name, sizeof name - 1) == 0)
            renderer->function = i;
    }
    /* The protocol build checks the function's ABI, so it is always present. */
    MO_ASSERT(renderer->function != MO_NO_INDEX);
    return true;
}

void mo_offline_renderer_close(struct mo_offline_renderer *renderer) {
    if (renderer->image != NULL)
        mo_image_free(renderer->image);
    *renderer = (struct mo_offline_renderer){.function = MO_NO_INDEX};
}

static const char unrendered[] = "{\"value\":null,\"complete\":false}";

bool mo_offline_render(struct mo_offline_renderer *renderer, const uint8_t *saved, size_t length,
                       struct mo_buffer *out) {
    MO_ASSERT(renderer->image != NULL && (saved != NULL || length == 0));
    struct mo_limits limits = mo_limits_default();
    limits.heap_size = MO_LIMIT_OFFLINE_RENDER_MEMORY;
    struct mo_process *process = mo_process_new(renderer->image, &limits);
    if (process == NULL)
        return mo_buffer_append_text(out, unrendered);
    mo_process_confidential(process);
    struct mo_heap *heap = mo_process_heap(process);
    const struct mo_bytecode_function *fn =
        &mo_image_bytecode(renderer->image)->functions[renderer->function];
    struct mo_cell args[2], result;
    bool appended = false, ok = mo_value_bytes(heap, saved, length, &args[0]);
    if (ok) {
        args[1] = mo_cell_int(MO_LIMIT_OFFLINE_VALUE_BYTES);
        struct mo_defect defect;
        ok = mo_call(process, renderer->function, args, 2, &result, &defect) == MO_RETURNED;
        if (fn->parameters[0].ownership != MO_PARAMETER_OWNED)
            mo_value_drop(heap, fn->parameters[0].type, args[0]);
        if (ok) {
            size_t size;
            const uint8_t *text = mo_value_data(result, &size);
            appended = size == 0 ? mo_buffer_append_text(out, unrendered)
                                 : mo_buffer_append(out, text, size);
            mo_value_drop(heap, fn->result_type, result);
            ok = true;
        }
    }
    mo_process_free(process);
    return ok ? appended : mo_buffer_append_text(out, unrendered);
}

bool mo_offline_store_path(const char *folder, char *out, size_t size) {
    size_t length = strnlen(folder, MO_LIMIT_PATH_SIZE + 1);
    static const char file[] = "/service.sqlite";
    if (length > MO_LIMIT_PATH_SIZE || length + sizeof file > size)
        return false;
    memcpy(out, folder, length);
    memcpy(out + length, file, sizeof file);
    return true;
}

const char *mo_offline_folder_code(enum mo_store_folder status) {
    switch (status) {
    case MO_STORE_FOLDER_OK:
        return "ok";
    case MO_STORE_FOLDER_MISSING:
        return "missing";
    case MO_STORE_FOLDER_OWNED:
        return "owned";
    case MO_STORE_FOLDER_RECOVERY:
        return "recovery_required";
    case MO_STORE_FOLDER_UNREADABLE:
        return "unreadable";
    case MO_STORE_FOLDER_DAMAGED:
        return "damaged";
    case MO_STORE_FOLDER_IO:
        return "io";
    case MO_STORE_FOLDER_MEMORY:
        return "memory";
    case MO_STORE_FOLDER_HANDLES:
        return "handles";
    }
    MO_UNREACHABLE();
}

const char *mo_offline_folder_text(enum mo_store_folder status) {
    switch (status) {
    case MO_STORE_FOLDER_OK:
        return "read";
    case MO_STORE_FOLDER_MISSING:
        return "no saved data in";
    case MO_STORE_FOLDER_OWNED:
        return "a running service owns";
    case MO_STORE_FOLDER_RECOVERY:
        return "recovery required in";
    case MO_STORE_FOLDER_UNREADABLE:
        return "cannot read";
    case MO_STORE_FOLDER_DAMAGED:
        return "damaged saved data in";
    case MO_STORE_FOLDER_IO:
        return "could not read";
    case MO_STORE_FOLDER_MEMORY:
        return "out of memory reading";
    case MO_STORE_FOLDER_HANDLES:
        return "out of file handles reading";
    }
    MO_UNREACHABLE();
}

bool mo_offline_folder_message(struct mo_buffer *out, const char *folder,
                               enum mo_store_folder status, uint64_t damaged_sequence) {
    bool ok = mo_buffer_append_text(out, mo_offline_folder_text(status)) &&
              mo_buffer_append_byte(out, ' ') &&
              mo_render_terminal_text(out, folder, strnlen(folder, MO_LIMIT_PATH_SIZE));
    if (ok && status == MO_STORE_FOLDER_OWNED)
        ok = mo_buffer_append_text(out, "; nothing was read, and nothing waited for it");
    if (ok && status == MO_STORE_FOLDER_RECOVERY)
        ok = mo_buffer_append_text(
            out, ": an unfinished write left a journal to roll back; run again as the folder's "
                 "owner with --recover, which rolls it back by writing database pages but "
                 "changes no record");
    if (ok && status == MO_STORE_FOLDER_DAMAGED && damaged_sequence != 0)
        ok = mo_buffer_append_text(out, ": log record ") &&
             mo_buffer_append_uint(out, damaged_sequence) &&
             mo_buffer_append_text(out, " failed its check");
    return ok;
}

const char *mo_offline_build_refusal(const struct mo_failure_record *record) {
    static const uint8_t zero[32];
    if (memcmp(record->build, zero, sizeof zero) == 0 || record->label.length == 0)
        return "the record does not identify the build that recorded it";
    uint8_t mine[32];
    if (!mo_runtime_identity(mine))
        return "this build cannot read its own identity";
    if (memcmp(mine, record->build, sizeof mine) != 0)
        return "recorded by another build";
    return NULL;
}

bool mo_offline_hex(struct mo_buffer *out, const uint8_t *bytes, size_t length) {
    bool ok = true;
    for (size_t i = 0; ok && i < length; i++)
        ok = mo_buffer_append_hex(out, bytes[i], 2);
    return ok;
}

const char *mo_offline_class(enum mo_signature_class class) {
    switch (class) {
    case MO_SIGNATURE_DEFECT:
        return "defect";
    case MO_SIGNATURE_WORK:
        return "work";
    case MO_SIGNATURE_MEMORY:
        return "memory";
    case MO_SIGNATURE_STATE_SIZE:
        return "state_size";
    }
    MO_UNREACHABLE();
}

static bool quoted_number(struct mo_buffer *out, const char *name, uint64_t value) {
    return mo_buffer_append_text(out, name) && mo_buffer_append_byte(out, '"') &&
           mo_buffer_append_uint(out, value) && mo_buffer_append_byte(out, '"');
}

bool mo_offline_signature_json(struct mo_buffer *out, uint8_t version,
                               const struct mo_signature *s) {
    return mo_buffer_append_text(out, "{\"version\":") && mo_buffer_append_uint(out, version) &&
           mo_buffer_append_text(out, ",\"class\":\"") &&
           mo_buffer_append_text(out, mo_offline_class(s->class)) &&
           mo_buffer_append_text(out, "\",\"reason\":") &&
           mo_render_json_text(out, s->reason.bytes, s->reason.length) &&
           mo_buffer_append_text(out, ",\"limit\":") &&
           mo_render_json_text(out, s->limit.bytes, s->limit.length) &&
           quoted_number(out, ",\"value\":", s->limit_value) &&
           quoted_number(out, ",\"function\":", s->function) &&
           quoted_number(out, ",\"file\":", s->file) &&
           quoted_number(out, ",\"start\":", s->start) && quoted_number(out, ",\"end\":", s->end) &&
           mo_buffer_append_byte(out, '}');
}

bool mo_offline_signature_text(struct mo_buffer *out, const struct mo_signature *s) {
    bool ok = mo_buffer_append_text(out, mo_offline_class(s->class));
    if (ok && s->reason.length != 0)
        ok = mo_buffer_append_text(out, " \"") &&
             mo_render_terminal_text(out, s->reason.bytes, s->reason.length) &&
             mo_buffer_append_byte(out, '"');
    if (ok && s->limit.length != 0)
        ok = mo_buffer_append_text(out, " at ") &&
             mo_render_terminal_text(out, s->limit.bytes, s->limit.length) &&
             mo_buffer_append_text(out, " = ") && mo_buffer_append_uint(out, s->limit_value);
    if (ok && s->class == MO_SIGNATURE_DEFECT)
        ok = mo_buffer_append_text(out, " in function ") &&
             mo_buffer_append_uint(out, s->function) && mo_buffer_append_text(out, ", file ") &&
             mo_buffer_append_uint(out, s->file) && mo_buffer_append_text(out, " bytes ") &&
             mo_buffer_append_uint(out, s->start) && mo_buffer_append_byte(out, '-') &&
             mo_buffer_append_uint(out, s->end);
    return ok;
}

struct mo_failure_slice mo_offline_bare_name(const uint8_t *bytes, size_t length) {
    size_t end = 0;
    while (end < length && bytes[end] != '@')
        end++;
    size_t start = end;
    while (start > 0 && bytes[start - 1] != '.')
        start--;
    return (struct mo_failure_slice){bytes + start, end - start};
}

bool mo_offline_flush(FILE *stream, struct mo_buffer *buffer) {
    bool ok =
        fwrite(buffer->bytes, 1, buffer->length, stream) == buffer->length && fflush(stream) == 0;
    mo_buffer_clear(buffer);
    return ok;
}
