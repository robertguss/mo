#include "base/binary.h"
#include "base/export.h"
#include "base/sha256.h"
#include "test.h"
#include <string.h>

static void seal(struct mo_export_pin *pin) {
    struct mo_sha256 hash;
    mo_sha256_init(&hash);
    mo_sha256_update(&hash, pin->bytes, pin->length - 32);
    mo_sha256_finish(&hash, pin->bytes + pin->length - 32);
}

int main(void) {
    size_t length = 99, count = 99;
    CHECK(mo_export_page_bounds(8193, 0, &length, &count));
    CHECK(length == 8192 && count == 2);
    CHECK(mo_export_page_bounds(8193, 8192, &length, &count));
    CHECK(length == 1 && count == 2);
    CHECK(!mo_export_page_bounds(8193, 1, &length, &count));
    CHECK(!mo_export_page_bounds(8192, 8192, &length, &count));
    CHECK(!mo_export_page_bounds(SIZE_MAX, 0, &length, &count));
    CHECK(!mo_export_page_bounds(1, SIZE_MAX, &length, &count));
    uint8_t saved[] = {'M', 'O', 'S', 'V', 1, 1, 0, 0, 0, 42, 0};
    struct mo_export_pin pin = {0};
    struct mo_export_identity identity = {
        .kind = 1, .sequence = 9, .encoded_sequence = 8, .label = "test", .program = 123};
    identity.id[0] = 1;
    CHECK(mo_export_create(&pin, &identity, saved, sizeof saved, 10));
    CHECK(pin.bytes != NULL && pin.sequence == 9 && pin.encoded_sequence == 8);
    CHECK(memcmp(pin.bytes, "MOEX\1\1", 6) == 0);
    CHECK(pin.arena.used <= MO_LIMIT_EXPORT_BYTES / MO_LIMIT_EXPORT_PINS);
    struct mo_export_identity decoded = {0};
    CHECK(mo_export_verify(pin.bytes, pin.length, &decoded));
    CHECK(decoded.sequence == 9 && decoded.encoded_sequence == 8 && decoded.program == 123);
    CHECK(memcmp(decoded.id, identity.id, 16) == 0);
    for (size_t i = 0; i < pin.length; i++) {
        pin.bytes[i] ^= 1;
        CHECK(!mo_export_verify(pin.bytes, pin.length, &decoded));
        pin.bytes[i] ^= 1;
    }
    CHECK(!mo_export_verify(pin.bytes, pin.length - 1, &decoded));
    /* Keep the outer digest valid so each inner check is reached. */
    const size_t corrupt[] = {4, 62, 66, 98, 130, 134, 139};
    for (size_t i = 0; i < sizeof corrupt / sizeof *corrupt; i++) {
        uint8_t mask = corrupt[i] == 66 ? 0x80 : 0x40;
        pin.bytes[corrupt[i]] ^= mask;
        seal(&pin);
        CHECK(!mo_export_verify(pin.bytes, pin.length, &decoded));
        pin.bytes[corrupt[i]] ^= mask;
        seal(&pin);
        CHECK(mo_export_verify(pin.bytes, pin.length, &decoded));
    }
    struct mo_arena consumed;
    mo_arena_init_confidential(&consumed, MO_LIMIT_EXPORT_BYTES);
    struct mo_export_consumer consumer = {0};
    struct mo_export_page page = {.sequence = 9,
                                  .encoded_sequence = 8,
                                  .total = pin.length,
                                  .length = pin.length,
                                  .count = 1,
                                  .final = true,
                                  .checksum = mo_binary_checksum(pin.bytes, pin.length)};
    memcpy(page.id, pin.id, 16);
    memcpy(page.digest, pin.bytes + pin.length - 32, 32);
    memcpy(page.bytes, pin.bytes, pin.length);
    CHECK(mo_export_accept(&consumed, &consumer, &page));
    CHECK(consumer.complete && consumer.offset == pin.length);
    CHECK(memcmp(consumer.bytes, pin.bytes, pin.length) == 0);
    CHECK(!mo_export_accept(&consumed, &consumer, &page));
    mo_arena_free(&consumed);
    saved[10] = 77;
    CHECK(pin.bytes[pin.length - 33] == 0);
    CHECK(!mo_export_expire(&pin, 10 + MO_LIMIT_EXPORT_IDLE_US - 1));
    CHECK(mo_export_expire(&pin, 10 + MO_LIMIT_EXPORT_IDLE_US));
    CHECK(pin.bytes == NULL && pin.arena.used == 0);
    CHECK(!mo_export_create(&pin, &identity, saved, 8, 0));
    saved[5] = 100;
    CHECK(!mo_export_create(&pin, &identity, saved, sizeof saved, 0));
    mo_export_release(&pin);
    struct mo_arena source;
    mo_arena_init(&source, MO_LIMIT_SAVED_BYTES + MO_ARENA_CHUNK_HEADER);
    uint8_t *large = mo_arena_allocate(&source, MO_LIMIT_SAVED_BYTES, 1);
    MO_ASSERT(large != NULL);
    memset(large, 0, MO_LIMIT_SAVED_BYTES);
    memcpy(large, "MOSV\1\1\0\0\0*", 10);
    CHECK(mo_export_create(&pin, &identity, large, MO_LIMIT_SAVED_BYTES, UINT64_MAX - 1));
    CHECK(pin.arena.used <= MO_LIMIT_EXPORT_BYTES / MO_LIMIT_EXPORT_PINS);
    CHECK(!mo_export_expire(&pin, UINT64_MAX));
    CHECK(!mo_export_create(&pin, &identity, large, (size_t)MO_LIMIT_SAVED_BYTES + 1, 0));
    CHECK(pin.bytes != NULL); /* oversized refusal leaves prior pin intact */
    CHECK(mo_export_page_bounds(pin.length, 0, &length, &count) && count == 2049);
    mo_arena_init_confidential(&consumed, MO_LIMIT_EXPORT_BYTES);
    consumer = (struct mo_export_consumer){0};
    page = (struct mo_export_page){
        .total = pin.length, .length = 8192, .count = count, .sequence = 9, .encoded_sequence = 8};
    memcpy(page.id, pin.id, 16);
    memcpy(page.bytes, pin.bytes, 8192);
    page.checksum = mo_binary_checksum(page.bytes, page.length);
    CHECK(mo_export_accept(&consumed, &consumer, &page));
    page.offset = 8192;
    memcpy(page.bytes, pin.bytes + 8192, 8192);
    page.checksum = mo_binary_checksum(page.bytes, page.length);
    page.id[0] ^= 1;
    CHECK(!mo_export_accept(&consumed, &consumer, &page));
    CHECK(consumer.offset == 8192 && !consumer.complete);
    page.id[0] ^= 1;
    page.encoded_sequence++;
    CHECK(!mo_export_accept(&consumed, &consumer, &page));
    page.encoded_sequence--;
    CHECK(mo_export_accept(&consumed, &consumer, &page));
    CHECK(consumer.offset == 16384 && !consumer.complete);
    mo_arena_free(&consumed);
    mo_export_release(&pin);
    mo_arena_free(&source);
    return mo_test_report("export");
}
