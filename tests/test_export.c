#include "base/binary.h"
#include "base/export.h"
#include "test.h"
#include <string.h>

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
    mo_export_release(&pin);
    mo_arena_free(&source);
    return mo_test_report("export");
}
