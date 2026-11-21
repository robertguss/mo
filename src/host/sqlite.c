#define _POSIX_C_SOURCE 200809L
#include "host/sqlite.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/utf8.h"
#include "host/files.h"

struct mo_sqlite_root {
    struct mo_host *owner;
    int descriptor;
};

struct fake_sqlite_entry {
    char *path;
    sqlite3 *db;
    uint8_t *raw;
    size_t raw_len;
    struct fake_sqlite_entry *next;
};

struct fake_sqlite_hub {
    size_t refs;
    struct fake_sqlite_entry *head;
};

struct mo_sqlite_root_fake {
    struct fake_sqlite_hub *hub;
};

struct mo_sqlite_db {
    struct mo_host *owner;
    sqlite3 *db;
    struct fake_sqlite_hub *hub;
};

struct sqlite_auth {
    bool in_step;
    bool vacuum_into;
    const char *denied_reason;
};

static uint32_t find_case_field_type(const struct mo_host *host, uint32_t choice_type,
                                     const char *case_name, uint32_t field_index) {
    const struct mo_types *types = &host->types->types;
    MO_ASSERT(choice_type < types->count);
    const struct mo_type *t = &types->items[choice_type];
    MO_ASSERT(t->kind == MO_TYPE_CHOICE);
    for (uint32_t i = 0; i < t->case_count; i++) {
        if (strcmp(t->cases[i].name, case_name) == 0) {
            MO_ASSERT(field_index < t->cases[i].field_count);
            return t->cases[i].fields[field_index].type;
        }
    }
    MO_UNREACHABLE();
}

static uint32_t find_choice_by_name(const struct mo_host *host, const char *name) {
    const struct mo_types *types = &host->types->types;
    for (uint32_t i = 0; i < types->count; i++) {
        if (types->items[i].kind == MO_TYPE_CHOICE && types->items[i].name != NULL &&
            strcmp(types->items[i].name, name) == 0) {
            return i;
        }
    }
    return MO_NO_INDEX;
}

static uint32_t find_record_field(const struct mo_types *types, uint32_t record_type,
                                  const char *name) {
    MO_ASSERT(record_type < types->count);
    const struct mo_type *t = &types->items[record_type];
    MO_ASSERT(t->kind == MO_TYPE_RECORD);
    for (uint32_t i = 0; i < t->field_count; i++) {
        if (strcmp(t->fields[i].name, name) == 0)
            return i;
    }
    MO_UNREACHABLE();
}

static void hub_retain(struct fake_sqlite_hub *hub) {
    MO_ASSERT(hub != NULL && hub->refs > 0);
    hub->refs++;
}

static void hub_release(struct fake_sqlite_hub *hub) {
    if (hub == NULL)
        return;
    MO_ASSERT(hub->refs > 0);
    hub->refs--;
    if (hub->refs != 0)
        return;
    struct fake_sqlite_entry *e = hub->head;
    while (e != NULL) {
        struct fake_sqlite_entry *next = e->next;
        if (e->db != NULL)
            (void)sqlite3_close(e->db);
        sqlite3_free(e->raw);
        sqlite3_free(e->path);
        sqlite3_free(e);
        e = next;
    }
    sqlite3_free(hub);
}

static void release_sqlite_root(intptr_t handle) {
    struct mo_sqlite_root *root = (struct mo_sqlite_root *)handle;
    mo_host_close(root->descriptor);
    MO_ASSERT(root->owner->open_folders != 0);
    root->owner->open_folders--;
}

static void release_sqlite_root_fake(intptr_t handle) {
    struct mo_sqlite_root_fake *root = (struct mo_sqlite_root_fake *)handle;
    hub_release(root->hub);
}

static void release_sqlite_db(intptr_t handle) {
    struct mo_sqlite_db *d = (struct mo_sqlite_db *)handle;
    if (d->hub != NULL) {
        hub_release(d->hub);
    } else if (d->db != NULL) {
        (void)sqlite3_close(d->db);
    }
    MO_ASSERT(d->owner->open_databases != 0);
    d->owner->open_databases--;
}

static MO_MUST_USE enum mo_host_outcome
error_outside_folder(struct mo_host *host, struct mo_heap *heap, uint32_t result_type,
                     struct mo_cell path_cell, struct mo_cell *out) {
    uint32_t err_type = find_case_field_type(host, result_type, "Error", 0);
    struct mo_cell err_val;
    if (!mo_host_case(host, heap, err_type, "OutsideFolder", &path_cell, 1, &err_val))
        return MO_HOST_OUT_OF_MEMORY;
    bool wrapped = mo_host_case(host, heap, result_type, "Error", &err_val, 1, out);
    mo_value_drop(heap, err_type, err_val);
    return wrapped ? MO_HOST_RETURNED : MO_HOST_OUT_OF_MEMORY;
}

static MO_MUST_USE enum mo_host_outcome error_text_msg(struct mo_host *host, struct mo_heap *heap,
                                                       uint32_t result_type, const char *case_name,
                                                       const char *msg, const char *fallback,
                                                       struct mo_cell *out) {
    if (msg == NULL || msg[0] == '\0')
        msg = fallback;
    size_t len = strlen(msg);
    if (!mo_utf8_valid((const uint8_t *)msg, len)) {
        msg = fallback;
        len = strlen(msg);
    }
    uint32_t err_type = find_case_field_type(host, result_type, "Error", 0);
    struct mo_cell text_val;
    if (!mo_value_text(heap, msg, len, &text_val))
        return MO_HOST_OUT_OF_MEMORY;
    struct mo_cell err_val;
    bool built = mo_host_case(host, heap, err_type, case_name, &text_val, 1, &err_val);
    mo_value_drop(heap, host->types->text, text_val);
    if (!built)
        return MO_HOST_OUT_OF_MEMORY;
    bool wrapped = mo_host_case(host, heap, result_type, "Error", &err_val, 1, out);
    mo_value_drop(heap, err_type, err_val);
    return wrapped ? MO_HOST_RETURNED : MO_HOST_OUT_OF_MEMORY;
}

static MO_MUST_USE enum mo_host_outcome
error_not_permitted(struct mo_host *host, struct mo_heap *heap, uint32_t result_type,
                    const char *reason, struct mo_cell *out) {
    return error_text_msg(host, heap, result_type, "NotPermitted", reason,
                          "statement is not permitted", out);
}

static MO_MUST_USE enum mo_host_outcome error_failed(struct mo_host *host, struct mo_heap *heap,
                                                     uint32_t result_type, const char *reason,
                                                     struct mo_cell *out) {
    return error_text_msg(host, heap, result_type, "SqlFailed", reason, "sqlite error", out);
}

static MO_MUST_USE enum mo_host_outcome
file_error_to_sqlite(struct mo_host *host, struct mo_heap *heap, uint32_t result_type,
                     struct mo_cell path_cell, struct mo_file_error err, struct mo_cell *out) {
    if (err.status == FILE_NOT_ALLOWED)
        return error_outside_folder(host, heap, result_type, path_cell, out);
    if (err.reason != NULL)
        return error_failed(host, heap, result_type, err.reason, out);
    if (err.status == FILE_NOT_FOUND)
        return error_failed(host, heap, result_type, "no such file or folder", out);
    char buf[256];
    const char *msg =
        (err.number != 0 && strerror_r(err.number, buf, sizeof buf) == 0) ? buf : "system error";
    return error_failed(host, heap, result_type, msg, out);
}

static MO_MUST_USE enum mo_host_outcome
wrap_database(struct mo_host *host, struct mo_heap *heap, uint32_t result_type, sqlite3 *db,
              struct fake_sqlite_hub *hub, struct mo_cell *out, struct mo_host_error *error) {
    if (host->open_databases == MO_LIMIT_OPEN_DATABASES)
        return mo_host_bug(error, "too many databases open at once");
    uint32_t db_type = find_case_field_type(host, result_type, "Ok", 0);
    struct mo_capability_info info = {
        .operations = &mo_host_operations[hub != NULL][MO_CAP_DATABASE],
        .release = release_sqlite_db,
    };
    struct mo_sqlite_db payload = {.owner = host, .db = db, .hub = hub};
    struct mo_cell db_cap;
    if (!mo_value_capability_native(heap, db_type, &info, &payload, sizeof payload, &db_cap))
        return MO_HOST_OUT_OF_MEMORY;
    if (hub != NULL)
        hub_retain(hub);
    host->open_databases++;
    bool wrapped = mo_host_case(host, heap, result_type, "Ok", &db_cap, 1, out);
    mo_value_drop(heap, db_type, db_cap);
    return wrapped ? MO_HOST_RETURNED : MO_HOST_OUT_OF_MEMORY;
}

static MO_MUST_USE bool configure_connection(sqlite3 *db) {
    (void)sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, NULL);
    return sqlite3_exec(db, "PRAGMA temp_store = MEMORY;", NULL, NULL, NULL) == SQLITE_OK;
}

static MO_MUST_USE bool check_db_header(sqlite3 *db, char err_buf[256]) {
    if (!configure_connection(db)) {
        (void)snprintf(err_buf, 256, "%s", sqlite3_errmsg(db));
        return false;
    }
    int rc = sqlite3_exec(db, "SELECT count(*) FROM sqlite_schema;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        (void)snprintf(err_buf, 256, "%s", sqlite3_errmsg(db));
        return false;
    }
    return true;
}

enum mo_host_outcome mo_host_sqlite_real(struct mo_host *host, struct mo_heap *heap, uint32_t type,
                                         int folder, struct mo_cell *out,
                                         struct mo_host_error *error) {
    MO_ASSERT(folder >= 0);
    if (!mo_host_sqlite_init())
        return mo_host_bug(error, "sqlite initialization failed");
    if (host->open_folders == MO_LIMIT_OPEN_FOLDERS)
        return mo_host_bug(error, "too many folders open at once");
    int dup_fd = fcntl(folder, F_DUPFD_CLOEXEC, 0);
    if (dup_fd < 0)
        return mo_host_bug(error, "too many folders open at once");
    struct mo_capability_info info = {
        .operations = &mo_host_operations[false][MO_CAP_SQLITE],
        .release = release_sqlite_root,
    };
    struct mo_sqlite_root payload = {.owner = host, .descriptor = dup_fd};
    if (!mo_value_capability_native(heap, type, &info, &payload, sizeof payload, out)) {
        mo_host_close(dup_fd);
        return MO_HOST_OUT_OF_MEMORY;
    }
    host->open_folders++;
    return MO_HOST_RETURNED;
}

static MO_MUST_USE bool plain_relative(const uint8_t *bytes, size_t length) {
    size_t start = 0;
    for (size_t end = 0; end <= length; end++) {
        if (end < length && bytes[end] != '/')
            continue;
        size_t n = end - start;
        if (n == 0 || (bytes[start] == '.' && (n == 1 || (n == 2 && bytes[start + 1] == '.'))))
            return false;
        start = end + 1;
    }
    return true;
}

static MO_MUST_USE bool paths_overlap(const char *a, size_t alen, const char *b, size_t blen) {
    if (alen == blen)
        return memcmp(a, b, alen) == 0;
    if (alen < blen)
        return b[alen] == '/' && memcmp(a, b, alen) == 0;
    return a[blen] == '/' && memcmp(a, b, blen) == 0;
}

enum mo_host_outcome mo_host_sqlite_fake_new(struct mo_host *host, struct mo_heap *heap,
                                             uint32_t type, struct mo_cell databases,
                                             struct mo_cell *out, struct mo_host_error *error) {
    (void)host;
    if (!mo_host_sqlite_init())
        return mo_host_bug(error, "sqlite initialization failed");
    struct fake_sqlite_hub *hub = sqlite3_malloc64(sizeof *hub);
    if (hub == NULL)
        return MO_HOST_OUT_OF_MEMORY;
    hub->refs = 1;
    hub->head = NULL;

    uint32_t pos = 0;
    struct mo_cell key_cell, val_cell;
    struct fake_sqlite_entry *tail = NULL;
    while (mo_table_next(databases, &pos, &key_cell, &val_cell)) {
        size_t key_len = 0;
        const uint8_t *key_bytes = mo_value_data(key_cell, &key_len);
        if (key_len >= MO_LIMIT_HOST_PATH_SIZE || memchr(key_bytes, '\0', key_len) != NULL ||
            !plain_relative(key_bytes, key_len)) {
            hub_release(hub);
            return mo_host_bug(error, "invalid database path");
        }
        char norm[MO_LIMIT_HOST_PATH_SIZE];
        size_t norm_len = 0;
        struct mo_file_error nerr = mo_host_normalize(key_cell, norm, &norm_len);
        if (nerr.status != FILE_OK || norm_len == 0) {
            hub_release(hub);
            return mo_host_bug(error, "invalid database path");
        }
        for (const struct fake_sqlite_entry *e = hub->head; e != NULL; e = e->next) {
            size_t elen = strlen(e->path);
            if (paths_overlap(norm, norm_len, e->path, elen)) {
                hub_release(hub);
                return mo_host_bug(error, "conflicting database path");
            }
        }
        size_t bytes_len = 0;
        const uint8_t *bytes = mo_value_data(val_cell, &bytes_len);
        if (bytes_len > (size_t)INT64_MAX) {
            hub_release(hub);
            return mo_host_bug(error, "database image too large");
        }

        struct fake_sqlite_entry *entry = sqlite3_malloc64(sizeof *entry);
        char *path_copy = sqlite3_malloc64(norm_len + 1);
        if (entry == NULL || path_copy == NULL) {
            sqlite3_free(path_copy);
            sqlite3_free(entry);
            hub_release(hub);
            return MO_HOST_OUT_OF_MEMORY;
        }
        memcpy(path_copy, norm, norm_len + 1);
        *entry = (struct fake_sqlite_entry){.path = path_copy};

        if (bytes_len > 0) {
            bool valid_magic = bytes_len >= 100 && memcmp(bytes, "SQLite format 3\0", 16) == 0;
            sqlite3 *db = NULL;
            int rc = SQLITE_NOTADB;
            if (valid_magic) {
                rc = sqlite3_open_v2(
                    ":memory:", &db,
                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_EXRESCODE, NULL);
                if (rc == SQLITE_NOMEM || db == NULL) {
                    if (db != NULL)
                        (void)sqlite3_close(db);
                    sqlite3_free(path_copy);
                    sqlite3_free(entry);
                    hub_release(hub);
                    return MO_HOST_OUT_OF_MEMORY;
                }
                if (rc == SQLITE_OK) {
                    uint8_t *buf = sqlite3_malloc64(bytes_len);
                    if (buf == NULL) {
                        (void)sqlite3_close(db);
                        sqlite3_free(path_copy);
                        sqlite3_free(entry);
                        hub_release(hub);
                        return MO_HOST_OUT_OF_MEMORY;
                    }
                    memcpy(buf, bytes, bytes_len);
                    rc = sqlite3_deserialize(
                        db, "main", buf, (sqlite3_int64)bytes_len, (sqlite3_int64)bytes_len,
                        SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_RESIZEABLE);
                }
            }
            char dummy[256];
            if (valid_magic && rc == SQLITE_OK && check_db_header(db, dummy)) {
                entry->db = db;
            } else {
                if (db != NULL)
                    (void)sqlite3_close(db);
                uint8_t *raw = sqlite3_malloc64(bytes_len);
                if (raw == NULL) {
                    sqlite3_free(path_copy);
                    sqlite3_free(entry);
                    hub_release(hub);
                    return MO_HOST_OUT_OF_MEMORY;
                }
                memcpy(raw, bytes, bytes_len);
                entry->raw = raw;
                entry->raw_len = bytes_len;
            }
        }

        if (tail == NULL)
            hub->head = entry;
        else
            tail->next = entry;
        tail = entry;
    }

    struct mo_capability_info info = {
        .operations = &mo_host_operations[true][MO_CAP_SQLITE],
        .release = release_sqlite_root_fake,
    };
    struct mo_sqlite_root_fake payload = {.hub = hub};
    if (!mo_value_capability_native(heap, type, &info, &payload, sizeof payload, out)) {
        hub_release(hub);
        return MO_HOST_OUT_OF_MEMORY;
    }
    return MO_HOST_RETURNED;
}

bool mo_host_sqlite_snapshot(struct mo_host *host, struct mo_heap *heap, struct mo_cell capability,
                             struct mo_cell *out) {
    const struct mo_capability_info *info = mo_capability_get(capability);
    MO_ASSERT(info->operations == &mo_host_operations[true][MO_CAP_SQLITE]);
    const struct mo_sqlite_root_fake *root = (const struct mo_sqlite_root_fake *)info->handle;
    struct mo_cell map;
    if (!mo_value_map(heap, host->types->databases, NULL, NULL, 0, &map))
        return false;
    for (struct fake_sqlite_entry *e = root->hub->head; e != NULL; e = e->next) {
        struct mo_cell key, val;
        if (!mo_value_text(heap, e->path, strlen(e->path), &key)) {
            mo_value_drop(heap, host->types->databases, map);
            return false;
        }
        bool ok = false;
        if (e->db != NULL) {
            sqlite3_int64 sz = -1;
            unsigned char *serialized = sqlite3_serialize(e->db, "main", &sz, 0);
            if (serialized == NULL && sz == 0) {
                ok = mo_value_bytes(heap, NULL, 0, &val);
            } else if (serialized != NULL && sz >= 0) {
                ok = mo_value_bytes(heap, serialized, (size_t)sz, &val);
                sqlite3_free(serialized);
            } else {
                sqlite3_free(serialized);
            }
        } else {
            ok = mo_value_bytes(heap, e->raw, e->raw_len, &val);
        }
        if (!ok) {
            mo_value_drop(heap, host->types->text, key);
            mo_value_drop(heap, host->types->databases, map);
            return false;
        }
        bool inserted = mo_table_set(heap, host->types->databases, &map, key, val);
        mo_value_drop(heap, host->types->text, key);
        mo_value_drop(heap, host->types->bytes, val);
        if (!inserted) {
            mo_value_drop(heap, host->types->databases, map);
            return false;
        }
    }
    *out = map;
    return true;
}

static MO_MUST_USE bool dir_fd_path(int dir_fd, char out[MO_LIMIT_HOST_PATH_SIZE]) {
#ifdef __APPLE__
    return fcntl(dir_fd, F_GETPATH, out) == 0;
#else
    char proc[64];
    int printed = snprintf(proc, sizeof proc, "/proc/self/fd/%d", dir_fd);
    if (printed <= 0 || (size_t)printed >= sizeof proc)
        return false;
    ssize_t n = readlink(proc, out, MO_LIMIT_HOST_PATH_SIZE - 1);
    if (n <= 0 || (size_t)n >= MO_LIMIT_HOST_PATH_SIZE)
        return false;
    out[n] = '\0';
    struct stat dir_st, path_st;
    if (fstat(dir_fd, &dir_st) != 0 || stat(out, &path_st) != 0)
        return false;
    return dir_st.st_dev == path_st.st_dev && dir_st.st_ino == path_st.st_ino;
#endif
}

static MO_MUST_USE bool check_leaf_and_sidecars(int dir_fd, const char *leaf, bool *outside,
                                                const char **reason) {
    static const char *const suffixes[] = {"", "-journal", "-wal", "-shm"};
    for (size_t i = 0; i < sizeof suffixes / sizeof suffixes[0]; i++) {
        char candidate[MO_LIMIT_HOST_PATH_SIZE];
        int n = snprintf(candidate, sizeof candidate, "%s%s", leaf, suffixes[i]);
        if (n < 0 || (size_t)n >= sizeof candidate) {
            *outside = false;
            *reason = "path too long";
            return false;
        }
        struct stat st;
        if (fstatat(dir_fd, candidate, &st, AT_SYMLINK_NOFOLLOW) == 0) {
            if (S_ISLNK(st.st_mode)) {
                *outside = true;
                return false;
            }
            if (i == 0) {
                if (S_ISDIR(st.st_mode)) {
                    *outside = false;
                    *reason = "is a folder";
                    return false;
                }
                if (!S_ISREG(st.st_mode)) {
                    *outside = false;
                    *reason = "not a regular file";
                    return false;
                }
            }
        } else if (errno != ENOENT) {
            *outside = (errno == EACCES || errno == EPERM || errno == ELOOP);
            *reason = errno == ELOOP ? "too many links" : "unable to access path";
            return false;
        }
    }
    return true;
}

static MO_MUST_USE enum mo_host_outcome
open_real(struct mo_host *host, struct mo_heap *heap, uint32_t result_type,
          const struct mo_capability_info *info, struct mo_cell path_cell, const char *norm,
          struct mo_cell *out, struct mo_host_error *error) {
    const struct mo_sqlite_root *root = (const struct mo_sqlite_root *)info->handle;
    char parent[MO_LIMIT_HOST_PATH_SIZE];
    const char *slash = strrchr(norm, '/');
    const char *leaf = slash == NULL ? norm : slash + 1;
    if (slash == NULL) {
        parent[0] = '\0';
    } else {
        size_t plen = (size_t)(slash - norm);
        MO_ASSERT(plen < sizeof parent);
        memcpy(parent, norm, plen);
        parent[plen] = '\0';
    }

    struct mo_path_result resolved;
    struct mo_file_error perr = mo_host_resolve(
        host, root->descriptor, parent[0] == '\0' ? "." : parent, PATH_FOLDER, &resolved);
    if (perr.status != FILE_OK)
        return file_error_to_sqlite(host, heap, result_type, path_cell, perr, out);
    int dir_fd = resolved.descriptor;

    bool outside = false;
    const char *reason = NULL;
    if (!check_leaf_and_sidecars(dir_fd, leaf, &outside, &reason)) {
        mo_host_close(dir_fd);
        return outside ? error_outside_folder(host, heap, result_type, path_cell, out)
                       : error_failed(host, heap, result_type, reason, out);
    }

    char dir_path[MO_LIMIT_HOST_PATH_SIZE];
    char full_path[MO_LIMIT_HOST_PATH_SIZE];
    if (!dir_fd_path(dir_fd, dir_path)) {
        mo_host_close(dir_fd);
        return error_failed(host, heap, result_type, "unable to resolve database folder", out);
    }
    int fn = strcmp(dir_path, "/") == 0
                 ? snprintf(full_path, sizeof full_path, "/%s", leaf)
                 : snprintf(full_path, sizeof full_path, "%s/%s", dir_path, leaf);
    if (fn < 0 || (size_t)fn >= sizeof full_path) {
        mo_host_close(dir_fd);
        return error_failed(host, heap, result_type, "path too long", out);
    }

    if (host->open_databases == MO_LIMIT_OPEN_DATABASES) {
        mo_host_close(dir_fd);
        return mo_host_bug(error, "too many databases open at once");
    }

    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(full_path, &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOFOLLOW |
                                 SQLITE_OPEN_EXRESCODE,
                             NULL);
    if (rc != SQLITE_OK || db == NULL) {
        char msg[256];
        (void)snprintf(msg, sizeof msg, "%s",
                       db != NULL ? sqlite3_errmsg(db) : "unable to open database");
        if (db != NULL)
            (void)sqlite3_close(db);
        bool after_outside = false;
        const char *after_reason = NULL;
        if (!check_leaf_and_sidecars(dir_fd, leaf, &after_outside, &after_reason)) {
        }
        mo_host_close(dir_fd);
        if (rc == SQLITE_NOMEM)
            return MO_HOST_OUT_OF_MEMORY;
        return after_outside ? error_outside_folder(host, heap, result_type, path_cell, out)
                             : error_failed(host, heap, result_type, msg, out);
    }

    if (!check_leaf_and_sidecars(dir_fd, leaf, &outside, &reason)) {
        (void)sqlite3_close(db);
        mo_host_close(dir_fd);
        return outside ? error_outside_folder(host, heap, result_type, path_cell, out)
                       : error_failed(host, heap, result_type, reason, out);
    }
    struct stat leaf_st, full_st;
    if (fstatat(dir_fd, leaf, &leaf_st, AT_SYMLINK_NOFOLLOW) != 0 ||
        lstat(full_path, &full_st) != 0 || leaf_st.st_dev != full_st.st_dev ||
        leaf_st.st_ino != full_st.st_ino || !S_ISREG(full_st.st_mode)) {
        (void)sqlite3_close(db);
        mo_host_close(dir_fd);
        return error_outside_folder(host, heap, result_type, path_cell, out);
    }
    if (leaf_st.st_size > 0 && leaf_st.st_size < 100) {
        (void)sqlite3_close(db);
        mo_host_close(dir_fd);
        return error_failed(host, heap, result_type, "file is not a database", out);
    }
    mo_host_close(dir_fd);

    char msg[256];
    if (!check_db_header(db, msg)) {
        (void)sqlite3_close(db);
        return error_failed(host, heap, result_type, msg, out);
    }

    enum mo_host_outcome res = wrap_database(host, heap, result_type, db, NULL, out, error);
    if (res != MO_HOST_RETURNED)
        (void)sqlite3_close(db);
    return res;
}

static MO_MUST_USE enum mo_host_outcome
open_fake(struct mo_host *host, struct mo_heap *heap, uint32_t result_type,
          const struct mo_capability_info *info, const char *norm, size_t norm_len,
          struct mo_cell *out, struct mo_host_error *error) {
    const struct mo_sqlite_root_fake *root = (const struct mo_sqlite_root_fake *)info->handle;
    struct fake_sqlite_hub *hub = root->hub;

    struct fake_sqlite_entry *found = NULL;
    struct fake_sqlite_entry *tail = NULL;
    for (struct fake_sqlite_entry *e = hub->head; e != NULL; e = e->next) {
        size_t elen = strlen(e->path);
        if (elen == norm_len && memcmp(e->path, norm, norm_len) == 0) {
            found = e;
            break;
        }
        tail = e;
    }

    if (found == NULL) {
        for (const struct fake_sqlite_entry *e = hub->head; e != NULL; e = e->next) {
            size_t elen = strlen(e->path);
            if (norm_len < elen && e->path[norm_len] == '/' && memcmp(e->path, norm, norm_len) == 0)
                return error_failed(host, heap, result_type, "is a folder", out);
            if (elen < norm_len && norm[elen] == '/' && memcmp(e->path, norm, elen) == 0)
                return error_failed(host, heap, result_type, "not a folder", out);
        }
    }

    if (host->open_databases == MO_LIMIT_OPEN_DATABASES)
        return mo_host_bug(error, "too many databases open at once");

    if (found != NULL) {
        if (found->db == NULL && found->raw != NULL)
            return error_failed(host, heap, result_type, "file is not a database", out);
        if (found->db == NULL) {
            sqlite3 *db = NULL;
            int rc = sqlite3_open_v2(
                ":memory:", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_EXRESCODE,
                NULL);
            if (rc != SQLITE_OK || db == NULL || !configure_connection(db)) {
                if (db != NULL)
                    (void)sqlite3_close(db);
                return MO_HOST_OUT_OF_MEMORY;
            }
            found->db = db;
        }
        return wrap_database(host, heap, result_type, found->db, hub, out, error);
    }

    struct fake_sqlite_entry *entry = sqlite3_malloc64(sizeof *entry);
    char *path_copy = sqlite3_malloc64(norm_len + 1);
    if (entry == NULL || path_copy == NULL) {
        sqlite3_free(path_copy);
        sqlite3_free(entry);
        return MO_HOST_OUT_OF_MEMORY;
    }
    memcpy(path_copy, norm, norm_len + 1);
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(
        ":memory:", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_EXRESCODE, NULL);
    if (rc != SQLITE_OK || db == NULL || !configure_connection(db)) {
        if (db != NULL)
            (void)sqlite3_close(db);
        sqlite3_free(path_copy);
        sqlite3_free(entry);
        return MO_HOST_OUT_OF_MEMORY;
    }
    *entry = (struct fake_sqlite_entry){.path = path_copy, .db = db};
    if (tail == NULL)
        hub->head = entry;
    else
        tail->next = entry;

    return wrap_database(host, heap, result_type, db, hub, out, error);
}

static bool ascii_ieq(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static bool ascii_tok_ieq(const char *tok, size_t len, const char *lit) {
    size_t llen = strlen(lit);
    if (len != llen)
        return false;
    for (size_t i = 0; i < len; i++) {
        if (tolower((unsigned char)tok[i]) != tolower((unsigned char)lit[i]))
            return false;
    }
    return true;
}

static const char *skip_sql_ws_comments(const char *p, const char *end) {
    while (p < end) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') {
            p++;
        } else if (p + 1 < end && p[0] == '-' && p[1] == '-') {
            p += 2;
            while (p < end && *p != '\n')
                p++;
        } else if (p + 1 < end && p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/'))
                p++;
            if (p + 1 < end)
                p += 2;
            else
                p = end;
        } else {
            break;
        }
    }
    return p;
}

static bool sql_is_vacuum_into(const char *sql, size_t len) {
    const char *p = skip_sql_ws_comments(sql, sql + len);
    const char *end = sql + len;
    if (p >= end || !(isalpha((unsigned char)*p) || *p == '_'))
        return false;
    const char *t1 = p;
    while (p < end && (isalnum((unsigned char)*p) || *p == '_'))
        p++;
    if (!ascii_tok_ieq(t1, (size_t)(p - t1), "VACUUM"))
        return false;
    p = skip_sql_ws_comments(p, end);
    if (p >= end || !(isalpha((unsigned char)*p) || *p == '_'))
        return false;
    const char *t2 = p;
    while (p < end && (isalnum((unsigned char)*p) || *p == '_'))
        p++;
    if (ascii_tok_ieq(t2, (size_t)(p - t2), "INTO"))
        return true;
    p = skip_sql_ws_comments(p, end);
    if (p < end && *p == ';')
        return false;
    if (p < end && (isalpha((unsigned char)*p) || *p == '_')) {
        const char *t3 = p;
        while (p < end && (isalnum((unsigned char)*p) || *p == '_'))
            p++;
        if (ascii_tok_ieq(t3, (size_t)(p - t3), "INTO"))
            return true;
    }
    return false;
}

static int sqlite_authorizer(void *userdata, int action, const char *arg1, const char *arg2,
                             const char *db_name, const char *trigger_name) {
    (void)arg2;
    (void)db_name;
    (void)trigger_name;
    struct sqlite_auth *auth = (struct sqlite_auth *)userdata;
    if (action == SQLITE_ATTACH) {
        if (auth->in_step && !auth->vacuum_into && arg1 != NULL && arg1[0] == '\0')
            return SQLITE_OK;
        if (auth->denied_reason == NULL)
            auth->denied_reason = "ATTACH is not permitted";
        return SQLITE_DENY;
    }
    if (action == SQLITE_DETACH) {
        if (auth->in_step && !auth->vacuum_into && arg1 != NULL &&
            strncmp(arg1, "vacuum_", 7) == 0) {
            return SQLITE_OK;
        }
        if (auth->denied_reason == NULL)
            auth->denied_reason = "DETACH is not permitted";
        return SQLITE_DENY;
    }
    if (action == SQLITE_PRAGMA && arg1 != NULL &&
        (ascii_ieq(arg1, "temp_store") || ascii_ieq(arg1, "temp_store_directory"))) {
        if (auth->denied_reason == NULL)
            auth->denied_reason = "PRAGMA temp_store is not permitted";
        return SQLITE_DENY;
    }
    if (action == SQLITE_FUNCTION && arg2 != NULL && ascii_ieq(arg2, "load_extension")) {
        if (auth->denied_reason == NULL)
            auth->denied_reason = "load_extension is not permitted";
        return SQLITE_DENY;
    }
    return SQLITE_OK;
}

static MO_MUST_USE enum mo_host_outcome bind_params(struct mo_host *host, struct mo_heap *heap,
                                                    uint32_t result_type, sqlite3 *db,
                                                    sqlite3_stmt *stmt, struct mo_cell params,
                                                    struct mo_cell *out, bool *failed) {
    *failed = false;
    int expected = sqlite3_bind_parameter_count(stmt);
    uint32_t supplied = mo_list_size(params);
    if (expected < 0 || supplied != (uint32_t)expected) {
        *failed = true;
        return error_failed(host, heap, result_type, "SQL parameter count mismatch", out);
    }
    if (supplied == 0)
        return MO_HOST_RETURNED;

    const struct mo_types *types = &host->types->types;
    uint32_t val_type = find_choice_by_name(host, "Sqlite.Value");
    MO_ASSERT(val_type != MO_NO_INDEX);

    for (uint32_t i = 0; i < supplied; i++) {
        struct mo_cell elem;
        MO_ASSERT(mo_list_get(params, i, &elem));
        uint32_t tag = mo_cell_get_tag(elem);
        MO_ASSERT(tag < types->items[val_type].case_count);
        const char *cname = types->items[val_type].cases[tag].name;
        struct mo_cell payload = mo_cell_get_payload(elem);
        if (mo_case_shape(types, val_type, tag) == MO_CASE_BOXED)
            payload = mo_value_field(payload, 0);

        int idx = (int)(i + 1);
        int rc = SQLITE_ERROR;
        if (strcmp(cname, "Null") == 0) {
            rc = sqlite3_bind_null(stmt, idx);
        } else if (strcmp(cname, "Integer") == 0) {
            rc = sqlite3_bind_int64(stmt, idx, mo_cell_get_int(payload));
        } else if (strcmp(cname, "Real") == 0) {
            rc = sqlite3_bind_double(stmt, idx, mo_cell_get_float(payload));
        } else if (strcmp(cname, "String") == 0) {
            size_t len = 0;
            const uint8_t *p = mo_value_data(payload, &len);
            rc = sqlite3_bind_text64(stmt, idx, len == 0 ? "" : (const char *)p,
                                     (sqlite3_uint64)len, SQLITE_TRANSIENT, SQLITE_UTF8);
        } else if (strcmp(cname, "Blob") == 0) {
            size_t len = 0;
            const uint8_t *p = mo_value_data(payload, &len);
            rc = sqlite3_bind_blob64(stmt, idx, len == 0 ? (const uint8_t *)"" : p,
                                     (sqlite3_uint64)len, SQLITE_TRANSIENT);
        } else {
            MO_UNREACHABLE();
        }
        if (rc == SQLITE_NOMEM)
            return MO_HOST_OUT_OF_MEMORY;
        if (rc != SQLITE_OK) {
            *failed = true;
            return error_failed(host, heap, result_type, sqlite3_errmsg(db), out);
        }
    }
    return MO_HOST_RETURNED;
}

static MO_MUST_USE enum mo_host_outcome read_row_value(struct mo_host *host, struct mo_heap *heap,
                                                       uint32_t result_type, uint32_t val_type,
                                                       sqlite3_stmt *stmt, int col,
                                                       struct mo_cell *out_value,
                                                       struct mo_cell *out_err, bool *failed) {
    *failed = false;
    int col_type = sqlite3_column_type(stmt, col);
    switch (col_type) {
    case SQLITE_NULL:
        return mo_host_case(host, heap, val_type, "Null", NULL, 0, out_value)
                   ? MO_HOST_RETURNED
                   : MO_HOST_OUT_OF_MEMORY;
    case SQLITE_INTEGER: {
        struct mo_cell field = mo_cell_int(sqlite3_column_int64(stmt, col));
        return mo_host_case(host, heap, val_type, "Integer", &field, 1, out_value)
                   ? MO_HOST_RETURNED
                   : MO_HOST_OUT_OF_MEMORY;
    }
    case SQLITE_FLOAT: {
        struct mo_cell field = mo_cell_float(sqlite3_column_double(stmt, col));
        return mo_host_case(host, heap, val_type, "Real", &field, 1, out_value)
                   ? MO_HOST_RETURNED
                   : MO_HOST_OUT_OF_MEMORY;
    }
    case SQLITE_TEXT: {
        const unsigned char *s = sqlite3_column_text(stmt, col);
        int slen = sqlite3_column_bytes(stmt, col);
        if (slen < 0)
            return MO_HOST_OUT_OF_MEMORY;
        size_t len = (size_t)slen;
        if (!mo_utf8_valid(s, len)) {
            *failed = true;
            return error_failed(host, heap, result_type, "result text is not valid UTF-8", out_err);
        }
        struct mo_cell field;
        if (!mo_value_text(heap, len == 0 ? "" : (const char *)s, len, &field))
            return MO_HOST_OUT_OF_MEMORY;
        bool ok = mo_host_case(host, heap, val_type, "String", &field, 1, out_value);
        mo_value_drop(heap, host->types->text, field);
        return ok ? MO_HOST_RETURNED : MO_HOST_OUT_OF_MEMORY;
    }
    case SQLITE_BLOB: {
        const void *b = sqlite3_column_blob(stmt, col);
        int blen = sqlite3_column_bytes(stmt, col);
        if (blen < 0)
            return MO_HOST_OUT_OF_MEMORY;
        size_t len = (size_t)blen;
        struct mo_cell field;
        if (!mo_value_bytes(heap, len == 0 ? NULL : (const uint8_t *)b, len, &field))
            return MO_HOST_OUT_OF_MEMORY;
        bool ok = mo_host_case(host, heap, val_type, "Blob", &field, 1, out_value);
        mo_value_drop(heap, host->types->bytes, field);
        return ok ? MO_HOST_RETURNED : MO_HOST_OUT_OF_MEMORY;
    }
    default:
        MO_UNREACHABLE();
    }
}

static MO_MUST_USE enum mo_host_outcome finish_stmt(struct mo_host *host, struct mo_heap *heap,
                                                    uint32_t result_type, sqlite3 *db,
                                                    sqlite3_stmt *stmt,
                                                    const struct sqlite_auth *auth, int step_rc,
                                                    struct mo_cell *out, bool *failed) {
    *failed = false;
    char msg[256] = {0};
    if (step_rc != SQLITE_DONE && step_rc != SQLITE_ROW)
        (void)snprintf(msg, sizeof msg, "%s", sqlite3_errmsg(db));
    int fin_rc = sqlite3_finalize(stmt);
    sqlite3_set_authorizer(db, NULL, NULL);
    if (auth->denied_reason != NULL || step_rc == SQLITE_AUTH || fin_rc == SQLITE_AUTH) {
        *failed = true;
        return error_not_permitted(host, heap, result_type, auth->denied_reason, out);
    }
    if (step_rc == SQLITE_NOMEM || fin_rc == SQLITE_NOMEM)
        return MO_HOST_OUT_OF_MEMORY;
    if (step_rc != SQLITE_DONE) {
        *failed = true;
        return error_failed(host, heap, result_type, msg, out);
    }
    if (fin_rc != SQLITE_OK) {
        *failed = true;
        return error_failed(host, heap, result_type, sqlite3_errmsg(db), out);
    }
    return MO_HOST_RETURNED;
}

static MO_MUST_USE enum mo_host_outcome exec_sql(struct mo_host *host, struct mo_heap *heap,
                                                 bool is_query, uint32_t result_type,
                                                 const struct mo_cell *args, struct mo_cell *out) {
    const struct mo_capability_info *info = mo_capability_get(args[0]);
    struct mo_sqlite_db *d = (struct mo_sqlite_db *)info->handle;

    size_t sql_len = 0;
    const uint8_t *sql_bytes = mo_value_data(args[1], &sql_len);
    if (sql_len > MO_LIMIT_SQL_BYTES)
        return error_failed(host, heap, result_type, "SQL statement too large", out);
    if (memchr(sql_bytes, '\0', sql_len) != NULL)
        return error_failed(host, heap, result_type, "SQL contains NUL byte", out);
    if (mo_list_size(args[2]) > MO_LIMIT_SQL_PARAMETERS)
        return error_failed(host, heap, result_type, "too many SQL parameters", out);

    struct sqlite_auth auth = {
        .in_step = false,
        .vacuum_into = sql_is_vacuum_into((const char *)sql_bytes, sql_len),
    };
    sqlite3_set_authorizer(d->db, sqlite_authorizer, &auth);

    sqlite3_stmt *stmt = NULL;
    const char *tail = NULL;
    int prep_rc = sqlite3_prepare_v2(d->db, sql_len == 0 ? "" : (const char *)sql_bytes,
                                     (int)sql_len, &stmt, &tail);
    if (auth.denied_reason != NULL || prep_rc == SQLITE_AUTH) {
        if (stmt != NULL)
            (void)sqlite3_finalize(stmt);
        sqlite3_set_authorizer(d->db, NULL, NULL);
        return error_not_permitted(host, heap, result_type, auth.denied_reason, out);
    }
    if (prep_rc != SQLITE_OK) {
        char msg[256];
        (void)snprintf(msg, sizeof msg, "%s", sqlite3_errmsg(d->db));
        if (stmt != NULL)
            (void)sqlite3_finalize(stmt);
        sqlite3_set_authorizer(d->db, NULL, NULL);
        if (prep_rc == SQLITE_NOMEM)
            return MO_HOST_OUT_OF_MEMORY;
        return error_failed(host, heap, result_type, msg, out);
    }
    if (stmt == NULL) {
        sqlite3_set_authorizer(d->db, NULL, NULL);
        return error_failed(host, heap, result_type, "empty SQL statement", out);
    }
    const char *sql_end = sql_len == 0 ? tail : (const char *)sql_bytes + sql_len;
    const char *rest = skip_sql_ws_comments(tail, sql_end);
    if (rest < sql_end) {
        (void)sqlite3_finalize(stmt);
        sqlite3_set_authorizer(d->db, NULL, NULL);
        return error_failed(host, heap, result_type, "multiple SQL statements are not permitted",
                            out);
    }

    bool failed = false;
    enum mo_host_outcome bind_res =
        bind_params(host, heap, result_type, d->db, stmt, args[2], out, &failed);
    if (bind_res != MO_HOST_RETURNED || failed) {
        (void)sqlite3_finalize(stmt);
        sqlite3_set_authorizer(d->db, NULL, NULL);
        return bind_res;
    }

    auth.in_step = true;
    if (!is_query) {
        int rc;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        }
        enum mo_host_outcome fin =
            finish_stmt(host, heap, result_type, d->db, stmt, &auth, rc, out, &failed);
        if (fin != MO_HOST_RETURNED || failed)
            return fin;
        struct mo_cell unit = mo_cell_unit();
        return mo_host_case(host, heap, result_type, "Ok", &unit, 1, out) ? MO_HOST_RETURNED
                                                                          : MO_HOST_OUT_OF_MEMORY;
    }

    uint32_t rows_type = find_case_field_type(host, result_type, "Ok", 0);
    const struct mo_types *types = &host->types->types;
    uint32_t col_field = find_record_field(types, rows_type, "columns");
    uint32_t rows_field = find_record_field(types, rows_type, "rows");
    uint32_t list_list_val_type = types->items[rows_type].fields[rows_field].type;
    uint32_t list_val_type = types->items[list_list_val_type].parameters[0];
    uint32_t val_type = types->items[list_val_type].parameters[0];

    int col_count = sqlite3_column_count(stmt);
    MO_ASSERT(col_count >= 0);

    struct mo_cell columns;
    if (!mo_value_list(heap, host->types->texts, NULL, 0, &columns)) {
        (void)sqlite3_finalize(stmt);
        sqlite3_set_authorizer(d->db, NULL, NULL);
        return MO_HOST_OUT_OF_MEMORY;
    }
    for (int c = 0; c < col_count; c++) {
        const char *cname = sqlite3_column_name(stmt, c);
        if (cname == NULL)
            cname = "";
        size_t clen = strlen(cname);
        if (!mo_utf8_valid((const uint8_t *)cname, clen)) {
            mo_value_drop(heap, host->types->texts, columns);
            (void)sqlite3_finalize(stmt);
            sqlite3_set_authorizer(d->db, NULL, NULL);
            return error_failed(host, heap, result_type, "column name is not valid UTF-8", out);
        }
        struct mo_cell name_cell;
        if (!mo_value_text(heap, cname, clen, &name_cell)) {
            mo_value_drop(heap, host->types->texts, columns);
            (void)sqlite3_finalize(stmt);
            sqlite3_set_authorizer(d->db, NULL, NULL);
            return MO_HOST_OUT_OF_MEMORY;
        }
        bool app = mo_list_append(heap, host->types->texts, &columns, name_cell);
        mo_value_drop(heap, host->types->text, name_cell);
        if (!app) {
            mo_value_drop(heap, host->types->texts, columns);
            (void)sqlite3_finalize(stmt);
            sqlite3_set_authorizer(d->db, NULL, NULL);
            return MO_HOST_OUT_OF_MEMORY;
        }
    }

    struct mo_cell rows_list;
    if (!mo_value_list(heap, list_list_val_type, NULL, 0, &rows_list)) {
        mo_value_drop(heap, host->types->texts, columns);
        (void)sqlite3_finalize(stmt);
        sqlite3_set_authorizer(d->db, NULL, NULL);
        return MO_HOST_OUT_OF_MEMORY;
    }

    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        struct mo_cell row;
        if (!mo_value_list(heap, list_val_type, NULL, 0, &row)) {
            mo_value_drop(heap, list_list_val_type, rows_list);
            mo_value_drop(heap, host->types->texts, columns);
            (void)sqlite3_finalize(stmt);
            sqlite3_set_authorizer(d->db, NULL, NULL);
            return MO_HOST_OUT_OF_MEMORY;
        }
        for (int c = 0; c < col_count; c++) {
            struct mo_cell vcell;
            bool val_failed = false;
            enum mo_host_outcome vr = read_row_value(host, heap, result_type, val_type, stmt, c,
                                                     &vcell, out, &val_failed);
            if (vr != MO_HOST_RETURNED || val_failed) {
                mo_value_drop(heap, list_val_type, row);
                mo_value_drop(heap, list_list_val_type, rows_list);
                mo_value_drop(heap, host->types->texts, columns);
                (void)sqlite3_finalize(stmt);
                sqlite3_set_authorizer(d->db, NULL, NULL);
                return vr;
            }
            bool app = mo_list_append(heap, list_val_type, &row, vcell);
            mo_value_drop(heap, val_type, vcell);
            if (!app) {
                mo_value_drop(heap, list_val_type, row);
                mo_value_drop(heap, list_list_val_type, rows_list);
                mo_value_drop(heap, host->types->texts, columns);
                (void)sqlite3_finalize(stmt);
                sqlite3_set_authorizer(d->db, NULL, NULL);
                return MO_HOST_OUT_OF_MEMORY;
            }
        }
        bool app_row = mo_list_append(heap, list_list_val_type, &rows_list, row);
        mo_value_drop(heap, list_val_type, row);
        if (!app_row) {
            mo_value_drop(heap, list_list_val_type, rows_list);
            mo_value_drop(heap, host->types->texts, columns);
            (void)sqlite3_finalize(stmt);
            sqlite3_set_authorizer(d->db, NULL, NULL);
            return MO_HOST_OUT_OF_MEMORY;
        }
    }

    enum mo_host_outcome fin =
        finish_stmt(host, heap, result_type, d->db, stmt, &auth, rc, out, &failed);
    if (fin != MO_HOST_RETURNED || failed) {
        mo_value_drop(heap, list_list_val_type, rows_list);
        mo_value_drop(heap, host->types->texts, columns);
        return fin;
    }

    struct mo_cell rec_fields[2];
    rec_fields[col_field] = columns;
    rec_fields[rows_field] = rows_list;
    struct mo_cell rec;
    bool built = mo_value_product(heap, rows_type, rec_fields, 2, &rec);
    mo_value_drop(heap, host->types->texts, columns);
    mo_value_drop(heap, list_list_val_type, rows_list);
    if (!built)
        return MO_HOST_OUT_OF_MEMORY;
    bool wrapped = mo_host_case(host, heap, result_type, "Ok", &rec, 1, out);
    mo_value_drop(heap, rows_type, rec);
    return wrapped ? MO_HOST_RETURNED : MO_HOST_OUT_OF_MEMORY;
}

enum mo_host_outcome mo_host_sqlite_call(struct mo_host *host, struct mo_heap *heap,
                                         uint32_t operation, uint32_t type,
                                         const struct mo_cell *args, struct mo_cell *out,
                                         struct mo_host_error *error) {
    if (operation == MO_HOST_SQLITE_OPEN) {
        const struct mo_capability_info *info = mo_capability_get(args[0]);
        bool fake = info->operations == &mo_host_operations[true][MO_CAP_SQLITE];
        char norm[MO_LIMIT_HOST_PATH_SIZE];
        size_t norm_len = 0;
        struct mo_file_error nerr = mo_host_normalize(args[1], norm, &norm_len);
        if (nerr.status != FILE_OK)
            return file_error_to_sqlite(host, heap, type, args[1], nerr, out);
        if (norm_len == 0)
            return error_failed(host, heap, type, "is a folder", out);
        return fake ? open_fake(host, heap, type, info, norm, norm_len, out, error)
                    : open_real(host, heap, type, info, args[1], norm, out, error);
    }
    if (operation == MO_HOST_SQLITE_RUN)
        return exec_sql(host, heap, false, type, args, out);
    if (operation == MO_HOST_SQLITE_QUERY)
        return exec_sql(host, heap, true, type, args, out);
    MO_UNREACHABLE();
}
