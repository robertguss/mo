#define _POSIX_C_SOURCE 200809L
#include "host/host.h"
#include "host_fixture.h"
#include "test.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static struct mo_cell call_op(struct host_fixture *f, uint32_t op, const struct mo_cell *args) {
    struct mo_cell out;
    struct mo_host_error error = {0};
    MO_ASSERT(mo_host_call(&f->host, f->heap, op, host_result_type(f, op), args, &out, &error) ==
              MO_HOST_RETURNED);
    return out;
}

static struct mo_cell unwrap_ok(const struct host_fixture *f, uint32_t result_type,
                                struct mo_cell res) {
    MO_ASSERT(mo_cell_get_tag(res) == 0);
    struct mo_cell payload = mo_cell_get_payload(res);
    if (mo_case_shape(&f->types.types, result_type, 0) == MO_CASE_BOXED)
        return mo_value_field(payload, 0);
    return payload;
}

static struct mo_cell unwrap_err(const struct host_fixture *f, uint32_t result_type,
                                 struct mo_cell res) {
    MO_ASSERT(mo_cell_get_tag(res) == 1);
    struct mo_cell payload = mo_cell_get_payload(res);
    if (mo_case_shape(&f->types.types, result_type, 1) == MO_CASE_BOXED)
        return mo_value_field(payload, 0);
    return payload;
}

static uint32_t value_list_type(const struct host_fixture *f) {
    uint32_t rows_matrix = f->types.types.items[f->sqlite_rows].fields[1].type;
    return f->types.types.items[rows_matrix].parameters[0];
}

static struct mo_cell empty_values(struct host_fixture *f) {
    struct mo_cell list;
    MO_ASSERT(mo_value_list(f->heap, value_list_type(f), NULL, 0, &list));
    return list;
}

static struct mo_cell val_null(struct host_fixture *f) {
    struct mo_cell out;
    MO_ASSERT(mo_value_case(f->heap, f->sqlite_value, 0, NULL, 0, &out));
    return out;
}

static struct mo_cell val_int(struct host_fixture *f, int64_t n) {
    struct mo_cell field = mo_cell_int(n), out;
    MO_ASSERT(mo_value_case(f->heap, f->sqlite_value, 1, &field, 1, &out));
    return out;
}

static struct mo_cell val_real(struct host_fixture *f, double d) {
    struct mo_cell field = mo_cell_float(d), out;
    MO_ASSERT(mo_value_case(f->heap, f->sqlite_value, 2, &field, 1, &out));
    return out;
}

static struct mo_cell val_text_len(struct host_fixture *f, const char *s, size_t n) {
    struct mo_cell t, out;
    MO_ASSERT(mo_value_text(f->heap, s, n, &t));
    MO_ASSERT(mo_value_case(f->heap, f->sqlite_value, 3, &t, 1, &out));
    mo_value_drop(f->heap, f->types.text, t);
    return out;
}

static struct mo_cell val_blob(struct host_fixture *f, const uint8_t *b, size_t n) {
    struct mo_cell cell, out;
    MO_ASSERT(mo_value_bytes(f->heap, b, n, &cell));
    MO_ASSERT(mo_value_case(f->heap, f->sqlite_value, 4, &cell, 1, &out));
    mo_value_drop(f->heap, f->types.bytes, cell);
    return out;
}

static struct mo_cell make_list(struct host_fixture *f, const struct mo_cell *vals,
                                uint32_t count) {
    struct mo_cell list;
    MO_ASSERT(mo_value_list(f->heap, value_list_type(f), vals, count, &list));
    for (uint32_t i = 0; i < count; i++)
        mo_value_drop(f->heap, f->sqlite_value, vals[i]);
    return list;
}

static struct mo_cell open_db_res(struct host_fixture *f, struct mo_cell sqlite_cap,
                                  const char *path) {
    struct mo_cell p = host_text(f, path);
    struct mo_cell args[] = {sqlite_cap, p};
    struct mo_cell res = call_op(f, MO_HOST_SQLITE_OPEN, args);
    mo_value_drop(f->heap, f->types.text, p);
    return res;
}

static struct mo_cell run_sql_res(struct host_fixture *f, struct mo_cell db, const char *sql,
                                  struct mo_cell params) {
    struct mo_cell s = host_text(f, sql);
    struct mo_cell args[] = {db, s, params};
    struct mo_cell res = call_op(f, MO_HOST_SQLITE_RUN, args);
    mo_value_drop(f->heap, f->types.text, s);
    mo_value_drop(f->heap, value_list_type(f), params);
    return res;
}

static struct mo_cell query_sql_res(struct host_fixture *f, struct mo_cell db, const char *sql,
                                    struct mo_cell params) {
    struct mo_cell s = host_text(f, sql);
    struct mo_cell args[] = {db, s, params};
    struct mo_cell res = call_op(f, MO_HOST_SQLITE_QUERY, args);
    mo_value_drop(f->heap, f->types.text, s);
    mo_value_drop(f->heap, value_list_type(f), params);
    return res;
}

static void expect_printed(struct host_fixture *f, uint32_t type, struct mo_cell value,
                           const char *expected) {
    struct mo_buffer out;
    mo_buffer_init(&out, MO_LIMIT_STRING_SIZE);
    CHECK(mo_value_print(f->heap, type, value, &out));
    CHECK(out.length == strlen(expected) && memcmp(out.bytes, expected, out.length) == 0);
    if (out.length != strlen(expected) || memcmp(out.bytes, expected, out.length) != 0)
        (void)fprintf(stderr, "expected: %s\nactual:   %.*s\n", expected, (int)out.length,
                      out.bytes);
    mo_buffer_free(&out);
}

static const char *err_case_name(const struct host_fixture *f, uint32_t result_type,
                                 struct mo_cell res) {
    struct mo_cell err = unwrap_err(f, result_type, res);
    uint32_t tag = mo_cell_get_tag(err);
    return f->types.types.items[f->sqlite_error].cases[tag].name;
}

static struct mo_cell make_real_sqlite(struct host_fixture *f, const char *dir_path) {
    int fd = open(dir_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    MO_ASSERT(fd >= 0);
    struct mo_cell out;
    struct mo_host_error error = {0};
    MO_ASSERT(mo_host_real(&f->host, f->heap, f->capabilities[MO_CAP_SQLITE], NULL, fd, &out,
                           &error) == MO_HOST_RETURNED);
    (void)close(fd);
    return out;
}

static void remove_tree(const char *path) {
    DIR *dir = opendir(path);
    if (dir == NULL)
        return;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char child[PATH_MAX];
        (void)snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
            remove_tree(child);
        else
            (void)unlink(child);
    }
    (void)closedir(dir);
    (void)rmdir(path);
}

static void write_raw_file(const char *path, const uint8_t *bytes, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    MO_ASSERT(fd >= 0);
    size_t written = 0;
    while (written < n) {
        ssize_t w = write(fd, bytes + written, n - written);
        MO_ASSERT(w > 0);
        written += (size_t)w;
    }
    MO_ASSERT(close(fd) == 0);
}

static void test_real_crud(void) {
    char template[] = "/tmp/mo-sqlite-crud-XXXXXX";
    char *dir = mkdtemp(template);
    MO_ASSERT(dir != NULL);

    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    struct mo_cell sq = make_real_sqlite(&f, dir);

    struct mo_cell opened = open_db_res(&f, sq, "app.db");
    struct mo_cell db = unwrap_ok(&f, f.result_database, opened);

    struct mo_cell jm = query_sql_res(&f, db, "PRAGMA journal_mode", empty_values(&f));
    expect_printed(&f, f.result_sqlite_rows, jm,
                   "Ok(Sqlite.Rows(columns: [\"journal_mode\"], rows: [[String(\"wal\")]]))");
    mo_value_drop(f.heap, f.result_sqlite_rows, jm);

    struct mo_cell create = run_sql_res(
        &f, db, "CREATE TABLE items(id INTEGER PRIMARY KEY, n INTEGER, r REAL, s TEXT, b BLOB)",
        empty_values(&f));
    expect_printed(&f, f.result_sqlite_unit, create, "Ok(Unit)");
    mo_value_drop(f.heap, f.result_sqlite_unit, create);

    struct mo_cell row1[] = {val_int(&f, 1), val_null(&f), val_real(&f, 3.5),
                             val_text_len(&f, "a\0b", 3),
                             val_blob(&f, (const uint8_t *)"\x00\xff", 2)};
    struct mo_cell ins1 = run_sql_res(
        &f, db, "INSERT INTO items(id, n, r, s, b) VALUES (?, ?, ?, ?, ?)", make_list(&f, row1, 5));
    expect_printed(&f, f.result_sqlite_unit, ins1, "Ok(Unit)");
    mo_value_drop(f.heap, f.result_sqlite_unit, ins1);

    const uint8_t empty_b = 0;
    struct mo_cell row2[] = {val_int(&f, 2), val_int(&f, -99), val_real(&f, 0.25),
                             val_text_len(&f, "", 0), val_blob(&f, &empty_b, 0)};
    struct mo_cell ins2 = run_sql_res(
        &f, db, "INSERT INTO items(id, n, r, s, b) VALUES (?, ?, ?, ?, ?)", make_list(&f, row2, 5));
    expect_printed(&f, f.result_sqlite_unit, ins2, "Ok(Unit)");
    mo_value_drop(f.heap, f.result_sqlite_unit, ins2);

    struct mo_cell beg = run_sql_res(&f, db, "BEGIN", empty_values(&f));
    mo_value_drop(f.heap, f.result_sqlite_unit, beg);
    struct mo_cell tmp_row[] = {val_int(&f, 3)};
    struct mo_cell ins_rb =
        run_sql_res(&f, db, "INSERT INTO items(id) VALUES (?)", make_list(&f, tmp_row, 1));
    mo_value_drop(f.heap, f.result_sqlite_unit, ins_rb);
    struct mo_cell rb = run_sql_res(&f, db, "ROLLBACK", empty_values(&f));
    mo_value_drop(f.heap, f.result_sqlite_unit, rb);

    struct mo_cell beg2 = run_sql_res(&f, db, "BEGIN", empty_values(&f));
    mo_value_drop(f.heap, f.result_sqlite_unit, beg2);
    struct mo_cell row3[] = {val_int(&f, 4)};
    struct mo_cell ins_c =
        run_sql_res(&f, db, "INSERT INTO items(id) VALUES (?)", make_list(&f, row3, 1));
    mo_value_drop(f.heap, f.result_sqlite_unit, ins_c);
    struct mo_cell cmt = run_sql_res(&f, db, "COMMIT", empty_values(&f));
    mo_value_drop(f.heap, f.result_sqlite_unit, cmt);

    mo_value_drop(f.heap, f.result_database, opened);
    CHECK(f.host.open_databases == 0);

    opened = open_db_res(&f, sq, "app.db");
    db = unwrap_ok(&f, f.result_database, opened);
    struct mo_cell q =
        query_sql_res(&f, db, "SELECT id, n, r, s, b FROM items ORDER BY id", empty_values(&f));
    MO_ASSERT(mo_cell_get_tag(q) == 0);
    struct mo_cell rows_rec = unwrap_ok(&f, f.result_sqlite_rows, q);
    struct mo_cell cols = mo_value_field(rows_rec, 0);
    struct mo_cell rows = mo_value_field(rows_rec, 1);
    expect_printed(&f, f.types.texts, cols, "[\"id\", \"n\", \"r\", \"s\", \"b\"]");
    CHECK(mo_list_size(rows) == 3);

    struct mo_cell r0;
    MO_ASSERT(mo_list_get(rows, 0, &r0));
    struct mo_cell r0_s;
    MO_ASSERT(mo_list_get(r0, 3, &r0_s));
    uint32_t s_tag = mo_cell_get_tag(r0_s);
    MO_ASSERT(s_tag == 3);
    struct mo_cell s_payload = mo_cell_get_payload(r0_s);
    if (mo_case_shape(&f.types.types, f.sqlite_value, s_tag) == MO_CASE_BOXED)
        s_payload = mo_value_field(s_payload, 0);
    size_t s_len = 0;
    const uint8_t *s_bytes = mo_value_data(s_payload, &s_len);
    CHECK(s_len == 3 && memcmp(s_bytes, "a\0b", 3) == 0);

    struct mo_cell r1;
    MO_ASSERT(mo_list_get(rows, 1, &r1));
    struct mo_cell r1_b;
    MO_ASSERT(mo_list_get(r1, 4, &r1_b));
    uint32_t b_tag = mo_cell_get_tag(r1_b);
    MO_ASSERT(b_tag == 4);
    struct mo_cell b_payload = mo_cell_get_payload(r1_b);
    if (mo_case_shape(&f.types.types, f.sqlite_value, b_tag) == MO_CASE_BOXED)
        b_payload = mo_value_field(b_payload, 0);
    size_t b_len = 99;
    const uint8_t *b_bytes = mo_value_data(b_payload, &b_len);
    CHECK(b_bytes != NULL && b_len == 0);

    mo_value_drop(f.heap, f.result_sqlite_rows, q);
    mo_value_drop(f.heap, f.result_database, opened);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_SQLITE], sq);
    CHECK(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
    remove_tree(dir);
}

static void test_real_errors(void) {
    char template[] = "/tmp/mo-sqlite-err-XXXXXX";
    char *dir = mkdtemp(template);
    MO_ASSERT(dir != NULL);

    char bad_path[PATH_MAX], empty_path[PATH_MAX];
    (void)snprintf(bad_path, sizeof bad_path, "%s/bad.db", dir);
    (void)snprintf(empty_path, sizeof empty_path, "%s/empty.db", dir);
    write_raw_file(bad_path, (const uint8_t *)"not a sqlite file", 17);
    write_raw_file(empty_path, (const uint8_t *)"", 0);

    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    struct mo_cell sq = make_real_sqlite(&f, dir);

    struct mo_cell bad_open = open_db_res(&f, sq, "bad.db");
    CHECK(strcmp(err_case_name(&f, f.result_database, bad_open), "SqlFailed") == 0);
    mo_value_drop(f.heap, f.result_database, bad_open);

    struct mo_cell opened = open_db_res(&f, sq, "empty.db");
    MO_ASSERT(mo_cell_get_tag(opened) == 0);
    struct mo_cell db = unwrap_ok(&f, f.result_database, opened);

    static const char *const bad_sqls[] = {"", "   -- only comment\n", "SELECT 1; SELECT 2",
                                           "SELECT CAST(X'FF' AS TEXT)", "SELEC 1"};
    for (size_t i = 0; i < sizeof bad_sqls / sizeof bad_sqls[0]; i++) {
        struct mo_cell q = query_sql_res(&f, db, bad_sqls[i], empty_values(&f));
        CHECK(strcmp(err_case_name(&f, f.result_sqlite_rows, q), "SqlFailed") == 0);
        mo_value_drop(f.heap, f.result_sqlite_rows, q);
    }

    struct mo_cell comment_ok =
        query_sql_res(&f, db, "SELECT 1; -- trailing comment\n", empty_values(&f));
    CHECK(mo_cell_get_tag(comment_ok) == 0);
    mo_value_drop(f.heap, f.result_sqlite_rows, comment_ok);

    struct mo_cell nul_sql;
    MO_ASSERT(mo_value_text(f.heap, "SELECT '\0'", 10, &nul_sql));
    struct mo_cell q_args[] = {db, nul_sql, empty_values(&f)};
    struct mo_cell q_nul = call_op(&f, MO_HOST_SQLITE_QUERY, q_args);
    CHECK(strcmp(err_case_name(&f, f.result_sqlite_rows, q_nul), "SqlFailed") == 0);
    mo_value_drop(f.heap, f.result_sqlite_rows, q_nul);
    mo_value_drop(f.heap, f.types.text, q_args[1]);
    mo_value_drop(f.heap, value_list_type(&f), q_args[2]);

    struct mo_cell one_arg[] = {val_int(&f, 1)};
    struct mo_cell mismatch = query_sql_res(&f, db, "SELECT ? + ?", make_list(&f, one_arg, 1));
    CHECK(strcmp(err_case_name(&f, f.result_sqlite_rows, mismatch), "SqlFailed") == 0);
    mo_value_drop(f.heap, f.result_sqlite_rows, mismatch);

    size_t big_len = MO_LIMIT_SQL_BYTES + 1;
    char *big_sql = malloc(big_len);
    MO_ASSERT(big_sql != NULL);
    memset(big_sql, ' ', big_len);
    struct mo_cell big_cell;
    MO_ASSERT(mo_value_text(f.heap, big_sql, big_len, &big_cell));
    free(big_sql);
    struct mo_cell big_args[] = {db, big_cell, empty_values(&f)};
    struct mo_cell big_res = call_op(&f, MO_HOST_SQLITE_RUN, big_args);
    CHECK(strcmp(err_case_name(&f, f.result_sqlite_unit, big_res), "SqlFailed") == 0);
    mo_value_drop(f.heap, f.result_sqlite_unit, big_res);
    mo_value_drop(f.heap, f.types.text, big_cell);
    mo_value_drop(f.heap, value_list_type(&f), big_args[2]);

    mo_value_drop(f.heap, f.result_database, opened);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_SQLITE], sq);
    CHECK(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
    remove_tree(dir);
}

static void test_containment(void) {
    char template_in[] = "/tmp/mo-sqlite-in-XXXXXX";
    char template_out[] = "/tmp/mo-sqlite-out-XXXXXX";
    char *dir = mkdtemp(template_in);
    char *out_dir = mkdtemp(template_out);
    MO_ASSERT(dir != NULL && out_dir != NULL);

    char out_file[PATH_MAX], sub_dir[PATH_MAX], link_db[PATH_MAX], link_dir[PATH_MAX];
    char j_link[PATH_MAX], w_link[PATH_MAX], s_link[PATH_MAX];
    (void)snprintf(out_file, sizeof out_file, "%s/secret.db", out_dir);
    write_raw_file(out_file, (const uint8_t *)"", 0);
    (void)snprintf(sub_dir, sizeof sub_dir, "%s/sub", dir);
    MO_ASSERT(mkdir(sub_dir, 0755) == 0);
    (void)snprintf(link_db, sizeof link_db, "%s/escape.db", dir);
    (void)snprintf(link_dir, sizeof link_dir, "%s/linkdir", dir);
    (void)snprintf(j_link, sizeof j_link, "%s/j.db-journal", dir);
    (void)snprintf(w_link, sizeof w_link, "%s/w.db-wal", dir);
    (void)snprintf(s_link, sizeof s_link, "%s/s.db-shm", dir);
    MO_ASSERT(symlink(out_file, link_db) == 0);
    MO_ASSERT(symlink(out_dir, link_dir) == 0);
    MO_ASSERT(symlink(out_file, j_link) == 0);
    MO_ASSERT(symlink(out_file, w_link) == 0);
    MO_ASSERT(symlink(out_file, s_link) == 0);

    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    struct mo_cell sq = make_real_sqlite(&f, dir);

    static const char *const outside_paths[] = {
        "",     "../out.db", "sub/../../out.db", "/tmp/abs.db", "escape.db", "linkdir/a.db", "j.db",
        "w.db", "s.db"};
    for (size_t i = 0; i < sizeof outside_paths / sizeof outside_paths[0]; i++) {
        struct mo_cell res = open_db_res(&f, sq, outside_paths[i]);
        CHECK(strcmp(err_case_name(&f, f.result_database, res), "OutsideFolder") == 0);
        mo_value_drop(f.heap, f.result_database, res);
    }

    static const char *const folder_paths[] = {".", "sub"};
    for (size_t i = 0; i < sizeof folder_paths / sizeof folder_paths[0]; i++) {
        struct mo_cell res = open_db_res(&f, sq, folder_paths[i]);
        CHECK(strcmp(err_case_name(&f, f.result_database, res), "SqlFailed") == 0);
        mo_value_drop(f.heap, f.result_database, res);
    }

    mo_value_drop(f.heap, f.capabilities[MO_CAP_SQLITE], sq);
    CHECK(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
    remove_tree(dir);
    remove_tree(out_dir);
}

static void test_authorizer(void) {
    char template[] = "/tmp/mo-sqlite-auth-XXXXXX";
    char *dir = mkdtemp(template);
    MO_ASSERT(dir != NULL);

    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    struct mo_cell sq = make_real_sqlite(&f, dir);
    struct mo_cell opened = open_db_res(&f, sq, "auth.db");
    struct mo_cell db = unwrap_ok(&f, f.result_database, opened);

    struct mo_cell mk = run_sql_res(&f, db, "CREATE TABLE t(x INT)", empty_values(&f));
    mo_value_drop(f.heap, f.result_sqlite_unit, mk);

    static const char *const denied[] = {
        "ATTACH 'other.db' AS b",
        "ATTACH ':memory:' AS m",
        "VACUUM INTO 'copy.db'",
        "VACUUM INTO ''",
        "VACUUM main INTO 'copy.db'",
        "PRAGMA temp_store = FILE",
        "PRAGMA temp_store_directory = '/tmp'",
    };
    for (size_t i = 0; i < sizeof denied / sizeof denied[0]; i++) {
        struct mo_cell res = run_sql_res(&f, db, denied[i], empty_values(&f));
        CHECK(strcmp(err_case_name(&f, f.result_sqlite_unit, res), "NotPermitted") == 0);
        mo_value_drop(f.heap, f.result_sqlite_unit, res);
    }

    char copy_path[PATH_MAX], other_path[PATH_MAX];
    (void)snprintf(copy_path, sizeof copy_path, "%s/copy.db", dir);
    (void)snprintf(other_path, sizeof other_path, "%s/other.db", dir);
    struct stat st;
    CHECK(lstat(copy_path, &st) != 0 && errno == ENOENT);
    CHECK(lstat(other_path, &st) != 0 && errno == ENOENT);

    struct mo_cell vac = run_sql_res(&f, db, "VACUUM", empty_values(&f));
    expect_printed(&f, f.result_sqlite_unit, vac, "Ok(Unit)");
    mo_value_drop(f.heap, f.result_sqlite_unit, vac);

    mo_value_drop(f.heap, f.result_database, opened);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_SQLITE], sq);
    CHECK(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
    remove_tree(dir);
}

static void test_open_limit(void) {
    char template[] = "/tmp/mo-sqlite-lim-XXXXXX";
    char *dir = mkdtemp(template);
    MO_ASSERT(dir != NULL);

    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    struct mo_cell sq = make_real_sqlite(&f, dir);

    struct mo_cell opened[MO_LIMIT_OPEN_DATABASES];
    for (unsigned i = 0; i < MO_LIMIT_OPEN_DATABASES; i++) {
        char name[32];
        (void)snprintf(name, sizeof name, "db-%u.sqlite", i);
        opened[i] = open_db_res(&f, sq, name);
        CHECK(mo_cell_get_tag(opened[i]) == 0);
    }
    CHECK(f.host.open_databases == MO_LIMIT_OPEN_DATABASES);

    struct mo_cell path17 = host_text(&f, "db-17.sqlite");
    struct mo_cell args17[] = {sq, path17};
    struct mo_cell out17 = mo_cell_int(0);
    struct mo_host_error error = {0};
    CHECK(mo_host_call(&f.host, f.heap, MO_HOST_SQLITE_OPEN, f.result_database, args17, &out17,
                       &error) == MO_HOST_BUG);
    mo_value_drop(f.heap, f.types.text, path17);

    mo_value_drop(f.heap, f.result_database, opened[0]);
    opened[0] = open_db_res(&f, sq, "db-17.sqlite");
    CHECK(mo_cell_get_tag(opened[0]) == 0);

    for (unsigned i = 0; i < MO_LIMIT_OPEN_DATABASES; i++)
        mo_value_drop(f.heap, f.result_database, opened[i]);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_SQLITE], sq);
    CHECK(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
    remove_tree(dir);
}

static void test_fake_sqlite(void) {
    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    struct mo_host_error error = {0};

    struct mo_cell bad_key = host_text(&f, "bad.db");
    struct mo_cell bad_bytes, init_map, fake1;
    MO_ASSERT(mo_value_bytes(f.heap, (const uint8_t *)"garbage", 7, &bad_bytes));
    MO_ASSERT(mo_value_map(f.heap, f.types.databases, &bad_key, &bad_bytes, 1, &init_map));
    mo_value_drop(f.heap, f.types.text, bad_key);
    mo_value_drop(f.heap, f.types.bytes, bad_bytes);

    struct mo_host_fake_state init_state = {.kind = MO_CAP_SQLITE, .as.sqlite = init_map};
    MO_ASSERT(mo_host_fake_restore(&f.host, f.heap, f.capabilities[MO_CAP_SQLITE], &init_state,
                                   &fake1, &error) == MO_HOST_RETURNED);
    mo_value_drop(f.heap, f.types.databases, init_map);

    struct mo_cell bad_open = open_db_res(&f, fake1, "bad.db");
    CHECK(strcmp(err_case_name(&f, f.result_database, bad_open), "SqlFailed") == 0);
    mo_value_drop(f.heap, f.result_database, bad_open);

    struct mo_cell o1 = open_db_res(&f, fake1, "a.db");
    struct mo_cell o2 = open_db_res(&f, fake1, "./a.db");
    struct mo_cell d1 = unwrap_ok(&f, f.result_database, o1);
    struct mo_cell d2 = unwrap_ok(&f, f.result_database, o2);
    struct mo_cell c1 = run_sql_res(&f, d1, "CREATE TABLE t(v INT)", empty_values(&f));
    mo_value_drop(f.heap, f.result_sqlite_unit, c1);
    struct mo_cell i1 = run_sql_res(&f, d1, "INSERT INTO t VALUES (9)", empty_values(&f));
    mo_value_drop(f.heap, f.result_sqlite_unit, i1);
    struct mo_cell q2 = query_sql_res(&f, d2, "SELECT v FROM t", empty_values(&f));
    expect_printed(&f, f.result_sqlite_rows, q2,
                   "Ok(Sqlite.Rows(columns: [\"v\"], rows: [[Integer(9)]]))");
    mo_value_drop(f.heap, f.result_sqlite_rows, q2);
    mo_value_drop(f.heap, f.result_database, o1);
    mo_value_drop(f.heap, f.result_database, o2);

    struct mo_cell esc = open_db_res(&f, fake1, "../x.db");
    CHECK(strcmp(err_case_name(&f, f.result_database, esc), "OutsideFolder") == 0);
    mo_value_drop(f.heap, f.result_database, esc);

    struct mo_host_fake_state snap = {0};
    CHECK(mo_host_fake_snapshot(&f.host, f.heap, fake1, &snap));
    CHECK(snap.kind == MO_CAP_SQLITE);

    struct mo_cell restored;
    MO_ASSERT(mo_host_fake_restore(&f.host, f.heap, f.capabilities[MO_CAP_SQLITE], &snap, &restored,
                                   &error) == MO_HOST_RETURNED);
    mo_host_fake_state_drop(&f.host, f.heap, &snap);

    struct mo_cell r_bad = open_db_res(&f, restored, "bad.db");
    CHECK(strcmp(err_case_name(&f, f.result_database, r_bad), "SqlFailed") == 0);
    mo_value_drop(f.heap, f.result_database, r_bad);

    struct mo_cell r_ok = open_db_res(&f, restored, "a.db");
    struct mo_cell r_db = unwrap_ok(&f, f.result_database, r_ok);
    struct mo_cell r_q = query_sql_res(&f, r_db, "SELECT v FROM t", empty_values(&f));
    expect_printed(&f, f.result_sqlite_rows, r_q,
                   "Ok(Sqlite.Rows(columns: [\"v\"], rows: [[Integer(9)]]))");
    mo_value_drop(f.heap, f.result_sqlite_rows, r_q);
    mo_value_drop(f.heap, f.result_database, r_ok);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_SQLITE], restored);

    struct mo_cell fake2 = call_op(&f, MO_HOST_SQLITE_FAKE, NULL);
    struct mo_cell f2_open = open_db_res(&f, fake2, "a.db");
    struct mo_cell f2_q = query_sql_res(&f, unwrap_ok(&f, f.result_database, f2_open),
                                        "SELECT name FROM sqlite_master", empty_values(&f));
    expect_printed(&f, f.result_sqlite_rows, f2_q,
                   "Ok(Sqlite.Rows(columns: [\"name\"], rows: []))");
    mo_value_drop(f.heap, f.result_sqlite_rows, f2_q);
    mo_value_drop(f.heap, f.result_database, f2_open);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_SQLITE], fake2);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_SQLITE], fake1);

    /* Prefix conflicts on open and invalid fake initial keys. */
    struct mo_cell pkey = host_text(&f, "dir/a.db"), pval, pmap, pfake;
    MO_ASSERT(mo_value_bytes(f.heap, NULL, 0, &pval));
    MO_ASSERT(mo_value_map(f.heap, f.types.databases, &pkey, &pval, 1, &pmap));
    mo_value_drop(f.heap, f.types.text, pkey);
    mo_value_drop(f.heap, f.types.bytes, pval);
    struct mo_host_fake_state pstate = {.kind = MO_CAP_SQLITE, .as.sqlite = pmap};
    MO_ASSERT(mo_host_fake_restore(&f.host, f.heap, f.capabilities[MO_CAP_SQLITE], &pstate, &pfake,
                                   &error) == MO_HOST_RETURNED);
    mo_value_drop(f.heap, f.types.databases, pmap);
    struct mo_cell odir = open_db_res(&f, pfake, "dir");
    CHECK(strcmp(err_case_name(&f, f.result_database, odir), "SqlFailed") == 0);
    mo_value_drop(f.heap, f.result_database, odir);
    struct mo_cell ochild = open_db_res(&f, pfake, "dir/a.db/sub.db");
    CHECK(strcmp(err_case_name(&f, f.result_database, ochild), "SqlFailed") == 0);
    mo_value_drop(f.heap, f.result_database, ochild);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_SQLITE], pfake);

    static const char *const bad_keys[] = {"./a.db", "a.db/", "", "."};
    for (size_t i = 0; i < sizeof bad_keys / sizeof bad_keys[0]; i++) {
        struct mo_cell bk = host_text(&f, bad_keys[i]), bv, bm, out_b = mo_cell_int(0);
        MO_ASSERT(mo_value_bytes(f.heap, NULL, 0, &bv));
        MO_ASSERT(mo_value_map(f.heap, f.types.databases, &bk, &bv, 1, &bm));
        mo_value_drop(f.heap, f.types.text, bk);
        mo_value_drop(f.heap, f.types.bytes, bv);
        struct mo_host_fake_state bstate = {.kind = MO_CAP_SQLITE, .as.sqlite = bm};
        CHECK(mo_host_fake_restore(&f.host, f.heap, f.capabilities[MO_CAP_SQLITE], &bstate, &out_b,
                                   &error) == MO_HOST_BUG);
        mo_value_drop(f.heap, f.types.databases, bm);
    }
    struct mo_cell ck[2] = {host_text(&f, "a"), host_text(&f, "a/b")};
    struct mo_cell cv[2], cm, out_c = mo_cell_int(0);
    MO_ASSERT(mo_value_bytes(f.heap, NULL, 0, &cv[0]) && mo_value_bytes(f.heap, NULL, 0, &cv[1]));
    MO_ASSERT(mo_value_map(f.heap, f.types.databases, ck, cv, 2, &cm));
    for (unsigned i = 0; i < 2; i++) {
        mo_value_drop(f.heap, f.types.text, ck[i]);
        mo_value_drop(f.heap, f.types.bytes, cv[i]);
    }
    struct mo_host_fake_state cstate = {.kind = MO_CAP_SQLITE, .as.sqlite = cm};
    CHECK(mo_host_fake_restore(&f.host, f.heap, f.capabilities[MO_CAP_SQLITE], &cstate, &out_c,
                               &error) == MO_HOST_BUG);
    mo_value_drop(f.heap, f.types.databases, cm);

    CHECK(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
}

static void test_ephemeral_memory(void) {
    char template[] = "/tmp/mo-sqlite-mem-XXXXXX";
    char *dir = mkdtemp(template);
    MO_ASSERT(dir != NULL);

    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    struct mo_cell sq = make_real_sqlite(&f, dir);

    struct mo_cell o1 = open_db_res(&f, sq, ":memory:");
    struct mo_cell db1 = unwrap_ok(&f, f.result_database, o1);
    struct mo_cell o2 = open_db_res(&f, sq, ":memory:");
    struct mo_cell db2 = unwrap_ok(&f, f.result_database, o2);

    struct mo_cell c1 = run_sql_res(&f, db1, "CREATE TABLE eph(v INTEGER)", empty_values(&f));
    expect_printed(&f, f.result_sqlite_unit, c1, "Ok(Unit)");
    mo_value_drop(f.heap, f.result_sqlite_unit, c1);

    struct mo_cell v1[] = {val_int(&f, 42)};
    struct mo_cell i1 = run_sql_res(&f, db1, "INSERT INTO eph VALUES (?)", make_list(&f, v1, 1));
    expect_printed(&f, f.result_sqlite_unit, i1, "Ok(Unit)");
    mo_value_drop(f.heap, f.result_sqlite_unit, i1);

    struct mo_cell q1 = query_sql_res(&f, db1, "SELECT v FROM eph", empty_values(&f));
    expect_printed(&f, f.result_sqlite_rows, q1,
                   "Ok(Sqlite.Rows(columns: [\"v\"], rows: [[Integer(42)]]))");
    mo_value_drop(f.heap, f.result_sqlite_rows, q1);

    /* Second :memory: open on the same real Sqlite capability is isolated. */
    struct mo_cell q2 = query_sql_res(&f, db2, "SELECT name FROM sqlite_master WHERE type='table'",
                                      empty_values(&f));
    expect_printed(&f, f.result_sqlite_rows, q2, "Ok(Sqlite.Rows(columns: [\"name\"], rows: []))");
    mo_value_drop(f.heap, f.result_sqlite_rows, q2);

    /* Authorizer rules still hold on ephemeral databases. */
    struct mo_cell bad_att = run_sql_res(&f, db1, "ATTACH 'x.db' AS x", empty_values(&f));
    CHECK(strcmp(err_case_name(&f, f.result_sqlite_unit, bad_att), "NotPermitted") == 0);
    mo_value_drop(f.heap, f.result_sqlite_unit, bad_att);

    /* No :memory: file or sidecar was created in the folder on disk. */
    DIR *dp = opendir(dir);
    MO_ASSERT(dp != NULL);
    size_t entries = 0;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0)
            entries++;
    }
    (void)closedir(dp);
    CHECK(entries == 0);

    mo_value_drop(f.heap, f.result_database, o1);
    mo_value_drop(f.heap, f.result_database, o2);
    CHECK(f.host.open_databases == 0);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_SQLITE], sq);

    /* Fake Sqlite also treats :memory: as private and ephemeral without snapshotting it. */
    struct mo_cell fake = call_op(&f, MO_HOST_SQLITE_FAKE, NULL);
    struct mo_cell fo1 = open_db_res(&f, fake, ":memory:");
    struct mo_cell fdb1 = unwrap_ok(&f, f.result_database, fo1);
    struct mo_cell fo2 = open_db_res(&f, fake, ":memory:");
    struct mo_cell fdb2 = unwrap_ok(&f, f.result_database, fo2);
    struct mo_cell fc1 = run_sql_res(&f, fdb1, "CREATE TABLE only_one(x INT)", empty_values(&f));
    mo_value_drop(f.heap, f.result_sqlite_unit, fc1);
    struct mo_cell fq2 = query_sql_res(
        &f, fdb2, "SELECT name FROM sqlite_master WHERE type='table'", empty_values(&f));
    expect_printed(&f, f.result_sqlite_rows, fq2, "Ok(Sqlite.Rows(columns: [\"name\"], rows: []))");
    mo_value_drop(f.heap, f.result_sqlite_rows, fq2);
    struct mo_host_fake_state snap = {0};
    MO_ASSERT(mo_host_fake_snapshot(&f.host, f.heap, fake, &snap));
    uint32_t pos = 0;
    struct mo_cell sk, sv;
    CHECK(!mo_table_next(snap.as.sqlite, &pos, &sk, &sv));
    mo_value_drop(f.heap, f.types.databases, snap.as.sqlite);
    mo_value_drop(f.heap, f.result_database, fo1);
    mo_value_drop(f.heap, f.result_database, fo2);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_SQLITE], fake);

    CHECK(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
    remove_tree(dir);
}

struct wal_worker_job {
    const char *dir;
    int worker_id;
    bool ok;
};

static void *wal_worker_run(void *arg) {
    struct wal_worker_job *job = arg;
    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    struct mo_cell sq = make_real_sqlite(&f, job->dir);
    struct mo_cell opened = open_db_res(&f, sq, "concurrent.db");
    if (mo_cell_get_tag(opened) != 0) {
        mo_value_drop(f.heap, f.result_database, opened);
        mo_value_drop(f.heap, f.capabilities[MO_CAP_SQLITE], sq);
        host_fixture_free(&f);
        return NULL;
    }
    struct mo_cell db = unwrap_ok(&f, f.result_database, opened);
    struct mo_cell cr = run_sql_res(
        &f, db, "CREATE TABLE IF NOT EXISTS hits(id INTEGER PRIMARY KEY AUTOINCREMENT, w INT)",
        empty_values(&f));
    bool cr_ok = mo_cell_get_tag(cr) == 0;
    mo_value_drop(f.heap, f.result_sqlite_unit, cr);
    bool ins_ok = false;
    if (cr_ok) {
        struct mo_cell wval[] = {val_int(&f, job->worker_id)};
        struct mo_cell ins =
            run_sql_res(&f, db, "INSERT INTO hits(w) VALUES (?)", make_list(&f, wval, 1));
        ins_ok = mo_cell_get_tag(ins) == 0;
        mo_value_drop(f.heap, f.result_sqlite_unit, ins);
    }
    mo_value_drop(f.heap, f.result_database, opened);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_SQLITE], sq);
    host_fixture_free(&f);
    job->ok = cr_ok && ins_ok;
    return NULL;
}

static void test_concurrent_wal_open(void) {
    for (int round = 0; round < 5; round++) {
        char template[] = "/tmp/mo-sqlite-wal-XXXXXX";
        char *dir = mkdtemp(template);
        MO_ASSERT(dir != NULL);

        enum { NWORKERS = 8 };
        pthread_t tids[NWORKERS];
        struct wal_worker_job jobs[NWORKERS];
        for (int i = 0; i < NWORKERS; i++) {
            jobs[i] = (struct wal_worker_job){.dir = dir, .worker_id = i + 1, .ok = false};
            MO_ASSERT(pthread_create(&tids[i], NULL, wal_worker_run, &jobs[i]) == 0);
        }
        for (int i = 0; i < NWORKERS; i++) {
            MO_ASSERT(pthread_join(tids[i], NULL) == 0);
            CHECK(jobs[i].ok);
        }

        struct host_fixture f;
        host_fixture_init(&f, mo_limits_default().heap_size);
        struct mo_cell sq = make_real_sqlite(&f, dir);
        struct mo_cell opened = open_db_res(&f, sq, "concurrent.db");
        struct mo_cell db = unwrap_ok(&f, f.result_database, opened);
        struct mo_cell q = query_sql_res(&f, db, "SELECT COUNT(*) FROM hits", empty_values(&f));
        expect_printed(&f, f.result_sqlite_rows, q,
                       "Ok(Sqlite.Rows(columns: [\"COUNT(*)\"], rows: [[Integer(8)]]))");
        mo_value_drop(f.heap, f.result_sqlite_rows, q);
        mo_value_drop(f.heap, f.result_database, opened);
        mo_value_drop(f.heap, f.capabilities[MO_CAP_SQLITE], sq);
        host_fixture_free(&f);
        remove_tree(dir);
    }
}

int main(void) {
    test_real_crud();
    test_ephemeral_memory();
    test_concurrent_wal_open();
    test_real_errors();
    test_containment();
    test_authorizer();
    test_open_limit();
    test_fake_sqlite();
    return mo_test_report("test_sqlite");
}
