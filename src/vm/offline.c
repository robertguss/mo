#include "vm/vm.h"

#include <string.h>

/* Live, an `Error` from a tool is a reply, not a failure; a timer's result is its new state. */
static enum mo_reply_kind reply_kind(const struct mo_types *types, bool timer, uint32_t type,
                                     struct mo_cell value) {
    const struct mo_type *t = &types->items[type];
    bool result = t->kind == MO_TYPE_CHOICE && strncmp(t->name, "Result(", 7) == 0;
    return !timer && result && mo_cell_get_tag(value) == 1 ? MO_REPLY_ERROR : MO_REPLY_OK;
}

static enum mo_outcome_status failure_status(enum mo_signature_class class) {
    switch (class) {
    case MO_SIGNATURE_DEFECT:
        return MO_OUTCOME_DEFECT;
    case MO_SIGNATURE_WORK:
        return MO_OUTCOME_WORK;
    case MO_SIGNATURE_MEMORY:
        return MO_OUTCOME_MEMORY;
    case MO_SIGNATURE_STATE_SIZE:
        return MO_OUTCOME_STATE_SIZE;
    }
    MO_UNREACHABLE();
}

/* Decodes the state and every argument into the process's heap; the values live until the
 * process is freed. False when the bytes do not decode as the handler's types. */
static bool decode_inputs(struct mo_process *process, const struct mo_bytecode_service *service,
                          uint32_t handler, const uint8_t *state, size_t state_length,
                          const struct mo_failure_record *record, struct mo_cell *state_out,
                          struct mo_cell *arguments) {
    struct mo_heap *heap = mo_process_heap(process);
    int64_t arrival = mo_failure_arrival(record);
    if (arrival < MO_TIME_MIN_US || arrival > MO_TIME_MAX_US ||
        !mo_value_decode(heap, service->state_type, state, state_length, state_out))
        return false;
    uint32_t count = record->timer ? 0 : service->tools[handler].argument_count;
    struct mo_failure_arguments walk = mo_failure_arguments(record);
    struct mo_failure_slice bytes;
    for (uint32_t i = 0; i < count; i++)
        if (mo_failure_argument_next(&walk, &bytes) != MO_FAILURE_ARGUMENT ||
            !mo_value_decode(heap, service->tools[handler].argument_types[i], bytes.bytes,
                             bytes.length, &arguments[i]))
            return false;
    return mo_failure_argument_next(&walk, &bytes) == MO_FAILURE_ARGUMENTS_END;
}

struct mo_offline_run mo_offline_call(const struct mo_image *image,
                                      const struct mo_bytecode_service *service, uint32_t handler,
                                      const uint8_t *state, size_t state_length,
                                      const struct mo_failure_record *record,
                                      struct mo_buffer *context) {
    MO_ASSERT(image != NULL && service != NULL && state != NULL && record != NULL &&
              context != NULL);
    const struct mo_bytecode *file = mo_image_bytecode(image);
    MO_ASSERT(service >= file->services && service < file->services + file->service_count);
    bool timer = record->timer != 0;
    MO_ASSERT(handler < (timer ? service->timer_count : service->tool_count));
    struct mo_offline_run run = {.outcome = {.status = MO_OUTCOME_REFUSED},
                                 .context = MO_FAILURE_CONTEXT_COMPLETE};
    struct mo_limits limits = mo_service_limits();
    struct mo_process *process = mo_process_new(image, &limits);
    if (process == NULL) {
        run.refusal = "out of memory for the run";
        return run;
    }
    struct mo_cell before, arguments[MO_LIMIT_BYTECODE_PARAMETERS - 2];
    if (!decode_inputs(process, service, handler, state, state_length, record, &before,
                       arguments)) {
        mo_process_free(process);
        run.refusal = "the recorded inputs could not be decoded";
        return run;
    }
    struct mo_buffer encoded;
    mo_buffer_init(&encoded, MO_LIMIT_SAVED_BYTES);
    struct mo_service_call call;
    mo_service_call_handler(process, service, timer, handler, before, mo_failure_arrival(record),
                            arguments, &encoded, &call);
    if (call.returned)
        run.outcome.reply = reply_kind(mo_image_types(image), timer, call.type, call.value);
    if (call.status == MO_SERVICE_OK) {
        run.outcome.status = MO_OUTCOME_SUCCEEDED;
        run.outcome.state_changed =
            call.updated &&
            (encoded.length != state_length || memcmp(encoded.bytes, state, state_length) != 0);
    } else {
        run.outcome.signature = mo_service_signature(&call);
        run.outcome.status = failure_status(run.outcome.signature.class);
        /* A new state that failed the size check differs from the one that passed it. */
        run.outcome.state_changed = run.outcome.status == MO_OUTCOME_STATE_SIZE;
        run.context = mo_service_context(process, &call.defect, context);
    }
    mo_buffer_free(&encoded);
    mo_process_free(process);
    return run;
}
