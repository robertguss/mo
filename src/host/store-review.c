#include "base/utf8.h"
#include "host/store-internal.h"

#include <stdlib.h>
#include <string.h>

static const struct store_table review_tables[] = {
    {"items", "CREATE TABLE items(id INTEGER PRIMARY KEY CHECK(id=1), ordinal INTEGER NOT NULL, "
              "list BLOB NOT NULL) STRICT"},
    {"approvals", "CREATE TABLE approvals(digest BLOB PRIMARY KEY, version INTEGER NOT NULL, "
                  "review BLOB NOT NULL) STRICT"},
    {"review", "CREATE TABLE review(id INTEGER PRIMARY KEY CHECK(id=1), setting TEXT NOT NULL, "
               "version INTEGER NOT NULL) STRICT"},
    {"pending", "CREATE TABLE pending(id INTEGER PRIMARY KEY CHECK(id=1), review BLOB NOT NULL, "
                "base BLOB NOT NULL, candidate BLOB NOT NULL, migration TEXT NOT NULL, version "
                "INTEGER NOT NULL, difference TEXT NOT NULL, bundle BLOB NOT NULL) STRICT"},
    {"decisions",
     "CREATE TABLE decisions(seq INTEGER PRIMARY KEY, time INTEGER NOT NULL, kind TEXT "
     "NOT NULL, base BLOB NOT NULL, candidate BLOB NOT NULL, review BLOB NOT NULL, "
     "setting TEXT NOT NULL, unattended INTEGER NOT NULL, detail TEXT NOT NULL) "
     "STRICT"}};

enum { REVIEW_TABLES = sizeof review_tables / sizeof *review_tables };

static bool review_tables_ready(struct mo_store *s) {
    for (size_t i = 0; i < REVIEW_TABLES; i++)
        if (!store_table_ready(s, &review_tables[i]))
            return false;
    return true;
}

bool store_items_valid(const struct mo_store_program *p) {
    struct mo_intent_list list;
    return p->items != NULL && mo_intent_list_read(p->items, p->items_size, &list);
}

static bool bind_blobs(sqlite3_stmt *stmt, int first, const void *const *blobs, const int *sizes,
                       int count) {
    int code = SQLITE_OK;
    for (int i = 0; code == SQLITE_OK && i < count; i++)
        code = sqlite3_bind_blob(stmt, first + i, blobs[i], sizes[i], SQLITE_STATIC);
    return code == SQLITE_OK;
}

static bool step_done(sqlite3_stmt *stmt, bool bound) {
    bool ok = bound && sqlite3_step(stmt) == SQLITE_DONE;
    return store_finish(stmt, ok ? MO_STORE_OK : MO_STORE_IO) == MO_STORE_OK;
}

static bool write_items(struct mo_store *s, const struct mo_store_program *p) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db,
                           "INSERT INTO items VALUES(1,?1,?2) ON CONFLICT(id) DO UPDATE SET "
                           "ordinal=excluded.ordinal,list=excluded.list",
                           -1, &stmt, NULL) != SQLITE_OK)
        return false;
    const void *blobs[] = {p->items};
    const int sizes[] = {(int)p->items_size};
    return step_done(stmt, sqlite3_bind_int64(stmt, 1, (sqlite3_int64)p->ordinal) == SQLITE_OK &&
                               bind_blobs(stmt, 2, blobs, sizes, 1));
}

static enum mo_store_status read_items(struct mo_store *s, struct mo_arena *arena, uint64_t ordinal,
                                       struct mo_intent_list *out) {
    sqlite3_stmt *stmt = NULL;
    int code =
        sqlite3_prepare_v2(s->db, "SELECT ordinal,list FROM items WHERE id=1", -1, &stmt, NULL);
    if (code != SQLITE_OK)
        return store_sqlite_status(code);
    int step = sqlite3_step(stmt);
    enum mo_store_status result = MO_STORE_CORRUPT;
    struct mo_intent_list list;
    if (step != SQLITE_ROW && step != SQLITE_DONE)
        result = store_sqlite_status(step);
    else if (step == SQLITE_ROW && sqlite3_column_type(stmt, 0) == SQLITE_INTEGER &&
             sqlite3_column_int64(stmt, 0) == (sqlite3_int64)ordinal &&
             sqlite3_column_type(stmt, 1) == SQLITE_BLOB &&
             mo_intent_list_read(sqlite3_column_blob(stmt, 1),
                                 (size_t)sqlite3_column_bytes(stmt, 1), &list)) {
        uint8_t *copy = mo_arena_allocate(arena, list.length, 1);
        result = copy == NULL ? MO_STORE_MEMORY : MO_STORE_OK;
        if (copy != NULL) {
            memcpy(copy, list.bytes, list.length);
            list.bytes = copy;
            *out = list;
        }
    }
    return store_finish(stmt, result);
}

static int64_t review_count(struct mo_store *s, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    int64_t n = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : -1;
    return store_finish(stmt, MO_STORE_OK) == MO_STORE_OK ? n : -1;
}

bool store_decide(struct mo_store *s, const struct mo_store_decision *d) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, "INSERT INTO decisions VALUES(NULL,?1,?2,?3,?4,?5,?6,?7,?8)", -1,
                           &stmt, NULL) != SQLITE_OK)
        return false;
    const void *blobs[] = {d->base, d->candidate, d->review};
    const int sizes[] = {32, d->has_candidate ? 32 : 0, d->has_review ? 32 : 0};
    bool bound = sqlite3_bind_int64(stmt, 1, d->time) == SQLITE_OK &&
                 sqlite3_bind_text(stmt, 2, d->kind, -1, SQLITE_STATIC) == SQLITE_OK &&
                 bind_blobs(stmt, 3, blobs, sizes, 3) &&
                 sqlite3_bind_text(stmt, 6, mo_store_setting_name(d->intent), -1, SQLITE_STATIC) ==
                     SQLITE_OK &&
                 sqlite3_bind_int(stmt, 7, d->unattended) == SQLITE_OK &&
                 sqlite3_bind_text(stmt, 8, d->detail == NULL ? "" : d->detail, -1,
                                   SQLITE_STATIC) == SQLITE_OK;
    return step_done(stmt, bound);
}

enum mo_store_status store_read_setting(struct mo_store *s, struct mo_store_review *out) {
    sqlite3_stmt *stmt = NULL;
    int code =
        sqlite3_prepare_v2(s->db, "SELECT setting,version FROM review WHERE id=1", -1, &stmt, NULL);
    if (code != SQLITE_OK)
        return store_sqlite_status(code);
    int step = sqlite3_step(stmt);
    enum mo_store_status result = MO_STORE_CORRUPT;
    const unsigned char *setting = step == SQLITE_ROW ? sqlite3_column_text(stmt, 0) : NULL;
    if (step != SQLITE_ROW && step != SQLITE_DONE)
        result = store_sqlite_status(step);
    else if (setting != NULL && sqlite3_column_type(stmt, 1) == SQLITE_INTEGER &&
             sqlite3_column_int64(stmt, 1) >= 0 &&
             (strcmp((const char *)setting, "everything") == 0 ||
              strcmp((const char *)setting, "intent") == 0)) {
        out->intent = setting[0] == 'i';
        out->version = (uint64_t)sqlite3_column_int64(stmt, 1);
        result = MO_STORE_OK;
    }
    return store_finish(stmt, result);
}

static int digest_order(const void *a, const void *b) {
    return memcmp(a, b, 32);
}

static uint8_t (*sorted_digests(struct mo_arena *arena, const struct mo_intent_list *list))[32] {
    uint8_t(*digests)[32] =
        mo_arena_allocate_array(arena, list->count == 0 ? 1 : list->count, 32, _Alignof(uint8_t));
    if (digests == NULL)
        return NULL;
    struct mo_binary cursor = mo_intent_list_first(list);
    struct mo_intent_entry entry;
    for (uint32_t i = 0; i < list->count; i++) {
        MO_ASSERT(mo_intent_list_next(list, &cursor, &entry));
        memcpy(digests[i], entry.digest, 32);
    }
    qsort(digests, list->count, 32, digest_order);
    return digests;
}

static bool listed(const uint8_t (*digests)[32], uint32_t count, const void *digest) {
    return bsearch(digest, digests, count, 32, digest_order) != NULL;
}

bool mo_store_review_approved(const struct mo_store_review *review, const uint8_t digest[32]) {
    return listed((const uint8_t(*)[32])review->approved, review->approved_count, digest);
}

const char *mo_store_setting_name(bool intent) {
    return intent ? "intent" : "everything";
}

static enum mo_store_status read_approvals(struct mo_store *s, struct mo_arena *arena,
                                           struct mo_store_review *out) {
    sqlite3_stmt *stmt = NULL;
    int code =
        sqlite3_prepare_v2(s->db, "SELECT digest FROM approvals ORDER BY digest", -1, &stmt, NULL);
    if (code != SQLITE_OK)
        return store_sqlite_status(code);
    uint8_t(*approved)[32] =
        mo_arena_allocate_array(arena, MO_LIMIT_INTENT_ITEMS, 32, _Alignof(uint8_t));
    uint8_t(*items)[32] = sorted_digests(arena, &out->items);
    enum mo_store_status result = approved == NULL || items == NULL ? MO_STORE_MEMORY : MO_STORE_OK;
    uint32_t count = 0;
    int step = SQLITE_DONE;
    while (result == MO_STORE_OK && (step = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (count == MO_LIMIT_INTENT_ITEMS || !store_blob_size(stmt, 0, 32) ||
            !listed((const uint8_t(*)[32])items, out->items.count, sqlite3_column_blob(stmt, 0)))
            result = MO_STORE_CORRUPT;
        else
            memcpy(approved[count++], sqlite3_column_blob(stmt, 0), 32);
    }
    if (result == MO_STORE_OK && step != SQLITE_DONE)
        result = store_sqlite_status(step);
    out->approved = approved;
    out->approved_count = count;
    return store_finish(stmt, result);
}

enum mo_store_status mo_store_review_read(struct mo_store *s, struct mo_arena *arena,
                                          struct mo_store_review *out) {
    MO_ASSERT(s != NULL && arena != NULL && out != NULL);
    if (s->unsafe)
        return MO_STORE_IO;
    int version;
    bool found = false;
    struct mo_store_review next = {0};
    enum mo_store_status result = store_version(s, &version);
    if (result == MO_STORE_OK)
        result = store_read_program(s, NULL, &next.program, &found);
    if (result != MO_STORE_OK || version != 3) {
        if (result == MO_STORE_OK)
            *out = next;
        return result;
    }
    if (!found)
        return MO_STORE_CORRUPT;
    next.reviewed = true;
    result = read_items(s, arena, next.program.ordinal, &next.items);
    if (result == MO_STORE_OK)
        result = store_read_setting(s, &next);
    if (result == MO_STORE_OK)
        result = read_approvals(s, arena, &next);
    int64_t records = result == MO_STORE_OK ? review_count(s, "SELECT count(*) FROM decisions") : 0;
    int64_t pending = result == MO_STORE_OK ? review_count(s, "SELECT count(*) FROM pending") : 0;
    if (records < 0 || pending < 0)
        result = MO_STORE_IO;
    if (result == MO_STORE_OK) {
        next.records = (uint64_t)records;
        next.pending = pending != 0;
        *out = next;
    }
    return result;
}

static bool retire_approvals(struct mo_store *s, const struct mo_store_program *p) {
    struct mo_intent_list list;
    sqlite3_stmt *stmt = NULL;
    if (!mo_intent_list_read(p->items, p->items_size, &list) ||
        sqlite3_prepare_v2(s->db, "SELECT digest FROM approvals", -1, &stmt, NULL) != SQLITE_OK)
        return false;
    struct mo_arena arena;
    mo_arena_init(&arena, 2 * (size_t)MO_LIMIT_INTENT_ITEMS * 32 + 4 * MO_ARENA_CHUNK_HEADER);
    uint8_t(*items)[32] = sorted_digests(&arena, &list);
    uint8_t(*retired)[32] =
        mo_arena_allocate_array(&arena, MO_LIMIT_INTENT_ITEMS, 32, _Alignof(uint8_t));
    uint32_t count = 0;
    bool ok = items != NULL && retired != NULL;
    int step = SQLITE_DONE;
    while (ok && (step = sqlite3_step(stmt)) == SQLITE_ROW) {
        ok = count < MO_LIMIT_INTENT_ITEMS && store_blob_size(stmt, 0, 32);
        if (ok && !listed((const uint8_t(*)[32])items, list.count, sqlite3_column_blob(stmt, 0)))
            memcpy(retired[count++], sqlite3_column_blob(stmt, 0), 32);
    }
    ok = store_finish(stmt, ok && step == SQLITE_DONE ? MO_STORE_OK : MO_STORE_CORRUPT) ==
         MO_STORE_OK;
    for (uint32_t i = 0; ok && i < count; i++) {
        stmt = NULL;
        const void *blobs[] = {retired[i]};
        const int sizes[] = {32};
        ok = sqlite3_prepare_v2(s->db, "DELETE FROM approvals WHERE digest=?1", -1, &stmt, NULL) ==
                 SQLITE_OK &&
             step_done(stmt, bind_blobs(stmt, 1, blobs, sizes, 1));
    }
    ok = ok && (count == 0 || store_exec(s, "UPDATE review SET version=version+1 WHERE id=1"));
    mo_arena_free(&arena);
    return ok;
}

static bool review_ready(struct mo_store *s) {
    return review_tables_ready(s) &&
           store_exec(s, "INSERT INTO review VALUES(1,'everything',0) ON CONFLICT(id) DO NOTHING");
}

bool store_review_write(struct mo_store *s, const struct mo_store_program *p) {
    return review_ready(s) && write_items(s, p) && retire_approvals(s, p);
}

bool store_not_version_two(struct mo_store *s) {
    int version;
    return store_version(s, &version) == MO_STORE_OK && version != 2;
}

static void hex(const uint8_t *bytes, size_t count, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < count; i++) {
        out[2 * i] = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 15];
    }
    out[2 * count] = 0;
}

static enum mo_store_review_status review_end(struct mo_store *s,
                                              enum mo_store_review_status result) {
    if (result == MO_STORE_REVIEW_OK && store_exec(s, "COMMIT"))
        return result;
    (void)store_exec(s, "ROLLBACK");
    return result == MO_STORE_REVIEW_OK ? MO_STORE_REVIEW_FAILED : result;
}

static enum mo_store_review_status review_begin(struct mo_store *s, struct mo_arena *arena,
                                                struct mo_store_review *review, bool drops) {
    if (s->unsafe || !store_exec(s, "BEGIN IMMEDIATE"))
        return MO_STORE_REVIEW_FAILED;
    if (mo_store_review_read(s, arena, review) != MO_STORE_OK || !review->reviewed)
        return review_end(s, MO_STORE_REVIEW_FAILED);
    uint64_t rows = drops && review->pending ? 2 : 1;
    return review->records + rows > MO_LIMIT_REVIEW_RECORDS ? review_end(s, MO_STORE_REVIEW_FULL)
                                                            : MO_STORE_REVIEW_OK;
}

enum mo_store_review_status mo_store_approve(struct mo_store *s, const uint8_t program[32],
                                             const uint8_t digest[32], int64_t time) {
    MO_ASSERT(s != NULL);
    struct mo_arena arena;
    mo_arena_init(&arena, MO_STORE_REVIEW_READ_BYTES);
    struct mo_store_review review;
    enum mo_store_review_status result = review_begin(s, &arena, &review, true);
    if (result == MO_STORE_REVIEW_OK) {
        char text[65];
        hex(digest, 32, text);
        if (memcmp(review.program.source, program, 32) != 0)
            result = review_end(s, MO_STORE_REVIEW_STALE);
        else if (!mo_intent_list_has(&review.items, digest))
            result = review_end(s, MO_STORE_REVIEW_ABSENT);
        else if (mo_store_review_approved(&review, digest))
            result = review_end(s, MO_STORE_REVIEW_OK);
        else {
            struct mo_store_decision decision = {
                .kind = "approval", .detail = text, .intent = review.intent, .time = time};
            memcpy(decision.base, program, 32);
            sqlite3_stmt *stmt = NULL;
            const void *blobs[] = {digest};
            const int sizes[] = {32};
            bool ok =
                sqlite3_prepare_v2(s->db, "INSERT INTO approvals VALUES(?1,?2,x'')", -1, &stmt,
                                   NULL) == SQLITE_OK &&
                step_done(stmt, bind_blobs(stmt, 1, blobs, sizes, 1) &&
                                    sqlite3_bind_int64(
                                        stmt, 2, (sqlite3_int64)review.version + 1) == SQLITE_OK) &&
                store_exec(s, "UPDATE review SET version=version+1 WHERE id=1") &&
                store_decide(s, &decision) && store_drop_stale(s, time);
            result = review_end(s, ok ? MO_STORE_REVIEW_OK : MO_STORE_REVIEW_FAILED);
        }
    }
    mo_arena_free(&arena);
    return result;
}

enum mo_store_review_status mo_store_set_review(struct mo_store *s, bool intent, int64_t time) {
    MO_ASSERT(s != NULL);
    struct mo_arena arena;
    mo_arena_init(&arena, MO_STORE_REVIEW_READ_BYTES);
    struct mo_store_review review;
    enum mo_store_review_status result = review_begin(s, &arena, &review, false);
    if (result == MO_STORE_REVIEW_OK && review.intent == intent)
        result = review_end(s, MO_STORE_REVIEW_OK);
    else if (result == MO_STORE_REVIEW_OK) {
        const char *setting = mo_store_setting_name(intent);
        struct mo_store_decision decision = {.kind = "setting", .intent = intent, .time = time};
        memcpy(decision.base, review.program.source, 32);
        sqlite3_stmt *stmt = NULL;
        bool ok =
            sqlite3_prepare_v2(s->db, "UPDATE review SET setting=?1 WHERE id=1", -1, &stmt, NULL) ==
                SQLITE_OK &&
            step_done(stmt, sqlite3_bind_text(stmt, 1, setting, -1, SQLITE_STATIC) == SQLITE_OK) &&
            store_decide(s, &decision);
        result = review_end(s, ok ? MO_STORE_REVIEW_OK : MO_STORE_REVIEW_FAILED);
    }
    mo_arena_free(&arena);
    return result;
}

struct pending_head {
    uint8_t review[32], base[32], candidate[32];
};

static enum mo_store_status pending_head(struct mo_store *s, struct pending_head *out,
                                         bool *found) {
    sqlite3_stmt *stmt = NULL;
    int code = sqlite3_prepare_v2(s->db, "SELECT review,base,candidate FROM pending WHERE id=1", -1,
                                  &stmt, NULL);
    if (code != SQLITE_OK)
        return store_sqlite_status(code);
    int step = sqlite3_step(stmt);
    enum mo_store_status result = MO_STORE_OK;
    *found = step == SQLITE_ROW;
    if (step != SQLITE_ROW && step != SQLITE_DONE)
        result = store_sqlite_status(step);
    else if (*found && (!store_blob_size(stmt, 0, 32) || !store_blob_size(stmt, 1, 32) ||
                        !store_blob_size(stmt, 2, 32)))
        result = MO_STORE_CORRUPT;
    else if (*found) {
        memcpy(out->review, sqlite3_column_blob(stmt, 0), 32);
        memcpy(out->base, sqlite3_column_blob(stmt, 1), 32);
        memcpy(out->candidate, sqlite3_column_blob(stmt, 2), 32);
    }
    return store_finish(stmt, result);
}

static bool decide_pending(struct mo_store *s, const char *kind, const struct pending_head *head,
                           int64_t time) {
    struct mo_store_review setting = {0};
    struct mo_store_decision d = {
        .kind = kind, .has_candidate = true, .has_review = true, .time = time};
    memcpy(d.base, head->base, 32);
    memcpy(d.candidate, head->candidate, 32);
    memcpy(d.review, head->review, 32);
    if (store_read_setting(s, &setting) != MO_STORE_OK)
        return false;
    d.intent = setting.intent;
    return store_exec(s, "DELETE FROM pending") && store_decide(s, &d);
}

bool store_drop_stale(struct mo_store *s, int64_t time) {
    struct pending_head head;
    bool found = false;
    return pending_head(s, &head, &found) == MO_STORE_OK &&
           (!found || decide_pending(s, "stale", &head, time));
}

bool store_door_decided(struct mo_store *s, const struct mo_store_activation *a) {
    const struct mo_store_decision *d = a->decision;
    if (d == NULL)
        return true;
    struct mo_store_review review = {0};
    struct pending_head head;
    bool found = false;
    if (!review_ready(s))
        return false;
    int64_t records = review_count(s, "SELECT count(*) FROM decisions");
    if (records < 0 || store_read_setting(s, &review) != MO_STORE_OK ||
        pending_head(s, &head, &found) != MO_STORE_OK || review.version != a->approval_version)
        return false;
    bool yes = strcmp(d->kind, "yes") == 0;
    bool own = yes && found && memcmp(head.review, d->review, 32) == 0;
    if ((yes && !own) || (uint64_t)records + (found && !own ? 2u : 1u) > MO_LIMIT_REVIEW_RECORDS)
        return false;
    bool ok = !own || store_exec(s, "DELETE FROM pending");
    int added = 0;
    for (uint32_t i = 0; ok && i < a->approve_count; i++) {
        sqlite3_stmt *stmt = NULL;
        const void *blobs[] = {a->approve[i], d->review};
        const int sizes[] = {32, 32};
        ok = sqlite3_prepare_v2(s->db,
                                "INSERT INTO approvals VALUES(?1,?3,?2) ON CONFLICT(digest) DO "
                                "NOTHING",
                                -1, &stmt, NULL) == SQLITE_OK &&
             step_done(stmt, bind_blobs(stmt, 1, blobs, sizes, 2) &&
                                 sqlite3_bind_int64(stmt, 3, (sqlite3_int64)review.version + 1) ==
                                     SQLITE_OK);
        added += ok ? sqlite3_changes(s->db) : 0;
    }
    for (uint32_t i = 0; ok && i < a->retire_count; i++) {
        sqlite3_stmt *stmt = NULL;
        const void *blobs[] = {a->retire[i]};
        const int sizes[] = {32};
        ok = sqlite3_prepare_v2(s->db, "DELETE FROM approvals WHERE digest=?1", -1, &stmt, NULL) ==
                 SQLITE_OK &&
             step_done(stmt, bind_blobs(stmt, 1, blobs, sizes, 1));
        added += ok ? sqlite3_changes(s->db) : 0;
    }
    return ok && (added == 0 || store_exec(s, "UPDATE review SET version=version+1 WHERE id=1")) &&
           store_decide(s, d);
}

enum mo_store_review_status mo_store_hold(struct mo_store *s, const struct mo_store_pending *p,
                                          const struct mo_store_decision *d) {
    MO_ASSERT(s != NULL && p != NULL && d != NULL);
    struct mo_arena arena;
    mo_arena_init(&arena, MO_STORE_REVIEW_READ_BYTES);
    struct mo_store_review review;
    enum mo_store_review_status result = review_begin(s, &arena, &review, true);
    if (result == MO_STORE_REVIEW_OK &&
        (memcmp(review.program.source, p->base, 32) != 0 || review.version != p->version))
        result = review_end(s, MO_STORE_REVIEW_STALE);
    else if (result == MO_STORE_REVIEW_OK) {
        struct pending_head head;
        bool found = false;
        sqlite3_stmt *stmt = NULL;
        const void *blobs[] = {p->review, p->base, p->candidate};
        const int sizes[] = {32, 32, 32};
        bool ok =
            pending_head(s, &head, &found) == MO_STORE_OK &&
            (!found || decide_pending(s, "replaced", &head, d->time)) &&
            sqlite3_prepare_v2(s->db, "INSERT INTO pending VALUES(1,?1,?2,?3,?4,?5,?6,?7)", -1,
                               &stmt, NULL) == SQLITE_OK &&
            step_done(
                stmt,
                bind_blobs(stmt, 1, blobs, sizes, 3) &&
                    sqlite3_bind_text(stmt, 4, p->migration, -1, SQLITE_STATIC) == SQLITE_OK &&
                    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)p->version) == SQLITE_OK &&
                    sqlite3_bind_text(stmt, 6,
                                      p->difference_size > 0 ? (const char *)p->difference : "",
                                      (int)p->difference_size, SQLITE_STATIC) == SQLITE_OK &&
                    sqlite3_bind_blob(stmt, 7, p->bundle, (int)p->bundle_size, SQLITE_STATIC) ==
                        SQLITE_OK) &&
            store_decide(s, d);
        result = review_end(s, ok ? MO_STORE_REVIEW_OK : MO_STORE_REVIEW_FAILED);
    }
    mo_arena_free(&arena);
    return result;
}

enum mo_store_review_status mo_store_decide(struct mo_store *s, const struct mo_store_decision *d,
                                            const uint8_t *drop) {
    MO_ASSERT(s != NULL && d != NULL);
    struct mo_arena arena;
    mo_arena_init(&arena, MO_STORE_REVIEW_READ_BYTES);
    struct mo_store_review review;
    enum mo_store_review_status result = review_begin(s, &arena, &review, false);
    if (result == MO_STORE_REVIEW_OK) {
        struct pending_head head;
        bool found = false;
        bool ok = pending_head(s, &head, &found) == MO_STORE_OK &&
                  (drop == NULL || !found || memcmp(head.review, drop, 32) != 0 ||
                   store_exec(s, "DELETE FROM pending")) &&
                  store_decide(s, d);
        result = review_end(s, ok ? MO_STORE_REVIEW_OK : MO_STORE_REVIEW_FAILED);
    }
    mo_arena_free(&arena);
    return result;
}

static const void *copy_column(sqlite3_stmt *stmt, int column, struct mo_arena *arena,
                               size_t *size) {
    *size = (size_t)sqlite3_column_bytes(stmt, column);
    const void *bytes = sqlite3_column_blob(stmt, column);
    uint8_t *copy = mo_arena_allocate(arena, *size + 1, 1);
    if (copy == NULL || (bytes == NULL && *size != 0))
        return NULL;
    if (*size != 0)
        memcpy(copy, bytes, *size);
    copy[*size] = 0;
    return copy;
}

enum mo_store_status mo_store_pending_read(struct mo_store *s, struct mo_arena *arena,
                                           struct mo_store_pending *out, bool *found) {
    MO_ASSERT(s != NULL && arena != NULL && out != NULL && found != NULL);
    sqlite3_stmt *stmt = NULL;
    int code = sqlite3_prepare_v2(s->db,
                                  "SELECT review,base,candidate,migration,version,difference,"
                                  "bundle FROM pending WHERE id=1",
                                  -1, &stmt, NULL);
    if (code != SQLITE_OK)
        return store_sqlite_status(code);
    int step = sqlite3_step(stmt);
    enum mo_store_status result = MO_STORE_OK;
    *found = step == SQLITE_ROW;
    struct mo_store_pending next = {0};
    size_t migration = 0;
    if (step != SQLITE_ROW && step != SQLITE_DONE)
        result = store_sqlite_status(step);
    else if (*found &&
             (!store_blob_size(stmt, 0, 32) || !store_blob_size(stmt, 1, 32) ||
              !store_blob_size(stmt, 2, 32) || sqlite3_column_type(stmt, 3) != SQLITE_TEXT ||
              sqlite3_column_bytes(stmt, 3) > MO_LIMIT_UPGRADE_MIGRATION_NAME ||
              sqlite3_column_type(stmt, 4) != SQLITE_INTEGER || sqlite3_column_int64(stmt, 4) < 0 ||
              sqlite3_column_type(stmt, 5) != SQLITE_TEXT ||
              sqlite3_column_bytes(stmt, 5) > 2 * MO_LIMIT_INTENT_LIST_BYTES ||
              sqlite3_column_type(stmt, 6) != SQLITE_BLOB || sqlite3_column_bytes(stmt, 6) == 0 ||
              sqlite3_column_bytes(stmt, 6) > MO_LIMIT_BUNDLE_BYTES))
        result = MO_STORE_CORRUPT;
    else if (*found) {
        memcpy(next.review, sqlite3_column_blob(stmt, 0), 32);
        memcpy(next.base, sqlite3_column_blob(stmt, 1), 32);
        memcpy(next.candidate, sqlite3_column_blob(stmt, 2), 32);
        next.version = (uint64_t)sqlite3_column_int64(stmt, 4);
        next.migration = copy_column(stmt, 3, arena, &migration);
        next.difference = copy_column(stmt, 5, arena, &next.difference_size);
        next.bundle = copy_column(stmt, 6, arena, &next.bundle_size);
        if (next.migration == NULL || next.difference == NULL || next.bundle == NULL)
            result = MO_STORE_MEMORY;
        else if (memchr(next.migration, 0, migration) != NULL ||
                 !mo_utf8_valid((const uint8_t *)next.migration, migration) ||
                 !mo_utf8_valid(next.difference, next.difference_size))
            result = MO_STORE_CORRUPT;
    }
    if (result == MO_STORE_OK && *found)
        *out = next;
    return store_finish(stmt, result);
}

enum mo_store_status mo_store_decisions(struct mo_store *s,
                                        bool (*consume)(const struct mo_store_decision *, void *),
                                        void *context) {
    MO_ASSERT(s != NULL && consume != NULL);
    sqlite3_stmt *stmt = NULL;
    int code = sqlite3_prepare_v2(s->db,
                                  "SELECT time,kind,base,candidate,review,setting,unattended,"
                                  "detail FROM decisions ORDER BY seq DESC",
                                  -1, &stmt, NULL);
    if (code != SQLITE_OK)
        return store_sqlite_status(code);
    enum mo_store_status result = MO_STORE_OK;
    int step = SQLITE_DONE;
    bool more = true;
    while (more && result == MO_STORE_OK && (step = sqlite3_step(stmt)) == SQLITE_ROW) {
        const unsigned char *kind = sqlite3_column_text(stmt, 1);
        const unsigned char *setting = sqlite3_column_text(stmt, 5);
        const unsigned char *detail = sqlite3_column_text(stmt, 7);
        int candidate = sqlite3_column_bytes(stmt, 3), review = sqlite3_column_bytes(stmt, 4);
        if (sqlite3_column_type(stmt, 0) != SQLITE_INTEGER || kind == NULL || detail == NULL ||
            setting == NULL || !store_blob_size(stmt, 2, 32) ||
            sqlite3_column_type(stmt, 3) != SQLITE_BLOB || (candidate != 0 && candidate != 32) ||
            sqlite3_column_type(stmt, 4) != SQLITE_BLOB || (review != 0 && review != 32) ||
            !mo_utf8_valid(kind, (size_t)sqlite3_column_bytes(stmt, 1)) ||
            !mo_utf8_valid(detail, (size_t)sqlite3_column_bytes(stmt, 7))) {
            result = MO_STORE_CORRUPT;
            break;
        }
        struct mo_store_decision d = {.kind = (const char *)kind,
                                      .detail = (const char *)detail,
                                      .has_candidate = candidate == 32,
                                      .has_review = review == 32,
                                      .intent = setting[0] == 'i',
                                      .unattended = sqlite3_column_int64(stmt, 6) != 0,
                                      .time = sqlite3_column_int64(stmt, 0)};
        memcpy(d.base, sqlite3_column_blob(stmt, 2), 32);
        if (d.has_candidate)
            memcpy(d.candidate, sqlite3_column_blob(stmt, 3), 32);
        if (d.has_review)
            memcpy(d.review, sqlite3_column_blob(stmt, 4), 32);
        more = consume(&d, context);
    }
    if (result == MO_STORE_OK && more && step != SQLITE_DONE)
        result = store_sqlite_status(step);
    return store_finish(stmt, result);
}
