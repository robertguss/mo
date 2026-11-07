/* The reader of a stopped service's data folder, over stores whose rows come from the input.
 *
 * Each input builds one store file from the runtime's own empty store: well-formed frames with
 * fuzzed payloads, raw fuzzed blobs, and edits to sequences, ids, the head and the version.
 * Then mo_store_history_read and mo_store_history_pair run over it. A model of the store's
 * rules, written here from the rows the input wrote, predicts each read's status and every
 * copied field; the reader must agree exactly, keep what it retains within
 * MO_LIMIT_HISTORY_BYTES, and leave the file's bytes unchanged. The Makefile shortens that
 * bound for this target so that small inputs pass it. */
#define _POSIX_C_SOURCE 200809L
#include "base/binary.h"
#include "base/log-record.h"
#include "base/utf8.h"
#include "host/host.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* Bounds on one input's work: rows past these are not written. MAX_LOG passes the 100
 * messages a snapshot covers and MAX_FAILURES passes the 64 records a store keeps. */
enum {
    INPUT_BYTES = 1 << 16,
    MAX_OPS = 512,
    MAX_LOG = 160,
    MAX_FAILURES = 80,
    POOL_BYTES = 1 << 20,
    FILE_BYTES = 1 << 21
};

struct blob {
    uint8_t *bytes;
    size_t length;
};
struct log_row {
    int64_t seq;
    struct blob record;
};
struct failure_row {
    int64_t id, first;
    struct blob kind, message;
};
struct state_row {
    int64_t id;
    struct blob record;
};
/* Every row the store file holds, as the input wrote it. */
struct model {
    bool failure_tables, state_table_only_dropped, recover;
    bool head_present, snapshot_present;
    int64_t head;
    int32_t version;
    uint64_t image, pair_id;
    struct blob snapshot;
    struct log_row logs[MAX_LOG];
    uint32_t log_count;
    struct failure_row failures[MAX_FAILURES];
    uint32_t failure_count;
    struct state_row states[MAX_FAILURES];
    uint32_t state_count;
};

static uint8_t pool[POOL_BYTES];
static size_t pool_used;

static struct blob keep(const uint8_t *bytes, size_t length) {
    MO_ASSERT(length <= POOL_BYTES - pool_used);
    struct blob b = {pool + pool_used, length};
    if (length != 0)
        memcpy(b.bytes, bytes, length);
    pool_used += length;
    return b;
}

struct input {
    const uint8_t *bytes;
    size_t length;
};
static uint8_t take(struct input *in) {
    if (in->length == 0)
        return 0;
    in->length--;
    return *in->bytes++;
}
static size_t take_span(struct input *in, size_t want, const uint8_t **out) {
    size_t n = want < in->length ? want : in->length;
    *out = in->bytes;
    in->bytes += n;
    in->length -= n;
    return n;
}
static int64_t take_signed(struct input *in) {
    return (int64_t)(int8_t)take(in);
}

static struct blob frame(enum mo_store_record_kind kind, uint64_t seq, uint64_t image,
                         const uint8_t *payload, size_t length) {
    struct mo_buffer b;
    mo_buffer_init(&b, MO_LIMIT_SAVED_BYTES + MO_STORE_FRAME_BYTES);
    MO_ASSERT(mo_buffer_append(&b, "MOST\1", 5) && mo_binary_write(&b, (uint64_t)kind, 1) &&
              mo_binary_write(&b, seq, 8) && mo_binary_write(&b, image, 8) &&
              mo_binary_write(&b, length, 4) && mo_buffer_append(&b, payload, length));
    MO_ASSERT(mo_binary_write(&b, mo_binary_checksum(b.bytes, b.length), 8));
    struct blob r = keep(b.bytes, b.length);
    mo_buffer_free(&b);
    return r;
}

static struct blob fuzzed_frame(struct input *in, enum mo_store_record_kind kind, uint64_t seq,
                                uint64_t image) {
    const uint8_t *payload;
    size_t length = take_span(in, take(in), &payload);
    return frame(kind, seq, image, payload, length);
}

static struct blob raw(struct input *in) {
    const uint8_t *bytes;
    size_t length = take_span(in, take(in), &bytes);
    return keep(bytes, length);
}

static void flip(struct blob *b, struct input *in) {
    uint8_t at = take(in), mask = take(in);
    if (b->length != 0)
        b->bytes[at % b->length] ^= (uint8_t)(mask | 1);
}

static bool log_seq_used(const struct model *m, int64_t seq) {
    for (uint32_t i = 0; i < m->log_count; i++)
        if (m->logs[i].seq == seq)
            return true;
    return false;
}
static bool failure_id_used(const struct model *m, int64_t id) {
    for (uint32_t i = 0; i < m->failure_count; i++)
        if (m->failures[i].id == id)
            return true;
    return false;
}
static bool state_id_used(const struct model *m, int64_t id) {
    for (uint32_t i = 0; i < m->state_count; i++)
        if (m->states[i].id == id)
            return true;
    return false;
}

/* A log record the runtime could have written: a timer flag, handler, arrival and up to three
 * arguments, each its length and bytes. */
static struct blob log_record(struct input *in, uint64_t seq, uint64_t image) {
    uint8_t shape = take(in);
    struct mo_log_record_header h = {.timer = (shape & 1) != 0,
                                     .count = (uint32_t)(shape >> 1) % 4,
                                     .handler = take(in),
                                     .arrival = take_signed(in) * 1000003,
                                     .state_checksum = take(in)};
    struct mo_buffer payload;
    mo_buffer_init(&payload, MO_LIMIT_SAVED_BYTES);
    MO_ASSERT(mo_log_record_header_write(&payload, &h));
    for (uint32_t i = 0; i < h.count; i++) {
        const uint8_t *bytes;
        size_t length = take_span(in, take(in) % 64, &bytes);
        MO_ASSERT(mo_log_record_argument_write(&payload, bytes, length));
    }
    struct blob r = frame(MO_STORE_LOG, seq, image, payload.bytes, payload.length);
    mo_buffer_free(&payload);
    return r;
}

static void build(struct model *m, struct input *in) {
    uint8_t flags = take(in);
    m->failure_tables = (flags & 1) == 0;
    m->state_table_only_dropped = m->failure_tables && (flags & 0x30) == 0x30;
    m->recover = (flags & 2) != 0;
    bool empty = (flags & 4) != 0;
    uint64_t snapshot_seq = take(in);
    m->image = 0x100u + take(in);
    uint8_t pair = take(in);
    m->pair_id = pair == 0xff   ? UINT64_MAX
                 : pair == 0xfe ? (uint64_t)INT64_MAX + 1
                                : (uint64_t)(pair & 0x7f);
    if (!empty) {
        m->version = 1;
        m->head_present = m->snapshot_present = true;
        m->snapshot = fuzzed_frame(in, MO_STORE_STATE, snapshot_seq, m->image);
    } else
        snapshot_seq = 0;
    int64_t next_seq = (int64_t)snapshot_seq + 1, next_id = 1;
    bool head_set = false;
    for (unsigned ops = 0; in->length != 0 && ops < MAX_OPS; ops++) {
        uint8_t op = take(in);
        struct log_row *last_log = m->log_count ? &m->logs[m->log_count - 1] : NULL;
        struct failure_row *last_failure =
            m->failure_count ? &m->failures[m->failure_count - 1] : NULL;
        switch (op & 7) {
        case 0:
        case 1:
        case 2:
            if (m->log_count == MAX_LOG || log_seq_used(m, next_seq))
                break;
            m->logs[m->log_count++] = (struct log_row){
                next_seq, (op & 7) == 0 ? log_record(in, (uint64_t)next_seq, m->image)
                          : (op & 7) == 1
                              ? fuzzed_frame(in, MO_STORE_LOG, (uint64_t)next_seq, m->image)
                              : raw(in)};
            next_seq++;
            break;
        case 3: /* the last log row: a flipped byte, or another key */
            if (last_log == NULL)
                break;
            if ((op & 8) == 0)
                flip(&last_log->record, in);
            else {
                int64_t seq = last_log->seq + take_signed(in);
                if (!log_seq_used(m, seq))
                    last_log->seq = seq;
            }
            break;
        case 4: { /* a failure record as capture writes it, with a fuzzed kind and payloads */
            if (m->failure_count == MAX_FAILURES || failure_id_used(m, next_id) ||
                state_id_used(m, next_id))
                break;
            const uint8_t *kind;
            size_t kind_length = take_span(in, take(in) % 16, &kind);
            struct blob k = kind_length ? keep(kind, kind_length) : keep((const uint8_t *)"k", 1);
            uint8_t first = take(in);
            uint64_t seq = take(in);
            m->failures[m->failure_count++] =
                (struct failure_row){next_id, (first & 0x80) ? (int64_t)(int8_t)first : first & 1,
                                     k, fuzzed_frame(in, MO_STORE_FAILURE_MESSAGE, seq, m->image)};
            m->states[m->state_count++] = (struct state_row){
                next_id, fuzzed_frame(in, MO_STORE_FAILURE_STATE, seq, m->image)};
            next_id++;
            break;
        }
        case 5: { /* the last failure: its id, its state row, or a byte of either frame */
            if (last_failure == NULL)
                break;
            struct state_row *state = NULL;
            for (uint32_t i = 0; i < m->state_count; i++)
                if (m->states[i].id == last_failure->id)
                    state = &m->states[i];
            uint8_t what = (uint8_t)(op >> 3) % 5;
            if (what == 0) {
                int64_t id = last_failure->id + take_signed(in);
                if (!failure_id_used(m, id))
                    last_failure->id = id;
            } else if (what == 1 && state != NULL)
                *state = m->states[--m->state_count];
            else if (what == 2 && state != NULL)
                flip(&state->record, in);
            else if (what == 3)
                flip(&last_failure->message, in);
            else if (what == 4 && state != NULL) {
                int64_t id = state->id + take_signed(in);
                if (!state_id_used(m, id))
                    state->id = id;
            }
            break;
        }
        case 6: /* the snapshot: a flipped byte, a raw blob, or none */
            if ((op & 0x18) == 0 && m->snapshot_present)
                flip(&m->snapshot, in);
            else if ((op & 0x18) == 0x08) {
                m->snapshot_present = true;
                m->snapshot = raw(in);
            } else if ((op & 0x18) == 0x10)
                m->snapshot_present = false;
            break;
        case 7: /* the head or the version */
            if ((op & 8) == 0) {
                head_set = true;
                m->head_present = (op & 0x10) == 0;
                m->head = next_seq - 1 + take_signed(in);
            } else
                m->version = (int32_t)take_signed(in);
            break;
        default:
            MO_UNREACHABLE();
        }
    }
    if (!head_set && !empty)
        m->head = next_seq - 1;
}

static uint8_t template_bytes[64 << 10];
static size_t template_length;

/* The runtime's own empty store, made once per process: the reader checks its tables'
 * definitions, so the target writes rows into tables the runtime made. */
static void make_template(const char *path) {
    struct mo_store *store;
    MO_ASSERT(mo_store_open(path, &store) == MO_STORE_OK);
    mo_store_close(store);
    FILE *file = fopen(path, "rb");
    MO_ASSERT(file != NULL);
    template_length = fread(template_bytes, 1, sizeof template_bytes, file);
    MO_ASSERT(template_length != 0 && template_length < sizeof template_bytes && feof(file) &&
              fclose(file) == 0 && unlink(path) == 0);
}

static void exec_sql(sqlite3 *db, const char *sql) {
    MO_ASSERT(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
}
static void insert(sqlite3 *db, const char *sql, int64_t key, const struct blob *text,
                   int64_t number, const struct blob *bytes) {
    sqlite3_stmt *stmt;
    MO_ASSERT(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    int column = 1;
    MO_ASSERT(sqlite3_bind_int64(stmt, column++, key) == SQLITE_OK);
    if (text != NULL)
        MO_ASSERT(sqlite3_bind_text(stmt, column++, (const char *)text->bytes, (int)text->length,
                                    SQLITE_TRANSIENT) == SQLITE_OK &&
                  sqlite3_bind_int64(stmt, column++, number) == SQLITE_OK);
    /* A non-null pointer binds an empty blob rather than NULL. */
    MO_ASSERT(sqlite3_bind_blob(stmt, column, bytes->bytes, (int)bytes->length, SQLITE_TRANSIENT) ==
              SQLITE_OK);
    MO_ASSERT(sqlite3_step(stmt) == SQLITE_DONE && sqlite3_finalize(stmt) == SQLITE_OK);
}

static void write_store(const char *path, const struct model *m) {
    FILE *file = fopen(path, "wb");
    MO_ASSERT(file != NULL && fwrite(template_bytes, 1, template_length, file) == template_length &&
              fclose(file) == 0);
    sqlite3 *db;
    MO_ASSERT(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK);
    exec_sql(db, "PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF; BEGIN");
    if (!m->failure_tables)
        exec_sql(db, "DROP TABLE failure; DROP TABLE failure_state");
    else if (m->state_table_only_dropped)
        exec_sql(db, "DROP TABLE failure_state");
    char version[64];
    MO_ASSERT(snprintf(version, sizeof version, "PRAGMA user_version=%d", (int)m->version) > 0);
    exec_sql(db, version);
    if (m->head_present) {
        char head[64];
        MO_ASSERT(
            snprintf(head, sizeof head, "INSERT INTO head VALUES(1,%lld)", (long long)m->head) > 0);
        exec_sql(db, head);
    }
    if (m->snapshot_present)
        insert(db, "INSERT INTO snapshot VALUES(?1,?2)", 1, NULL, 0, &m->snapshot);
    for (uint32_t i = 0; i < m->log_count; i++)
        insert(db, "INSERT INTO log VALUES(?1,?2)", m->logs[i].seq, NULL, 0, &m->logs[i].record);
    for (uint32_t i = 0; m->failure_tables && i < m->failure_count; i++) {
        const struct failure_row *f = &m->failures[i];
        insert(db, "INSERT INTO failure VALUES(?1,?2,?3,?4)", f->id, &f->kind, f->first,
               &f->message);
    }
    for (uint32_t i = 0; m->failure_tables && !m->state_table_only_dropped && i < m->state_count;
         i++)
        insert(db, "INSERT INTO failure_state VALUES(?1,?2)", m->states[i].id, NULL, 0,
               &m->states[i].record);
    exec_sql(db, "COMMIT");
    MO_ASSERT(sqlite3_close(db) == SQLITE_OK);
}

static size_t file_bytes(const char *path, uint8_t *out) {
    FILE *file = fopen(path, "rb");
    MO_ASSERT(file != NULL);
    size_t length = fread(out, 1, FILE_BYTES, file);
    MO_ASSERT(length < FILE_BYTES && feof(file) && fclose(file) == 0);
    return length;
}

/* The model's reading of the rules each row must meet. */

static bool frame_valid(enum mo_store_record_kind kind, const struct blob *b,
                        struct mo_store_record *out) {
    return b->length != 0 && mo_store_record_read(kind, b->bytes, b->length, out);
}

/* The log payload read whole, as the runtime's replay reads it. */
static bool log_payload(const struct mo_store_record *r, struct mo_log_record_header *h,
                        uint32_t *argument_bytes) {
    struct mo_binary in = {r->bytes, r->length, 0};
    if (!mo_log_record_header_read(&in, h) || h->count > MO_LOG_RECORD_ARGUMENTS)
        return false;
    for (uint32_t i = 0; i < h->count; i++) {
        const uint8_t *bytes;
        size_t length;
        if (!mo_log_record_argument_read(&in, &bytes, &length))
            return false;
        argument_bytes[i] = (uint32_t)length;
    }
    return in.position == r->length;
}

static bool kind_valid(const struct blob *k) {
    return k->length > 0 && k->length <= MO_LIMIT_FAILURE_KIND_BYTES &&
           memchr(k->bytes, 0, k->length) == NULL && mo_utf8_valid(k->bytes, k->length);
}

/* Every failure row has a positive id and its state row, and there are at most the records a
 * store keeps. */
static bool pairs_complete(const struct model *m) {
    if (m->failure_count > MO_LIMIT_FAILURE_RECORDS || m->failure_count != m->state_count)
        return false;
    for (uint32_t i = 0; i < m->failure_count; i++)
        if (m->failures[i].id <= 0 || !state_id_used(m, m->failures[i].id))
            return false;
    return true;
}

static const struct state_row *state_of(const struct model *m, int64_t id) {
    for (uint32_t i = 0; i < m->state_count; i++)
        if (m->states[i].id == id)
            return &m->states[i];
    return NULL;
}

static int by_seq(const void *a, const void *b) {
    int64_t x = ((const struct log_row *)a)->seq, y = ((const struct log_row *)b)->seq;
    return (x > y) - (x < y);
}
static int by_id(const void *a, const void *b) {
    int64_t x = ((const struct failure_row *)a)->id, y = ((const struct failure_row *)b)->id;
    return (x > y) - (x < y);
}

/* Copied in order until the first that would pass the bound, then left. */
struct bound {
    uint64_t retained, left;
    bool full;
};
static bool fits(struct bound *b, uint64_t bytes) {
    if (!b->full && bytes <= MO_LIMIT_HISTORY_BYTES - b->retained) {
        b->retained += bytes;
        return true;
    }
    b->full = true;
    b->left += bytes;
    return false;
}

static void check_history(const struct model *m, const struct log_row *logs,
                          const struct failure_row *failures, const struct mo_store_history *h,
                          enum mo_store_folder status) {
    enum mo_store_folder expected = MO_STORE_FOLDER_OK;
    uint64_t damaged = 0;
    struct bound bound = {0};
    struct mo_store_record snapshot = {0};
    bool empty = !m->head_present;
    uint64_t head = empty || m->head < 0 ? 0 : (uint64_t)m->head;
    uint32_t entries = 0, failure_total = 0, failure_count = 0;
    if (m->state_table_only_dropped || m->version < 0 || m->version > 1 || (!empty && m->head < 0))
        expected = MO_STORE_FOLDER_DAMAGED;
    else if (empty || !m->snapshot_present) {
        if (!empty || m->snapshot_present || m->version != 0)
            expected = MO_STORE_FOLDER_DAMAGED;
    } else if (m->version != 1 || !frame_valid(MO_STORE_STATE, &m->snapshot, &snapshot) ||
               snapshot.sequence > head ||
               head - snapshot.sequence > MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES)
        expected = MO_STORE_FOLDER_DAMAGED;
    bool snapshot_copied =
        expected == MO_STORE_FOLDER_OK && m->snapshot_present && fits(&bound, snapshot.length);
    uint64_t sequence = snapshot.sequence;
    for (uint32_t i = 0; expected == MO_STORE_FOLDER_OK && i < m->log_count; i++) {
        int64_t n = logs[i].seq;
        const struct blob *b = &logs[i].record;
        if (empty || sequence >= head || n <= 0 || (uint64_t)n != sequence + 1 ||
            b->length < MO_STORE_FRAME_BYTES ||
            b->length > MO_LIMIT_SAVED_BYTES + MO_STORE_FRAME_BYTES) {
            damaged = n > 0 ? (uint64_t)n : 0;
            expected = MO_STORE_FOLDER_DAMAGED;
            break;
        }
        sequence++;
        struct mo_store_record r;
        struct mo_log_record_header lh;
        uint32_t sizes[MO_LOG_RECORD_ARGUMENTS];
        if (!frame_valid(MO_STORE_LOG, b, &r) || r.sequence != sequence ||
            r.image != snapshot.image || !log_payload(&r, &lh, sizes)) {
            damaged = sequence;
            expected = MO_STORE_FOLDER_DAMAGED;
            break;
        }
        if (!fits(&bound, sizeof(struct mo_store_history_entry) + lh.count * sizeof sizes[0]) ||
            status != MO_STORE_FOLDER_OK)
            continue;
        MO_ASSERT(entries < h->entry_count);
        const struct mo_store_history_entry *e = &h->entries[entries++];
        MO_ASSERT(e->sequence == sequence && e->arrival == lh.arrival && e->handler == lh.handler &&
                  e->timer == lh.timer && e->argument_count == lh.count);
        for (uint32_t a = 0; a < lh.count; a++)
            MO_ASSERT(e->argument_bytes[a] == sizes[a]);
    }
    if (expected == MO_STORE_FOLDER_OK && sequence != head)
        expected = MO_STORE_FOLDER_DAMAGED;
    if (expected == MO_STORE_FOLDER_OK && m->failure_tables && !pairs_complete(m))
        expected = MO_STORE_FOLDER_DAMAGED;
    for (uint32_t i = 0;
         expected == MO_STORE_FOLDER_OK && m->failure_tables && i < m->failure_count; i++) {
        const struct failure_row *f = &failures[i];
        const struct state_row *s = state_of(m, f->id);
        struct mo_store_record message;
        if (f->id <= 0 || !kind_valid(&f->kind) || (f->first != 0 && f->first != 1) ||
            !frame_valid(MO_STORE_FAILURE_MESSAGE, &f->message, &message) ||
            s->record.length < MO_STORE_FRAME_BYTES ||
            s->record.length > MO_LIMIT_SAVED_BYTES + MO_STORE_FRAME_BYTES) {
            expected = MO_STORE_FOLDER_DAMAGED;
            break;
        }
        failure_total++;
        if (!fits(&bound,
                  sizeof(struct mo_store_history_failure) + f->kind.length + 1 + message.length) ||
            status != MO_STORE_FOLDER_OK)
            continue;
        MO_ASSERT(failure_count < h->failure_count);
        const struct mo_store_history_failure *x = &h->failures[failure_count++];
        struct mo_store_record state;
        bool state_valid = frame_valid(MO_STORE_FAILURE_STATE, &s->record, &state) &&
                           state.sequence == message.sequence && state.image == message.image;
        MO_ASSERT(
            x->id == (uint64_t)f->id && x->first == (f->first == 1) &&
            x->state_valid == state_valid && strlen(x->kind) == f->kind.length &&
            memcmp(x->kind, f->kind.bytes, f->kind.length) == 0 &&
            x->state_bytes == s->record.length - MO_STORE_FRAME_BYTES &&
            x->message.kind == MO_STORE_FAILURE_MESSAGE &&
            x->message.sequence == message.sequence && x->message.image == message.image &&
            x->message.length == message.length &&
            (message.length == 0 || memcmp(x->message.bytes, message.bytes, message.length) == 0));
    }
    MO_ASSERT(status == expected);
    MO_ASSERT(h->damaged_sequence == damaged && !h->recovered);
    if (status != MO_STORE_FOLDER_OK) {
        MO_ASSERT(h->entry_count == 0 && h->failure_count == 0 && h->retained_bytes == 0);
        return;
    }
    MO_ASSERT(h->failure_tables == m->failure_tables && h->has_snapshot == m->snapshot_present &&
              h->head == head && h->log_count == m->log_count && h->entry_count == entries &&
              h->failure_total == failure_total && h->failure_count == failure_count &&
              h->retained_bytes == bound.retained && h->left_bytes == bound.left &&
              h->retained_bytes <= MO_LIMIT_HISTORY_BYTES);
    MO_ASSERT((h->snapshot.bytes != NULL) == snapshot_copied);
    if (m->snapshot_present)
        MO_ASSERT(h->snapshot.kind == MO_STORE_STATE && h->snapshot.sequence == snapshot.sequence &&
                  h->snapshot.image == snapshot.image && h->snapshot.length == snapshot.length);
    if (snapshot_copied && snapshot.length != 0)
        MO_ASSERT(memcmp(h->snapshot.bytes, snapshot.bytes, snapshot.length) == 0);
}

static void check_pair(const struct model *m, const struct failure_row *failures,
                       const struct mo_store_history_pair *p, enum mo_store_folder status) {
    enum mo_store_folder expected = MO_STORE_FOLDER_OK;
    const struct failure_row *f = NULL;
    struct mo_store_record state, message;
    bool lookup = m->pair_id != 0 && m->pair_id <= INT64_MAX;
    if (m->state_table_only_dropped)
        expected = MO_STORE_FOLDER_DAMAGED;
    else if (m->failure_tables && lookup) {
        for (uint32_t i = 0; i < m->failure_count; i++)
            if ((uint64_t)failures[i].id == m->pair_id)
                f = &failures[i];
        if (!pairs_complete(m))
            expected = MO_STORE_FOLDER_DAMAGED;
        else if (f != NULL &&
                 (!kind_valid(&f->kind) || (f->first != 0 && f->first != 1) ||
                  !frame_valid(MO_STORE_FAILURE_STATE, &state_of(m, f->id)->record, &state) ||
                  !frame_valid(MO_STORE_FAILURE_MESSAGE, &f->message, &message) ||
                  state.sequence != message.sequence || state.image != message.image))
            expected = MO_STORE_FOLDER_DAMAGED;
    }
    /* Without the record, the ids the folder holds, each positive and at most the limit. */
    if (expected == MO_STORE_FOLDER_OK && m->failure_tables && f == NULL) {
        bool listed = m->failure_count <= MO_LIMIT_FAILURE_RECORDS;
        for (uint32_t i = 0; listed && i < m->failure_count; i++)
            listed = failures[i].id > 0;
        if (!listed)
            expected = MO_STORE_FOLDER_DAMAGED;
    }
    MO_ASSERT(status == expected && p->id == m->pair_id && !p->recovered);
    if (status != MO_STORE_FOLDER_OK) {
        MO_ASSERT(!p->found && p->retained_count == 0);
        return;
    }
    MO_ASSERT(p->failure_tables == m->failure_tables && p->found == (f != NULL));
    if (f != NULL) {
        MO_ASSERT(strlen(p->kind) == f->kind.length &&
                  memcmp(p->kind, f->kind.bytes, f->kind.length) == 0 &&
                  p->first == (f->first == 1) && p->retained_count == 0);
        MO_ASSERT(p->state.kind == MO_STORE_FAILURE_STATE && p->state.length == state.length &&
                  p->state.sequence == state.sequence && p->state.image == state.image &&
                  (state.length == 0 || memcmp(p->state.bytes, state.bytes, state.length) == 0));
        MO_ASSERT(
            p->message.kind == MO_STORE_FAILURE_MESSAGE && p->message.length == message.length &&
            p->message.sequence == message.sequence && p->message.image == message.image &&
            (message.length == 0 || memcmp(p->message.bytes, message.bytes, message.length) == 0));
        return;
    }
    MO_ASSERT(p->retained_count == (m->failure_tables ? m->failure_count : 0));
    for (uint32_t i = 0; i < p->retained_count; i++)
        MO_ASSERT(p->retained[i] == (uint64_t)failures[i].id);
}

static struct model model;
static struct log_row sorted_logs[MAX_LOG];
static struct failure_row sorted_failures[MAX_FAILURES];
static uint8_t before[FILE_BYTES], after[FILE_BYTES];

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > INPUT_BYTES)
        return 0;
    char directory[] = "/tmp/mo-history-fuzz-XXXXXX";
    MO_ASSERT(mkdtemp(directory) != NULL);
    char path[sizeof directory + 16];
    MO_ASSERT(snprintf(path, sizeof path, "%s/service.sqlite", directory) > 0);
    if (template_length == 0)
        make_template(path);
    pool_used = 0;
    model = (struct model){0};
    struct input in = {data, size};
    build(&model, &in);
    write_store(path, &model);
    size_t length = file_bytes(path, before);

    memcpy(sorted_logs, model.logs, model.log_count * sizeof sorted_logs[0]);
    qsort(sorted_logs, model.log_count, sizeof sorted_logs[0], by_seq);
    memcpy(sorted_failures, model.failures, model.failure_count * sizeof sorted_failures[0]);
    qsort(sorted_failures, model.failure_count, sizeof sorted_failures[0], by_id);

    struct mo_store_history h;
    enum mo_store_folder status = mo_store_history_read(path, model.recover, &h);
    check_history(&model, sorted_logs, sorted_failures, &h, status);
    if (status == MO_STORE_FOLDER_OK)
        mo_store_history_free(&h);

    struct mo_store_history_pair p;
    status = mo_store_history_pair(path, model.recover, model.pair_id, &p);
    check_pair(&model, sorted_failures, &p, status);
    if (status == MO_STORE_FOLDER_OK)
        mo_store_history_pair_free(&p);

    MO_ASSERT(file_bytes(path, after) == length && memcmp(before, after, length) == 0);
    MO_ASSERT(unlink(path) == 0 && rmdir(directory) == 0);
    return 0;
}
