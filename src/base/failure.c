#include "base/failure.h"
#include "base/limits.h"
#include "base/utf8.h"
#include <string.h>

bool mo_failure_text(struct mo_buffer *out, const void *bytes, size_t length) {
    return length <= UINT32_MAX && mo_binary_write(out, length, 4) &&
           mo_buffer_append(out, bytes, length);
}

static bool integer(struct mo_binary *in, unsigned width, uint64_t maximum, uint64_t *out) {
    return mo_binary_uint(in, width, out) && *out <= maximum;
}
static bool slice(struct mo_binary *in, size_t maximum, bool text, struct mo_failure_slice *out) {
    uint64_t length;
    const uint8_t *bytes;
    if (!integer(in, 4, maximum, &length) || !mo_binary_read(in, (size_t)length, &bytes) ||
        (text &&
         (!mo_utf8_valid(bytes, (size_t)length) || memchr(bytes, 0, (size_t)length) != NULL)))
        return false;
    *out = (struct mo_failure_slice){bytes, (size_t)length};
    return true;
}

/* One argument: a length and an encoded value, exactly as a log record holds it. */
static bool argument(struct mo_binary *in, struct mo_failure_slice *out) {
    struct mo_failure_slice value;
    if (!slice(in, MO_LIMIT_FAILURE_MESSAGE_BYTES, false, &value) || value.length < 9 ||
        memcmp(value.bytes, "MOSV\1", 5) != 0)
        return false;
    *out = value;
    return true;
}

bool mo_failure_read(const uint8_t *bytes, size_t length, struct mo_failure_record *out) {
    if (length > MO_LIMIT_FAILURE_MESSAGE_BYTES || length == 0 || bytes[0] != MO_FAILURE_VERSION)
        return false;
    struct mo_binary in = {bytes, length, MO_FAILURE_ID_OFFSET};
    struct mo_failure_record r = {0};
    uint64_t timer, handler, readonly, count, version, class, function, file, start, end, status;
    if (!integer(&in, MO_FAILURE_ID_BYTES, INT64_MAX, &r.id) || r.id == 0 ||
        !integer(&in, 8, INT64_MAX, &r.sequence) || !mo_binary_uint(&in, 8, &r.arrival) ||
        !integer(&in, 1, 1, &timer) || !mo_binary_uint(&in, 4, &handler) ||
        !slice(&in, MO_LIMIT_FAILURE_KIND_BYTES, true, &r.name) || r.name.length == 0 ||
        !integer(&in, 1, 1, &readonly) || (timer != 0 && readonly != 0) ||
        !integer(&in, 4, MO_LIMIT_BYTECODE_PARAMETERS - 2, &count))
        return false;
    size_t arguments = in.position;
    for (uint64_t i = 0; i < count; i++) {
        struct mo_failure_slice value;
        if (!argument(&in, &value))
            return false;
    }
    int64_t arrival;
    memcpy(&arrival, &r.arrival, sizeof arrival);
    if (arrival < MO_TIME_MIN_US || arrival > MO_TIME_MAX_US || (timer != 0 && count != 0))
        return false;
    r.arguments = (struct mo_failure_slice){bytes + arguments, in.position - arguments};
    const uint8_t *build, *schema;
    if (!integer(&in, 1, 1, &version) || version != 1 || !integer(&in, 1, 3, &class) ||
        !slice(&in, MO_LIMIT_FAILURE_KIND_BYTES, true, &r.reason) ||
        !slice(&in, MO_LIMIT_FAILURE_KIND_BYTES, true, &r.limit) ||
        !mo_binary_uint(&in, 8, &r.limit_value) || !mo_binary_uint(&in, 4, &function) ||
        !mo_binary_uint(&in, 4, &file) || !mo_binary_uint(&in, 4, &start) ||
        !mo_binary_uint(&in, 4, &end) || start > end || !mo_binary_uint(&in, 8, &r.work) ||
        !mo_binary_uint(&in, 8, &r.heap) || !mo_binary_uint(&in, 8, &r.stack) ||
        !mo_binary_uint(&in, 8, &r.depth) || !mo_binary_uint(&in, 8, &r.saved) ||
        !mo_binary_uint(&in, 8, &r.program) || !mo_binary_read(&in, 32, &build) ||
        !slice(&in, MO_LIMIT_EXPORT_LABEL_BYTES, true, &r.label) ||
        !mo_binary_read(&in, 32, &schema) ||
        !integer(&in, 1, MO_FAILURE_CONTEXT_UNAVAILABLE, &status) ||
        !slice(&in, MO_LIMIT_FAILURE_CONTEXT_BYTES, true, &r.context) || in.position != length)
        return false;
    r.timer = (uint8_t)timer;
    r.read_only = (uint8_t)readonly;
    r.handler = (uint32_t)handler;
    r.argument_count = (uint32_t)count;
    r.signature_version = (uint8_t)version;
    r.class = (uint8_t) class;
    r.function = (uint32_t)function;
    r.file = (uint32_t)file;
    r.start = (uint32_t)start;
    r.end = (uint32_t)end;
    r.context_status = (uint8_t)status;
    memcpy(r.build, build, 32);
    memcpy(r.schema, schema, 32);
    *out = r;
    return true;
}

struct mo_failure_arguments mo_failure_arguments(const struct mo_failure_record *record) {
    return (struct mo_failure_arguments){record->arguments, 0, record->argument_count};
}

enum mo_failure_argument mo_failure_argument_next(struct mo_failure_arguments *walk,
                                                  struct mo_failure_slice *out) {
    MO_ASSERT(walk->position <= walk->bytes.length);
    struct mo_binary in = {walk->bytes.bytes, walk->bytes.length, walk->position};
    if (walk->remaining == 0)
        return in.position == in.length ? MO_FAILURE_ARGUMENTS_END : MO_FAILURE_ARGUMENTS_DAMAGED;
    if (!argument(&in, out))
        return MO_FAILURE_ARGUMENTS_DAMAGED;
    walk->position = in.position;
    walk->remaining--;
    return MO_FAILURE_ARGUMENT;
}
