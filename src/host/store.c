#include "host/host.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sqlite3.h>
#include <string.h>
#include <sys/stat.h>

#include "base/binary.h"
#include "base/failure.h"
#include "base/log-record.h"
#include "base/store-memory.h"
#include "base/utf8.h"

#define TEXT_OF(n) DIGITS_OF(n)
#define DIGITS_OF(n) #n
#define CACHE_PRAGMA "PRAGMA cache_size=-" TEXT_OF(MO_LIMIT_STORE_CACHE_KIB) ";"

struct mo_store {
    struct mo_arena arena;
    sqlite3 *db;
    bool unsafe;
    uint64_t evicted_kinds;
};

static pthread_once_t once = PTHREAD_ONCE_INIT;
static int initialized = SQLITE_ERROR;
static pthread_mutex_t stores_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned stores;

const char *mo_store_status_text(enum mo_store_status status) {
    switch (status) {
    case MO_STORE_OK:
        return NULL;
    case MO_STORE_IO:
        return "service store failed; reopen it to see whether the last change was saved";
    case MO_STORE_CORRUPT:
        return "saved service data is damaged or was written by a different program; recovery "
               "refused";
    case MO_STORE_MEMORY:
        return "service store memory allocation failed";
    case MO_STORE_EMPTY_PATH:
        return "service store path is empty; use a file path";
    case MO_STORE_MEMORY_PATH:
        return ":memory: is not a service store path; use a file path";
    case MO_STORE_HANDLES:
        return "too many open service stores; close one of the " TEXT_OF(
            MO_LIMIT_STORES) " open stores";
    case MO_STORE_BUSY:
        return "another process holds the service store, such as a second mo serve or a reader "
               "of its data folder";
    }
    MO_UNREACHABLE();
}

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

struct store_table {
    const char *name, *sql;
};
static const struct store_table tables[] = {
    {"head", "CREATE TABLE head(id INTEGER PRIMARY KEY CHECK(id=1), seq INTEGER NOT NULL)"
             " STRICT"},
    {"snapshot", "CREATE TABLE snapshot(id INTEGER PRIMARY KEY CHECK(id=1), record BLOB NOT NULL)"
                 " STRICT"},
    {"log", "CREATE TABLE log(seq INTEGER PRIMARY KEY, record BLOB NOT NULL) STRICT"},
    {"failure", "CREATE TABLE failure(id INTEGER PRIMARY KEY, kind TEXT NOT NULL, first INTEGER "
                "NOT NULL, message BLOB NOT NULL) STRICT"},
    {"failure_state",
     "CREATE TABLE failure_state(id INTEGER PRIMARY KEY, record BLOB NOT NULL) STRICT"},
};
/* The tables from here on came with failure capture; an older store lacks them. */
enum { FAILURE_TABLES = 3, TABLE_COUNT = sizeof tables / sizeof tables[0] };

enum table_state { TABLE_FAILED, TABLE_ABSENT, TABLE_SAME, TABLE_DIFFERENT };

/* Whether the table exists as the runtime creates it. FAILED leaves SQLite's code in code. */
static enum table_state table_state(struct mo_store *s, const struct store_table *t, int *code) {
    sqlite3_stmt *stmt = NULL;
    *code =
        sqlite3_prepare_v2(s->db, "SELECT sql FROM sqlite_schema WHERE name=?1", -1, &stmt, NULL);
    if (*code != SQLITE_OK)
        return TABLE_FAILED;
    *code = sqlite3_bind_text(stmt, 1, t->name, -1, SQLITE_STATIC);
    int step = *code == SQLITE_OK ? sqlite3_step(stmt) : *code;
    enum table_state state = TABLE_FAILED;
    if (step == SQLITE_ROW) {
        const unsigned char *stored = sqlite3_column_text(stmt, 0);
        bool same = stored != NULL && strcmp((const char *)stored, t->sql) == 0;
        step = same ? sqlite3_step(stmt) : SQLITE_DONE;
        state = step != SQLITE_DONE ? TABLE_FAILED : same ? TABLE_SAME : TABLE_DIFFERENT;
    } else if (step == SQLITE_DONE)
        state = TABLE_ABSENT;
    int finalized = sqlite3_finalize(stmt);
    if (state == TABLE_FAILED)
        *code = step;
    else if (finalized != SQLITE_OK) {
        state = TABLE_FAILED;
        *code = finalized;
    }
    return state;
}

static bool table(struct mo_store *s, const struct store_table *t) {
    int code;
    enum table_state state = table_state(s, t, &code);
    return state == TABLE_SAME || (state == TABLE_ABSENT && exec(s, t->sql));
}

_Static_assert(MO_LIMIT_SERVE_STORE_WAIT_US / 1000 <= INT_MAX, "the wait fits SQLite's timeout");

static enum mo_store_status open_store(const char *path, bool wait, struct mo_store **out) {
    struct stat info;
    if (path != NULL && path[0] == '\0')
        return MO_STORE_EMPTY_PATH;
    if (path != NULL && strcmp(path, ":memory:") == 0)
        return MO_STORE_MEMORY_PATH;
    if (path != NULL && stat(path, &info) == 0 && info.st_size == 0)
        return MO_STORE_CORRUPT;
    if (pthread_once(&once, initialize) != 0 || initialized != SQLITE_OK)
        return MO_STORE_IO;
    if (!claim_store())
        return MO_STORE_HANDLES;
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_SERVICE_METADATA);
    struct mo_store *s = mo_arena_allocate(&arena, sizeof *s, _Alignof(struct mo_store));
    if (s == NULL) {
        release_store();
        return MO_STORE_MEMORY;
    }
    *s = (struct mo_store){.arena = arena};
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(path == NULL ? ":memory:" : path, &s->db, flags, NULL) != SQLITE_OK) {
        mo_store_close(s);
        return MO_STORE_IO;
    }
    (void)sqlite3_limit(s->db, SQLITE_LIMIT_LENGTH, MO_LIMIT_STORE_ROW);
    (void)sqlite3_limit(s->db, SQLITE_LIMIT_SQL_LENGTH, MO_LIMIT_STORE_SQL);
    if (wait &&
        sqlite3_busy_timeout(s->db, (int)(MO_LIMIT_SERVE_STORE_WAIT_US / 1000)) != SQLITE_OK) {
        mo_store_close(s);
        return MO_STORE_IO;
    }
    if (sqlite3_db_config(s->db, SQLITE_DBCONFIG_DEFENSIVE, 1, NULL) != SQLITE_OK ||
        sqlite3_db_config(s->db, SQLITE_DBCONFIG_ENABLE_TRIGGER, 0, NULL) != SQLITE_OK ||
        sqlite3_db_config(s->db, SQLITE_DBCONFIG_ENABLE_VIEW, 0, NULL) != SQLITE_OK ||
        !exec(s, "PRAGMA trusted_schema=OFF; PRAGMA journal_mode=DELETE;"
                 "PRAGMA synchronous=EXTRA; PRAGMA temp_store=MEMORY;" CACHE_PRAGMA
                 "PRAGMA locking_mode=EXCLUSIVE;BEGIN EXCLUSIVE") ||
        !table(s, &tables[0]) || !table(s, &tables[1]) || !table(s, &tables[2]) ||
        !table(s, &tables[3]) || !table(s, &tables[4]) || !exec(s, "COMMIT")) {
        bool busy = (sqlite3_errcode(s->db) & 0xff) == SQLITE_BUSY;
        (void)exec(s, "ROLLBACK");
        mo_store_close(s);
        return busy ? MO_STORE_BUSY : MO_STORE_IO;
    }
    /* The open holds the store's lock until close, so nothing after it waits. */
    if (wait && sqlite3_busy_timeout(s->db, 0) != SQLITE_OK) {
        mo_store_close(s);
        return MO_STORE_IO;
    }
    *out = s;
    return MO_STORE_OK;
}

enum mo_store_status mo_store_open(const char *path, struct mo_store **out) {
    return open_store(path, false, out);
}

enum mo_store_status mo_store_open_wait(const char *path, struct mo_store **out) {
    return open_store(path, true, out);
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
        (kind == MO_STORE_FAILURE_MESSAGE &&
         length > MO_LIMIT_FAILURE_MESSAGE_BYTES + MO_STORE_FRAME_BYTES) ||
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

/* "MOST\1", kind, sequence, image and payload length; then payload and checksum. */
enum { FRAME_HEADER_BYTES = 5 + 1 + 8 + 8 + 4, FRAME_CHECKSUM_BYTES = 8 };
_Static_assert(FRAME_HEADER_BYTES + FRAME_CHECKSUM_BYTES == MO_STORE_FRAME_BYTES,
               "frame layout matches its envelope");

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

/* The head and the snapshot, checked against the version and each other. */
struct saved_head {
    uint64_t head, sequence, image;
    bool empty;
};

/* An empty store has no snapshot, so no callback. A refusing callback is CORRUPT. */
static enum mo_store_status load_snapshot(struct mo_store *s, struct saved_head *h,
                                          bool (*consume)(const struct mo_store_record *, void *),
                                          void *context) {
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
    *h = (struct saved_head){0};
    result = current_head(s, &h->head, &h->empty);
    if (result != MO_STORE_OK)
        return result;
    code = sqlite3_prepare_v2(s->db, "SELECT record FROM snapshot WHERE id=1", -1, &stmt, NULL);
    if (code != SQLITE_OK)
        return failure(code);
    step = sqlite3_step(stmt);
    struct mo_store_record record;
    if (step != SQLITE_ROW && step != SQLITE_DONE)
        result = failure(step);
    else if (h->empty || step == SQLITE_DONE)
        result = h->empty && step == SQLITE_DONE && version == 0 ? MO_STORE_OK : MO_STORE_CORRUPT;
    else if (version != 1)
        result = MO_STORE_CORRUPT;
    else if ((result = row(stmt, 0, MO_STORE_STATE, &record)) == MO_STORE_OK) {
        h->sequence = record.sequence;
        h->image = record.image;
        if (h->sequence > h->head || h->head - h->sequence > MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES ||
            !consume(&record, context))
            result = MO_STORE_CORRUPT;
    }
    return finish(stmt, result);
}

/* The log row in stmt's first column is the one after sequence, within the head. */
static bool next_log_row(const struct saved_head *h, sqlite3_stmt *stmt, uint64_t sequence) {
    bool integer = sqlite3_column_type(stmt, 0) == SQLITE_INTEGER;
    sqlite3_int64 n = sqlite3_column_int64(stmt, 0);
    return !h->empty && sequence < h->head && integer && n > 0 && (uint64_t)n == sequence + 1;
}

static enum mo_store_status
load(struct mo_store *s, bool (*consume)(const struct mo_store_record *, void *), void *context) {
    struct saved_head h;
    enum mo_store_status result = load_snapshot(s, &h, consume, context);
    if (result != MO_STORE_OK)
        return result;
    uint64_t sequence = h.sequence;
    sqlite3_stmt *stmt = NULL;
    int code =
        sqlite3_prepare_v2(s->db, "SELECT seq,record FROM log ORDER BY seq", -1, &stmt, NULL);
    if (code != SQLITE_OK)
        return failure(code);
    int step;
    struct mo_store_record record;
    while (result == MO_STORE_OK && (step = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (!next_log_row(&h, stmt, sequence))
            result = MO_STORE_CORRUPT;
        else if ((result = row(stmt, 1, MO_STORE_LOG, &record)) == MO_STORE_OK &&
                 (record.sequence != sequence + 1 || record.image != h.image ||
                  !consume(&record, context)))
            result = MO_STORE_CORRUPT;
        else
            sequence++;
    }
    if (result == MO_STORE_OK && step != SQLITE_DONE)
        result = failure(step);
    result = finish(stmt, result);
    return result == MO_STORE_OK && sequence != h.head ? MO_STORE_CORRUPT : result;
}

static int write_blob_code(struct mo_store *s, const char *sql, uint64_t seq,
                           const struct mo_buffer *bytes) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return sqlite3_errcode(s->db);
    int code = sqlite3_bind_int64(stmt, 1, (sqlite3_int64)seq);
    if (code == SQLITE_OK)
        code = sqlite3_bind_blob(stmt, 2, bytes->bytes, (int)bytes->length, SQLITE_STATIC);
    if (code == SQLITE_OK) {
        code = sqlite3_step(stmt);
        if (code == SQLITE_DONE)
            code = SQLITE_OK;
    }
    int finalized = sqlite3_finalize(stmt);
    return code == SQLITE_OK ? finalized : code;
}

static bool write_blob(struct mo_store *s, const char *sql, uint64_t seq,
                       const struct mo_buffer *bytes) {
    return write_blob_code(s, sql, seq, bytes) == SQLITE_OK;
}

enum mo_store_status mo_store_call(struct mo_store *s, enum mo_store_operation op,
                                   const struct mo_store_record *record,
                                   bool (*consume)(const struct mo_store_record *, void *),
                                   void *context) {
    MO_ASSERT(s != NULL);
    if (s->unsafe)
        return MO_STORE_IO;
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

struct failure_row {
    uint64_t id;
    size_t bytes;
    bool first, same;
    char kind[MO_LIMIT_FAILURE_KIND_BYTES + 1];
};

/* Not a SQLite code: diagnostic rows read cleanly but failed validation. */
enum { FAILURE_DAMAGED = -1 };

static bool failure_kind_valid(const unsigned char *kind, int length) {
    return kind != NULL && length > 0 && length <= MO_LIMIT_FAILURE_KIND_BYTES &&
           memchr(kind, 0, (size_t)length) == NULL && mo_utf8_valid(kind, (size_t)length);
}

/* SQLITE_OK, a SQLite error, or FAILURE_DAMAGED. */
static int failure_pairs_complete(struct mo_store *s) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT (SELECT count(*) FROM (SELECT id FROM failure LIMIT " TEXT_OF(
        MO_LIMIT_FAILURE_RECORDS) "+1)),"
                                  "(SELECT count(*) FROM (SELECT id FROM failure_state "
                                  "LIMIT " TEXT_OF(MO_LIMIT_FAILURE_RECORDS) "+1))";
    int code = sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL);
    if (code != SQLITE_OK)
        return code;
    int step = sqlite3_step(stmt);
    bool valid = false;
    if (step == SQLITE_ROW) {
        sqlite3_int64 n = sqlite3_column_int64(stmt, 0);
        valid = n <= MO_LIMIT_FAILURE_RECORDS && n == sqlite3_column_int64(stmt, 1);
    }
    code = sqlite3_finalize(stmt);
    if (step != SQLITE_ROW || code != SQLITE_OK)
        return step != SQLITE_ROW ? step : code;
    if (!valid)
        return FAILURE_DAMAGED;
    /* Each table now has at most 64 rows; indexed equality probes bound this scan. */
    code =
        sqlite3_prepare_v2(s->db,
                           "SELECT 1 FROM failure f LEFT JOIN failure_state s ON f.id=s.id WHERE "
                           "s.id IS NULL LIMIT 1",
                           -1, &stmt, NULL);
    if (code != SQLITE_OK)
        return code;
    step = sqlite3_step(stmt);
    code = sqlite3_finalize(stmt);
    if (step == SQLITE_ROW)
        return code == SQLITE_OK ? FAILURE_DAMAGED : code;
    return step != SQLITE_DONE ? step : code;
}

/* SQLITE_OK, a SQLite error, or FAILURE_DAMAGED. */
static int failure_rows(struct mo_store *s, const char *kind, struct failure_row *rows,
                        uint32_t *count, size_t *total) {
    sqlite3_stmt *stmt = NULL;
    int code = sqlite3_prepare_v2(s->db,
                                  "SELECT f.id,f.kind,f.first,length(f.message)+length(s.record) "
                                  "FROM failure f JOIN failure_state s ON f.id=s.id ORDER BY f.id",
                                  -1, &stmt, NULL);
    if (code != SQLITE_OK)
        return code;
    bool ok = true;
    int step;
    *count = 0;
    *total = 0;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        int length = sqlite3_column_bytes(stmt, 1);
        const unsigned char *text = sqlite3_column_text(stmt, 1);
        sqlite3_int64 id = sqlite3_column_int64(stmt, 0);
        sqlite3_int64 bytes = sqlite3_column_int64(stmt, 3);
        int first = sqlite3_column_int(stmt, 2);
        if (*count == MO_LIMIT_FAILURE_RECORDS || id <= 0 || !failure_kind_valid(text, length) ||
            bytes <= 0 || bytes > MO_LIMIT_FAILURE_STORE_BYTES || (first != 0 && first != 1) ||
            (size_t)bytes > MO_LIMIT_FAILURE_STORE_BYTES - *total) {
            ok = false;
            break;
        }
        struct failure_row *r = &rows[(*count)++];
        *r = (struct failure_row){.id = (uint64_t)id, .bytes = (size_t)bytes, .first = first != 0};
        memcpy(r->kind, text, (size_t)length);
        r->kind[length] = '\0';
        r->same = strcmp(r->kind, kind) == 0;
        *total += r->bytes;
    }
    code = sqlite3_finalize(stmt);
    if (!ok)
        return code == SQLITE_OK ? FAILURE_DAMAGED : code;
    return step != SQLITE_DONE ? step : code;
}

static int delete_failure(struct mo_store *s, uint64_t id) {
    const char *sql[] = {"DELETE FROM failure WHERE id=?1",
                         "DELETE FROM failure_state WHERE id=?1"};
    for (size_t i = 0; i < sizeof sql / sizeof *sql; i++) {
        sqlite3_stmt *stmt = NULL;
        int code = sqlite3_prepare_v2(s->db, sql[i], -1, &stmt, NULL);
        if (code != SQLITE_OK)
            return code;
        code = sqlite3_bind_int64(stmt, 1, (sqlite3_int64)id);
        if (code == SQLITE_OK) {
            code = sqlite3_step(stmt);
            if (code == SQLITE_DONE)
                code = SQLITE_OK;
        }
        int finalized = sqlite3_finalize(stmt);
        if (code != SQLITE_OK || finalized != SQLITE_OK)
            return code == SQLITE_OK ? finalized : code;
    }
    return SQLITE_OK;
}

/* A commit error is always uncertain. Before commit, only resource/constraint
 * refusals are recoverable, after rollback and an independent head read. */
static enum mo_store_capture_status capture_refusal(struct mo_store *s, int code) {
    bool rolled_back = sqlite3_get_autocommit(s->db) != 0 || exec(s, "ROLLBACK");
    uint64_t head = 0;
    bool empty = false;
    bool damaged = code == FAILURE_DAMAGED;
    code &= 0xff;
    bool recoverable = damaged || code == SQLITE_FULL || code == SQLITE_TOOBIG ||
                       code == SQLITE_NOMEM || code == SQLITE_CONSTRAINT || code == SQLITE_BUSY ||
                       code == SQLITE_LOCKED;
    if (!rolled_back || sqlite3_get_autocommit(s->db) == 0 || !recoverable ||
        current_head(s, &head, &empty) != MO_STORE_OK)
        return MO_STORE_CAPTURE_UNSAFE;
    return damaged ? MO_STORE_CAPTURE_DAMAGED : MO_STORE_CAPTURE_REFUSED;
}

/* The version-1 payload carries its own id, bound to the row id chosen here. */
static void bind_failure_message_id(struct mo_buffer *frame, uint64_t id) {
    uint8_t *payload = frame->bytes + FRAME_HEADER_BYTES;
    if (frame->length < MO_STORE_FRAME_BYTES + MO_FAILURE_ID_OFFSET + MO_FAILURE_ID_BYTES ||
        payload[0] != MO_FAILURE_VERSION)
        return;
    for (unsigned i = 0; i < MO_FAILURE_ID_BYTES; i++)
        payload[MO_FAILURE_ID_OFFSET + i] = (uint8_t)(id >> (8 * i));
    size_t checked = frame->length - FRAME_CHECKSUM_BYTES;
    uint64_t sum = mo_binary_checksum(frame->bytes, checked);
    for (unsigned i = 0; i < FRAME_CHECKSUM_BYTES; i++)
        frame->bytes[checked + i] = (uint8_t)(sum >> (8 * i));
}

enum mo_store_capture_status mo_store_capture(struct mo_store *s, const char *kind,
                                              const struct mo_store_record *state,
                                              const struct mo_store_record *message, uint64_t *id) {
    MO_ASSERT(s != NULL && kind != NULL && state != NULL && message != NULL && id != NULL);
    if (s->unsafe)
        return MO_STORE_CAPTURE_UNSAFE;
    size_t kind_length = 0;
    while (kind_length <= MO_LIMIT_FAILURE_KIND_BYTES && kind[kind_length] != 0)
        kind_length++;
    if (kind_length == 0 || kind_length > MO_LIMIT_FAILURE_KIND_BYTES ||
        state->kind != MO_STORE_FAILURE_STATE || message->kind != MO_STORE_FAILURE_MESSAGE ||
        state->sequence != message->sequence || state->image != message->image ||
        state->sequence > INT64_MAX || state->length > MO_LIMIT_SAVED_BYTES ||
        message->length > MO_LIMIT_FAILURE_MESSAGE_BYTES ||
        message->length >
            MO_LIMIT_FAILURE_RECORD_BYTES - MO_LIMIT_FAILURE_ENVELOPE_BYTES - state->length)
        return MO_STORE_CAPTURE_REFUSED;
    struct mo_buffer state_frame, message_frame;
    mo_buffer_init(&state_frame, MO_LIMIT_SAVED_BYTES + MO_STORE_FRAME_BYTES);
    mo_buffer_init(&message_frame, MO_LIMIT_FAILURE_MESSAGE_BYTES + MO_STORE_FRAME_BYTES);
    bool prepared = frame(state, &state_frame) && frame(message, &message_frame);
    enum mo_store_capture_status result = MO_STORE_CAPTURE_REFUSED;
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_FAILURE_SCRATCH);
    struct failure_row *rows = mo_arena_allocate_array(&arena, MO_LIMIT_FAILURE_RECORDS,
                                                       sizeof *rows, _Alignof(struct failure_row));
    if (!prepared || rows == NULL)
        goto done;
    if (!exec(s, "BEGIN IMMEDIATE")) {
        uint64_t head = 0;
        bool empty = false;
        int code = sqlite3_errcode(s->db);
        result = sqlite3_get_autocommit(s->db) != 0 &&
                         (code == SQLITE_BUSY || code == SQLITE_LOCKED || code == SQLITE_NOMEM) &&
                         current_head(s, &head, &empty) == MO_STORE_OK
                     ? MO_STORE_CAPTURE_REFUSED
                     : MO_STORE_CAPTURE_UNSAFE;
        goto done;
    }
    uint32_t count = 0;
    size_t total = 0;
    int checked = failure_pairs_complete(s);
    if (checked == SQLITE_OK)
        checked = failure_rows(s, kind, rows, &count, &total);
    if (checked != SQLITE_OK) {
        result = capture_refusal(s, checked);
        goto done;
    }
    uint64_t next = count == 0 ? 1 : rows[count - 1].id + 1;
    bind_failure_message_id(&message_frame, next);
    if (next > INT64_MAX) {
        result = capture_refusal(s, SQLITE_FULL);
        goto done;
    }
    bool first = true, ok = true;
    for (uint32_t i = 0; i < count; i++) {
        if (!rows[i].same)
            continue;
        first = false;
        if (!rows[i].first) {
            int deleted = delete_failure(s, rows[i].id);
            if (deleted != SQLITE_OK) {
                result = capture_refusal(s, deleted);
                goto done;
            }
            total -= rows[i].bytes;
            rows[i].id = 0;
        }
    }
    uint64_t evicted = 0;
    size_t added = state_frame.length + message_frame.length;
    for (;;) {
        uint32_t live = 0, oldest = MO_NO_INDEX;
        uint64_t oldest_latest = UINT64_MAX;
        for (uint32_t i = 0; i < count; i++) {
            if (rows[i].id == 0)
                continue;
            live++;
            if (rows[i].same)
                continue;
            uint64_t latest = rows[i].id;
            for (uint32_t j = i + 1; j < count; j++)
                if (rows[j].id != 0 && strcmp(rows[i].kind, rows[j].kind) == 0)
                    latest = rows[j].id;
            if (latest < oldest_latest) {
                oldest = i;
                oldest_latest = latest;
            }
        }
        if (live < MO_LIMIT_FAILURE_RECORDS && added <= MO_LIMIT_FAILURE_STORE_BYTES - total)
            break;
        /* Repeating kinds never evict another kind's first record. */
        if (!first || oldest == MO_NO_INDEX) {
            result = capture_refusal(s, SQLITE_FULL);
            goto done;
        }
        evicted++;
        for (uint32_t i = 0; i < count; i++)
            if (rows[i].id != 0 && strcmp(rows[i].kind, rows[oldest].kind) == 0) {
                int deleted = delete_failure(s, rows[i].id);
                if (deleted != SQLITE_OK) {
                    result = capture_refusal(s, deleted);
                    goto done;
                }
                total -= rows[i].bytes;
                rows[i].id = 0;
            }
    }
    sqlite3_stmt *stmt = NULL;
    ok = ok && sqlite3_prepare_v2(s->db, "INSERT INTO failure VALUES(?1,?2,?3,?4)", -1, &stmt,
                                  NULL) == SQLITE_OK;
    if (ok)
        ok = sqlite3_bind_int64(stmt, 1, (sqlite3_int64)next) == SQLITE_OK &&
             sqlite3_bind_text(stmt, 2, kind, (int)kind_length, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_bind_int(stmt, 3, first ? 1 : 0) == SQLITE_OK &&
             sqlite3_bind_blob(stmt, 4, message_frame.bytes, (int)message_frame.length,
                               SQLITE_STATIC) == SQLITE_OK;
    int code = ok ? sqlite3_step(stmt) : sqlite3_errcode(s->db);
    ok = code == SQLITE_DONE;
    if (ok)
        code = SQLITE_OK;
    int finalized = sqlite3_finalize(stmt);
    if (ok && finalized != SQLITE_OK) {
        ok = false;
        code = finalized;
    }
    if (ok) {
        code = write_blob_code(s, "INSERT INTO failure_state VALUES(?1,?2)", next, &state_frame);
        ok = code == SQLITE_OK;
    }
    if (!ok)
        result = capture_refusal(s, code);
    else if (!exec(s, "COMMIT")) {
        (void)exec(s, "ROLLBACK");
        result = MO_STORE_CAPTURE_UNSAFE;
    } else {
        *id = next;
        s->evicted_kinds =
            evicted > UINT64_MAX - s->evicted_kinds ? UINT64_MAX : s->evicted_kinds + evicted;
        result = MO_STORE_CAPTURE_OK;
    }
done:
    if (result == MO_STORE_CAPTURE_UNSAFE)
        s->unsafe = true;
    mo_arena_free(&arena);
    mo_buffer_free(&state_frame);
    mo_buffer_free(&message_frame);
    return result;
}

static enum mo_store_status failure_read(struct mo_store *s, uint64_t after, uint64_t id,
                                         bool metadata,
                                         bool (*consume)(const struct mo_store_failure *, void *),
                                         void *context) {
    MO_ASSERT(s != NULL && consume != NULL);
    if (s->unsafe)
        return MO_STORE_IO;
    int pairs = failure_pairs_complete(s);
    if (pairs != SQLITE_OK)
        return pairs == FAILURE_DAMAGED ? MO_STORE_CORRUPT : failure(pairs);
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        metadata
            ? "SELECT f.id,f.kind,f.first,length(s.record),f.message FROM failure f "
              "LEFT JOIN failure_state s ON f.id=s.id WHERE f.id>? ORDER BY f.id LIMIT " TEXT_OF(
                  MO_LIMIT_FAILURE_VIEW_RECORDS)
        : id != 0 ? "SELECT f.id,f.kind,f.first,s.record,f.message FROM failure f "
                    "LEFT JOIN failure_state s ON f.id=s.id WHERE f.id=?"
                  : "SELECT f.id,f.kind,f.first,s.record,f.message FROM failure f "
                    "LEFT JOIN failure_state s ON f.id=s.id ORDER BY f.id";
    int code = sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL);
    if (code == SQLITE_OK && (metadata || id != 0))
        code = sqlite3_bind_int64(stmt, 1, (sqlite3_int64)(metadata ? after : id));
    if (code != SQLITE_OK)
        return finish(stmt, failure(code));
    enum mo_store_status result = MO_STORE_OK;
    uint32_t count = 0;
    int step;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        struct mo_store_failure f = {0};
        sqlite3_int64 row_id = sqlite3_column_int64(stmt, 0);
        const unsigned char *kind = sqlite3_column_text(stmt, 1);
        int length = sqlite3_column_bytes(stmt, 1);
        int first = sqlite3_column_int(stmt, 2);
        if (++count > MO_LIMIT_FAILURE_RECORDS || row_id <= 0 ||
            !failure_kind_valid(kind, length) || (first != 0 && first != 1) ||
            (!metadata && row(stmt, 3, MO_STORE_FAILURE_STATE, &f.state) != MO_STORE_OK) ||
            row(stmt, 4, MO_STORE_FAILURE_MESSAGE, &f.message) != MO_STORE_OK ||
            (!metadata &&
             (f.state.sequence != f.message.sequence || f.state.image != f.message.image))) {
            result = MO_STORE_CORRUPT;
            break;
        }
        if (metadata) {
            sqlite3_int64 bytes = sqlite3_column_int64(stmt, 3);
            if (bytes < MO_STORE_FRAME_BYTES ||
                bytes > MO_LIMIT_SAVED_BYTES + MO_STORE_FRAME_BYTES) {
                result = MO_STORE_CORRUPT;
                break;
            }
            f.state.length = (size_t)bytes - MO_STORE_FRAME_BYTES;
        }
        f.id = (uint64_t)row_id;
        f.kind = (const char *)kind;
        f.first = first != 0;
        if (!consume(&f, context)) {
            result = MO_STORE_MEMORY;
            break;
        }
    }
    if (result == MO_STORE_OK && step != SQLITE_DONE)
        result = failure(step);
    return finish(stmt, result);
}

enum mo_store_status mo_store_failures(struct mo_store *s,
                                       bool (*consume)(const struct mo_store_failure *, void *),
                                       void *context) {
    return failure_read(s, 0, 0, false, consume, context);
}

enum mo_store_status mo_store_failure_get(struct mo_store *s, uint64_t id,
                                          bool (*consume)(const struct mo_store_failure *, void *),
                                          void *context) {
    if (id == 0 || id > INT64_MAX)
        return MO_STORE_CORRUPT;
    return failure_read(s, 0, id, false, consume, context);
}

enum mo_store_status mo_store_failure_catalog(struct mo_store *s, uint64_t after,
                                              bool (*consume)(const struct mo_store_failure *,
                                                              void *),
                                              void *context, uint32_t *omitted) {
    if (after > INT64_MAX)
        return MO_STORE_CORRUPT;
    sqlite3_stmt *stmt = NULL;
    int code =
        sqlite3_prepare_v2(s->db,
                           "SELECT count(*) FROM (SELECT id FROM failure WHERE id>? LIMIT " TEXT_OF(
                               MO_LIMIT_FAILURE_RECORDS) "+1)",
                           -1, &stmt, NULL);
    if (code == SQLITE_OK)
        code = sqlite3_bind_int64(stmt, 1, (sqlite3_int64)after);
    if (code == SQLITE_OK)
        code = sqlite3_step(stmt);
    sqlite3_int64 count = code == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : -1;
    enum mo_store_status status = finish(stmt, code == SQLITE_ROW ? MO_STORE_OK : failure(code));
    if (status != MO_STORE_OK)
        return status;
    if (count < 0 || count > MO_LIMIT_FAILURE_RECORDS)
        return MO_STORE_CORRUPT;
    status = failure_read(s, after, 0, true, consume, context);
    if (status == MO_STORE_OK)
        *omitted = count > MO_LIMIT_FAILURE_VIEW_RECORDS
                       ? (uint32_t)count - MO_LIMIT_FAILURE_VIEW_RECORDS
                       : 0;
    return status;
}

struct failure_export {
    uint64_t id;
    struct mo_buffer *out;
    bool found;
};
static bool export_failure(const struct mo_store_failure *f, void *context) {
    struct failure_export *e = context;
    if (f->id != e->id)
        return true;
    struct mo_buffer part;
    mo_buffer_init(&part, MO_LIMIT_SAVED_BYTES + MO_STORE_FRAME_BYTES);
    bool ok = frame(&f->state, &part) && mo_binary_write(e->out, part.length, 4) &&
              mo_buffer_append(e->out, part.bytes, part.length);
    mo_buffer_clear(&part);
    ok = ok && frame(&f->message, &part) && mo_binary_write(e->out, part.length, 4) &&
         mo_buffer_append(e->out, part.bytes, part.length);
    mo_buffer_free(&part);
    e->found = ok;
    return ok;
}
bool mo_store_failure_export(struct mo_store *s, uint64_t id, struct mo_buffer *out) {
    size_t start = out->length;
    struct failure_export e = {id, out, false};
    bool ok = mo_store_failure_get(s, id, export_failure, &e) == MO_STORE_OK && e.found;
    if (!ok)
        out->length = start;
    return ok;
}

enum mo_store_status mo_store_failure_stats(struct mo_store *s, uint32_t *count,
                                            uint64_t *evicted) {
    if (s->unsafe)
        return MO_STORE_IO;
    sqlite3_stmt *stmt = NULL;
    int code = sqlite3_prepare_v2(s->db,
                                  "SELECT count(*) FROM (SELECT id FROM failure LIMIT " TEXT_OF(
                                      MO_LIMIT_FAILURE_RECORDS) "+1)",
                                  -1, &stmt, NULL);
    if (code != SQLITE_OK)
        return failure(code);
    int step = sqlite3_step(stmt);
    sqlite3_int64 n = step == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : -1;
    enum mo_store_status result =
        n >= 0 && n <= MO_LIMIT_FAILURE_RECORDS ? MO_STORE_OK : MO_STORE_CORRUPT;
    result = finish(stmt, result);
    if (result == MO_STORE_OK) {
        *count = (uint32_t)n;
        *evicted = s->evicted_kinds;
    }
    return result;
}

/* Reading a stopped folder. Every lock the reader takes is SQLite's shared lock for one read
 * transaction, so a process killed mid-read leaves none behind. */

static enum mo_store_folder folder_code(int code) {
    if (code == SQLITE_READONLY_ROLLBACK)
        return MO_STORE_FOLDER_RECOVERY;
    code &= 0xff;
    if (code == SQLITE_BUSY)
        return MO_STORE_FOLDER_OWNED;
    if (code == SQLITE_CANTOPEN || code == SQLITE_PERM)
        return MO_STORE_FOLDER_UNREADABLE;
    if (code == SQLITE_CORRUPT || code == SQLITE_NOTADB)
        return MO_STORE_FOLDER_DAMAGED;
    return code == SQLITE_NOMEM ? MO_STORE_FOLDER_MEMORY : MO_STORE_FOLDER_IO;
}

/* Once the read lock is held, only a stored value or a resource can fail. */
static enum mo_store_folder folder_status(enum mo_store_status status) {
    if (status == MO_STORE_OK)
        return MO_STORE_FOLDER_OK;
    if (status == MO_STORE_CORRUPT)
        return MO_STORE_FOLDER_DAMAGED;
    return status == MO_STORE_MEMORY ? MO_STORE_FOLDER_MEMORY : MO_STORE_FOLDER_IO;
}

/* No CREATE, no journal or locking pragmas and no busy timeout. */
static enum mo_store_folder reader_open(const char *path, bool write, struct mo_store **out) {
    if (pthread_once(&once, initialize) != 0 || initialized != SQLITE_OK)
        return MO_STORE_FOLDER_IO;
    if (!claim_store())
        return MO_STORE_FOLDER_HANDLES;
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_SERVICE_METADATA);
    struct mo_store *s = mo_arena_allocate(&arena, sizeof *s, _Alignof(struct mo_store));
    if (s == NULL) {
        release_store();
        return MO_STORE_FOLDER_MEMORY;
    }
    *s = (struct mo_store){.arena = arena};
    int flags = (write ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY) | SQLITE_OPEN_FULLMUTEX;
    int code = sqlite3_open_v2(path, &s->db, flags, NULL);
    if (code == SQLITE_OK)
        code = sqlite3_extended_result_codes(s->db, 1);
    if (code != SQLITE_OK) {
        mo_store_close(s);
        return folder_code(code);
    }
    (void)sqlite3_limit(s->db, SQLITE_LIMIT_LENGTH, MO_LIMIT_STORE_ROW);
    (void)sqlite3_limit(s->db, SQLITE_LIMIT_SQL_LENGTH, MO_LIMIT_STORE_SQL);
    if (sqlite3_db_config(s->db, SQLITE_DBCONFIG_DEFENSIVE, 1, NULL) != SQLITE_OK ||
        sqlite3_db_config(s->db, SQLITE_DBCONFIG_ENABLE_TRIGGER, 0, NULL) != SQLITE_OK ||
        sqlite3_db_config(s->db, SQLITE_DBCONFIG_ENABLE_VIEW, 0, NULL) != SQLITE_OK) {
        mo_store_close(s);
        return MO_STORE_FOLDER_IO;
    }
    /* Setting the cache size reads the file, so it can be the first read. */
    code = sqlite3_exec(s->db, "PRAGMA trusted_schema=OFF;" CACHE_PRAGMA "BEGIN", NULL, NULL, NULL);
    if (code != SQLITE_OK) {
        mo_store_close(s);
        return folder_code(code);
    }
    *out = s;
    return MO_STORE_FOLDER_OK;
}

/* The first read takes the lock, so its code is the one that classifies the folder. The
 * check never creates a table. */
static enum mo_store_folder reader_tables(struct mo_store *s, bool *failure_tables) {
    enum table_state states[TABLE_COUNT];
    for (unsigned i = 0; i < TABLE_COUNT; i++) {
        int code = SQLITE_OK;
        states[i] = table_state(s, &tables[i], &code);
        if (states[i] == TABLE_FAILED)
            return folder_code(code);
        if (i < FAILURE_TABLES && states[i] != TABLE_SAME)
            return MO_STORE_FOLDER_DAMAGED;
    }
    for (unsigned i = FAILURE_TABLES; i < TABLE_COUNT; i++)
        if (states[i] != states[FAILURE_TABLES] ||
            (states[i] != TABLE_SAME && states[i] != TABLE_ABSENT))
            return MO_STORE_FOLDER_DAMAGED;
    *failure_tables = states[FAILURE_TABLES] == TABLE_SAME;
    return MO_STORE_FOLDER_OK;
}

typedef enum mo_store_folder (*folder_body)(struct mo_store *s, bool failure_tables, void *context);

static enum mo_store_folder read_once(const char *path, bool write, folder_body body, void *context,
                                      bool *began) {
    struct mo_store *s = NULL;
    bool failure_tables = false;
    enum mo_store_folder status = reader_open(path, write, &s);
    if (status == MO_STORE_FOLDER_OK)
        status = reader_tables(s, &failure_tables);
    *began = status == MO_STORE_FOLDER_OK;
    if (status == MO_STORE_FOLDER_OK)
        status = body(s, failure_tables, context);
    if (s != NULL) {
        bool ended = exec(s, status == MO_STORE_FOLDER_OK ? "COMMIT" : "ROLLBACK");
        if (status == MO_STORE_FOLDER_OK && !ended)
            status = MO_STORE_FOLDER_IO;
        mo_store_close(s);
    }
    return status;
}

static enum mo_store_folder read_folder(const char *path, bool recover, bool *recovered,
                                        folder_body body, void *context) {
    MO_ASSERT(path != NULL);
    *recovered = false;
    struct stat info;
    if (stat(path, &info) != 0)
        return errno == ENOENT ? MO_STORE_FOLDER_MISSING : MO_STORE_FOLDER_UNREADABLE;
    bool began = false;
    enum mo_store_folder status = read_once(path, false, body, context, &began);
    if (status == MO_STORE_FOLDER_RECOVERY && recover) {
        MO_ASSERT(!began);
        status = read_once(path, true, body, context, &began);
        *recovered = began;
    }
    return status;
}

/* Log rows are checked in pieces of this size, so a whole row is never held. */
enum { HISTORY_CHUNK_BYTES = 64 << 10 };
/* What the copy may take beyond its retained bytes: its tables, its checking buffer, and the
 * chunks the arena leaves partly used. */
#define HISTORY_ARENA_BYTES ((size_t)2 * MO_LIMIT_HISTORY_BYTES + (size_t)(4 << 20))

struct history_copy {
    struct mo_store_history *out;
    struct mo_store_history_entry *entries;
    struct mo_store_history_failure *failures;
    uint8_t *chunk;
    uint64_t last_id;
    uint32_t page;
    bool full, memory;
};

/* Bytes are copied in order until the first that would pass the bound; after that every
 * byte is left. */
static bool retain(struct history_copy *c, uint64_t bytes) {
    struct mo_store_history *h = c->out;
    if (!c->full && bytes <= MO_LIMIT_HISTORY_BYTES - h->retained_bytes) {
        h->retained_bytes += bytes;
        return true;
    }
    c->full = true;
    h->left_bytes += bytes;
    return false;
}

static const uint8_t *copy_bytes(struct mo_arena *arena, const uint8_t *bytes, size_t length) {
    uint8_t *copy = mo_arena_allocate(arena, length, 1);
    if (copy != NULL && length != 0)
        memcpy(copy, bytes, length);
    return copy;
}

static bool copy_snapshot(const struct mo_store_record *r, void *context) {
    struct history_copy *c = context;
    struct mo_store_history *h = c->out;
    h->has_snapshot = true;
    h->snapshot = *r;
    h->snapshot.bytes = NULL;
    if (retain(c, r->length)) {
        h->snapshot.bytes = copy_bytes(&h->arena, r->bytes, r->length);
        c->memory = h->snapshot.bytes == NULL;
    }
    return !c->memory;
}

static uint64_t checksum_more(uint64_t sum, const uint8_t *bytes, size_t length) {
    for (size_t i = 0; i < length; i++)
        sum = (sum ^ bytes[i]) * MO_FNV_PRIME;
    return sum;
}

struct log_blob {
    sqlite3_blob *blob;
    int code;
};
static bool read_log_payload(void *context, size_t offset, uint8_t *out, size_t count) {
    struct log_blob *b = context;
    MO_ASSERT(offset <= MO_LIMIT_SAVED_BYTES && count <= MO_LIMIT_SAVED_BYTES);
    b->code = sqlite3_blob_read(b->blob, out, (int)count, (int)(FRAME_HEADER_BYTES + offset));
    return b->code == SQLITE_OK;
}

/* One log row of length bytes: its frame, then its checksum over every byte, read in fixed
 * pieces, then its payload's metadata. */
static enum mo_store_status stream_log(struct mo_store *s, uint64_t sequence, size_t length,
                                       uint64_t image, uint8_t *chunk,
                                       struct mo_log_record_metadata *out) {
    MO_ASSERT(length >= MO_STORE_FRAME_BYTES && length <= MO_LIMIT_STORE_ROW);
    sqlite3_blob *blob = NULL;
    int code = sqlite3_blob_open(s->db, "main", "log", "record", (sqlite3_int64)sequence, 0, &blob);
    if (code != SQLITE_OK) {
        MO_ASSERT(sqlite3_blob_close(blob) == SQLITE_OK);
        return failure(code);
    }
    enum mo_store_status result = MO_STORE_OK;
    uint8_t header[FRAME_HEADER_BYTES], tail[FRAME_CHECKSUM_BYTES];
    uint64_t seq = 0, frame_image = 0, payload = 0, expected = 0;
    struct mo_binary in = {header, sizeof header, 6};
    if (sqlite3_blob_bytes(blob) < 0 || (size_t)sqlite3_blob_bytes(blob) != length)
        result = MO_STORE_CORRUPT;
    else if ((code = sqlite3_blob_read(blob, header, sizeof header, 0)) != SQLITE_OK)
        result = failure(code);
    else if (memcmp(header, "MOST\1", 5) != 0 || header[5] != MO_STORE_LOG ||
             !mo_binary_uint(&in, 8, &seq) || !mo_binary_uint(&in, 8, &frame_image) ||
             !mo_binary_uint(&in, 4, &payload) || seq != sequence || frame_image != image ||
             payload != length - MO_STORE_FRAME_BYTES)
        result = MO_STORE_CORRUPT;
    uint64_t sum = checksum_more(MO_FNV_OFFSET, header, sizeof header);
    size_t end = length - FRAME_CHECKSUM_BYTES;
    for (size_t at = sizeof header; result == MO_STORE_OK && at < end;) {
        size_t n = end - at < HISTORY_CHUNK_BYTES ? end - at : HISTORY_CHUNK_BYTES;
        code = sqlite3_blob_read(blob, chunk, (int)n, (int)at);
        if (code != SQLITE_OK)
            result = failure(code);
        sum = checksum_more(sum, chunk, n);
        at += n;
    }
    struct mo_binary checksum = {tail, sizeof tail, 0};
    if (result == MO_STORE_OK &&
        (code = sqlite3_blob_read(blob, tail, sizeof tail, (int)end)) != SQLITE_OK)
        result = failure(code);
    else if (result == MO_STORE_OK &&
             (!mo_binary_uint(&checksum, FRAME_CHECKSUM_BYTES, &expected) || expected != sum))
        result = MO_STORE_CORRUPT;
    struct log_blob reader = {blob, SQLITE_OK};
    if (result == MO_STORE_OK &&
        !mo_log_record_walk((size_t)payload, read_log_payload, &reader, out))
        result = reader.code != SQLITE_OK ? failure(reader.code) : MO_STORE_CORRUPT;
    code = sqlite3_blob_close(blob);
    return result == MO_STORE_OK && code != SQLITE_OK ? failure(code) : result;
}

static enum mo_store_status copy_log(struct mo_store *s, const struct saved_head *head,
                                     struct history_copy *c) {
    struct mo_store_history *h = c->out;
    sqlite3_stmt *stmt = NULL;
    int code = sqlite3_prepare_v2(s->db,
                                  "SELECT seq,length(record),typeof(record)='blob' FROM log "
                                  "ORDER BY seq",
                                  -1, &stmt, NULL);
    if (code != SQLITE_OK)
        return failure(code);
    enum mo_store_status result = MO_STORE_OK;
    uint64_t sequence = head->sequence;
    int step;
    while (result == MO_STORE_OK && (step = sqlite3_step(stmt)) == SQLITE_ROW) {
        sqlite3_int64 length = sqlite3_column_int64(stmt, 1);
        if (!next_log_row(head, stmt, sequence) || sqlite3_column_int(stmt, 2) != 1 ||
            length < MO_STORE_FRAME_BYTES || length > MO_LIMIT_SAVED_BYTES + MO_STORE_FRAME_BYTES) {
            sqlite3_int64 n = sqlite3_column_int64(stmt, 0);
            h->damaged_sequence = n > 0 ? (uint64_t)n : 0;
            result = MO_STORE_CORRUPT;
            break;
        }
        sequence++;
        struct mo_log_record_metadata m;
        result = stream_log(s, sequence, (size_t)length, head->image, c->chunk, &m);
        if (result == MO_STORE_CORRUPT)
            h->damaged_sequence = sequence;
        if (result != MO_STORE_OK)
            break;
        MO_ASSERT(h->log_count < MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES);
        h->log_count++;
        size_t sizes = m.header.count * sizeof m.argument_bytes[0];
        if (!retain(c, sizeof(struct mo_store_history_entry) + sizes))
            continue;
        uint32_t *argument_bytes = mo_arena_allocate(&h->arena, sizes, _Alignof(uint32_t));
        if (argument_bytes == NULL) {
            c->memory = true;
            result = MO_STORE_MEMORY;
            break;
        }
        memcpy(argument_bytes, m.argument_bytes, sizes);
        c->entries[h->entry_count++] =
            (struct mo_store_history_entry){.sequence = sequence,
                                            .arrival = m.header.arrival,
                                            .handler = m.header.handler,
                                            .argument_count = m.header.count,
                                            .timer = m.header.timer,
                                            .argument_bytes = argument_bytes};
    }
    if (result == MO_STORE_OK && step != SQLITE_DONE)
        result = failure(step);
    result = finish(stmt, result);
    return result == MO_STORE_OK && sequence != head->head ? MO_STORE_CORRUPT : result;
}

static bool copy_failure_metadata(const struct mo_store_failure *f, void *context) {
    struct history_copy *c = context;
    struct mo_store_history *h = c->out;
    c->page++;
    c->last_id = f->id;
    MO_ASSERT(h->failure_total < MO_LIMIT_FAILURE_RECORDS);
    h->failure_total++;
    size_t kind = strlen(f->kind) + 1;
    if (!retain(c, sizeof(struct mo_store_history_failure) + kind + f->message.length))
        return true;
    const char *copied = (const char *)copy_bytes(&h->arena, (const uint8_t *)f->kind, kind);
    const uint8_t *message = copy_bytes(&h->arena, f->message.bytes, f->message.length);
    if (copied == NULL || message == NULL) {
        c->memory = true;
        return false;
    }
    struct mo_store_history_failure *x = &c->failures[h->failure_count++];
    *x = (struct mo_store_history_failure){.id = f->id,
                                           .kind = copied,
                                           .first = f->first,
                                           .state_bytes = f->state.length,
                                           .message = f->message};
    x->message.bytes = message;
    return true;
}

static enum mo_store_folder history_body(struct mo_store *s, bool failure_tables, void *context) {
    struct history_copy *c = context;
    struct mo_store_history *h = c->out;
    h->failure_tables = failure_tables;
    struct saved_head head;
    enum mo_store_status status = load_snapshot(s, &head, copy_snapshot, c);
    if (status == MO_STORE_CORRUPT && c->memory)
        status = MO_STORE_MEMORY;
    if (status != MO_STORE_OK)
        return folder_status(status);
    h->head = head.head;
    status = copy_log(s, &head, c);
    for (c->last_id = 0; status == MO_STORE_OK && failure_tables;) {
        c->page = 0;
        status = failure_read(s, c->last_id, 0, true, copy_failure_metadata, c);
        if (c->page < MO_LIMIT_FAILURE_VIEW_RECORDS)
            break;
    }
    return folder_status(status);
}

enum mo_store_folder mo_store_history_read(const char *path, bool recover,
                                           struct mo_store_history *out) {
    *out = (struct mo_store_history){0};
    mo_arena_init(&out->arena, HISTORY_ARENA_BYTES);
    struct history_copy c = {.out = out};
    c.entries = mo_arena_allocate_array(&out->arena, MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES,
                                        sizeof *c.entries, _Alignof(struct mo_store_history_entry));
    c.failures = mo_arena_allocate_array(&out->arena, MO_LIMIT_FAILURE_RECORDS, sizeof *c.failures,
                                         _Alignof(struct mo_store_history_failure));
    c.chunk = mo_arena_allocate(&out->arena, HISTORY_CHUNK_BYTES, 1);
    bool recovered = false;
    enum mo_store_folder status = c.entries == NULL || c.failures == NULL || c.chunk == NULL
                                      ? MO_STORE_FOLDER_MEMORY
                                      : read_folder(path, recover, &recovered, history_body, &c);
    out->entries = c.entries;
    out->failures = c.failures;
    out->recovered = recovered;
    if (status != MO_STORE_FOLDER_OK) {
        uint64_t damaged = out->damaged_sequence;
        mo_arena_free(&out->arena);
        *out = (struct mo_store_history){
            .recovered = recovered, .damaged_sequence = damaged, .arena = out->arena};
    }
    return status;
}

void mo_store_history_free(struct mo_store_history *history) {
    mo_arena_free(&history->arena);
}

struct pair_copy {
    struct mo_store_history_pair *out;
    bool memory;
};

static bool copy_pair(const struct mo_store_failure *f, void *context) {
    struct pair_copy *c = context;
    struct mo_store_history_pair *p = c->out;
    const char *kind = mo_arena_copy_text(&p->arena, f->kind);
    const uint8_t *state = copy_bytes(&p->arena, f->state.bytes, f->state.length);
    const uint8_t *message = copy_bytes(&p->arena, f->message.bytes, f->message.length);
    if (kind == NULL || state == NULL || message == NULL) {
        c->memory = true;
        return false;
    }
    p->found = true;
    p->kind = kind;
    p->first = f->first;
    p->state = f->state;
    p->state.bytes = state;
    p->message = f->message;
    p->message.bytes = message;
    return true;
}

static enum mo_store_status retained_ids(struct mo_store *s, struct mo_store_history_pair *p) {
    sqlite3_stmt *stmt = NULL;
    int code = sqlite3_prepare_v2(
        s->db, "SELECT id FROM failure ORDER BY id LIMIT " TEXT_OF(MO_LIMIT_FAILURE_RECORDS) "+1",
        -1, &stmt, NULL);
    if (code != SQLITE_OK)
        return failure(code);
    enum mo_store_status result = MO_STORE_OK;
    int step;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        sqlite3_int64 id = sqlite3_column_int64(stmt, 0);
        if (p->retained_count == MO_LIMIT_FAILURE_RECORDS || id <= 0) {
            result = MO_STORE_CORRUPT;
            break;
        }
        p->retained[p->retained_count++] = (uint64_t)id;
    }
    if (result == MO_STORE_OK && step != SQLITE_DONE)
        result = failure(step);
    return finish(stmt, result);
}

static enum mo_store_folder pair_body(struct mo_store *s, bool failure_tables, void *context) {
    struct pair_copy *c = context;
    struct mo_store_history_pair *p = c->out;
    p->failure_tables = failure_tables;
    if (!failure_tables)
        return MO_STORE_FOLDER_OK;
    enum mo_store_status status = MO_STORE_OK;
    if (p->id != 0 && p->id <= INT64_MAX) {
        status = mo_store_failure_get(s, p->id, copy_pair, c);
        if (status == MO_STORE_MEMORY && !c->memory)
            status = MO_STORE_CORRUPT;
    }
    if (status == MO_STORE_OK && !p->found)
        status = retained_ids(s, p);
    return folder_status(status);
}

enum mo_store_folder mo_store_history_pair(const char *path, bool recover, uint64_t id,
                                           struct mo_store_history_pair *out) {
    *out = (struct mo_store_history_pair){.id = id};
    mo_arena_init(&out->arena, 2 * (size_t)MO_LIMIT_FAILURE_RECORD_BYTES);
    struct pair_copy c = {.out = out};
    bool recovered = false;
    enum mo_store_folder status = read_folder(path, recover, &recovered, pair_body, &c);
    out->recovered = recovered;
    if (status != MO_STORE_FOLDER_OK) {
        mo_arena_free(&out->arena);
        *out =
            (struct mo_store_history_pair){.id = id, .recovered = recovered, .arena = out->arena};
    }
    return status;
}

void mo_store_history_pair_free(struct mo_store_history_pair *pair) {
    mo_arena_free(&pair->arena);
}
