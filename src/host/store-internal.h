#ifndef MO_STORE_INTERNAL_H
#define MO_STORE_INTERNAL_H

#include <sqlite3.h>

#include "host/host.h"

#define TEXT_OF(n) DIGITS_OF(n)
#define DIGITS_OF(n) #n

struct mo_store {
    struct mo_arena arena;
    sqlite3 *db;
    bool unsafe;
    uint64_t evicted_kinds;
};

/* "MOST\1", kind, sequence, image and payload length; then payload and checksum. */
enum { FRAME_HEADER_BYTES = 5 + 1 + 8 + 8 + 4, FRAME_CHECKSUM_BYTES = 8 };

bool store_exec(struct mo_store *s, const char *sql);
bool store_frame(const struct mo_store_record *r, struct mo_buffer *out);
enum mo_store_status store_sqlite_status(int code);
enum mo_store_status store_finish(sqlite3_stmt *stmt, enum mo_store_status result);
enum mo_store_status store_row(sqlite3_stmt *stmt, int column, enum mo_store_record_kind kind,
                               struct mo_store_record *out);
enum mo_store_status store_current_head(struct mo_store *s, uint64_t *head, bool *empty);
int store_write_blob_code(struct mo_store *s, const char *sql, uint64_t seq,
                          const struct mo_buffer *bytes);
enum mo_store_status store_failure_read(struct mo_store *s, uint64_t after, uint64_t id,
                                        bool metadata,
                                        bool (*consume)(const struct mo_store_failure *, void *),
                                        void *context);

/* The head and the snapshot, checked against the version and each other. */
struct saved_head {
    uint64_t head, sequence, image;
    bool empty;
};

typedef enum mo_store_folder (*folder_body)(struct mo_store *s, bool failure_tables, void *context);

enum mo_store_status store_version(struct mo_store *s, int *version);
enum mo_store_status store_load_snapshot(struct mo_store *s, struct saved_head *h,
                                         bool (*consume)(const struct mo_store_record *, void *),
                                         void *context);
bool store_next_log_row(const struct saved_head *h, sqlite3_stmt *stmt, uint64_t sequence);
enum mo_store_folder store_folder_status(enum mo_store_status status);
enum mo_store_folder store_read_folder(const char *path, bool recover, bool *recovered,
                                       folder_body body, void *context);
bool store_blob_size(sqlite3_stmt *stmt, int column, int length);
bool store_program_row(sqlite3_stmt *stmt, struct mo_store_program *p);
enum mo_store_status store_read_program(struct mo_store *s, struct mo_arena *arena,
                                        struct mo_store_program *out, bool *found);
enum mo_store_status store_read_setting(struct mo_store *s, struct mo_store_review *out);

struct store_table {
    const char *name, *sql;
};

bool store_table_ready(struct mo_store *s, const struct store_table *t);
bool store_items_valid(const struct mo_store_program *p);
bool store_decide(struct mo_store *s, int64_t time, const char *kind, const uint8_t base[32],
                  const char *setting, const char *detail);
bool store_review_write(struct mo_store *s, const struct mo_store_program *p);
bool store_not_version_two(struct mo_store *s);

#endif
