/* Capabilities own heap storage; the caller owns their execution context. */
#ifndef MO_HOST_H
#define MO_HOST_H

#include "base/hostops.h"
#include "heap/heap.h"

#define MO_HOST_PRIVATE_TYPES 6
struct mo_host_types {
    struct mo_types types;
    uint32_t integer, time, text, texts, folders, files;
    uint32_t capabilities[MO_CAP_RANDOM + 1];
    uint32_t parameters[2];
};
/* Once per image. storage has source->count + MO_HOST_PRIVATE_TYPES entries;
 * it and out must remain at these addresses for the lifetime of every heap.
 * Source indices and layouts are preserved. The VM charges this storage to
 * its initialization budget. The resulting table is read-only. */
void mo_host_prepare_types(struct mo_host_types *out, const struct mo_types *source,
                           struct mo_type *storage);

struct mo_host {
    const struct mo_host_types *types;
    uint32_t open_folders;
    int input, output, errors;
    bool force_fallback; /* C tests exercise the portable resolver on Linux. */
};
void mo_host_init(struct mo_host *host, const struct mo_host_types *types);

enum mo_host_outcome { MO_HOST_RETURNED, MO_HOST_OUT_OF_MEMORY, MO_HOST_BUG, MO_HOST_INTERNAL };
struct mo_host_error {
    const char *reason;
    bool has_detail;
    struct mo_cell detail; /* borrowed; retained arguments outlive the report */
};
/* Verified arguments. Only success writes an owned result and consumes the
 * owned arguments of hostops.def. Refusal preserves cells and ownership.
 * External I/O is not rolled back, including partial writes or folder creation.
 * The host context outlives its capabilities, including heap teardown. */
MO_MUST_USE enum mo_host_outcome mo_host_call(struct mo_host *host, struct mo_heap *heap,
                                              uint32_t operation, uint32_t result_type,
                                              const struct mo_cell *arguments, struct mo_cell *out,
                                              struct mo_host_error *error);
/* Runtime construction for main. contents is List(Text) for Args, Map(Text,
 * Text) for Env, otherwise NULL. folder is an open borrowed descriptor for
 * Files/ReadFiles. Success duplicates contents/descriptors. */
MO_MUST_USE enum mo_host_outcome mo_host_real(struct mo_host *host, struct mo_heap *heap,
                                              uint32_t type, const struct mo_cell *contents,
                                              int folder, struct mo_cell *out,
                                              struct mo_host_error *error);

#endif
