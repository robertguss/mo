#include "vm/internal.h"

#include <stdlib.h>
#include <string.h>

size_t mo_vm_frame_size(uint32_t registers) {
    MO_ASSERT(registers <= MO_LIMIT_FUNCTION_REGISTERS);
    return (sizeof(struct mo_vm_frame) + (size_t)registers * (sizeof(struct mo_cell) + 1) + 7) &
           ~(size_t)7;
}

struct mo_vm_frame *mo_vm_frame(struct mo_process *process) {
    MO_ASSERT(process->depth != 0 && process->top < process->used);
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
    void *stack = realloc(process->stack, capacity);
    if (stack == NULL)
        return "--max-heap";
    process->stack = stack;
    process->capacity = capacity;
    return NULL;
}

void mo_vm_enter(struct mo_process *process, uint32_t function, const struct mo_cell *arguments,
                 struct mo_cell environment, bool closure, bool tail, uint32_t destination,
                 uint32_t next) {
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
    MO_ASSERT(offset <= process->capacity && extent <= process->capacity - offset);
    struct mo_vm_frame *frame = (struct mo_vm_frame *)(process->stack + offset);
    memset(frame, 0, extent);
    *frame = (struct mo_vm_frame){.previous = previous,
                                  .extent = extent,
                                  .function = function,
                                  .destination = destination,
                                  .next = next};
    process->top = offset;
    process->used = offset + extent;
    struct mo_cell *cells = mo_vm_cells(frame);
    uint8_t *states = mo_vm_states(process, frame);
    for (uint32_t i = 0; i < fn->parameter_count; i++) {
        cells[i] = arguments[i];
        states[i] =
            process->types->items[fn->parameters[i].type].reference
                ? (fn->parameters[i].ownership == MO_PARAMETER_OWNED ? VM_OWNED : VM_BORROWED)
                : VM_VALUE;
    }
    if (closure) {
        cells[fn->parameter_count] = environment;
        states[fn->parameter_count] = VM_OWNED;
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
    process->types = mo_image_types(image);
    process->runtime = runtime;
    process->limits = *limits;
    process->owns_heap = true;
    return process;
}

void mo_vm_dispose(struct mo_process *process) {
    free(process->stack);
    if (process->owns_heap)
        mo_heap_free(process->heap);
}

void mo_process_free(struct mo_process *process) {
    if (process != NULL) {
        mo_vm_dispose(process);
        free(process);
    }
}

struct mo_heap *mo_process_heap(struct mo_process *process) {
    MO_ASSERT(process != NULL);
    return process->heap;
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

enum mo_outcome mo_call(struct mo_process *process, uint32_t function,
                        const struct mo_cell *arguments, uint32_t argument_count,
                        struct mo_cell *result_out, struct mo_defect *defect_out) {
    MO_ASSERT(process != NULL && !process->terminal && process->depth == 0 &&
              function < process->file->function_count && result_out != NULL && defect_out != NULL);
    const struct mo_bytecode_function *fn = &process->file->functions[function];
    MO_ASSERT(fn->kind != MO_FUNCTION_CLOSURE && argument_count == fn->parameter_count &&
              (arguments != NULL || argument_count == 0));
    const char *limit = mo_vm_reserve(process, mo_vm_frame_size(fn->register_count), 1);
    enum mo_outcome outcome;
    if (limit != NULL) {
        for (uint32_t i = 0; i < argument_count; i++) {
            if (fn->parameters[i].ownership == MO_PARAMETER_OWNED)
                mo_value_drop(process->heap, fn->parameters[i].type, arguments[i]);
        }
        outcome = mo_vm_stop(process, "out of stack", limit, false, mo_cell_unit(), MO_STOPPED);
    } else {
        mo_vm_enter(process, function, arguments, mo_cell_unit(), false, false, 0, 0);
        outcome = mo_vm_execute(process, result_out);
    }
    if (outcome != MO_RETURNED)
        *defect_out = process->defect;
    return outcome;
}
