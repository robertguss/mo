/* Live upgrade tools on the MCP endpoint: one upload slot, its pages, and the commit that
 * applies the staged candidate between two service messages. Every reply uses the shared
 * upgrade report; no credential, page byte or source text reaches a reply or an event. */
#define _DEFAULT_SOURCE
#include "base/codes.h"
#include "base/sha256.h"
#include "tools/protocol-server.h"
#include "tools/upgrade.h"

#include <stdio.h>
#include <string.h>
#include <sys/random.h>

enum { VIEW_OPEN = 9, VIEW_PAGE = 10, VIEW_COMMIT = 11 };

static bool unhex(const char *text, uint8_t *out, size_t length) {
    if (strlen(text) != 2 * length)
        return false;
    for (size_t i = 0; i < 2 * length; i++) {
        char c = text[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0)
            return false;
        if (i % 2 == 0)
            out[i / 2] = (uint8_t)(v << 4);
        else
            out[i / 2] = (uint8_t)(out[i / 2] | v);
    }
    return true;
}

static bool decimal(const char *text, uint64_t *out) {
    uint64_t n = 0;
    if (text[0] == 0 || (text[0] == '0' && text[1] != 0))
        return false;
    for (const char *c = text; *c != 0; c++) {
        if (*c < '0' || *c > '9' || n > (UINT64_MAX - (uint64_t)(*c - '0')) / 10)
            return false;
        n = n * 10 + (uint64_t)(*c - '0');
    }
    *out = n;
    return true;
}

/* Text fields of a validated call: base, candidate, total, migration, slot, index, offset, bytes.
 */
struct upgrade_arguments {
    char base[65], candidate[65], total[21], migration[MO_LIMIT_UPGRADE_MIGRATION_NAME + 1],
        slot[33], index[21], offset[21];
    char bytes[MO_LIMIT_UPGRADE_PAGE_TEXT + 1];
};

static bool arguments(struct mo_protocol_server *s, struct protocol_connection *c,
                      struct upgrade_arguments *out) {
    struct mo_cell fields = mo_cell_get_payload(c->action);
    struct mo_cell json = mo_value_field(fields, 2), list;
    if (!mo_protocol_invoke(s, c, PF_UPGRADE_ARGUMENTS, &json, &list))
        return false;
    char *targets[] = {out->base, out->candidate, out->total,  out->migration,
                       out->slot, out->index,     out->offset, out->bytes};
    const size_t sizes[] = {sizeof out->base,      sizeof out->candidate, sizeof out->total,
                            sizeof out->migration, sizeof out->slot,      sizeof out->index,
                            sizeof out->offset,    sizeof out->bytes};
    bool ok = mo_list_size(list) == 8;
    for (uint32_t i = 0; ok && i < 8; i++) {
        struct mo_cell item;
        MO_ASSERT(mo_list_get(list, i, &item));
        size_t size;
        const uint8_t *bytes = mo_value_data(item, &size);
        ok = size < sizes[i] && memchr(bytes, 0, size) == NULL;
        if (ok) {
            memcpy(targets[i], bytes, size);
            targets[i][size] = 0;
        }
    }
    mo_protocol_drop(s, c, PF_UPGRADE_ARGUMENTS, list);
    return ok;
}

/* Renders one report into the call's reply; refusal marks the tool result as an error. */
static void reply(struct mo_protocol_server *s, struct protocol_connection *c,
                  const struct mo_buffer *json, const struct mo_buffer *text, bool refused,
                  enum protocol_effect effect) {
    struct mo_heap *heap = mo_process_heap(c->process);
    const struct mo_bytecode_function *fn = mo_protocol_function(s, PF_UPGRADE_REPLY);
    uint32_t request_type = mo_image_types(s->protocol)
                                ->items[mo_protocol_function(s, PF_RECEIVE)->result_type]
                                .cases[2]
                                .fields[0]
                                .type;
    struct mo_cell request = mo_value_field(mo_cell_get_payload(c->action), 0);
    struct mo_cell args[5] = {mo_protocol_field(s->protocol, request_type, request, 0)};
    bool texts = mo_protocol_charge(c, json->length + text->length) &&
                 mo_value_text(heap, (const char *)json->bytes, json->length, &args[1]);
    if (texts && !mo_value_text(heap, (const char *)text->bytes, text->length, &args[2])) {
        mo_value_drop(heap, fn->parameters[1].type, args[1]);
        texts = false;
    }
    args[3] = mo_cell_bool(refused);
    args[4] = mo_cell_int(MO_LIMIT_MCP_RESPONSE_BYTES);
    struct mo_cell rendered;
    bool ok = texts && mo_protocol_invoke(s, c, PF_UPGRADE_REPLY, args, &rendered);
    if (texts) {
        mo_value_drop(heap, fn->parameters[1].type, args[1]);
        mo_value_drop(heap, fn->parameters[2].type, args[2]);
    }
    if (!ok || mo_cell_get_tag(rendered) != 1) {
        mo_protocol_fallback(s, c, ok ? PE_RESPONSE : PE_MEMORY, effect);
        return;
    }
    mo_protocol_respond(s, c, 200, mo_protocol_field(s->protocol, fn->result_type, rendered, 0),
                        effect);
}

/* A refusal: the shared report to the caller and an event with only class and digests. */
static void refuse(struct mo_protocol_server *s, struct protocol_connection *c, uint16_t code,
                   const char *class, const char *message, const uint8_t *candidate) {
    const struct protocol_upgrade *u = &s->upgrade;
    const uint8_t *base = u->active_found ? u->active.source : NULL;
    struct mo_service_upgrade note = {.code = code};
    if (base != NULL)
        memcpy(note.old_source, base, 32);
    if (candidate != NULL)
        memcpy(note.new_source, candidate, 32);
    if (code != 0)
        mo_service_note_upgrade(s->service, &note);
    if (c == NULL)
        return;
    struct mo_buffer json, text;
    mo_buffer_init(&json, MO_LIMIT_UPGRADE_REPORT_JSON_BYTES);
    mo_buffer_init(&text, MO_LIMIT_UPGRADE_REPORT_JSON_BYTES);
    /* A caller without the upgrade credential learns nothing about the slot. */
    const char *migration =
        c != NULL && c->upgrade_authorized && u->open && u->has_migration ? u->migration : NULL;
    if (mo_live_refusal(&json, true, code, class, message, base, candidate, migration) &&
        mo_live_refusal(&text, false, code, class, message, base, candidate, migration))
        reply(s, c, &json, &text, code != 0, EFFECT_NONE);
    else
        mo_protocol_fallback(s, c, PE_MEMORY, EFFECT_NONE);
    mo_buffer_free(&json);
    mo_buffer_free(&text);
}

static void close_slot(struct mo_protocol_server *s) {
    struct protocol_upgrade *u = &s->upgrade;
    mo_live_free(u->live);
    u->live = NULL;
    mo_buffer_free(&u->bytes);
    u->open = false;
    u->compiling = false;
    u->waiting = SIZE_MAX;
    mo_host_wipe(u->slot, sizeof u->slot);
}

static void progress(struct mo_protocol_server *s, struct protocol_connection *c,
                     const char *fields) {
    struct mo_buffer json;
    mo_buffer_init(&json, 512);
    if (mo_buffer_append_text(&json, fields))
        reply(s, c, &json, &json, false, EFFECT_NONE);
    else
        mo_protocol_fallback(s, c, PE_MEMORY, EFFECT_NONE);
    mo_buffer_free(&json);
}

static void refuse_stale_base(struct mo_protocol_server *s, struct protocol_connection *c,
                              const uint8_t *candidate) {
    char active[65], message[160];
    mo_protocol_hex(s->upgrade.active.source, 32, active);
    int n = snprintf(message, sizeof message, "base is not the active program %s", active);
    MO_ASSERT(n > 0 && (size_t)n < sizeof message);
    refuse(s, c, MO_CODE_UPGRADE_STALE_BASE, "base", message, candidate);
}

static void open_slot(struct mo_protocol_server *s, struct protocol_connection *c,
                      const struct upgrade_arguments *a) {
    struct protocol_upgrade *u = &s->upgrade;
    uint8_t base[32], candidate[32];
    uint64_t total;
    if (!unhex(a->base, base, 32) || !unhex(a->candidate, candidate, 32) ||
        !decimal(a->total, &total)) {
        refuse(s, c, MO_CODE_UPGRADE_BUNDLE_FORMAT, "bundle", "invalid upload arguments", NULL);
        return;
    }
    if (u->open) {
        refuse(s, c, MO_CODE_UPGRADE_BUSY, "busy",
               "another upload slot is open; it closes after 60 seconds without progress",
               candidate);
        return;
    }
    if (!u->active_found) {
        refuse(s, c, MO_CODE_UPGRADE_SOURCE_MISMATCH, "source",
               "this service has no recorded program to replace", candidate);
        return;
    }
    if (total == 0 || total > MO_LIMIT_BUNDLE_BYTES) {
        refuse(s, c, MO_CODE_UPGRADE_BUNDLE_SIZE, "bundle", "a source bundle holds 1 byte to 1 MiB",
               candidate);
        return;
    }
    if (memcmp(base, u->active.source, 32) != 0) {
        refuse_stale_base(s, c, candidate);
        return;
    }
    if (memcmp(candidate, u->active.source, 32) == 0) {
        refuse(s, c, 0, "compatible", "candidate source is already active", candidate);
        return;
    }
    uint8_t random[16];
    if (getentropy(random, sizeof random) != 0) {
        mo_protocol_fallback(s, c, PE_INTERNAL, EFFECT_NONE);
        return;
    }
    mo_buffer_init(&u->bytes, MO_LIMIT_BUNDLE_BYTES);
    static const uint8_t zeros[MO_LIMIT_UPGRADE_PAGE_BYTES];
    bool ok = true;
    for (uint64_t at = 0; ok && at < total; at += sizeof zeros)
        ok = mo_buffer_append(&u->bytes, zeros,
                              total - at < sizeof zeros ? (size_t)(total - at) : sizeof zeros);
    if (!ok) {
        mo_buffer_free(&u->bytes);
        mo_protocol_fallback(s, c, PE_MEMORY, EFFECT_NONE);
        return;
    }
    u->open = true;
    mo_protocol_hex(random, sizeof random, u->slot);
    memcpy(u->base, base, 32);
    memcpy(u->candidate, candidate, 32);
    u->has_migration = a->migration[0] != 0;
    memcpy(u->migration, a->migration, sizeof u->migration);
    mo_upload_init(&u->upload, u->bytes.bytes, (size_t)total);
    u->touched = s->now.monotonic_us;
    char fields[160];
    int n = snprintf(fields, sizeof fields, "{\"slot\":\"%s\",\"pages\":%llu,\"page_bytes\":%d}",
                     u->slot,
                     (unsigned long long)((total + MO_LIMIT_UPGRADE_PAGE_BYTES - 1) /
                                          MO_LIMIT_UPGRADE_PAGE_BYTES),
                     MO_LIMIT_UPGRADE_PAGE_BYTES);
    MO_ASSERT(n > 0 && (size_t)n < sizeof fields);
    progress(s, c, fields);
}

static bool slot_named(const struct protocol_upgrade *u, const char *slot) {
    return u->open && strcmp(u->slot, slot) == 0;
}

static void page(struct mo_protocol_server *s, struct protocol_connection *c,
                 const struct upgrade_arguments *a) {
    struct protocol_upgrade *u = &s->upgrade;
    uint64_t index, offset, total;
    if (!slot_named(u, a->slot) || !decimal(a->index, &index) || !decimal(a->offset, &offset) ||
        !decimal(a->total, &total)) {
        refuse(s, c, MO_CODE_UPGRADE_BUNDLE_FORMAT, "upload",
               "no open upload slot has this id; it may have closed; open a new one with "
               "mo-upgrade-open",
               NULL);
        return;
    }
    if (u->live != NULL) {
        refuse(s, c, MO_CODE_UPGRADE_BUNDLE_FORMAT, "upload",
               "this slot already received its last page", u->candidate);
        return;
    }
    enum mo_upload_status status = mo_upload_page(&u->upload, index, offset, total,
                                                  (const uint8_t *)a->bytes, strlen(a->bytes));
    /* Each refusal names what the slot expected and what the page said; the slot stays. */
    char problem[256];
    int n = 0;
    switch (status) {
    case MO_UPLOAD_MORE:
    case MO_UPLOAD_COMPLETE:
        break;
    case MO_UPLOAD_ORDER:
        n = snprintf(problem, sizeof problem,
                     "expected page %llu at offset %zu, got page %llu at offset %llu; send the "
                     "expected page",
                     (unsigned long long)u->upload.next, u->upload.received,
                     (unsigned long long)index, (unsigned long long)offset);
        break;
    case MO_UPLOAD_TOTAL:
        n = snprintf(problem, sizeof problem, "the slot holds %zu bytes, the page says %llu",
                     u->upload.total, (unsigned long long)total);
        break;
    case MO_UPLOAD_SIZE:
        n = snprintf(problem, sizeof problem,
                     "pages hold %d decoded bytes except the last, which ends at byte %zu",
                     MO_LIMIT_UPGRADE_PAGE_BYTES, u->upload.total);
        break;
    case MO_UPLOAD_ENCODING:
        n = snprintf(problem, sizeof problem, "page bytes are not canonical padded base64");
        break;
    }
    MO_ASSERT(n >= 0 && (size_t)n < sizeof problem);
    if (n > 0) {
        refuse(s, c, MO_CODE_UPGRADE_BUNDLE_FORMAT, "upload", problem, u->candidate);
        return;
    }
    u->touched = s->now.monotonic_us;
    char fields[96];
    if (status == MO_UPLOAD_MORE) {
        n = snprintf(fields, sizeof fields, "{\"received\":%zu,\"complete\":false}",
                     u->upload.received);
        MO_ASSERT(n > 0 && (size_t)n < sizeof fields);
        progress(s, c, fields);
        return;
    }
    uint8_t digest[32];
    struct mo_sha256 hash;
    mo_sha256_init(&hash);
    mo_sha256_update(&hash, u->bytes.bytes, u->upload.total);
    mo_sha256_finish(&hash, digest);
    if (memcmp(digest, u->candidate, 32) != 0) {
        uint8_t candidate[32];
        memcpy(candidate, u->candidate, 32);
        char got[65], wanted[65], message[256];
        mo_protocol_hex(digest, 32, got);
        mo_protocol_hex(candidate, 32, wanted);
        n = snprintf(message, sizeof message,
                     "uploaded bytes do not match the candidate digest: they hash to %s, not %s; "
                     "open a new slot",
                     got, wanted);
        MO_ASSERT(n > 0 && (size_t)n < sizeof message);
        close_slot(s);
        refuse(s, c, MO_CODE_UPGRADE_BUNDLE_FORMAT, "upload", message, candidate);
        return;
    }
    u->live = mo_live_start(u->compiler, &u->active, u->bytes.bytes, u->upload.total,
                            u->has_migration ? u->migration : NULL);
    if (u->live == NULL) {
        close_slot(s);
        mo_protocol_fallback(s, c, PE_MEMORY, EFFECT_NONE);
        return;
    }
    u->compiling = true;
    n = snprintf(fields, sizeof fields, "{\"received\":%zu,\"complete\":true}", u->upload.received);
    MO_ASSERT(n > 0 && (size_t)n < sizeof fields);
    progress(s, c, fields);
}

static void commit(struct mo_protocol_server *s, struct protocol_connection *c,
                   const struct upgrade_arguments *a) {
    struct protocol_upgrade *u = &s->upgrade;
    uint8_t base[32];
    if (!slot_named(u, a->slot) || !unhex(a->base, base, 32)) {
        refuse(s, c, MO_CODE_UPGRADE_BUNDLE_FORMAT, "upload",
               "no open upload slot has this id; it may have closed; open a new one with "
               "mo-upgrade-open",
               NULL);
        return;
    }
    if (u->live == NULL) {
        refuse(s, c, MO_CODE_UPGRADE_BUNDLE_FORMAT, "upload", "the upload is incomplete",
               u->candidate);
        return;
    }
    if (u->waiting != SIZE_MAX) {
        refuse(s, c, MO_CODE_UPGRADE_BUSY, "busy", "this slot is already committing", u->candidate);
        return;
    }
    if (memcmp(base, u->active.source, 32) != 0) {
        uint8_t candidate[32];
        memcpy(candidate, u->candidate, 32);
        close_slot(s);
        refuse_stale_base(s, c, candidate);
        return;
    }
    c->phase = PS_UPGRADE;
    u->waiting = (size_t)(c - s->connections);
    u->wait_deadline = mo_protocol_deadline(s->now.monotonic_us, MO_LIMIT_MCP_BODY_US);
}

void mo_protocol_upgrade_call(struct mo_protocol_server *s, struct protocol_connection *c,
                              uint32_t view) {
    MO_ASSERT(view >= VIEW_OPEN && view <= VIEW_COMMIT);
    if (!s->upgrade_enabled || !c->upgrade_authorized) {
        /* Refused before any argument is read; nothing presented is echoed. */
        refuse(s, c, MO_CODE_UPGRADE_CREDENTIAL, "credential",
               s->upgrade_enabled ? "the upgrade credential is missing or wrong"
                                  : "this service was started without an upgrade credential",
               NULL);
        return;
    }
    struct mo_arena arena;
    mo_arena_init(&arena, sizeof(struct upgrade_arguments) + MO_ARENA_CHUNK_HEADER);
    struct upgrade_arguments *a =
        mo_arena_allocate(&arena, sizeof *a, _Alignof(struct upgrade_arguments));
    if (a == NULL || !arguments(s, c, a)) {
        mo_arena_free(&arena);
        mo_protocol_fallback(s, c, c->work_left == 0 ? PE_WORK : PE_MEMORY, EFFECT_NONE);
        return;
    }
    if (view == VIEW_OPEN)
        open_slot(s, c, a);
    else if (view == VIEW_PAGE)
        page(s, c, a);
    else
        commit(s, c, a);
    mo_host_wipe(a->bytes, sizeof a->bytes);
    mo_arena_free(&arena);
}

static bool previous_borrowed(const struct mo_protocol_server *s) {
    for (size_t i = 0; i < MO_LIMIT_MCP_CONNECTIONS; i++) {
        const struct protocol_connection *c = &s->connections[i];
        if (c->phase != PS_EMPTY && c->borrowed && c->epoch == s->previous.epoch)
            return true;
    }
    return false;
}

static void release_previous(struct mo_protocol_server *s) {
    mo_process_free(s->previous.catalog);
    if (s->previous.owned != NULL)
        mo_image_free(s->previous.owned);
    s->previous = (struct protocol_previous){0};
}

static bool copy_active(struct protocol_upgrade *u, const struct mo_store_program *program) {
    struct mo_arena arena;
    mo_arena_init(&arena, program->bundle_size + program->context_size + 2 * MO_ARENA_CHUNK_HEADER);
    uint8_t *bundle = mo_arena_allocate(&arena, program->bundle_size, 1);
    uint8_t *context = mo_arena_allocate(&arena, program->context_size, 1);
    if (bundle == NULL || context == NULL) {
        mo_arena_free(&arena);
        return false;
    }
    memcpy(bundle, program->bundle, program->bundle_size);
    memcpy(context, program->context, program->context_size);
    mo_arena_free(&u->active_arena);
    u->active_arena = arena;
    u->active = *program;
    u->active.bundle = bundle;
    u->active.context = context;
    u->active_found = true;
    return true;
}

/* Admission, preparation and the activation transaction, all at one message boundary. */
static void activate(struct mo_protocol_server *s) {
    struct protocol_upgrade *u = &s->upgrade;
    struct mo_live_upgrade *live = u->live;
    if (!mo_service_running(s->service)) {
        mo_live_refuse(live, MO_CODE_UPGRADE_BOUNDARY_DEADLINE, "boundary",
                       "the service is stopped; no message boundary can be reached");
        return;
    }
    const uint8_t *state;
    size_t length;
    if (!mo_service_encoded_state(s->service, &state, &length)) {
        mo_live_refuse(live, MO_CODE_UPGRADE_BOUNDARY_DEADLINE, "boundary",
                       "the service has no saved state to migrate");
        return;
    }
    uint64_t head = mo_service_observe(s->service).sequence;
    if (!mo_live_admit(live, s->image, s->service_index, &u->active, state, length))
        return;
    uint32_t index;
    const struct mo_image *image = mo_live_image(live, &index);
    struct mo_process *catalog;
    struct mo_cell descriptor, prepared;
    if (!mo_protocol_catalog(s, image, index, &catalog, &descriptor, &prepared)) {
        mo_live_refuse(live, MO_CODE_UPGRADE_COMPILE_DIAGNOSTIC, "catalog",
                       "the candidate's tool catalog could not be prepared");
        return;
    }
    size_t migrated_length;
    const uint8_t *migrated = mo_live_state(live, &migrated_length);
    struct mo_service_loop *next = NULL;
    enum mo_service_swap_status prepared_loop =
        mo_service_swap_prepare(s->service, image, index, migrated, migrated_length, &next);
    if (prepared_loop != MO_SERVICE_SWAP_READY) {
        mo_process_free(catalog);
        switch (prepared_loop) {
        case MO_SERVICE_SWAP_READY:
            break;
        case MO_SERVICE_SWAP_MEMORY:
            mo_live_refuse(live, MO_CODE_UPGRADE_MIGRATION_BUDGET, "budget",
                           "queued calls do not fit the new program's memory");
            break;
        case MO_SERVICE_SWAP_INTERVAL:
            mo_live_refuse(live, MO_CODE_UPGRADE_TIMER_INTERVAL, "timer",
                           "candidate timer interval is invalid");
            break;
        case MO_SERVICE_SWAP_STATE:
            mo_live_refuse(live, MO_CODE_UPGRADE_INVALID_VALUE, "migration",
                           "migrated state failed exact saved-value validation");
            break;
        case MO_SERVICE_SWAP_STOPPED:
            mo_live_refuse(live, MO_CODE_UPGRADE_BOUNDARY_DEADLINE, "boundary",
                           "the service stopped before the boundary");
            break;
        }
        return;
    }
    const struct mo_store_program *candidate = mo_live_program(live);
    size_t report_size = 0;
    const char *report = mo_live_activation(live, u->active.ordinal + 1, head, &report_size);
    const char *migration = mo_live_migration(live);
    struct mo_store_activation activation = {
        .previous = u->active,
        .candidate = *candidate,
        .previous_state = {MO_STORE_STATE, head, u->active.image, state, length},
        .candidate_state = {MO_STORE_STATE, head, candidate->image, migrated, migrated_length},
        .migration = migration,
        .migration_size = strlen(migration),
        .report_json = report,
        .report_size = report_size};
    enum mo_store_activation_status stored =
        report == NULL ? MO_STORE_ACTIVATION_REFUSED : mo_store_activate(s->store, &activation);
    if (stored != MO_STORE_ACTIVATED) {
        mo_service_swap_abandon(next);
        mo_process_free(catalog);
        if (stored == MO_STORE_ACTIVATION_UNSAFE) {
            /* The new program may be durable: the old one must never serve again. */
            mo_service_stop(s->service, MO_SERVICE_STORE);
            mo_live_refuse(live, MO_CODE_UPGRADE_INVALID_VALUE, "store",
                           "the activation outcome is unknown; the service stopped; restart it "
                           "to see which program is active");
        } else
            mo_live_refuse(live, MO_CODE_UPGRADE_INVALID_VALUE, "store",
                           "the activation transaction was rolled back; the old program serves");
        return;
    }
    struct mo_service_upgrade note = {.accepted = true, .ordinal = u->active.ordinal + 1};
    memcpy(note.old_source, u->active.source, 32);
    memcpy(note.new_source, candidate->source, 32);
    if (s->previous.image != NULL)
        release_previous(s);
    s->previous = (struct protocol_previous){.image = s->image,
                                             .owned = s->owned_image,
                                             .catalog = s->service_catalog,
                                             .epoch = s->epoch};
    s->owned_image = mo_live_take_image(live);
    s->image = s->owned_image;
    s->service_index = index;
    s->descriptor = &mo_image_bytecode(s->image)->services[index];
    s->service_catalog = catalog;
    s->cached_descriptor = descriptor;
    s->cached_catalog = prepared;
    s->epoch++;
    /* Re-admitted calls are answered by the new program against the new tool table. */
    for (size_t i = 0; i < MO_LIMIT_MCP_CONNECTIONS; i++)
        if (s->connections[i].phase == PS_SERVICE)
            s->connections[i].descriptor = descriptor;
    mo_export_release(&s->state_export);
    mo_export_release(&s->failure_export);
    struct mo_service_loop *old = s->service;
    s->service = next;
    s->service_heap = mo_service_heap(next);
    mo_service_swap_finish(old, next, &note);
    if (!copy_active(u, candidate)) {
        /* The row is durable; without its copy no later push can be checked, so stop. */
        mo_service_stop(s->service, MO_SERVICE_MEMORY);
        u->active_found = false;
    }
}

static void finish_commit(struct mo_protocol_server *s, struct protocol_connection *c,
                          bool activated) {
    struct mo_buffer json, text;
    mo_buffer_init(&json, MO_LIMIT_UPGRADE_REPORT_JSON_BYTES);
    mo_buffer_init(&text, MO_LIMIT_UPGRADE_REPORT_JSON_BYTES);
    bool rendered = mo_live_report(s->upgrade.live, true, &json) &&
                    mo_live_report(s->upgrade.live, false, &text);
    if (c != NULL && c->phase == PS_UPGRADE) {
        enum protocol_effect effect = activated ? EFFECT_APPLIED : EFFECT_NONE;
        if (rendered)
            reply(s, c, &json, &text, !activated, effect);
        else
            mo_protocol_fallback(s, c, PE_MEMORY, effect);
    }
    mo_buffer_free(&json);
    mo_buffer_free(&text);
}

void mo_protocol_upgrade_poll(struct mo_protocol_server *s) {
    struct protocol_upgrade *u = &s->upgrade;
    if (!u->open)
        return;
    if (u->compiling && mo_live_poll(u->live)) {
        u->compiling = false;
        u->touched = s->now.monotonic_us;
    }
    if (u->waiting == SIZE_MAX) {
        if (!u->compiling && s->now.monotonic_us - u->touched >= MO_LIMIT_UPGRADE_SLOT_IDLE_US)
            close_slot(s);
        return;
    }
    if (u->compiling)
        return;
    struct protocol_connection *c = &s->connections[u->waiting];
    /* A caller that disconnected still gets its commit: disconnection is not cancellation. */
    struct protocol_connection *caller = c->phase == PS_UPGRADE ? c : NULL;
    if (!mo_live_staged(u->live)) {
        refuse(s, NULL, mo_live_code(u->live), "compile", "", u->candidate);
        finish_commit(s, caller, false);
        close_slot(s);
        return;
    }
    /* Two programs at most: events and earlier requests keep the previous one; the boundary
     * waits until no request borrows it, and the activation then releases it. */
    if (s->previous.image != NULL && previous_borrowed(s)) {
        if (s->now.monotonic_us < u->wait_deadline)
            return;
        mo_live_refuse(u->live, MO_CODE_UPGRADE_BOUNDARY_DEADLINE, "boundary",
                       "requests from before the last upgrade still hold its program");
    } else
        activate(s);
    bool activated = mo_live_code(u->live) == 0;
    if (!activated)
        refuse(s, NULL, mo_live_code(u->live), "", "", u->candidate);
    finish_commit(s, caller, activated);
    close_slot(s);
    if (activated)
        mo_protocol_list_changed(s);
}

uint64_t mo_protocol_upgrade_wake(const struct mo_protocol_server *s, uint64_t until) {
    const struct protocol_upgrade *u = &s->upgrade;
    if (!u->open)
        return until;
    /* Compilation and a waiting commit are polled often; an idle slot wakes to close. */
    uint64_t wake = u->compiling || u->waiting != SIZE_MAX
                        ? mo_protocol_deadline(s->now.monotonic_us, MO_LIMIT_UPGRADE_POLL_US)
                        : mo_protocol_deadline(u->touched, MO_LIMIT_UPGRADE_SLOT_IDLE_US);
    return wake < until ? wake : until;
}

void mo_protocol_upgrade_free(struct mo_protocol_server *s) {
    if (s->upgrade.open)
        close_slot(s);
    mo_arena_free(&s->upgrade.active_arena);
}

enum mo_protocol_server_status mo_protocol_server_upgrades(struct mo_protocol_server *s,
                                                           const struct mo_bearer *credential,
                                                           const char *compiler) {
    MO_ASSERT(s != NULL && compiler != NULL);
    struct mo_arena arena;
    mo_arena_init_confidential(&arena, MO_LIMIT_UPGRADE_RESTART_HEAP);
    struct mo_store_program program;
    bool found = false;
    enum mo_store_status read = mo_store_program_read(s->store, &arena, &program, &found);
    bool ok = read == MO_STORE_OK && (!found || copy_active(&s->upgrade, &program));
    mo_arena_free(&arena);
    if (!ok)
        return read == MO_STORE_OK ? MO_PROTOCOL_SERVER_MEMORY : MO_PROTOCOL_SERVER_SERVICE;
    s->upgrade.compiler = compiler;
    s->upgrade_enabled = credential != NULL;
    if (credential != NULL)
        s->upgrade_bearer = *credential;
    return MO_PROTOCOL_SERVER_OK;
}
