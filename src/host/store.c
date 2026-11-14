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

/* Whether the table exists as the runtime creates it. FAILED leaves SQLite's code in code. */
enum table_state store_table_state(struct mo_store *s, const struct store_table *t, int *code) {
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

bool store_write_blob(struct mo_store *s, const char *sql, uint64_t seq,
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
