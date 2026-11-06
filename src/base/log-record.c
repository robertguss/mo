#include "base/log-record.h"

#include <string.h>

bool mo_log_record_header_write(struct mo_buffer *out, const struct mo_log_record_header *h) {
    return mo_binary_write(out, h->timer ? 1 : 0, MO_LOG_RECORD_TIMER_BYTES) &&
           mo_binary_write(out, h->handler, MO_LOG_RECORD_HANDLER_BYTES) &&
           mo_binary_write(out, (uint64_t)h->arrival, MO_LOG_RECORD_ARRIVAL_BYTES) &&
           mo_binary_write(out, h->count, MO_LOG_RECORD_COUNT_BYTES) &&
           mo_binary_write(out, h->state_checksum, MO_LOG_RECORD_CHECKSUM_BYTES);
}

bool mo_log_record_argument_write(struct mo_buffer *out, const uint8_t *bytes, size_t length) {
    MO_ASSERT(length <= UINT32_MAX);
    return mo_binary_write(out, length, MO_LOG_RECORD_LENGTH_BYTES) &&
           mo_buffer_append(out, bytes, length);
}

bool mo_log_record_header_read(struct mo_binary *in, struct mo_log_record_header *out) {
    struct mo_binary r = *in;
    uint64_t timer, handler, arrival, count, checksum;
    if (!mo_binary_uint(&r, MO_LOG_RECORD_TIMER_BYTES, &timer) || timer > 1 ||
        !mo_binary_uint(&r, MO_LOG_RECORD_HANDLER_BYTES, &handler) ||
        !mo_binary_uint(&r, MO_LOG_RECORD_ARRIVAL_BYTES, &arrival) ||
        !mo_binary_uint(&r, MO_LOG_RECORD_COUNT_BYTES, &count) ||
        !mo_binary_uint(&r, MO_LOG_RECORD_CHECKSUM_BYTES, &checksum))
        return false;
    *out = (struct mo_log_record_header){.state_checksum = checksum,
                                         .handler = (uint32_t)handler,
                                         .count = (uint32_t)count,
                                         .timer = timer != 0};
    memcpy(&out->arrival, &arrival, sizeof out->arrival);
    *in = r;
    return true;
}

bool mo_log_record_argument_read(struct mo_binary *in, const uint8_t **bytes, size_t *length) {
    struct mo_binary r = *in;
    uint64_t n;
    if (!mo_binary_uint(&r, MO_LOG_RECORD_LENGTH_BYTES, &n) ||
        !mo_binary_read(&r, (size_t)n, bytes))
        return false;
    *length = (size_t)n;
    *in = r;
    return true;
}

bool mo_log_record_walk(size_t length, mo_log_record_reader read, void *context,
                        struct mo_log_record_metadata *out) {
    uint8_t header[MO_LOG_RECORD_HEADER_BYTES];
    if (length < MO_LOG_RECORD_HEADER_BYTES || !read(context, 0, header, sizeof header))
        return false;
    struct mo_binary in = {header, sizeof header, 0};
    struct mo_log_record_header h;
    if (!mo_log_record_header_read(&in, &h) || h.count > MO_LOG_RECORD_ARGUMENTS)
        return false;
    uint32_t sizes[MO_LOG_RECORD_ARGUMENTS];
    size_t offset = MO_LOG_RECORD_HEADER_BYTES;
    for (uint32_t i = 0; i < h.count; i++) {
        uint8_t prefix[MO_LOG_RECORD_LENGTH_BYTES];
        if (length - offset < sizeof prefix || !read(context, offset, prefix, sizeof prefix))
            return false;
        offset += sizeof prefix;
        struct mo_binary p = {prefix, sizeof prefix, 0};
        uint64_t n;
        MO_ASSERT(mo_binary_uint(&p, MO_LOG_RECORD_LENGTH_BYTES, &n));
        if (n > length - offset)
            return false;
        offset += (size_t)n;
        sizes[i] = (uint32_t)n;
    }
    if (offset != length)
        return false;
    out->header = h;
    memcpy(out->argument_bytes, sizes, h.count * sizeof sizes[0]);
    return true;
}
