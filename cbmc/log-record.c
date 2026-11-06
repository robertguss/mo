/* Proves the actual length and offset arithmetic of the walk over a service log record's
 * payload, with the header reader it calls. The reader callback copies from the harness's bytes
 * and may fail any read, after scribbling over its output. Writer round trips and longer records
 * are tested and fuzzed. */
#include "base/log-record.c"
#include "base/binary.c"
#include <stdlib.h>

size_t nondet_size(void);
uint8_t nondet_byte(void);
bool nondet_bool(void);
uint32_t nondet_u32(void);
uint64_t nondet_u64(void);
int64_t nondet_i64(void);
void reached_end(void) {
}
_Noreturn void mo_assert_failed(const char *expression, const char *file, int line) {
    (void)expression;
    (void)file;
    (void)line;
    __CPROVER_assert(0, "MO_ASSERT holds");
    abort();
}
/* A byte loop: CBMC's own memcpy, with the walk's symbolic size, flattens the whole metadata. */
void *memcpy(void *destination, const void *source, size_t bytes) {
    __CPROVER_assert(__CPROVER_r_ok(source, bytes) && __CPROVER_w_ok(destination, bytes),
                     "memcpy stays inside its buffers");
    uint8_t *to = destination;
    const uint8_t *from = source;
    for (size_t i = 0; i < bytes; i++)
        to[i] = from[i];
    return destination;
}

/* The reads the oracle expects, in order: the header at 0, then each argument's length. */
#define MAX_READS (MO_CBMC_INPUT / 4 + 2)
struct payload {
    const uint8_t *bytes;
    size_t length, reads, planned;
    size_t offsets[MAX_READS], counts[MAX_READS];
    bool failed;
};

static bool read_payload(void *context, size_t offset, uint8_t *out, size_t count) {
    struct payload *p = context;
    __CPROVER_assert(offset <= p->length && count <= p->length - offset,
                     "every read is inside the payload");
    __CPROVER_assert(p->reads < p->planned && offset == p->offsets[p->reads] &&
                         count == p->counts[p->reads],
                     "only the header and each argument's length are read, in order");
    p->reads++;
    if (nondet_bool()) {
        for (size_t i = 0; i < count; i++)
            out[i] = nondet_byte();
        p->failed = true;
        return false;
    }
    for (size_t i = 0; i < count; i++)
        out[i] = p->bytes[offset + i];
    return true;
}

static uint64_t le(const uint8_t *bytes, unsigned width) {
    uint64_t value = 0;
    for (unsigned i = 0; i < width; i++)
        value |= (uint64_t)bytes[i] << (8 * i);
    return value;
}

static uint64_t arrival_bits(int64_t arrival) {
    uint64_t bits;
    memcpy(&bits, &arrival, sizeof bits);
    return bits;
}

void harness_mo_log_record_walk(void) {
    uint8_t bytes[MO_CBMC_INPUT];
    for (size_t i = 0; i < sizeof bytes; i++)
        bytes[i] = nondet_byte();
    size_t length = nondet_size();
    __CPROVER_assume(length <= sizeof bytes);

    /* The oracle, from the layout's fixed offsets in 64-bit sums: a 25-byte header with the
     * timer flag at 0 and the count at 13, then count arguments, each a 4-byte length and that
     * many bytes, ending exactly at the payload's end. */
    struct payload p = {.bytes = bytes, .length = length};
    uint64_t sizes[MO_CBMC_INPUT / 4 + 1];
    bool expected = length >= 25;
    uint64_t count = 0, at = 25;
    if (expected) {
        p.offsets[0] = 0;
        p.counts[0] = 25;
        p.planned = 1;
        count = le(bytes + 13, 4);
        expected = bytes[0] <= 1 && count <= MO_LOG_RECORD_ARGUMENTS;
    }
    for (uint64_t i = 0; expected && i < count; i++) {
        if (at + 4 > length) {
            expected = false;
            break;
        }
        p.offsets[p.planned] = (size_t)at;
        p.counts[p.planned] = 4;
        p.planned++;
        sizes[i] = le(bytes + at, 4);
        if (at + 4 + sizes[i] > length) {
            expected = false;
            break;
        }
        at += 4 + sizes[i];
    }
    expected = expected && at == length;

    struct mo_log_record_metadata out;
    out.header = (struct mo_log_record_header){.state_checksum = nondet_u64(),
                                               .arrival = nondet_i64(),
                                               .handler = nondet_u32(),
                                               .count = nondet_u32(),
                                               .timer = nondet_bool()};
    for (size_t i = 0; i < MO_LOG_RECORD_ARGUMENTS; i++)
        out.argument_bytes[i] = nondet_u32();
    struct mo_log_record_metadata before = out;

    bool ok = mo_log_record_walk(length, read_payload, &p, &out);
    __CPROVER_assert(ok == (expected && !p.failed),
                     "acceptance is exactly the layout's conditions and every read succeeding");
    if (ok) {
        __CPROVER_assert(p.reads == p.planned, "every argument's length is read");
        __CPROVER_assert(
            out.header.timer == (bytes[0] == 1) && out.header.handler == le(bytes + 1, 4) &&
                arrival_bits(out.header.arrival) == le(bytes + 5, 8) && out.header.count == count &&
                out.header.state_checksum == le(bytes + 17, 8),
            "the header is read from its offsets");
        for (uint64_t i = 0; i < count; i++)
            __CPROVER_assert(out.argument_bytes[i] == sizes[i],
                             "each argument's size is its length prefix");
    } else {
        __CPROVER_assert(out.header.state_checksum == before.header.state_checksum &&
                             out.header.arrival == before.header.arrival &&
                             out.header.handler == before.header.handler &&
                             out.header.count == before.header.count &&
                             out.header.timer == before.header.timer,
                         "refusal preserves the header");
        for (size_t i = 0; i < MO_LOG_RECORD_ARGUMENTS; i++)
            __CPROVER_assert(out.argument_bytes[i] == before.argument_bytes[i],
                             "refusal preserves the sizes");
    }
    reached_end();
}
