#include "base/log-record.h"

#include <string.h>

struct span {
    const uint8_t *bytes;
    size_t length;
};
static bool read_span(void *context, size_t offset, uint8_t *out, size_t count) {
    const struct span *s = context;
    MO_ASSERT(offset <= s->length && count <= s->length - offset);
    memcpy(out, s->bytes + offset, count);
    return true;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    struct span s = {data, size};
    struct mo_log_record_metadata m;
    bool walked = mo_log_record_walk(size, read_span, &s, &m);
    /* The walk accepts exactly what the runtime's own reads accept, and the writer rebuilds
     * the input from what they return. */
    struct mo_binary in = {data, size, 0};
    struct mo_log_record_header h;
    bool read = mo_log_record_header_read(&in, &h) && h.count <= MO_LOG_RECORD_ARGUMENTS;
    struct mo_buffer out;
    mo_buffer_init(&out, size + MO_LOG_RECORD_HEADER_BYTES);
    read = read && mo_log_record_header_write(&out, &h);
    for (uint32_t i = 0; read && i < h.count; i++) {
        const uint8_t *bytes;
        size_t length;
        read = mo_log_record_argument_read(&in, &bytes, &length) &&
               (!walked || m.argument_bytes[i] == length);
        MO_ASSERT(!read || mo_log_record_argument_write(&out, bytes, length));
    }
    read = read && in.position == size;
    MO_ASSERT(walked == read);
    if (walked) {
        MO_ASSERT(m.header.count == h.count && m.header.handler == h.handler &&
                  m.header.arrival == h.arrival && m.header.timer == h.timer &&
                  m.header.state_checksum == h.state_checksum);
        MO_ASSERT(out.length == size && memcmp(out.bytes, data, size) == 0);
    }
    mo_buffer_free(&out);
    return 0;
}
