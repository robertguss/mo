#include "base/binary.h"
#include "base/export.h"
#include "base/sha256.h"
#include "heap_fixture.h"
#include "host/host.h"
#include "test.h"
#include <string.h>

static const uint8_t no_items[] = {'M', 'O', 'I', 'L', 1, 1, 0, 0, 0, 0};

static void bundle(struct mo_buffer *b, unsigned version, struct mo_store_program *p) {
    b->length = 0;
    CHECK(mo_buffer_append(b, "MOSB\1", 5));
    CHECK(mo_binary_write(b, 7, 4) && mo_buffer_append(b, "Main.mo", 7));
    CHECK(mo_binary_write(b, 1, 4));
    CHECK(mo_binary_write(b, 7, 4) && mo_buffer_append(b, "Main.mo", 7));
    CHECK(mo_binary_write(b, 1, 4) && mo_buffer_append_byte(b, (uint8_t)('a' + version)));
    p->bundle = b->bytes;
    p->bundle_size = b->length;
    struct mo_sha256 hash;
    mo_sha256_init(&hash);
    mo_sha256_update(&hash, b->bytes, b->length);
    mo_sha256_finish(&hash, p->source);
}
static bool snapshot(const struct mo_store_record *record, void *context) {
    struct mo_store_record *expected = context;
    CHECK(record->kind == MO_STORE_STATE && record->image == expected->image &&
          record->sequence == expected->sequence && record->length == expected->length &&
          memcmp(record->bytes, expected->bytes, record->length) == 0);
    return true;
}
int main(void) {
    struct heap_fixture f;
    heap_fixture_init(&f);
    struct mo_limits limits = mo_limits_default();
    struct mo_heap *heap = mo_heap_new(f.image, &limits);
    MO_ASSERT(heap != NULL);
    struct mo_buffer saved, old_bundle, new_bundle;
    mo_buffer_init(&saved, MO_LIMIT_SAVED_BYTES);
    mo_buffer_init(&old_bundle, MO_LIMIT_BUNDLE_BYTES);
    mo_buffer_init(&new_bundle, MO_LIMIT_BUNDLE_BYTES);
    CHECK(mo_value_encode(heap, f.builtin[MO_TYPE_INT], mo_cell_int(42), &saved));
    struct mo_store_program old = {.image = 1}, next = {.image = 2, .ordinal = 1};
    static const uint8_t empty_context[13] = {'M', 'O', 'C', 'C', 1};
    old.context = empty_context;
    old.context_size = sizeof empty_context;
    old.items = no_items;
    old.items_size = sizeof no_items;
    next.context = empty_context;
    next.context_size = sizeof empty_context;
    next.items = no_items;
    next.items_size = sizeof no_items;
    struct mo_sha256 context_hash;
    mo_sha256_init(&context_hash);
    mo_sha256_update(&context_hash, empty_context, sizeof empty_context);
    mo_sha256_finish(&context_hash, old.context_digest);
    memcpy(next.context_digest, old.context_digest, 32);
    bundle(&old_bundle, 0, &old);
    bundle(&new_bundle, 1, &next);
    CHECK(mo_export_schema(saved.bytes, saved.length, old.schema));
    memcpy(next.schema, old.schema, 32);
    struct mo_store_record state = {MO_STORE_STATE, 0, 1, saved.bytes, saved.length};
    struct mo_store *store = NULL;
    CHECK(mo_store_open(NULL, &store) == MO_STORE_OK);
    struct mo_arena copies;
    mo_arena_init(&copies, 1 << 20);
    struct mo_store_program read;
    bool found = true;
    CHECK(mo_store_program_read(store, &copies, &read, &found) == MO_STORE_OK && !found);
    CHECK(mo_store_program_record(store, &old, &state) == MO_STORE_OK);
    CHECK(mo_store_program_read(store, &copies, &read, &found) == MO_STORE_OK && found);
    CHECK(read.image == 1 && read.ordinal == 0 && memcmp(read.source, old.source, 32) == 0);
    CHECK(read.context_size == sizeof empty_context &&
          memcmp(read.context, empty_context, sizeof empty_context) == 0 &&
          memcmp(read.context_digest, old.context_digest, 32) == 0);
    CHECK(mo_store_call(store, MO_STORE_LOAD, NULL, snapshot, &state) == MO_STORE_OK);
    struct mo_store_activation activation = {.previous = old,
                                             .candidate = next,
                                             .previous_state = state,
                                             .candidate_state = state,
                                             .migration = "",
                                             .report_json = "{}",
                                             .report_size = 2};
    activation.candidate_state.image = 2;
    /* Each refusal must preserve both durable authority and the snapshot. */
    for (unsigned fault = 0; fault < 7; fault++) {
        struct mo_store_activation bad = activation;
        switch (fault) {
        case 0:
            bad.candidate.source[0] ^= 1;
            break;
        case 1:
            bad.candidate.schema[0] ^= 1;
            break;
        case 2:
            bad.candidate.ordinal = 2;
            break;
        case 3:
            bad.previous_state.sequence = 1;
            bad.candidate_state.sequence = 1;
            break;
        case 5:
            bad.candidate.context_digest[0] ^= 1;
            break;
        case 6:
            bad.candidate.context_size--;
            break;
        case 4:
            bad.previous = next;
            bad.previous.ordinal = 0;
            bad.previous_state.image = next.image;
            break;
        }
        CHECK(mo_store_activate(store, &bad) == MO_STORE_ACTIVATION_REFUSED);
        CHECK(mo_store_program_read(store, &copies, &read, &found) == MO_STORE_OK && found);
        CHECK(read.image == old.image && read.ordinal == old.ordinal &&
              memcmp(read.source, old.source, sizeof old.source) == 0);
        CHECK(mo_store_call(store, MO_STORE_LOAD, NULL, snapshot, &state) == MO_STORE_OK);
    }
    CHECK(mo_store_activate(store, &activation) == MO_STORE_ACTIVATED);
    CHECK(mo_store_program_read(store, &copies, &read, &found) == MO_STORE_OK && found);
    CHECK(read.image == 2 && read.ordinal == 1 && memcmp(read.source, next.source, 32) == 0);
    CHECK(mo_store_call(store, MO_STORE_LOAD, NULL, snapshot, &activation.candidate_state) ==
          MO_STORE_OK);
    CHECK(mo_store_activate(store, &activation) == MO_STORE_ACTIVATION_REFUSED);
    /* An ordinary snapshot preserves user_version=2 and the active source. */
    CHECK(mo_store_call(store, MO_STORE_SNAPSHOT, &activation.candidate_state, NULL, NULL) ==
          MO_STORE_OK);
    CHECK(mo_store_call(store, MO_STORE_LOAD, NULL, snapshot, &activation.candidate_state) ==
          MO_STORE_OK);
    mo_store_close(store);
    store = NULL;
    CHECK(mo_store_open(NULL, &store) == MO_STORE_OK);
    CHECK(mo_store_call(store, MO_STORE_SNAPSHOT, &state, NULL, NULL) == MO_STORE_OK);
    struct mo_store_activation adoption = activation;
    memset(adoption.previous.source, 0, sizeof adoption.previous.source);
    adoption.previous.bundle = NULL;
    adoption.previous.bundle_size = 0;
    adoption.previous.context = NULL;
    adoption.previous.context_size = 0;
    /* Adoption cannot replace a snapshot other than the one replay verified. */
    adoption.previous_state.image++;
    CHECK(mo_store_adopt(store, &adoption, true) == MO_STORE_ACTIVATION_REFUSED);
    adoption.previous_state = state;
    struct mo_buffer wrong_snapshot;
    mo_buffer_init(&wrong_snapshot, MO_LIMIT_SAVED_BYTES);
    CHECK(mo_value_encode(heap, f.builtin[MO_TYPE_INT], mo_cell_int(43), &wrong_snapshot));
    adoption.previous_state.bytes = wrong_snapshot.bytes;
    adoption.previous_state.length = wrong_snapshot.length;
    CHECK(mo_store_adopt(store, &adoption, true) == MO_STORE_ACTIVATION_REFUSED);
    CHECK(mo_store_call(store, MO_STORE_LOAD, NULL, snapshot, &state) == MO_STORE_OK);
    mo_buffer_free(&wrong_snapshot);
    adoption.previous_state = state;
    CHECK(mo_store_adopt(store, &adoption, true) == MO_STORE_ACTIVATED);
    CHECK(mo_store_program_read(store, &copies, &read, &found) == MO_STORE_OK && found);
    CHECK(read.ordinal == 1 && read.image == 2 && memcmp(read.source, next.source, 32) == 0);
    CHECK(mo_store_adopt(store, &adoption, true) == MO_STORE_ACTIVATION_REFUSED);
    CHECK(mo_store_call(store, MO_STORE_LOAD, NULL, snapshot, &adoption.candidate_state) ==
          MO_STORE_OK);
    mo_store_close(store);
    mo_arena_free(&copies);
    mo_buffer_free(&saved);
    mo_buffer_free(&old_bundle);
    mo_buffer_free(&new_bundle);
    mo_heap_free(heap);
    mo_heap_image_free(f.image);
    mo_arena_free(&f.arena);
    return mo_test_report("upgrade store");
}
