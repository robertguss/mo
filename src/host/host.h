/* Capabilities own heap storage; the caller owns their execution context. */
#ifndef MO_HOST_H
#define MO_HOST_H

#include "base/hostops.h"
#include "heap/heap.h"

#define MO_HOST_PRIVATE_TYPES 6
struct mo_host_types {
    struct mo_types types;
    uint32_t integer, time, text, texts, folders, files;
    uint32_t capabilities[MO_CAP_RANDOM + 1];
    uint32_t parameters[2];
};
/* Once per image. storage has source->count + MO_HOST_PRIVATE_TYPES entries;
 * it and out must remain at these addresses for the lifetime of every heap.
 * Source indices and layouts are preserved. The VM charges this storage to
 * its initialization budget. The resulting table is read-only. */
void mo_host_prepare_types(struct mo_host_types *out, const struct mo_types *source,
                           struct mo_type *storage);

struct mo_host {
    const struct mo_host_types *types;
    uint32_t open_folders;
    int input, output, errors;
    bool force_fallback; /* C tests exercise the portable resolver on Linux. */
    bool started, kernel_probed, kernel_available;
};
void mo_host_init(struct mo_host *host, const struct mo_host_types *types);
/* Before running main, once per context and before starting threads. Changes
 * process-wide SIGPIPE to ignored and tries to raise the soft descriptor budget to
 * MO_LIMIT_OPEN_FOLDERS + 32 (never above its hard limit). C embedders calling
 * real Console directly must call this, or arrange SIGPIPE handling themselves.
 * A refused raise leaves the current budget; later opens report exhaustion.
 * NULL on success, otherwise a static internal-stop reason. Prints never change
 * signal dispositions. Embedders may manage their descriptor budget themselves. */
MO_MUST_USE const char *mo_host_start(struct mo_host *host);

enum mo_host_outcome { MO_HOST_RETURNED, MO_HOST_OUT_OF_MEMORY, MO_HOST_BUG, MO_HOST_INTERNAL };
struct mo_host_error {
    const char *reason;
    bool has_detail;
    struct mo_cell detail; /* borrowed; retained arguments outlive the report */
};
/* Verified arguments. Only success writes an owned result and consumes the
 * owned arguments of hostops.def. Refusal preserves cells and ownership.
 * External I/O is not rolled back, including partial writes or folder creation.
 * The host context outlives its capabilities, including heap teardown. */
MO_MUST_USE enum mo_host_outcome mo_host_call(struct mo_host *host, struct mo_heap *heap,
                                              uint32_t operation, uint32_t result_type,
                                              const struct mo_cell *arguments, struct mo_cell *out,
                                              struct mo_host_error *error);
/* Runtime construction for main. contents is List(Text) for Args, Map(Text,
 * Text) for Env, otherwise NULL. folder is an open borrowed descriptor for
 * Files/ReadFiles. Success duplicates contents/descriptors. */
MO_MUST_USE enum mo_host_outcome mo_host_real(struct mo_host *host, struct mo_heap *heap,
                                              uint32_t type, const struct mo_cell *contents,
                                              int folder, struct mo_cell *out,
                                              struct mo_host_error *error);

/* Semantic backing state for C fixtures/embedders, independent of heap slots.
 * Collections are List(Text), Map(Text,Text), or Set(Text) from this heap.
 * Files and ReadFiles use the same family. Random stores four nonzero-together
 * generator words, including advanced states, not a seed. */
struct mo_host_fake_state {
    enum mo_capability kind;
    union {
        struct mo_cell args, env;
        struct {
            struct mo_cell input, output, errors;
        } console;
        struct {
            struct mo_cell contents, folders;
        } files;
        int64_t time;
        uint64_t random[4];
    } as;
};
/* Restore an existing well-formed fake, not a fresh Mo constructor: explicit
 * empty folders and output histories are retained. Borrows every input; only
 * success writes an owned root capability of the matching declared kind.
 * Derive narrowed/read-only aliases through the ordinary host operations. */
MO_MUST_USE enum mo_host_outcome mo_host_fake_restore(struct mo_host *host, struct mo_heap *heap,
                                                      uint32_t type,
                                                      const struct mo_host_fake_state *initial,
                                                      struct mo_cell *out,
                                                      struct mo_host_error *error);
/* Snapshot a fake's backing store (not just its narrowed view). Collections in
 * the successful result are owned immutable snapshots, even after mutations;
 * only success writes out. Dispose them before freeing the heap. */
MO_MUST_USE bool mo_host_fake_snapshot(struct mo_host *host, struct mo_heap *heap,
                                       struct mo_cell capability, struct mo_host_fake_state *out);
void mo_host_fake_state_drop(struct mo_host *host, struct mo_heap *heap,
                             struct mo_host_fake_state *state);

/* Runtime-owned capability; never handed to a Mo handler. A NULL path makes
 * the in-memory fake, retained by the embedding across simulated restarts.
 * A disk path names one service's database, locked exclusively until close.
 * Each handle belongs to at most one live service. Only success writes out. */
/* Candidate compiler subprocess. Paths and executable are absolute; tools owns a private
 * bundle directory and validates the emitted image. Poll never blocks unless it must reap
 * a child it has just killed. The child shares the service's OS identity, not a sandbox. */
struct mo_compile_child {
    int pid;
    uint64_t deadline;
};
enum mo_compile_status {
    MO_COMPILE_RUNNING,
    MO_COMPILE_OK,
    MO_COMPILE_DIAGNOSTIC,
    /* Compiler child runtime failures retain their ordinary startup meaning. */
    MO_COMPILE_INITIALIZER, /* exit 70 */
    MO_COMPILE_SERVICE,     /* exit 69 */
    MO_COMPILE_BUDGET,
    MO_COMPILE_CRASH,
    MO_COMPILE_IO
};
MO_MUST_USE enum mo_compile_status mo_compile_start(const char *executable, const char *entry,
                                                    const char *output, const char *diagnostics,
                                                    const char *old_bundle, const char *migration,
                                                    const char *context, uint64_t now,
                                                    struct mo_compile_child *child);
MO_MUST_USE enum mo_compile_status mo_compile_poll(struct mo_compile_child *child, uint64_t now);
void mo_compile_cancel(struct mo_compile_child *child);

struct mo_store;
enum mo_store_status {
    MO_STORE_OK,
    MO_STORE_IO,
    MO_STORE_CORRUPT,
    MO_STORE_MEMORY,
    MO_STORE_EMPTY_PATH,
    MO_STORE_MEMORY_PATH,
    MO_STORE_HANDLES,
    MO_STORE_BUSY
};
MO_MUST_USE const char *mo_store_status_text(enum mo_store_status status);
enum mo_store_operation {
#define HOSTOP(id, module, name, signature, params, bugs, spec_only)
#define STOREOP(id) MO_STORE_##id,
#include "base/hostops.def"
};
enum mo_store_record_kind {
    MO_STORE_LOG = 1,
    MO_STORE_STATE = 2,
    MO_STORE_FAILURE_STATE = 3,
    MO_STORE_FAILURE_MESSAGE = 4
};
/* A payload holds at most MO_LIMIT_SAVED_BYTES; its frame adds this envelope. */
#define MO_STORE_FRAME_BYTES 34
struct mo_store_record {
    enum mo_store_record_kind kind;
    uint64_t sequence, image;
    const uint8_t *bytes;
    size_t length;
};
/* A pure bounded frame reader, shared by disk loading and fuzzing. The payload
 * view borrows input; its values still require heap decoding before use. */
MO_MUST_USE bool mo_store_record_read(enum mo_store_record_kind kind, const uint8_t *bytes,
                                      size_t length, struct mo_store_record *out);
MO_MUST_USE enum mo_store_status mo_store_open(const char *path, struct mo_store **out);
/* The same, but a store another connection holds is waited for, up to
 * MO_LIMIT_SERVE_STORE_WAIT_US, before the open returns BUSY. Without the wait a
 * held store is BUSY at once. Only taking the lock waits; nothing after it does. */
MO_MUST_USE enum mo_store_status mo_store_open_wait(const char *path, struct mo_store **out);
void mo_store_close(struct mo_store *store);
/* LOAD streams a snapshot then consecutive log records in one read transaction.
 * The callback stages results privately; publish only after LOAD succeeds.
 * No callback means an empty store. APPEND requires sequence = head + 1.
 * SNAPSHOT requires sequence = head and prunes the covered log atomically.
 * Initial SNAPSHOT is sequence zero. Every successful write is synced before
 * returning. A failed write has an uncertain commit outcome: close/recover. */
MO_MUST_USE enum mo_store_status
mo_store_call(struct mo_store *store, enum mo_store_operation operation,
              const struct mo_store_record *record,
              bool (*consume)(const struct mo_store_record *record, void *context), void *context);

/* The active canonical source and its runtime identities. Bundles borrow caller storage. */
struct mo_store_program {
    uint8_t source[32], build[32], schema[32], context_digest[32];
    uint64_t image, ordinal;
    const uint8_t *bundle;
    size_t bundle_size;
    const uint8_t *context;
    size_t context_size;
};
struct mo_store_activation {
    struct mo_store_program previous, candidate;
    struct mo_store_record previous_state, candidate_state;
    const char *migration, *report_json;
    size_t migration_size, report_size;
};
enum mo_store_activation_status {
    MO_STORE_ACTIVATED,
    MO_STORE_ACTIVATION_REFUSED,
    MO_STORE_ACTIVATION_UNSAFE
};
/* Copies the active bundle into arena; absent means a legacy/empty folder. Refusal
 * preserves out and found. The exclusive store lock must already be held. */
MO_MUST_USE enum mo_store_status mo_store_program_read(struct mo_store *store,
                                                       struct mo_arena *arena,
                                                       struct mo_store_program *out, bool *found);
/* Record/adopt the initial program, atomically with its authoritative snapshot. An
 * existing program may only be refreshed with the same source and ordinal after verified
 * replay. A failure is uncertain and requires close/recovery. */
MO_MUST_USE enum mo_store_status mo_store_program_record(struct mo_store *store,
                                                         const struct mo_store_program *program,
                                                         const struct mo_store_record *snapshot);
MO_MUST_USE enum mo_store_activation_status
mo_store_activate(struct mo_store *store, const struct mo_store_activation *activation);
/* Verified replay adoption retains previous_state verbatim, which must equal the
 * stored snapshot; candidate_state is at the current head. Legacy adoption has no
 * previous program, uses zero old source (unavailable), and installs ordinal 1.
 * Recorded rebuilds retain the previous program and increment its ordinal. */
MO_MUST_USE enum mo_store_activation_status
mo_store_adopt(struct mo_store *store, const struct mo_store_activation *activation, bool legacy);

/* Diagnostic writes publish an id only after commit. REFUSED means rollback and
 * a subsequent recovery-head read succeeded; DAMAGED is REFUSED because stored
 * diagnostic rows failed validation; UNSAFE requires closing the store. */
enum mo_store_capture_status {
    MO_STORE_CAPTURE_OK,
    MO_STORE_CAPTURE_REFUSED,
    MO_STORE_CAPTURE_DAMAGED,
    MO_STORE_CAPTURE_UNSAFE
};
enum mo_store_failure_operation {
#define HOSTOP(id, module, name, signature, params, bugs, spec_only)
#define FAILUREOP(id, function) MO_STORE_FAILURE_##id,
#include "base/hostops.def"
};
struct mo_store_failure {
    uint64_t id;
    const char *kind;
    bool first;
    struct mo_store_record state, message;
};
MO_MUST_USE enum mo_store_capture_status mo_store_capture(struct mo_store *store, const char *kind,
                                                          const struct mo_store_record *state,
                                                          const struct mo_store_record *message,
                                                          uint64_t *id);
MO_MUST_USE enum mo_store_status
mo_store_failures(struct mo_store *store, bool (*consume)(const struct mo_store_failure *, void *),
                  void *context);

/* Catalog visits at most VIEW_RECORDS messages after the cursor, without reading state
 * blobs. The state field supplies length only. Get verifies exactly the selected pair. */
MO_MUST_USE enum mo_store_status
mo_store_failure_catalog(struct mo_store *store, uint64_t after,
                         bool (*consume)(const struct mo_store_failure *, void *), void *context,
                         uint32_t *omitted);
MO_MUST_USE enum mo_store_status
mo_store_failure_get(struct mo_store *store, uint64_t id,
                     bool (*consume)(const struct mo_store_failure *, void *), void *context);

MO_MUST_USE bool mo_store_failure_export(struct mo_store *store, uint64_t id,
                                         struct mo_buffer *out);

MO_MUST_USE enum mo_store_status mo_store_failure_stats(struct mo_store *store, uint32_t *count,
                                                        uint64_t *evicted);

/* Reading a stopped service's store without starting the service. The file is opened
 * read-only and never created, no lock is waited for, and everything is copied in one read
 * transaction whose lock ends before the call returns. The status is classified by SQLite's
 * result code, never by which files lie beside the database. With recover, a store whose
 * journal needs a rollback is opened read-write, still never created, so SQLite rolls it
 * back: that writes database pages, though no record changes. */
enum mo_store_folder {
    MO_STORE_FOLDER_OK,
    MO_STORE_FOLDER_MISSING,    /* no file, so no saved data */
    MO_STORE_FOLDER_OWNED,      /* another process, such as a running service, holds it */
    MO_STORE_FOLDER_RECOVERY,   /* an unfinished write left a journal to roll back */
    MO_STORE_FOLDER_UNREADABLE, /* the user may not read it */
    MO_STORE_FOLDER_DAMAGED,    /* not a service store, or a record failed its check */
    MO_STORE_FOLDER_IO,
    MO_STORE_FOLDER_MEMORY,
    MO_STORE_FOLDER_HANDLES
};
/* Read only the active program from a stopped store file, without creating it or
 * recovering a journal. Bundle/context are validated and copied into arena.
 * Only OK publishes out/found; a missing program is OK with found=false. */
MO_MUST_USE enum mo_store_folder mo_store_program_read_file(const char *path,
                                                            struct mo_arena *arena,
                                                            struct mo_store_program *out,
                                                            bool *found);
/* A log record's metadata: argument_bytes has argument_count lengths. The handler is an
 * index; the log does not record its name. */
struct mo_store_history_entry {
    uint64_t sequence;
    int64_t arrival;
    uint32_t handler, argument_count;
    bool timer;
    const uint32_t *argument_bytes;
};
/* A failure record without its state: the message's frame fields and copied payload.
 * state_valid is that row's MOST header and checksum, checked without retaining the blob. */
struct mo_store_history_failure {
    uint64_t id;
    const char *kind;
    bool first, state_valid;
    size_t state_bytes;
    struct mo_store_record message;
};
/* What was copied is bounded by MO_LIMIT_HISTORY_BYTES, in this order: the snapshot, the log
 * entries, the failures. Past the bound nothing more is copied, and left_bytes counts what
 * was not; every log row is still checked. A snapshot that was not copied has NULL bytes. */
struct mo_store_history_upgrade {
    struct mo_store_history_upgrade *next;
    uint64_t ordinal, head;
    uint8_t old_source[32], new_source[32];
    bool program_retained;
};
struct mo_store_history_program {
    struct mo_store_history_program *next;
    struct mo_store_program program; /* identities only; bundle/context pointers are NULL */
};
struct mo_store_history {
    struct mo_arena arena;
    bool has_program;
    struct mo_store_program program;
    struct mo_store_history_program *previous_programs;
    struct mo_store_history_upgrade *upgrades;
    uint64_t upgrade_count;
    bool upgrades_truncated;
    bool recovered;      /* opened read-write and rolled a journal back */
    bool failure_tables; /* false for a store from before failure capture */
    bool has_snapshot;   /* false only for a store a first start never finished */
    struct mo_store_record snapshot;
    uint64_t head;             /* the last committed sequence */
    uint64_t damaged_sequence; /* the log row that failed its check, or 0 */
    const struct mo_store_history_entry *entries;
    uint32_t entry_count, log_count;
    const struct mo_store_history_failure *failures;
    uint32_t failure_count, failure_total;
    uint64_t retained_bytes, left_bytes;
};
/* Only OK leaves out to free; on another status out holds only recovered and
 * damaged_sequence. */
MO_MUST_USE enum mo_store_folder mo_store_history_read(const char *path, bool recover,
                                                       struct mo_store_history *out);
void mo_store_history_free(struct mo_store_history *history);

/* One failure record's state and message for replay, both checked and copied. When the
 * folder holds no record with that id, found is false and retained lists the ids it holds:
 * ids are never reused within a folder, so a gap is a replaced or evicted record. */
struct mo_store_history_pair {
    struct mo_arena arena;
    bool recovered, failure_tables, found;
    uint64_t id;
    const char *kind;
    bool first;
    struct mo_store_record state, message;
    uint64_t retained[MO_LIMIT_FAILURE_RECORDS];
    uint32_t retained_count;
};
MO_MUST_USE enum mo_store_folder mo_store_history_pair(const char *path, bool recover, uint64_t id,
                                                       struct mo_store_history_pair *out);
void mo_store_history_pair_free(struct mo_store_history_pair *pair);

struct mo_bearer {
    uint8_t bytes[MO_LIMIT_MCP_TOKEN_BYTES];
    size_t length;
};
MO_MUST_USE bool mo_bearer_read(struct mo_bearer *out, const char *environment);
MO_MUST_USE bool mo_bearer_check(const struct mo_bearer *bearer, const uint8_t *bytes,
                                 size_t length);
void mo_host_wipe(void *bytes, size_t length);

struct mo_listener;
enum mo_listener_status {
    MO_LISTENER_OK,
    MO_LISTENER_AGAIN,
    MO_LISTENER_EOF,
    MO_LISTENER_IO,
    MO_LISTENER_MEMORY,
    MO_LISTENER_FULL,
    MO_LISTENER_STALE,
    MO_LISTENER_CONFIG,
    MO_LISTENER_CREDENTIAL
};
enum mo_listener_operation {
#define HOSTOP(id, module, name, signature, params, bugs, spec_only)
#define LISTENEROP(id) MO_LISTENER_##id,
#include "base/hostops.def"
};
struct mo_listener_config {
    const char *address;
    uint16_t port;
    bool expose;
};
struct mo_listener_key {
    uint64_t cookie, generation;
    size_t slot;
};
struct mo_listener_time {
    uint64_t monotonic_us;
    int64_t wall_us;
};
enum mo_listener_event {
    MO_LISTENER_READABLE = 1,
    MO_LISTENER_WRITABLE = 2,
    MO_LISTENER_ERROR = 4
};
struct mo_listener_interest {
    struct mo_listener_key connection;
    unsigned events;
};
struct mo_listener_wait {
    uint64_t until_us;
    bool accept;
    size_t count;
    struct mo_listener_interest interests[MO_LIMIT_MCP_CONNECTIONS];
};
struct mo_listener_read {
    struct mo_listener_key connection;
    uint8_t *bytes;
    size_t capacity;
};
struct mo_listener_write {
    struct mo_listener_key connection;
    const uint8_t *bytes;
    size_t size;
};
struct mo_listener_auth {
    const uint8_t *bytes;
    size_t size, start, length;
};
union mo_listener_request {
    struct mo_listener_wait wait;
    struct mo_listener_read read;
    struct mo_listener_write write;
    struct mo_listener_auth auth;
    struct mo_listener_key connection;
};
struct mo_listener_ready {
    struct mo_listener_time now;
    bool accept;
    unsigned events[MO_LIMIT_MCP_CONNECTIONS];
};
union mo_listener_result {
    struct mo_listener_time now;
    struct mo_listener_ready wait;
    struct mo_listener_key accepted;
    size_t transferred;
    bool authenticated;
};
struct mo_listener_fake_peer {
    const uint8_t *input;
    size_t input_size, input_offset;
    uint8_t *output;
    size_t output_capacity, output_size;
    size_t read_quantum, write_quantum;
    bool readable, writable, eof, read_error, write_error;
    bool closed, half_closed;
};
MO_MUST_USE const char *mo_listener_status_text(enum mo_listener_status status);
MO_MUST_USE enum mo_listener_status mo_listener_open(const struct mo_listener_config *config,
                                                     const char *environment,
                                                     struct mo_listener **out);
MO_MUST_USE enum mo_listener_status mo_listener_fake(const char *environment,
                                                     struct mo_listener **out);
void mo_listener_close(struct mo_listener *listener);
void mo_listener_stop_accepting(struct mo_listener *listener);
MO_MUST_USE uint16_t mo_listener_port(const struct mo_listener *listener);
MO_MUST_USE enum mo_listener_status mo_listener_call(struct mo_listener *listener,
                                                     enum mo_listener_operation operation,
                                                     const union mo_listener_request *request,
                                                     union mo_listener_result *out);
MO_MUST_USE enum mo_listener_status mo_listener_fake_connect(struct mo_listener *listener,
                                                             struct mo_listener_fake_peer *peer);
MO_MUST_USE enum mo_listener_status mo_listener_fake_advance(struct mo_listener *listener,
                                                             uint64_t microseconds);
MO_MUST_USE enum mo_listener_status mo_listener_fake_last_wait(const struct mo_listener *listener,
                                                               uint64_t *until_us);

#endif
