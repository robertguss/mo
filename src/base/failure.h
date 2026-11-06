#ifndef MO_FAILURE_H
#define MO_FAILURE_H
#include "base/binary.h"

/* The payload is versioned independently of the MOST envelope. Slices borrow
 * the complete input, and are published only after a bounded full parse. */
#define MO_FAILURE_VERSION 1
/* The store binds the record id into these payload bytes after choosing it. */
#define MO_FAILURE_ID_OFFSET 1
#define MO_FAILURE_ID_BYTES 8
enum mo_failure_context {
    MO_FAILURE_CONTEXT_COMPLETE,
    MO_FAILURE_CONTEXT_TRUNCATED,
    MO_FAILURE_CONTEXT_UNAVAILABLE
};
struct mo_failure_slice {
    const uint8_t *bytes;
    size_t length;
};
struct mo_failure_record {
    uint64_t id, sequence, arrival, program, work, heap, stack, depth, saved, limit_value;
    uint32_t handler, function, file, start, end;
    uint8_t timer, read_only, signature_version, class, context_status;
    struct mo_failure_slice name, reason, limit, arguments, label, context;
    uint32_t argument_count;
    uint8_t build[32], schema[32];
};
MO_MUST_USE bool mo_failure_read(const uint8_t *bytes, size_t length,
                                 struct mo_failure_record *out);
MO_MUST_USE bool mo_failure_text(struct mo_buffer *out, const void *bytes, size_t length);
#endif
