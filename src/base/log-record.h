#ifndef MO_LOG_RECORD_H
#define MO_LOG_RECORD_H
#include "base/binary.h"
#include "base/limits.h"

/* A service log record's payload, inside its MOST frame: the timer flag, the handler's index
 * among the service's tools or timers, the arrival time in microseconds as two's complement,
 * the argument count, and the FNV-1a checksum of the saved state the message produced. Each
 * argument's saved encoding follows, after its length. Integers are little-endian. The runtime
 * writes it and replays it; a reader of a stopped folder walks it. */
#define MO_LOG_RECORD_TIMER_BYTES 1
#define MO_LOG_RECORD_HANDLER_BYTES 4
#define MO_LOG_RECORD_ARRIVAL_BYTES 8
#define MO_LOG_RECORD_COUNT_BYTES 4
#define MO_LOG_RECORD_CHECKSUM_BYTES 8
#define MO_LOG_RECORD_HEADER_BYTES                                                                 \
    (MO_LOG_RECORD_TIMER_BYTES + MO_LOG_RECORD_HANDLER_BYTES + MO_LOG_RECORD_ARRIVAL_BYTES +       \
     MO_LOG_RECORD_COUNT_BYTES + MO_LOG_RECORD_CHECKSUM_BYTES)
#define MO_LOG_RECORD_LENGTH_BYTES 4
/* A tool's arguments, after its state and time. */
#define MO_LOG_RECORD_ARGUMENTS (MO_LIMIT_BYTECODE_PARAMETERS - 2)

struct mo_log_record_header {
    uint64_t state_checksum;
    int64_t arrival;
    uint32_t handler, count;
    bool timer;
};
/* The writer: the header, then each argument's encoding in order. */
MO_MUST_USE bool mo_log_record_header_write(struct mo_buffer *out,
                                            const struct mo_log_record_header *header);
MO_MUST_USE bool mo_log_record_argument_write(struct mo_buffer *out, const uint8_t *bytes,
                                              size_t length);
/* Reads from the input's cursor. A timer flag other than 0 or 1 is refused. A failed read
 * leaves the cursor and output as they were. The argument read borrows the input. */
MO_MUST_USE bool mo_log_record_header_read(struct mo_binary *in, struct mo_log_record_header *out);
MO_MUST_USE bool mo_log_record_argument_read(struct mo_binary *in, const uint8_t **bytes,
                                             size_t *length);

/* A payload's metadata without its argument bytes. */
struct mo_log_record_metadata {
    struct mo_log_record_header header;
    uint32_t argument_bytes[MO_LOG_RECORD_ARGUMENTS];
};
/* Copies count bytes at offset in the payload into out; false if it cannot. */
typedef bool (*mo_log_record_reader)(void *context, size_t offset, uint8_t *out, size_t count);
/* Walks a payload of length bytes through read, which is asked only for the header and each
 * argument's length, all inside the payload. It refuses a truncated header, a count above
 * MO_LOG_RECORD_ARGUMENTS, an argument past the end, and bytes after the last argument. The
 * output is written only on success. */
MO_MUST_USE bool mo_log_record_walk(size_t length, mo_log_record_reader read, void *context,
                                    struct mo_log_record_metadata *out);
#endif
