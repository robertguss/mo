#include "vm/internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

size_t mo_vm_frame_size(uint32_t registers) {
    MO_ASSERT(registers <= MO_LIMIT_FUNCTION_REGISTERS);
    return (sizeof(struct mo_vm_frame) + (size_t)registers * (sizeof(struct mo_cell) + 1) + 7) &
           ~(size_t)7;
}

struct mo_vm_frame *mo_vm_frame(struct mo_process *process) {
    MO_ASSERT(process->depth != 0 && process->top < process->used);
    MO_ASSERT(process->used <= process->capacity);
    return (struct mo_vm_frame *)(process->stack + process->top);
}

struct mo_cell *mo_vm_cells(struct mo_vm_frame *frame) {
    return (struct mo_cell *)(frame + 1);
}

uint8_t *mo_vm_states(const struct mo_process *process, struct mo_vm_frame *frame) {
    return (uint8_t *)(mo_vm_cells(frame) +
                       process->file->functions[frame->function].register_count);
}

const char *mo_vm_reserve(struct mo_process *process, size_t needed, uint32_t depth) {
    if (depth > process->limits.call_depth)
        return "--max-depth";
    if (needed > process->limits.stack_size)
        return "--max-stack";
    if (needed <= process->capacity)
        return NULL;
    size_t capacity = process->capacity;
    if (capacity == 0)
        capacity = 256;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    if (capacity > process->limits.stack_size)
        capacity = process->limits.stack_size;
    void *stack;
    if (process->confidential) {
        errno = ENOSPC;
        stack = mo_arena_resize(&process->confidential_stack, process->stack, process->capacity,
                                capacity, _Alignof(struct mo_vm_frame));
        if (stack == NULL)
            return errno == ENOSPC ? "--max-stack" : "native memory";
    } else
        stack = realloc(process->stack, capacity);
    if (stack == NULL)
        return "native memory";
    process->stack = stack;
    process->capacity = capacity;
    return NULL;
}

void mo_vm_enter(struct mo_process *process, uint32_t function, const struct mo_cell *arguments,
                 struct mo_cell environment, bool closure, bool tail, uint32_t destination,
                 uint32_t next) {
    MO_ASSERT(function < process->file->function_count);
    const struct mo_bytecode_function *fn = &process->file->functions[function];
    size_t previous = process->depth == 0 ? SIZE_MAX : process->top;
    size_t offset = process->used;
    if (tail) {
        struct mo_vm_frame *old = mo_vm_frame(process);
        offset = process->top;
        previous = old->previous;
        destination = old->destination;
        next = old->next;
    } else
        process->depth++;
    size_t extent = mo_vm_frame_size(fn->register_count);
    /* Verified: the parameters, then a closure's environment, are the first registers, and
     * a closure's code is entered only as a closure. */
    MO_ASSERT((size_t)fn->parameter_count + (closure ? 1u : 0u) <= fn->register_count);
    MO_ASSERT(closure == (fn->kind == MO_FUNCTION_CLOSURE));
    MO_ASSERT(offset <= process->capacity && extent <= process->capacity - offset);
    struct mo_vm_frame *frame = (struct mo_vm_frame *)(process->stack + offset);
    *frame = (struct mo_vm_frame){.previous = previous,
                                  .extent = extent,
                                  .function = function,
                                  .destination = destination,
                                  .next = next};
    process->top = offset;
    process->used = offset + extent;
    struct mo_cell *cells = mo_vm_cells(frame);
    uint8_t *states = mo_vm_states(process, frame);
    memset(states, VM_UNDEFINED, fn->register_count);
    for (uint32_t i = 0; i < fn->parameter_count; i++) {
        MO_ASSERT(fn->register_types[i] == fn->parameters[i].type);
        cells[i] = arguments[i];
        states[i] =
            process->types->items[fn->parameters[i].type].reference
                ? (fn->parameters[i].ownership == MO_PARAMETER_OWNED ? VM_OWNED : VM_BORROWED)
                : VM_VALUE;
        frame->owners += states[i] == VM_OWNED ? 1u : 0u;
    }
    if (closure) {
        MO_ASSERT(process->types->items[fn->register_types[fn->parameter_count]].kind ==
                  MO_TYPE_ENV);
        cells[fn->parameter_count] = environment;
        states[fn->parameter_count] = VM_OWNED;
        frame->owners++;
    }
}

struct mo_process *mo_process_new(const struct mo_image *image, const struct mo_limits *limits) {
    MO_ASSERT(image != NULL && limits != NULL);
    struct mo_vm_image *runtime = mo_image_runtime(image);
    if (runtime == NULL || !runtime->ready || limits->heap_size < sizeof(struct mo_process))
        return NULL;
    struct mo_process *process = calloc(1, sizeof *process);
    if (process == NULL)
        return NULL;
    struct mo_limits heap_limits = *limits;
    heap_limits.heap_size -= sizeof *process;
    process->heap = mo_heap_new(runtime->heap, &heap_limits);
    if (process->heap == NULL) {
        free(process);
        return NULL;
    }
    process->image = image;
    process->file = mo_image_bytecode(image);
    process->types = &runtime->host_types.types;
    mo_host_init(&process->host, &runtime->host_types);
    process->runtime = runtime;
    process->limits = *limits;
    process->owns_heap = true;
    return process;
}

void mo_process_confidential(struct mo_process *process) {
    MO_ASSERT(process != NULL && process->owns_heap && process->capacity == 0 &&
              !process->terminal);
    process->confidential = true;
    mo_arena_init_confidential(&process->confidential_stack, process->limits.stack_size);
    mo_heap_confidential(process->heap);
}

void mo_vm_dispose(struct mo_process *process) {
    if (process->confidential)
        mo_arena_free(&process->confidential_stack);
    else
        free(process->stack);
    if (process->owns_heap)
        mo_heap_free(process->heap);
}

void mo_process_free(struct mo_process *process) {
    if (process != NULL) {
        mo_vm_dispose(process);
        if (process->confidential)
            mo_host_wipe(process, sizeof *process);
        free(process);
    }
}

struct mo_heap *mo_process_heap(struct mo_process *process) {
    MO_ASSERT(process != NULL);
    return process->heap;
}

struct mo_host *mo_process_host(struct mo_process *process) {
    MO_ASSERT(process != NULL);
    return &process->host;
}

size_t mo_process_stack_used(const struct mo_process *process) {
    MO_ASSERT(process != NULL);
    return process->used;
}

void mo_process_instruction_limit(struct mo_process *process, uint64_t instructions) {
    MO_ASSERT(process != NULL && !process->terminal && process->depth == 0);
    process->bounded = true;
    process->instructions = instructions;
}

/* No frame owns these transferred arguments yet. */
static void drop_arguments(struct mo_process *process, const struct mo_bytecode_function *fn,
                           const struct mo_cell *arguments) {
    for (uint32_t i = 0; i < fn->parameter_count; i++)
        if (fn->parameters[i].ownership == MO_PARAMETER_OWNED)
            mo_value_drop(process->heap, fn->parameters[i].type, arguments[i]);
}

static MO_MUST_USE enum mo_controlled_outcome
call(struct mo_process *process, uint32_t function, const struct mo_cell *arguments,
     uint32_t argument_count, bool (*keep_running)(void *context), void *context,
     struct mo_cell *result_out, struct mo_defect *defect_out) {
    MO_ASSERT(process != NULL && !process->terminal && process->depth == 0 &&
              function < process->file->function_count && result_out != NULL && defect_out != NULL);
    const struct mo_bytecode_function *fn = &process->file->functions[function];
    MO_ASSERT(fn->kind != MO_FUNCTION_CLOSURE &&
              (fn->kind != MO_FUNCTION_INIT || !process->owns_heap) &&
              argument_count == fn->parameter_count && (arguments != NULL || argument_count == 0));
    MO_ASSERT(process->keep_running == NULL && process->control_context == NULL);
    if (keep_running != NULL && !keep_running(context)) {
        drop_arguments(process, fn, arguments);
        process->terminal = true;
        return MO_CONTROL_CANCELLED;
    }
    process->keep_running = keep_running;
    process->control_context = context;
    process->cancelled = false;
    const char *limit = mo_vm_reserve(process, mo_vm_frame_size(fn->register_count), 1);
    enum mo_outcome outcome;
    if (limit != NULL) {
        drop_arguments(process, fn, arguments);
        bool native = strcmp(limit, "native memory") == 0;
        outcome = mo_vm_stop(process, native ? "running out of memory" : "out of stack", limit,
                             false, mo_cell_unit(), MO_STOPPED);
    } else {
        mo_vm_enter(process, function, arguments, mo_cell_unit(), false, false, 0, 0);
        outcome = mo_vm_execute(process, result_out);
    }
    process->keep_running = NULL;
    process->control_context = NULL;
    if (process->cancelled) {
        MO_ASSERT(outcome == MO_STOPPED && process->terminal);
        return MO_CONTROL_CANCELLED;
    }
    if (outcome != MO_RETURNED)
        *defect_out = process->defect;
    switch (outcome) {
    case MO_RETURNED:
        return MO_CONTROL_RETURNED;
    case MO_STOPPED:
        return MO_CONTROL_STOPPED;
    case MO_FAILED:
        return MO_CONTROL_FAILED;
    }
    MO_UNREACHABLE();
}

enum mo_outcome mo_call(struct mo_process *process, uint32_t function,
                        const struct mo_cell *arguments, uint32_t argument_count,
                        struct mo_cell *result_out, struct mo_defect *defect_out) {
    switch (
        call(process, function, arguments, argument_count, NULL, NULL, result_out, defect_out)) {
    case MO_CONTROL_RETURNED:
        return MO_RETURNED;
    case MO_CONTROL_STOPPED:
        return MO_STOPPED;
    case MO_CONTROL_FAILED:
        return MO_FAILED;
    case MO_CONTROL_CANCELLED:
        MO_UNREACHABLE();
    }
    MO_UNREACHABLE();
}

enum mo_controlled_outcome
mo_call_controlled(struct mo_process *process, uint32_t function, const struct mo_cell *arguments,
                   uint32_t argument_count, bool (*keep_running)(void *context), void *context,
                   struct mo_cell *result_out, struct mo_defect *defect_out) {
    MO_ASSERT(keep_running != NULL);
    return call(process, function, arguments, argument_count, keep_running, context, result_out,
                defect_out);
}

enum mo_outcome mo_call_main(struct mo_process *process, uint32_t function,
                             const struct mo_cell *args, const struct mo_cell *env, int folder,
                             const struct mo_main_capabilities *supplied,
                             struct mo_cell *result_out, struct mo_defect *defect_out) {
    MO_ASSERT(process != NULL && !process->terminal && process->depth == 0 &&
              function < process->file->function_count && result_out != NULL && defect_out != NULL);
    const struct mo_bytecode_function *fn = &process->file->functions[function];
    MO_ASSERT(fn->kind == MO_FUNCTION_FUNCTION &&
              fn->parameter_count <= MO_LIMIT_BYTECODE_PARAMETERS);
    MO_ASSERT(supplied == NULL || (supplied->count == fn->parameter_count &&
                                   (supplied->count == 0 || supplied->values != NULL)));
    const char *setup = mo_host_start(&process->host);
    if (setup != NULL) {
        enum mo_outcome stopped =
            mo_vm_stop(process, setup, NULL, false, mo_cell_unit(), MO_STOPPED);
        *defect_out = process->defect;
        return stopped;
    }
    /* Construct before duplicating any owned argument, so refusal cannot leave
     * duplicates that have not yet entered a frame. Cached kinds remain owned. */
    for (uint32_t i = 0; i < fn->parameter_count; i++) {
        uint32_t type = fn->parameters[i].type;
        MO_ASSERT(process->types->items[type].kind == MO_TYPE_CAPABILITY &&
                  process->types->items[type].capability != MO_CAP_SIMULATION);
        if (supplied != NULL)
            continue;
        enum mo_capability cap = process->types->items[type].capability;
        if (process->capability_set[cap])
            continue;
        struct mo_host_error error;
        const struct mo_cell *contents = cap == MO_CAP_ARGS ? args : cap == MO_CAP_ENV ? env : NULL;
        enum mo_host_outcome outcome = mo_host_real(&process->host, process->heap, type, contents,
                                                    folder, &process->capabilities[cap], &error);
        if (outcome != MO_HOST_RETURNED) {
            const char *reason =
                outcome == MO_HOST_OUT_OF_MEMORY ? "running out of memory" : error.reason;
            const char *limit = outcome == MO_HOST_OUT_OF_MEMORY && errno != ENOMEM ? "--max-heap"
                                : outcome == MO_HOST_OUT_OF_MEMORY ? "native memory"
                                                                   : NULL;
            enum mo_outcome stopped =
                mo_vm_stop(process, reason, limit, false, mo_cell_unit(), MO_STOPPED);
            *defect_out = process->defect;
            return stopped;
        }
        process->capability_set[cap] = true;
    }
    struct mo_cell parameters[MO_LIMIT_BYTECODE_PARAMETERS];
    for (uint32_t i = 0; i < fn->parameter_count; i++) {
        uint32_t type = fn->parameters[i].type;
        enum mo_capability cap = process->types->items[type].capability;
        struct mo_cell value = supplied == NULL ? process->capabilities[cap] : supplied->values[i];
        parameters[i] = fn->parameters[i].ownership == MO_PARAMETER_OWNED
                            ? mo_value_dup(process->heap, type, value)
                            : value;
    }
    return mo_call(process, function, parameters, fn->parameter_count, result_out, defect_out);
}
