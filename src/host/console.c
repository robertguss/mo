#define _POSIX_C_SOURCE 200809L
#include "host/internal.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

struct console_input {
    uint8_t bytes[4096];
    size_t position, length;
    bool ended;
};

enum mo_host_outcome mo_host_console_real(struct mo_heap *heap, uint32_t type,
                                          struct mo_cell *out) {
    struct console_input input = {0};
    struct mo_capability_info info = {.operations = &mo_host_operations[false][MO_CAP_CONSOLE]};
    return mo_value_capability_native(heap, type, &info, &input, sizeof input, out)
               ? MO_HOST_RETURNED
               : MO_HOST_OUT_OF_MEMORY;
}

/* Unicode's maximal subpart rule: consume the valid prefix of an incomplete
 * sequence, but leave its first invalid next byte for the following subpart. */
static MO_MUST_USE size_t subpart(const uint8_t *bytes, size_t length, bool *valid) {
    MO_ASSERT(length != 0);
    uint8_t first = bytes[0];
    *valid = first < 0x80;
    if (*valid)
        return 1;
    size_t count = first >= 0xc2 && first <= 0xdf   ? 2
                   : first >= 0xe0 && first <= 0xef ? 3
                   : first >= 0xf0 && first <= 0xf4 ? 4
                                                    : 1;
    if (count == 1)
        return 1;
    for (size_t i = 1; i < count; i++) {
        if (i == length || bytes[i] < 0x80 || bytes[i] > 0xbf)
            return i;
        if (i == 1 && ((first == 0xe0 && bytes[i] < 0xa0) || (first == 0xed && bytes[i] >= 0xa0) ||
                       (first == 0xf0 && bytes[i] < 0x90) || (first == 0xf4 && bytes[i] > 0x8f)))
            return 1;
    }
    *valid = true;
    return count;
}

static MO_MUST_USE bool lossy(struct mo_heap *heap, struct mo_scratch *scratch,
                              const uint8_t *bytes, size_t length, struct mo_cell *out) {
    size_t size = 0;
    for (size_t at = 0; at < length;) {
        bool valid;
        size_t count = subpart(bytes + at, length - at, &valid);
        size_t added = valid ? count : 3;
        if (added > UINT32_MAX - size) {
            errno = ENOSPC;
            return false;
        }
        size += added;
        at += count;
    }
    uint8_t *text = size == 0 ? NULL : mo_scratch_resize(scratch, NULL, 0, size);
    if (size != 0 && text == NULL)
        return false;
    size_t written = 0;
    for (size_t at = 0; at < length;) {
        bool valid;
        size_t count = subpart(bytes + at, length - at, &valid);
        size_t n = valid ? count : 3;
        memcpy(text + written, valid ? bytes + at : (const uint8_t *)"\xef\xbf\xbd", n);
        written += n;
        at += count;
    }
    MO_ASSERT(written == size);
    return mo_value_text(heap, (const char *)text, size, out);
}

static MO_MUST_USE enum mo_host_outcome input(struct mo_host *host, struct mo_heap *heap,
                                              uint32_t type, const struct mo_capability_info *info,
                                              struct mo_cell *out, struct mo_host_error *error) {
    struct console_input *in = (struct console_input *)info->handle;
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, heap);
    uint8_t *bytes = NULL;
    size_t length = 0, capacity = 0;
    enum mo_host_outcome outcome = MO_HOST_OUT_OF_MEMORY;
    bool newline = false;
    for (;;) {
        if (in->position == in->length && !in->ended) {
            ssize_t n;
            do {
                n = read(host->input, in->bytes, sizeof in->bytes);
            } while (n < 0 && errno == EINTR);
            if (n < 0) {
                *error =
                    (struct mo_host_error){.reason = "internal error: console input unavailable"};
                outcome = MO_HOST_INTERNAL;
                goto done;
            }
            in->position = 0;
            in->length = (size_t)n;
            in->ended = n == 0;
        }
        if (in->position == in->length)
            break;
        const uint8_t *start = in->bytes + in->position;
        size_t available = in->length - in->position;
        const uint8_t *end = memchr(start, '\n', available);
        size_t count = end == NULL ? available : (size_t)(end - start);
        if (count > UINT32_MAX - length) {
            errno = ENOSPC;
            goto done;
        }
        if (length + count > capacity) {
            size_t next = capacity == 0 ? 256 : capacity;
            while (next < length + count)
                next = next > UINT32_MAX / 2 ? UINT32_MAX : next * 2;
            void *grown = mo_scratch_resize(&scratch, bytes, capacity, next);
            if (grown == NULL)
                goto done;
            bytes = grown;
            capacity = next;
        }
        if (count != 0)
            memcpy(bytes + length, start, count);
        length += count;
        in->position += count;
        if (end != NULL) {
            in->position++;
            newline = true;
            break;
        }
    }
    if (newline && length != 0 && bytes[length - 1] == '\r')
        length--;
    bool found = newline || length != 0;
    struct mo_cell text;
    if (found && !lossy(heap, &scratch, bytes, length, &text))
        goto done;
    bool built = mo_host_case(host, heap, type, found ? "Some" : "None", found ? &text : NULL,
                              found ? 1 : 0, out);
    if (found)
        mo_value_drop(heap, host->types->text, text);
    outcome = built ? MO_HOST_RETURNED : MO_HOST_OUT_OF_MEMORY;
done:
    mo_scratch_free(&scratch);
    return outcome;
}

enum mo_host_outcome mo_host_console_fake(struct mo_host *host, struct mo_heap *heap, uint32_t type,
                                          const struct mo_cell *input, struct mo_cell *out) {
    struct mo_cell empty;
    if (!mo_value_list(heap, host->types->texts, NULL, 0, &empty))
        return MO_HOST_OUT_OF_MEMORY;
    uint32_t types[] = {host->types->texts, host->types->texts, host->types->texts,
                        host->types->integer};
    struct mo_cell values[] = {input == NULL ? empty : *input, empty, empty, mo_cell_int(0)};
    enum mo_host_outcome result = mo_host_state_cap(host, heap, type, true, types, values, 4, out);
    mo_value_drop(heap, host->types->texts, empty);
    return result;
}

static MO_MUST_USE enum mo_host_outcome output(struct mo_host *host, struct mo_cell text, int fd,
                                               struct mo_host_error *error) {
    (void)host;
    struct sigaction action = {.sa_handler = SIG_IGN};
    if (sigemptyset(&action.sa_mask) != 0 || sigaction(SIGPIPE, &action, NULL) != 0)
        return mo_host_bug(error, "could not ignore SIGPIPE");
    size_t length;
    const uint8_t *bytes = mo_value_data(text, &length);
    size_t at = 0;
    while (at < length) {
        ssize_t n = write(fd, bytes + at, length - at);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return mo_host_bug(error, "printing to an output that is gone");
        at += (size_t)n;
    }
    ssize_t n;
    do {
        n = write(fd, "\n", 1);
    } while (n < 0 && errno == EINTR);
    if (n != 1)
        return mo_host_bug(error, "printing to an output that is gone");
    return MO_HOST_RETURNED;
}

enum mo_host_outcome mo_host_console(struct mo_host *host, struct mo_heap *heap, uint32_t operation,
                                     uint32_t type, const struct mo_cell *args, struct mo_cell *out,
                                     struct mo_host_error *error) {
    const struct mo_capability_info *info = mo_capability_get(args[0]);
    const struct mo_host_operations *operations = info->operations;
    bool fake = operations->fake;
    if (operation == MO_HOST_CONSOLE_PRINT || operation == MO_HOST_CONSOLE_ERROR) {
        if (fake) {
            uint32_t index = operation == MO_HOST_CONSOLE_PRINT ? 1 : 2;
            if (!mo_list_append(heap, mo_state_type(info->state, index),
                                mo_state_slot(info->state, index), args[1]))
                return MO_HOST_OUT_OF_MEMORY;
        } else {
            enum mo_host_outcome result =
                output(host, args[1],
                       operation == MO_HOST_CONSOLE_PRINT ? host->output : host->errors, error);
            if (result != MO_HOST_RETURNED)
                return result;
        }
        *out = mo_cell_unit();
        return MO_HOST_RETURNED;
    }
    if (operation == MO_HOST_CONSOLE_PRINTED || operation == MO_HOST_CONSOLE_PRINTED_ERRORS) {
        if (!fake)
            return mo_host_bug(error, "console observation needs a fake");
        *out = mo_value_dup(
            heap, type, mo_state_get(info->state, operation == MO_HOST_CONSOLE_PRINTED ? 1 : 2));
        return MO_HOST_RETURNED;
    }
    if (operation == MO_HOST_CONSOLE_READ_LINE && fake) {
        uint32_t index = (uint32_t)mo_cell_get_int(mo_state_get(info->state, 3));
        struct mo_cell line;
        bool found = mo_list_get(mo_state_get(info->state, 0), index, &line);
        if (!mo_host_case(host, heap, type, found ? "Some" : "None", found ? &line : NULL,
                          found ? 1 : 0, out))
            return MO_HOST_OUT_OF_MEMORY;
        if (found)
            mo_state_set(heap, info->state, 3, mo_cell_int((int64_t)index + 1));
        return MO_HOST_RETURNED;
    }
    MO_ASSERT(operation == MO_HOST_CONSOLE_READ_LINE && !fake);
    return input(host, heap, type, info, out, error);
}
