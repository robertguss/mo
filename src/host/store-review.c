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

bool store_decide(struct mo_store *s, int64_t time, const char *kind, const uint8_t base[32],
                  const char *setting, const char *detail) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, "INSERT INTO decisions VALUES(NULL,?1,?2,?3,x'',x'',?4,0,?5)", -1,
                           &stmt, NULL) != SQLITE_OK)
        return false;
    const void *blobs[] = {base};
    const int sizes[] = {32};
    bool bound = sqlite3_bind_int64(stmt, 1, time) == SQLITE_OK &&
                 sqlite3_bind_text(stmt, 2, kind, -1, SQLITE_STATIC) == SQLITE_OK &&
                 bind_blobs(stmt, 3, blobs, sizes, 1) &&
                 sqlite3_bind_text(stmt, 4, setting, -1, SQLITE_STATIC) == SQLITE_OK &&
                 sqlite3_bind_text(stmt, 5, detail, -1, SQLITE_STATIC) == SQLITE_OK;
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
        MO_ASSERT(mo_intent_list_next(&cursor, &entry));
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
    if (result == MO_STORE_OK)
        *out = next;
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

bool store_review_write(struct mo_store *s, const struct mo_store_program *p) {
    return review_tables_ready(s) && write_items(s, p) &&
           store_exec(s,
                      "INSERT INTO review VALUES(1,'everything',0) ON CONFLICT(id) DO NOTHING") &&
           retire_approvals(s, p);
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
                                                struct mo_store_review *review) {
    if (s->unsafe || !store_exec(s, "BEGIN IMMEDIATE"))
        return MO_STORE_REVIEW_FAILED;
    if (mo_store_review_read(s, arena, review) != MO_STORE_OK || !review->reviewed)
        return review_end(s, MO_STORE_REVIEW_FAILED);
    int64_t count = review_count(s, "SELECT count(*) FROM decisions");
    if (count < 0)
        return review_end(s, MO_STORE_REVIEW_FAILED);
    return count >= MO_LIMIT_REVIEW_RECORDS ? review_end(s, MO_STORE_REVIEW_FULL)
                                            : MO_STORE_REVIEW_OK;
}

enum mo_store_review_status mo_store_approve(struct mo_store *s, const uint8_t program[32],
                                             const uint8_t digest[32], int64_t time) {
    MO_ASSERT(s != NULL);
    struct mo_arena arena;
    mo_arena_init(&arena, MO_STORE_REVIEW_READ_BYTES);
    struct mo_store_review review;
    enum mo_store_review_status result = review_begin(s, &arena, &review);
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
                store_decide(s, time, "approval", program, mo_store_setting_name(review.intent),
                             text);
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
    enum mo_store_review_status result = review_begin(s, &arena, &review);
    if (result == MO_STORE_REVIEW_OK && review.intent == intent)
        result = review_end(s, MO_STORE_REVIEW_OK);
    else if (result == MO_STORE_REVIEW_OK) {
        const char *setting = mo_store_setting_name(intent);
        sqlite3_stmt *stmt = NULL;
        bool ok =
            sqlite3_prepare_v2(s->db, "UPDATE review SET setting=?1 WHERE id=1", -1, &stmt, NULL) ==
                SQLITE_OK &&
            step_done(stmt, sqlite3_bind_text(stmt, 1, setting, -1, SQLITE_STATIC) == SQLITE_OK) &&
            store_decide(s, time, "setting", review.program.source, setting, "");
        result = review_end(s, ok ? MO_STORE_REVIEW_OK : MO_STORE_REVIEW_FAILED);
    }
    mo_arena_free(&arena);
    return result;
}
