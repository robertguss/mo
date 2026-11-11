#include "base/binary.h"
#include "base/compile-context.h"
#include "base/compile-metadata.h"
#include "test.h"
#include <string.h>

static void span(struct mo_buffer *b, const char *s) {
    CHECK(mo_binary_write(b, strlen(s), 4));
    CHECK(mo_buffer_append(b, s, strlen(s)));
}
static void fixture(struct mo_buffer *b, const char *migration, const char *a, const char *z) {
    b->length = 0;
    CHECK(mo_buffer_append(b, "MOCC\1", 5));
    span(b, migration);
    CHECK(mo_binary_write(b, a == NULL ? 0 : (z == NULL ? 1 : 2), 4));
    if (a != NULL) {
        span(b, a);
        span(b, "module UpgradeOld\n");
    }
    if (z != NULL) {
        span(b, z);
        span(b, "");
    }
}
static void metadata_fixture(struct mo_buffer *b, const char *token, const char *path,
                             const char *tool, uint64_t interval) {
    b->length = 0;
    CHECK(mo_buffer_append(b, "MOCS\1", 5));
    span(b, token);
    span(b, "schema");
    CHECK(mo_binary_write(b, 1, 4));
    span(b, path);
    span(b, "module UpgradeOld\n");
    CHECK(mo_binary_write(b, 1, 4));
    span(b, tool);
    static const uint8_t digest[32] = {1};
    CHECK(mo_buffer_append(b, digest, sizeof digest));
    CHECK(mo_binary_write(b, 1, 4));
    span(b, "tick");
    CHECK(mo_binary_write(b, interval, 8));
}
static void structural_metadata(void) {
    struct mo_buffer b, context;
    mo_buffer_init(&b, MO_LIMIT_BUNDLE_BYTES);
    mo_buffer_init(&context, MO_LIMIT_BUNDLE_BYTES);
    metadata_fixture(&b, "TOKEN", "UpgradeOld.mo", "read", 1000);
    struct mo_compile_metadata m = {.tool_count = 99};
    CHECK(mo_compile_metadata_read(b.bytes, b.length, &m));
    CHECK(m.type_count == 1 && m.tool_count == 1 && m.timer_count == 1 && m.token.length == 5);
    struct mo_binary in = {m.tools.bytes, m.tools.length, 0};
    struct mo_compile_handler h;
    CHECK(mo_compile_handler_read(&in, false, &h) && in.position == in.length);
    CHECK(h.name.length == 4 && memcmp(h.name.bytes, "read", 4) == 0 && h.digest[0] == 1);
    in = (struct mo_binary){m.timers.bytes, m.timers.length, 0};
    CHECK(mo_compile_handler_read(&in, true, &h) && in.position == in.length && h.interval == 1000);
    for (size_t n = 0; n < b.length; n++) {
        m.tool_count = 99;
        CHECK(!mo_compile_metadata_read(b.bytes, n, &m) && m.tool_count == 99);
    }
    fixture(&context, "", NULL, NULL);
    CHECK(mo_binary_write(&context, b.length, 4));
    CHECK(mo_buffer_append(&context, b.bytes, b.length));
    struct mo_compile_context parsed;
    CHECK(mo_compile_context_read(context.bytes, context.length, &parsed));
    CHECK(parsed.metadata.length == b.length);
    CHECK(mo_buffer_append_byte(&context, 0));
    CHECK(!mo_compile_context_read(context.bytes, context.length, &parsed));
    const char *bad_paths[] = {"../UpgradeOld.mo", "UpgradeOld..mo", "Main.mo"};
    for (size_t i = 0; i < sizeof bad_paths / sizeof *bad_paths; i++) {
        metadata_fixture(&b, "TOKEN", bad_paths[i], "read", 1000);
        CHECK(!mo_compile_metadata_read(b.bytes, b.length, &m));
    }
    metadata_fixture(&b, "", "UpgradeOld.mo", "read", 1000);
    CHECK(!mo_compile_metadata_read(b.bytes, b.length, &m));
    metadata_fixture(&b, "TOKEN", "UpgradeOld.mo", "bad\nname", 1000);
    CHECK(!mo_compile_metadata_read(b.bytes, b.length, &m));
    metadata_fixture(&b, "TOKEN", "UpgradeOld.mo", "read", 0);
    CHECK(!mo_compile_metadata_read(b.bytes, b.length, &m));
    metadata_fixture(&b, "TOKEN", "UpgradeOld.mo", "read", UINT64_MAX);
    CHECK(!mo_compile_metadata_read(b.bytes, b.length, &m));
    mo_buffer_free(&b);
    mo_buffer_free(&context);
}

int main(void) {
    structural_metadata();
    struct mo_buffer b;
    mo_buffer_init(&b, MO_LIMIT_BUNDLE_BYTES + 1);
    struct mo_compile_context out = {.count = 99};
    fixture(&b, "", NULL, NULL);
    CHECK(b.length == 13 && mo_compile_context_read(b.bytes, b.length, &out));
    CHECK(out.count == 0 && out.migration.length == 0);
    fixture(&b, "Main.migrate", NULL, NULL);
    CHECK(!mo_compile_context_read(b.bytes, b.length, &out));
    fixture(&b, "Main.migrate", "UpgradeOld.mo", NULL);
    CHECK(mo_compile_context_read(b.bytes, b.length, &out));
    CHECK(out.count == 1 && out.migration.length == 12 && out.files[0].source.length == 18);
    for (size_t n = 0; n < b.length; n++) {
        out.count = 99;
        CHECK(!mo_compile_context_read(b.bytes, n, &out));
        CHECK(out.count == 99);
    }
    CHECK(mo_buffer_append_byte(&b, 0));
    CHECK(!mo_compile_context_read(b.bytes, b.length, &out));
    fixture(&b, "", "UpgradeOld.A.mo", "UpgradeOld.mo");
    CHECK(mo_compile_context_read(b.bytes, b.length, &out));
    fixture(&b, "", "UpgradeOld.mo", "UpgradeOld.A.mo");
    CHECK(!mo_compile_context_read(b.bytes, b.length, &out));
    fixture(&b, "", "UpgradeOld.mo", "UpgradeOld.mo");
    CHECK(!mo_compile_context_read(b.bytes, b.length, &out));
    const char *bad[] = {"Main.mo",          "UpgradeOld/State.mo", "UpgradeOld..mo",
                         "../UpgradeOld.mo", "/UpgradeOld.mo",      "UpgradeOld\\State.mo",
                         "UpgradeOld",       "UpgradeOld.mo/",      "UpgradeOld.\xff.mo"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        fixture(&b, "", bad[i], NULL);
        CHECK(!mo_compile_context_read(b.bytes, b.length, &out));
    }
    fixture(&b, "\xff", "UpgradeOld.mo", NULL);
    CHECK(!mo_compile_context_read(b.bytes, b.length, &out));
    fixture(&b, "", "UpgradeOld.mo", NULL);
    b.bytes[b.length - 1] = 0;
    CHECK(!mo_compile_context_read(b.bytes, b.length, &out));
    fixture(&b, "", "UpgradeOld.mo", NULL);
    b.bytes[b.length - 1] = 0xff;
    CHECK(!mo_compile_context_read(b.bytes, b.length, &out));
    /* Pin literal admission boundaries independently of the macros. */
    b.length = 0;
    CHECK(mo_buffer_append(&b, "MOCC\1", 5));
    span(&b, "");
    CHECK(mo_binary_write(&b, 64, 4));
    for (unsigned i = 0; i < 64; i++) {
        char path[32];
        int n = snprintf(path, sizeof path, "UpgradeOld.T%02u.mo", i);
        CHECK(n > 0 && (size_t)n < sizeof path);
        span(&b, path);
        span(&b, "");
    }
    CHECK(mo_compile_context_read(b.bytes, b.length, &out) && out.count == 64);
    b.bytes[9] = 65;
    CHECK(!mo_compile_context_read(b.bytes, b.length, &out));
    fixture(&b, "", "UpgradeOld.mo", NULL);
    const size_t source_at = b.length - 18;
    b.length = source_at - 4;
    CHECK(mo_binary_write(&b, 1048576 - source_at, 4));
    while (b.length < 1048576)
        CHECK(mo_buffer_append_byte(&b, ' '));
    CHECK(mo_compile_context_read(b.bytes, b.length, &out));
    CHECK(mo_buffer_append_byte(&b, ' '));
    CHECK(!mo_compile_context_read(b.bytes, b.length, &out));
    mo_buffer_free(&b);
    return mo_test_report("compile context");
}
