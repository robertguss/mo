#include "host/store-internal.h"

#include <string.h>

#include "base/log-record.h"

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

static enum mo_store_status close_blob(sqlite3_blob *blob, enum mo_store_status result) {
    int code = sqlite3_blob_close(blob);
    return result == MO_STORE_OK && code != SQLITE_OK ? store_sqlite_status(code) : result;
}

/* Header, payload and checksum of one MOST blob, read in HISTORY_CHUNK_BYTES pieces so the
 * payload is never held. On OK, *blob is left open for a payload walk; the caller closes it.
 * Any other status closes it. */
static enum mo_store_status stream_most(struct mo_store *s, const char *table, sqlite3_int64 rowid,
                                        enum mo_store_record_kind kind, size_t length,
                                        uint64_t sequence, uint64_t image, uint8_t *chunk,
                                        sqlite3_blob **blob) {
    MO_ASSERT(s != NULL && table != NULL && chunk != NULL && blob != NULL);
    MO_ASSERT(length >= MO_STORE_FRAME_BYTES && length <= MO_LIMIT_STORE_ROW);
    *blob = NULL;
    int code = sqlite3_blob_open(s->db, "main", table, "record", rowid, 0, blob);
    if (code != SQLITE_OK) {
        MO_ASSERT(sqlite3_blob_close(*blob) == SQLITE_OK);
        *blob = NULL;
        return store_sqlite_status(code);
    }
    enum mo_store_status result = MO_STORE_OK;
    uint8_t header[FRAME_HEADER_BYTES], tail[FRAME_CHECKSUM_BYTES];
    uint64_t seq = 0, frame_image = 0, payload = 0, expected = 0;
    struct mo_binary in = {header, sizeof header, 6};
    if (sqlite3_blob_bytes(*blob) < 0 || (size_t)sqlite3_blob_bytes(*blob) != length)
        result = MO_STORE_CORRUPT;
    else if ((code = sqlite3_blob_read(*blob, header, sizeof header, 0)) != SQLITE_OK)
        result = store_sqlite_status(code);
    else if (memcmp(header, "MOST\1", 5) != 0 || header[5] != (uint8_t)kind ||
             !mo_binary_uint(&in, 8, &seq) || !mo_binary_uint(&in, 8, &frame_image) ||
             !mo_binary_uint(&in, 4, &payload) || seq != sequence || frame_image != image ||
             payload != length - MO_STORE_FRAME_BYTES)
        result = MO_STORE_CORRUPT;
    uint64_t sum = checksum_more(MO_FNV_OFFSET, header, sizeof header);
    size_t end = length - FRAME_CHECKSUM_BYTES;
    for (size_t at = sizeof header; result == MO_STORE_OK && at < end;) {
        size_t n = end - at < HISTORY_CHUNK_BYTES ? end - at : HISTORY_CHUNK_BYTES;
        code = sqlite3_blob_read(*blob, chunk, (int)n, (int)at);
        if (code != SQLITE_OK)
            result = store_sqlite_status(code);
        sum = checksum_more(sum, chunk, n);
        at += n;
    }
    struct mo_binary checksum = {tail, sizeof tail, 0};
    if (result == MO_STORE_OK &&
        (code = sqlite3_blob_read(*blob, tail, sizeof tail, (int)end)) != SQLITE_OK)
        result = store_sqlite_status(code);
    else if (result == MO_STORE_OK &&
             (!mo_binary_uint(&checksum, FRAME_CHECKSUM_BYTES, &expected) || expected != sum))
        result = MO_STORE_CORRUPT;
    if (result != MO_STORE_OK) {
        result = close_blob(*blob, result);
        *blob = NULL;
        return result;
    }
    return result;
}

/* One log row of length bytes: its frame, then its checksum over every byte, read in fixed
 * pieces, then its payload's metadata. */
static enum mo_store_status stream_log(struct mo_store *s, uint64_t sequence, size_t length,
                                       uint64_t image, uint8_t *chunk,
                                       struct mo_log_record_metadata *out) {
    sqlite3_blob *blob = NULL;
    enum mo_store_status result = stream_most(s, "log", (sqlite3_int64)sequence, MO_STORE_LOG,
                                              length, sequence, image, chunk, &blob);
    struct log_blob reader = {blob, SQLITE_OK};
    if (result == MO_STORE_OK &&
        !mo_log_record_walk(length - MO_STORE_FRAME_BYTES, read_log_payload, &reader, out))
        result = reader.code != SQLITE_OK ? store_sqlite_status(reader.code) : MO_STORE_CORRUPT;
    return close_blob(blob, result);
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
        return store_sqlite_status(code);
    enum mo_store_status result = MO_STORE_OK;
    uint64_t sequence = head->sequence;
    int step;
    while (result == MO_STORE_OK && (step = sqlite3_step(stmt)) == SQLITE_ROW) {
        sqlite3_int64 length = sqlite3_column_int64(stmt, 1);
        if (!store_next_log_row(head, stmt, sequence) || sqlite3_column_int(stmt, 2) != 1 ||
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
        result = store_sqlite_status(step);
    result = store_finish(stmt, result);
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
                                           .state_valid = false,
                                           .state_bytes = f->state.length,
                                           .message = f->message};
    x->message.bytes = message;
    return true;
}

static enum mo_store_status copy_program_history(struct mo_store *s, struct history_copy *c);

static enum mo_store_status copy_review_history(struct mo_store *s, struct history_copy *c);

static enum mo_store_folder history_body(struct mo_store *s, bool failure_tables, void *context) {
    struct history_copy *c = context;
    struct mo_store_history *h = c->out;
    h->failure_tables = failure_tables;
    struct saved_head head;
    enum mo_store_status status = store_load_snapshot(s, &head, copy_snapshot, c);
    if (status == MO_STORE_CORRUPT && c->memory)
        status = MO_STORE_MEMORY;
    if (status != MO_STORE_OK)
        return store_folder_status(status);
    h->head = head.head;
    status = copy_log(s, &head, c);
    for (c->last_id = 0; status == MO_STORE_OK && failure_tables;) {
        c->page = 0;
        status = store_failure_read(s, c->last_id, 0, true, copy_failure_metadata, c);
        if (c->page < MO_LIMIT_FAILURE_VIEW_RECORDS)
            break;
    }
    for (uint32_t i = 0; status == MO_STORE_OK && i < h->failure_count; i++) {
        struct mo_store_history_failure *x = &c->failures[i];
        sqlite3_blob *blob = NULL;
        enum mo_store_status frame =
            stream_most(s, "failure_state", (sqlite3_int64)x->id, MO_STORE_FAILURE_STATE,
                        x->state_bytes + MO_STORE_FRAME_BYTES, x->message.sequence,
                        x->message.image, c->chunk, &blob);
        if (frame == MO_STORE_OK) {
            status = close_blob(blob, MO_STORE_OK);
            x->state_valid = status == MO_STORE_OK;
        } else if (frame == MO_STORE_CORRUPT) {
            x->state_valid = false;
        } else {
            status = frame;
        }
    }
    if (status == MO_STORE_OK)
        status = copy_program_history(s, c);
    if (status == MO_STORE_OK)
        status = copy_review_history(s, c);
    return store_folder_status(status);
}

enum mo_store_folder mo_store_history_read(const char *path, bool recover,
                                           struct mo_store_history *out) {
    *out = (struct mo_store_history){0};
    mo_arena_init_confidential(&out->arena, HISTORY_ARENA_BYTES);
    struct history_copy c = {.out = out};
    c.entries = mo_arena_allocate_array(&out->arena, MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES,
                                        sizeof *c.entries, _Alignof(struct mo_store_history_entry));
    c.failures = mo_arena_allocate_array(&out->arena, MO_LIMIT_FAILURE_RECORDS, sizeof *c.failures,
                                         _Alignof(struct mo_store_history_failure));
    c.chunk = mo_arena_allocate(&out->arena, HISTORY_CHUNK_BYTES, 1);
    bool recovered = false;
    enum mo_store_folder status =
        c.entries == NULL || c.failures == NULL || c.chunk == NULL
            ? MO_STORE_FOLDER_MEMORY
            : store_read_folder(path, recover, &recovered, history_body, &c);
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
        return store_sqlite_status(code);
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
        result = store_sqlite_status(step);
    return store_finish(stmt, result);
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
    return store_folder_status(status);
}

enum mo_store_folder mo_store_history_pair(const char *path, bool recover, uint64_t id,
                                           struct mo_store_history_pair *out) {
    *out = (struct mo_store_history_pair){.id = id};
    mo_arena_init_confidential(&out->arena, 2 * (size_t)MO_LIMIT_FAILURE_RECORD_BYTES);
    struct pair_copy c = {.out = out};
    bool recovered = false;
    enum mo_store_folder status = store_read_folder(path, recover, &recovered, pair_body, &c);
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

static enum mo_store_status copy_program_history(struct mo_store *s, struct history_copy *c) {
    struct mo_store_history *h = c->out;
    enum mo_store_status status = store_read_program(s, NULL, &h->program, &h->has_program);
    h->program.context = NULL;
    if (status != MO_STORE_OK || !h->has_program)
        return status;
    sqlite3_stmt *stmt = NULL;
    int code =
        sqlite3_prepare_v2(s->db,
                           "SELECT ordinal,source,image,build,schema,bundle,context_digest,context "
                           "FROM previous_programs ORDER BY ordinal",
                           -1, &stmt, NULL);
    if (code != SQLITE_OK)
        return store_sqlite_status(code);
    struct mo_store_history_program **tail = &h->previous_programs;
    int step;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        struct mo_store_program program = {0};
        if (!store_program_row(stmt, &program) || program.ordinal == 0 ||
            program.ordinal > h->program.ordinal) {
            status = MO_STORE_CORRUPT;
            break;
        }
        if (!retain(c, sizeof **tail))
            continue;
        struct mo_store_history_program *node =
            mo_arena_allocate(&h->arena, sizeof *node, _Alignof(struct mo_store_history_program));
        if (node == NULL) {
            status = MO_STORE_MEMORY;
            break;
        }
        program.bundle = NULL;
        program.context = NULL;
        *node = (struct mo_store_history_program){.program = program};
        *tail = node;
        tail = &node->next;
    }
    if (status == MO_STORE_OK && step != SQLITE_DONE)
        status = store_sqlite_status(step);
    status = store_finish(stmt, status);
    if (status != MO_STORE_OK)
        return status;
    code = sqlite3_prepare_v2(
        s->db, "SELECT ordinal,head,old_source,new_source FROM upgrades ORDER BY ordinal", -1,
        &stmt, NULL);
    if (code != SQLITE_OK)
        return store_sqlite_status(code);
    struct mo_store_history_upgrade **upgrade = &h->upgrades;
    uint64_t last = 0;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (sqlite3_column_type(stmt, 0) != SQLITE_INTEGER || sqlite3_column_int64(stmt, 0) <= 0 ||
            sqlite3_column_type(stmt, 1) != SQLITE_INTEGER || sqlite3_column_int64(stmt, 1) < 0 ||
            !store_blob_size(stmt, 2, 32) || !store_blob_size(stmt, 3, 32)) {
            status = MO_STORE_CORRUPT;
            break;
        }
        uint64_t ordinal = (uint64_t)sqlite3_column_int64(stmt, 0);
        if (ordinal != last + 1 || ordinal > h->program.ordinal) {
            status = MO_STORE_CORRUPT;
            break;
        }
        last = ordinal;
        h->upgrade_count++;
        if (!retain(c, sizeof **upgrade)) {
            h->upgrades_truncated = true;
            continue;
        }
        struct mo_store_history_upgrade *node =
            mo_arena_allocate(&h->arena, sizeof *node, _Alignof(struct mo_store_history_upgrade));
        if (node == NULL) {
            status = MO_STORE_MEMORY;
            break;
        }
        *node =
            (struct mo_store_history_upgrade){.ordinal = ordinal,
                                              .head = (uint64_t)sqlite3_column_int64(stmt, 1),
                                              .program_retained = ordinal == h->program.ordinal};
        memcpy(node->old_source, sqlite3_column_blob(stmt, 2), 32);
        memcpy(node->new_source, sqlite3_column_blob(stmt, 3), 32);
        for (const struct mo_store_history_program *p = h->previous_programs; p != NULL;
             p = p->next)
            if (p->program.ordinal == ordinal + 1 &&
                memcmp(node->new_source, p->program.source, 32) == 0)
                node->program_retained = true;
        *upgrade = node;
        upgrade = &node->next;
    }
    if (status == MO_STORE_OK && step != SQLITE_DONE)
        status = store_sqlite_status(step);
    if (status == MO_STORE_OK && last != h->program.ordinal)
        status = MO_STORE_CORRUPT;
    return store_finish(stmt, status);
}

static bool text_valid(sqlite3_stmt *stmt, int column) {
    const unsigned char *text = sqlite3_column_text(stmt, column);
    size_t length = (size_t)sqlite3_column_bytes(stmt, column);
    return sqlite3_column_type(stmt, column) == SQLITE_TEXT && text != NULL &&
           length <= MO_LIMIT_INTENT_LIST_BYTES && memchr(text, 0, length) == NULL;
}

static const char *copy_text(struct history_copy *c, sqlite3_stmt *stmt, int column) {
    const unsigned char *text = sqlite3_column_text(stmt, column);
    size_t length = (size_t)sqlite3_column_bytes(stmt, column);
    char *copy = mo_arena_allocate(&c->out->arena, length + 1, 1);
    if (copy != NULL) {
        memcpy(copy, text, length);
        copy[length] = 0;
    }
    return copy;
}

static enum mo_store_status copy_approvals(struct mo_store *s, struct history_copy *c) {
    struct mo_store_history *h = c->out;
    sqlite3_stmt *stmt = NULL;
    int code = sqlite3_prepare_v2(
        s->db, "SELECT digest,version FROM approvals ORDER BY version,digest", -1, &stmt, NULL);
    if (code != SQLITE_OK)
        return store_sqlite_status(code);
    struct mo_store_history_approval **tail = &h->approvals;
    enum mo_store_status status = MO_STORE_OK;
    int step;
    while (status == MO_STORE_OK && (step = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (!store_blob_size(stmt, 0, 32) || sqlite3_column_type(stmt, 1) != SQLITE_INTEGER ||
            sqlite3_column_int64(stmt, 1) <= 0 ||
            (uint64_t)sqlite3_column_int64(stmt, 1) > h->review_version)
            status = MO_STORE_CORRUPT;
        else if (!retain(c, sizeof **tail))
            h->review_truncated = true;
        else if ((*tail = mo_arena_allocate(&h->arena, sizeof **tail,
                                            _Alignof(struct mo_store_history_approval))) == NULL)
            status = MO_STORE_MEMORY;
        else {
            **tail = (struct mo_store_history_approval){
                .version = (uint64_t)sqlite3_column_int64(stmt, 1)};
            memcpy((*tail)->digest, sqlite3_column_blob(stmt, 0), 32);
            tail = &(*tail)->next;
        }
    }
    if (status == MO_STORE_OK && step != SQLITE_DONE)
        status = store_sqlite_status(step);
    return store_finish(stmt, status);
}

static enum mo_store_status copy_decisions(struct mo_store *s, struct history_copy *c) {
    struct mo_store_history *h = c->out;
    sqlite3_stmt *stmt = NULL;
    int code = sqlite3_prepare_v2(
        s->db, "SELECT seq,time,kind,base,setting,detail FROM decisions ORDER BY seq", -1, &stmt,
        NULL);
    if (code != SQLITE_OK)
        return store_sqlite_status(code);
    struct mo_store_history_decision **tail = &h->decisions;
    enum mo_store_status status = MO_STORE_OK;
    int step;
    while (status == MO_STORE_OK && (step = sqlite3_step(stmt)) == SQLITE_ROW) {
        const unsigned char *setting = sqlite3_column_text(stmt, 4);
        if (sqlite3_column_type(stmt, 0) != SQLITE_INTEGER || sqlite3_column_int64(stmt, 0) <= 0 ||
            sqlite3_column_type(stmt, 1) != SQLITE_INTEGER || !store_blob_size(stmt, 3, 32) ||
            !text_valid(stmt, 2) || sqlite3_column_bytes(stmt, 2) == 0 || !text_valid(stmt, 5) ||
            setting == NULL ||
            (strcmp((const char *)setting, "everything") != 0 &&
             strcmp((const char *)setting, "intent") != 0)) {
            status = MO_STORE_CORRUPT;
            break;
        }
        h->decision_count++;
        size_t bytes = sizeof **tail + (size_t)sqlite3_column_bytes(stmt, 2) +
                       (size_t)sqlite3_column_bytes(stmt, 5) + 2;
        if (!retain(c, bytes)) {
            h->review_truncated = true;
            continue;
        }
        struct mo_store_history_decision *node =
            mo_arena_allocate(&h->arena, sizeof *node, _Alignof(struct mo_store_history_decision));
        if (node == NULL) {
            status = MO_STORE_MEMORY;
            break;
        }
        *node =
            (struct mo_store_history_decision){.sequence = (uint64_t)sqlite3_column_int64(stmt, 0),
                                               .time = sqlite3_column_int64(stmt, 1),
                                               .intent = setting[0] == 'i',
                                               .kind = copy_text(c, stmt, 2),
                                               .detail = copy_text(c, stmt, 5)};
        memcpy(node->base, sqlite3_column_blob(stmt, 3), 32);
        if (node->kind == NULL || node->detail == NULL) {
            status = MO_STORE_MEMORY;
            break;
        }
        *tail = node;
        tail = &node->next;
    }
    if (status == MO_STORE_OK && step != SQLITE_DONE)
        status = store_sqlite_status(step);
    return store_finish(stmt, status);
}

static enum mo_store_status copy_review_history(struct mo_store *s, struct history_copy *c) {
    struct mo_store_history *h = c->out;
    int version;
    enum mo_store_status status = store_version(s, &version);
    if (status != MO_STORE_OK || version != 3)
        return status;
    struct mo_store_review review = {0};
    status = store_read_setting(s, &review);
    if (status != MO_STORE_OK)
        return status;
    h->reviewed = true;
    h->review_intent = review.intent;
    h->review_version = review.version;
    status = copy_approvals(s, c);
    return status == MO_STORE_OK ? copy_decisions(s, c) : status;
}
