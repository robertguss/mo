#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#include "host/host.h"
#include "host_fixture.h"
#include "test.h"
#include "vm/vm.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void test_console(struct heap_fixture *f) {
    struct mo_type *storage = calloc(f->types.count + MO_HOST_PRIVATE_TYPES, sizeof *storage);
    MO_ASSERT(storage != NULL);
    struct mo_host_types types;
    mo_host_prepare_types(&types, &f->types, storage);
    struct mo_limits limits = mo_limits_default();
    struct mo_heap_image *image = mo_heap_image_new(&types.types, &limits);
    MO_ASSERT(image != NULL);
    struct mo_heap *heap = mo_heap_new(image, &limits);
    MO_ASSERT(heap != NULL);
    struct mo_host host;
    mo_host_init(&host, &types);
    struct mo_host_error error;
    struct mo_cell console, text, result, snapshot;
    CHECK(mo_host_call(&host, heap, MO_HOST_CONSOLE_FAKE, f->capabilities[MO_CAP_CONSOLE], NULL,
                       &console, &error) == MO_HOST_RETURNED);
    CHECK(mo_value_text(heap, "a\0\xc3\xa9", 4, &text));
    struct mo_cell args[] = {console, text};
    CHECK(mo_host_call(&host, heap, MO_HOST_CONSOLE_PRINT, f->builtin[MO_TYPE_UNIT], args, &result,
                       &error) == MO_HOST_RETURNED);
    CHECK(mo_host_call(&host, heap, MO_HOST_CONSOLE_PRINTED, types.texts, args, &snapshot,
                       &error) == MO_HOST_RETURNED);
    CHECK(mo_host_call(&host, heap, MO_HOST_CONSOLE_PRINT, f->builtin[MO_TYPE_UNIT], args, &result,
                       &error) == MO_HOST_RETURNED);
    CHECK(mo_host_call(&host, heap, MO_HOST_CONSOLE_PRINTED, types.texts, args, &result, &error) ==
          MO_HOST_RETURNED);
    CHECK(mo_list_size(snapshot) == 1 && mo_list_size(result) == 2);
    struct mo_cell item;
    CHECK(mo_list_get(result, 1, &item));
    size_t length;
    CHECK(memcmp(mo_value_data(item, &length), "a\0\xc3\xa9", 4) == 0 && length == 4);
    mo_value_drop(heap, types.texts, result);
    mo_value_drop(heap, types.texts, snapshot);
    mo_value_drop(heap, types.text, text);
    mo_value_drop(heap, f->capabilities[MO_CAP_CONSOLE], console);
    CHECK(mo_heap_objects(heap) == 0);
    mo_heap_free(heap);
    mo_heap_image_free(image);
    free(storage);
}

static void expect(struct host_fixture *f, uint32_t operation, uint32_t type,
                   struct mo_cell capability, const char *path, const char *text,
                   const char *expected) {
    struct mo_cell args[] = {capability, host_text(f, path), host_text(f, text)};
    struct mo_host_error error;
    struct mo_cell result;
    enum mo_host_outcome outcome =
        mo_host_call(&f->host, f->heap, operation, type, args, &result, &error);
    CHECK(outcome == MO_HOST_RETURNED);
    if (outcome == MO_HOST_RETURNED) {
        struct mo_buffer printed;
        mo_buffer_init(&printed, MO_LIMIT_STRING_SIZE);
        CHECK(mo_value_print(f->heap, type, result, &printed));
        bool equal = printed.length == strlen(expected) &&
                     memcmp(printed.bytes, expected, printed.length) == 0;
        if (!equal)
            CHECK(fprintf(stderr, "expected %s, got %.*s\n", expected, (int)printed.length,
                          printed.bytes) >= 0);
        CHECK(equal);
        mo_buffer_free(&printed);
        mo_value_drop(f->heap, type, result);
    }
    mo_value_drop(f->heap, f->types.text, args[1]);
    mo_value_drop(f->heap, f->types.text, args[2]);
}

static void test_fake_restore(void) {
    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    struct mo_cell words[] = {host_text(&f, "first"), host_text(&f, "last"), host_text(&f, "old"),
                              host_text(&f, "error"), host_text(&f, "after")};
    struct mo_host_fake_state initial = {.kind = MO_CAP_CONSOLE}, snapshot;
    MO_ASSERT(mo_value_list(f.heap, f.types.texts, words, 2, &initial.as.console.input));
    MO_ASSERT(mo_value_list(f.heap, f.types.texts, words + 2, 1, &initial.as.console.output));
    MO_ASSERT(mo_value_list(f.heap, f.types.texts, words + 3, 1, &initial.as.console.errors));
    struct mo_cell console;
    struct mo_host_error error;
    uint32_t type = f.capabilities[MO_CAP_CONSOLE];
    MO_ASSERT(mo_host_fake_restore(&f.host, f.heap, type, &initial, &console, &error) ==
              MO_HOST_RETURNED);
    mo_host_fake_state_drop(&f.host, f.heap, &initial);
    expect(&f, MO_HOST_CONSOLE_READ_LINE, f.option, console, "", "", "Some(\"first\")");
    expect(&f, MO_HOST_CONSOLE_PRINT, f.unit, console, "after", "", "Unit");
    MO_ASSERT(mo_host_fake_snapshot(&f.host, f.heap, console, &snapshot));
    expect(&f, MO_HOST_CONSOLE_READ_LINE, f.option, console, "", "", "Some(\"last\")");
    expect(&f, MO_HOST_CONSOLE_PRINT, f.unit, console, "later", "", "Unit");
    struct mo_cell last, old, after, error_item;
    CHECK(mo_list_size(snapshot.as.console.input) == 1 &&
          mo_list_size(snapshot.as.console.output) == 2 &&
          mo_list_size(snapshot.as.console.errors) == 1);
    MO_ASSERT(mo_list_get(snapshot.as.console.input, 0, &last) &&
              mo_list_get(snapshot.as.console.output, 0, &old) &&
              mo_list_get(snapshot.as.console.output, 1, &after) &&
              mo_list_get(snapshot.as.console.errors, 0, &error_item));
    bool equal;
    CHECK(mo_value_equal(f.heap, f.types.text, words[1], last, &equal) && equal);
    CHECK(mo_value_equal(f.heap, f.types.text, words[2], old, &equal) && equal);
    CHECK(mo_value_equal(f.heap, f.types.text, words[3], error_item, &equal) && equal);
    CHECK(mo_value_equal(f.heap, f.types.text, words[4], after, &equal) && equal);
    mo_host_fake_state_drop(&f.host, f.heap, &snapshot);
    mo_value_drop(f.heap, type, console);
    for (unsigned i = 0; i < 5; i++)
        mo_value_drop(f.heap, f.types.text, words[i]);
    CHECK(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
}

static void test_fake_files(void) {
    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    struct mo_cell key = host_text(&f, "nest/a"), old = host_text(&f, "old"), map;
    CHECK(mo_value_map(f.heap, f.types.files, &key, &old, 1, &map));
    struct mo_cell retained = mo_value_dup(f.heap, f.types.files, map), files;
    struct mo_host_error error;
    enum mo_host_outcome outcome = mo_host_call(&f.host, f.heap, MO_HOST_FILES_IN_MEMORY,
                                                f.capabilities[MO_CAP_FILES], &map, &files, &error);
    CHECK(outcome == MO_HOST_RETURNED);
    if (outcome != MO_HOST_RETURNED) {
        host_fixture_free(&f);
        return;
    }
    expect(&f, MO_HOST_FILES_READ_TEXT, f.result_text, files, "nest/a", "", "Ok(\"old\")");
    expect(&f, MO_HOST_FILES_WRITE_TEXT, f.result_unit, files, "nest/a", "new", "Ok(Unit)");
    bool found;
    struct mo_cell value;
    CHECK(mo_table_get(f.heap, f.types.files, retained, key, &found, &value) && found);
    bool same;
    CHECK(mo_value_equal(f.heap, f.types.text, value, old, &same) && same);
    expect(&f, MO_HOST_FILES_REMOVE, f.result_unit, files, "nest/a", "", "Ok(Unit)");
    expect(&f, MO_HOST_FILES_LIST, f.result_names, files, ".", "", "Ok([\"nest\"])");
    expect(&f, MO_HOST_FILES_LIST, f.result_names, files, "nest", "", "Ok([])");
    struct mo_cell args[] = {files, host_text(&f, "future")}, narrowed;
    CHECK(mo_host_call(&f.host, f.heap, MO_HOST_FILES_WITHIN, f.capabilities[MO_CAP_FILES], args,
                       &narrowed, &error) == MO_HOST_RETURNED);
    mo_value_drop(f.heap, f.types.text, args[1]);
    expect(&f, MO_HOST_FILES_READ_TEXT, f.result_text, narrowed, "x", "",
           "Error(NotFound(path: \"x\"))");
    expect(&f, MO_HOST_FILES_MAKE_FOLDER, f.result_unit, files, "future/deep", "", "Ok(Unit)");
    expect(&f, MO_HOST_FILES_WRITE_TEXT, f.result_unit, narrowed, "x", "yes", "Ok(Unit)");
    expect(&f, MO_HOST_FILES_READ_TEXT, f.result_text, files, "future/x", "", "Ok(\"yes\")");
    mo_value_drop(f.heap, f.capabilities[MO_CAP_FILES], narrowed);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_FILES], files);
    mo_value_drop(f.heap, f.types.files, retained);
    mo_value_drop(f.heap, f.types.text, key);
    mo_value_drop(f.heap, f.types.text, old);
    CHECK(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
}

static void test_fake_file_keys(void) {
    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    const struct {
        const char *bytes;
        size_t length;
    } invalid[] = {{"/a", 2},   {"", 0},     {".", 1},       {"..", 2},    {"./a", 3},
                   {"../a", 4}, {"a/", 2},   {"a//b", 4},    {"a/./b", 5}, {"a/../b", 6},
                   {"a/.", 3},  {"a/..", 4}, {"a\0/../b", 7}};
    struct mo_cell value = host_text(&f, "old");
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        struct mo_cell key, map;
        MO_ASSERT(mo_value_text(f.heap, invalid[i].bytes, invalid[i].length, &key) &&
                  mo_value_map(f.heap, f.types.files, &key, &value, 1, &map));
        struct mo_cell out = mo_cell_int(99);
        struct mo_host_error error;
        size_t used = mo_heap_used(f.heap);
        enum mo_host_outcome outcome =
            mo_host_call(&f.host, f.heap, MO_HOST_FILES_IN_MEMORY, f.capabilities[MO_CAP_FILES],
                         &map, &out, &error);
        CHECK(outcome == MO_HOST_BUG);
        if (outcome == MO_HOST_RETURNED)
            mo_value_drop(f.heap, f.capabilities[MO_CAP_FILES], out);
        else {
            CHECK(mo_cell_get_int(out) == 99 && mo_heap_used(f.heap) == used);
            CHECK(strcmp(error.reason, "fake has a key that is not a plain relative path") == 0 &&
                  error.has_detail);
            bool same, found;
            struct mo_cell kept;
            CHECK(mo_value_equal(f.heap, f.types.text, error.detail, key, &same) && same);
            CHECK(mo_table_get(f.heap, f.types.files, map, key, &found, &kept) && found);
            CHECK(mo_value_equal(f.heap, f.types.text, kept, value, &same) && same);
            mo_value_drop(f.heap, f.types.files, map);
        }
        mo_value_drop(f.heap, f.types.text, key);
    }
    mo_value_drop(f.heap, f.types.text, value);
    CHECK(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
}

static void test_fake_folder_exhaustion(void) {
    unsigned refusals = 0, successes = 0;
    for (size_t remaining = 128; remaining <= 12288; remaining += 128) {
        struct host_fixture f;
        const size_t limit = 32768;
        host_fixture_init(&f, limit);
        struct mo_cell map, files, result = mo_cell_int(37);
        MO_ASSERT(mo_value_map(f.heap, f.types.files, NULL, NULL, 0, &map));
        struct mo_host_error error;
        MO_ASSERT(mo_host_call(&f.host, f.heap, MO_HOST_FILES_IN_MEMORY,
                               f.capabilities[MO_CAP_FILES], &map, &files,
                               &error) == MO_HOST_RETURNED);
        struct mo_cell args[] = {files, host_text(&f, "a/b/c/d/e/f/g/h/i/j/k/l")};
        size_t used = mo_heap_used(f.heap);
        struct mo_scratch held;
        mo_scratch_init(&held, f.heap);
        MO_ASSERT(mo_scratch_resize(&held, NULL, 0, limit - used - remaining) != NULL);
        enum mo_host_outcome outcome = mo_host_call(&f.host, f.heap, MO_HOST_FILES_MAKE_FOLDER,
                                                    f.result_unit, args, &result, &error);
        mo_scratch_free(&held);
        if (outcome == MO_HOST_OUT_OF_MEMORY) {
            refusals++;
            CHECK(mo_cell_get_int(result) == 37 && mo_heap_used(f.heap) == used);
            expect(&f, MO_HOST_FILES_LIST, f.result_names, files, ".", "", "Ok([])");
        } else {
            successes++;
            CHECK(outcome == MO_HOST_RETURNED);
            mo_value_drop(f.heap, f.result_unit, result);
            expect(&f, MO_HOST_FILES_LIST, f.result_names, files, ".", "", "Ok([\"a\"])");
        }
        mo_value_drop(f.heap, f.types.text, args[1]);
        mo_value_drop(f.heap, f.capabilities[MO_CAP_FILES], files);
        CHECK(mo_heap_objects(f.heap) == 0);
        host_fixture_free(&f);
    }
    CHECK(refusals != 0 && successes != 0);
}

static void pending_root(struct host_fixture *f, struct mo_cell files) {
    struct mo_cell args[] = {files, host_text(f, "future")}, child;
    struct mo_host_error error;
    enum mo_host_outcome outcome =
        mo_host_call(&f->host, f->heap, MO_HOST_FILES_WITHIN, f->capabilities[MO_CAP_FILES], args,
                     &child, &error);
    CHECK(outcome == MO_HOST_RETURNED);
    mo_value_drop(f->heap, f->types.text, args[1]);
    if (outcome != MO_HOST_RETURNED)
        return;
    expect(f, MO_HOST_FILES_READ_TEXT, f->result_text, child, "x", "",
           "Error(NotFound(path: \"x\"))");
    expect(f, MO_HOST_FILES_MAKE_FOLDER, f->result_unit, child, "x", "",
           "Error(NotFound(path: \"x\"))");
    expect(f, MO_HOST_FILES_MAKE_FOLDER, f->result_unit, files, "future", "", "Ok(Unit)");
    expect(f, MO_HOST_FILES_WRITE_TEXT, f->result_unit, child, "x", "bound", "Ok(Unit)");
    expect(f, MO_HOST_FILES_READ_TEXT, f->result_text, files, "future/x", "", "Ok(\"bound\")");
    expect(f, MO_HOST_FILES_REMOVE, f->result_unit, child, "x", "", "Ok(Unit)");
    mo_value_drop(f->heap, f->capabilities[MO_CAP_FILES], child);
}

static void retained_folder(struct host_fixture *f, struct mo_cell files, int directory) {
    expect(f, MO_HOST_FILES_MAKE_FOLDER, f->result_unit, files, "held", "", "Ok(Unit)");
    expect(f, MO_HOST_FILES_WRITE_TEXT, f->result_unit, files, "held/a", "object", "Ok(Unit)");
    CHECK(symlinkat("../a", directory, "held/escape") == 0);
    struct mo_cell args[] = {files, host_text(f, "held")}, child, readonly;
    struct mo_host_error error;
    MO_ASSERT(mo_host_call(&f->host, f->heap, MO_HOST_FILES_WITHIN, f->capabilities[MO_CAP_FILES],
                           args, &child, &error) == MO_HOST_RETURNED);
    mo_value_drop(f->heap, f->types.text, args[1]);
    MO_ASSERT(mo_host_call(&f->host, f->heap, MO_HOST_FILES_READ_ONLY,
                           f->capabilities[MO_CAP_READ_FILES], &child, &readonly,
                           &error) == MO_HOST_RETURNED);
    char path[] = "/tmp/mo-host-move-XXXXXX";
    MO_ASSERT(mkdtemp(path) != NULL);
    int outside = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    MO_ASSERT(outside >= 0);
    CHECK(renameat(directory, "held", outside, "moved") == 0);
    expect(f, MO_HOST_FILES_READ_TEXT, f->result_text, child, "a", "", "Ok(\"object\")");
    expect(f, MO_HOST_READFILES_READ_TEXT, f->result_text, readonly, "a", "", "Ok(\"object\")");
    expect(f, MO_HOST_FILES_READ_TEXT, f->result_text, files, "held/a", "",
           "Error(NotFound(path: \"held/a\"))");
    expect(f, MO_HOST_FILES_READ_TEXT, f->result_text, child, "escape", "",
           "Error(NotAllowed(path: \"escape\"))");
    expect(f, MO_HOST_FILES_REMOVE, f->result_unit, child, "a", "", "Ok(Unit)");
    expect(f, MO_HOST_FILES_REMOVE, f->result_unit, child, "escape", "", "Ok(Unit)");
    mo_value_drop(f->heap, f->capabilities[MO_CAP_READ_FILES], readonly);
    mo_value_drop(f->heap, f->capabilities[MO_CAP_FILES], child);
    CHECK(unlinkat(outside, "moved", AT_REMOVEDIR) == 0);
    CHECK(close(outside) == 0 && rmdir(path) == 0);
}

static void test_real_files(bool portable) {
    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    f.host.force_fallback = portable;
    char path[] = "/tmp/mo-host-XXXXXX";
    CHECK(mkdtemp(path) != NULL);
    int directory = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    MO_ASSERT(directory >= 0);
    struct mo_cell files;
    struct mo_host_error error;
    enum mo_host_outcome outcome = mo_host_real(&f.host, f.heap, f.capabilities[MO_CAP_FILES], NULL,
                                                directory, &files, &error);
    CHECK(outcome == MO_HOST_RETURNED);
    if (outcome == MO_HOST_RETURNED) {
        expect(&f, MO_HOST_FILES_READ_TEXT, f.result_text, files, ".", "",
               "Error(Failed(path: \".\", reason: \"is a folder\"))");
        expect(&f, MO_HOST_FILES_WRITE_TEXT, f.result_unit, files, "a", "hello", "Ok(Unit)");
        expect(&f, MO_HOST_FILES_APPEND_TEXT, f.result_unit, files, "a", "!", "Ok(Unit)");
        expect(&f, MO_HOST_FILES_READ_TEXT, f.result_text, files, "a", "", "Ok(\"hello!\")");
        expect(&f, MO_HOST_FILES_LIST, f.result_names, files, ".", "", "Ok([\"a\"])");
        expect(&f, MO_HOST_FILES_WRITE_TEXT, f.result_unit, files, "missing/x", "",
               "Error(NotFound(path: \"missing/x\"))");
        expect(&f, MO_HOST_FILES_MAKE_FOLDER, f.result_unit, files, "deep/nest", "", "Ok(Unit)");
        expect(&f, MO_HOST_FILES_LIST, f.result_names, files, "a", "",
               "Error(Failed(path: \"a\", reason: \"not a folder\"))");
        CHECK(symlinkat("deep/nest/../../a", directory, "inside") == 0);
        CHECK(symlinkat("../outside", directory, "escape") == 0);
        CHECK(symlinkat("/etc/passwd", directory, "absolute") == 0);
        CHECK(symlinkat("a/", directory, "slash") == 0);
        expect(&f, MO_HOST_FILES_READ_TEXT, f.result_text, files, "inside", "", "Ok(\"hello!\")");
        expect(&f, MO_HOST_FILES_READ_TEXT, f.result_text, files, "escape", "",
               "Error(NotAllowed(path: \"escape\"))");
        expect(&f, MO_HOST_FILES_READ_TEXT, f.result_text, files, "absolute", "",
               "Error(NotAllowed(path: \"absolute\"))");
        expect(&f, MO_HOST_FILES_READ_TEXT, f.result_text, files, "slash", "",
               "Error(Failed(path: \"slash\", reason: \"not a folder\"))");
        expect(&f, MO_HOST_FILES_READ_TEXT, f.result_text, files, "a/../inside", "",
               "Ok(\"hello!\")");
        expect(&f, MO_HOST_FILES_READ_TEXT, f.result_text, files, "../a", "",
               "Error(NotAllowed(path: \"../a\"))");
        CHECK(mkfifoat(directory, "fifo", 0600) == 0);
        expect(&f, MO_HOST_FILES_ENTRIES, f.result_entries, files, ".", "",
               "Ok([Entry(name: \"a\", kind: File), Entry(name: \"absolute\", kind: Link), "
               "Entry(name: \"deep\", kind: Folder), Entry(name: \"escape\", kind: Link), "
               "Entry(name: \"fifo\", kind: Other), Entry(name: \"inside\", kind: Link), "
               "Entry(name: \"slash\", kind: Link)])");
        expect(&f, MO_HOST_FILES_READ_TEXT, f.result_text, files, "fifo", "",
               "Error(Failed(path: \"fifo\", reason: \"not a regular file\"))");
        expect(&f, MO_HOST_FILES_WRITE_TEXT, f.result_unit, files, "fifo", "x",
               "Error(Failed(path: \"fifo\", reason: \"not a regular file\"))");
        CHECK(unlinkat(directory, "fifo", 0) == 0);
        int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        MO_ASSERT(socket_fd >= 0);
        struct sockaddr_un address = {.sun_family = AF_UNIX};
        int address_size = snprintf(address.sun_path, sizeof address.sun_path, "%s/socket", path);
        MO_ASSERT(address_size > 0 && (size_t)address_size < sizeof address.sun_path &&
                  bind(socket_fd, (const struct sockaddr *)&address, sizeof address) == 0);
        expect(&f, MO_HOST_FILES_READ_TEXT, f.result_text, files, "socket", "",
               "Error(Failed(path: \"socket\", reason: \"not a regular file\"))");
        expect(&f, MO_HOST_FILES_WRITE_TEXT, f.result_unit, files, "socket", "x",
               "Error(Failed(path: \"socket\", reason: \"not a regular file\"))");
        CHECK(close(socket_fd) == 0 && unlinkat(directory, "socket", 0) == 0);
        CHECK(symlinkat("newdir/", directory, "missing-slash") == 0);
        expect(&f, MO_HOST_FILES_WRITE_TEXT, f.result_unit, files, "missing-slash", "x",
               "Error(Failed(path: \"missing-slash\", reason: \"not a folder\"))");
        struct stat absent;
        CHECK(fstatat(directory, "newdir", &absent, AT_SYMLINK_NOFOLLOW) < 0 && errno == ENOENT);
        CHECK(unlinkat(directory, "missing-slash", 0) == 0);
        expect(&f, MO_HOST_FILES_WRITE_TEXT, f.result_unit, files, "slash", "x",
               "Error(Failed(path: \"slash\", reason: \"not a folder\"))");
        char long_target[4000];
        memset(long_target, 'a', sizeof long_target - 1);
        long_target[sizeof long_target - 1] = '\0';
        if (symlinkat(long_target, directory, "long-rest") == 0) {
            char long_path[129], expected[256];
            memcpy(long_path, "long-rest/", 10);
            memset(long_path + 10, 'b', 118);
            long_path[128] = '\0';
            int n = snprintf(expected, sizeof expected,
                             "Error(Failed(path: \"%s\", reason: \"path too long\"))", long_path);
            MO_ASSERT(n > 0 && (size_t)n < sizeof expected);
            expect(&f, MO_HOST_FILES_READ_TEXT, f.result_text, files, long_path, "", expected);
            CHECK(unlinkat(directory, "long-rest", 0) == 0);
        } else
            CHECK(errno == ENAMETOOLONG);
        CHECK(mkdirat(directory, "search", 0700) == 0);
        expect(&f, MO_HOST_FILES_WRITE_TEXT, f.result_unit, files, "search/f", "searchable",
               "Ok(Unit)");
        CHECK(fchmodat(directory, "search", 0311, 0) == 0);
        expect(&f, MO_HOST_FILES_READ_TEXT, f.result_text, files, "search/f", "",
               "Ok(\"searchable\")");
        if (geteuid() != 0) {
            CHECK(mkdirat(directory, "search/nest", 0700) == 0);
            expect(&f, MO_HOST_FILES_WRITE_TEXT, f.result_unit, files, "search/nest/f", "restored",
                   "Ok(Unit)");
            CHECK(fchmodat(directory, "search", 0000, 0) == 0);
            struct mo_cell args[] = {files, host_text(&f, "search/nest")}, narrowed;
            MO_ASSERT(mo_host_call(&f.host, f.heap, MO_HOST_FILES_WITHIN,
                                   f.capabilities[MO_CAP_FILES], args, &narrowed,
                                   &error) == MO_HOST_RETURNED);
            expect(&f, MO_HOST_FILES_READ_TEXT, f.result_text, narrowed, "f", "",
                   "Error(NotAllowed(path: \"f\"))");
            CHECK(fchmodat(directory, "search", 0755, 0) == 0);
            expect(&f, MO_HOST_FILES_READ_TEXT, f.result_text, narrowed, "f", "",
                   "Ok(\"restored\")");
            mo_value_drop(f.heap, f.capabilities[MO_CAP_FILES], narrowed);
            mo_value_drop(f.heap, f.types.text, args[1]);
            CHECK(unlinkat(directory, "search/nest/f", 0) == 0 &&
                  unlinkat(directory, "search/nest", AT_REMOVEDIR) == 0);
        }
        CHECK(fchmodat(directory, "search", 0755, 0) == 0 &&
              unlinkat(directory, "search/f", 0) == 0 &&
              unlinkat(directory, "search", AT_REMOVEDIR) == 0);
        expect(&f, MO_HOST_FILES_WRITE_TEXT, f.result_unit, files, "deep", "",
               "Error(Failed(path: \"deep\", reason: \"is a folder\"))");
        expect(&f, MO_HOST_FILES_APPEND_TEXT, f.result_unit, files, "deep", "",
               "Error(Failed(path: \"deep\", reason: \"is a folder\"))");
        expect(&f, MO_HOST_FILES_REMOVE, f.result_unit, files, "deep", "",
               "Error(Failed(path: \"deep\", reason: \"is a folder\"))");
        expect(&f, MO_HOST_FILES_MAKE_FOLDER, f.result_unit, files, "a", "",
               "Error(Failed(path: \"a\", reason: \"not a folder\"))");
        expect(&f, MO_HOST_FILES_REMOVE, f.result_unit, files, "escape", "", "Ok(Unit)");
        expect(&f, MO_HOST_FILES_READ_TEXT, f.result_text, files, "a", "", "Ok(\"hello!\")");
        expect(&f, MO_HOST_FILES_REMOVE, f.result_unit, files, "inside", "", "Ok(Unit)");
        expect(&f, MO_HOST_FILES_REMOVE, f.result_unit, files, "absolute", "", "Ok(Unit)");
        expect(&f, MO_HOST_FILES_REMOVE, f.result_unit, files, "slash", "", "Ok(Unit)");
        for (unsigned i = 0; i <= MO_LIMIT_PATH_LINKS; i++) {
            char name[32], target[32] = "a";
            int n = snprintf(name, sizeof name, "link%u", i);
            int t = i == MO_LIMIT_PATH_LINKS ? 1 : snprintf(target, sizeof target, "link%u", i + 1);
            MO_ASSERT(n > 0 && (size_t)n < sizeof name && t > 0 && (size_t)t < sizeof target);
            CHECK(symlinkat(target, directory, name) == 0);
        }
        expect(&f, MO_HOST_FILES_READ_TEXT, f.result_text, files, "link1", "", "Ok(\"hello!\")");
        expect(&f, MO_HOST_FILES_READ_TEXT, f.result_text, files, "link0", "",
               "Error(Failed(path: \"link0\", reason: \"too many links\"))");
        for (unsigned i = 0; i <= MO_LIMIT_PATH_LINKS; i++) {
            char name[32];
            int n = snprintf(name, sizeof name, "link%u", i);
            MO_ASSERT(n > 0 && (size_t)n < sizeof name);
            CHECK(unlinkat(directory, name, 0) == 0);
        }
        char longest[MO_LIMIT_HOST_PATH_SIZE];
        for (size_t i = 0; i < sizeof longest - 2; i += 2) {
            longest[i] = '.';
            longest[i + 1] = '/';
        }
        longest[sizeof longest - 2] = 'a';
        longest[sizeof longest - 1] = '\0';
        /* Some systems impose a smaller link-target bound than Mo's buffer. */
        if (symlinkat(longest, directory, "longest") == 0) {
            expect(&f, MO_HOST_FILES_READ_TEXT, f.result_text, files, "longest", "",
                   "Ok(\"hello!\")");
            CHECK(unlinkat(directory, "longest", 0) == 0);
            longest[sizeof longest - 2] = '.';
            CHECK(symlinkat(longest, directory, "longest") == 0);
            expect(&f, MO_HOST_FILES_READ_TEXT, f.result_text, files, "longest/a", "",
                   "Error(Failed(path: \"longest/a\", reason: \"path too long\"))");
            CHECK(unlinkat(directory, "longest", 0) == 0);
        } else
            CHECK(errno == ENAMETOOLONG);
        CHECK(symlinkat("new-file", directory, "create-link") == 0);
        expect(&f, MO_HOST_FILES_WRITE_TEXT, f.result_unit, files, "create-link", "created",
               "Ok(Unit)");
        expect(&f, MO_HOST_FILES_READ_TEXT, f.result_text, files, "new-file", "",
               "Ok(\"created\")");
        CHECK(unlinkat(directory, "new-file", 0) == 0 &&
              unlinkat(directory, "create-link", 0) == 0);
        /* macOS starts with 256 descriptors. Let this test reach Mo's limit
         * before the test process's unrelated descriptors reach the OS limit. */
        struct rlimit original, raised;
        MO_ASSERT(getrlimit(RLIMIT_NOFILE, &original) == 0);
        raised = original;
        if (raised.rlim_cur < MO_LIMIT_OPEN_FOLDERS + 32u) {
            raised.rlim_cur = MO_LIMIT_OPEN_FOLDERS + 32u;
            MO_ASSERT(setrlimit(RLIMIT_NOFILE, &raised) == 0);
        }
        struct mo_cell handles[MO_LIMIT_OPEN_FOLDERS - 1];
        for (unsigned i = 0; i < MO_LIMIT_OPEN_FOLDERS - 1; i++)
            MO_ASSERT(mo_host_call(&f.host, f.heap, MO_HOST_FILES_READ_ONLY,
                                   f.capabilities[MO_CAP_READ_FILES], &files, &handles[i],
                                   &error) == MO_HOST_RETURNED);
        struct mo_cell refused = mo_cell_int(123);
        CHECK(mo_host_call(&f.host, f.heap, MO_HOST_FILES_READ_ONLY,
                           f.capabilities[MO_CAP_READ_FILES], &files, &refused,
                           &error) == MO_HOST_BUG);
        CHECK(mo_cell_get_int(refused) == 123 &&
              strcmp(error.reason, "too many folders open at once") == 0 &&
              f.host.open_folders == MO_LIMIT_OPEN_FOLDERS);
        for (unsigned i = 0; i < MO_LIMIT_OPEN_FOLDERS - 1; i++)
            mo_value_drop(f.heap, f.capabilities[MO_CAP_READ_FILES], handles[i]);
        CHECK(setrlimit(RLIMIT_NOFILE, &original) == 0);
        const uint8_t invalid[] = {0xff, 0, 0xc0};
        int raw = openat(directory, "invalid", O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
        MO_ASSERT(raw >= 0);
        CHECK(write(raw, invalid, sizeof invalid) == (ssize_t)sizeof invalid);
        CHECK(close(raw) == 0);
        expect(&f, MO_HOST_FILES_READ_TEXT, f.result_text, files, "invalid", "",
               "Error(NotText(path: \"invalid\"))");
        struct mo_cell raw_args[] = {files, host_text(&f, "invalid")}, read_bytes;
        MO_ASSERT(mo_host_call(&f.host, f.heap, MO_HOST_FILES_READ_BYTES, f.result_bytes, raw_args,
                               &read_bytes, &error) == MO_HOST_RETURNED);
        size_t raw_size;
        struct mo_cell data = mo_cell_get_payload(read_bytes);
        CHECK(mo_cell_get_tag(read_bytes) == 0 &&
              memcmp(mo_value_data(data, &raw_size), invalid, sizeof invalid) == 0 &&
              raw_size == sizeof invalid);
        mo_value_drop(f.heap, f.result_bytes, read_bytes);
        mo_value_drop(f.heap, f.types.text, raw_args[1]);
        CHECK(unlinkat(directory, "invalid", 0) == 0);
        raw = openat(directory, "\xff", O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
        MO_ASSERT(raw >= 0);
        CHECK(close(raw) == 0);
        expect(&f, MO_HOST_FILES_LIST, f.result_names, files, ".", "",
               "Error(Failed(path: \".\", reason: \"folder contains a name that is not text\"))");
        CHECK(unlinkat(directory, "\xff", 0) == 0);
        pending_root(&f, files);
        retained_folder(&f, files, directory);
        expect(&f, MO_HOST_FILES_REMOVE, f.result_unit, files, "a", "", "Ok(Unit)");
        mo_value_drop(f.heap, f.capabilities[MO_CAP_FILES], files);
        CHECK(mo_heap_objects(f.heap) == 0 && f.host.open_folders == 0);
        CHECK(unlinkat(directory, "deep/nest", AT_REMOVEDIR) == 0);
        CHECK(unlinkat(directory, "deep", AT_REMOVEDIR) == 0);
        /* If pending capability construction refused, no folder was created. */
        if (f.host.open_folders == 0) {
            int removed = unlinkat(directory, "future", AT_REMOVEDIR);
            CHECK(removed == 0 || errno == ENOENT);
        }
    }
    CHECK(close(directory) == 0);
    CHECK(rmdir(path) == 0);
    host_fixture_free(&f);
}

static volatile sig_atomic_t broken_outputs;
static void broken_output(int signal_number) {
    if (signal_number == SIGPIPE)
        broken_outputs++;
}

static void test_real_console(void) {
    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    MO_ASSERT(mo_host_start(&f.host) == NULL);
    int input[2], output[2];
    MO_ASSERT(pipe(input) == 0 && pipe(output) == 0);
    f.host.input = input[0];
    f.host.output = output[1];
    const char lines[] = "A\r\n\n\xed\xa0\x80\xf0\x9fTAIL";
    CHECK(write(input[1], lines, sizeof lines - 1) == (ssize_t)sizeof lines - 1);
    CHECK(close(input[1]) == 0);
    struct mo_host_error error;
    struct mo_cell console, result;
    MO_ASSERT(mo_host_real(&f.host, f.heap, f.capabilities[MO_CAP_CONSOLE], NULL, -1, &console,
                           &error) == MO_HOST_RETURNED);
    expect(&f, MO_HOST_CONSOLE_READ_LINE, f.option, console, "", "", "Some(\"A\")");
    expect(&f, MO_HOST_CONSOLE_READ_LINE, f.option, console, "", "", "Some(\"\")");
    expect(&f, MO_HOST_CONSOLE_READ_LINE, f.option, console, "", "",
           "Some(\"\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbdTAIL\")");
    expect(&f, MO_HOST_CONSOLE_READ_LINE, f.option, console, "", "", "None");
    struct mo_cell args[] = {console, host_text(&f, "hello")};
    CHECK(mo_host_call(&f.host, f.heap, MO_HOST_CONSOLE_PRINT, f.unit, args, &result, &error) ==
          MO_HOST_RETURNED);
    char bytes[16];
    CHECK(read(output[0], bytes, sizeof bytes) == 6 && memcmp(bytes, "hello\n", 6) == 0);
    CHECK(close(output[0]) == 0);
    struct sigaction handler = {.sa_handler = broken_output}, previous, observed;
    MO_ASSERT(sigemptyset(&handler.sa_mask) == 0 && sigaction(SIGPIPE, &handler, &previous) == 0);
    CHECK(mo_host_start(&f.host) == NULL); /* No repeated process-wide change. */
    result = mo_cell_int(19);
    CHECK(mo_host_call(&f.host, f.heap, MO_HOST_CONSOLE_PRINT, f.unit, args, &result, &error) ==
          MO_HOST_BUG);
    CHECK(mo_cell_get_int(result) == 19 &&
          strcmp(error.reason, "printing to an output that is gone") == 0);
    CHECK(sigaction(SIGPIPE, NULL, &observed) == 0 && observed.sa_handler == broken_output &&
          broken_outputs == 1);
    CHECK(sigaction(SIGPIPE, &previous, NULL) == 0);
    mo_value_drop(f.heap, f.types.text, args[1]);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_CONSOLE], console);
    CHECK(close(input[0]) == 0 && close(output[1]) == 0);
    CHECK(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
}

static void test_nonblocking_console(void) {
    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    MO_ASSERT(mo_host_start(&f.host) == NULL);
    int output[2];
    MO_ASSERT(pipe(output) == 0 && fcntl(output[1], F_SETFL, O_NONBLOCK) == 0);
    char fill[4096];
    memset(fill, 'P', sizeof fill);
    size_t prefix = 0;
    for (;;) {
        ssize_t n = write(output[1], fill, sizeof fill);
        if (n < 0) {
            MO_ASSERT(errno == EAGAIN || errno == EWOULDBLOCK);
            break;
        }
        MO_ASSERT(n > 0);
        prefix += (size_t)n;
    }
    const size_t length = 200000;
    pid_t child = fork();
    MO_ASSERT(child >= 0);
    if (child == 0) {
        MO_ASSERT(close(output[1]) == 0);
        size_t used = 0;
        ssize_t n;
        while ((n = read(output[0], fill, sizeof fill)) > 0) {
            for (size_t i = 0; i < (size_t)n; i++) {
                size_t at = used + i;
                uint8_t expected = at < prefix             ? 'P'
                                   : at == prefix + length ? '\n'
                                                           : (uint8_t)('a' + (at - prefix) % 23);
                CHECK((uint8_t)fill[i] == expected);
            }
            used += (size_t)n;
        }
        CHECK(n == 0 && used == prefix + length + 1 && close(output[0]) == 0);
        _exit(mo_test_failures == 0 ? 0 : 1);
    }
    MO_ASSERT(close(output[0]) == 0);
    f.host.output = output[1];
    char *bytes = malloc(length);
    MO_ASSERT(bytes != NULL);
    for (size_t i = 0; i < length; i++)
        bytes[i] = (char)('a' + i % 23);
    struct mo_cell console, text, out;
    struct mo_host_error error;
    MO_ASSERT(mo_host_real(&f.host, f.heap, f.capabilities[MO_CAP_CONSOLE], NULL, -1, &console,
                           &error) == MO_HOST_RETURNED &&
              mo_value_text(f.heap, bytes, length, &text));
    free(bytes);
    struct mo_cell args[] = {console, text};
    CHECK(mo_host_call(&f.host, f.heap, MO_HOST_CONSOLE_PRINT, f.unit, args, &out, &error) ==
          MO_HOST_RETURNED);
    CHECK(close(output[1]) == 0);
    int status;
    MO_ASSERT(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    mo_value_drop(f.heap, f.types.text, text);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_CONSOLE], console);
    int input[2];
    MO_ASSERT(pipe(input) == 0 && fcntl(input[0], F_SETFL, O_NONBLOCK) == 0);
    f.host.input = input[0];
    child = fork();
    MO_ASSERT(child >= 0);
    if (child == 0) {
        MO_ASSERT(close(input[0]) == 0 && write(input[1], "delayed\n", 8) == 8 &&
                  close(input[1]) == 0);
        _exit(0);
    }
    MO_ASSERT(close(input[1]) == 0);
    MO_ASSERT(mo_host_real(&f.host, f.heap, f.capabilities[MO_CAP_CONSOLE], NULL, -1, &console,
                           &error) == MO_HOST_RETURNED);
    expect(&f, MO_HOST_CONSOLE_READ_LINE, f.option, console, "", "", "Some(\"delayed\")");
    expect(&f, MO_HOST_CONSOLE_READ_LINE, f.option, console, "", "", "None");
    MO_ASSERT(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0 && close(input[0]) == 0);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_CONSOLE], console);
    /* A fresh capability must turn EBADF, not just a buffered EOF, into None. */
    MO_ASSERT(mo_host_real(&f.host, f.heap, f.capabilities[MO_CAP_CONSOLE], NULL, -1, &console,
                           &error) == MO_HOST_RETURNED);
    expect(&f, MO_HOST_CONSOLE_READ_LINE, f.option, console, "", "", "None");
    mo_value_drop(f.heap, f.capabilities[MO_CAP_CONSOLE], console);
    CHECK(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
}

static MO_MUST_USE unsigned descriptors(void) {
    unsigned count = 0;
    for (int fd = 0; fd < 128; fd++) {
        int result = fcntl(fd, F_GETFD);
        MO_ASSERT(result >= 0 || errno == EBADF);
        count += result >= 0 ? 1u : 0u;
    }
    return count;
}

static void test_deep_path(bool portable) {
    char temporary[] = "/tmp/mo-host-deep-XXXXXX";
    MO_ASSERT(mkdtemp(temporary) != NULL);
    int root = open(temporary, O_RDONLY | O_DIRECTORY | O_CLOEXEC), current = root;
    MO_ASSERT(root >= 0);
    int marker = openat(root, "marker", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    MO_ASSERT(marker >= 0 && write(marker, "deep", 4) == 4 && close(marker) == 0 &&
              symlinkat("marker", root, "up") == 0);
    char target[604], path[2403];
    for (unsigned i = 0; i < 200; i++)
        memcpy(target + 3 * i, "../", 3);
    memcpy(target + 600, "up", 3);
    for (unsigned depth = 1; depth <= 1200; depth++) {
        MO_ASSERT(mkdirat(current, "d", 0700) == 0);
        int next = openat(current, "d", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        MO_ASSERT(next >= 0);
        if (current != root)
            MO_ASSERT(close(current) == 0);
        current = next;
        memcpy(path + 2 * (depth - 1), "d/", 2);
        if (depth % 200 == 0)
            MO_ASSERT(symlinkat(target, current, "up") == 0);
    }
    memcpy(path + 2400, "up", 3);
    struct rlimit original;
    MO_ASSERT(getrlimit(RLIMIT_NOFILE, &original) == 0 && original.rlim_cur >= 64);
    struct rlimit lowered = original;
    lowered.rlim_cur = 64;
    MO_ASSERT(setrlimit(RLIMIT_NOFILE, &lowered) == 0);
    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    f.host.force_fallback = portable;
    struct mo_cell files;
    struct mo_host_error error;
    MO_ASSERT(mo_host_real(&f.host, f.heap, f.capabilities[MO_CAP_FILES], NULL, root, &files,
                           &error) == MO_HOST_RETURNED);
    unsigned before = descriptors();
    expect(&f, MO_HOST_FILES_READ_TEXT, f.result_text, files, path, "", "Ok(\"deep\")");
    CHECK(descriptors() == before);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_FILES], files);
    CHECK(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
    CHECK(setrlimit(RLIMIT_NOFILE, &original) == 0);
    for (unsigned depth = 1200; depth != 0; depth--) {
        if (depth % 200 == 0)
            CHECK(unlinkat(current, "up", 0) == 0);
        int parent = openat(current, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        MO_ASSERT(parent >= 0 && close(current) == 0 && unlinkat(parent, "d", AT_REMOVEDIR) == 0);
        current = parent;
    }
    CHECK(close(current) == 0 && unlinkat(root, "up", 0) == 0 && unlinkat(root, "marker", 0) == 0 &&
          close(root) == 0 && rmdir(temporary) == 0);
}

static void test_hostile_links(bool portable) {
    enum { DEPTH = 500, LINKS = 40 };
#if defined(__APPLE__)
    enum { REPEATS = 190 }; /* Shorter native link targets still exhaust the replay budget. */
#else
    enum { REPEATS = 790 };
#endif
    char temporary[] = "/tmp/mo-host-hostile-XXXXXX";
    MO_ASSERT(mkdtemp(temporary) != NULL);
    int root = open(temporary, O_RDONLY | O_DIRECTORY | O_CLOEXEC), current = root;
    MO_ASSERT(root >= 0);
    char path[DEPTH * 2 + 8], target[MO_LIMIT_HOST_PATH_SIZE], name[16];
    for (unsigned depth = 0; depth < DEPTH; depth++) {
        MO_ASSERT(mkdirat(current, "d", 0700) == 0);
        int next = openat(current, "d", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        MO_ASSERT(next >= 0);
        if (current != root)
            MO_ASSERT(close(current) == 0);
        current = next;
        memcpy(path + 2 * depth, "d/", 2);
    }
    memcpy(path + DEPTH * 2, "l0", 3);
    MO_ASSERT(mkdirat(current, "b", 0700) == 0);
    int marker = openat(current, "marker", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    MO_ASSERT(marker >= 0 && write(marker, "kept", 4) == 4 && close(marker) == 0);
    for (unsigned repeat = 0; repeat < REPEATS; repeat++)
        memcpy(target + 5 * repeat, "b/../", 5);
    for (unsigned link = 0; link < LINKS; link++) {
        int n = snprintf(name, sizeof name, "l%u", link);
        MO_ASSERT(n > 0 && (size_t)n < sizeof name);
        n = link + 1 == LINKS
                ? snprintf(target + 5 * REPEATS, sizeof target - 5 * REPEATS, "marker")
                : snprintf(target + 5 * REPEATS, sizeof target - 5 * REPEATS, "l%u", link + 1);
        MO_ASSERT(n > 0 && (size_t)n < sizeof target - 5 * REPEATS &&
                  symlinkat(target, current, name) == 0);
    }
    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    f.host.force_fallback = portable;
    struct mo_cell files;
    struct mo_host_error error;
    MO_ASSERT(mo_host_real(&f.host, f.heap, f.capabilities[MO_CAP_FILES], NULL, root, &files,
                           &error) == MO_HOST_RETURNED);
    char expected[sizeof path + 80];
    int n = snprintf(expected, sizeof expected,
                     "Error(Failed(path: \"%s\", reason: \"MO_LIMIT_PATH_STEPS\"))", path);
    MO_ASSERT(n > 0 && (size_t)n < sizeof expected);
    unsigned before = descriptors();
    struct timespec start, end;
    MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    expect(&f, MO_HOST_FILES_READ_TEXT, f.result_text, files, path, "", expected);
    MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    double seconds =
        (double)(end.tv_sec - start.tv_sec) + (double)(end.tv_nsec - start.tv_nsec) / 1000000000;
    CHECK(printf("host hostile links (%s): %.3f s; depth %u, %u links, %u parent pairs per link\n",
                 portable ? "portable" : "default", seconds, DEPTH, LINKS, REPEATS) > 0);
    CHECK(descriptors() == before);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_FILES], files);
    CHECK(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
    for (unsigned link = 0; link < LINKS; link++) {
        n = snprintf(name, sizeof name, "l%u", link);
        MO_ASSERT(n > 0 && (size_t)n < sizeof name && unlinkat(current, name, 0) == 0);
    }
    CHECK(unlinkat(current, "b", AT_REMOVEDIR) == 0 && unlinkat(current, "marker", 0) == 0);
    for (unsigned depth = DEPTH; depth != 0; depth--) {
        int parent = openat(current, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        MO_ASSERT(parent >= 0 && close(current) == 0 && unlinkat(parent, "d", AT_REMOVEDIR) == 0);
        current = parent;
    }
    CHECK(close(current) == 0 && close(root) == 0 && rmdir(temporary) == 0);
}

static void test_large_input(bool portable) {
    char path[] = "/tmp/mo-host-large-XXXXXX";
    MO_ASSERT(mkdtemp(path) != NULL);
    int folder = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int fd = openat(folder, "large", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    MO_ASSERT(folder >= 0 && fd >= 0);
    char block[1000];
    for (size_t i = 0; i < sizeof block; i++)
        block[i] = (char)('a' + i % 23);
    for (unsigned i = 0; i < 9000; i++)
        MO_ASSERT(write(fd, block, sizeof block) == (ssize_t)sizeof block);
    for (unsigned small = 0; small < 2; small++) {
        struct host_fixture f;
        host_fixture_init(&f, small == 0 ? (64u << 20) : (8u << 20));
        f.host.force_fallback = portable;
        struct mo_cell files, console, out = mo_cell_int(99);
        struct mo_host_error error;
        MO_ASSERT(mo_host_real(&f.host, f.heap, f.capabilities[MO_CAP_FILES], NULL, folder, &files,
                               &error) == MO_HOST_RETURNED);
        struct mo_cell args[] = {files, host_text(&f, "large")};
        enum mo_host_outcome result = mo_host_call(&f.host, f.heap, MO_HOST_FILES_READ_TEXT,
                                                   f.result_text, args, &out, &error);
        if (small == 0) {
            CHECK(result == MO_HOST_RETURNED && mo_cell_get_tag(out) == 0);
            size_t length;
            const uint8_t *data = mo_value_data(mo_cell_get_payload(out), &length);
            CHECK(length == 9000000);
            for (size_t i = 0; i < length; i++)
                if (data[i] != (uint8_t)block[i % sizeof block]) {
                    CHECK(false);
                    break;
                }
            mo_value_drop(f.heap, f.result_text, out);
        } else
            CHECK(result == MO_HOST_OUT_OF_MEMORY && errno == ENOSPC && mo_cell_get_int(out) == 99);
        MO_ASSERT(lseek(fd, 0, SEEK_SET) == 0);
        f.host.input = fd;
        MO_ASSERT(mo_host_real(&f.host, f.heap, f.capabilities[MO_CAP_CONSOLE], NULL, -1, &console,
                               &error) == MO_HOST_RETURNED);
        out = mo_cell_int(99);
        result = mo_host_call(&f.host, f.heap, MO_HOST_CONSOLE_READ_LINE, f.option, &console, &out,
                              &error);
        if (small == 0) {
            CHECK(result == MO_HOST_RETURNED && mo_cell_get_tag(out) == 1);
            size_t length;
            const uint8_t *data = mo_value_data(mo_cell_get_payload(out), &length);
            CHECK(length == 9000000);
            for (size_t i = 0; i < length; i++)
                if (data[i] != (uint8_t)block[i % sizeof block]) {
                    CHECK(false);
                    break;
                }
            mo_value_drop(f.heap, f.option, out);
        } else
            CHECK(result == MO_HOST_OUT_OF_MEMORY && errno == ENOSPC && mo_cell_get_int(out) == 99);
        mo_value_drop(f.heap, f.capabilities[MO_CAP_CONSOLE], console);
        mo_value_drop(f.heap, f.capabilities[MO_CAP_FILES], files);
        mo_value_drop(f.heap, f.types.text, args[1]);
        CHECK(mo_heap_objects(f.heap) == 0);
        host_fixture_free(&f);
    }
    CHECK(close(fd) == 0 && unlinkat(folder, "large", 0) == 0 && close(folder) == 0 &&
          rmdir(path) == 0);
}

static void test_clock_random(void) {
    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    struct mo_cell clock, random, value;
    struct mo_host_error error;
    struct mo_cell instant = mo_cell_time(-1);
    CHECK(mo_host_call(&f.host, f.heap, MO_HOST_CLOCK_FIXED, f.capabilities[MO_CAP_CLOCK], &instant,
                       &clock, &error) == MO_HOST_RETURNED);
    CHECK(mo_host_call(&f.host, f.heap, MO_HOST_CLOCK_NOW, f.time, &clock, &value, &error) ==
          MO_HOST_RETURNED);
    CHECK(mo_cell_get_time(value) == -1);
    struct mo_cell seed = mo_cell_int(0);
    CHECK(mo_host_call(&f.host, f.heap, MO_HOST_RANDOM_SEEDED, f.capabilities[MO_CAP_RANDOM], &seed,
                       &random, &error) == MO_HOST_RETURNED);
    struct mo_cell args[] = {random, mo_cell_int(0), mo_cell_int(100)};
    CHECK(mo_host_call(&f.host, f.heap, MO_HOST_RANDOM_INT, f.types.integer, args, &value,
                       &error) == MO_HOST_RETURNED);
    CHECK(mo_cell_get_int(value) == 60);
    args[2] = args[1];
    value = mo_cell_int(77);
    CHECK(mo_host_call(&f.host, f.heap, MO_HOST_RANDOM_INT, f.types.integer, args, &value,
                       &error) == MO_HOST_BUG);
    CHECK(mo_cell_get_int(value) == 77 && strcmp(error.reason, "high not greater than low") == 0);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_CLOCK], clock);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_RANDOM], random);
    MO_ASSERT(mo_host_real(&f.host, f.heap, f.capabilities[MO_CAP_CLOCK], NULL, -1, &clock,
                           &error) == MO_HOST_RETURNED);
    CHECK(mo_host_call(&f.host, f.heap, MO_HOST_CLOCK_NOW, f.time, &clock, &value, &error) ==
          MO_HOST_RETURNED);
    CHECK(mo_cell_get_time(value) >= INT64_C(-62135596800000000) &&
          mo_cell_get_time(value) <= INT64_C(253402300799999999));
    MO_ASSERT(mo_host_real(&f.host, f.heap, f.capabilities[MO_CAP_RANDOM], NULL, -1, &random,
                           &error) == MO_HOST_RETURNED);
    args[0] = random;
    args[1] = mo_cell_int(INT64_MIN);
    args[2] = mo_cell_int(INT64_MAX);
    for (unsigned draw = 0; draw < 100; draw++) {
        CHECK(mo_host_call(&f.host, f.heap, MO_HOST_RANDOM_INT, f.types.integer, args, &value,
                           &error) == MO_HOST_RETURNED);
        CHECK(mo_cell_get_int(value) < INT64_MAX);
    }
    mo_value_drop(f.heap, f.capabilities[MO_CAP_CLOCK], clock);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_RANDOM], random);
    CHECK(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
}

static void test_empty_image(void) {
    const char *source = "bytecode 0.1 shorthand\nmodule M exposes []\n";
    struct mo_buffer wire;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    struct mo_assemble_error assembly;
    MO_ASSERT(mo_bytecode_assemble(source, strlen(source), &wire, &assembly));
    struct mo_load_error error;
    struct mo_image *image = mo_load(wire.bytes, wire.length, &error);
    MO_ASSERT(image != NULL);
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    CHECK(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    struct mo_process *p = mo_process_new(image, &limits);
    MO_ASSERT(p != NULL);
    /* Capability storage can build Text even when the file has no types. */
    struct mo_cell text;
    CHECK(mo_value_text(mo_process_heap(p), "empty", 5, &text));
    mo_value_drop(mo_process_heap(p), mo_process_host(p)->types->text, text);
    CHECK(mo_heap_objects(mo_process_heap(p)) == 0);
    mo_process_free(p);
    mo_image_free(image);
    mo_buffer_free(&wire);
}

static void test_main_capabilities(void) {
    const char *source =
        "bytecode 0.1 shorthand\nmodule M exposes [M.main]\n"
        "function M.main(a: Args borrowed, e: Env borrowed, c: Console borrowed, "
        "f: Files borrowed, ro: ReadFiles borrowed, t: Clock borrowed, n: Random borrowed) "
        ": List(Text) function in M\n registers [r7: List(Text)]\n"
        " host r7, Args.list, r0\n ret r7\nend\n";
    struct mo_buffer wire;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    struct mo_assemble_error assembly;
    MO_ASSERT(mo_bytecode_assemble(source, strlen(source), &wire, &assembly));
    struct mo_load_error load_error;
    struct mo_image *image = mo_load(wire.bytes, wire.length, &load_error);
    MO_ASSERT(image != NULL);
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    MO_ASSERT(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    struct mo_process *p = mo_process_new(image, &limits);
    MO_ASSERT(p != NULL);
    struct mo_heap *heap = mo_process_heap(p);
    struct mo_host *host = mo_process_host(p);
    struct mo_cell text, args, env, result;
    MO_ASSERT(mo_value_text(heap, "first", 5, &text));
    MO_ASSERT(mo_value_list(heap, host->types->texts, &text, 1, &args));
    MO_ASSERT(mo_value_map(heap, host->types->files, NULL, NULL, 0, &env));
    int folder = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    MO_ASSERT(folder >= 0);
    for (unsigned call = 0; call < 2; call++) {
        CHECK(mo_call_main(p, 0, &args, &env, folder, NULL, &result, &defect) == MO_RETURNED);
        struct mo_cell item;
        CHECK(mo_list_size(result) == 1);
        MO_ASSERT(mo_list_get(result, 0, &item));
        bool equal;
        CHECK(mo_value_equal(heap, host->types->text, text, item, &equal) && equal);
        mo_value_drop(heap, host->types->texts, result);
        CHECK(host->open_folders == 2);
        if (call == 0)
            CHECK(close(folder) == 0); /* capabilities retain their own handles */
    }
    mo_value_drop(heap, host->types->text, text);
    mo_value_drop(heap, host->types->texts, args);
    mo_value_drop(heap, host->types->files, env);
    mo_process_free(p);
    mo_image_free(image);
    mo_buffer_free(&wire);
}

static void test_supplied_main_capabilities(void) {
    const char *source =
        "bytecode 0.1 shorthand\nmodule M exposes [M.main]\n"
        "function M.main(left: Clock borrowed, right: Clock owned): Time function in M\n"
        " registers [r2: Time]\n host r2, Clock.now, r1\n drop r1\n ret r2\nend\n";
    struct mo_buffer wire;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    struct mo_assemble_error assembly;
    MO_ASSERT(mo_bytecode_assemble(source, strlen(source), &wire, &assembly));
    struct mo_load_error error;
    struct mo_image *image = mo_load(wire.bytes, wire.length, &error);
    MO_ASSERT(image != NULL);
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    MO_ASSERT(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    struct mo_process *p = mo_process_new(image, &limits);
    MO_ASSERT(p != NULL);
    struct mo_heap *heap = mo_process_heap(p);
    struct mo_host *host = mo_process_host(p);
    uint32_t type = host->types->capabilities[MO_CAP_CLOCK];
    struct mo_cell clocks[2], instant = mo_cell_time(-17), result;
    struct mo_host_error host_error;
    MO_ASSERT(mo_host_call(host, heap, MO_HOST_CLOCK_FIXED, type, &instant, &clocks[0],
                           &host_error) == MO_HOST_RETURNED);
    instant = mo_cell_time(31);
    MO_ASSERT(mo_host_call(host, heap, MO_HOST_CLOCK_FIXED, type, &instant, &clocks[1],
                           &host_error) == MO_HOST_RETURNED);
    struct mo_main_capabilities supplied = {.values = clocks, .count = 2};
    for (unsigned call = 0; call < 2; call++) {
        CHECK(mo_call_main(p, 0, NULL, NULL, -1, &supplied, &result, &defect) == MO_RETURNED);
        CHECK(mo_cell_get_time(result) == 31);
    }
    mo_value_drop(heap, type, clocks[0]);
    mo_value_drop(heap, type, clocks[1]);
    CHECK(mo_heap_objects(heap) == 0);
    mo_process_free(p);
    mo_image_free(image);
    mo_buffer_free(&wire);
}

static void test_vm_host_exhaustion(void) {
    const char *source =
        "bytecode 0.1 shorthand\n"
        /* Host signatures require both boxed layouts; nominal declarations
         * reserve t0/t1 before their Text field introduces t2. */
        "layouts\n L0 case t0 3 [t2, t2]\n L1 case t1 1 [t0]\nend\n"
        "choice FileError\n case NotFound(path: Text)\n"
        " case NotAllowed(path: Text)\n case NotText(path: Text)\n"
        " case Failed(path: Text, reason: Text)\nend\n"
        "choice Result(Unit, FileError)\n case Ok(Unit)\n case Error(FileError)\nend\n"
        "module M exposes []\n"
        "function M.fake(m: Map(Text, Text) owned) : Int promise spec in M\n"
        " registers [r1: Files, r2: Text, r3: Int, r4: Result(Unit, FileError)]\n"
        " host r1, Files.in_memory, r0\n const r2, \"a/b/c/d/e/f/g/h/i/j/k/l\"\n"
        " host r4, Files.make_folder, r1, r2\n drop r4\n drop r1\n"
        " const r3, 37\n ret r3\nend\n";
    struct mo_buffer wire;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    struct mo_assemble_error assembly;
    bool assembled = mo_bytecode_assemble(source, strlen(source), &wire, &assembly);
    if (!assembled)
        CHECK(fprintf(stderr, "assembly: %s at %u\n", assembly.reason, assembly.offset) > 0);
    MO_ASSERT(assembled);
    struct mo_load_error load_error;
    struct mo_image *image = mo_load(wire.bytes, wire.length, &load_error);
    MO_ASSERT(image != NULL);
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    MO_ASSERT(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    unsigned constructor = 0, update = 0, returned = 0;
    for (size_t size = 512; size <= 16384; size += 128) {
        limits.heap_size = size;
        struct mo_process *p = mo_process_new(image, &limits);
        if (p == NULL)
            continue;
        struct mo_heap *heap = mo_process_heap(p);
        const struct mo_host *host = mo_process_host(p);
        struct mo_cell map, result = mo_cell_int(123);
        if (!mo_value_map(heap, host->types->files, NULL, NULL, 0, &map)) {
            mo_process_free(p);
            continue;
        }
        enum mo_outcome outcome = mo_call(p, 0, &map, 1, &result, &defect);
        if (outcome == MO_RETURNED) {
            returned++;
            CHECK(mo_cell_get_int(result) == 37 && mo_heap_objects(heap) == 0);
        } else {
            CHECK(outcome == MO_STOPPED && mo_cell_get_int(result) == 123 &&
                  strcmp(defect.reason, "running out of memory") == 0 &&
                  strcmp(defect.limit_name, "--max-heap") == 0);
            bool readable[5];
            MO_ASSERT(
                mo_image_registers_readable(image, 0, defect.frames->pc, readable, &load_error));
            if (defect.frames->pc == 0) {
                constructor++;
                CHECK(readable[0] && !readable[1] && !readable[2]);
            } else {
                update++;
                CHECK(defect.frames->pc == 7 && !readable[0] && readable[1] && readable[2] &&
                      !readable[4]);
            }
        }
        mo_process_free(p);
    }
    CHECK(constructor != 0 && update != 0 && returned != 0);
    mo_image_free(image);
    mo_buffer_free(&wire);
}

static MO_MUST_USE struct mo_cell sequence_text(struct host_fixture *f, const char *hex) {
    uint8_t bytes[1024];
    size_t length = strcmp(hex, "-") == 0 ? 0 : strlen(hex) / 2;
    MO_ASSERT(length <= sizeof bytes);
    for (size_t i = 0; i < length; i++) {
        unsigned byte;
        MO_ASSERT(sscanf(hex + 2 * i, "%2x", &byte) == 1 && byte <= UINT8_MAX);
        bytes[i] = (uint8_t)byte;
    }
    struct mo_cell value;
    MO_ASSERT(mo_value_text(f->heap, (const char *)bytes, length, &value));
    return value;
}

/* The Lean runner replays each prefix separately; this driver keeps the same
 * capability objects across every operation, including retained snapshots. */
static MO_MUST_USE int sequence(void) {
    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    struct mo_cell handles[32];
    uint32_t types[32] = {0};
    bool live[32] = {false};
    unsigned operation, source, destination;
    long long low, high;
    char path[2049], text[2049];
    int read;
    while ((read = scanf("%u %u %u %2048s %2048s %lld %lld", &operation, &source, &destination,
                         path, text, &low, &high)) == 7) {
        MO_ASSERT(operation < MO_HOST_COUNT && source < 32 && destination < 32);
        struct mo_cell p = sequence_text(&f, path), t = sequence_text(&f, text);
        struct mo_cell args[] = {live[source] ? handles[source] : mo_cell_unit(), p, t};
        uint32_t temporary_type = MO_NO_INDEX;
        if (operation == MO_HOST_FILES_IN_MEMORY || operation == MO_HOST_ENV_OF) {
            temporary_type = f.types.files;
            struct mo_cell keys[] = {p, t}, values[] = {t, p};
            MO_ASSERT(low >= 0 && low <= 2);
            MO_ASSERT(mo_value_map(f.heap, temporary_type, keys, values, (uint32_t)low, &args[0]));
        } else if (operation == MO_HOST_ARGS_OF || operation == MO_HOST_CONSOLE_FAKE_INPUT) {
            temporary_type = f.types.texts;
            struct mo_cell lines[] = {p, t};
            MO_ASSERT(mo_value_list(f.heap, temporary_type, lines, 2, &args[0]));
        } else if (operation == MO_HOST_CLOCK_FIXED)
            args[0] = mo_cell_time((int64_t)low);
        else if (operation == MO_HOST_RANDOM_SEEDED)
            args[0] = mo_cell_int((int64_t)low);
        else if (operation == MO_HOST_RANDOM_INT) {
            args[1] = mo_cell_int((int64_t)low);
            args[2] = mo_cell_int((int64_t)high);
        } else if (operation == MO_HOST_CONSOLE_PRINT || operation == MO_HOST_CONSOLE_ERROR)
            args[1] = t;
        uint32_t type = host_result_type(&f, operation);
        struct mo_cell result;
        struct mo_host_error error;
        enum mo_host_outcome outcome =
            mo_host_call(&f.host, f.heap, operation, type, args, &result, &error);
        struct mo_buffer printed;
        mo_buffer_init(&printed, MO_LIMIT_STRING_SIZE);
        if (outcome == MO_HOST_RETURNED) {
            MO_ASSERT(mo_value_print(f.heap, type, result, &printed));
            MO_ASSERT(printf("returned ") >= 0);
        } else {
            MO_ASSERT(outcome == MO_HOST_BUG);
            MO_ASSERT(mo_buffer_append_text(&printed, error.reason));
            if (error.has_detail) {
                size_t length;
                const uint8_t *bytes = mo_value_data(error.detail, &length);
                MO_ASSERT(mo_buffer_append_text(&printed, ": ") &&
                          mo_buffer_append(&printed, bytes, length));
            }
            MO_ASSERT(printf("stopped ") >= 0);
        }
        for (size_t i = 0; i < printed.length; i++)
            MO_ASSERT(printf("%02x", (unsigned)printed.bytes[i]) >= 0);
        MO_ASSERT(putchar('\n') != EOF);
        mo_buffer_free(&printed);
        if (temporary_type != MO_NO_INDEX && outcome != MO_HOST_RETURNED)
            mo_value_drop(f.heap, temporary_type, args[0]);
        if (outcome == MO_HOST_RETURNED) {
            if (f.types.types.items[type].kind == MO_TYPE_CAPABILITY) {
                if (live[destination])
                    mo_value_drop(f.heap, types[destination], handles[destination]);
                handles[destination] = result;
                types[destination] = type;
                live[destination] = true;
            } else
                mo_value_drop(f.heap, type, result);
        }
        mo_value_drop(f.heap, f.types.text, p);
        mo_value_drop(f.heap, f.types.text, t);
    }
    MO_ASSERT(read == EOF);
    for (unsigned i = 0; i < 32; i++)
        if (live[i])
            mo_value_drop(f.heap, types[i], handles[i]);
    MO_ASSERT(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--sequence") == 0)
        return sequence();
    struct heap_fixture fixture;
    heap_fixture_init(&fixture);
    test_console(&fixture);
    test_fake_restore();
    test_fake_files();
    test_fake_file_keys();
    test_fake_folder_exhaustion();
    test_real_files(false);
    test_real_files(true);
    test_real_console();
    test_nonblocking_console();
    test_deep_path(false);
    test_deep_path(true);
    test_hostile_links(false);
    test_hostile_links(true);
    test_large_input(false);
    test_large_input(true);
    test_clock_random();
    test_empty_image();
    test_main_capabilities();
    test_supplied_main_capabilities();
    test_vm_host_exhaustion();
    mo_heap_image_free(fixture.image);
    mo_arena_free(&fixture.arena);
    return mo_test_report("host");
}
