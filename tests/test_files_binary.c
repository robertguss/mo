#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#include "host/host.h"
#include "host_fixture.h"
#include "test.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static struct mo_cell make_bytes(struct host_fixture *f, const uint8_t *data, size_t len) {
    struct mo_cell out;
    MO_ASSERT(mo_value_bytes(f->heap, len == 0 ? (const uint8_t *)"" : data, len, &out));
    return out;
}

static void check_printed(struct host_fixture *f, uint32_t type, struct mo_cell value,
                          const char *expected) {
    struct mo_buffer printed;
    mo_buffer_init(&printed, MO_LIMIT_STRING_SIZE);
    CHECK(mo_value_print(f->heap, type, value, &printed));
    bool eq =
        printed.length == strlen(expected) && memcmp(printed.bytes, expected, printed.length) == 0;
    if (!eq)
        CHECK(fprintf(stderr, "expected %s, got %.*s\n", expected, (int)printed.length,
                      printed.bytes) >= 0);
    CHECK(eq);
    mo_buffer_free(&printed);
}

static struct mo_cell call_ok(struct host_fixture *f, uint32_t op, const struct mo_cell *args) {
    struct mo_cell out = mo_cell_int(77);
    struct mo_host_error error = {0};
    enum mo_host_outcome outcome =
        mo_host_call(&f->host, f->heap, op, host_result_type(f, op), args, &out, &error);
    CHECK(outcome == MO_HOST_RETURNED);
    MO_ASSERT(outcome == MO_HOST_RETURNED);
    return out;
}

static void check_bug(struct host_fixture *f, uint32_t op, const struct mo_cell *args,
                      const char *expected_reason) {
    struct mo_cell out = mo_cell_int(77);
    struct mo_host_error error = {0};
    enum mo_host_outcome outcome =
        mo_host_call(&f->host, f->heap, op, host_result_type(f, op), args, &out, &error);
    CHECK(outcome == MO_HOST_BUG);
    CHECK(mo_cell_get_int(out) == 77);
    CHECK(error.reason != NULL && strcmp(error.reason, expected_reason) == 0);
}

static void check_bytes_result(struct host_fixture *f, struct mo_cell res, const uint8_t *want,
                               size_t want_len) {
    CHECK(mo_cell_get_tag(res) == 0);
    if (mo_cell_get_tag(res) == 0) {
        size_t got_len = 0;
        const uint8_t *got = mo_value_data(mo_cell_get_payload(res), &got_len);
        CHECK(got_len == want_len && (want_len == 0 || memcmp(got, want, want_len) == 0));
    }
    mo_value_drop(f->heap, f->result_bytes, res);
}

static void check_int_result(struct host_fixture *f, struct mo_cell res, int64_t want) {
    CHECK(mo_cell_get_tag(res) == 0);
    if (mo_cell_get_tag(res) == 0)
        CHECK(mo_cell_get_int(mo_cell_get_payload(res)) == want);
    mo_value_drop(f->heap, f->result_int, res);
}

static struct mo_cell open_file_ok(struct host_fixture *f, struct mo_cell files, const char *path) {
    struct mo_cell p = host_text(f, path);
    struct mo_cell args[] = {files, p};
    struct mo_cell res = call_ok(f, MO_HOST_FILES_OPEN, args);
    mo_value_drop(f->heap, f->types.text, p);
    CHECK(mo_cell_get_tag(res) == 0);
    MO_ASSERT(mo_cell_get_tag(res) == 0);
    struct mo_cell fh =
        mo_value_dup(f->heap, f->capabilities[MO_CAP_FILE], mo_cell_get_payload(res));
    mo_value_drop(f->heap, f->result_file, res);
    return fh;
}

static void check_error_call(struct host_fixture *f, uint32_t op, const struct mo_cell *args,
                             const char *expected) {
    struct mo_cell res = call_ok(f, op, args);
    check_printed(f, host_result_type(f, op), res, expected);
    mo_value_drop(f->heap, host_result_type(f, op), res);
}

static void clean_dir_tree(int dir_fd) {
    int copy = openat(dir_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    MO_ASSERT(copy >= 0);
    DIR *dir = fdopendir(copy);
    MO_ASSERT(dir != NULL);
    for (;;) {
        errno = 0;
        struct dirent *ent = readdir(dir);
        if (ent == NULL) {
            MO_ASSERT(errno == 0);
            break;
        }
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        struct stat st;
        MO_ASSERT(fstatat(dir_fd, ent->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0);
        if (S_ISDIR(st.st_mode)) {
            int sub = openat(dir_fd, ent->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            MO_ASSERT(sub >= 0);
            clean_dir_tree(sub);
            MO_ASSERT(close(sub) == 0);
            MO_ASSERT(unlinkat(dir_fd, ent->d_name, AT_REMOVEDIR) == 0);
        } else {
            MO_ASSERT(unlinkat(dir_fd, ent->d_name, 0) == 0);
        }
    }
    MO_ASSERT(closedir(dir) == 0);
}

static int count_open_fds(void) {
#if defined(__linux__)
    DIR *dir = opendir("/proc/self/fd");
    if (dir == NULL)
        return -1;
    int count = 0;
    while (readdir(dir) != NULL)
        count++;
    MO_ASSERT(closedir(dir) == 0);
    return count;
#else
    return -1;
#endif
}

static void make_files_cap(struct host_fixture *f, bool is_fake, bool force_fallback, char *tmp_out,
                           int *dir_fd_out, struct mo_cell *files_out) {
    host_fixture_init(f, 32 << 20);
    f->host.force_fallback = force_fallback;
    if (is_fake) {
        *dir_fd_out = -1;
        struct mo_cell empty_map;
        MO_ASSERT(mo_value_map(f->heap, f->types.files, NULL, NULL, 0, &empty_map));
        struct mo_host_error error = {0};
        MO_ASSERT(mo_host_call(&f->host, f->heap, MO_HOST_FILES_IN_MEMORY,
                               f->capabilities[MO_CAP_FILES], &empty_map, files_out,
                               &error) == MO_HOST_RETURNED);
    } else {
        memcpy(tmp_out, "/tmp/mo-files-bin-XXXXXX", 25);
        MO_ASSERT(mkdtemp(tmp_out) != NULL);
        int fd = open(tmp_out, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        MO_ASSERT(fd >= 0);
        *dir_fd_out = fd;
        struct mo_host_error error = {0};
        MO_ASSERT(mo_host_real(&f->host, f->heap, f->capabilities[MO_CAP_FILES], NULL, fd,
                               files_out, &error) == MO_HOST_RETURNED);
    }
}

static void free_files_cap(struct host_fixture *f, struct mo_cell files, const char *tmp_dir,
                           int dir_fd) {
    mo_value_drop(f->heap, f->capabilities[MO_CAP_FILES], files);
    CHECK(mo_heap_objects(f->heap) == 0 && f->host.open_files == 0 && f->host.open_folders == 0);
    host_fixture_free(f);
    if (dir_fd >= 0) {
        clean_dir_tree(dir_fd);
        MO_ASSERT(close(dir_fd) == 0);
        MO_ASSERT(rmdir(tmp_dir) == 0);
    }
}

static void test_parity_core(bool is_fake, bool force_fallback) {
    struct host_fixture f;
    char tmp[32] = {0};
    int dir_fd = -1;
    struct mo_cell files;
    make_files_cap(&f, is_fake, force_fallback, tmp, &dir_fd, &files);

    /* write_bytes with non-UTF-8 raw bytes (NUL, 0xff, overlong UTF-8) */
    const uint8_t raw[] = {0x00, 0xff, 0xc0, 0x80, 0xfe, 0x01};
    struct mo_cell p_bin = host_text(&f, "raw.bin");
    struct mo_cell b_raw = make_bytes(&f, raw, sizeof raw);
    struct mo_cell w_args[] = {files, p_bin, b_raw};
    check_error_call(&f, MO_HOST_FILES_WRITE_BYTES, w_args, "Ok(Unit)");

    struct mo_cell r_args[] = {files, p_bin};
    check_bytes_result(&f, call_ok(&f, MO_HOST_FILES_READ_BYTES, r_args), raw, sizeof raw);
    check_error_call(&f, MO_HOST_FILES_READ_TEXT, r_args, "Error(NotText(path: \"raw.bin\"))");

    /* Overwrite existing with shorter and then empty bytes */
    const uint8_t short_raw[] = {0xde, 0xad};
    struct mo_cell b_short = make_bytes(&f, short_raw, sizeof short_raw);
    struct mo_cell w_short[] = {files, p_bin, b_short};
    check_error_call(&f, MO_HOST_FILES_WRITE_BYTES, w_short, "Ok(Unit)");
    check_bytes_result(&f, call_ok(&f, MO_HOST_FILES_READ_BYTES, r_args), short_raw,
                       sizeof short_raw);

    struct mo_cell b_empty = make_bytes(&f, NULL, 0);
    struct mo_cell w_empty[] = {files, p_bin, b_empty};
    check_error_call(&f, MO_HOST_FILES_WRITE_BYTES, w_empty, "Ok(Unit)");
    check_bytes_result(&f, call_ok(&f, MO_HOST_FILES_READ_BYTES, r_args), NULL, 0);

    /* Restore raw bytes for rename tests */
    check_error_call(&f, MO_HOST_FILES_WRITE_BYTES, w_args, "Ok(Unit)");

    /* write_bytes error paths: missing parent, folder, escape */
    struct mo_cell p_missing_dir = host_text(&f, "nope/x.bin");
    struct mo_cell w_miss[] = {files, p_missing_dir, b_raw};
    check_error_call(&f, MO_HOST_FILES_WRITE_BYTES, w_miss,
                     "Error(NotFound(path: \"nope/x.bin\"))");

    struct mo_cell p_dot = host_text(&f, ".");
    struct mo_cell w_dot[] = {files, p_dot, b_raw};
    check_error_call(&f, MO_HOST_FILES_WRITE_BYTES, w_dot,
                     "Error(Failed(path: \".\", reason: \"is a folder\"))");

    struct mo_cell p_esc = host_text(&f, "../escape.bin");
    struct mo_cell w_esc[] = {files, p_esc, b_raw};
    check_error_call(&f, MO_HOST_FILES_WRITE_BYTES, w_esc,
                     "Error(NotAllowed(path: \"../escape.bin\"))");

    struct mo_cell p_abs = host_text(&f, "/tmp/nope");
    struct mo_cell w_abs[] = {files, p_abs, b_raw};
    check_error_call(&f, MO_HOST_FILES_WRITE_BYTES, w_abs,
                     "Error(NotAllowed(path: \"/tmp/nope\"))");

    /* Folders + cross-folder rename, overwrite, same-path, and error paths */
    struct mo_cell p_sub1 = host_text(&f, "sub1");
    struct mo_cell p_sub2 = host_text(&f, "sub2");
    struct mo_cell mk1[] = {files, p_sub1};
    struct mo_cell mk2[] = {files, p_sub2};
    check_error_call(&f, MO_HOST_FILES_MAKE_FOLDER, mk1, "Ok(Unit)");
    check_error_call(&f, MO_HOST_FILES_MAKE_FOLDER, mk2, "Ok(Unit)");

    struct mo_cell p_a = host_text(&f, "sub1/a.bin");
    struct mo_cell p_b = host_text(&f, "sub2/b.bin");
    struct mo_cell ren1[] = {files, p_bin, p_a};
    check_error_call(&f, MO_HOST_FILES_RENAME, ren1, "Ok(Unit)");
    check_error_call(&f, MO_HOST_FILES_READ_BYTES, r_args, "Error(NotFound(path: \"raw.bin\"))");
    struct mo_cell r_a[] = {files, p_a};
    check_bytes_result(&f, call_ok(&f, MO_HOST_FILES_READ_BYTES, r_a), raw, sizeof raw);

    /* Atomic overwrite existing dest */
    struct mo_cell w_b[] = {files, p_b, b_short};
    check_error_call(&f, MO_HOST_FILES_WRITE_BYTES, w_b, "Ok(Unit)");
    struct mo_cell ren2[] = {files, p_a, p_b};
    check_error_call(&f, MO_HOST_FILES_RENAME, ren2, "Ok(Unit)");
    check_error_call(&f, MO_HOST_FILES_READ_BYTES, r_a, "Error(NotFound(path: \"sub1/a.bin\"))");
    struct mo_cell r_b[] = {files, p_b};
    check_bytes_result(&f, call_ok(&f, MO_HOST_FILES_READ_BYTES, r_b), raw, sizeof raw);

    /* Same-path rename preserves file */
    struct mo_cell ren_same[] = {files, p_b, p_b};
    check_error_call(&f, MO_HOST_FILES_RENAME, ren_same, "Ok(Unit)");
    check_bytes_result(&f, call_ok(&f, MO_HOST_FILES_READ_BYTES, r_b), raw, sizeof raw);

    /* Rename error paths */
    struct mo_cell ren_missing[] = {files, p_a, p_b};
    check_error_call(&f, MO_HOST_FILES_RENAME, ren_missing,
                     "Error(NotFound(path: \"sub1/a.bin\"))");
    struct mo_cell ren_miss_dst[] = {files, p_b, p_missing_dir};
    check_error_call(&f, MO_HOST_FILES_RENAME, ren_miss_dst,
                     "Error(NotFound(path: \"nope/x.bin\"))");
    struct mo_cell ren_dir_src[] = {files, p_sub1, p_b};
    check_error_call(&f, MO_HOST_FILES_RENAME, ren_dir_src,
                     "Error(Failed(path: \"sub1\", reason: \"is a folder\"))");
    struct mo_cell ren_dir_dst[] = {files, p_b, p_sub1};
    check_error_call(&f, MO_HOST_FILES_RENAME, ren_dir_dst,
                     "Error(Failed(path: \"sub1\", reason: \"is a folder\"))");
    struct mo_cell ren_esc_from[] = {files, p_esc, p_b};
    check_error_call(&f, MO_HOST_FILES_RENAME, ren_esc_from,
                     "Error(NotAllowed(path: \"../escape.bin\"))");
    struct mo_cell ren_esc_to[] = {files, p_b, p_esc};
    check_error_call(&f, MO_HOST_FILES_RENAME, ren_esc_to,
                     "Error(NotAllowed(path: \"../escape.bin\"))");

    /* Narrowed within("..") poisons write_bytes, rename, and open */
    struct mo_cell p_up = host_text(&f, "..");
    struct mo_cell within_args[] = {files, p_up};
    struct mo_cell poisoned = call_ok(&f, MO_HOST_FILES_WITHIN, within_args);
    struct mo_cell pw[] = {poisoned, p_bin, b_raw};
    check_error_call(&f, MO_HOST_FILES_WRITE_BYTES, pw, "Error(NotAllowed(path: \"raw.bin\"))");
    struct mo_cell pr[] = {poisoned, p_b, p_a};
    check_error_call(&f, MO_HOST_FILES_RENAME, pr, "Error(NotAllowed(path: \"sub2/b.bin\"))");
    struct mo_cell po[] = {poisoned, p_bin};
    check_error_call(&f, MO_HOST_FILES_OPEN, po, "Error(NotAllowed(path: \"raw.bin\"))");
    mo_value_drop(f.heap, f.capabilities[MO_CAP_FILES], poisoned);

    /* Files.open error paths */
    struct mo_cell open_miss[] = {files, p_missing_dir};
    check_error_call(&f, MO_HOST_FILES_OPEN, open_miss, "Error(NotFound(path: \"nope/x.bin\"))");
    struct mo_cell open_dir[] = {files, p_sub1};
    check_error_call(&f, MO_HOST_FILES_OPEN, open_dir,
                     "Error(Failed(path: \"sub1\", reason: \"is a folder\"))");
    struct mo_cell open_esc[] = {files, p_esc};
    check_error_call(&f, MO_HOST_FILES_OPEN, open_esc,
                     "Error(NotAllowed(path: \"../escape.bin\"))");

    /* Files.open creates new file without truncating existing, full random-access lifecycle */
    struct mo_cell fh = open_file_ok(&f, files, "pages.db");
    CHECK(f.host.open_files == 1);
    struct mo_cell sz_args[] = {fh};
    check_int_result(&f, call_ok(&f, MO_HOST_FILES_SIZE, sz_args), 0);

    /* Empty read and read at offset 0 on empty file */
    struct mo_cell rd_empty[] = {fh, mo_cell_int(0), mo_cell_int(16)};
    check_bytes_result(&f, call_ok(&f, MO_HOST_FILES_READ_AT, rd_empty), NULL, 0);

    /* Empty write past EOF leaves size at 0 (matches POSIX pwrite) */
    struct mo_cell wr_empty_past[] = {fh, mo_cell_int(100), b_empty};
    check_error_call(&f, MO_HOST_FILES_WRITE_AT, wr_empty_past, "Ok(Unit)");
    check_int_result(&f, call_ok(&f, MO_HOST_FILES_SIZE, sz_args), 0);

    /* Write at offset 0, then write past EOF verifying zero-filled gap */
    const uint8_t head[] = {0x11, 0x22, 0x33, 0x44};
    struct mo_cell b_head = make_bytes(&f, head, sizeof head);
    struct mo_cell wr0[] = {fh, mo_cell_int(0), b_head};
    check_error_call(&f, MO_HOST_FILES_WRITE_AT, wr0, "Ok(Unit)");
    check_int_result(&f, call_ok(&f, MO_HOST_FILES_SIZE, sz_args), 4);

    const uint8_t tail[] = {0xaa, 0xbb};
    struct mo_cell b_tail = make_bytes(&f, tail, sizeof tail);
    struct mo_cell wr_gap[] = {fh, mo_cell_int(8), b_tail};
    check_error_call(&f, MO_HOST_FILES_WRITE_AT, wr_gap, "Ok(Unit)");
    check_int_result(&f, call_ok(&f, MO_HOST_FILES_SIZE, sz_args), 10);

    const uint8_t expected_gap[10] = {0x11, 0x22, 0x33, 0x44, 0x00, 0x00, 0x00, 0x00, 0xaa, 0xbb};
    struct mo_cell rd_all[] = {fh, mo_cell_int(0), mo_cell_int(10)};
    check_bytes_result(&f, call_ok(&f, MO_HOST_FILES_READ_AT, rd_all), expected_gap, 10);

    /* Short read across EOF, read at EOF, past EOF, INT64_MAX, and count == 0 */
    struct mo_cell rd_short[] = {fh, mo_cell_int(7), mo_cell_int(100)};
    check_bytes_result(&f, call_ok(&f, MO_HOST_FILES_READ_AT, rd_short), expected_gap + 7, 3);
    struct mo_cell rd_eof[] = {fh, mo_cell_int(10), mo_cell_int(10)};
    check_bytes_result(&f, call_ok(&f, MO_HOST_FILES_READ_AT, rd_eof), NULL, 0);
    struct mo_cell rd_past[] = {fh, mo_cell_int(999), mo_cell_int(10)};
    check_bytes_result(&f, call_ok(&f, MO_HOST_FILES_READ_AT, rd_past), NULL, 0);
    struct mo_cell rd_max[] = {fh, mo_cell_int(INT64_MAX), mo_cell_int(10)};
    check_bytes_result(&f, call_ok(&f, MO_HOST_FILES_READ_AT, rd_max), NULL, 0);
    struct mo_cell rd_zero[] = {fh, mo_cell_int(2), mo_cell_int(0)};
    check_bytes_result(&f, call_ok(&f, MO_HOST_FILES_READ_AT, rd_zero), NULL, 0);

    /* Overwrite middle without changing size */
    const uint8_t mid[] = {0x55, 0x66};
    struct mo_cell b_mid = make_bytes(&f, mid, sizeof mid);
    struct mo_cell wr_mid[] = {fh, mo_cell_int(4), b_mid};
    check_error_call(&f, MO_HOST_FILES_WRITE_AT, wr_mid, "Ok(Unit)");
    check_int_result(&f, call_ok(&f, MO_HOST_FILES_SIZE, sz_args), 10);
    const uint8_t after_mid[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x00, 0x00, 0xaa, 0xbb};
    check_bytes_result(&f, call_ok(&f, MO_HOST_FILES_READ_AT, rd_all), after_mid, 10);

    /* Truncate shrink, then grow (zero-filled), then sync */
    struct mo_cell tr_shrink[] = {fh, mo_cell_int(5)};
    check_error_call(&f, MO_HOST_FILES_TRUNCATE, tr_shrink, "Ok(Unit)");
    check_int_result(&f, call_ok(&f, MO_HOST_FILES_SIZE, sz_args), 5);
    struct mo_cell tr_grow[] = {fh, mo_cell_int(8)};
    check_error_call(&f, MO_HOST_FILES_TRUNCATE, tr_grow, "Ok(Unit)");
    check_int_result(&f, call_ok(&f, MO_HOST_FILES_SIZE, sz_args), 8);
    const uint8_t after_grow[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x00, 0x00, 0x00};
    struct mo_cell rd_8[] = {fh, mo_cell_int(0), mo_cell_int(8)};
    check_bytes_result(&f, call_ok(&f, MO_HOST_FILES_READ_AT, rd_8), after_grow, 8);
    check_error_call(&f, MO_HOST_FILES_SYNC, sz_args, "Ok(Unit)");

    /* Bug checks on Files.File */
    struct mo_cell neg_rd_off[] = {fh, mo_cell_int(-1), mo_cell_int(4)};
    check_bug(&f, MO_HOST_FILES_READ_AT, neg_rd_off, "negative file offset");
    struct mo_cell neg_rd_cnt[] = {fh, mo_cell_int(0), mo_cell_int(-1)};
    check_bug(&f, MO_HOST_FILES_READ_AT, neg_rd_cnt, "read count outside 0 to the read limit");
    struct mo_cell big_rd_cnt[] = {fh, mo_cell_int(0),
                                   mo_cell_int((int64_t)MO_LIMIT_FILE_IO_BYTES + 1)};
    check_bug(&f, MO_HOST_FILES_READ_AT, big_rd_cnt, "read count outside 0 to the read limit");
    struct mo_cell neg_wr_off[] = {fh, mo_cell_int(-1), b_head};
    check_bug(&f, MO_HOST_FILES_WRITE_AT, neg_wr_off, "negative file offset");
    struct mo_cell neg_tr[] = {fh, mo_cell_int(-1)};
    check_bug(&f, MO_HOST_FILES_TRUNCATE, neg_tr, "negative file size");

    /* Drop handle, re-open existing without truncating, verify persistence and truncate(0) */
    mo_value_drop(f.heap, f.capabilities[MO_CAP_FILE], fh);
    CHECK(f.host.open_files == 0);
    struct mo_cell p_pages = host_text(&f, "pages.db");
    struct mo_cell r_pages[] = {files, p_pages};
    check_bytes_result(&f, call_ok(&f, MO_HOST_FILES_READ_BYTES, r_pages), after_grow, 8);

    fh = open_file_ok(&f, files, "pages.db");
    struct mo_cell sz_args2[] = {fh};
    check_int_result(&f, call_ok(&f, MO_HOST_FILES_SIZE, sz_args2), 8);
    struct mo_cell tr_zero[] = {fh, mo_cell_int(0)};
    check_error_call(&f, MO_HOST_FILES_TRUNCATE, tr_zero, "Ok(Unit)");
    check_int_result(&f, call_ok(&f, MO_HOST_FILES_SIZE, sz_args2), 0);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_FILE], fh);

    mo_value_drop(f.heap, f.types.text, p_pages);
    mo_value_drop(f.heap, f.types.bytes, b_mid);
    mo_value_drop(f.heap, f.types.bytes, b_tail);
    mo_value_drop(f.heap, f.types.bytes, b_head);
    mo_value_drop(f.heap, f.types.text, p_up);
    mo_value_drop(f.heap, f.types.text, p_b);
    mo_value_drop(f.heap, f.types.text, p_a);
    mo_value_drop(f.heap, f.types.text, p_sub2);
    mo_value_drop(f.heap, f.types.text, p_sub1);
    mo_value_drop(f.heap, f.types.text, p_abs);
    mo_value_drop(f.heap, f.types.text, p_esc);
    mo_value_drop(f.heap, f.types.text, p_dot);
    mo_value_drop(f.heap, f.types.text, p_missing_dir);
    mo_value_drop(f.heap, f.types.bytes, b_empty);
    mo_value_drop(f.heap, f.types.bytes, b_short);
    mo_value_drop(f.heap, f.types.bytes, b_raw);
    mo_value_drop(f.heap, f.types.text, p_bin);
    free_files_cap(&f, files, tmp, dir_fd);
}

static void test_real_strict_confinement(bool force_fallback) {
    char base_path[] = "/tmp/mo-files-confine-XXXXXX";
    MO_ASSERT(mkdtemp(base_path) != NULL);
    int base = open(base_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    MO_ASSERT(base >= 0);
    MO_ASSERT(mkdirat(base, "root", 0700) == 0 && mkdirat(base, "outside", 0700) == 0);
    int root_fd = openat(base, "root", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int outside_fd = openat(base, "outside", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    MO_ASSERT(root_fd >= 0 && outside_fd >= 0);

    int secret = openat(outside_fd, "secret", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    MO_ASSERT(secret >= 0 && write(secret, "keepme", 6) == 6);
    int inside = openat(root_fd, "target.bin", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    MO_ASSERT(inside >= 0 && write(inside, "orig", 4) == 4 && close(inside) == 0);
    MO_ASSERT(mkdirat(root_fd, "real_dir", 0700) == 0);
    MO_ASSERT(symlinkat("target.bin", root_fd, "link_inside") == 0);
    MO_ASSERT(symlinkat("../outside/secret", root_fd, "link_escape") == 0);
    MO_ASSERT(symlinkat("/etc/hosts", root_fd, "link_abs") == 0);
    MO_ASSERT(symlinkat("real_dir", root_fd, "dir_link") == 0);
    MO_ASSERT(symlinkat("../outside", root_fd, "dir_escape") == 0);
    MO_ASSERT(mkfifoat(root_fd, "my_fifo", 0600) == 0);

    struct host_fixture f;
    host_fixture_init(&f, 16 << 20);
    f.host.force_fallback = force_fallback;
    struct mo_cell files;
    struct mo_host_error error = {0};
    MO_ASSERT(mo_host_real(&f.host, f.heap, f.capabilities[MO_CAP_FILES], NULL, root_fd, &files,
                           &error) == MO_HOST_RETURNED);

    const uint8_t payload[] = {0x01, 0x02, 0x03};
    struct mo_cell b_pay = make_bytes(&f, payload, sizeof payload);
    struct mo_cell p_ok = host_text(&f, "target.bin");
    const char *const sym_paths[] = {"link_inside", "link_escape", "link_abs", "dir_link/child",
                                     "dir_escape/secret"};
    for (size_t i = 0; i < sizeof sym_paths / sizeof sym_paths[0]; i++) {
        char want[128];
        int n = snprintf(want, sizeof want, "Error(NotAllowed(path: \"%s\"))", sym_paths[i]);
        MO_ASSERT(n > 0 && (size_t)n < sizeof want);
        struct mo_cell p = host_text(&f, sym_paths[i]);
        struct mo_cell w_args[] = {files, p, b_pay};
        check_error_call(&f, MO_HOST_FILES_WRITE_BYTES, w_args, want);
        struct mo_cell o_args[] = {files, p};
        check_error_call(&f, MO_HOST_FILES_OPEN, o_args, want);
        struct mo_cell r_from[] = {files, p, p_ok};
        check_error_call(&f, MO_HOST_FILES_RENAME, r_from, want);
        struct mo_cell r_to[] = {files, p_ok, p};
        check_error_call(&f, MO_HOST_FILES_RENAME, r_to, want);
        mo_value_drop(f.heap, f.types.text, p);
    }

    /* Embedded NUL rejected on real POSIX paths across write_bytes, open, and rename */
    struct mo_cell p_nul;
    MO_ASSERT(mo_value_text(f.heap, "a\0b", 3, &p_nul));
    const char *nul_err = "Error(NotAllowed(path: \"a\\u{0}b\"))";
    struct mo_cell wn[] = {files, p_nul, b_pay};
    check_error_call(&f, MO_HOST_FILES_WRITE_BYTES, wn, nul_err);
    struct mo_cell on[] = {files, p_nul};
    check_error_call(&f, MO_HOST_FILES_OPEN, on, nul_err);
    struct mo_cell rn1[] = {files, p_nul, p_ok};
    check_error_call(&f, MO_HOST_FILES_RENAME, rn1, nul_err);
    struct mo_cell rn2[] = {files, p_ok, p_nul};
    check_error_call(&f, MO_HOST_FILES_RENAME, rn2, nul_err);
    mo_value_drop(f.heap, f.types.text, p_nul);

    /* FIFO rejected as not a regular file across write_bytes, open, and rename */
    struct mo_cell p_fifo = host_text(&f, "my_fifo");
    const char *fifo_err = "Error(Failed(path: \"my_fifo\", reason: \"not a regular file\"))";
    struct mo_cell wf[] = {files, p_fifo, b_pay};
    check_error_call(&f, MO_HOST_FILES_WRITE_BYTES, wf, fifo_err);
    struct mo_cell of[] = {files, p_fifo};
    check_error_call(&f, MO_HOST_FILES_OPEN, of, fifo_err);
    struct mo_cell rf1[] = {files, p_fifo, p_ok};
    check_error_call(&f, MO_HOST_FILES_RENAME, rf1, fifo_err);
    struct mo_cell rf2[] = {files, p_ok, p_fifo};
    check_error_call(&f, MO_HOST_FILES_RENAME, rf2, fifo_err);

    /* Verify sentinel outside root and target.bin inside root were never modified */
    char check_buf[16];
    MO_ASSERT(pread(secret, check_buf, sizeof check_buf, 0) == 6 &&
              memcmp(check_buf, "keepme", 6) == 0);
    struct mo_cell r_ok[] = {files, p_ok};
    check_bytes_result(&f, call_ok(&f, MO_HOST_FILES_READ_BYTES, r_ok), (const uint8_t *)"orig", 4);

    mo_value_drop(f.heap, f.types.text, p_fifo);
    mo_value_drop(f.heap, f.types.text, p_ok);
    mo_value_drop(f.heap, f.types.bytes, b_pay);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_FILES], files);
    CHECK(mo_heap_objects(f.heap) == 0 && f.host.open_files == 0 && f.host.open_folders == 0);
    host_fixture_free(&f);

    clean_dir_tree(root_fd);
    MO_ASSERT(close(root_fd) == 0 && unlinkat(base, "root", AT_REMOVEDIR) == 0);
    MO_ASSERT(close(secret) == 0 && unlinkat(outside_fd, "secret", 0) == 0);
    MO_ASSERT(close(outside_fd) == 0 && unlinkat(base, "outside", AT_REMOVEDIR) == 0);
    MO_ASSERT(close(base) == 0 && rmdir(base_path) == 0);
}

static void test_open_file_limit(bool is_fake, bool force_fallback) {
    struct host_fixture f;
    char tmp[32] = {0};
    int dir_fd = -1;
    struct mo_cell files;
    int fd_baseline = is_fake ? -1 : count_open_fds();
    make_files_cap(&f, is_fake, force_fallback, tmp, &dir_fd, &files);

    struct mo_cell handles[MO_LIMIT_OPEN_FILES];
    for (unsigned i = 0; i < MO_LIMIT_OPEN_FILES; i++) {
        char name[32];
        int n = snprintf(name, sizeof name, "f%u.bin", i);
        MO_ASSERT(n > 0 && (size_t)n < sizeof name);
        handles[i] = open_file_ok(&f, files, name);
    }
    CHECK(f.host.open_files == MO_LIMIT_OPEN_FILES);

    /* 65th open stops with bug and does not create the file */
    struct mo_cell p_over = host_text(&f, "over_limit.bin");
    struct mo_cell over_args[] = {files, p_over};
    check_bug(&f, MO_HOST_FILES_OPEN, over_args, "too many files open at once");
    check_error_call(&f, MO_HOST_FILES_EXISTS, over_args, "false");

    /* Dropping one handle frees a slot */
    mo_value_drop(f.heap, f.capabilities[MO_CAP_FILE], handles[0]);
    CHECK(f.host.open_files == MO_LIMIT_OPEN_FILES - 1);
    handles[0] = open_file_ok(&f, files, "over_limit.bin");
    CHECK(f.host.open_files == MO_LIMIT_OPEN_FILES);

    for (unsigned i = 0; i < MO_LIMIT_OPEN_FILES; i++)
        mo_value_drop(f.heap, f.capabilities[MO_CAP_FILE], handles[i]);
    CHECK(f.host.open_files == 0);

    mo_value_drop(f.heap, f.types.text, p_over);
    free_files_cap(&f, files, tmp, dir_fd);
    if (!is_fake && fd_baseline >= 0)
        CHECK(count_open_fds() == fd_baseline);
}

static void test_fake_snapshot_restore(void) {
    struct host_fixture f;
    host_fixture_init(&f, 16 << 20);
    struct mo_cell k1 = host_text(&f, "hello.txt"), v1 = host_text(&f, "world"), init_map, files;
    MO_ASSERT(mo_value_map(f.heap, f.types.files, &k1, &v1, 1, &init_map));
    struct mo_host_error error = {0};
    MO_ASSERT(mo_host_call(&f.host, f.heap, MO_HOST_FILES_IN_MEMORY, f.capabilities[MO_CAP_FILES],
                           &init_map, &files, &error) == MO_HOST_RETURNED);

    struct mo_cell fh = open_file_ok(&f, files, "hello.txt");
    const uint8_t bang[] = {'!'};
    struct mo_cell b_bang = make_bytes(&f, bang, 1);
    struct mo_cell wr[] = {fh, mo_cell_int(5), b_bang};
    check_error_call(&f, MO_HOST_FILES_WRITE_AT, wr, "Ok(Unit)");
    mo_value_drop(f.heap, f.capabilities[MO_CAP_FILE], fh);

    struct mo_host_fake_state snap;
    MO_ASSERT(mo_host_fake_snapshot(&f.host, f.heap, files, &snap));
    struct mo_cell restored;
    MO_ASSERT(mo_host_fake_restore(&f.host, f.heap, f.capabilities[MO_CAP_FILES], &snap, &restored,
                                   &error) == MO_HOST_RETURNED);
    mo_host_fake_state_drop(&f.host, f.heap, &snap);

    struct mo_cell rd[] = {restored, k1};
    check_error_call(&f, MO_HOST_FILES_READ_TEXT, rd, "Ok(\"world!\")");

    mo_value_drop(f.heap, f.capabilities[MO_CAP_FILES], restored);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_FILES], files);
    mo_value_drop(f.heap, f.types.bytes, b_bang);
    mo_value_drop(f.heap, f.types.text, v1);
    mo_value_drop(f.heap, f.types.text, k1);
    CHECK(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
}

int main(void) {
    test_parity_core(false, false);
    test_parity_core(false, true);
    test_parity_core(true, false);
    test_real_strict_confinement(false);
    test_real_strict_confinement(true);
    test_open_file_limit(false, false);
    test_open_file_limit(false, true);
    test_open_file_limit(true, false);
    test_fake_snapshot_restore();
    return mo_test_report("files binary");
}
