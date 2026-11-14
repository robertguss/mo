#include "host/store-internal.h"

#include <string.h>

#include "base/export.h"
#include "base/utf8.h"

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
    /* Each table now has at most 64 rows; indexed equality probes bound this scan. Ids start
     * at 1, and a read of ids after a cursor would pass over one below it. */
    code =
        sqlite3_prepare_v2(s->db,
                           "SELECT 1 FROM failure f LEFT JOIN failure_state s ON f.id=s.id WHERE "
                           "s.id IS NULL OR f.id<=0 LIMIT 1",
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
    bool rolled_back = sqlite3_get_autocommit(s->db) != 0 || store_exec(s, "ROLLBACK");
    uint64_t head = 0;
    bool empty = false;
    bool damaged = code == FAILURE_DAMAGED;
    code &= 0xff;
    bool recoverable = damaged || code == SQLITE_FULL || code == SQLITE_TOOBIG ||
                       code == SQLITE_NOMEM || code == SQLITE_CONSTRAINT || code == SQLITE_BUSY ||
                       code == SQLITE_LOCKED;
    if (!rolled_back || sqlite3_get_autocommit(s->db) == 0 || !recoverable ||
        store_current_head(s, &head, &empty) != MO_STORE_OK)
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
    bool prepared = store_frame(state, &state_frame) && store_frame(message, &message_frame);
    enum mo_store_capture_status result = MO_STORE_CAPTURE_REFUSED;
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_FAILURE_SCRATCH);
    struct failure_row *rows = mo_arena_allocate_array(&arena, MO_LIMIT_FAILURE_RECORDS,
                                                       sizeof *rows, _Alignof(struct failure_row));
    if (!prepared || rows == NULL)
        goto done;
    if (!store_exec(s, "BEGIN IMMEDIATE")) {
        uint64_t head = 0;
        bool empty = false;
        int code = sqlite3_errcode(s->db);
        result = sqlite3_get_autocommit(s->db) != 0 &&
                         (code == SQLITE_BUSY || code == SQLITE_LOCKED || code == SQLITE_NOMEM) &&
                         store_current_head(s, &head, &empty) == MO_STORE_OK
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
        code =
            store_write_blob_code(s, "INSERT INTO failure_state VALUES(?1,?2)", next, &state_frame);
        ok = code == SQLITE_OK;
    }
    if (!ok)
        result = capture_refusal(s, code);
    else if (!store_exec(s, "COMMIT")) {
        (void)store_exec(s, "ROLLBACK");
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

enum mo_store_status store_failure_read(struct mo_store *s, uint64_t after, uint64_t id,
                                        bool metadata,
                                        bool (*consume)(const struct mo_store_failure *, void *),
                                        void *context) {
    MO_ASSERT(s != NULL && consume != NULL);
    if (s->unsafe)
        return MO_STORE_IO;
    int pairs = failure_pairs_complete(s);
    if (pairs != SQLITE_OK)
        return pairs == FAILURE_DAMAGED ? MO_STORE_CORRUPT : store_sqlite_status(pairs);
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
        return store_finish(stmt, store_sqlite_status(code));
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
            (!metadata && store_row(stmt, 3, MO_STORE_FAILURE_STATE, &f.state) != MO_STORE_OK) ||
            store_row(stmt, 4, MO_STORE_FAILURE_MESSAGE, &f.message) != MO_STORE_OK ||
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
        result = store_sqlite_status(step);
    return store_finish(stmt, result);
}

enum mo_store_status mo_store_failures(struct mo_store *s,
                                       bool (*consume)(const struct mo_store_failure *, void *),
                                       void *context) {
    return store_failure_read(s, 0, 0, false, consume, context);
}

enum mo_store_status mo_store_failure_get(struct mo_store *s, uint64_t id,
                                          bool (*consume)(const struct mo_store_failure *, void *),
                                          void *context) {
    if (id == 0 || id > INT64_MAX)
        return MO_STORE_CORRUPT;
    return store_failure_read(s, 0, id, false, consume, context);
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
    enum mo_store_status status =
        store_finish(stmt, code == SQLITE_ROW ? MO_STORE_OK : store_sqlite_status(code));
    if (status != MO_STORE_OK)
        return status;
    if (count < 0 || count > MO_LIMIT_FAILURE_RECORDS)
        return MO_STORE_CORRUPT;
    status = store_failure_read(s, after, 0, true, consume, context);
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
    bool ok = store_frame(&f->state, &part) && mo_binary_write(e->out, part.length, 4) &&
              mo_buffer_append(e->out, part.bytes, part.length);
    mo_buffer_clear(&part);
    ok = ok && store_frame(&f->message, &part) && mo_binary_write(e->out, part.length, 4) &&
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
        return store_sqlite_status(code);
    int step = sqlite3_step(stmt);
    sqlite3_int64 n = step == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : -1;
    enum mo_store_status result =
        n >= 0 && n <= MO_LIMIT_FAILURE_RECORDS ? MO_STORE_OK : MO_STORE_CORRUPT;
    result = store_finish(stmt, result);
    if (result == MO_STORE_OK) {
        *count = (uint32_t)n;
        *evicted = s->evicted_kinds;
    }
    return result;
}
