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
        !table(s, "failure",
               "CREATE TABLE failure(id INTEGER PRIMARY KEY, kind TEXT NOT NULL, first INTEGER "
               "NOT NULL, message BLOB NOT NULL) STRICT") ||
        !table(s, "failure_state",
               "CREATE TABLE failure_state(id INTEGER PRIMARY KEY, record BLOB NOT NULL) STRICT") ||
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

static bool failure_pairs_complete(struct mo_store *s) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT (SELECT count(*) FROM (SELECT id FROM failure LIMIT " TEXT_OF(
        MO_LIMIT_FAILURE_RECORDS) "+1)),"
                                  "(SELECT count(*) FROM (SELECT id FROM failure_state "
                                  "LIMIT " TEXT_OF(MO_LIMIT_FAILURE_RECORDS) "+1))";
    if (sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;
    bool ok = sqlite3_step(stmt) == SQLITE_ROW;
    if (ok) {
        sqlite3_int64 n = sqlite3_column_int64(stmt, 0);
        ok = n <= MO_LIMIT_FAILURE_RECORDS && n == sqlite3_column_int64(stmt, 1);
    }
    ok = sqlite3_finalize(stmt) == SQLITE_OK && ok;
    if (!ok)
        return false;
    /* Each table now has at most 64 rows; indexed equality probes bound this scan. */
    if (sqlite3_prepare_v2(s->db,
                           "SELECT 1 FROM failure f LEFT JOIN failure_state s ON f.id=s.id WHERE "
                           "s.id IS NULL LIMIT 1",
                           -1, &stmt, NULL) != SQLITE_OK)
        return false;
    ok = sqlite3_step(stmt) == SQLITE_DONE;
    return sqlite3_finalize(stmt) == SQLITE_OK && ok;
}

static bool failure_rows(struct mo_store *s, const char *kind, struct failure_row *rows,
                         uint32_t *count, size_t *total) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db,
                           "SELECT f.id,f.kind,f.first,length(f.message)+length(s.record) "
                           "FROM failure f JOIN failure_state s ON f.id=s.id ORDER BY f.id",
                           -1, &stmt, NULL) != SQLITE_OK)
        return false;
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
        if (*count == MO_LIMIT_FAILURE_RECORDS || id <= 0 || length <= 0 ||
            length > MO_LIMIT_FAILURE_KIND_BYTES || text == NULL ||
            memchr(text, 0, (size_t)length) != NULL || bytes <= 0 ||
            bytes > MO_LIMIT_FAILURE_STORE_BYTES || (first != 0 && first != 1) ||
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
    ok = ok && step == SQLITE_DONE;
    return sqlite3_finalize(stmt) == SQLITE_OK && ok;
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
    code &= 0xff;
    bool recoverable = code == SQLITE_FULL || code == SQLITE_TOOBIG || code == SQLITE_NOMEM ||
                       code == SQLITE_CONSTRAINT || code == SQLITE_BUSY || code == SQLITE_LOCKED;
    return rolled_back && sqlite3_get_autocommit(s->db) != 0 && recoverable &&
                   current_head(s, &head, &empty) == MO_STORE_OK
               ? MO_STORE_CAPTURE_REFUSED
               : MO_STORE_CAPTURE_UNSAFE;
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
    if (!failure_pairs_complete(s) || !failure_rows(s, kind, rows, &count, &total)) {
        result = capture_refusal(s, sqlite3_errcode(s->db));
        goto done;
    }
    uint64_t next = count == 0 ? 1 : rows[count - 1].id + 1;
    /* Bind a versioned message payload's embedded id to both committed rows. */
    if (message->length >= 9 && message->bytes[0] == 1) {
        for (unsigned i = 0; i < 8; i++)
            message_frame.bytes[26 + 1 + i] = (uint8_t)(next >> (8 * i));
        uint64_t sum = mo_binary_checksum(message_frame.bytes, message_frame.length - 8);
        for (unsigned i = 0; i < 8; i++)
            message_frame.bytes[message_frame.length - 8 + i] = (uint8_t)(sum >> (8 * i));
    }
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
    if (!failure_pairs_complete(s))
        return MO_STORE_CORRUPT;
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
        if (++count > MO_LIMIT_FAILURE_RECORDS || row_id <= 0 || kind == NULL || length <= 0 ||
            length > MO_LIMIT_FAILURE_KIND_BYTES || memchr(kind, 0, (size_t)length) != NULL ||
            (first != 0 && first != 1) ||
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
