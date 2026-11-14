#include "host/store-internal.h"

#include <string.h>

#include "base/compile-context.h"
#include "base/compile-metadata.h"
#include "base/export.h"
#include "base/sha256.h"
#include "base/utf8.h"

/* Upgrade tables are created only inside recording/activation transactions, so a
 * refused candidate cannot upgrade a legacy folder's on-disk format. */
static const struct store_table upgrade_tables[] = {
    {"program",
     "CREATE TABLE program(id INTEGER PRIMARY KEY CHECK(id=1), ordinal INTEGER NOT "
     "NULL, source BLOB NOT NULL, image BLOB NOT NULL, build BLOB NOT NULL, schema BLOB "
     "NOT NULL, bundle BLOB NOT NULL, context_digest BLOB NOT NULL, context BLOB NOT NULL) STRICT"},
    {"upgrades", "CREATE TABLE upgrades(ordinal INTEGER PRIMARY KEY, head INTEGER NOT NULL, "
                 "old_source BLOB NOT NULL, new_source BLOB NOT NULL, old_image BLOB NOT NULL, "
                 "new_image BLOB NOT NULL, old_schema BLOB NOT NULL, new_schema BLOB NOT NULL, "
                 "build BLOB NOT NULL, migration TEXT NOT NULL, report TEXT NOT NULL) STRICT"},
    {"previous_programs",
     "CREATE TABLE previous_programs(ordinal INTEGER PRIMARY KEY, source BLOB NOT NULL, image BLOB "
     "NOT NULL, build BLOB NOT NULL, schema BLOB NOT NULL, bundle BLOB NOT NULL, context_digest "
     "BLOB NOT NULL, context BLOB NOT NULL) STRICT"},
    {"previous_snapshots",
     "CREATE TABLE previous_snapshots(ordinal INTEGER PRIMARY KEY, record BLOB NOT NULL) STRICT"}};

static bool upgrade_tables_ready(struct mo_store *s) {
    for (size_t i = 0; i < sizeof upgrade_tables / sizeof *upgrade_tables; i++)
        if (!store_table_ready(s, &upgrade_tables[i]))
            return false;
    return true;
}

static void image_bytes(uint64_t image, uint8_t bytes[8]) {
    for (unsigned i = 0; i < 8; i++) {
        bytes[i] = (uint8_t)image;
        image >>= 8;
    }
}

static bool program_valid(const struct mo_store_program *p) {
    struct mo_bundle bundle;
    if (p->ordinal > INT64_MAX || !mo_bundle_read(p->bundle, p->bundle_size, &bundle))
        return false;
    struct mo_sha256 hash;
    uint8_t digest[32];
    mo_sha256_init(&hash);
    mo_sha256_update(&hash, p->bundle, p->bundle_size);
    mo_sha256_finish(&hash, digest);
    if (memcmp(p->source, digest, 32) != 0)
        return false;
    struct mo_compile_context context;
    if (!mo_compile_context_read(p->context, p->context_size, &context))
        return false;
    if (context.metadata.length != 0) {
        struct mo_compile_metadata metadata;
        if (!mo_compile_metadata_read(context.metadata.bytes, context.metadata.length, &metadata))
            return false;
        mo_sha256_init(&hash);
        mo_sha256_update(&hash, metadata.schema.bytes, metadata.schema.length);
        mo_sha256_finish(&hash, digest);
        if (memcmp(p->schema, digest, 32) != 0)
            return false;
    }
    mo_sha256_init(&hash);
    mo_sha256_update(&hash, p->context, p->context_size);
    mo_sha256_finish(&hash, digest);
    return memcmp(p->context_digest, digest, 32) == 0;
}

bool store_blob_size(sqlite3_stmt *stmt, int column, int length) {
    return sqlite3_column_type(stmt, column) == SQLITE_BLOB &&
           sqlite3_column_bytes(stmt, column) == length;
}

/* ordinal, source, image, build, schema, bundle. All spans remain statement-owned. */
bool store_program_row(sqlite3_stmt *stmt, struct mo_store_program *p) {
    if (sqlite3_column_type(stmt, 0) != SQLITE_INTEGER || sqlite3_column_int64(stmt, 0) < 0 ||
        !store_blob_size(stmt, 1, 32) || !store_blob_size(stmt, 2, 8) ||
        !store_blob_size(stmt, 3, 32) || !store_blob_size(stmt, 4, 32) ||
        sqlite3_column_type(stmt, 5) != SQLITE_BLOB || !store_blob_size(stmt, 6, 32) ||
        sqlite3_column_type(stmt, 7) != SQLITE_BLOB)
        return false;
    p->ordinal = (uint64_t)sqlite3_column_int64(stmt, 0);
    memcpy(p->source, sqlite3_column_blob(stmt, 1), 32);
    memcpy(p->build, sqlite3_column_blob(stmt, 3), 32);
    memcpy(p->schema, sqlite3_column_blob(stmt, 4), 32);
    const uint8_t *image = sqlite3_column_blob(stmt, 2);
    p->image = 0;
    for (unsigned i = 0; i < 8; i++)
        p->image |= (uint64_t)image[i] << (8 * i);
    p->bundle = sqlite3_column_blob(stmt, 5);
    p->bundle_size = (size_t)sqlite3_column_bytes(stmt, 5);
    memcpy(p->context_digest, sqlite3_column_blob(stmt, 6), 32);
    p->context = sqlite3_column_blob(stmt, 7);
    p->context_size = (size_t)sqlite3_column_bytes(stmt, 7);
    return program_valid(p);
}

static bool same_program(const struct mo_store_program *a, const struct mo_store_program *b) {
    return a->ordinal == b->ordinal && a->image == b->image &&
           memcmp(a->source, b->source, 32) == 0 && memcmp(a->build, b->build, 32) == 0 &&
           memcmp(a->schema, b->schema, 32) == 0 &&
           memcmp(a->context_digest, b->context_digest, 32) == 0;
}

enum mo_store_status store_read_program(struct mo_store *s, struct mo_arena *arena,
                                        struct mo_store_program *out, bool *found) {
    int code;
    enum table_state state = store_table_state(s, &upgrade_tables[0], &code);
    if (state == TABLE_ABSENT) {
        *found = false;
        return MO_STORE_OK;
    }
    if (state != TABLE_SAME)
        return state == TABLE_FAILED ? store_sqlite_status(code) : MO_STORE_CORRUPT;
    sqlite3_stmt *stmt = NULL;
    code =
        sqlite3_prepare_v2(s->db,
                           "SELECT ordinal,source,image,build,schema,bundle,context_digest,context "
                           "FROM program WHERE id=1",
                           -1, &stmt, NULL);
    if (code != SQLITE_OK)
        return store_sqlite_status(code);
    int step = sqlite3_step(stmt);
    struct mo_store_program next = {0};
    enum mo_store_status result = MO_STORE_OK;
    bool present = step == SQLITE_ROW;
    if (step != SQLITE_ROW && step != SQLITE_DONE)
        result = store_sqlite_status(step);
    else if (present && !store_program_row(stmt, &next))
        result = MO_STORE_CORRUPT;
    if (result == MO_STORE_OK && present && arena != NULL) {
        uint8_t *copy = mo_arena_allocate(arena, next.bundle_size + next.context_size, 1);
        if (copy == NULL)
            result = MO_STORE_MEMORY;
        else {
            memcpy(copy, next.bundle, next.bundle_size);
            memcpy(copy + next.bundle_size, next.context, next.context_size);
            next.bundle = copy;
            next.context = copy + next.bundle_size;
        }
    }
    if (arena == NULL)
        next.bundle = NULL; /* Identity-only comparison does not retain a row's pointer. */
    result = store_finish(stmt, result);
    if (result == MO_STORE_OK) {
        *found = present;
        if (present)
            *out = next;
    }
    return result;
}

struct program_copy {
    struct mo_arena *arena;
    struct mo_store_program program;
    bool found;
};

static enum mo_store_folder program_body(struct mo_store *s, bool failure_tables, void *context) {
    (void)failure_tables;
    struct program_copy *copy = context;
    return store_folder_status(store_read_program(s, copy->arena, &copy->program, &copy->found));
}

enum mo_store_folder mo_store_program_read_file(const char *path, struct mo_arena *arena,
                                                struct mo_store_program *out, bool *found) {
    MO_ASSERT(path != NULL && arena != NULL && out != NULL && found != NULL);
    struct program_copy copy = {.arena = arena};
    bool recovered = false;
    enum mo_store_folder status = store_read_folder(path, false, &recovered, program_body, &copy);
    MO_ASSERT(!recovered);
    if (status == MO_STORE_FOLDER_OK) {
        *found = copy.found;
        if (copy.found)
            *out = copy.program;
    }
    return status;
}

/* The retained programs whose image, build and schema match a failure record: the active row
 * and every previous program. The greatest ordinal is copied; distinct source digests among the
 * matches make the selection ambiguous. */
struct retained_copy {
    struct mo_arena *arena;
    uint64_t image;
    const uint8_t *build, *schema;
    struct mo_store_program program;
    uint32_t matches;
    bool distinct, memory;
};

static void consider_retained(struct retained_copy *c, const struct mo_store_program *p) {
    if (p->image != c->image || memcmp(p->build, c->build, 32) != 0 ||
        memcmp(p->schema, c->schema, 32) != 0)
        return;
    if (c->matches != 0 && memcmp(c->program.source, p->source, 32) != 0)
        c->distinct = true;
    c->matches++;
    if (c->matches != 1 && p->ordinal <= c->program.ordinal)
        return;
    uint8_t *copy = mo_arena_allocate(c->arena, p->bundle_size + p->context_size, 1);
    if (copy == NULL) {
        c->memory = true;
        return;
    }
    memcpy(copy, p->bundle, p->bundle_size);
    memcpy(copy + p->bundle_size, p->context, p->context_size);
    c->program = *p;
    c->program.bundle = copy;
    c->program.context = copy + p->bundle_size;
}

static enum mo_store_folder retained_body(struct mo_store *s, bool failure_tables, void *context) {
    (void)failure_tables;
    struct retained_copy *c = context;
    struct mo_store_program active;
    bool found = false;
    enum mo_store_status status = store_read_program(s, c->arena, &active, &found);
    if (status != MO_STORE_OK || !found)
        return store_folder_status(status);
    consider_retained(c, &active);
    sqlite3_stmt *stmt = NULL;
    int code =
        sqlite3_prepare_v2(s->db,
                           "SELECT ordinal,source,image,build,schema,bundle,context_digest,context "
                           "FROM previous_programs ORDER BY ordinal",
                           -1, &stmt, NULL);
    if (code != SQLITE_OK)
        return store_folder_status(store_sqlite_status(code));
    int step;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        struct mo_store_program program = {0};
        if (!store_program_row(stmt, &program)) {
            status = MO_STORE_CORRUPT;
            break;
        }
        consider_retained(c, &program);
    }
    if (status == MO_STORE_OK && step != SQLITE_DONE)
        status = store_sqlite_status(step);
    if (status == MO_STORE_OK && c->memory)
        status = MO_STORE_MEMORY;
    return store_folder_status(store_finish(stmt, status));
}

enum mo_store_folder mo_store_retained_program_read_file(const char *path, struct mo_arena *arena,
                                                         uint64_t image, const uint8_t build[32],
                                                         const uint8_t schema[32],
                                                         struct mo_store_program *out,
                                                         uint32_t *matches, bool *distinct) {
    MO_ASSERT(path != NULL && arena != NULL && out != NULL && matches != NULL && distinct != NULL);
    struct retained_copy copy = {.arena = arena, .image = image, .build = build, .schema = schema};
    bool recovered = false;
    enum mo_store_folder status = store_read_folder(path, false, &recovered, retained_body, &copy);
    MO_ASSERT(!recovered);
    if (status == MO_STORE_FOLDER_OK) {
        *matches = copy.matches;
        *distinct = copy.distinct;
        if (copy.matches != 0)
            *out = copy.program;
    }
    return status;
}

enum mo_store_status mo_store_program_read(struct mo_store *s, struct mo_arena *arena,
                                           struct mo_store_program *out, bool *found) {
    MO_ASSERT(s != NULL && arena != NULL && out != NULL && found != NULL);
    return s->unsafe ? MO_STORE_IO : store_read_program(s, arena, out, found);
}

static bool write_program(struct mo_store *s, const char *sql, const struct mo_store_program *p,
                          uint64_t ordinal) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;
    uint8_t image[8];
    image_bytes(p->image, image);
    int code = sqlite3_bind_int64(stmt, 1, (sqlite3_int64)ordinal);
    const void *blobs[] = {p->source,         image,     p->build, p->schema, p->bundle,
                           p->context_digest, p->context};
    int sizes[] = {32, 8, 32, 32, (int)p->bundle_size, 32, (int)p->context_size};
    for (int i = 0; code == SQLITE_OK && i < 7; i++)
        code = sqlite3_bind_blob(stmt, i + 2, blobs[i], sizes[i], SQLITE_STATIC);
    bool ok = code == SQLITE_OK && sqlite3_step(stmt) == SQLITE_DONE;
    return store_finish(stmt, ok ? MO_STORE_OK : MO_STORE_IO) == MO_STORE_OK;
}

static const char replace_program[] =
    "INSERT INTO program VALUES(1,?1,?2,?3,?4,?5,?6,?7,?8) ON CONFLICT(id) DO UPDATE SET "
    "ordinal=excluded.ordinal,source=excluded.source,image=excluded.image,build=excluded.build,"
    "schema=excluded.schema,bundle=excluded.bundle,context_digest=excluded.context_digest,context="
    "excluded.context";

static bool snapshot_matches(const struct mo_store_program *p, const struct mo_store_record *r) {
    uint8_t schema[32];
    return r->kind == MO_STORE_STATE && r->image == p->image &&
           mo_export_schema(r->bytes, r->length, schema) && memcmp(schema, p->schema, 32) == 0;
}

static bool replace_snapshot(struct mo_store *s, const struct mo_buffer *bytes, uint64_t head) {
    return store_write_blob(s,
                            "INSERT INTO snapshot VALUES(1,?2) ON CONFLICT(id) DO UPDATE SET "
                            "record=excluded.record",
                            head, bytes) &&
           store_exec(
               s, "INSERT INTO head VALUES(1,0) ON CONFLICT(id) DO NOTHING;DELETE FROM log;PRAGMA "
                  "user_version=3");
}

enum mo_store_status mo_store_program_record(struct mo_store *s,
                                             const struct mo_store_program *program,
                                             const struct mo_store_record *snapshot) {
    if (s->unsafe || !program_valid(program) || !store_items_valid(program) ||
        !snapshot_matches(program, snapshot))
        return MO_STORE_CORRUPT;
    struct mo_buffer bytes;
    mo_buffer_init(&bytes, MO_LIMIT_SAVED_BYTES + MO_STORE_FRAME_BYTES);
    if (!store_frame(snapshot, &bytes)) {
        mo_buffer_free(&bytes);
        return MO_STORE_MEMORY;
    }
    bool ok = store_exec(s, "BEGIN IMMEDIATE");
    if (ok) {
        uint64_t head = 0;
        bool empty, found;
        struct mo_store_program previous;
        ok = store_current_head(s, &head, &empty) == MO_STORE_OK && snapshot->sequence == head &&
             store_read_program(s, NULL, &previous, &found) == MO_STORE_OK &&
             (found ? previous.ordinal == program->ordinal &&
                          memcmp(previous.source, program->source, 32) == 0
                    : program->ordinal == 0) &&
             store_not_version_two(s) && upgrade_tables_ready(s) &&
             replace_snapshot(s, &bytes, head) &&
             write_program(s, replace_program, program, program->ordinal) &&
             store_review_write(s, program);
        if (ok)
            ok = store_exec(s, "COMMIT");
        if (!ok) {
            (void)store_exec(s, "ROLLBACK");
            s->unsafe = true;
        }
    }
    mo_buffer_free(&bytes);
    return ok ? MO_STORE_OK : MO_STORE_IO;
}

struct program_pin {
    const struct mo_store_program *program;
    bool pinned, valid;
};

static bool failure_pins_program(const struct mo_store_failure *failure, void *context) {
    struct program_pin *pin = context;
    struct mo_failure_record record;
    if (!mo_failure_read(failure->message.bytes, failure->message.length, &record)) {
        pin->valid = false;
        return true;
    }
    pin->pinned |= record.program == pin->program->image &&
                   memcmp(record.build, pin->program->build, 32) == 0 &&
                   memcmp(record.schema, pin->program->schema, 32) == 0;
    return true;
}

static bool delete_ordinal(struct mo_store *s, const char *sql, uint64_t ordinal) {
    sqlite3_stmt *stmt = NULL;
    int code = sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL);
    if (code == SQLITE_OK)
        code = sqlite3_bind_int64(stmt, 1, (sqlite3_int64)ordinal);
    bool ok = code == SQLITE_OK && sqlite3_step(stmt) == SQLITE_DONE;
    return store_finish(stmt, ok ? MO_STORE_OK : MO_STORE_IO) == MO_STORE_OK;
}

static bool retain_programs(struct mo_store *s, uint64_t ordinal) {
    sqlite3_stmt *stmt = NULL;
    int code = sqlite3_prepare_v2(
        s->db,
        "SELECT ordinal,source,image,build,schema,bundle,context_digest,context FROM "
        "previous_programs ORDER BY ordinal DESC",
        -1, &stmt, NULL);
    if (code != SQLITE_OK)
        return false;
    uint64_t remove[MO_LIMIT_FAILURE_RECORDS + MO_LIMIT_UPGRADE_PREVIOUS + 1];
    uint32_t count = 0, removed = 0;
    bool ok = true;
    int step;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        struct mo_store_program program;
        if (++count > MO_LIMIT_FAILURE_RECORDS + MO_LIMIT_UPGRADE_PREVIOUS + 1 ||
            !store_program_row(stmt, &program) || program.ordinal == 0 ||
            program.ordinal > ordinal) {
            ok = false;
            break;
        }
        if (count <= MO_LIMIT_UPGRADE_PREVIOUS)
            continue;
        struct program_pin pin = {&program, false, true};
        if (mo_store_failures(s, failure_pins_program, &pin) != MO_STORE_OK || !pin.valid) {
            ok = false;
            break;
        }
        if (!pin.pinned)
            remove[removed++] = program.ordinal;
    }
    ok = store_finish(stmt, ok && step == SQLITE_DONE ? MO_STORE_OK : MO_STORE_CORRUPT) ==
         MO_STORE_OK;
    for (uint32_t i = 0; ok && i < removed; i++)
        ok = delete_ordinal(s, "DELETE FROM previous_programs WHERE ordinal=?1", remove[i]);
    if (ok && ordinal > MO_LIMIT_UPGRADE_PREVIOUS)
        ok = delete_ordinal(s, "DELETE FROM previous_snapshots WHERE ordinal<=?1",
                            ordinal - MO_LIMIT_UPGRADE_PREVIOUS);
    if (!ok)
        return false;
    stmt = NULL;
    code = sqlite3_prepare_v2(
        s->db, "SELECT coalesce(sum(length(record)),0) FROM previous_snapshots", -1, &stmt, NULL);
    ok = code == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW &&
         sqlite3_column_int64(stmt, 0) >= 0 &&
         sqlite3_column_int64(stmt, 0) <= MO_LIMIT_UPGRADE_RETAINED_BYTES;
    return store_finish(stmt, ok ? MO_STORE_OK : MO_STORE_CORRUPT) == MO_STORE_OK;
}

static bool write_upgrade(struct mo_store *s, const struct mo_store_activation *a) {
    sqlite3_stmt *stmt = NULL;
    int code = sqlite3_prepare_v2(
        s->db, "INSERT INTO upgrades VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)", -1, &stmt, NULL);
    uint8_t old_image[8], new_image[8];
    image_bytes(a->previous.image, old_image);
    image_bytes(a->candidate.image, new_image);
    if (code == SQLITE_OK)
        code = sqlite3_bind_int64(stmt, 1, (sqlite3_int64)a->candidate.ordinal);
    if (code == SQLITE_OK)
        code = sqlite3_bind_int64(stmt, 2, (sqlite3_int64)a->candidate_state.sequence);
    const void *blobs[] = {a->previous.source, a->candidate.source, old_image,         new_image,
                           a->previous.schema, a->candidate.schema, a->candidate.build};
    const int sizes[] = {32, 32, 8, 8, 32, 32, 32};
    for (int i = 0; code == SQLITE_OK && i < 7; i++)
        code = sqlite3_bind_blob(stmt, i + 3, blobs[i], sizes[i], SQLITE_STATIC);
    if (code == SQLITE_OK)
        code = sqlite3_bind_text(stmt, 10, a->migration, (int)a->migration_size, SQLITE_STATIC);
    if (code == SQLITE_OK)
        code = sqlite3_bind_text(stmt, 11, a->report_json, (int)a->report_size, SQLITE_STATIC);
    bool ok = code == SQLITE_OK && sqlite3_step(stmt) == SQLITE_DONE;
    return store_finish(stmt, ok ? MO_STORE_OK : MO_STORE_IO) == MO_STORE_OK;
}

struct adoption_snapshot {
    const struct mo_store_record *expected;
    bool matched;
};

static bool compare_adoption_snapshot(const struct mo_store_record *r, void *context) {
    struct adoption_snapshot *check = context;
    const struct mo_store_record *e = check->expected;
    check->matched = r->kind == e->kind && r->sequence == e->sequence && r->image == e->image &&
                     r->length == e->length && memcmp(r->bytes, e->bytes, r->length) == 0;
    return check->matched;
}

static enum mo_store_activation_status
activate(struct mo_store *s, const struct mo_store_activation *a, bool adoption, bool legacy) {
    if (s->unsafe)
        return MO_STORE_ACTIVATION_UNSAFE;
    static const uint8_t unavailable[32] = {0};
    if ((!legacy && !program_valid(&a->previous)) || !program_valid(&a->candidate) ||
        !store_items_valid(&a->candidate) ||
        (legacy && (a->previous.ordinal != 0 ||
                    memcmp(a->previous.source, unavailable, sizeof unavailable) != 0)) ||
        (adoption && memcmp(a->previous.schema, a->candidate.schema, 32) != 0) ||
        a->previous.ordinal == INT64_MAX || a->candidate.ordinal != a->previous.ordinal + 1 ||
        a->migration == NULL || a->report_json == NULL ||
        a->migration_size > MO_LIMIT_UPGRADE_MIGRATION_NAME ||
        a->report_size > MO_LIMIT_UPGRADE_REPORT_JSON_BYTES ||
        !mo_utf8_valid((const uint8_t *)a->migration, a->migration_size) ||
        !mo_utf8_valid((const uint8_t *)a->report_json, a->report_size) ||
        !snapshot_matches(&a->previous, &a->previous_state) ||
        !snapshot_matches(&a->candidate, &a->candidate_state) ||
        (adoption ? a->previous_state.sequence > a->candidate_state.sequence
                  : a->previous_state.sequence != a->candidate_state.sequence))
        return MO_STORE_ACTIVATION_REFUSED;
    struct mo_buffer previous, candidate;
    mo_buffer_init(&previous, MO_LIMIT_SAVED_BYTES + MO_STORE_FRAME_BYTES);
    mo_buffer_init(&candidate, MO_LIMIT_SAVED_BYTES + MO_STORE_FRAME_BYTES);
    bool framed =
        store_frame(&a->previous_state, &previous) && store_frame(&a->candidate_state, &candidate);
    enum mo_store_activation_status result = MO_STORE_ACTIVATION_REFUSED;
    if (!framed || !store_exec(s, "BEGIN IMMEDIATE"))
        goto done;
    uint64_t head = 0;
    bool empty = true, found = false;
    struct mo_store_program current;
    bool base_read = store_current_head(s, &head, &empty) == MO_STORE_OK && !empty &&
                     store_read_program(s, NULL, &current, &found) == MO_STORE_OK;
    int version = -1;
    struct adoption_snapshot original = {.expected = &a->previous_state};
    struct saved_head saved;
    bool verified_snapshot = !adoption || (store_load_snapshot(s, &saved, compare_adoption_snapshot,
                                                               &original) == MO_STORE_OK &&
                                           original.matched);
    bool ok =
        base_read && head == a->candidate_state.sequence && verified_snapshot &&
        (legacy ? !found : found && same_program(&current, &a->previous)) &&
        store_version(s, &version) == MO_STORE_OK && upgrade_tables_ready(s) &&
        (legacy || write_program(s, "INSERT INTO previous_programs VALUES(?1,?2,?3,?4,?5,?6,?7,?8)",
                                 &a->previous, a->candidate.ordinal)) &&
        store_write_blob(s, "INSERT INTO previous_snapshots VALUES(?1,?2)", a->candidate.ordinal,
                         &previous) &&
        retain_programs(s, a->candidate.ordinal) && replace_snapshot(s, &candidate, head) &&
        write_program(s, replace_program, &a->candidate, a->candidate.ordinal) &&
        write_upgrade(s, a) && store_review_write(s, &a->candidate) &&
        (version != 2 ||
         store_decide(s, a->time, "conversion", a->previous.source, "everything", ""));
    if (ok) {
        if (store_exec(s, "COMMIT"))
            result = MO_STORE_ACTIVATED;
        else {
            s->unsafe = true;
            (void)store_exec(s, "ROLLBACK");
            result = MO_STORE_ACTIVATION_UNSAFE;
        }
    } else {
        uint64_t restored = 0;
        bool restored_empty = true, restored_found = false;
        struct mo_store_program restored_program;
        if (!store_exec(s, "ROLLBACK") ||
            store_current_head(s, &restored, &restored_empty) != MO_STORE_OK || restored_empty ||
            restored != head ||
            store_read_program(s, NULL, &restored_program, &restored_found) != MO_STORE_OK ||
            !base_read || restored_found != found ||
            (found && !same_program(&restored_program, &current))) {
            s->unsafe = true;
            result = MO_STORE_ACTIVATION_UNSAFE;
        }
    }
done:
    mo_buffer_free(&previous);
    mo_buffer_free(&candidate);
    return result;
}

enum mo_store_activation_status mo_store_activate(struct mo_store *s,
                                                  const struct mo_store_activation *a) {
    return activate(s, a, false, false);
}

enum mo_store_activation_status mo_store_adopt(struct mo_store *s,
                                               const struct mo_store_activation *a, bool legacy) {
    return activate(s, a, true, legacy);
}
