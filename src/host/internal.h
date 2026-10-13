#ifndef MO_HOST_INTERNAL_H
#define MO_HOST_INTERNAL_H

#include "host/host.h"

typedef enum mo_host_outcome (*mo_host_operation)(struct mo_host *, struct mo_heap *, uint32_t,
                                                  uint32_t, const struct mo_cell *,
                                                  struct mo_cell *, struct mo_host_error *);
struct mo_host_operations {
    enum mo_capability capability;
    bool fake;
    mo_host_operation call;
};
extern const struct mo_host_operations mo_host_operations[2][MO_CAP_RANDOM + 1];
MO_MUST_USE enum mo_host_outcome mo_host_bug(struct mo_host_error *error, const char *reason);
MO_MUST_USE uint32_t mo_host_tag(const struct mo_host *host, uint32_t type, const char *name);
MO_MUST_USE bool mo_host_case(struct mo_host *host, struct mo_heap *heap, uint32_t type,
                              const char *name, const struct mo_cell *fields, uint32_t count,
                              struct mo_cell *out);
MO_MUST_USE enum mo_host_outcome mo_host_console(struct mo_host *host, struct mo_heap *heap,
                                                 uint32_t operation, uint32_t type,
                                                 const struct mo_cell *args, struct mo_cell *out,
                                                 struct mo_host_error *error);
MO_MUST_USE enum mo_host_outcome mo_host_misc(struct mo_host *host, struct mo_heap *heap,
                                              uint32_t operation, uint32_t type,
                                              const struct mo_cell *args, struct mo_cell *out,
                                              struct mo_host_error *error);
MO_MUST_USE enum mo_host_outcome mo_host_files(struct mo_host *host, struct mo_heap *heap,
                                               uint32_t operation, uint32_t type,
                                               const struct mo_cell *args, struct mo_cell *out,
                                               struct mo_host_error *error);
MO_MUST_USE enum mo_host_outcome mo_host_console_fake(struct mo_host *host, struct mo_heap *heap,
                                                      uint32_t type, const struct mo_cell *input,
                                                      struct mo_cell *out);
MO_MUST_USE enum mo_host_outcome mo_host_console_real(struct mo_heap *heap, uint32_t type,
                                                      struct mo_cell *out);
MO_MUST_USE enum mo_host_outcome mo_host_state_cap(struct mo_host *host, struct mo_heap *heap,
                                                   uint32_t type, bool fake, const uint32_t *types,
                                                   const struct mo_cell *values, uint32_t count,
                                                   struct mo_cell *out);
MO_MUST_USE enum mo_host_outcome mo_host_random_new(struct mo_host *host, struct mo_heap *heap,
                                                    uint32_t type, bool fake, uint64_t seed,
                                                    struct mo_cell *out,
                                                    struct mo_host_error *error);
MO_MUST_USE enum mo_host_outcome mo_host_random(struct mo_host *host, struct mo_heap *heap,
                                                uint32_t type, const struct mo_cell *args,
                                                struct mo_cell *out, struct mo_host_error *error);

#endif
