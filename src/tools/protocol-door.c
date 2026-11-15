#define _DEFAULT_SOURCE
#include "base/codes.h"
#include "base/intent-list.h"
#include "base/upload.h"
#include "base/utf8.h"
#include "tools/protocol-server.h"
#include "tools/tools.h"
#include "tools/upgrade.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

enum { REVIEW_PENDING = 7, REVIEW_SOURCE = 10 };
enum { PAGE_BYTES = MO_LIMIT_MCP_RESPONSE_BYTES / 4 - 256 };
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

static bool append_hex(struct mo_buffer *out, const char *before, const uint8_t *digest) {
    char hex[65];
    mo_protocol_hex(digest, 32, hex);
    return mo_buffer_append_text(out, before) && mo_buffer_append(out, hex, 64);
}

static bool held_reply(struct mo_protocol_server *s, struct protocol_connection *c,
                       const struct protocol_upgrade *u) {
    struct mo_buffer json, text;
    mo_buffer_init(&json, PAGE_BYTES);
    mo_buffer_init(&text, PAGE_BYTES);
    bool ok = append_hex(&json,
                         "{\"version\":1,\"status\":\"held\",\"code\":null,"
                         "\"class\":\"review\",\"message\":\"the upgrade waits "
                         "for review\",\"review\":\"",
                         u->review) &&
              append_hex(&json, "\",\"base_source\":\"", u->base) &&
              append_hex(&json, "\",\"candidate_source\":\"", u->candidate) &&
              mo_buffer_append_text(&json, "\",\"migration\":") &&
              (u->has_migration ? mo_render_json_text(&json, u->migration, strlen(u->migration))
                                : mo_buffer_append_text(&json, "null")) &&
              mo_buffer_append_text(&json, u->would ? ",\"intent_would_activate\":true}"
                                                    : ",\"intent_would_activate\":false}") &&
              append_hex(&text, "held ", u->review) && mo_buffer_append_byte(&text, '\n');
    if (ok && c != NULL)
        mo_protocol_upgrade_reply(s, c, &json, &text, false);
    mo_buffer_free(&json);
    mo_buffer_free(&text);
    return ok;
}

static enum door_outcome hold(struct mo_protocol_server *s, struct protocol_connection *caller,
                              const struct mo_store_review *r, const struct mo_door_view *view) {
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
    mo_arena_init(&arena, MO_DOOR_BYTES);
    struct mo_door_view view;
    mo_buffer_init(&view.text, MO_DOOR_DIFFERENCE_BYTES);
    struct mo_store_review r = {0};
    struct mo_intent_list items;
    const struct mo_store_program *candidate = mo_live_program(u->live);
    enum door_outcome outcome = DOOR_REFUSED;
    if (mo_store_review_read(s->store, &arena, &r) != MO_STORE_OK || !r.reviewed ||
        !mo_intent_list_read(candidate->items, candidate->items_size, &items) ||
        !mo_door_difference(&arena, &r, &items, &view))
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
        mo_door_review_id(r.program.source, candidate->source, migration_of(u), r.version,
                          view.text.bytes, view.text.length, id);
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
    mo_arena_init(arena, MO_DOOR_BYTES);
    struct mo_door_view view;
    mo_buffer_init(&view.text, MO_DOOR_DIFFERENCE_BYTES);
    view.approve_count = view.retire_count = 0;
    struct mo_store_review r = {0};
    struct mo_intent_list items;
    bool ok = mo_store_review_read(s->store, arena, &r) == MO_STORE_OK && r.reviewed &&
              mo_intent_list_read(activation->candidate.items, activation->candidate.items_size,
                                  &items) &&
              (!u->yes || mo_door_difference(arena, &r, &items, &view));
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
        mo_door_activate(activation, decision, r.version, &view);
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
        mo_door_lines(text, p->difference, shown) &&
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

static bool named(struct mo_protocol_server *s, struct protocol_connection *c,
                  const struct upgrade_arguments *a, const struct mo_store_review *r,
                  const struct mo_store_pending *p, bool found) {
    uint8_t id[32], current[32];
    MO_ASSERT(mo_protocol_unhex(a->review, id, 32));
    if (!found) {
        mo_protocol_upgrade_refuse(s, c, MO_CODE_REVIEW_NOT_PENDING, "review",
                                   "no upgrade is pending", NULL);
        return false;
    }
    mo_door_review_id(r->program.source, p->candidate, p->migration, r->version, p->difference,
                      p->difference_size, current);
    if (memcmp(id, p->review, 32) == 0 && memcmp(current, p->review, 32) == 0)
        return true;
    char hex[65], message[192];
    mo_protocol_hex(p->review, 32, hex);
    int n =
        snprintf(message, sizeof message,
                 "the review id is not the pending upgrade's; the pending review id is %s", hex);
    MO_ASSERT(n > 0 && (size_t)n < sizeof message);
    mo_protocol_upgrade_refuse(s, c, MO_CODE_REVIEW_MISMATCH, "review", message, NULL);
    return false;
}

static void source_page(struct mo_protocol_server *s, struct protocol_connection *c,
                        const struct upgrade_arguments *a, const struct mo_store_pending *p) {
    const struct protocol_upgrade *u = &s->upgrade;
    bool pending = strcmp(a->source, "pending") == 0;
    MO_ASSERT(pending || u->active_found);
    const uint8_t *bundle = pending ? p->bundle : u->active.bundle;
    size_t total = pending ? p->bundle_size : u->active.bundle_size;
    uint64_t offset = 0;
    if (a->offset[0] != 0 && !mo_protocol_decimal(a->offset, &offset))
        offset = UINT64_MAX;
    size_t at = offset < total ? (size_t)offset : total;
    size_t length =
        total - at < MO_LIMIT_UPGRADE_PAGE_BYTES ? total - at : MO_LIMIT_UPGRADE_PAGE_BYTES;
    char bytes[MO_LIMIT_UPGRADE_PAGE_TEXT + 1];
    mo_upload_text(bundle + at, length, bytes);
    struct mo_buffer json, text;
    mo_buffer_init(&json, MO_LIMIT_UPGRADE_PAGE_TEXT + 512);
    mo_buffer_init(&text, 256);
    bool ok = append_hex(&json, "{\"version\":1,\"review\":\"", p->review) &&
              mo_buffer_append_text(&json, "\",\"source\":\"") &&
              mo_buffer_append_text(&json, pending ? "pending" : "active") &&
              mo_buffer_append_text(&json, "\",\"offset\":") && mo_buffer_append_uint(&json, at) &&
              mo_buffer_append_text(&json, ",\"total\":") && mo_buffer_append_uint(&json, total) &&
              mo_buffer_append_text(&json, ",\"bytes\":\"") &&
              mo_buffer_append_text(&json, bytes) && mo_buffer_append_byte(&json, '"') &&
              (at + length == total ||
               (mo_buffer_append_text(&json, ",\"next\":{\"offset\":") &&
                mo_buffer_append_uint(&json, at + length) && mo_buffer_append_byte(&json, '}'))) &&
              mo_buffer_append_byte(&json, '}') &&
              mo_buffer_append_text(&text,
                                    pending ? "pending source, bytes " : "active source, bytes ") &&
              mo_buffer_append_uint(&text, at) && mo_buffer_append_text(&text, " to ") &&
              mo_buffer_append_uint(&text, at + length) && mo_buffer_append_text(&text, " of ") &&
              mo_buffer_append_uint(&text, total) && mo_buffer_append_byte(&text, '\n');
    if (ok)
        mo_protocol_upgrade_reply(s, c, &json, &text, false);
    else
        mo_protocol_fallback(s, c, PE_MEMORY, EFFECT_NONE);
    mo_buffer_free(&json);
    mo_buffer_free(&text);
}

static void answer(struct mo_protocol_server *s, struct protocol_connection *c,
                   const struct upgrade_arguments *a, const struct mo_store_review *r,
                   const struct mo_store_pending *p) {
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
    mo_arena_init(&arena, MO_DOOR_BYTES);
    struct mo_store_review r = {0};
    struct mo_store_pending p = {0};
    bool found = false;
    if (mo_store_review_read(s->store, &arena, &r) != MO_STORE_OK || !r.reviewed ||
        mo_store_pending_read(s->store, &arena, &p, &found) != MO_STORE_OK)
        mo_protocol_upgrade_refuse(s, c, MO_CODE_UPGRADE_INVALID_VALUE, "store",
                                   "the data folder's review tables could not be read", NULL);
    else if (kind == REVIEW_PENDING)
        pending_reply(s, c, &p, found);
    else if (named(s, c, a, &r, &p, found)) {
        if (kind == REVIEW_SOURCE)
            source_page(s, c, a, &p);
        else
            answer(s, c, a, &r, &p);
    }
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
    mo_arena_init(&arena, MO_DOOR_DIFFERENCE_BYTES + MO_LIMIT_BUNDLE_BYTES +
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
