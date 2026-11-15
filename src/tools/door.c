#include "base/intent-list.h"
#include "base/sha256.h"
#include "tools/offline.h"
#include "tools/upgrade.h"

#include <stdlib.h>
#include <string.h>

static int digest_order(const void *a, const void *b) {
    return memcmp(a, b, 32);
}

static uint8_t (*sorted(struct mo_arena *arena, const struct mo_intent_list *list, bool reached,
                        uint32_t *count))[32] {
    *count = 0;
    uint8_t(*digests)[32] =
        mo_arena_allocate_array(arena, list->count == 0 ? 1 : list->count, 32, _Alignof(uint8_t));
    if (digests == NULL)
        return NULL;
    struct mo_binary cursor = mo_intent_list_first(list);
    struct mo_intent_entry entry;
    for (uint32_t i = 0; i < list->count; i++) {
        MO_ASSERT(mo_intent_list_next(&cursor, &entry));
        if (entry.reached || !reached)
            memcpy(digests[(*count)++], entry.digest, 32);
    }
    qsort(digests, *count, 32, digest_order);
    return digests;
}

static bool listed(const uint8_t (*digests)[32], uint32_t count, const uint8_t *digest) {
    return bsearch(digest, digests, count, 32, digest_order) != NULL;
}

static bool entry(struct mo_buffer *out, const char *mark, const struct mo_intent_entry *e) {
    bool ok = mo_buffer_append_text(out, mark) && mo_buffer_append_byte(out, ' ') &&
              mo_offline_hex(out, e->digest, 32) && mo_buffer_append_byte(out, ' ') &&
              mo_buffer_append_text(out, mo_intent_kind_name(e->kind)) &&
              mo_buffer_append_byte(out, ' ') &&
              mo_buffer_append(out, e->name.bytes, e->name.length);
    for (size_t at = 0; ok && at < e->text.length;) {
        const uint8_t *end = memchr(e->text.bytes + at, '\n', e->text.length - at);
        size_t length = end == NULL ? e->text.length - at : (size_t)(end - e->text.bytes) - at;
        ok =
            mo_buffer_append_text(out, "\n  ") && mo_buffer_append(out, e->text.bytes + at, length);
        at += length + 1;
    }
    struct mo_binary reads = e->reads;
    struct mo_bundle_span name;
    for (uint32_t i = 0; ok && i < e->read_count; i++)
        ok = mo_intent_list_read_name(&reads, &name) &&
             mo_buffer_append_text(out, i == 0 ? "\n  reads " : ", ") &&
             mo_buffer_append(out, name.bytes, name.length);
    return ok && mo_buffer_append_byte(out, '\n');
}

static bool same_owner(const struct mo_bundle_span *owners, uint32_t count,
                       struct mo_bundle_span owner) {
    for (uint32_t i = 0; i < count; i++)
        if (owners[i].length == owner.length &&
            memcmp(owners[i].bytes, owner.bytes, owner.length) == 0)
            return true;
    return false;
}

bool mo_door_difference(struct mo_arena *arena, const struct mo_store_review *r,
                        const struct mo_intent_list *candidate, struct mo_door_view *out) {
    uint32_t has_count, reached_count, had_count;
    uint8_t(*has)[32] = sorted(arena, candidate, false, &has_count);
    uint8_t(*reached)[32] = sorted(arena, candidate, true, &reached_count);
    uint8_t(*had)[32] = sorted(arena, &r->items, false, &had_count);
    struct mo_bundle_span *owners =
        mo_arena_allocate_array(arena, r->approved_count == 0 ? 1 : r->approved_count,
                                sizeof *owners, _Alignof(struct mo_bundle_span));
    out->approve = mo_arena_allocate_array(arena, candidate->count == 0 ? 1 : candidate->count, 32,
                                           _Alignof(uint8_t));
    out->retire = mo_arena_allocate_array(arena, r->approved_count == 0 ? 1 : r->approved_count, 32,
                                          _Alignof(uint8_t));
    out->approve_count = out->retire_count = 0;
    bool ok = has != NULL && reached != NULL && had != NULL && owners != NULL &&
              out->approve != NULL && out->retire != NULL;
    uint32_t lacking = 0;
    out->keeps = true;
    struct mo_binary cursor = mo_intent_list_first(&r->items);
    struct mo_intent_entry e;
    for (uint32_t i = 0; ok && i < r->items.count; i++) {
        MO_ASSERT(mo_intent_list_next(&cursor, &e));
        if (!mo_store_review_approved(r, e.digest))
            continue;
        if (!listed((const uint8_t(*)[32])has, has_count, e.digest)) {
            owners[lacking++] = e.owner;
            ok = entry(&out->text, "lacks", &e);
            out->keeps = false;
        } else if (!listed((const uint8_t(*)[32])reached, reached_count, e.digest)) {
            memcpy(out->retire[out->retire_count++], e.digest, 32);
            ok = entry(&out->text, "not reached", &e);
            out->keeps = false;
        }
    }
    cursor = mo_intent_list_first(candidate);
    for (uint32_t i = 0; ok && i < candidate->count; i++) {
        MO_ASSERT(mo_intent_list_next(&cursor, &e));
        if (!listed((const uint8_t(*)[32])had, had_count, e.digest) &&
            same_owner(owners, lacking, e.owner)) {
            memcpy(out->approve[out->approve_count++], e.digest, 32);
            ok = entry(&out->text, "added", &e);
        }
    }
    return ok;
}

void mo_door_review_id(const uint8_t base[32], const uint8_t candidate[32], const char *migration,
                       uint64_t version, const uint8_t *text, size_t size, uint8_t out[32]) {
    uint8_t numbers[24];
    uint64_t values[] = {strlen(migration), version, size};
    for (size_t i = 0; i < 24; i++)
        numbers[i] = (uint8_t)(values[i / 8] >> (56 - 8 * (i % 8)));
    struct mo_sha256 hash;
    mo_sha256_init(&hash);
    mo_sha256_update(&hash, base, 32);
    mo_sha256_update(&hash, candidate, 32);
    mo_sha256_update(&hash, numbers, 8);
    mo_sha256_update(&hash, migration, strlen(migration));
    mo_sha256_update(&hash, numbers + 8, 16);
    mo_sha256_update(&hash, text, size);
    mo_sha256_finish(&hash, out);
}

bool mo_door_lines(struct mo_buffer *out, const uint8_t *bytes, size_t length) {
    bool ok = true;
    for (size_t at = 0; ok && at < length;) {
        const uint8_t *end = memchr(bytes + at, '\n', length - at);
        size_t line = end == NULL ? length - at : (size_t)(end - bytes) - at;
        ok = mo_render_terminal_text(out, bytes + at, line) && mo_buffer_append_byte(out, '\n');
        at += line + 1;
    }
    return ok;
}

void mo_door_activate(struct mo_store_activation *activation, struct mo_store_decision *decision,
                      uint64_t version, const struct mo_door_view *view) {
    activation->decision = decision;
    activation->approval_version = version;
    activation->approve = (const uint8_t(*)[32])view->approve;
    activation->approve_count = view->approve_count;
    activation->retire = (const uint8_t(*)[32])view->retire;
    activation->retire_count = view->retire_count;
}
