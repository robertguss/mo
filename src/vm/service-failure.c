#include "vm/service-internal.h"

#include <string.h>

#include "base/binary.h"
#include "base/export.h"
#include "base/failure.h"
#include "base/runtime-identity.h"

static bool capture_string(struct mo_buffer *out, const char *text) {
    return mo_failure_text(out, text, strlen(text));
}

struct captured_variables {
    struct mo_buffer *context;
    struct mo_heap *heap;
    const struct mo_defect_frame *top;
    bool unavailable;
};
static bool capture_variable(const struct mo_defect_variable *v, void *context) {
    struct captured_variables *c = context;
    if (v->frame != c->top)
        return false;
    struct mo_buffer value;
    mo_buffer_init(&value, MO_LIMIT_FAILURE_VALUE_BYTES);
    bool rendered = mo_value_print_literal(c->heap, v->type, v->value, &value);
    bool ok = mo_buffer_append_text(c->context, "variable ") &&
              mo_buffer_append(c->context, v->name, v->name_length) &&
              mo_buffer_append_text(c->context, rendered ? " available: " : " unavailable: ") &&
              (!rendered || mo_buffer_append(c->context, value.bytes, value.length)) &&
              mo_buffer_append_byte(c->context, '\n');
    c->unavailable = c->unavailable || !rendered;
    mo_buffer_free(&value);
    return ok;
}

/* The store binds the id; the payload must decode once it does. */
static bool capture_readable(struct mo_buffer *message) {
    struct mo_failure_record record;
    message->bytes[MO_FAILURE_ID_OFFSET] = 1;
    bool readable = mo_failure_read(message->bytes, message->length, &record);
    message->bytes[MO_FAILURE_ID_OFFSET] = 0;
    return readable;
}

static struct mo_failure_slice slice(const char *text) {
    return (struct mo_failure_slice){(const uint8_t *)text, strlen(text)};
}

struct mo_signature mo_service_signature(const struct mo_service_call *call) {
    MO_ASSERT(call->status == MO_SERVICE_MEMORY || call->status == MO_SERVICE_WORK ||
              call->status == MO_SERVICE_DEFECT);
    const struct mo_defect *defect = &call->defect;
    /* A returned handler failed only in its new state's saved copy; the size limits are named. */
    bool state_size =
        call->returned && call->saved != MO_SAVED_OK && call->saved != MO_SAVED_MEMORY;
    bool work = call->status == MO_SERVICE_WORK;
    struct mo_signature signature = {
        .function = defect->frames == NULL ? call->function : defect->frames->function,
        .file = defect->reason == NULL ? MO_NO_INDEX : defect->span.file,
        .start = defect->reason == NULL ? 0 : defect->span.start,
        .end = defect->reason == NULL ? 0 : defect->span.end};
    if (state_size) {
        signature.class = MO_SIGNATURE_STATE_SIZE;
        signature.reason = slice("saved state size");
        bool depth = call->saved == MO_SAVED_DEPTH, nodes = call->saved == MO_SAVED_NODES;
        signature.limit = slice(depth   ? "MO_LIMIT_SAVED_DEPTH"
                                : nodes ? "MO_LIMIT_SAVED_NODES"
                                        : "MO_LIMIT_SAVED_BYTES");
        signature.limit_value = depth   ? MO_LIMIT_SAVED_DEPTH
                                : nodes ? MO_LIMIT_SAVED_NODES
                                        : MO_LIMIT_SAVED_BYTES;
    } else if (work) {
        signature.class = MO_SIGNATURE_WORK;
        signature.reason = slice("work limit");
        signature.limit = slice("MO_LIMIT_SERVICE_WORK");
        signature.limit_value = MO_LIMIT_SERVICE_WORK;
    } else {
        bool memory = call->status == MO_SERVICE_MEMORY || defect->limit_name != NULL;
        signature.class = memory ? MO_SIGNATURE_MEMORY : MO_SIGNATURE_DEFECT;
        signature.reason = slice(defect->reason == NULL ? "native memory" : defect->reason);
        signature.limit = slice(defect->limit_name == NULL
                                    ? (call->status == MO_SERVICE_MEMORY ? "native allocation" : "")
                                    : defect->limit_name);
        signature.limit_value = defect->limit;
    }
    return signature;
}

struct mo_frame_walk mo_process_frames(const struct mo_process *process) {
    MO_ASSERT(process != NULL);
    return (struct mo_frame_walk){process, process->depth == 0 ? SIZE_MAX : process->top};
}

bool mo_process_frame_next(struct mo_frame_walk *walk, struct mo_defect_frame *out) {
    MO_ASSERT(walk != NULL && out != NULL);
    if (walk->offset == SIZE_MAX)
        return false;
    const struct mo_process *p = walk->process;
    MO_ASSERT(walk->offset < p->capacity);
    const struct mo_vm_frame *f = (const struct mo_vm_frame *)(p->stack + walk->offset);
    *out = (struct mo_defect_frame){
        .function = f->function, .pc = f->pc, .span = mo_vm_span(p->file, f->function, f->pc)};
    walk->offset = f->previous;
    return true;
}

enum mo_failure_context mo_service_context(struct mo_process *p, const struct mo_defect *defect,
                                           struct mo_buffer *out) {
    enum mo_failure_context status = MO_FAILURE_CONTEXT_COMPLETE;
    /* Walk live frames before normal cleanup, including cancelled calls. */
    struct mo_frame_walk walk = mo_process_frames(p);
    struct mo_defect_frame frame;
    unsigned frames = 0;
    bool truncated = false;
    while (mo_process_frame_next(&walk, &frame)) {
        if (frames == MO_LIMIT_FAILURE_FRAMES) {
            truncated = true;
            break;
        }
        bool ok =
            mo_buffer_append_text(out, "frame ") && mo_buffer_append_uint(out, frame.function) &&
            mo_buffer_append_text(out, " file ") && mo_buffer_append_uint(out, frame.span.file) &&
            mo_buffer_append_text(out, " span ") && mo_buffer_append_uint(out, frame.span.start) &&
            mo_buffer_append_text(out, "..") && mo_buffer_append_uint(out, frame.span.end) &&
            mo_buffer_append_byte(out, '\n');
        if (!ok) {
            status = MO_FAILURE_CONTEXT_UNAVAILABLE;
            break;
        }
        frames++;
    }
    if (defect->frames != NULL && defect->process != NULL) {
        struct captured_variables variables = {out, p->heap, defect->frames, false};
        enum mo_defect_variables_result visited =
            mo_visit_defect_variables(p, defect, MO_LIMIT_FAILURE_VARIABLES,
                                      MO_LIMIT_FAILURE_CONTEXT_WORK, capture_variable, &variables);
        truncated = truncated || visited != MO_DEFECT_VARIABLES_COMPLETE || variables.unavailable;
    } else if (!mo_buffer_append_text(out, "variables unavailable\n"))
        status = MO_FAILURE_CONTEXT_UNAVAILABLE;
    else
        truncated = true;
    /* Unavailable is never downgraded to truncated. */
    if (truncated && status == MO_FAILURE_CONTEXT_COMPLETE)
        status = MO_FAILURE_CONTEXT_TRUNCATED;
    return status;
}

void mo_service_capture_failure(struct mo_service_loop *s, const struct service_message *m,
                                const struct mo_service_call *call) {
    s->capture_id = 0;
    s->capture_unsafe = false;
    s->capture_refusal = "out of memory for the record";
    if (s->store == NULL || s->current_encoding.length == 0) {
        s->capture_refusal = "persistent state unavailable";
        return;
    }
    struct mo_buffer metadata, context, kind;
    mo_buffer_init(&metadata, MO_LIMIT_FAILURE_MESSAGE_BYTES);
    mo_buffer_init(&context, MO_LIMIT_FAILURE_CONTEXT_BYTES);
    /* An over-long kind still fits here, so its refusal names the kind, not memory. */
    mo_buffer_init(&kind, MO_LIMIT_FAILURE_MESSAGE_BYTES);
    struct mo_bytecode_string name = mo_service_handler_name(s, call->function);
    uint32_t count = m->timer ? 0 : s->descriptor->tools[m->handler].argument_count;
    bool readonly = !m->timer && s->descriptor->tools[m->handler].read_only;
    struct mo_signature signature = mo_service_signature(call);
    uint8_t class = (uint8_t)signature.class;
    enum mo_failure_context context_status =
        mo_service_context(s->process, &call->defect, &context);
    bool ok = mo_binary_write(&metadata, MO_FAILURE_VERSION, 1) &&
              mo_binary_write(&metadata, 0, MO_FAILURE_ID_BYTES) &&
              mo_binary_write(&metadata, s->sequence, 8) &&
              mo_binary_write(&metadata, (uint64_t)m->arrival, 8) &&
              mo_binary_write(&metadata, m->timer, 1) &&
              mo_binary_write(&metadata, m->handler, 4) &&
              mo_failure_text(&metadata, name.bytes, name.length) &&
              mo_binary_write(&metadata, readonly, 1) && mo_binary_write(&metadata, count, 4);
    for (uint32_t i = 0; ok && i < count; i++) {
        size_t at = metadata.length;
        ok = mo_binary_write(&metadata, 0, 4);
        size_t start = metadata.length;
        if (ok) {
            enum mo_saved_status encoded = mo_value_encode_diagnostic(
                s->process->heap, s->descriptor->tools[m->handler].argument_types[i],
                m->arguments[i], &metadata);
            if (encoded != MO_SAVED_OK) {
                s->capture_refusal = encoded == MO_SAVED_MEMORY ? "out of memory for the record"
                                     : readonly                 ? "arguments could not be encoded"
                                                                : "too large to record";
                ok = false;
            }
        }
        if (ok)
            for (unsigned j = 0; j < 4; j++)
                metadata.bytes[at + j] = (uint8_t)((metadata.length - start) >> (8 * j));
    }
    size_t tail = 10 + 4 + signature.reason.length + 4 + signature.limit.length + 16 + 48 + 32 + 4 +
                  strlen(mo_runtime_build_label()) + 32 + 1 + 4 + context.length;
    if (ok && tail > metadata.limit - metadata.length) {
        s->capture_refusal = "too large to record";
        ok = false;
    }
    uint8_t build[32], schema[32];
    if (ok && (!mo_runtime_identity(build) ||
               !mo_export_schema(s->current_encoding.bytes, s->current_encoding.length, schema))) {
        s->capture_refusal = "runtime identity unavailable";
        ok = false;
    }
    ok = ok && mo_binary_write(&metadata, 1, 1) && mo_binary_write(&metadata, class, 1) &&
         mo_failure_text(&metadata, signature.reason.bytes, signature.reason.length) &&
         mo_failure_text(&metadata, signature.limit.bytes, signature.limit.length) &&
         mo_binary_write(&metadata, signature.limit_value, 8) &&
         mo_binary_write(&metadata, signature.function, 4) &&
         mo_binary_write(&metadata, signature.file, 4) &&
         mo_binary_write(&metadata, signature.start, 4) &&
         mo_binary_write(&metadata, signature.end, 4) &&
         mo_binary_write(&metadata, MO_LIMIT_SERVICE_WORK, 8) &&
         mo_binary_write(&metadata, s->process->limits.heap_size, 8) &&
         mo_binary_write(&metadata, s->process->limits.stack_size, 8) &&
         mo_binary_write(&metadata, s->process->limits.call_depth, 8) &&
         mo_binary_write(&metadata, MO_LIMIT_SAVED_BYTES, 8) &&
         mo_binary_write(&metadata, s->image_key, 8) && mo_buffer_append(&metadata, build, 32) &&
         capture_string(&metadata, mo_runtime_build_label()) &&
         mo_buffer_append(&metadata, schema, 32) &&
         mo_binary_write(&metadata, (uint64_t)context_status, 1) &&
         mo_failure_text(&metadata, context.bytes, context.length);
    ok = ok && mo_buffer_append(&kind, name.bytes, name.length) &&
         mo_buffer_append_byte(&kind, ':') && mo_buffer_append_uint(&kind, class) &&
         mo_buffer_append_byte(&kind, ':') &&
         mo_buffer_append(&kind, signature.reason.bytes, signature.reason.length) &&
         mo_buffer_append_byte(&kind, ':') &&
         mo_buffer_append(&kind, signature.limit.bytes, signature.limit.length);
    if (ok && class == 0)
        ok = mo_buffer_append_byte(&kind, ':') &&
             mo_buffer_append_uint(&kind, signature.function) &&
             mo_buffer_append_byte(&kind, ':') && mo_buffer_append_uint(&kind, signature.file) &&
             mo_buffer_append_byte(&kind, ':') && mo_buffer_append_uint(&kind, signature.start) &&
             mo_buffer_append_byte(&kind, ':') && mo_buffer_append_uint(&kind, signature.end);
    ok = ok && mo_buffer_append_byte(&kind, 0);
    if (ok && kind.length - 1 > MO_LIMIT_FAILURE_KIND_BYTES) {
        s->capture_refusal = "failure kind too long";
        ok = false;
    }
    if (ok && metadata.length > MO_LIMIT_FAILURE_RECORD_BYTES - MO_LIMIT_FAILURE_ENVELOPE_BYTES -
                                    s->current_encoding.length) {
        s->capture_refusal = "too large to record";
        ok = false;
    }
    if (ok && !capture_readable(&metadata)) {
        s->capture_refusal = "record failed validation";
        ok = false;
    }
    if (ok) {
        struct mo_store_record state = {MO_STORE_FAILURE_STATE, s->sequence, s->image_key,
                                        s->current_encoding.bytes, s->current_encoding.length};
        struct mo_store_record message = {MO_STORE_FAILURE_MESSAGE, s->sequence, s->image_key,
                                          metadata.bytes, metadata.length};
        enum mo_store_capture_status result =
            mo_store_capture(s->store, (const char *)kind.bytes, &state, &message, &s->capture_id);
        s->capture_refusal = result == MO_STORE_CAPTURE_OK        ? NULL
                             : result == MO_STORE_CAPTURE_DAMAGED ? "diagnostic records damaged"
                             : result == MO_STORE_CAPTURE_UNSAFE  ? "store outcome uncertain"
                                                                  : "the store refused the record";
        s->capture_unsafe = result == MO_STORE_CAPTURE_UNSAFE;
    }
    mo_buffer_free(&metadata);
    mo_buffer_free(&context);
    mo_buffer_free(&kind);
}

enum mo_store_status mo_service_failures(const struct mo_service_loop *s,
                                         bool (*consume)(const struct mo_store_failure *, void *),
                                         void *context) {
    return s->store == NULL ? MO_STORE_IO : mo_store_failures(s->store, consume, context);
}

enum mo_store_status mo_service_failure_catalog(const struct mo_service_loop *s, uint64_t after,
                                                bool (*consume)(const struct mo_store_failure *,
                                                                void *),
                                                void *context, uint32_t *omitted) {
    return s->store == NULL ? MO_STORE_IO
                            : mo_store_failure_catalog(s->store, after, consume, context, omitted);
}

bool mo_service_failure_export(const struct mo_service_loop *s, uint64_t id,
                               struct mo_buffer *out) {
    return s->store != NULL && mo_store_failure_export(s->store, id, out);
}

struct failure_pin {
    uint64_t failure, now;
    struct mo_export_pin *pin;
    struct mo_export_identity identity;
    bool found;
};
static bool pin_failure(const struct mo_store_failure *record, void *context) {
    struct failure_pin *p = context;
    if (record->id != p->failure)
        return true;
    struct mo_failure_record metadata;
    if (!mo_failure_read(record->message.bytes, record->message.length, &metadata) ||
        metadata.id != record->id)
        return false;
    char label[MO_LIMIT_EXPORT_LABEL_BYTES + 1];
    memcpy(label, metadata.label.bytes, metadata.label.length);
    label[metadata.label.length] = 0;
    p->identity.label = label;
    memcpy(p->identity.build, metadata.build, 32);
    p->identity.program = metadata.program ^ ((uint64_t)p->identity.service << 32);
    p->identity.sequence = record->state.sequence;
    p->identity.encoded_sequence = record->state.sequence;
    /* One failure pin at a time: release the old one only once its replacement is found. */
    mo_export_release(p->pin);
    p->found =
        mo_export_create_failure(p->pin, &p->identity, record->state.bytes, record->state.length,
                                 record->message.bytes, record->message.length, p->now);
    return p->found;
}
bool mo_service_pin_failure(const struct mo_service_loop *s, uint64_t failure,
                            struct mo_export_pin *pin, const struct mo_export_identity *identity,
                            uint64_t now) {
    struct failure_pin context = {failure, now, pin, *identity, false};
    return s->store != NULL &&
           mo_store_failure_get(s->store, failure, pin_failure, &context) == MO_STORE_OK &&
           context.found;
}
