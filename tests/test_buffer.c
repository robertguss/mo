#include "base/buffer.h"
#include "test.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Whether the buffer holds exactly the text. */
static bool holds(const struct mo_buffer *buffer, const char *text) {
    size_t length = strlen(text);
    return buffer->length == length && (length == 0 || memcmp(buffer->bytes, text, length) == 0);
}

static void test_it_grows(void) {
    struct mo_buffer buffer;
    mo_buffer_init(&buffer, 1 << 20);
    CHECK(buffer.length == 0);
    for (size_t i = 0; i < 100000; i++) {
        CHECK(mo_buffer_append_byte(&buffer, (uint8_t)(i % 253)));
    }
    CHECK(buffer.length == 100000);
    bool intact = buffer.bytes != NULL;
    for (size_t i = 0; intact && i < 100000; i++) {
        intact = buffer.bytes[i] == (uint8_t)(i % 253);
    }
    CHECK(intact);
    /* Pieces larger than what the buffer holds. */
    uint8_t piece[70000];
    memset(piece, 'x', sizeof piece);
    CHECK(mo_buffer_append(&buffer, piece, sizeof piece));
    CHECK(mo_buffer_append(&buffer, piece, sizeof piece));
    CHECK(buffer.length == 240000);
    CHECK(buffer.bytes != NULL && buffer.bytes[99999] == (uint8_t)(99999 % 253));
    CHECK(buffer.bytes != NULL && buffer.bytes[239999] == 'x');
    /* Appending nothing is fine, even with no bytes. */
    CHECK(mo_buffer_append(&buffer, NULL, 0));
    CHECK(mo_buffer_append_text(&buffer, ""));
    CHECK(buffer.length == 240000);
    mo_buffer_free(&buffer);
    CHECK(buffer.length == 0 && buffer.bytes == NULL);
}

static void test_it_wastes_nothing_as_it_grows(void) {
    /* The buffer's arena may take only the bytes the buffer may hold and one chunk header. Growing
     * by copying would leave old blocks behind and run out of room before the buffer is full. */
    struct mo_buffer buffer;
    size_t limit = (size_t)3 << 20;
    mo_buffer_init(&buffer, limit);
    uint8_t piece[1000];
    memset(piece, 'y', sizeof piece);
    bool appended = true;
    while (appended && buffer.length + sizeof piece <= limit) {
        appended = mo_buffer_append(&buffer, piece, sizeof piece);
    }
    CHECK(appended && buffer.length == limit / 1000 * 1000);
    CHECK(mo_buffer_append(&buffer, piece, limit - buffer.length));
    CHECK(buffer.length == limit);
    mo_buffer_free(&buffer);
}

static void test_it_appends_its_own_bytes(void) {
    /* Each append doubles the text by appending the buffer's own bytes, which move as it grows. */
    struct mo_buffer buffer;
    mo_buffer_init(&buffer, 1 << 20);
    CHECK(mo_buffer_append_text(&buffer, "ab"));
    for (int i = 0; i < 16; i++) {
        CHECK(mo_buffer_append(&buffer, buffer.bytes, buffer.length));
    }
    CHECK(buffer.length == (size_t)2 << 16);
    bool alternating = buffer.bytes != NULL;
    for (size_t i = 0; alternating && i < buffer.length; i++) {
        alternating = buffer.bytes[i] == (i % 2 == 0 ? 'a' : 'b');
    }
    CHECK(alternating);
    /* Part of its own bytes, from the middle. */
    mo_buffer_clear(&buffer);
    CHECK(mo_buffer_append_text(&buffer, "0123456789"));
    CHECK(mo_buffer_append(&buffer, buffer.bytes + 3, 4));
    CHECK(holds(&buffer, "01234567893456"));
    mo_buffer_free(&buffer);
}

static void test_the_limit_holds(void) {
    struct mo_buffer buffer;
    mo_buffer_init(&buffer, 10);
    CHECK(mo_buffer_append_text(&buffer, "abcdefg"));
    /* An append that doesn't fit adds nothing, not even a part of it. */
    CHECK(!mo_buffer_append_text(&buffer, "hijk"));
    CHECK(holds(&buffer, "abcdefg"));
    CHECK(!mo_buffer_append_int(&buffer, -1234));
    CHECK(!mo_buffer_append_uint(&buffer, 1234));
    CHECK(!mo_buffer_append_hex(&buffer, 0, 4));
    CHECK(!mo_buffer_append_le32(&buffer, 1));
    CHECK(holds(&buffer, "abcdefg"));
    CHECK(mo_buffer_append_text(&buffer, "hij"));
    CHECK(holds(&buffer, "abcdefghij"));
    CHECK(!mo_buffer_append_byte(&buffer, 'k'));
    CHECK(mo_buffer_append(&buffer, "k", 0));
    CHECK(holds(&buffer, "abcdefghij"));
    /* Cleared, it holds as much again, in the same memory. */
    uint8_t *bytes = buffer.bytes;
    mo_buffer_clear(&buffer);
    CHECK(buffer.length == 0);
    CHECK(mo_buffer_append_text(&buffer, "0123456789"));
    CHECK(buffer.bytes == bytes && holds(&buffer, "0123456789"));
    mo_buffer_free(&buffer);

    /* A buffer with no room holds nothing. */
    mo_buffer_init(&buffer, 0);
    CHECK(!mo_buffer_append_byte(&buffer, 'a'));
    CHECK(mo_buffer_append(&buffer, NULL, 0));
    CHECK(buffer.length == 0);
    mo_buffer_free(&buffer);
}

static void test_numbers_in_decimal(void) {
    struct mo_buffer buffer;
    mo_buffer_init(&buffer, 1000);
    CHECK(mo_buffer_append_int(&buffer, 0));
    CHECK(mo_buffer_append_text(&buffer, " "));
    CHECK(mo_buffer_append_int(&buffer, 42));
    CHECK(mo_buffer_append_text(&buffer, " "));
    CHECK(mo_buffer_append_int(&buffer, -1));
    CHECK(mo_buffer_append_text(&buffer, " "));
    CHECK(mo_buffer_append_int(&buffer, INT64_MIN));
    CHECK(mo_buffer_append_text(&buffer, " "));
    CHECK(mo_buffer_append_int(&buffer, INT64_MAX));
    CHECK(mo_buffer_append_text(&buffer, " "));
    CHECK(mo_buffer_append_uint(&buffer, 0));
    CHECK(mo_buffer_append_text(&buffer, " "));
    CHECK(mo_buffer_append_uint(&buffer, 1000));
    CHECK(mo_buffer_append_text(&buffer, " "));
    CHECK(mo_buffer_append_uint(&buffer, UINT64_MAX));
    CHECK(holds(&buffer, "0 42 -1 -9223372036854775808 9223372036854775807 0 1000 "
                         "18446744073709551615"));
    mo_buffer_free(&buffer);
}

static void test_numbers_in_hexadecimal(void) {
    struct mo_buffer buffer;
    mo_buffer_init(&buffer, 1000);
    CHECK(mo_buffer_append_hex(&buffer, 0, 0));
    CHECK(mo_buffer_append_text(&buffer, " "));
    CHECK(mo_buffer_append_hex(&buffer, 0, 3));
    CHECK(mo_buffer_append_text(&buffer, " "));
    CHECK(mo_buffer_append_hex(&buffer, 255, 1));
    CHECK(mo_buffer_append_text(&buffer, " "));
    CHECK(mo_buffer_append_hex(&buffer, 255, 4));
    CHECK(mo_buffer_append_text(&buffer, " "));
    CHECK(mo_buffer_append_hex(&buffer, 0x3ff0000000000000u, 16));
    CHECK(mo_buffer_append_text(&buffer, " "));
    CHECK(mo_buffer_append_hex(&buffer, 1, 16));
    CHECK(mo_buffer_append_text(&buffer, " "));
    CHECK(mo_buffer_append_hex(&buffer, UINT64_MAX, 0));
    CHECK(holds(&buffer, "0 000 ff 00ff 3ff0000000000000 0000000000000001 ffffffffffffffff"));
    mo_buffer_free(&buffer);
}

static void test_numbers_in_bytes(void) {
    struct mo_buffer buffer;
    mo_buffer_init(&buffer, 1000);
    CHECK(mo_buffer_append_le32(&buffer, 0x89abcdefu));
    const uint8_t expected[] = {0xef, 0xcd, 0xab, 0x89};
    CHECK(buffer.length == sizeof expected);
    CHECK(buffer.bytes != NULL && memcmp(buffer.bytes, expected, sizeof expected) == 0);
    mo_buffer_free(&buffer);
}

static void test_it_can_be_used_again(void) {
    struct mo_buffer buffer;
    mo_buffer_init(&buffer, 100);
    CHECK(mo_buffer_append_text(&buffer, "first"));
    mo_buffer_free(&buffer);
    CHECK(mo_buffer_append_text(&buffer, "second"));
    CHECK(holds(&buffer, "second"));
    mo_buffer_free(&buffer);
}

int main(void) {
    test_it_grows();
    test_it_wastes_nothing_as_it_grows();
    test_it_appends_its_own_bytes();
    test_the_limit_holds();
    test_numbers_in_decimal();
    test_numbers_in_hexadecimal();
    test_numbers_in_bytes();
    test_it_can_be_used_again();
    return mo_test_report("test_buffer");
}
