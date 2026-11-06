#include "base/log-record.h"
#include "test.h"

#include <string.h>

/* The layout written out by hand: a tool message (timer 0) to handler 2, arriving at -2 us,
 * with two arguments, "ab" and an empty one, after a state whose checksum is 0x0102...08. */
static const uint8_t golden[] = {
    0,                                              /* timer */
    2,    0,    0,    0,                            /* handler */
    0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* arrival */
    2,    0,    0,    0,                            /* count */
    8,    7,    6,    5,    4,    3,    2,    1,    /* state checksum */
    2,    0,    0,    0,    'a',  'b',              /* first argument */
    0,    0,    0,    0,                            /* second argument */
};

struct span {
    const uint8_t *bytes;
    size_t length, requested, calls;
    bool fail;
};
static bool read_span(void *context, size_t offset, uint8_t *out, size_t count) {
    struct span *s = context;
    CHECK(offset <= s->length && count <= s->length - offset);
    s->requested += count;
    s->calls++;
    if (s->fail || offset > s->length || count > s->length - offset)
        return false;
    memcpy(out, s->bytes + offset, count);
    return true;
}

static bool walk(const uint8_t *bytes, size_t length, struct mo_log_record_metadata *out) {
    struct span s = {bytes, length, 0, 0, false};
    return mo_log_record_walk(length, read_span, &s, out);
}

static void writer(void) {
    CHECK(MO_LOG_RECORD_HEADER_BYTES == 25);
    struct mo_buffer out;
    mo_buffer_init(&out, 1024);
    struct mo_log_record_header h = {
        .state_checksum = UINT64_C(0x0102030405060708), .arrival = -2, .handler = 2, .count = 2};
    CHECK(mo_log_record_header_write(&out, &h));
    CHECK(mo_log_record_argument_write(&out, (const uint8_t *)"ab", 2));
    CHECK(mo_log_record_argument_write(&out, NULL, 0));
    CHECK(out.length == sizeof golden && memcmp(out.bytes, golden, sizeof golden) == 0);
    mo_buffer_clear(&out);
    h = (struct mo_log_record_header){.arrival = 1, .handler = 7, .timer = true};
    CHECK(mo_log_record_header_write(&out, &h));
    static const uint8_t timer[] = {1, 7, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
                                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(out.length == sizeof timer && memcmp(out.bytes, timer, sizeof timer) == 0);
    mo_buffer_free(&out);
}

static void reads(void) {
    struct mo_binary in = {golden, sizeof golden, 0};
    struct mo_log_record_header h;
    CHECK(mo_log_record_header_read(&in, &h));
    CHECK(!h.timer && h.handler == 2 && h.arrival == -2 && h.count == 2 &&
          h.state_checksum == UINT64_C(0x0102030405060708) && in.position == 25);
    const uint8_t *bytes = NULL;
    size_t length = 99;
    CHECK(mo_log_record_argument_read(&in, &bytes, &length));
    CHECK(length == 2 && memcmp(bytes, "ab", 2) == 0 && in.position == 31);
    CHECK(mo_log_record_argument_read(&in, &bytes, &length) && length == 0);
    CHECK(in.position == sizeof golden);
    CHECK(!mo_log_record_argument_read(&in, &bytes, &length) && in.position == sizeof golden);

    uint8_t bad[sizeof golden];
    memcpy(bad, golden, sizeof bad);
    bad[0] = 2;
    in = (struct mo_binary){bad, sizeof bad, 0};
    CHECK(!mo_log_record_header_read(&in, &h) && in.position == 0);
    in = (struct mo_binary){golden, 24, 0};
    CHECK(!mo_log_record_header_read(&in, &h) && in.position == 0);
    /* An argument longer than what is left is refused and leaves the cursor. */
    in = (struct mo_binary){golden, 30, 25};
    CHECK(!mo_log_record_argument_read(&in, &bytes, &length) && in.position == 25);
}

static void walks(void) {
    struct mo_log_record_metadata m;
    memset(&m, 0xaa, sizeof m);
    struct span s = {golden, sizeof golden, 0, 0, false};
    CHECK(mo_log_record_walk(sizeof golden, read_span, &s, &m));
    CHECK(!m.header.timer && m.header.handler == 2 && m.header.arrival == -2 &&
          m.header.count == 2 && m.header.state_checksum == UINT64_C(0x0102030405060708));
    CHECK(m.argument_bytes[0] == 2 && m.argument_bytes[1] == 0);
    /* Only the header and the two length prefixes are read, never the argument bytes. */
    CHECK(s.requested == 25 + 2 * 4 && s.calls == 3);

    struct mo_log_record_metadata untouched;
    memset(&untouched, 0x5c, sizeof untouched);
    struct mo_log_record_metadata out = untouched;
    uint8_t bytes[64];
    /* A truncated header, and a header that ends where its first argument should start. */
    CHECK(!walk(golden, 24, &out));
    CHECK(!walk(golden, 25, &out));
    /* The first argument's length runs past the end. */
    CHECK(!walk(golden, 30, &out));
    /* Trailing bytes after the last argument. */
    memcpy(bytes, golden, sizeof golden);
    bytes[sizeof golden] = 0;
    CHECK(!walk(bytes, sizeof golden + 1, &out));
    /* A count that disagrees with the arguments: three claimed, two present. */
    memcpy(bytes, golden, sizeof golden);
    bytes[13] = 3;
    CHECK(!walk(bytes, sizeof golden, &out));
    /* One claimed, two present: the second is trailing bytes. */
    bytes[13] = 1;
    CHECK(!walk(bytes, sizeof golden, &out));
    /* More arguments than any tool takes, even with the bytes for them. */
    static uint8_t many[25 + 4 * (MO_LOG_RECORD_ARGUMENTS + 1)];
    memcpy(many, golden, 25);
    many[13] = (uint8_t)(MO_LOG_RECORD_ARGUMENTS + 1);
    many[14] = (uint8_t)((MO_LOG_RECORD_ARGUMENTS + 1) >> 8);
    CHECK(!walk(many, sizeof many, &out));
    many[13] = (uint8_t)MO_LOG_RECORD_ARGUMENTS;
    many[14] = (uint8_t)(MO_LOG_RECORD_ARGUMENTS >> 8);
    CHECK(walk(many, sizeof many - 4, &out) && out.header.count == MO_LOG_RECORD_ARGUMENTS);
    out = untouched;
    /* A bad timer flag. */
    memcpy(bytes, golden, sizeof golden);
    bytes[0] = 2;
    CHECK(!walk(bytes, sizeof golden, &out));
    /* A reader that cannot read. */
    s = (struct span){golden, sizeof golden, 0, 0, true};
    CHECK(!mo_log_record_walk(sizeof golden, read_span, &s, &out));
    CHECK(memcmp(&out, &untouched, sizeof out) == 0);
}

/* Writer then walk: every count up to a few arguments, and argument lengths around a prefix. */
static void round_trip(void) {
    struct mo_buffer out;
    mo_buffer_init(&out, 1 << 16);
    static uint8_t data[4096];
    for (size_t i = 0; i < sizeof data; i++)
        data[i] = (uint8_t)(i * 7);
    static const size_t lengths[] = {0, 1, 3, 4, 5, 255, 256, 4096};
    for (uint32_t count = 0; count <= 4; count++) {
        mo_buffer_clear(&out);
        struct mo_log_record_header h = {.state_checksum = count * UINT64_C(0x9e3779b97f4a7c15),
                                         .arrival = INT64_MIN + count,
                                         .handler = UINT32_MAX - count,
                                         .count = count,
                                         .timer = count == 0};
        CHECK(mo_log_record_header_write(&out, &h));
        for (uint32_t i = 0; i < count; i++)
            CHECK(mo_log_record_argument_write(&out, data, lengths[(i * 3 + count) % 8]));
        struct mo_log_record_metadata m;
        CHECK(walk(out.bytes, out.length, &m));
        CHECK(m.header.count == count && m.header.handler == h.handler &&
              m.header.arrival == h.arrival && m.header.timer == h.timer &&
              m.header.state_checksum == h.state_checksum);
        for (uint32_t i = 0; i < count; i++)
            CHECK(m.argument_bytes[i] == lengths[(i * 3 + count) % 8]);
        struct mo_binary in = {out.bytes, out.length, 0};
        struct mo_log_record_header read;
        CHECK(mo_log_record_header_read(&in, &read) && read.count == count);
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *bytes;
            size_t length;
            CHECK(mo_log_record_argument_read(&in, &bytes, &length) &&
                  length == lengths[(i * 3 + count) % 8] && memcmp(bytes, data, length) == 0);
        }
        CHECK(in.position == out.length);
        /* Every shorter prefix of a valid record is refused. */
        for (size_t cut = 0; cut < out.length; cut++)
            CHECK(!walk(out.bytes, cut, &m));
    }
    mo_buffer_free(&out);
}

int main(void) {
    writer();
    reads();
    walks();
    round_trip();
    return mo_test_report("log record");
}
