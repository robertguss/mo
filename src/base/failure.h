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
/* Whether the two slices hold the same bytes; either may be empty with a NULL pointer. */
MO_MUST_USE bool mo_failure_slice_equal(struct mo_failure_slice a, struct mo_failure_slice b);
/* The record's arrival, a signed microsecond time kept in an unsigned field. */
MO_MUST_USE int64_t mo_failure_arrival(const struct mo_failure_record *record);
/* A walk over a record's arguments, in order. It trusts nothing about the record: the count and
 * the bytes must agree, and each argument must be a length-prefixed MOSV value. */
struct mo_failure_arguments {
    struct mo_failure_slice bytes;
    size_t position;
    uint32_t remaining;
};
enum mo_failure_argument {
    MO_FAILURE_ARGUMENT,
    MO_FAILURE_ARGUMENTS_END,
    MO_FAILURE_ARGUMENTS_DAMAGED
};
MO_MUST_USE struct mo_failure_arguments
mo_failure_arguments(const struct mo_failure_record *record);
/* ARGUMENT sets out to the next encoded value and advances. END means the count and the bytes
 * ran out together. DAMAGED means they disagree or an argument is malformed. Both leave out
 * and the walk unchanged. */
MO_MUST_USE enum mo_failure_argument mo_failure_argument_next(struct mo_failure_arguments *walk,
                                                              struct mo_failure_slice *out);
MO_MUST_USE bool mo_failure_text(struct mo_buffer *out, const void *bytes, size_t length);
#endif
