#include "base/bundle.h"
#include "base/codes.h"
#include "base/sha256.h"
#include "tools/upgrade-internal.h"
#include "vm/vm.h"
#include <string.h>

/* A candidate pushed to a running service: compiled by a child while the service keeps serving,
 * then admitted against the state current at the boundary with the live migration limits. */
struct mo_live_upgrade {
    struct mo_arena arena;
    struct compile_job job;
    struct mo_buffer bundle, previous, context, migrated, json;
    char migration[MO_LIMIT_UPGRADE_MIGRATION_NAME + 1];
    bool has_migration, compiling, staged;
    struct upgrade_image next;
    struct upgrade_report report;
};

static void live_free_buffers(struct mo_live_upgrade *u) {
    mo_upgrade_compile_cleanup(&u->job);
    if (u->next.image != NULL)
        mo_image_free(u->next.image);
    mo_upgrade_image_buffers_free(&u->next);
    mo_buffer_free(&u->report.differences);
    mo_buffer_free(&u->bundle);
    mo_buffer_free(&u->previous);
    mo_buffer_free(&u->context);
    mo_buffer_free(&u->migrated);
    mo_buffer_free(&u->json);
}

struct mo_live_upgrade *mo_live_start(const char *compiler, const struct mo_store_program *active,
                                      const uint8_t *bundle, size_t size, const char *migration) {
    MO_ASSERT(compiler != NULL && active != NULL && bundle != NULL);
    struct mo_arena arena;
    mo_arena_init(&arena, sizeof(struct mo_live_upgrade) + MO_ARENA_CHUNK_HEADER);
    struct mo_live_upgrade *u =
        mo_arena_allocate(&arena, sizeof *u, _Alignof(struct mo_live_upgrade));
    if (u == NULL) {
        mo_arena_free(&arena);
        return NULL;
    }
    *u = (struct mo_live_upgrade){.arena = arena,
                                  .report = {.class = "compile", .message = "compiling"}};
    mo_buffer_init(&u->bundle, MO_LIMIT_BUNDLE_BYTES);
    mo_buffer_init(&u->previous, MO_LIMIT_BUNDLE_BYTES);
    mo_buffer_init(&u->context, MO_LIMIT_BUNDLE_BYTES);
    mo_buffer_init(&u->migrated, MO_LIMIT_SAVED_BYTES);
    mo_buffer_init(&u->json, MO_LIMIT_UPGRADE_REPORT_JSON_BYTES);
    mo_upgrade_image_buffers_init(&u->next);
    mo_buffer_init(&u->report.differences, MO_LIMIT_UPGRADE_REPORT_JSON_BYTES);
    struct mo_sha256 hash;
    mo_sha256_init(&hash);
    mo_sha256_update(&hash, bundle, size);
    mo_sha256_finish(&hash, u->report.candidate);
    memcpy(u->report.base, active->source, 32);
    u->report.have_base = u->report.have_candidate = true;
    u->has_migration = migration != NULL;
    if (migration != NULL) {
        size_t n = strlen(migration);
        MO_ASSERT(n <= MO_LIMIT_UPGRADE_MIGRATION_NAME);
        memcpy(u->migration, migration, n + 1);
    }
    if (!mo_buffer_append(&u->bundle, bundle, size) ||
        (u->has_migration &&
         !mo_buffer_append(&u->previous, active->bundle, active->bundle_size)) ||
        !mo_buffer_append(&u->context, active->context, active->context_size)) {
        mo_live_free(u);
        return NULL;
    }
    struct mo_bundle checked;
    if (!mo_bundle_read(bundle, size, &checked))
        (void)mo_upgrade_refuse(&u->report, MO_CODE_UPGRADE_BUNDLE_FORMAT, "bundle",
                                "source bundle is malformed");
    else
        /* A tool-only candidate inherits the recorded context; a migration also gets the old
         * bundle so the child generates its input types from the running program. */
        u->compiling = mo_upgrade_compile_begin(
            compiler, &u->bundle, u->has_migration ? &u->previous : NULL,
            u->has_migration ? u->migration : NULL, &u->context, &u->job, &u->report);
    return u;
}

bool mo_live_poll(struct mo_live_upgrade *u) {
    if (!u->compiling)
        return true;
    enum mo_compile_status status = mo_upgrade_compile_poll(&u->job);
    if (status == MO_COMPILE_RUNNING)
        return false;
    u->compiling = false;
    mo_upgrade_image_buffers_free(&u->next);
    u->staged = mo_upgrade_compile_end(&u->job, status, &u->bundle, &u->next, &u->report);
    return true;
}

uint16_t mo_live_code(const struct mo_live_upgrade *u) {
    return u->report.code;
}

bool mo_live_staged(const struct mo_live_upgrade *u) {
    return u->staged && u->report.code == 0;
}

bool mo_live_admit(struct mo_live_upgrade *u, const struct mo_image *running, uint32_t index,
                   const struct mo_store_program *active, const uint8_t *state, size_t length) {
    MO_ASSERT(mo_live_staged(u));
    struct upgrade_image old = {.running = running, .service = index, .program = *active};
    memcpy(u->report.base, active->source, 32);
    mo_buffer_free(&u->report.differences);
    mo_buffer_clear(&u->migrated);
    const char *migration = u->has_migration ? u->migration : NULL;
    bool ok = mo_upgrade_compare_programs(&old, &u->next, migration, &u->report) &&
              mo_upgrade_migration_state(&old, &u->next, migration, state, length, &u->migrated,
                                         &u->report, true);
    if (ok) {
        u->report.class = "compatible";
        u->report.message = migration == NULL ? "upgrade activated; state schema unchanged"
                                              : "upgrade activated; migrated state validated";
    }
    return ok;
}

void mo_live_refuse(struct mo_live_upgrade *u, uint16_t code, const char *class,
                    const char *message) {
    (void)mo_upgrade_refuse(&u->report, code, class, message);
    u->report.activated = false;
}

const char *mo_live_activation(struct mo_live_upgrade *u, uint64_t ordinal, uint64_t head,
                               size_t *size) {
    u->report.activated = true;
    u->report.ordinal = ordinal;
    u->report.head = head;
    u->next.program.ordinal = ordinal;
    mo_buffer_clear(&u->json);
    if (!mo_upgrade_report_json(&u->json, &u->report, u->has_migration ? u->migration : NULL))
        return NULL;
    *size = u->json.length;
    return (const char *)u->json.bytes;
}

const struct mo_store_program *mo_live_program(const struct mo_live_upgrade *u) {
    return &u->next.program;
}

const uint8_t *mo_live_state(const struct mo_live_upgrade *u, size_t *length) {
    *length = u->migrated.length;
    return u->migrated.bytes;
}

const struct mo_image *mo_live_image(const struct mo_live_upgrade *u, uint32_t *service) {
    *service = u->next.service;
    return u->next.image;
}

struct mo_image *mo_live_take_image(struct mo_live_upgrade *u) {
    struct mo_image *image = u->next.image;
    u->next.image = NULL;
    return image;
}

const char *mo_live_migration(const struct mo_live_upgrade *u) {
    return u->has_migration ? u->migration : "";
}

bool mo_live_report(const struct mo_live_upgrade *u, bool json, struct mo_buffer *out) {
    return json ? mo_upgrade_report_json(out, &u->report, u->has_migration ? u->migration : NULL)
                : mo_upgrade_report_text(out, &u->report);
}

void mo_live_free(struct mo_live_upgrade *u) {
    if (u == NULL)
        return;
    live_free_buffers(u);
    struct mo_arena arena = u->arena;
    mo_arena_free(&arena);
}

bool mo_live_refusal(struct mo_buffer *out, bool json, uint16_t code, const char *class,
                     const char *message, const uint8_t *base, const uint8_t *candidate,
                     const char *migration) {
    struct upgrade_report r = {.code = code, .class = class, .message = message};
    if (base != NULL) {
        memcpy(r.base, base, 32);
        r.have_base = true;
    }
    if (candidate != NULL) {
        memcpy(r.candidate, candidate, 32);
        r.have_candidate = true;
    }
    r.already_active = code == 0;
    return json ? mo_upgrade_report_json(out, &r, migration) : mo_upgrade_report_text(out, &r);
}
