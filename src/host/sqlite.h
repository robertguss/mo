#ifndef MO_HOST_SQLITE_H
#define MO_HOST_SQLITE_H

#include "host/internal.h"

MO_MUST_USE enum mo_host_outcome mo_host_sqlite_real(struct mo_host *host, struct mo_heap *heap,
                                                     uint32_t type, int folder, struct mo_cell *out,
                                                     struct mo_host_error *error);
MO_MUST_USE enum mo_host_outcome mo_host_sqlite_fake_new(struct mo_host *host, struct mo_heap *heap,
                                                         uint32_t type, struct mo_cell databases,
                                                         struct mo_cell *out,
                                                         struct mo_host_error *error);
MO_MUST_USE bool mo_host_sqlite_snapshot(struct mo_host *host, struct mo_heap *heap,
                                         struct mo_cell capability, struct mo_cell *out);
MO_MUST_USE enum mo_host_outcome
mo_host_sqlite_call(struct mo_host *host, struct mo_heap *heap, uint32_t operation, uint32_t type,
                    const struct mo_cell *args, struct mo_cell *out, struct mo_host_error *error);

#endif
