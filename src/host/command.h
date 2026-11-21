#ifndef MO_HOST_COMMAND_H
#define MO_HOST_COMMAND_H

#include "host/internal.h"

MO_MUST_USE enum mo_host_outcome mo_host_command_real(struct mo_host *host, struct mo_heap *heap,
                                                      uint32_t type, int folder,
                                                      struct mo_cell *out,
                                                      struct mo_host_error *error);
MO_MUST_USE enum mo_host_outcome
mo_host_command_call(struct mo_host *host, struct mo_heap *heap, uint32_t operation, uint32_t type,
                     const struct mo_cell *args, struct mo_cell *out, struct mo_host_error *error);

#endif
