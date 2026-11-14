#include "host/store-internal.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "base/binary.h"
#include "base/bundle.h"
#include "base/compile-context.h"
#include "base/compile-metadata.h"
#include "base/export.h"
#include "base/failure.h"
#include "base/intent-list.h"
#include "base/log-record.h"
#include "base/sha256.h"
#include "base/store-memory.h"
#include "base/utf8.h"

#define CACHE_PRAGMA "PRAGMA cache_size=-" TEXT_OF(MO_LIMIT_STORE_CACHE_KIB) ";"

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

bool store_exec(struct mo_store *s, const char *sql) {
    return sqlite3_exec(s->db, sql, NULL, NULL, NULL) == SQLITE_OK;
}

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
enum { FAILURE_TABLES = 3, TABLE_COUNT = sizeof tables / sizeof tables[0] };

enum table_state { TABLE_FAILED, TABLE_ABSENT, TABLE_SAME, TABLE_DIFFERENT };

/* Whether the table exists as the runtime creates it. FAILED leaves SQLite's code in code. */
static enum table_state store_table_state(struct mo_store *s, const struct store_table *t,
                                          int *code) {
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

bool store_table_ready(struct mo_store *s, const struct store_table *t) {
    int code;
    enum table_state state = store_table_state(s, t, &code);
    return state == TABLE_SAME || (state == TABLE_ABSENT && store_exec(s, t->sql));
}

static bool all_tables(struct mo_store *s) {
    for (unsigned i = 0; i < TABLE_COUNT; i++)
        if (!store_table_ready(s, &tables[i]))
            return false;
    return true;
}

/* A store with no connection yet, in its own arena, or NULL with the claim released. */
static struct mo_store *store_new(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_SERVICE_METADATA);
    struct mo_store *s = mo_arena_allocate(&arena, sizeof *s, _Alignof(struct mo_store));
    if (s == NULL) {
        release_store();
        return NULL;
    }
    *s = (struct mo_store){.arena = arena};
    return s;
}

static bool defensive(struct mo_store *s) {
    return sqlite3_db_config(s->db, SQLITE_DBCONFIG_DEFENSIVE, 1, NULL) == SQLITE_OK &&
           sqlite3_db_config(s->db, SQLITE_DBCONFIG_ENABLE_TRIGGER, 0, NULL) == SQLITE_OK &&
           sqlite3_db_config(s->db, SQLITE_DBCONFIG_ENABLE_VIEW, 0, NULL) == SQLITE_OK;
}

_Static_assert(MO_LIMIT_SERVE_STORE_WAIT_US / 1000 <= INT_MAX, "the wait fits SQLite's timeout");

static enum mo_store_status rollback_close(struct mo_store *s, enum mo_store_status status) {
    (void)store_exec(s, "ROLLBACK");
    mo_store_close(s);
    return status;
}

static enum mo_store_status open_store(const char *path, bool wait, struct mo_store **out) {
    struct stat info;
    if (path != NULL && path[0] == '\0')
        return MO_STORE_EMPTY_PATH;
    if (path != NULL && strcmp(path, ":memory:") == 0)
        return MO_STORE_MEMORY_PATH;
    /* Leftover empty files are corrupt. SQLite keeps a first commit empty until COMMIT, so a
     * waiter must take the lock before deciding. Without the wait, refuse at once. */
    bool existed_empty = path != NULL && stat(path, &info) == 0 && info.st_size == 0;
    if (existed_empty && !wait)
        return MO_STORE_CORRUPT;
    if (pthread_once(&once, initialize) != 0 || initialized != SQLITE_OK)
        return MO_STORE_IO;
    if (!claim_store())
        return MO_STORE_HANDLES;
    struct mo_store *s = store_new();
    if (s == NULL)
        return MO_STORE_MEMORY;
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
    if (!defensive(s) ||
        !store_exec(s, "PRAGMA trusted_schema=OFF; PRAGMA journal_mode=DELETE;"
                       "PRAGMA synchronous=EXTRA; PRAGMA temp_store=MEMORY;" CACHE_PRAGMA
                       "PRAGMA locking_mode=EXCLUSIVE;BEGIN EXCLUSIVE")) {
        bool busy = (sqlite3_errcode(s->db) & 0xff) == SQLITE_BUSY;
        return rollback_close(s, busy ? MO_STORE_BUSY : MO_STORE_IO);
    }
    if (existed_empty) {
        MO_ASSERT(path != NULL);
        if (stat(path, &info) != 0)
            return rollback_close(s, MO_STORE_IO);
        if (info.st_size == 0)
            return rollback_close(s, MO_STORE_CORRUPT);
    }
    if (!all_tables(s) || !store_exec(s, "COMMIT")) {
        bool busy = (sqlite3_errcode(s->db) & 0xff) == SQLITE_BUSY;
        return rollback_close(s, busy ? MO_STORE_BUSY : MO_STORE_IO);
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

_Static_assert(FRAME_HEADER_BYTES + FRAME_CHECKSUM_BYTES == MO_STORE_FRAME_BYTES,
               "frame layout matches its envelope");

bool store_frame(const struct mo_store_record *r, struct mo_buffer *out) {
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
enum mo_store_status store_sqlite_status(int code) {
    code &= 0xff;
    return code == SQLITE_CORRUPT || code == SQLITE_NOTADB || code == SQLITE_TOOBIG
               ? MO_STORE_CORRUPT
               : MO_STORE_IO;
}

enum mo_store_status store_finish(sqlite3_stmt *stmt, enum mo_store_status result) {
    int code = sqlite3_finalize(stmt);
    return result == MO_STORE_OK && code != SQLITE_OK ? store_sqlite_status(code) : result;
}

enum mo_store_status store_row(sqlite3_stmt *stmt, int column, enum mo_store_record_kind kind,
                               struct mo_store_record *out) {
    if (sqlite3_column_type(stmt, column) != SQLITE_BLOB)
        return MO_STORE_CORRUPT;
    const uint8_t *bytes = sqlite3_column_blob(stmt, column);
    int length = sqlite3_column_bytes(stmt, column);
    return bytes != NULL && mo_store_record_read(kind, bytes, (size_t)length, out)
               ? MO_STORE_OK
               : MO_STORE_CORRUPT;
}

enum mo_store_status store_current_head(struct mo_store *s, uint64_t *head, bool *empty) {
    sqlite3_stmt *stmt = NULL;
    int code = sqlite3_prepare_v2(s->db, "SELECT seq FROM head WHERE id=1", -1, &stmt, NULL);
    if (code != SQLITE_OK)
        return store_sqlite_status(code);
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
        result = store_sqlite_status(step);
    return store_finish(stmt, result);
}

enum mo_store_status store_version(struct mo_store *s, int *version) {
    sqlite3_stmt *stmt = NULL;
    int code = sqlite3_prepare_v2(s->db, "PRAGMA user_version", -1, &stmt, NULL);
    if (code != SQLITE_OK)
        return store_sqlite_status(code);
    int step = sqlite3_step(stmt);
    *version = step == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : -1;
    enum mo_store_status result =
        store_finish(stmt, step == SQLITE_ROW ? MO_STORE_OK : store_sqlite_status(step));
    return result == MO_STORE_OK && (*version < 0 || *version > 3) ? MO_STORE_CORRUPT : result;
}

/* An empty store has no snapshot, so no callback. A refusing callback is CORRUPT. */
enum mo_store_status store_load_snapshot(struct mo_store *s, struct saved_head *h,
                                         bool (*consume)(const struct mo_store_record *, void *),
                                         void *context) {
    int version;
    enum mo_store_status result = store_version(s, &version);
    if (result != MO_STORE_OK)
        return result;
    *h = (struct saved_head){0};
    result = store_current_head(s, &h->head, &h->empty);
    if (result != MO_STORE_OK)
        return result;
    sqlite3_stmt *stmt = NULL;
    int code = sqlite3_prepare_v2(s->db, "SELECT record FROM snapshot WHERE id=1", -1, &stmt, NULL);
    if (code != SQLITE_OK)
        return store_sqlite_status(code);
    int step = sqlite3_step(stmt);
    struct mo_store_record record;
    if (step != SQLITE_ROW && step != SQLITE_DONE)
        result = store_sqlite_status(step);
    else if (h->empty || step == SQLITE_DONE)
        result = h->empty && step == SQLITE_DONE && version == 0 ? MO_STORE_OK : MO_STORE_CORRUPT;
    else if (version == 0)
        result = MO_STORE_CORRUPT;
    else if ((result = store_row(stmt, 0, MO_STORE_STATE, &record)) == MO_STORE_OK) {
        if (version >= 2) {
            struct mo_store_program program;
            bool found;
            uint8_t schema[32];
            result = store_read_program(s, NULL, &program, &found);
            if (result != MO_STORE_OK || !found || program.image != record.image ||
                !mo_export_schema(record.bytes, record.length, schema) ||
                memcmp(program.schema, schema, 32) != 0)
                return store_finish(stmt, result == MO_STORE_OK ? MO_STORE_CORRUPT : result);
        }
        h->sequence = record.sequence;
        h->image = record.image;
        if (h->sequence > h->head || h->head - h->sequence > MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES ||
            !consume(&record, context))
            result = MO_STORE_CORRUPT;
    }
    return store_finish(stmt, result);
}

/* The log row in stmt's first column is the one after sequence, within the head. */
bool store_next_log_row(const struct saved_head *h, sqlite3_stmt *stmt, uint64_t sequence) {
    bool integer = sqlite3_column_type(stmt, 0) == SQLITE_INTEGER;
    sqlite3_int64 n = sqlite3_column_int64(stmt, 0);
    return !h->empty && sequence < h->head && integer && n > 0 && (uint64_t)n == sequence + 1;
}

static enum mo_store_status
load(struct mo_store *s, bool (*consume)(const struct mo_store_record *, void *), void *context) {
    struct saved_head h;
    enum mo_store_status result = store_load_snapshot(s, &h, consume, context);
    if (result != MO_STORE_OK)
        return result;
    uint64_t sequence = h.sequence;
    sqlite3_stmt *stmt = NULL;
    int code =
        sqlite3_prepare_v2(s->db, "SELECT seq,record FROM log ORDER BY seq", -1, &stmt, NULL);
    if (code != SQLITE_OK)
        return store_sqlite_status(code);
    int step;
    struct mo_store_record record;
    while (result == MO_STORE_OK && (step = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (!store_next_log_row(&h, stmt, sequence))
            result = MO_STORE_CORRUPT;
        else if ((result = store_row(stmt, 1, MO_STORE_LOG, &record)) == MO_STORE_OK &&
                 (record.sequence != sequence + 1 || record.image != h.image ||
                  !consume(&record, context)))
            result = MO_STORE_CORRUPT;
        else
            sequence++;
    }
    if (result == MO_STORE_OK && step != SQLITE_DONE)
        result = store_sqlite_status(step);
    result = store_finish(stmt, result);
    return result == MO_STORE_OK && sequence != h.head ? MO_STORE_CORRUPT : result;
}

int store_write_blob_code(struct mo_store *s, const char *sql, uint64_t seq,
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

static bool store_write_blob(struct mo_store *s, const char *sql, uint64_t seq,
                             const struct mo_buffer *bytes) {
    return store_write_blob_code(s, sql, seq, bytes) == SQLITE_OK;
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
    if (!store_exec(s, reading ? "BEGIN" : "BEGIN IMMEDIATE"))
        return MO_STORE_IO;
    enum mo_store_status result = MO_STORE_OK;
    if (reading)
        result = load(s, consume, context);
    else {
        struct mo_buffer bytes;
        mo_buffer_init(&bytes, MO_LIMIT_SAVED_BYTES + MO_STORE_FRAME_BYTES);
        uint64_t head = 0;
        bool empty = false;
        bool ok =
            store_current_head(s, &head, &empty) == MO_STORE_OK && store_frame(record, &bytes);
        if (op == MO_STORE_APPEND) {
            ok = ok && !empty && head < INT64_MAX && record->sequence == head + 1 &&
                 record->kind == MO_STORE_LOG &&
                 store_write_blob(s, "INSERT INTO log VALUES(?1,?2)", record->sequence, &bytes) &&
                 store_exec(s, "UPDATE head SET seq=seq+1 WHERE id=1");
        } else {
            MO_ASSERT(op == MO_STORE_SNAPSHOT);
            ok = ok && (!empty || store_exec(s, "PRAGMA user_version=1")) &&
                 record->sequence == head && record->kind == MO_STORE_STATE &&
                 store_write_blob(s,
                                  "INSERT INTO snapshot VALUES(1,?2) ON CONFLICT(id) DO UPDATE"
                                  " SET record=excluded.record",
                                  record->sequence, &bytes) &&
                 store_exec(s, "INSERT INTO head VALUES(1,0) ON CONFLICT(id) DO NOTHING;"
                               "DELETE FROM log");
        }
        mo_buffer_free(&bytes);
        if (!ok)
            result = MO_STORE_IO;
    }
    if (result == MO_STORE_OK && !store_exec(s, "COMMIT"))
        result = MO_STORE_IO;
    if (result != MO_STORE_OK)
        (void)store_exec(s, "ROLLBACK");
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
enum mo_store_folder store_folder_status(enum mo_store_status status) {
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
    struct mo_store *s = store_new();
    if (s == NULL)
        return MO_STORE_FOLDER_MEMORY;
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
    if (!defensive(s)) {
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
        states[i] = store_table_state(s, &tables[i], &code);
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
        bool ended = store_exec(s, status == MO_STORE_FOLDER_OK ? "COMMIT" : "ROLLBACK");
        if (status == MO_STORE_FOLDER_OK && !ended)
            status = MO_STORE_FOLDER_IO;
        mo_store_close(s);
    }
    return status;
}

enum mo_store_folder store_read_folder(const char *path, bool recover, bool *recovered,
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
