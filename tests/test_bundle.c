#include "base/binary.h"
#include "base/bundle.h"
#include "test.h"
#include <string.h>

static void span(struct mo_buffer *b, const char *s) {
    CHECK(mo_binary_write(b, strlen(s), 4));
    CHECK(mo_buffer_append(b, s, strlen(s)));
}
static void fixture(struct mo_buffer *b, const char *entry, const char *first, const char *second) {
    b->length = 0;
    CHECK(mo_buffer_append(b, "MOSB\1", 5));
    span(b, entry);
    CHECK(mo_binary_write(b, second == NULL ? 1 : 2, 4));
    span(b, first);
    span(b, "module Main\n");
    if (second != NULL) {
        span(b, second);
        span(b, "");
    }
}
int main(void) {
    struct mo_buffer b;
    mo_buffer_init(&b, MO_LIMIT_BUNDLE_BYTES + 1);
    struct mo_bundle out = {.count = 99};
    fixture(&b, "Main.mo", "Main.mo", NULL);
    CHECK(mo_bundle_read(b.bytes, b.length, &out));
    CHECK(out.count == 1 && out.entry.length == 7 && out.files[0].source.length == 12);
    for (size_t n = 0; n < b.length; n++) {
        out.count = 99;
        CHECK(!mo_bundle_read(b.bytes, n, &out));
        CHECK(out.count == 99);
    }
    CHECK(mo_buffer_append_byte(&b, 0));
    CHECK(!mo_bundle_read(b.bytes, b.length, &out));
    fixture(&b, "Main.mo", "A.mo", "Main.mo");
    CHECK(mo_bundle_read(b.bytes, b.length, &out));
    fixture(&b, "Main.mo", "Main.mo", "A.mo");
    CHECK(!mo_bundle_read(b.bytes, b.length, &out));
    fixture(&b, "Main.mo", "Main.mo", "Main.mo");
    CHECK(!mo_bundle_read(b.bytes, b.length, &out));
    fixture(&b, "Missing.mo", "Main.mo", NULL);
    CHECK(!mo_bundle_read(b.bytes, b.length, &out));
    const char *bad[] = {"",
                         "/Main.mo",
                         "../Main.mo",
                         "a/../Main.mo",
                         "a//b.mo",
                         "./Main.mo",
                         "a/./b.mo",
                         "a/",
                         "lib/List.mo",
                         "UpgradeOld.mo",
                         "UpgradeOld/State.mo",
                         "a\\b.mo",
                         "\xff.mo"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        fixture(&b, bad[i], bad[i], NULL);
        CHECK(!mo_bundle_read(b.bytes, b.length, &out));
    }
    fixture(&b, "Main.mo", "Main.mo", NULL);
    b.bytes[b.length - 1] = 0xff;
    CHECK(!mo_bundle_read(b.bytes, b.length, &out));
    char path[257];
    memset(path, 'a', sizeof path);
    path[255] = 0;
    fixture(&b, path, path, NULL);
    CHECK(mo_bundle_read(b.bytes, b.length, &out));
    path[255] = 'a';
    path[256] = 0;
    fixture(&b, path, path, NULL);
    CHECK(!mo_bundle_read(b.bytes, b.length, &out));
    b.length = 0;
    CHECK(mo_buffer_append(&b, "MOSB\1", 5));
    span(&b, "00.mo");
    CHECK(mo_binary_write(&b, 64, 4));
    for (unsigned i = 0; i < 64; i++) {
        char filename[6];
        int n = snprintf(filename, sizeof filename, "%02u.mo", i);
        MO_ASSERT(n == 5);
        span(&b, filename);
        span(&b, "");
    }
    CHECK(mo_bundle_read(b.bytes, b.length, &out));
    CHECK(out.count == 64);
    b.bytes[14] = 65;
    CHECK(!mo_bundle_read(b.bytes, b.length, &out));
    fixture(&b, "Main.mo", "Main.mo", NULL);
    /* Exactly one MiB is admissible, one byte more is not. */
    const size_t source_at = 35;
    MO_ASSERT(b.length == source_at + 12);
    b.length = source_at - 4;
    CHECK(mo_binary_write(&b, 1048576 - source_at, 4));
    while (b.length < 1048576)
        CHECK(mo_buffer_append_byte(&b, ' '));
    CHECK(mo_bundle_read(b.bytes, b.length, &out));
    CHECK(mo_buffer_append_byte(&b, ' '));
    CHECK(!mo_bundle_read(b.bytes, b.length, &out));
    mo_buffer_free(&b);
    return mo_test_report("bundle");
}
