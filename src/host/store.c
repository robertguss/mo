#include "host/host.h"

#include <pthread.h>
#include <sqlite3.h>
#include <string.h>
#include <sys/stat.h>

#include "base/binary.h"
#include "base/store-memory.h"

#define TEXT_OF(n) DIGITS_OF(n)
#define DIGITS_OF(n) #n
#define CACHE_PRAGMA "PRAGMA cache_size=-" TEXT_OF(MO_LIMIT_STORE_CACHE_KIB) ";"

struct mo_store {
    struct mo_arena arena;
    sqlite3 *db;
};

static pthread_once_t once = PTHREAD_ONCE_INIT;
static int initialized = SQLITE_ERROR;
static pthread_mutex_t stores_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned stores;

static bool claim_store(void) {
    MO_ASSERT(pthread_mutex_lock(&stores_mutex) == 0);
    bool ok = stores < MO_LIMIT_STORES;
    if (ok)
        stores++;
    MO_ASSERT(pthread_mutex_unlock(&stores_mutex) == 0);
    return ok;
}

static void release_store(void) {
    MO_ASSERT(pthread_mutex_lock(&stores_mutex) == 0);
    MO_ASSERT(stores > 0);
    stores--;
    MO_ASSERT(pthread_mutex_unlock(&stores_mutex) == 0);
}

static int allocator_init(void *context) {
    (void)context;
    return SQLITE_OK;
}

static void initialize(void) {
    sqlite3_mem_methods memory = {mo_store_malloc,
                                  mo_store_free,
                                  mo_store_realloc,
                                  mo_store_size,
                                  mo_store_roundup,
                                  allocator_init,
                                  NULL,
                                  NULL};
    initialized = sqlite3_config(SQLITE_CONFIG_MALLOC, &memory);
    if (initialized == SQLITE_OK)
        initialized = sqlite3_initialize();
}

static bool exec(struct mo_store *s, const char *sql) {
    return sqlite3_exec(s->db, sql, NULL, NULL, NULL) == SQLITE_OK;
}

static bool table(struct mo_store *s, const char *name, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, "SELECT sql FROM sqlite_schema WHERE name=?1", -1, &stmt, NULL) !=
        SQLITE_OK)
        return false;
    bool ok = sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC) == SQLITE_OK;
    int step = ok ? sqlite3_step(stmt) : SQLITE_ERROR;
    if (step == SQLITE_ROW) {
        const unsigned char *stored = sqlite3_column_text(stmt, 0);
        ok = stored != NULL && strcmp((const char *)stored, sql) == 0 &&
             sqlite3_step(stmt) == SQLITE_DONE;
    } else
        ok = step == SQLITE_DONE;
    ok = sqlite3_finalize(stmt) == SQLITE_OK && ok;
    return ok && (step != SQLITE_DONE || exec(s, sql));
}

enum mo_store_status mo_store_open(const char *path, struct mo_store **out) {
    struct stat info;
    if (path != NULL && (path[0] == '\0' || strcmp(path, ":memory:") == 0 ||
                         (stat(path, &info) == 0 && info.st_size == 0)))
        return MO_STORE_CORRUPT;
    if (pthread_once(&once, initialize) != 0 || initialized != SQLITE_OK)
        return MO_STORE_IO;
    if (!claim_store())
        return MO_STORE_LIMIT;
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_SERVICE_METADATA);
    struct mo_store *s = mo_arena_allocate(&arena, sizeof *s, _Alignof(struct mo_store));
    if (s == NULL) {
        release_store();
        return MO_STORE_LIMIT;
    }
    *s = (struct mo_store){.arena = arena};
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(path == NULL ? ":memory:" : path, &s->db, flags, NULL) != SQLITE_OK) {
        mo_store_close(s);
        return MO_STORE_IO;
    }
    (void)sqlite3_limit(s->db, SQLITE_LIMIT_LENGTH, MO_LIMIT_STORE_ROW);
    (void)sqlite3_limit(s->db, SQLITE_LIMIT_SQL_LENGTH, MO_LIMIT_STORE_SQL);
    if (sqlite3_db_config(s->db, SQLITE_DBCONFIG_DEFENSIVE, 1, NULL) != SQLITE_OK ||
        sqlite3_db_config(s->db, SQLITE_DBCONFIG_ENABLE_TRIGGER, 0, NULL) != SQLITE_OK ||
        sqlite3_db_config(s->db, SQLITE_DBCONFIG_ENABLE_VIEW, 0, NULL) != SQLITE_OK ||
        !exec(s, "PRAGMA trusted_schema=OFF; PRAGMA journal_mode=DELETE;"
                 "PRAGMA synchronous=EXTRA; PRAGMA temp_store=MEMORY;" CACHE_PRAGMA
                 "PRAGMA locking_mode=EXCLUSIVE;BEGIN EXCLUSIVE") ||
        !table(s, "head",
               "CREATE TABLE head(id INTEGER PRIMARY KEY CHECK(id=1), seq INTEGER NOT NULL)"
               " STRICT") ||
        !table(s, "snapshot",
               "CREATE TABLE snapshot(id INTEGER PRIMARY KEY CHECK(id=1), record BLOB NOT NULL)"
               " STRICT") ||
        !table(s, "log",
               "CREATE TABLE log(seq INTEGER PRIMARY KEY, record BLOB NOT NULL) STRICT") ||
        !exec(s, "COMMIT")) {
        (void)exec(s, "ROLLBACK");
        mo_store_close(s);
        return MO_STORE_IO;
    }
    *out = s;
    return MO_STORE_OK;
}

void mo_store_close(struct mo_store *s) {
    if (s != NULL) {
        struct mo_arena arena = s->arena;
        if (s->db != NULL)
            MO_ASSERT(sqlite3_close(s->db) == SQLITE_OK);
        mo_arena_free(&arena);
        release_store();
    }
}

bool mo_store_record_read(enum mo_store_record_kind kind, const uint8_t *bytes, size_t length,
                          struct mo_store_record *out) {
    if (length < MO_STORE_FRAME_BYTES || length > MO_LIMIT_SAVED_BYTES + MO_STORE_FRAME_BYTES ||
        memcmp(bytes, "MOST\1", 5) != 0 || bytes[5] != (uint8_t)kind)
        return false;
    struct mo_binary r = {bytes, length, 6};
    uint64_t seq, image, payload, sum;
    const uint8_t *data;
    if (!mo_binary_uint(&r, 8, &seq) || seq > INT64_MAX || !mo_binary_uint(&r, 8, &image) ||
        !mo_binary_uint(&r, 4, &payload) || !mo_binary_read(&r, (size_t)payload, &data) ||
        !mo_binary_uint(&r, 8, &sum) || r.position != length ||
        sum != mo_binary_checksum(bytes, length - 8))
        return false;
    *out = (struct mo_store_record){kind, seq, image, data, (size_t)payload};
    return true;
}

static bool frame(const struct mo_store_record *r, struct mo_buffer *out) {
    if (r->length > MO_LIMIT_SAVED_BYTES || r->sequence > INT64_MAX)
        return false;
    return mo_buffer_append(out, "MOST\1", 5) && mo_binary_write(out, r->kind, 1) &&
           mo_binary_write(out, r->sequence, 8) && mo_binary_write(out, r->image, 8) &&
           mo_binary_write(out, r->length, 4) && mo_buffer_append(out, r->bytes, r->length) &&
           mo_binary_write(out, mo_binary_checksum(out->bytes, out->length), 8);
}

/* SQLite's corruption codes, and a stored value longer than any the runtime
 * writes, mean bad saved bytes. Every other failure is I/O or resources, which
 * a later retry may survive. */
static enum mo_store_status failure(int code) {
    code &= 0xff;
    return code == SQLITE_CORRUPT || code == SQLITE_NOTADB || code == SQLITE_TOOBIG
               ? MO_STORE_CORRUPT
               : MO_STORE_IO;
}

static enum mo_store_status finish(sqlite3_stmt *stmt, enum mo_store_status result) {
    int code = sqlite3_finalize(stmt);
    return result == MO_STORE_OK && code != SQLITE_OK ? failure(code) : result;
}

static enum mo_store_status row(sqlite3_stmt *stmt, int column, enum mo_store_record_kind kind,
                                struct mo_store_record *out) {
    if (sqlite3_column_type(stmt, column) != SQLITE_BLOB)
        return MO_STORE_CORRUPT;
    const uint8_t *bytes = sqlite3_column_blob(stmt, column);
    int length = sqlite3_column_bytes(stmt, column);
    return bytes != NULL && mo_store_record_read(kind, bytes, (size_t)length, out)
               ? MO_STORE_OK
               : MO_STORE_CORRUPT;
}

static enum mo_store_status current_head(struct mo_store *s, uint64_t *head, bool *empty) {
    sqlite3_stmt *stmt = NULL;
    int code = sqlite3_prepare_v2(s->db, "SELECT seq FROM head WHERE id=1", -1, &stmt, NULL);
    if (code != SQLITE_OK)
        return failure(code);
    int step = sqlite3_step(stmt);
    enum mo_store_status result = MO_STORE_OK;
    *empty = step == SQLITE_DONE;
    if (step == SQLITE_ROW) {
        bool integer = sqlite3_column_type(stmt, 0) == SQLITE_INTEGER;
        sqlite3_int64 n = sqlite3_column_int64(stmt, 0);
        if (!integer || n < 0)
            result = MO_STORE_CORRUPT;
        else
            *head = (uint64_t)n;
    } else if (step != SQLITE_DONE)
        result = failure(step);
    return finish(stmt, result);
}

static enum mo_store_status
load(struct mo_store *s, bool (*consume)(const struct mo_store_record *, void *), void *context) {
    sqlite3_stmt *stmt = NULL;
    int code = sqlite3_prepare_v2(s->db, "PRAGMA user_version", -1, &stmt, NULL);
    if (code != SQLITE_OK)
        return failure(code);
    int step = sqlite3_step(stmt);
    int version = step == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : -1;
    enum mo_store_status result = finish(stmt, step == SQLITE_ROW ? MO_STORE_OK : failure(step));
    if (result != MO_STORE_OK)
        return result;
    if (version < 0 || version > 1)
        return MO_STORE_CORRUPT;
    uint64_t head = 0, sequence = 0, image = 0;
    bool empty = false;
    result = current_head(s, &head, &empty);
    if (result != MO_STORE_OK)
        return result;
    code = sqlite3_prepare_v2(s->db, "SELECT record FROM snapshot WHERE id=1", -1, &stmt, NULL);
    if (code != SQLITE_OK)
        return failure(code);
    step = sqlite3_step(stmt);
    struct mo_store_record record;
    if (step != SQLITE_ROW && step != SQLITE_DONE)
        result = failure(step);
    else if (empty || step == SQLITE_DONE)
        result = empty && step == SQLITE_DONE && version == 0 ? MO_STORE_OK : MO_STORE_CORRUPT;
    else if (version != 1)
        result = MO_STORE_CORRUPT;
    else if ((result = row(stmt, 0, MO_STORE_STATE, &record)) == MO_STORE_OK) {
        sequence = record.sequence;
        image = record.image;
        if (sequence > head || head - sequence > MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES ||
            !consume(&record, context))
            result = MO_STORE_CORRUPT;
    }
    result = finish(stmt, result);
    if (result != MO_STORE_OK)
        return result;
    code = sqlite3_prepare_v2(s->db, "SELECT seq,record FROM log ORDER BY seq", -1, &stmt, NULL);
    if (code != SQLITE_OK)
        return failure(code);
    while (result == MO_STORE_OK && (step = sqlite3_step(stmt)) == SQLITE_ROW) {
        bool integer = sqlite3_column_type(stmt, 0) == SQLITE_INTEGER;
        sqlite3_int64 n = sqlite3_column_int64(stmt, 0);
        if (empty || sequence >= head || !integer || n <= 0 || (uint64_t)n != sequence + 1)
            result = MO_STORE_CORRUPT;
        else if ((result = row(stmt, 1, MO_STORE_LOG, &record)) == MO_STORE_OK &&
                 (record.sequence != (uint64_t)n || record.image != image ||
                  !consume(&record, context)))
            result = MO_STORE_CORRUPT;
        else
            sequence++;
    }
    if (result == MO_STORE_OK && step != SQLITE_DONE)
        result = failure(step);
    result = finish(stmt, result);
    return result == MO_STORE_OK && sequence != head ? MO_STORE_CORRUPT : result;
}

static bool write_blob(struct mo_store *s, const char *sql, uint64_t seq,
                       const struct mo_buffer *bytes) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;
    bool ok =
        sqlite3_bind_int64(stmt, 1, (sqlite3_int64)seq) == SQLITE_OK &&
        sqlite3_bind_blob(stmt, 2, bytes->bytes, (int)bytes->length, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_DONE;
    return sqlite3_finalize(stmt) == SQLITE_OK && ok;
}

enum mo_store_status mo_store_call(struct mo_store *s, enum mo_store_operation op,
                                   const struct mo_store_record *record,
                                   bool (*consume)(const struct mo_store_record *, void *),
                                   void *context) {
    MO_ASSERT(s != NULL);
    bool reading = op == MO_STORE_LOAD;
    MO_ASSERT(reading ? consume != NULL : record != NULL);
    if (!exec(s, reading ? "BEGIN" : "BEGIN IMMEDIATE"))
        return MO_STORE_IO;
    enum mo_store_status result = MO_STORE_OK;
    if (reading)
        result = load(s, consume, context);
    else {
        struct mo_buffer bytes;
        mo_buffer_init(&bytes, MO_LIMIT_SAVED_BYTES + MO_STORE_FRAME_BYTES);
        uint64_t head = 0;
        bool empty = false;
        bool ok = current_head(s, &head, &empty) == MO_STORE_OK && frame(record, &bytes);
        if (op == MO_STORE_APPEND) {
            ok = ok && !empty && head < INT64_MAX && record->sequence == head + 1 &&
                 record->kind == MO_STORE_LOG &&
                 write_blob(s, "INSERT INTO log VALUES(?1,?2)", record->sequence, &bytes) &&
                 exec(s, "UPDATE head SET seq=seq+1 WHERE id=1");
        } else {
            MO_ASSERT(op == MO_STORE_SNAPSHOT);
            ok = ok && record->sequence == head && record->kind == MO_STORE_STATE &&
                 write_blob(s,
                            "INSERT INTO snapshot VALUES(1,?2) ON CONFLICT(id) DO UPDATE"
                            " SET record=excluded.record",
                            record->sequence, &bytes) &&
                 exec(s, "INSERT INTO head VALUES(1,0) ON CONFLICT(id) DO NOTHING;"
                         "DELETE FROM log; PRAGMA user_version=1");
        }
        mo_buffer_free(&bytes);
        if (!ok)
            result = MO_STORE_IO;
    }
    if (result == MO_STORE_OK && !exec(s, "COMMIT"))
        result = MO_STORE_IO;
    if (result != MO_STORE_OK)
        (void)exec(s, "ROLLBACK");
    return result;
}
