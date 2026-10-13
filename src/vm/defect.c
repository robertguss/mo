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

/* Clip untrusted text at UTF-8 boundaries, escape terminal controls, and leave
 * room for the truncation marker and closing quote. No scratch allocation. */
static MO_MUST_USE bool text(struct mo_buffer *out, const uint8_t *bytes, size_t length,
                             bool quoted, size_t budget) {
    static const char marker[] = "… <truncated>";
    size_t available = out->limit - out->length;
    if (budget > available)
        budget = available;
    size_t reserve = sizeof marker - 1 + (quoted ? 2u : 0u);
    if (budget < reserve)
        return false;
    size_t end = out->length + budget - reserve;
    if (quoted && !mo_buffer_append_byte(out, '"'))
        return false;
    size_t i = 0;
    while (i < length) {
        uint8_t c = bytes[i];
        size_t count = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        MO_ASSERT(count <= length - i);
        char escape[16];
        const uint8_t *part = bytes + i;
        size_t n = count;
        bool interpolation = quoted && c == '#' && i + 1 < length && bytes[i + 1] == '{';
        bool c1 = c == 0xc2 && count == 2 && bytes[i + 1] >= 0x80 && bytes[i + 1] <= 0x9f;
        if (c < 32 || c == 127 || c1 || c == '\\' || (quoted && c == '"') || interpolation) {
            int written = c == '\n'   ? snprintf(escape, sizeof escape, "\\n")
                          : c == '\r' ? snprintf(escape, sizeof escape, "\\r")
                          : c == '\t' ? snprintf(escape, sizeof escape, "\\t")
                          : c == '\\' || c == '"' || interpolation
                              ? snprintf(escape, sizeof escape, "\\%c", c)
                              : snprintf(escape, sizeof escape, "\\u{%x}", c1 ? bytes[i + 1] : c);
            MO_ASSERT(written > 0 && (size_t)written < sizeof escape);
            part = (const uint8_t *)escape;
            n = (size_t)written;
        }
        if (out->length > end || n > end - out->length)
            break;
        if (!mo_buffer_append(out, part, n))
            return false;
        i += count;
    }
    if (i != length && !mo_buffer_append_text(out, marker))
        return false;
    return !quoted || mo_buffer_append_byte(out, '"');
}

static MO_MUST_USE bool string(const struct mo_bytecode *file, uint32_t index,
                               struct mo_buffer *out, size_t budget) {
    const struct mo_bytecode_string *s = &file->strings[index];
    return text(out, s->bytes, s->length, false, budget);
}

static MO_MUST_USE bool frame_text(const struct mo_bytecode *file,
                                   const struct mo_defect_frame *frame, struct mo_buffer *out) {
    char position[64];
    int n = 0;
    size_t fixed = 6; /* prefix and newline */
    if (frame->span.file != MO_NO_INDEX) {
        n = snprintf(position, sizeof position, ":%u..%u", frame->span.start, frame->span.end);
        MO_ASSERT(n > 0 && (size_t)n < sizeof position);
        fixed += (size_t)n + 3;
    }
    size_t available = out->limit - out->length;
    if (available < fixed)
        return false;
    /* Long function/file names must not crowd out the rest of the top frame
     * or the marker for omitted details, even in a small caller buffer. */
    if (available > fixed + 64)
        available -= 32;
    size_t budget = (available - fixed) / (frame->span.file == MO_NO_INDEX ? 1u : 2u);
    if (budget > 512)
        budget = 512;
    if (!mo_buffer_append_text(out, "  at ") ||
        !string(file, file->functions[frame->function].name, out, budget))
        return false;
    if (frame->span.file != MO_NO_INDEX) {
        if (!mo_buffer_append_text(out, " (") || !string(file, frame->span.file, out, budget) ||
            !mo_buffer_append(out, position, (size_t)n) || !mo_buffer_append_byte(out, ')'))
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
    struct mo_buffer *destination = out, report;
    size_t available = out->limit - out->length;
    mo_buffer_init(&report,
                   available < MO_LIMIT_DEFECT_TEXT_SIZE ? available : MO_LIMIT_DEFECT_TEXT_SIZE);
    out = &report;
    size_t reason_budget = defect->frames == NULL ? out->limit : out->limit / 3;
    if (reason_budget > 512)
        reason_budget = 512;
    bool ok =
        text(out, (const uint8_t *)defect->reason, strlen(defect->reason), false, reason_budget);
    if (defect->limit_name != NULL) {
        char number[64];
        int n = snprintf(number, sizeof number, " = %zu)", defect->limit);
        ok = ok && n >= 0 && (size_t)n < sizeof number && mo_buffer_append_text(out, " (") &&
             mo_buffer_append_text(out, defect->limit_name) &&
             mo_buffer_append(out, number, (size_t)n);
    }
    ok = ok && mo_buffer_append_byte(out, '\n');
    /* The reason and top frame precede all optional detail. */
    if (ok && defect->frames != NULL)
        ok = frame_text(process->file, defect->frames, out);
    bool essential = ok;
    if (!essential)
        goto finish;
    if (defect->has_detail) {
        size_t length;
        const uint8_t *data = mo_value_data(defect->detail, &length);
        size_t row = out->length;
        if (!mo_buffer_append_text(out, "  detail: ") || !text(out, data, length, false, 4096) ||
            !mo_buffer_append_byte(out, '\n')) {
            out->length = row;
            goto truncated;
        }
    }
    uint32_t frames = 0;
    for (const struct mo_defect_frame *f = defect->frames; f != NULL; f = f->previous)
        frames++;
    uint32_t index = 0;
    for (const struct mo_defect_frame *f = defect->frames; f != NULL; f = f->previous, index++) {
        if (index == 0)
            continue;
        if (out->limit - out->length < 128)
            goto truncated;
        size_t row = out->length;
        if (frames > 8 && index >= 4 && index < frames - 4) {
            if (index != 4)
                continue;
            ok = mo_buffer_append_text(out, "  … ") && mo_buffer_append_uint(out, frames - 8) &&
                 mo_buffer_append_text(out, " frames …\n");
        } else
            ok = frame_text(process->file, f, out);
        if (!ok) {
            out->length = row;
            goto truncated;
        }
    }
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
                if (out->limit - out->length < 128) {
                    ok = false;
                    break;
                }
                size_t row = out->length;
                ok = mo_buffer_append_text(out, "  ") &&
                     string(process->file, name->name, out, 512) &&
                     mo_buffer_append_text(out, " = ");
                if (ok) {
                    uint32_t type = fn->register_types[name->reg];
                    struct mo_cell value = mo_vm_cells(f)[name->reg];
                    size_t start = out->length;
                    size_t budget = out->limit - start > 32 ? out->limit - start - 32 : 0;
                    if (budget > 4096)
                        budget = 4096;
                    if (process->types->items[type].kind == MO_TYPE_TEXT) {
                        size_t length;
                        const uint8_t *bytes = mo_value_data(value, &length);
                        ok = text(out, bytes, length, true, budget);
                    } else {
                        struct mo_buffer variable;
                        mo_buffer_init(&variable, budget);
                        ok = mo_value_print(process->heap, type, value, &variable) &&
                             mo_buffer_append(out, variable.bytes, variable.length);
                        mo_buffer_free(&variable);
                    }
                    if (!ok) {
                        out->length = start;
                        ok = mo_buffer_append_text(out, "<value unavailable>");
                    }
                }
                ok = ok && mo_buffer_append_byte(out, '\n');
                if (!ok)
                    out->length = row;
            }
        }
        free(readable);
    }
    if (!ok) {
    truncated:;
        bool marked = mo_buffer_append_text(out, "  … <truncated>\n");
        (void)marked;
    }
finish:;
    bool appended = mo_buffer_append(destination, out->bytes, out->length);
    mo_buffer_free(&report);
    return appended && essential;
}
