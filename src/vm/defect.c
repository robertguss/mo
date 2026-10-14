#include "prim/prim.h"
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

/* One escaped UTF-8 scalar. The image/heap already validated its encoding. */
static MO_MUST_USE size_t text_part(const uint8_t *bytes, size_t length, uint8_t delimiter,
                                    char escape[16], const uint8_t **part, size_t *size) {
    uint8_t c = bytes[0];
    size_t count = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    MO_ASSERT(count <= length);
    *part = bytes;
    *size = count;
    bool interpolation = delimiter != 0 && c == '#' && length > 1 && bytes[1] == '{';
    bool c1 = c == 0xc2 && count == 2 && bytes[1] >= 0x80 && bytes[1] <= 0x9f;
    if (c < 32 || c == 127 || c1 || c == '\\' || (delimiter != 0 && c == delimiter) ||
        interpolation) {
        int written = c == '\n'   ? snprintf(escape, 16, "\\n")
                      : c == '\r' ? snprintf(escape, 16, "\\r")
                      : c == '\t' ? snprintf(escape, 16, "\\t")
                      : c == '\\' || (delimiter != 0 && c == delimiter) || interpolation
                          ? snprintf(escape, 16, "\\%c", c)
                          : snprintf(escape, 16, "\\u{%x}", c1 ? bytes[1] : c);
        MO_ASSERT(written > 0 && written < 16);
        *part = (const uint8_t *)escape;
        *size = (size_t)written;
    }
    return count;
}

/* Stop measuring once the budget cannot hold the whole escaped spelling. */
static MO_MUST_USE size_t text_size(const uint8_t *bytes, size_t length, uint8_t delimiter,
                                    size_t budget) {
    size_t total = delimiter == 0 ? 0 : 2;
    if (total > budget)
        return budget + 1;
    for (size_t at = 0; at < length;) {
        char escape[16];
        const uint8_t *part;
        size_t size;
        at += text_part(bytes + at, length - at, delimiter, escape, &part, &size);
        if (size > budget - total)
            return budget + 1;
        total += size;
    }
    return total;
}

/* Reserve a marker only if truncating, and keep complete UTF-8 scalars. */
static MO_MUST_USE bool text(struct mo_buffer *out, const uint8_t *bytes, size_t length,
                             uint8_t delimiter, size_t budget) {
    static const char marker[] = "… <truncated>";
    size_t available = out->limit - out->length;
    if (budget > available)
        budget = available;
    bool clipped = text_size(bytes, length, delimiter, budget) > budget;
    size_t reserve = (clipped ? sizeof marker - 1 : 0) + (delimiter != 0 ? 2u : 0u);
    if (budget < reserve)
        return false;
    size_t end = out->length + budget - reserve + (delimiter != 0 ? 1u : 0u);
    if (delimiter != 0 && !mo_buffer_append_byte(out, delimiter))
        return false;
    size_t i = 0;
    while (i < length) {
        char escape[16];
        const uint8_t *part;
        size_t n;
        size_t count = text_part(bytes + i, length - i, delimiter, escape, &part, &n);
        if (out->length > end || n > end - out->length)
            break;
        if (!mo_buffer_append(out, part, n))
            return false;
        i += count;
    }
    if (i != length && !mo_buffer_append_text(out, marker))
        return false;
    return delimiter == 0 || mo_buffer_append_byte(out, delimiter);
}

static MO_MUST_USE bool string(const struct mo_bytecode *file, uint32_t index,
                               struct mo_buffer *out, size_t budget) {
    const struct mo_bytecode_string *s = &file->strings[index];
    return text(out, s->bytes, s->length, 0, budget);
}

/* Paths are bytes, not Text. Preserve valid scalars, and escape each invalid byte separately.
 * In particular a literal backslash is escaped, so FF and ASCII backslash-xff stay distinct. */
static MO_MUST_USE size_t filename_part(const uint8_t *bytes, size_t length, char escape[16],
                                        const uint8_t **part, size_t *size) {
    uint8_t c = bytes[0];
    uint32_t count = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    /* Width validates UTF-8 too; this bounded candidate contains at most one scalar. */
    uint64_t width;
    if (count <= length && mo_unicode16_display_width(bytes, count, &width))
        return text_part(bytes, length, 0, escape, part, size);
    static const char hex[] = "0123456789abcdef";
    escape[0] = '\\';
    escape[1] = 'x';
    escape[2] = hex[c >> 4];
    escape[3] = hex[c & 15];
    *part = (const uint8_t *)escape;
    *size = 4;
    return 1;
}

static MO_MUST_USE size_t filename_size(const struct mo_bytecode_bytes *path, size_t budget) {
    size_t total = 0;
    for (size_t i = 0; i < path->length;) {
        char escape[16];
        const uint8_t *part;
        size_t size;
        i += filename_part(path->bytes + i, path->length - i, escape, &part, &size);
        if (size > budget - total)
            return budget + 1;
        total += size;
    }
    return total;
}

static MO_MUST_USE bool filename(const struct mo_bytecode_bytes *path, struct mo_buffer *out,
                                 size_t budget) {
    static const char marker[] = "… <truncated>";
    size_t available = out->limit - out->length;
    if (budget > available)
        budget = available;
    bool clipped = filename_size(path, budget) > budget;
    size_t reserve = clipped ? sizeof marker - 1 : 0;
    if (budget < reserve)
        return false;
    size_t end = out->length + budget - reserve;
    size_t i = 0;
    while (i < path->length) {
        char escape[16];
        const uint8_t *part;
        size_t size;
        size_t count = filename_part(path->bytes + i, path->length - i, escape, &part, &size);
        if (size > end - out->length)
            break;
        if (!mo_buffer_append(out, part, size))
            return false;
        i += count;
    }
    return i == path->length || mo_buffer_append_text(out, marker);
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
    size_t space = available - fixed;
    size_t budget = space / (frame->span.file == MO_NO_INDEX ? 1u : 2u);
    if (budget > 512)
        budget = 512;
    size_t file_budget = budget;
    if (frame->span.file != MO_NO_INDEX) {
        const struct mo_bytecode_string *fn = &file->strings[file->functions[frame->function].name];
        const struct mo_bytecode_bytes *path = &file->files[frame->span.file];
        size_t fn_size = text_size(fn->bytes, fn->length, 0, 512);
        size_t path_size = filename_size(path, 512);
        if (fn_size <= budget) {
            budget = fn_size;
            file_budget = space - budget;
        } else if (path_size <= file_budget) {
            file_budget = path_size;
            budget = space - file_budget;
        } else
            file_budget = space - budget;
        if (budget > 512)
            budget = 512;
        if (file_budget > 512)
            file_budget = 512;
    }
    if (!mo_buffer_append_text(out, "  at ") ||
        !string(file, file->functions[frame->function].name, out, budget))
        return false;
    if (frame->span.file != MO_NO_INDEX) {
        if (!mo_buffer_append_text(out, " (") ||
            !filename(&file->files[frame->span.file], out, file_budget) ||
            !mo_buffer_append(out, position, (size_t)n) || !mo_buffer_append_byte(out, ')'))
            return false;
    }
    return mo_buffer_append_byte(out, '\n');
}

/* Never inspect cells after a failed replay, even if it filled part of the
 * readability vector before refusing. */
static MO_MUST_USE bool *variables_readable(const struct mo_process *process,
                                            const struct mo_vm_frame *frame) {
    const struct mo_bytecode_function *fn = &process->file->functions[frame->function];
    bool *readable = calloc(fn->register_count, sizeof *readable);
    struct mo_load_error error;
    if (readable == NULL || !mo_image_registers_readable(process->image, frame->function, frame->pc,
                                                         readable, &error)) {
        free(readable);
        return NULL;
    }
    return readable;
}

static MO_MUST_USE bool variable_visible(const struct mo_process *process,
                                         struct mo_vm_frame *frame,
                                         const struct mo_bytecode_register_name *name,
                                         const bool *readable) {
    if (name->code_start > frame->pc || name->code_end <= frame->pc || !readable[name->reg])
        return false;
    /* A suspended caller is still at its call instruction, but owned operands
     * have already transferred. Replay alone describes the pre-call state. */
    uint8_t state = mo_vm_states(process, frame)[name->reg];
    return state == VM_VALUE || state == VM_BORROWED || state == VM_OWNED;
}

enum mo_defect_variables_result mo_visit_defect_variables(
    const struct mo_process *process, const struct mo_defect *defect, uint32_t count, uint64_t work,
    bool (*visit)(const struct mo_defect_variable *variable, void *context), void *context) {
    MO_ASSERT(defect != NULL && defect->reason != NULL && visit != NULL);
    if (process == NULL)
        process = defect->process;
    MO_ASSERT(process == defect->process);
    if (defect->frames == NULL)
        return MO_DEFECT_VARIABLES_COMPLETE;
    MO_ASSERT(process != NULL && process->terminal && defect->frames == process->defect.frames);
    size_t offset = process->top;
    while (offset != SIZE_MAX) {
        if (work == 0)
            return MO_DEFECT_VARIABLES_LIMIT;
        work--;
        struct mo_vm_frame *frame = (struct mo_vm_frame *)(process->stack + offset);
        offset = frame->previous;
        const struct mo_bytecode_debug *debug = &process->file->debug[frame->function];
        if (debug->name_count == 0)
            continue;
        const struct mo_bytecode_function *fn = &process->file->functions[frame->function];
        uint64_t replay = ((uint64_t)fn->register_count + fn->code_count + 32) *
                          (MO_LIMIT_VERIFY_WORK_PER_WORD + 1u);
        if (replay > work)
            return MO_DEFECT_VARIABLES_LIMIT;
        work -= replay;
        bool *readable = variables_readable(process, frame);
        if (readable == NULL)
            return MO_DEFECT_VARIABLES_UNAVAILABLE;
        enum mo_defect_variables_result result = MO_DEFECT_VARIABLES_COMPLETE;
        for (uint32_t i = 0; i < debug->name_count; i++) {
            if (work == 0) {
                result = MO_DEFECT_VARIABLES_LIMIT;
                break;
            }
            work--;
            const struct mo_bytecode_register_name *name = &debug->names[i];
            if (!variable_visible(process, frame, name, readable))
                continue;
            if (count == 0) {
                result = MO_DEFECT_VARIABLES_LIMIT;
                break;
            }
            count--;
            const struct mo_bytecode_string *spelling = &process->file->strings[name->name];
            struct mo_defect_variable variable = {.frame = &frame->trace,
                                                  .name = spelling->bytes,
                                                  .name_length = spelling->length,
                                                  .type = fn->register_types[name->reg],
                                                  .value = mo_vm_cells(frame)[name->reg]};
            if (!visit(&variable, context)) {
                result = MO_DEFECT_VARIABLES_STOPPED;
                break;
            }
        }
        free(readable);
        if (result != MO_DEFECT_VARIABLES_COMPLETE)
            return result;
    }
    return MO_DEFECT_VARIABLES_COMPLETE;
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
    size_t reason_size = text_size((const uint8_t *)defect->reason, strlen(defect->reason), 0,
                                   out->limit < 512 ? out->limit : 512);
    if (reason_size <= out->limit && reason_size <= 512 && reason_size > reason_budget)
        reason_budget = reason_size;
    bool ok = text(out, (const uint8_t *)defect->reason, strlen(defect->reason), 0, reason_budget);
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
        bool prefix = mo_buffer_append_text(out, "  detail: ");
        size_t budget = out->limit - out->length;
        if (budget != 0)
            budget--;
        if (budget > 4096)
            budget = 4096;
        if (!prefix || !text(out, data, length, 0, budget) || !mo_buffer_append_byte(out, '\n')) {
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
        bool *readable = variables_readable(process, f);
        if (readable == NULL) {
            ok = mo_buffer_append_text(out, "  <variables unavailable>\n");
        } else {
            for (uint32_t i = 0; ok && i < debug->name_count; i++) {
                const struct mo_bytecode_register_name *name = &debug->names[i];
                if (!variable_visible(process, f, name, readable))
                    continue;
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
                        ok = text(out, bytes, length, '"', budget);
                    } else if (process->types->items[type].kind == MO_TYPE_CHAR) {
                        uint32_t code = mo_cell_get_char(value);
                        uint8_t bytes[4];
                        size_t length = code < 0x80 ? 1 : code < 0x800 ? 2 : code < 0x10000 ? 3 : 4;
                        for (size_t k = length; k > 1; k--) {
                            bytes[k - 1] = (uint8_t)(0x80 | (code & 0x3f));
                            code >>= 6;
                        }
                        bytes[0] = (uint8_t)((length == 1   ? 0
                                              : length == 2 ? 0xc0
                                              : length == 3 ? 0xe0
                                                            : 0xf0) |
                                             code);
                        ok = text(out, bytes, length, '\'', budget);
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
