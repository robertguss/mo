#define _POSIX_C_SOURCE 200809L
#include "host/internal.h"

#include <errno.h>
#include <signal.h>
#include <unistd.h>

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
    *error = (struct mo_host_error){.reason = "internal error: console input unavailable"};
    return MO_HOST_INTERNAL;
}
