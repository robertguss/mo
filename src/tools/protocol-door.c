#define _DEFAULT_SOURCE
#include "base/codes.h"
#include "base/intent-list.h"
#include "base/sha256.h"
#include "base/utf8.h"
#include "tools/protocol-server.h"
#include "tools/tools.h"
#include "tools/upgrade.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

enum { REVIEW_PENDING = 7 };
enum { PAGE_BYTES = MO_LIMIT_MCP_RESPONSE_BYTES / 4 - 256 };
enum { DIFFERENCE_BYTES = 2 * MO_LIMIT_INTENT_LIST_BYTES };
enum {
    DOOR_BYTES = MO_STORE_REVIEW_READ_BYTES + 5 * MO_LIMIT_INTENT_ITEMS * 32 +
                 MO_LIMIT_INTENT_ITEMS * (int)sizeof(struct mo_bundle_span) + DIFFERENCE_BYTES +
                 MO_LIMIT_BUNDLE_BYTES + MO_LIMIT_UPGRADE_MIGRATION_NAME +
                 12 * MO_ARENA_CHUNK_HEADER
};

struct door_view {
    struct mo_buffer text;
    uint8_t (*approve)[32], (*retire)[32];
    uint32_t approve_count, retire_count;
    bool keeps;
};

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
    char digest[65];
    mo_protocol_hex(e->digest, 32, digest);
    bool ok = mo_buffer_append_text(out, mark) && mo_buffer_append_byte(out, ' ') &&
              mo_buffer_append(out, digest, 64) && mo_buffer_append_byte(out, ' ') &&
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

static bool difference(struct mo_arena *arena, const struct mo_store_review *r,
                       const struct mo_intent_list *candidate, struct door_view *out) {
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

static void review_id(const uint8_t base[32], const uint8_t candidate[32], const char *migration,
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

static const char *migration_of(const struct protocol_upgrade *u) {
    return u->has_migration ? u->migration : "";
}

static struct mo_store_decision row(const struct protocol_upgrade *u, const char *kind, bool intent,
                                    int64_t time) {
    struct mo_store_decision d = {.kind = kind,
                                  .has_candidate = true,
                                  .has_review = u->yes,
                                  .intent = intent,
                                  .unattended = u->would,
                                  .time = time};
    memcpy(d.base, u->base, 32);
    memcpy(d.candidate, u->candidate, 32);
    memcpy(d.review, u->review, 32);
    return d;
}

static enum door_outcome door_refuse(struct protocol_upgrade *u, uint16_t code, const char *class,
                                     const char *message) {
    mo_live_refuse(u->live, code, class, message);
    return DOOR_REFUSED;
}

static bool held_reply(struct mo_protocol_server *s, struct protocol_connection *c,
                       const struct protocol_upgrade *u) {
    char id[65], base[65], candidate[65];
    mo_protocol_hex(u->review, 32, id);
    mo_protocol_hex(u->base, 32, base);
    mo_protocol_hex(u->candidate, 32, candidate);
    struct mo_buffer json, text;
    mo_buffer_init(&json, PAGE_BYTES);
    mo_buffer_init(&text, PAGE_BYTES);
    bool ok = mo_buffer_append_text(&json, "{\"version\":1,\"status\":\"held\",\"code\":null,"
                                           "\"class\":\"review\",\"message\":\"the upgrade waits "
                                           "for review\",\"review\":\"") &&
              mo_buffer_append(&json, id, 64) &&
              mo_buffer_append_text(&json, "\",\"base_source\":\"") &&
              mo_buffer_append(&json, base, 64) &&
              mo_buffer_append_text(&json, "\",\"candidate_source\":\"") &&
              mo_buffer_append(&json, candidate, 64) &&
              mo_buffer_append_text(&json, "\",\"migration\":") &&
              (u->has_migration ? mo_render_json_text(&json, u->migration, strlen(u->migration))
                                : mo_buffer_append_text(&json, "null")) &&
              mo_buffer_append_text(&json, u->would ? ",\"intent_would_activate\":true}"
                                                    : ",\"intent_would_activate\":false}") &&
              mo_buffer_append_text(&text, "held ") && mo_buffer_append(&text, id, 64) &&
              mo_buffer_append_byte(&text, '\n');
    if (ok && c != NULL)
        mo_protocol_upgrade_reply(s, c, &json, &text, false);
    mo_buffer_free(&json);
    mo_buffer_free(&text);
    return ok;
}

static enum door_outcome hold(struct mo_protocol_server *s, struct protocol_connection *caller,
                              const struct mo_store_review *r, const struct door_view *view) {
    struct protocol_upgrade *u = &s->upgrade;
    const struct mo_store_program *candidate = mo_live_program(u->live);
    struct mo_store_pending pending = {.migration = migration_of(u),
                                       .version = r->version,
                                       .difference = view->text.bytes,
                                       .difference_size = view->text.length,
                                       .bundle = candidate->bundle,
                                       .bundle_size = candidate->bundle_size};
    memcpy(pending.review, u->review, 32);
    memcpy(pending.base, u->base, 32);
    memcpy(pending.candidate, u->candidate, 32);
    struct mo_store_decision d = row(u, "held", r->intent, s->now.wall_us);
    d.has_review = true;
    switch (mo_store_hold(s->store, &pending, &d)) {
    case MO_STORE_REVIEW_OK:
        break;
    case MO_STORE_REVIEW_FULL:
        return door_refuse(u, MO_CODE_REVIEW_FULL, "record",
                           "the review record is full; only a fresh data folder accepts upgrades");
    case MO_STORE_REVIEW_STALE:
        return door_refuse(u, MO_CODE_UPGRADE_STALE_BASE, "base",
                           "the active program or its approvals changed; push again");
    case MO_STORE_REVIEW_ABSENT:
    case MO_STORE_REVIEW_FAILED:
        return door_refuse(u, MO_CODE_UPGRADE_INVALID_VALUE, "store",
                           "the held upgrade could not be stored; the old program serves");
    }
    if (!held_reply(s, caller, u) && caller != NULL)
        mo_protocol_fallback(s, caller, PE_MEMORY, EFFECT_APPLIED);
    return DOOR_HELD;
}

static enum door_outcome recheck(struct mo_protocol_server *s, struct mo_arena *arena,
                                 const uint8_t id[32], bool intent) {
    struct protocol_upgrade *u = &s->upgrade;
    struct mo_store_pending pending;
    bool found = false;
    if (mo_store_pending_read(s->store, arena, &pending, &found) != MO_STORE_OK)
        return door_refuse(u, MO_CODE_UPGRADE_INVALID_VALUE, "store",
                           "the held upgrade could not be read; the old program serves");
    if (found && memcmp(pending.review, u->review, 32) == 0 && memcmp(id, u->review, 32) == 0)
        return DOOR_PROCEED;
    struct mo_store_decision d = row(u, "refused", intent, s->now.wall_us);
    d.detail = "the review changed while it was rechecked";
    enum mo_store_review_status recorded = mo_store_decide(s->store, &d, u->review);
    MO_ASSERT(recorded != MO_STORE_REVIEW_STALE && recorded != MO_STORE_REVIEW_ABSENT);
    if (!found || memcmp(pending.review, u->review, 32) == 0)
        return door_refuse(u, MO_CODE_REVIEW_NOT_PENDING, "review",
                           "the upgrade changed while it was rechecked and was dropped; nothing "
                           "is pending");
    char current[65];
    mo_protocol_hex(pending.review, 32, current);
    int n = snprintf(u->message, sizeof u->message,
                     "the upgrade changed while it was rechecked; the pending review id is %s",
                     current);
    MO_ASSERT(n > 0 && (size_t)n < sizeof u->message);
    return door_refuse(u, MO_CODE_REVIEW_MISMATCH, "review", u->message);
}

enum door_outcome mo_protocol_door_decide(struct mo_protocol_server *s,
                                          struct protocol_connection *caller) {
    struct protocol_upgrade *u = &s->upgrade;
    struct mo_arena arena;
    mo_arena_init(&arena, DOOR_BYTES);
    struct door_view view;
    mo_buffer_init(&view.text, DIFFERENCE_BYTES);
    struct mo_store_review r = {0};
    struct mo_intent_list items;
    const struct mo_store_program *candidate = mo_live_program(u->live);
    enum door_outcome outcome = DOOR_REFUSED;
    if (mo_store_review_read(s->store, &arena, &r) != MO_STORE_OK || !r.reviewed ||
        !mo_intent_list_read(candidate->items, candidate->items_size, &items) ||
        !difference(&arena, &r, &items, &view))
        outcome = door_refuse(u, MO_CODE_UPGRADE_INVALID_VALUE, "store",
                              "the data folder's review tables could not be read");
    else if (memcmp(r.program.source, u->base, 32) != 0) {
        char active[65];
        mo_protocol_hex(r.program.source, 32, active);
        int n =
            snprintf(u->message, sizeof u->message, "base is not the active program %s", active);
        MO_ASSERT(n > 0 && (size_t)n < sizeof u->message);
        outcome = door_refuse(u, MO_CODE_UPGRADE_STALE_BASE, "base", u->message);
    } else if (r.records >= MO_LIMIT_REVIEW_RECORDS)
        outcome = door_refuse(u, MO_CODE_REVIEW_FULL, "record",
                              "the review record is full; only a fresh data folder accepts "
                              "upgrades");
    else {
        u->intent = r.intent;
        u->would = view.keeps && !u->has_migration;
        u->approval_version = r.version;
        uint8_t id[32];
        review_id(r.program.source, candidate->source, migration_of(u), r.version, view.text.bytes,
                  view.text.length, id);
        if (u->yes)
            outcome = recheck(s, &arena, id, r.intent);
        else if (u->intent && u->would)
            outcome = DOOR_PROCEED;
        else {
            memcpy(u->review, id, 32);
            outcome = hold(s, caller, &r, &view);
        }
    }
    u->decided = outcome == DOOR_PROCEED;
    mo_buffer_free(&view.text);
    mo_arena_free(&arena);
    return outcome;
}

bool mo_protocol_door_activation(struct mo_protocol_server *s,
                                 struct mo_store_activation *activation,
                                 struct mo_store_decision *decision, struct mo_arena *arena) {
    struct protocol_upgrade *u = &s->upgrade;
    mo_arena_init(arena, DOOR_BYTES);
    struct door_view view;
    mo_buffer_init(&view.text, DIFFERENCE_BYTES);
    view.approve_count = view.retire_count = 0;
    struct mo_store_review r = {0};
    struct mo_intent_list items;
    bool ok = mo_store_review_read(s->store, arena, &r) == MO_STORE_OK && r.reviewed &&
              mo_intent_list_read(activation->candidate.items, activation->candidate.items_size,
                                  &items) &&
              (!u->yes || difference(arena, &r, &items, &view));
    if (!ok)
        mo_live_refuse(u->live, MO_CODE_UPGRADE_INVALID_VALUE, "store",
                       "the data folder's review tables could not be read");
    else if (r.version != u->approval_version) {
        mo_live_refuse(u->live, MO_CODE_UPGRADE_STALE_BASE, "base",
                       "the approvals changed before the message boundary; nothing was activated");
        ok = false;
    }
    if (ok) {
        *decision = row(u, u->yes ? "yes" : "activated", u->intent, s->now.wall_us);
        activation->decision = decision;
        activation->approval_version = r.version;
        activation->approve = (const uint8_t(*)[32])view.approve;
        activation->approve_count = view.approve_count;
        activation->retire = (const uint8_t(*)[32])view.retire;
        activation->retire_count = view.retire_count;
    }
    mo_buffer_free(&view.text);
    return ok;
}

void mo_protocol_door_refused(struct mo_protocol_server *s, uint16_t code) {
    struct protocol_upgrade *u = &s->upgrade;
    if (!u->decided && !u->yes)
        return;
    struct mo_arena arena;
    mo_arena_init(&arena, MO_STORE_REVIEW_READ_BYTES);
    struct mo_store_review r = {0};
    char detail[8];
    int n = snprintf(detail, sizeof detail, "E%04u", (unsigned)code);
    MO_ASSERT(n > 0 && (size_t)n < sizeof detail);
    if (mo_store_review_read(s->store, &arena, &r) == MO_STORE_OK) {
        struct mo_store_decision d = row(u, "refused", r.intent, s->now.wall_us);
        d.detail = detail;
        enum mo_store_review_status recorded =
            mo_store_decide(s->store, &d, u->yes && !u->decided ? u->review : NULL);
        MO_ASSERT(recorded != MO_STORE_REVIEW_STALE && recorded != MO_STORE_REVIEW_ABSENT);
    }
    mo_arena_free(&arena);
}

static bool append_hex(struct mo_buffer *out, const char *before, const uint8_t *digest) {
    char hex[65];
    mo_protocol_hex(digest, 32, hex);
    return mo_buffer_append_text(out, before) && mo_buffer_append(out, hex, 64);
}

static bool append_lines(struct mo_buffer *out, const uint8_t *bytes, size_t length) {
    bool ok = true;
    for (size_t at = 0; ok && at < length;) {
        const uint8_t *end = memchr(bytes + at, '\n', length - at);
        size_t line = end == NULL ? length - at : (size_t)(end - bytes) - at;
        ok = mo_render_terminal_text(out, bytes + at, line) && mo_buffer_append_byte(out, '\n');
        at += line + 1;
    }
    return ok;
}

static const char *pending_state(const struct protocol_upgrade *u,
                                 const struct mo_store_pending *p) {
    return u->open && u->yes && memcmp(u->review, p->review, 32) == 0 ? "rechecking" : "held";
}

static bool pending_view(struct mo_buffer *json, struct mo_buffer *text,
                         const struct protocol_upgrade *u, const struct mo_store_pending *p,
                         size_t shown) {
    const char *state = pending_state(u, p);
    bool ok =
        append_hex(json, "{\"version\":1,\"review\":\"", p->review) &&
        mo_buffer_append_text(json, "\",\"state\":\"") && mo_buffer_append_text(json, state) &&
        append_hex(json, "\",\"base_source\":\"", p->base) &&
        append_hex(json, "\",\"candidate_source\":\"", p->candidate) &&
        mo_buffer_append_text(json, "\",\"migration\":") &&
        (p->migration[0] == 0 ? mo_buffer_append_text(json, "null")
                              : mo_render_json_text(json, p->migration, strlen(p->migration))) &&
        mo_buffer_append_text(json, ",\"approval_version\":\"") &&
        mo_buffer_append_uint(json, p->version) &&
        mo_buffer_append_text(json, "\",\"difference\":") &&
        mo_render_json_text(json, p->difference, shown) &&
        mo_buffer_append_text(json, shown == p->difference_size ? ",\"clipped\":false}"
                                                                : ",\"clipped\":true}") &&
        append_hex(text, "review ", p->review) && mo_buffer_append_text(text, ": ") &&
        mo_buffer_append_text(text, state) && append_hex(text, "\nbase ", p->base) &&
        append_hex(text, "\ncandidate ", p->candidate) &&
        mo_buffer_append_text(text, "\nmigration ") &&
        (p->migration[0] == 0
             ? mo_buffer_append_text(text, "none")
             : mo_render_terminal_text(text, p->migration, strlen(p->migration))) &&
        mo_buffer_append_text(text, "\napproval version ") &&
        mo_buffer_append_uint(text, p->version) && mo_buffer_append_byte(text, '\n') &&
        append_lines(text, p->difference, shown) &&
        (shown == p->difference_size ||
         mo_buffer_append_text(text, "the intent difference is clipped here\n"));
    return ok;
}

static void pending_reply(struct mo_protocol_server *s, struct protocol_connection *c,
                          const struct mo_store_pending *p, bool found) {
    struct mo_buffer json, text;
    mo_buffer_init(&json, PAGE_BYTES);
    mo_buffer_init(&text, PAGE_BYTES);
    bool ok = found ? false
                    : mo_buffer_append_text(&json, "{\"version\":1,\"review\":null}") &&
                          mo_buffer_append_text(&text, "nothing is pending\n");
    size_t shown = found ? p->difference_size : 0;
    while (found && !ok) {
        mo_buffer_clear(&json);
        mo_buffer_clear(&text);
        ok = pending_view(&json, &text, &s->upgrade, p, shown);
        if (ok || shown == 0)
            break;
        shown = shown > PAGE_BYTES / 4 ? PAGE_BYTES / 4 : 0;
        while (shown > 0 && !mo_utf8_valid(p->difference, shown))
            shown--;
    }
    if (ok)
        mo_protocol_upgrade_reply(s, c, &json, &text, false);
    else
        mo_protocol_fallback(s, c, PE_RESPONSE, EFFECT_NONE);
    mo_buffer_free(&json);
    mo_buffer_free(&text);
}

static void start_yes(struct mo_protocol_server *s, struct protocol_connection *c,
                      const struct mo_store_pending *p) {
    struct protocol_upgrade *u = &s->upgrade;
    uint8_t random[16];
    if (getentropy(random, sizeof random) != 0) {
        mo_protocol_fallback(s, c, PE_INTERNAL, EFFECT_NONE);
        return;
    }
    u->live = mo_live_start(u->compiler, &u->active, p->bundle, p->bundle_size,
                            p->migration[0] == 0 ? NULL : p->migration);
    if (u->live == NULL) {
        mo_protocol_fallback(s, c, PE_MEMORY, EFFECT_NONE);
        return;
    }
    mo_buffer_init(&u->bytes, MO_LIMIT_BUNDLE_BYTES);
    u->open = u->compiling = u->yes = true;
    u->decided = u->would = false;
    mo_protocol_hex(random, sizeof random, u->slot);
    memcpy(u->base, p->base, 32);
    memcpy(u->candidate, p->candidate, 32);
    memcpy(u->review, p->review, 32);
    u->has_migration = p->migration[0] != 0;
    memcpy(u->migration, p->migration, strlen(p->migration) + 1);
    u->touched = s->now.monotonic_us;
    c->phase = PS_UPGRADE;
    u->waiting = (size_t)(c - s->connections);
    u->wait_deadline = mo_protocol_deadline(s->now.monotonic_us, MO_LIMIT_MCP_BODY_US);
}

static void dropped_reply(struct mo_protocol_server *s, struct protocol_connection *c,
                          const uint8_t review[32]) {
    struct mo_buffer json, text;
    mo_buffer_init(&json, 256);
    mo_buffer_init(&text, 256);
    if (append_hex(&json, "{\"version\":1,\"status\":\"dropped\",\"dropped\":\"", review) &&
        mo_buffer_append_text(&json, "\"}") && append_hex(&text, "dropped ", review) &&
        mo_buffer_append_byte(&text, '\n'))
        mo_protocol_upgrade_reply(s, c, &json, &text, false);
    else
        mo_protocol_fallback(s, c, PE_MEMORY, EFFECT_APPLIED);
    mo_buffer_free(&json);
    mo_buffer_free(&text);
}

static void answer(struct mo_protocol_server *s, struct protocol_connection *c,
                   const struct upgrade_arguments *a, const struct mo_store_review *r,
                   const struct mo_store_pending *p, bool found) {
    uint8_t named[32], current[32];
    MO_ASSERT(mo_protocol_unhex(a->review, named, 32));
    if (!found) {
        mo_protocol_upgrade_refuse(s, c, MO_CODE_REVIEW_NOT_PENDING, "review",
                                   "no upgrade is pending", NULL);
        return;
    }
    review_id(r->program.source, p->candidate, p->migration, r->version, p->difference,
              p->difference_size, current);
    if (memcmp(named, p->review, 32) != 0 || memcmp(current, p->review, 32) != 0) {
        char hex[65], message[192];
        mo_protocol_hex(p->review, 32, hex);
        int n = snprintf(message, sizeof message,
                         "the review id is not the pending upgrade's; the pending review id is %s",
                         hex);
        MO_ASSERT(n > 0 && (size_t)n < sizeof message);
        mo_protocol_upgrade_refuse(s, c, MO_CODE_REVIEW_MISMATCH, "review", message, NULL);
        return;
    }
    if (strcmp(a->answer, "yes") == 0) {
        if (s->upgrade.open)
            mo_protocol_upgrade_refuse(s, c, MO_CODE_UPGRADE_BUSY, "busy",
                                       "an upload or another yes is in progress; answer again "
                                       "after it ends",
                                       NULL);
        else
            start_yes(s, c, p);
        return;
    }
    struct mo_store_decision d = {.kind = "no",
                                  .has_candidate = true,
                                  .has_review = true,
                                  .intent = r->intent,
                                  .time = s->now.wall_us};
    memcpy(d.base, p->base, 32);
    memcpy(d.candidate, p->candidate, 32);
    memcpy(d.review, p->review, 32);
    enum mo_store_review_status status = mo_store_decide(s->store, &d, p->review);
    if (status == MO_STORE_REVIEW_OK)
        dropped_reply(s, c, p->review);
    else if (status == MO_STORE_REVIEW_FULL)
        mo_protocol_upgrade_refuse(s, c, MO_CODE_REVIEW_FULL, "record",
                                   "the review record is full; only a fresh data folder accepts "
                                   "further decisions",
                                   NULL);
    else
        mo_protocol_upgrade_refuse(s, c, MO_CODE_UPGRADE_INVALID_VALUE, "store",
                                   "the data folder's review tables could not be read or written",
                                   NULL);
}

void mo_protocol_door_call(struct mo_protocol_server *s, struct protocol_connection *c,
                           uint32_t kind, const struct upgrade_arguments *a) {
    struct mo_arena arena;
    mo_arena_init(&arena, DOOR_BYTES);
    struct mo_store_review r = {0};
    struct mo_store_pending p = {0};
    bool found = false;
    if (mo_store_review_read(s->store, &arena, &r) != MO_STORE_OK || !r.reviewed ||
        mo_store_pending_read(s->store, &arena, &p, &found) != MO_STORE_OK)
        mo_protocol_upgrade_refuse(s, c, MO_CODE_UPGRADE_INVALID_VALUE, "store",
                                   "the data folder's review tables could not be read", NULL);
    else if (kind == REVIEW_PENDING)
        pending_reply(s, c, &p, found);
    else
        answer(s, c, a, &r, &p, found);
    mo_arena_free(&arena);
}

struct status_page {
    struct mo_buffer json, text, one_json, one_text;
    uint32_t shown;
    bool ok;
};

static bool status_record(const struct mo_store_decision *d, void *context) {
    struct status_page *page = context;
    struct mo_buffer *json = &page->one_json, *text = &page->one_text;
    mo_buffer_clear(json);
    mo_buffer_clear(text);
    bool ok =
        mo_buffer_append_text(json, page->shown == 0 ? "{\"time_us\":\"" : ",{\"time_us\":\"") &&
        mo_buffer_append_int(json, d->time) && mo_buffer_append_text(json, "\",\"kind\":") &&
        mo_render_json_text(json, d->kind, strlen(d->kind)) &&
        append_hex(json, ",\"base_source\":\"", d->base) &&
        mo_buffer_append_text(json, "\",\"candidate_source\":") &&
        (d->has_candidate ? append_hex(json, "\"", d->candidate) && mo_buffer_append_byte(json, '"')
                          : mo_buffer_append_text(json, "null")) &&
        mo_buffer_append_text(json, ",\"review\":") &&
        (d->has_review ? append_hex(json, "\"", d->review) && mo_buffer_append_byte(json, '"')
                       : mo_buffer_append_text(json, "null")) &&
        mo_buffer_append_text(json, ",\"setting\":\"") &&
        mo_buffer_append_text(json, mo_store_setting_name(d->intent)) &&
        mo_buffer_append_text(json, d->unattended ? "\",\"intent_would_activate\":true"
                                                  : "\",\"intent_would_activate\":false") &&
        mo_buffer_append_text(json, ",\"detail\":") &&
        mo_render_json_text(json, d->detail, strlen(d->detail)) &&
        mo_buffer_append_byte(json, '}') &&
        mo_render_terminal_text(text, d->kind, strlen(d->kind)) &&
        mo_buffer_append_text(text, " at ") && mo_buffer_append_int(text, d->time) &&
        append_hex(text, " us, base ", d->base) &&
        (!d->has_candidate || append_hex(text, ", candidate ", d->candidate)) &&
        (!d->has_review || append_hex(text, ", review ", d->review)) &&
        mo_buffer_append_text(text, ", setting ") &&
        mo_buffer_append_text(text, mo_store_setting_name(d->intent)) &&
        mo_buffer_append_text(text,
                              d->unattended ? ", intent would activate" : ", intent would hold") &&
        (d->detail[0] == 0 || (mo_buffer_append_text(text, ", ") &&
                               mo_render_terminal_text(text, d->detail, strlen(d->detail)))) &&
        mo_buffer_append_byte(text, '\n');
    if (!ok || page->json.length + json->length + 64 > PAGE_BYTES ||
        page->text.length + text->length > PAGE_BYTES)
        return false;
    page->ok = mo_buffer_append(&page->json, json->bytes, json->length) &&
               mo_buffer_append(&page->text, text->bytes, text->length);
    page->shown++;
    return page->ok;
}

void mo_protocol_door_status(struct mo_protocol_server *s, struct protocol_connection *c) {
    if (!s->upgrade_enabled || !c->upgrade_authorized) {
        mo_protocol_upgrade_refuse(s, c, MO_CODE_UPGRADE_CREDENTIAL, "credential",
                                   s->upgrade_enabled
                                       ? "the upgrade credential is missing or wrong"
                                       : "this service was started without an upgrade credential",
                                   NULL);
        return;
    }
    struct mo_arena arena;
    mo_arena_init(&arena, DIFFERENCE_BYTES + MO_LIMIT_BUNDLE_BYTES +
                              MO_LIMIT_UPGRADE_MIGRATION_NAME + 4 * MO_ARENA_CHUNK_HEADER);
    struct mo_store_pending p = {0};
    bool found = false;
    struct status_page page = {.ok = true};
    mo_buffer_init(&page.json, PAGE_BYTES);
    mo_buffer_init(&page.text, PAGE_BYTES);
    mo_buffer_init(&page.one_json, PAGE_BYTES);
    mo_buffer_init(&page.one_text, PAGE_BYTES);
    bool ok = mo_store_pending_read(s->store, &arena, &p, &found) == MO_STORE_OK &&
              mo_buffer_append_text(&page.json, "{\"version\":1,\"pending\":");
    if (ok && found)
        ok = append_hex(&page.json, "{\"review\":\"", p.review) &&
             mo_buffer_append_text(&page.json, "\",\"state\":\"") &&
             mo_buffer_append_text(&page.json, pending_state(&s->upgrade, &p)) &&
             append_hex(&page.json, "\",\"base_source\":\"", p.base) &&
             append_hex(&page.json, "\",\"candidate_source\":\"", p.candidate) &&
             mo_buffer_append_text(&page.json, "\",\"migration\":") &&
             (p.migration[0] == 0
                  ? mo_buffer_append_text(&page.json, "null")
                  : mo_render_json_text(&page.json, p.migration, strlen(p.migration))) &&
             mo_buffer_append_text(&page.json, "},\"decisions\":[") &&
             append_hex(&page.text, "pending ", p.review) &&
             mo_buffer_append_text(&page.text, ": ") &&
             mo_buffer_append_text(&page.text, pending_state(&s->upgrade, &p)) &&
             mo_buffer_append_byte(&page.text, '\n');
    else if (ok)
        ok = mo_buffer_append_text(&page.json, "null,\"decisions\":[") &&
             mo_buffer_append_text(&page.text, "nothing is pending\n");
    ok = ok && mo_store_decisions(s->store, status_record, &page) == MO_STORE_OK && page.ok;
    char tail[64];
    int n = snprintf(tail, sizeof tail, "],\"shown\":%u}", page.shown);
    ok = ok && n > 0 && (size_t)n < sizeof tail && mo_buffer_append(&page.json, tail, (size_t)n);
    if (ok)
        mo_protocol_upgrade_reply(s, c, &page.json, &page.text, false);
    else
        mo_protocol_upgrade_refuse(s, c, MO_CODE_UPGRADE_INVALID_VALUE, "store",
                                   "the data folder's review tables could not be read", NULL);
    mo_buffer_free(&page.json);
    mo_buffer_free(&page.text);
    mo_buffer_free(&page.one_json);
    mo_buffer_free(&page.one_text);
    mo_arena_free(&arena);
}
