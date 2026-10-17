#include "host_fixture.h"
#include "test.h"
#include "tools/fakes.h"

#include <stdio.h>
#include <string.h>

struct fixture {
    struct host_fixture host;
    struct mo_fakes fakes;
};

static void fixture_init(struct fixture *f) {
    host_fixture_init(&f->host, mo_limits_default().heap_size);
    mo_fakes_init(&f->fakes, &f->host.source);
    MO_ASSERT(mo_fakes_count(&f->fakes) == MO_CAP_RANDOM + 1);
    mo_fakes_bind(&f->fakes, &f->host.host);
}

static void fixture_free(struct fixture *f) {
    host_fixture_free(&f->host);
}

static const struct mo_input_factory *factory(struct fixture *f, enum mo_capability kind) {
    for (uint32_t i = 0; i < mo_fakes_count(&f->fakes); i++)
        if (f->fakes.factories[i].type == f->host.capabilities[kind])
            return &f->fakes.factories[i];
    MO_ASSERT(false);
    return NULL;
}

static struct mo_cell boundary(struct fixture *f, enum mo_capability kind, uint32_t which) {
    const struct mo_input_factory *maker = factory(f, kind);
    struct mo_input_request request = {.kind = MO_INPUT_BOUNDARY, .boundary = which};
    struct mo_input_limits limits = mo_input_limits_default();
    struct mo_cell out;
    MO_ASSERT(which < maker->boundaries);
    MO_ASSERT(maker->build(maker->context, f->host.heap, &request, &limits, &out) == MO_INPUT_OK);
    return out;
}

/* Calls one capability operation on the fake and compares the printed result. */
static void expect(struct fixture *f, uint32_t operation, struct mo_cell capability,
                   const char *first, const char *second, const char *expected) {
    struct host_fixture *h = &f->host;
    uint32_t type = host_result_type(h, operation);
    struct mo_cell args[] = {capability, host_text(h, first), host_text(h, second)};
    struct mo_host_error error;
    struct mo_cell result;
    enum mo_host_outcome outcome =
        mo_host_call(&h->host, h->heap, operation, type,
                     operation == MO_HOST_RANDOM_INT
                         ? (struct mo_cell[]){capability, mo_cell_int(0), mo_cell_int(9)}
                         : args,
                     &result, &error);
    CHECK(outcome == MO_HOST_RETURNED);
    if (outcome == MO_HOST_RETURNED) {
        struct mo_buffer printed;
        mo_buffer_init(&printed, MO_LIMIT_STRING_SIZE);
        CHECK(mo_value_print(h->heap, type, result, &printed));
        bool equal = expected == NULL || (printed.length == strlen(expected) &&
                                          memcmp(printed.bytes, expected, printed.length) == 0);
        if (!equal)
            CHECK(fprintf(stderr, "expected %s, got %.*s\n", expected, (int)printed.length,
                          printed.bytes) >= 0);
        CHECK(equal);
        mo_buffer_free(&printed);
        mo_value_drop(h->heap, type, result);
    }
    mo_value_drop(h->heap, h->types.text, args[1]);
    mo_value_drop(h->heap, h->types.text, args[2]);
}

static void drop(struct fixture *f, enum mo_capability kind, struct mo_cell fake) {
    mo_value_drop(f->host.heap, f->host.capabilities[kind], fake);
}

static void files(void) {
    struct fixture f;
    fixture_init(&f);
    uint32_t type = MO_CAP_FILES;
    struct mo_cell none = boundary(&f, type, 0), one = boundary(&f, type, 1),
                   all = boundary(&f, type, 2);
    expect(&f, MO_HOST_FILES_EXISTS, none, "a.txt", "", "false");
    expect(&f, MO_HOST_FILES_EXISTS, one, "a.txt", "", "true");
    expect(&f, MO_HOST_FILES_EXISTS, one, "b.md", "", "false");
    expect(&f, MO_HOST_FILES_EXISTS, all, "notes/deep/e.txt", "", "true");
    expect(&f, MO_HOST_FILES_EXISTS, all, "notes/missing", "", "false");
    expect(&f, MO_HOST_FILES_READ_TEXT, one, "a.txt", "", "Ok(\"x\")");
    expect(&f, MO_HOST_FILES_READ_TEXT, all, "a.txt", "", "Ok(\"\")");
    expect(&f, MO_HOST_FILES_READ_TEXT, all, "b.md", "", "Ok(\"x\")");
    expect(&f, MO_HOST_FILES_LIST, none, ".", "", "Ok([])");
    expect(&f, MO_HOST_FILES_LIST, all, ".", "", "Ok([\"a.txt\", \"b.md\", \"notes\"])");
    drop(&f, type, none);
    drop(&f, type, one);
    drop(&f, type, all);
    CHECK(mo_heap_objects(f.host.heap) == 0);
    fixture_free(&f);
}

static void read_only_files(void) {
    struct fixture f;
    fixture_init(&f);
    struct mo_cell all = boundary(&f, MO_CAP_READ_FILES, 2);
    expect(&f, MO_HOST_READFILES_READ_TEXT, all, "notes/c.txt", "", "Ok(\"one\\ntwo\\n\")");
    drop(&f, MO_CAP_READ_FILES, all);
    CHECK(mo_heap_objects(f.host.heap) == 0);
    fixture_free(&f);
}

static void console_args_env(void) {
    struct fixture f;
    fixture_init(&f);
    struct mo_cell none = boundary(&f, MO_CAP_CONSOLE, 0), one = boundary(&f, MO_CAP_CONSOLE, 1);
    expect(&f, MO_HOST_CONSOLE_READ_LINE, none, "", "", "None");
    expect(&f, MO_HOST_CONSOLE_READ_LINE, one, "", "", "Some(\"x\")");
    drop(&f, MO_CAP_CONSOLE, none);
    drop(&f, MO_CAP_CONSOLE, one);
    struct mo_cell args = boundary(&f, MO_CAP_ARGS, 2);
    expect(&f, MO_HOST_ARGS_LIST, args, "", "",
           "[\"\", \"a\", \"--help\", \"-x\", \"file.txt\", \"caf\xc3\xa9\", \"two words\"]");
    drop(&f, MO_CAP_ARGS, args);
    struct mo_cell empty = boundary(&f, MO_CAP_ENV, 0), full = boundary(&f, MO_CAP_ENV, 2);
    expect(&f, MO_HOST_ENV_GET, empty, "HOME", "", "None");
    expect(&f, MO_HOST_ENV_GET, full, "HOME", "", "Some(\"/home/mo\")");
    expect(&f, MO_HOST_ENV_GET, full, "EMPTY", "", "Some(\"\")");
    expect(&f, MO_HOST_ENV_GET, full, "MISSING", "", "None");
    drop(&f, MO_CAP_ENV, empty);
    drop(&f, MO_CAP_ENV, full);
    CHECK(mo_heap_objects(f.host.heap) == 0);
    fixture_free(&f);
}

static void clock_random(void) {
    struct fixture f;
    fixture_init(&f);
    struct mo_cell epoch = boundary(&f, MO_CAP_CLOCK, 0), first = boundary(&f, MO_CAP_CLOCK, 1),
                   last = boundary(&f, MO_CAP_CLOCK, 2);
    expect(&f, MO_HOST_CLOCK_NOW, epoch, "", "", "1970-01-01T00:00:00Z");
    expect(&f, MO_HOST_CLOCK_NOW, first, "", "", "0001-01-01T00:00:00Z");
    expect(&f, MO_HOST_CLOCK_NOW, last, "", "", "9999-12-31T23:59:59.999999Z");
    drop(&f, MO_CAP_CLOCK, epoch);
    drop(&f, MO_CAP_CLOCK, first);
    drop(&f, MO_CAP_CLOCK, last);
    /* Random inputs land in the years 1 to 9999, so Clock.now never stops. */
    const struct mo_input_factory *clock = factory(&f, MO_CAP_CLOCK);
    for (uint64_t seed = 0; seed < 64; seed++) {
        struct mo_input_request request = {.kind = MO_INPUT_RANDOM, .size = 3, .seed = seed * seed};
        struct mo_input_limits limits = mo_input_limits_default();
        struct mo_cell made;
        CHECK(clock->build(clock->context, f.host.heap, &request, &limits, &made) == MO_INPUT_OK);
        expect(&f, MO_HOST_CLOCK_NOW, made, "", "", NULL);
        drop(&f, MO_CAP_CLOCK, made);
    }
    fixture_free(&f);
}

static void copies_start_from_the_recipe(void) {
    struct fixture f;
    fixture_init(&f);
    struct mo_cell all = boundary(&f, MO_CAP_FILES, 2);
    mo_fakes_keep(&f.fakes);
    expect(&f, MO_HOST_FILES_REMOVE, all, "a.txt", "", "Ok(Unit)");
    expect(&f, MO_HOST_FILES_EXISTS, all, "a.txt", "", "false");
    const struct mo_input_factory *maker = factory(&f, MO_CAP_FILES);
    struct mo_input_limits limits = mo_input_limits_default();
    struct mo_cell copy;
    mo_fakes_bind(&f.fakes, &f.host.host);
    CHECK(maker->copy(maker->context, f.host.heap, all, &limits, &copy) == MO_INPUT_OK);
    expect(&f, MO_HOST_FILES_EXISTS, copy, "a.txt", "", "true");
    expect(&f, MO_HOST_FILES_EXISTS, all, "a.txt", "", "false");
    /* A fake nobody kept a recipe for cannot be copied. */
    struct mo_cell other = boundary(&f, MO_CAP_FILES, 0);
    mo_fakes_bind(&f.fakes, &f.host.host);
    struct mo_cell refused = mo_cell_int(7);
    CHECK(maker->copy(maker->context, f.host.heap, other, &limits, &refused) ==
          MO_INPUT_UNSUPPORTED);
    CHECK(mo_cell_get_int(refused) == 7);
    drop(&f, MO_CAP_FILES, other);
    drop(&f, MO_CAP_FILES, copy);
    drop(&f, MO_CAP_FILES, all);
    CHECK(mo_heap_objects(f.host.heap) == 0);
    fixture_free(&f);
}

static void same_request_same_fake(void) {
    struct fixture f;
    fixture_init(&f);
    const struct mo_input_factory *maker = factory(&f, MO_CAP_FILES);
    struct mo_input_request request = {.kind = MO_INPUT_RANDOM, .size = 5, .seed = 0x1234abcd};
    struct mo_cell made[2];
    for (unsigned i = 0; i < 2; i++) {
        struct mo_input_limits limits = mo_input_limits_default();
        CHECK(maker->build(maker->context, f.host.heap, &request, &limits, &made[i]) ==
              MO_INPUT_OK);
    }
    static const char *const paths[] = {"a.txt", "b.md", "notes/c.txt", "notes/d.md",
                                        "notes/deep/e.txt"};
    for (unsigned p = 0; p < 5; p++)
        for (unsigned i = 0; i < 2; i++) {
            struct mo_buffer printed[2];
            for (unsigned k = 0; k < 2; k++) {
                struct mo_cell args[] = {made[k], host_text(&f.host, paths[p])};
                struct mo_host_error error;
                struct mo_cell result;
                CHECK(mo_host_call(&f.host.host, f.host.heap, MO_HOST_FILES_READ_TEXT,
                                   f.host.result_text, args, &result, &error) == MO_HOST_RETURNED);
                mo_buffer_init(&printed[k], MO_LIMIT_STRING_SIZE);
                CHECK(mo_value_print(f.host.heap, f.host.result_text, result, &printed[k]));
                mo_value_drop(f.host.heap, f.host.result_text, result);
                mo_value_drop(f.host.heap, f.host.types.text, args[1]);
            }
            CHECK(printed[0].length == printed[1].length &&
                  memcmp(printed[0].bytes, printed[1].bytes, printed[0].length) == 0);
            mo_buffer_free(&printed[0]);
            mo_buffer_free(&printed[1]);
        }
    drop(&f, MO_CAP_FILES, made[0]);
    drop(&f, MO_CAP_FILES, made[1]);
    fixture_free(&f);
}

static void limits(void) {
    struct fixture f;
    fixture_init(&f);
    const struct mo_input_factory *maker = factory(&f, MO_CAP_FILES);
    struct mo_input_request request = {.kind = MO_INPUT_BOUNDARY, .boundary = 2};
    /* The five files of boundary 2 are 46 bytes of paths and 43 of texts, and 6 nodes. */
    struct mo_input_limits full = mo_input_limits_default();
    struct mo_cell made;
    CHECK(maker->build(maker->context, f.host.heap, &request, &full, &made) == MO_INPUT_OK);
    drop(&f, MO_CAP_FILES, made);
    size_t bytes = mo_input_limits_default().bytes - full.bytes;
    uint32_t nodes = mo_input_limits_default().nodes - full.nodes;
    CHECK(nodes == 6 && bytes == 89);
    struct mo_input_limits at = mo_input_limits_default();
    at.bytes = bytes;
    at.nodes = nodes;
    CHECK(maker->build(maker->context, f.host.heap, &request, &at, &made) == MO_INPUT_OK);
    drop(&f, MO_CAP_FILES, made);
    CHECK(at.bytes == 0 && at.nodes == 0);
    struct mo_cell untouched = mo_cell_int(5);
    struct mo_input_limits past = mo_input_limits_default();
    past.bytes = bytes - 1;
    CHECK(maker->build(maker->context, f.host.heap, &request, &past, &untouched) == MO_INPUT_LIMIT);
    past = mo_input_limits_default();
    past.nodes = nodes - 1;
    CHECK(maker->build(maker->context, f.host.heap, &request, &past, &untouched) == MO_INPUT_LIMIT);
    CHECK(mo_cell_get_int(untouched) == 5 && mo_heap_objects(f.host.heap) == 0);

    /* A tuple holds at most MO_CLI_FAKE_RECIPES fakes; one more is refused and not leaked. */
    const struct mo_input_factory *args = factory(&f, MO_CAP_ARGS);
    request.boundary = 0;
    struct mo_cell fakes[MO_CLI_FAKE_RECIPES];
    mo_fakes_bind(&f.fakes, &f.host.host);
    for (unsigned i = 0; i < MO_CLI_FAKE_RECIPES; i++) {
        struct mo_input_limits limits = mo_input_limits_default();
        CHECK(args->build(args->context, f.host.heap, &request, &limits, &fakes[i]) == MO_INPUT_OK);
    }
    struct mo_input_limits spare = mo_input_limits_default();
    size_t objects = mo_heap_objects(f.host.heap);
    CHECK(args->build(args->context, f.host.heap, &request, &spare, &untouched) == MO_INPUT_LIMIT);
    CHECK(mo_cell_get_int(untouched) == 5 && mo_heap_objects(f.host.heap) == objects);
    for (unsigned i = 0; i < MO_CLI_FAKE_RECIPES; i++)
        drop(&f, MO_CAP_ARGS, fakes[i]);
    CHECK(mo_heap_objects(f.host.heap) == 0);
    fixture_free(&f);
}

/* A heap too small for a fake refuses it and leaks nothing, whichever allocation fails. */
static void small_heaps(void) {
    for (size_t heap_size = 256; heap_size < 6144; heap_size += 64) {
        struct fixture f;
        host_fixture_init(&f.host, heap_size);
        mo_fakes_init(&f.fakes, &f.host.source);
        mo_fakes_bind(&f.fakes, &f.host.host);
        for (unsigned kind = 0; kind <= MO_CAP_RANDOM; kind++) {
            const struct mo_input_factory *maker = factory(&f, (enum mo_capability)kind);
            struct mo_input_request request = {.kind = MO_INPUT_BOUNDARY, .boundary = 2};
            struct mo_input_limits limits = mo_input_limits_default();
            size_t objects = mo_heap_objects(f.host.heap);
            struct mo_cell out = mo_cell_int(3);
            enum mo_input_result result =
                maker->build(maker->context, f.host.heap, &request, &limits, &out);
            CHECK(result == MO_INPUT_OK || result == MO_INPUT_NO_MEMORY);
            if (result == MO_INPUT_OK)
                drop(&f, (enum mo_capability)kind, out);
            else
                CHECK(mo_cell_get_int(out) == 3);
            CHECK(mo_heap_objects(f.host.heap) == objects);
            mo_fakes_bind(&f.fakes, &f.host.host);
        }
        fixture_free(&f);
    }
}

int main(void) {
    files();
    read_only_files();
    console_args_env();
    clock_random();
    copies_start_from_the_recipe();
    same_request_same_fake();
    limits();
    small_heaps();
    return mo_test_report("test_fakes");
}
