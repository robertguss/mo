#define _POSIX_C_SOURCE 200809L
#include "host/network.h"

#include <string.h>

bool mo_host_named(const uint8_t *name, size_t length) {
    return length != 0 && length <= MO_LIMIT_HOST_NAME && memchr(name, 0, length) == NULL;
}

uint32_t mo_host_network_payload(const struct mo_host *host, uint32_t type, const char *name) {
    return host->types->types.items[type].cases[mo_host_tag(host, type, name)].fields[0].type;
}

enum mo_host_outcome mo_host_network_ok(struct mo_host *host, struct mo_heap *heap, uint32_t type,
                                        struct mo_cell value, struct mo_cell *out) {
    bool ok = mo_host_case(host, heap, type, "Ok", &value, 1, out);
    mo_value_drop(heap, mo_host_network_payload(host, type, "Ok"), value);
    return ok ? MO_HOST_RETURNED : MO_HOST_OUT_OF_MEMORY;
}

enum mo_host_outcome mo_host_network_error(struct mo_host *host, struct mo_heap *heap,
                                           uint32_t type, struct mo_network_error error,
                                           struct mo_cell name, int64_t port, struct mo_cell *out,
                                           struct mo_host_error *stop) {
    static const char *const cases[] = {[NETWORK_TIMED_OUT] = "TimedOut",
                                        [NETWORK_CLOSED] = "Closed",
                                        [NETWORK_REFUSED] = "Refused",
                                        [NETWORK_UNKNOWN_HOST] = "UnknownHost",
                                        [NETWORK_BROKEN] = "Broken"};
    if (error.status == NETWORK_INTERNAL) {
        *stop = (struct mo_host_error){.reason = "internal error: monotonic clock unavailable"};
        return MO_HOST_INTERNAL;
    }
    MO_ASSERT(error.status != NETWORK_OK);
    struct mo_cell fields[] = {name, mo_cell_int(port)};
    uint32_t count = error.status == NETWORK_REFUSED        ? 2
                     : error.status == NETWORK_UNKNOWN_HOST ? 1
                                                            : 0;
    if (error.status == NETWORK_BROKEN) {
        char system[256];
        const char *reason = error.reason;
        if (reason == NULL)
            reason = strerror_r(error.number, system, sizeof system) == 0 ? system : "system error";
        if (!mo_value_text(heap, reason, strlen(reason), &fields[0]))
            return MO_HOST_OUT_OF_MEMORY;
        count = 1;
    }
    uint32_t error_type = mo_host_network_payload(host, type, "Error");
    struct mo_cell value;
    bool built = mo_host_case(host, heap, error_type, cases[error.status], fields, count, &value);
    if (error.status == NETWORK_BROKEN)
        mo_value_drop(heap, host->types->text, fields[0]);
    if (!built)
        return MO_HOST_OUT_OF_MEMORY;
    bool wrapped = mo_host_case(host, heap, type, "Error", &value, 1, out);
    mo_value_drop(heap, error_type, value);
    return wrapped ? MO_HOST_RETURNED : MO_HOST_OUT_OF_MEMORY;
}

enum mo_host_outcome mo_host_network(struct mo_host *host, struct mo_heap *heap, uint32_t operation,
                                     uint32_t type, const struct mo_cell *args, struct mo_cell *out,
                                     struct mo_host_error *error) {
    if (operation == MO_HOST_NETWORK_CONNECT) {
        int64_t port = mo_cell_get_int(args[2]);
        if (port < 1 || port > 65535)
            return mo_host_bug(error, "port outside 1 to 65535");
    }
    if (operation == MO_HOST_CONNECTION_RECEIVE) {
        int64_t most = mo_cell_get_int(args[1]);
        if (most < 1 || most > MO_LIMIT_RECEIVE_BYTES)
            return mo_host_bug(error, "most below 1 or above the receive limit");
    }
    const struct mo_host_operations *operations = mo_capability_get(args[0])->operations;
    if (operations->fake)
        return mo_host_fake_network_call(host, heap, operation, type, args, out, error);
    return mo_host_real_network_call(host, heap, operation, type, args, out, error);
}
