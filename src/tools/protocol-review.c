#include "base/codes.h"
#include "tools/protocol-server.h"
#include "tools/tools.h"

#include <stdio.h>
#include <string.h>

enum { REVIEW_ITEMS = 4, REVIEW_APPROVE = 5, REVIEW_SETTING = 6 };
enum { PAGE_BYTES = MO_LIMIT_MCP_RESPONSE_BYTES / 4 - 256 };

struct review_reply {
    struct mo_buffer json, text;
};

static bool head(struct review_reply *out, const struct mo_store_review *r, bool program_line) {
    char program[65], json[256], text[256];
    mo_protocol_hex(r->program.source, 32, program);
    const char *setting = mo_store_setting_name(r->intent);
    unsigned long long version = (unsigned long long)r->version;
    int n = snprintf(json, sizeof json,
                     "{\"version\":1,\"program\":\"%s\",\"setting\":\"%s\",\"approval_version\":"
                     "\"%llu\"",
                     program, setting, version);
    int m =
        program_line
            ? snprintf(text, sizeof text, "program %s\nreview setting: %s; approval version %llu\n",
                       program, setting, version)
            : snprintf(text, sizeof text, "review setting: %s; approval version %llu\n", setting,
                       version);
    return n > 0 && (size_t)n < sizeof json && m > 0 && (size_t)m < sizeof text &&
           mo_buffer_append(&out->json, json, (size_t)n) &&
           mo_buffer_append(&out->text, text, (size_t)m);
}

static bool field(struct mo_buffer *json, const char *key, struct mo_bundle_span value) {
    return mo_buffer_append_text(json, key) && mo_render_json_text(json, value.bytes, value.length);
}

static bool indented(struct mo_buffer *text, struct mo_bundle_span value) {
    bool ok = true;
    for (size_t at = 0; ok && at < value.length;) {
        const uint8_t *end = memchr(value.bytes + at, '\n', value.length - at);
        size_t length = end == NULL ? value.length - at : (size_t)(end - value.bytes) - at;
        ok = mo_buffer_append_text(text, "\n  ") &&
             mo_render_terminal_text(text, value.bytes + at, length);
        at += length + 1;
    }
    return ok;
}

static bool item(struct review_reply *out, const struct mo_store_review *r,
                 const struct mo_intent_entry *e, bool whole) {
    char digest[65];
    mo_protocol_hex(e->digest, 32, digest);
    const char *kind = mo_intent_kind_name(e->kind);
    bool mark = mo_store_review_approved(r, e->digest);
    struct mo_buffer *json = &out->json, *text = &out->text;
    bool ok = mo_buffer_append_text(json, "{\"digest\":\"") && mo_buffer_append(json, digest, 64) &&
              mo_buffer_append_text(json, "\",\"kind\":") &&
              mo_render_json_text(json, kind, strlen(kind)) &&
              mo_buffer_append_text(text, mark ? "approved   " : "unapproved ") &&
              mo_buffer_append(text, digest, 64) && mo_buffer_append_byte(text, ' ') &&
              mo_buffer_append_text(text, kind) && mo_buffer_append_byte(text, ' ') &&
              mo_render_terminal_text(text, e->name.bytes, e->name.length) &&
              field(json, ",\"owner\":", e->owner) && field(json, ",\"name\":", e->name);
    if (!whole)
        return ok &&
               mo_buffer_append_text(json, mark ? ",\"approved\":true,\"clipped\":true}"
                                                : ",\"approved\":false,\"clipped\":true}") &&
               mo_buffer_append_text(text, "\n  too large for a review page; mo intent shows it\n");
    ok = ok && indented(text, e->text) && field(json, ",\"text\":", e->text) &&
         mo_buffer_append_text(json, mark ? ",\"approved\":true,\"reads\":["
                                          : ",\"approved\":false,\"reads\":[");
    struct mo_binary reads = e->reads;
    struct mo_bundle_span name;
    for (uint32_t i = 0; ok && i < e->read_count; i++)
        ok = mo_intent_list_read_name(&reads, &name) &&
             (i == 0 || mo_buffer_append_byte(json, ',')) &&
             mo_render_json_text(json, name.bytes, name.length) &&
             mo_buffer_append_text(text, i == 0 ? "\n  reads " : ", ") &&
             mo_render_terminal_text(text, name.bytes, name.length);
    return ok && mo_buffer_append_text(json, "]}") && mo_buffer_append_byte(text, '\n');
}

static bool page(struct review_reply *out, const struct mo_store_review *r, uint64_t offset) {
    struct review_reply one;
    mo_buffer_init(&one.json, PAGE_BYTES);
    mo_buffer_init(&one.text, PAGE_BYTES);
    char tail[128];
    bool ok = head(out, r, offset == 0) && mo_buffer_append_text(&out->json, ",\"items\":[");
    struct mo_binary cursor = mo_intent_list_first(&r->items);
    struct mo_intent_entry e;
    uint32_t at = 0, shown = 0;
    for (; ok && at < r->items.count; at++) {
        MO_ASSERT(mo_intent_list_next(&cursor, &e));
        if (at < offset)
            continue;
        one.json.length = 0;
        one.text.length = 0;
        ok = item(&one, r, &e, true);
        if (!ok) {
            one.json.length = 0;
            one.text.length = 0;
            ok = item(&one, r, &e, false);
        }
        if (!ok || out->json.length + one.json.length + sizeof tail > PAGE_BYTES ||
            out->text.length + one.text.length > PAGE_BYTES)
            break;
        ok = (shown == 0 || mo_buffer_append_byte(&out->json, ',')) &&
             mo_buffer_append(&out->json, one.json.bytes, one.json.length) &&
             mo_buffer_append(&out->text, one.text.bytes, one.text.length);
        shown++;
    }
    mo_buffer_free(&one.json);
    mo_buffer_free(&one.text);
    ok = ok && (shown > 0 || at == r->items.count);
    int n = at == r->items.count
                ? snprintf(tail, sizeof tail, "],\"total\":%u}", r->items.count)
                : snprintf(tail, sizeof tail,
                           "],\"total\":%u,\"truncated\":%u,\"next\":{\"offset\":%u}}",
                           r->items.count, r->items.count - at, at);
    return ok && n > 0 && (size_t)n < sizeof tail && mo_buffer_append(&out->json, tail, (size_t)n);
}

static void refuse_status(struct mo_protocol_server *s, struct protocol_connection *c,
                          enum mo_store_review_status status, const struct mo_store_review *r,
                          const uint8_t *digest) {
    char active[65], hex[65], message[256];
    mo_protocol_hex(r->program.source, 32, active);
    int n = 0;
    switch (status) {
    case MO_STORE_REVIEW_STALE:
        n = snprintf(message, sizeof message, "program is not the active program %s", active);
        MO_ASSERT(n > 0 && (size_t)n < sizeof message);
        mo_protocol_upgrade_refuse(s, c, MO_CODE_UPGRADE_STALE_BASE, "base", message, NULL);
        return;
    case MO_STORE_REVIEW_ABSENT:
        mo_protocol_hex(digest, 32, hex);
        n = snprintf(message, sizeof message, "the active program %s has no item %s", active, hex);
        MO_ASSERT(n > 0 && (size_t)n < sizeof message);
        mo_protocol_upgrade_refuse(s, c, MO_CODE_UPGRADE_STALE_BASE, "item", message, NULL);
        return;
    case MO_STORE_REVIEW_FULL:
        mo_protocol_upgrade_refuse(s, c, MO_CODE_REVIEW_FULL, "record",
                                   "the review record is full; only a fresh data folder accepts "
                                   "further decisions",
                                   NULL);
        return;
    case MO_STORE_REVIEW_OK:
    case MO_STORE_REVIEW_FAILED:
        break;
    }
    mo_protocol_upgrade_refuse(s, c, MO_CODE_UPGRADE_INVALID_VALUE, "store",
                               "the data folder's review tables could not be read or written",
                               NULL);
}

static bool decided(struct review_reply *out, const struct mo_store_review *r, uint32_t kind,
                    const uint8_t *digest) {
    char hex[65];
    mo_protocol_hex(digest, 32, hex);
    if (kind == REVIEW_APPROVE &&
        !(mo_buffer_append_text(&out->text, "approved ") && mo_buffer_append(&out->text, hex, 64) &&
          mo_buffer_append_byte(&out->text, '\n')))
        return false;
    return head(out, r, false) && mo_buffer_append_byte(&out->json, '}');
}

static void answer(struct mo_protocol_server *s, struct protocol_connection *c, uint32_t kind,
                   const struct upgrade_arguments *a, struct mo_arena *arena) {
    struct mo_store_review r = {0};
    if (mo_store_review_read(s->store, arena, &r) != MO_STORE_OK || !r.reviewed) {
        refuse_status(s, c, MO_STORE_REVIEW_FAILED, &r, NULL);
        return;
    }
    uint8_t program[32] = {0}, digest[32] = {0};
    enum mo_store_review_status status = MO_STORE_REVIEW_OK;
    if (kind == REVIEW_APPROVE) {
        if (!mo_protocol_unhex(a->program, program, 32) ||
            !mo_protocol_unhex(a->digest, digest, 32))
            status = MO_STORE_REVIEW_FAILED;
        else
            status = mo_store_approve(s->store, program, digest, s->now.wall_us);
    } else if (kind == REVIEW_SETTING && a->setting[0] != 0)
        status = mo_store_set_review(s->store, strcmp(a->setting, "intent") == 0, s->now.wall_us);
    if (status == MO_STORE_REVIEW_OK && kind != REVIEW_ITEMS &&
        mo_store_review_read(s->store, arena, &r) != MO_STORE_OK)
        status = MO_STORE_REVIEW_FAILED;
    if (status != MO_STORE_REVIEW_OK) {
        refuse_status(s, c, status, &r, digest);
        return;
    }
    uint64_t offset = 0;
    if (kind == REVIEW_ITEMS && a->offset[0] != 0 && !mo_protocol_decimal(a->offset, &offset))
        offset = UINT64_MAX;
    struct review_reply reply;
    mo_buffer_init(&reply.json, PAGE_BYTES);
    mo_buffer_init(&reply.text, PAGE_BYTES);
    bool ok = kind == REVIEW_ITEMS ? page(&reply, &r, offset) : decided(&reply, &r, kind, digest);
    if (ok)
        mo_protocol_upgrade_reply(s, c, &reply.json, &reply.text, false);
    else
        mo_protocol_fallback(s, c, PE_RESPONSE, EFFECT_NONE);
    mo_buffer_free(&reply.json);
    mo_buffer_free(&reply.text);
}

void mo_protocol_review_call(struct mo_protocol_server *s, struct protocol_connection *c,
                             uint32_t kind) {
    MO_ASSERT(kind >= REVIEW_ITEMS && kind <= REVIEW_SETTING);
    if (!s->approval_enabled || !c->approval_authorized) {
        mo_protocol_upgrade_refuse(s, c, MO_CODE_UPGRADE_CREDENTIAL, "credential",
                                   s->approval_enabled
                                       ? "the approval credential is missing or wrong"
                                       : "this service was started without an approval credential",
                                   NULL);
        return;
    }
    struct mo_arena arena;
    mo_arena_init(&arena, sizeof(struct upgrade_arguments) + MO_ARENA_CHUNK_HEADER +
                              2 * (size_t)MO_STORE_REVIEW_READ_BYTES);
    struct upgrade_arguments *a =
        mo_arena_allocate(&arena, sizeof *a, _Alignof(struct upgrade_arguments));
    if (a == NULL || !mo_protocol_upgrade_arguments(s, c, a))
        mo_protocol_fallback(s, c, c->work_left == 0 ? PE_WORK : PE_MEMORY, EFFECT_NONE);
    else
        answer(s, c, kind, a, &arena);
    mo_arena_free(&arena);
}
