#ifndef MO_HOST_NETWORK_H
#define MO_HOST_NETWORK_H

#include "host/internal.h"

enum mo_network_status {
    NETWORK_OK,
    NETWORK_TIMED_OUT,
    NETWORK_CLOSED,
    NETWORK_REFUSED,
    NETWORK_UNKNOWN_HOST,
    NETWORK_BROKEN,
    NETWORK_INTERNAL
};
struct mo_network_error {
    enum mo_network_status status;
    int number;
    const char *reason;
};
MO_MUST_USE bool mo_host_named(const uint8_t *name, size_t length);
MO_MUST_USE uint32_t mo_host_network_payload(const struct mo_host *host, uint32_t type,
                                             const char *name);
MO_MUST_USE enum mo_host_outcome mo_host_network_ok(struct mo_host *host, struct mo_heap *heap,
                                                    uint32_t type, struct mo_cell value,
                                                    struct mo_cell *out);
MO_MUST_USE enum mo_host_outcome mo_host_network_error(struct mo_host *host, struct mo_heap *heap,
                                                       uint32_t type, struct mo_network_error error,
                                                       struct mo_cell name, int64_t port,
                                                       struct mo_cell *out,
                                                       struct mo_host_error *stop);
MO_MUST_USE enum mo_host_outcome
mo_host_fake_network_call(struct mo_host *host, struct mo_heap *heap, uint32_t operation,
                          uint32_t type, const struct mo_cell *args, struct mo_cell *out,
                          struct mo_host_error *error);
MO_MUST_USE enum mo_host_outcome
mo_host_real_network_call(struct mo_host *host, struct mo_heap *heap, uint32_t operation,
                          uint32_t type, const struct mo_cell *args, struct mo_cell *out,
                          struct mo_host_error *error);

#endif
