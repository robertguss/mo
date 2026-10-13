#include "vm/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mo_vm_span mo_vm_span(const struct mo_bytecode *file, uint32_t function, uint32_t pc) {
    const struct mo_bytecode_debug *debug = &file->debug[function];
    uint32_t low = 0, high = debug->span_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        if (debug->spans[middle].code_offset <= pc)
            low = middle + 1;
        else
            high = middle;
    }
    if (low == 0)
        return (struct mo_vm_span){.file = MO_NO_INDEX};
    const struct mo_bytecode_span *span = &debug->spans[low - 1];
    return (struct mo_vm_span){span->file, span->start, span->end};
}

enum mo_outcome mo_vm_stop(struct mo_process *p, const char *reason, const char *limit, bool detail,
                           struct mo_cell value, enum mo_outcome outcome) {
    MO_ASSERT(outcome != MO_RETURNED && reason != NULL);
    size_t offset = p->depth == 0 ? SIZE_MAX : p->top;
    const struct mo_defect_frame *top = NULL;
    while (offset != SIZE_MAX) {
        struct mo_vm_frame *f = (struct mo_vm_frame *)(p->stack + offset);
        f->trace = (struct mo_defect_frame){
            .function = f->function,
            .pc = f->pc,
            .span = mo_vm_span(p->file, f->function, f->pc),
            .previous = f->previous == SIZE_MAX
                            ? NULL
                            : &((struct mo_vm_frame *)(p->stack + f->previous))->trace};
        if (top == NULL)
            top = &f->trace;
        offset = f->previous;
    }
    size_t amount = limit == NULL                       ? 0
                    : strcmp(limit, "--max-stack") == 0 ? p->limits.stack_size
                    : strcmp(limit, "--max-depth") == 0 ? p->limits.call_depth
                                                        : p->limits.heap_size;
    p->terminal = true;
    p->defect = (struct mo_defect){.reason = reason,
                                   .limit_name = limit,
                                   .limit = amount,
                                   .has_detail = detail,
                                   .detail = value,
                                   .frames = top,
                                   .process = p,
                                   .span = top == NULL ? (struct mo_vm_span){.file = MO_NO_INDEX}
                                                       : top->span};
    return outcome;
}

static MO_MUST_USE bool string(const struct mo_bytecode *file, uint32_t index,
                               struct mo_buffer *out) {
    const struct mo_bytecode_string *s = &file->strings[index];
    return mo_buffer_append(out, s->bytes, s->length);
}

static MO_MUST_USE bool frame_text(const struct mo_bytecode *file,
                                   const struct mo_defect_frame *frame, struct mo_buffer *out) {
    if (!mo_buffer_append_text(out, "  at ") ||
        !string(file, file->functions[frame->function].name, out))
        return false;
    if (frame->span.file != MO_NO_INDEX) {
        char position[64];
        int n = snprintf(position, sizeof position, ":%u..%u", frame->span.start, frame->span.end);
        if (n < 0 || (size_t)n >= sizeof position || !mo_buffer_append_text(out, " (") ||
            !string(file, frame->span.file, out) || !mo_buffer_append(out, position, (size_t)n) ||
            !mo_buffer_append_byte(out, ')'))
            return false;
    }
    return mo_buffer_append_byte(out, '\n');
}

bool mo_render_defect(const struct mo_process *process, const struct mo_defect *defect,
                      struct mo_buffer *out) {
    MO_ASSERT(defect != NULL && defect->reason != NULL && out != NULL);
    if (process == NULL)
        process = defect->process;
    MO_ASSERT(process == defect->process);
    size_t before = out->length;
    bool ok = mo_buffer_append_text(out, defect->reason);
    if (defect->has_detail) {
        size_t length;
        const uint8_t *data = mo_value_data(defect->detail, &length);
        ok = ok && mo_buffer_append_text(out, ": ") && mo_buffer_append(out, data, length);
    }
    if (defect->limit_name != NULL) {
        char number[64];
        int n = snprintf(number, sizeof number, " = %zu)", defect->limit);
        ok = ok && n >= 0 && (size_t)n < sizeof number && mo_buffer_append_text(out, " (") &&
             mo_buffer_append_text(out, defect->limit_name) &&
             mo_buffer_append(out, number, (size_t)n);
    }
    ok = ok && mo_buffer_append_byte(out, '\n');
    for (const struct mo_defect_frame *f = defect->frames; ok && f != NULL; f = f->previous)
        ok = frame_text(process->file, f, out);
    if (ok && defect->frames != NULL &&
        process->file->debug[defect->frames->function].name_count != 0) {
        struct mo_vm_frame *f = (struct mo_vm_frame *)(process->stack + process->top);
        const struct mo_bytecode_function *fn = &process->file->functions[f->function];
        const struct mo_bytecode_debug *debug = &process->file->debug[f->function];
        bool *readable = malloc((size_t)fn->register_count * sizeof(bool));
        struct mo_load_error error;
        if (readable == NULL ||
            !mo_image_registers_readable(process->image, f->function, f->pc, readable, &error)) {
            ok = mo_buffer_append_text(out, "  <variables unavailable>\n");
        } else {
            for (uint32_t i = 0; ok && i < debug->name_count; i++) {
                const struct mo_bytecode_register_name *name = &debug->names[i];
                if (name->code_start > f->pc || name->code_end <= f->pc || !readable[name->reg])
                    continue;
                uint8_t state = mo_vm_states(process, f)[name->reg];
                MO_ASSERT(state == VM_VALUE || state == VM_BORROWED || state == VM_OWNED);
                ok = mo_buffer_append_text(out, "  ") && string(process->file, name->name, out) &&
                     mo_buffer_append_text(out, " = ");
                if (ok && !mo_value_print(process->heap, fn->register_types[name->reg],
                                          mo_vm_cells(f)[name->reg], out))
                    ok = mo_buffer_append_text(out, "<value unavailable>");
                ok = ok && mo_buffer_append_byte(out, '\n');
            }
        }
        free(readable);
    }
    if (!ok)
        out->length = before;
    return ok;
}
