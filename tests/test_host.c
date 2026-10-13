#define _POSIX_C_SOURCE 200809L
#include "host/host.h"
#include "host_fixture.h"
#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
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
        expect(&f, MO_HOST_FILES_REMOVE, f.result_unit, files, "escape", "", "Ok(Unit)");
        expect(&f, MO_HOST_FILES_READ_TEXT, f.result_text, files, "a", "", "Ok(\"hello!\")");
        expect(&f, MO_HOST_FILES_REMOVE, f.result_unit, files, "inside", "", "Ok(Unit)");
        expect(&f, MO_HOST_FILES_REMOVE, f.result_unit, files, "absolute", "", "Ok(Unit)");
        expect(&f, MO_HOST_FILES_REMOVE, f.result_unit, files, "slash", "", "Ok(Unit)");
        pending_root(&f, files);
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

static void test_real_console(void) {
    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
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
    result = mo_cell_int(19);
    CHECK(mo_host_call(&f.host, f.heap, MO_HOST_CONSOLE_PRINT, f.unit, args, &result, &error) ==
          MO_HOST_BUG);
    CHECK(mo_cell_get_int(result) == 19 &&
          strcmp(error.reason, "printing to an output that is gone") == 0);
    mo_value_drop(f.heap, f.types.text, args[1]);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_CONSOLE], console);
    CHECK(close(input[0]) == 0 && close(output[1]) == 0);
    CHECK(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
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
    CHECK(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
}

int main(void) {
    struct heap_fixture fixture;
    heap_fixture_init(&fixture);
    test_console(&fixture);
    test_fake_files();
    test_fake_folder_exhaustion();
    test_real_files(false);
    test_real_files(true);
    test_real_console();
    test_clock_random();
    mo_heap_image_free(fixture.image);
    mo_arena_free(&fixture.arena);
    return mo_test_report("host");
}
