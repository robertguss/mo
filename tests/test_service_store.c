#define _POSIX_C_SOURCE 200809L
#include "base/binary.h"
#include "base/stack.h"
#include "base/store-memory.h"
#include "codegen/codegen.h"
#include "lower/lower.h"
#include "program_fixture.h"
#include "test.h"
#include "vm/vm.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned crash_point, log_steps;
MO_MUST_USE enum mo_store_status
mo_store_fault_call(struct mo_store *store, enum mo_store_operation operation,
                    const struct mo_store_record *record,
                    bool (*consume)(const struct mo_store_record *, void *), void *context);
int mo_store_fault_step(sqlite3_stmt *stmt);

enum mo_store_status mo_store_fault_call(struct mo_store *store, enum mo_store_operation operation,
                                         const struct mo_store_record *record,
                                         bool (*consume)(const struct mo_store_record *, void *),
                                         void *context) {
    if (operation == MO_STORE_APPEND && crash_point == 1)
        _exit(71);
    if (operation == MO_STORE_SNAPSHOT && crash_point == 6)
        return MO_STORE_IO;
    enum mo_store_status result = mo_store_call(store, operation, record, consume, context);
    if (operation == MO_STORE_APPEND && result == MO_STORE_OK && crash_point == 2)
        _exit(72);
    if (operation == MO_STORE_APPEND && crash_point == 4)
        return MO_STORE_IO;
    return result;
}

int mo_store_fault_step(sqlite3_stmt *stmt) {
    int result = sqlite3_step(stmt);
    if (crash_point == 3 && result == SQLITE_DONE &&
        strstr(sqlite3_sql(stmt), "INSERT INTO snapshot") != NULL)
        _exit(73);
    const char *sql = sqlite3_sql(stmt);
    if (crash_point == 7 && strstr(sql, "SELECT record FROM snapshot") != NULL)
        return SQLITE_IOERR;
    if (crash_point == 8 && strstr(sql, "FROM log") != NULL && ++log_steps == 2)
        return SQLITE_IOERR;
    if (crash_point == 9 && strstr(sql, "SELECT seq FROM head") != NULL)
        return SQLITE_BUSY;
    if (crash_point == 10 && strstr(sql, "PRAGMA user_version") != NULL)
        return SQLITE_NOMEM;
    if (crash_point == 11 && strstr(sql, "FROM log") != NULL)
        return SQLITE_CORRUPT;
    if (crash_point == 12 && strstr(sql, "SELECT seq FROM head") != NULL)
        return SQLITE_CORRUPT;
    if (crash_point == 5 && result == SQLITE_DONE &&
        strstr(sqlite3_sql(stmt), "INSERT INTO log") != NULL)
        return SQLITE_IOERR;
    return result;
}

struct observations {
    struct mo_service_time time;
    unsigned replies, failures, stops;
    enum mo_service_status status;
    struct mo_store *store;
    uint64_t sequence;
};
static bool last_sequence(const struct mo_store_record *record, void *context) {
    *(uint64_t *)context = record->sequence;
    return true;
}
static struct mo_service_time clock_now(void *context) {
    return ((struct observations *)context)->time;
}
static void reply(const struct mo_service_response *r, void *context) {
    struct observations *o = context;
    o->replies++;
    o->status = r->status;
    struct mo_cell none = {0};
    if (r->status != MO_SERVICE_OK)
        CHECK(memcmp(&r->value, &none, sizeof none) == 0);
    if (r->updated) {
        uint64_t sequence = 0;
        CHECK(mo_store_call(o->store, MO_STORE_LOAD, NULL, last_sequence, &sequence) ==
              MO_STORE_OK);
        CHECK(sequence == ++o->sequence);
    }
}
static void event(const struct mo_service_event *e, void *context) {
    struct observations *o = context;
    o->failures += e->kind == MO_SERVICE_FAILURE ? 1u : 0u;
    o->stops += e->kind == MO_SERVICE_STOP ? 1u : 0u;
}

static struct mo_image *compile(const char *source) {
    struct fixture f;
    init(&f, source);
    MO_ASSERT(f.program != NULL && !mo_diags_any(&f.diags));
    const char *modules[] = {"Main"};
    f.set.names = modules;
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_LOWER_WORK);
    struct mo_services services;
    struct mo_core *core =
        mo_lower_services(&arena, f.program, &f.sources, &f.diags, NULL, &services);
    MO_ASSERT(core != NULL && !mo_diags_any(&f.diags));
    struct mo_buffer bytes;
    mo_buffer_init(&bytes, MO_LIMIT_BYTECODE_SIZE);
    MO_ASSERT(mo_compile_services(core, &services, &bytes, &f.diags));
    struct mo_load_error error;
    struct mo_image *image = mo_load(bytes.bytes, bytes.length, &error);
    MO_ASSERT(image != NULL);
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    MO_ASSERT(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    mo_buffer_free(&bytes);
    mo_arena_free(&arena);
    free_fixture(&f);
    return image;
}

static struct mo_service_loop *open_service(struct mo_image *image, struct mo_store *store,
                                            struct observations *o) {
    struct mo_service_hooks hooks = {clock_now, reply, event, o};
    struct mo_service_loop *s = NULL;
    MO_ASSERT(mo_service_open(image, 0, &hooks, store, &s) == MO_SERVICE_OK);
    o->store = store;
    CHECK(mo_store_call(store, MO_STORE_LOAD, NULL, last_sequence, &o->sequence) == MO_STORE_OK);
    return s;
}

static void add(struct mo_service_loop *s, int64_t n) {
    struct mo_cell value = mo_cell_int(n);
    CHECK(mo_service_send(s, 0, &value, 1, 123) == MO_SERVICE_OK);
    CHECK(mo_service_step(s));
}

struct inventory {
    uint64_t snapshot, last;
    unsigned logs;
};
static bool count_record(const struct mo_store_record *r, void *context) {
    struct inventory *i = context;
    if (r->kind == MO_STORE_STATE)
        i->snapshot = r->sequence;
    else
        i->logs++;
    i->last = r->sequence;
    return true;
}

static void check_inventory(struct mo_store *store, uint64_t snapshot, unsigned logs) {
    struct inventory i = {0};
    CHECK(mo_store_call(store, MO_STORE_LOAD, NULL, count_record, &i) == MO_STORE_OK);
    CHECK(i.snapshot == snapshot && i.logs == logs && i.last == snapshot + logs);
}

static void memory_store(struct mo_image *image) {
    struct mo_store *store = NULL;
    MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
    struct observations o = {0};
    struct mo_service_loop *s = open_service(image, store, &o);
    for (unsigned i = 0; i < MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES + 3; i++)
        add(s, 11);
    CHECK(o.replies == MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES + 3 && o.status == MO_SERVICE_OK);
    check_inventory(store, MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES, 3);
    for (unsigned i = 0; i < 20; i++) {
        CHECK(mo_service_send(s, 1 + i % 2, NULL, 0, i) == MO_SERVICE_OK);
        CHECK(mo_service_step(s));
    }
    check_inventory(store, MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES, 3);
    CHECK(o.failures == 0 && o.stops == 0);
    for (unsigned i = 0; i < MO_LIMIT_SERVICE_FAILURES; i++) {
        CHECK(mo_service_send(s, 3, NULL, 0, i) == MO_SERVICE_OK);
        CHECK(mo_service_step(s));
    }
    CHECK(mo_service_running(s));
    CHECK(mo_cell_get_int(mo_service_state(s)) ==
          7 + 11 * (MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES + 3));
    mo_service_free(s);
    o.replies = 0;
    s = open_service(image, store, &o);
    CHECK(o.replies == 0);
    CHECK(mo_cell_get_int(mo_service_state(s)) ==
          7 + 11 * (MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES + 3));
    add(s, -19);
    CHECK(mo_cell_get_int(mo_service_state(s)) ==
          7 + 11 * (MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES + 3) - 19);
    mo_service_free(s);
    mo_store_close(store);
}

static void crashes(struct mo_image *image, const char *path) {
    for (unsigned point = 1; point <= 3; point++) {
        struct mo_store *store;
        MO_ASSERT(mo_store_open(path, &store) == MO_STORE_OK);
        struct observations o = {0};
        struct mo_service_loop *s = open_service(image, store, &o);
        unsigned before = point == 3 ? MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES - 1 : 1;
        for (unsigned i = 0; i < before; i++)
            add(s, 3);
        mo_service_free(s);
        mo_store_close(store);
        pid_t pid = fork();
        MO_ASSERT(pid >= 0);
        if (pid == 0) {
            MO_ASSERT(mo_store_open(path, &store) == MO_STORE_OK);
            s = open_service(image, store, &o);
            crash_point = point;
            add(s, 13);
            _exit(99);
        }
        int status;
        CHECK(waitpid(pid, &status, 0) == pid);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 70 + (int)point);
        MO_ASSERT(mo_store_open(path, &store) == MO_STORE_OK);
        o.replies = 0;
        s = open_service(image, store, &o);
        CHECK(o.replies == 0);
        CHECK(mo_cell_get_int(mo_service_state(s)) ==
              7 + 3 * (int64_t)before + (point == 1 ? 0 : 13));
        add(s, -5);
        CHECK(o.replies == 1 && o.status == MO_SERVICE_OK);
        mo_service_free(s);
        mo_store_close(store);
        CHECK(unlink(path) == 0);
    }
}

static void corruption(struct mo_image *image, const char *path) {
    const char *edits[] = {
        "UPDATE log SET record=substr(record,1,length(record)-1)",
        "UPDATE snapshot SET record=substr(record,1,length(record)-1)",
        "UPDATE log SET record=zeroblob(length(record))",
        "UPDATE snapshot SET record=zeroblob(length(record))",
        "DELETE FROM log WHERE seq=2",
        "DELETE FROM snapshot",
        "DELETE FROM head",
        "UPDATE head SET seq=seq-1",
        "PRAGMA user_version=0",
        "PRAGMA user_version=2",
        "DELETE FROM head; DELETE FROM snapshot; DELETE FROM log",
        "UPDATE snapshot SET record=zeroblob(16777216+4096+1)",
        "UPDATE log SET record=zeroblob(16777216+4096+1)",
    };
    for (size_t i = 0; i < sizeof edits / sizeof edits[0]; i++) {
        struct mo_store *store;
        MO_ASSERT(mo_store_open(path, &store) == MO_STORE_OK);
        struct observations o = {0};
        struct mo_service_loop *s = open_service(image, store, &o);
        add(s, 11);
        add(s, 13);
        mo_service_free(s);
        mo_store_close(store);
        sqlite3 *db = NULL;
        MO_ASSERT(sqlite3_open(path, &db) == SQLITE_OK);
        MO_ASSERT(sqlite3_exec(db, edits[i], NULL, NULL, NULL) == SQLITE_OK);
        CHECK(sqlite3_close(db) == SQLITE_OK);
        MO_ASSERT(mo_store_open(path, &store) == MO_STORE_OK);
        struct mo_service_hooks hooks = {clock_now, reply, event, &o};
        s = NULL;
        CHECK(mo_service_open(image, 0, &hooks, store, &s) == MO_SERVICE_SAVED_INVALID);
        CHECK(s == NULL && o.replies == 2);
        mo_store_close(store);
        CHECK(unlink(path) == 0);
    }
}

static void uncertain_commit(struct mo_image *image) {
    struct mo_store *store;
    MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
    struct observations o = {0};
    struct mo_service_loop *s = open_service(image, store, &o);
    crash_point = 4;
    add(s, 11);
    crash_point = 0;
    CHECK(o.status == MO_SERVICE_STORE && !mo_service_running(s) && o.stops == 1);
    CHECK(strcmp(mo_service_status_text(o.status),
                 "service store failed; reopen it to see whether the last change was saved") == 0);
    CHECK(mo_cell_get_int(mo_service_state(s)) == 7);
    mo_service_free(s);
    s = open_service(image, store, &o);
    CHECK(mo_cell_get_int(mo_service_state(s)) == 18);
    crash_point = 5;
    add(s, 5);
    crash_point = 0;
    CHECK(o.status == MO_SERVICE_STORE && !mo_service_running(s));
    mo_service_free(s);
    s = open_service(image, store, &o);
    CHECK(mo_cell_get_int(mo_service_state(s)) == 18);
    check_inventory(store, 0, 1);
    mo_service_free(s);
    crash_point = 7;
    struct mo_service_hooks hooks = {clock_now, reply, event, &o};
    s = NULL;
    CHECK(mo_service_open(image, 0, &hooks, store, &s) == MO_SERVICE_STORE && s == NULL);
    crash_point = 0;
    s = open_service(image, store, &o);
    CHECK(mo_cell_get_int(mo_service_state(s)) == 18);
    mo_service_free(s);
    mo_store_close(store);
}

static void failed_snapshot(struct mo_image *image) {
    struct mo_store *store;
    MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
    struct observations o = {0};
    struct mo_service_loop *s = open_service(image, store, &o);
    for (unsigned i = 1; i < MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES; i++)
        add(s, 1);
    crash_point = 6;
    add(s, 1);
    crash_point = 0;
    CHECK(o.status == MO_SERVICE_STORE && !mo_service_running(s));
    check_inventory(store, 0, MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES);
    mo_service_free(s);
    s = open_service(image, store, &o);
    CHECK(mo_cell_get_int(mo_service_state(s)) == 7 + MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES);
    check_inventory(store, MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES, 0);
    mo_service_free(s);
    crash_point = 6;
    struct mo_service_hooks hooks = {clock_now, reply, event, &o};
    s = NULL;
    CHECK(mo_service_open(image, 0, &hooks, store, &s) == MO_SERVICE_OK);
    crash_point = 0;
    mo_service_free(s);
    mo_store_close(store);
}

static bool payload_length(const struct mo_store_record *record, void *context) {
    *(size_t *)context = record->length;
    return true;
}

static void payload_limit(const char *path) {
    struct mo_buffer bytes;
    mo_buffer_init(&bytes, MO_LIMIT_SAVED_BYTES + 1);
    for (size_t i = 0; i <= MO_LIMIT_SAVED_BYTES; i++)
        MO_ASSERT(mo_buffer_append_byte(&bytes, (uint8_t)i));
    struct mo_store *others[MO_LIMIT_STORES - 1];
    char names[MO_LIMIT_STORES - 1][300];
    for (unsigned i = 0; i < MO_LIMIT_STORES - 1; i++) {
        MO_ASSERT(snprintf(names[i], sizeof names[i], "%s.%u", path, i) > 0);
        MO_ASSERT(mo_store_open(names[i], &others[i]) == MO_STORE_OK);
        struct mo_store_record small = {MO_STORE_STATE, 0, 1, bytes.bytes, 4096};
        CHECK(mo_store_call(others[i], MO_STORE_SNAPSHOT, &small, NULL, NULL) == MO_STORE_OK);
    }
    sqlite3_int64 before = sqlite3_memory_used();
    struct mo_store *store;
    MO_ASSERT(mo_store_open(path, &store) == MO_STORE_OK);
    struct mo_store *extra = NULL;
    CHECK(mo_store_open(NULL, &extra) == MO_STORE_LIMIT && extra == NULL);
    struct mo_store_record record = {MO_STORE_STATE, 0, 1, bytes.bytes, MO_LIMIT_SAVED_BYTES + 1};
    CHECK(mo_store_call(store, MO_STORE_SNAPSHOT, &record, NULL, NULL) != MO_STORE_OK);
    record.length = MO_LIMIT_SAVED_BYTES;
    CHECK(mo_store_call(store, MO_STORE_SNAPSHOT, &record, NULL, NULL) == MO_STORE_OK);
    /* An append must not rewrite the snapshot, and the page cache stays bounded. */
    sqlite3_int64 idle = sqlite3_memory_used();
    (void)sqlite3_memory_highwater(1);
    record = (struct mo_store_record){MO_STORE_LOG, 1, 1, bytes.bytes, 4096};
    CHECK(mo_store_call(store, MO_STORE_APPEND, &record, NULL, NULL) == MO_STORE_OK);
    CHECK(sqlite3_memory_highwater(0) - idle < (1 << 20));
    CHECK(idle - before < (1 << 20));
    record = (struct mo_store_record){MO_STORE_LOG, 2, 1, bytes.bytes, MO_LIMIT_SAVED_BYTES};
    CHECK(mo_store_call(store, MO_STORE_APPEND, &record, NULL, NULL) == MO_STORE_OK);
    mo_store_close(store);
    MO_ASSERT(mo_store_open(path, &store) == MO_STORE_OK);
    size_t length = 0;
    CHECK(mo_store_call(store, MO_STORE_LOAD, NULL, payload_length, &length) == MO_STORE_OK);
    CHECK(length == MO_LIMIT_SAVED_BYTES);
    mo_store_close(store);
    for (unsigned i = 0; i < MO_LIMIT_STORES - 1; i++) {
        mo_store_close(others[i]);
        CHECK(unlink(names[i]) == 0);
    }
    mo_buffer_free(&bytes);
    CHECK(unlink(path) == 0);
}

static size_t text_overhead(struct mo_heap *heap, uint32_t type) {
    struct mo_cell empty;
    MO_ASSERT(mo_value_text(heap, "", 0, &empty));
    struct mo_buffer bytes;
    mo_buffer_init(&bytes, MO_LIMIT_SAVED_BYTES);
    MO_ASSERT(mo_value_encode(heap, type, empty, &bytes));
    size_t overhead = bytes.length;
    mo_buffer_free(&bytes);
    mo_value_drop(heap, type, empty);
    return overhead;
}

static void message_limit(void) {
    struct mo_image *image =
        compile("module Main\nservice\nstate Int = 0\ntools pair\ntoken from env \"TOKEN\"\nend\n"
                "fn pair(state: Int, _now: Time, a: Text, b: Text) : (Int, Int)\n"
                "(state + a.bytes.size + b.bytes.size, 0)\nend\n");
    struct mo_store *store;
    MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
    struct observations o = {0};
    struct mo_service_loop *s = open_service(image, store, &o);
    struct mo_heap *heap = mo_service_heap(s);
    uint32_t text = mo_image_bytecode(image)->services[0].tools[0].argument_types[0];
    /* Timer flag, handler, arrival, count and checksum, then a length per argument. */
    size_t room = MO_LIMIT_SAVED_BYTES - 25 - 2 * (4 + text_overhead(heap, text));
    size_t first = 8 << 20;
    char *bytes = calloc(room + 1, 1);
    MO_ASSERT(bytes != NULL);
    memset(bytes, 'a', room + 1);
    for (unsigned fits = 0; fits < 2; fits++) {
        size_t second = room - first + (fits ? 0 : 1);
        struct mo_cell pair[2];
        MO_ASSERT(mo_value_text(heap, bytes, first, &pair[0]));
        MO_ASSERT(mo_value_text(heap, bytes, second, &pair[1]));
        CHECK(mo_service_send(s, 0, pair, 2, fits) == (fits ? MO_SERVICE_OK : MO_SERVICE_MEMORY));
        CHECK(mo_service_pending(s) == fits);
        mo_value_drop(heap, text, pair[0]);
        mo_value_drop(heap, text, pair[1]);
    }
    free(bytes);
    CHECK(mo_service_step(s));
    CHECK(o.status == MO_SERVICE_OK && mo_service_running(s));
    CHECK(mo_cell_get_int(mo_service_state(s)) == (int64_t)room);
    mo_service_free(s);
    s = open_service(image, store, &o);
    CHECK(mo_cell_get_int(mo_service_state(s)) == (int64_t)room);
    mo_service_free(s);
    mo_store_close(store);
    mo_image_free(image);
}

static void state_limit(void) {
    struct mo_image *probe = compile(
        "module Main\nservice\nstate Text = \"\"\ntools keep\ntoken from env \"TOKEN\"\nend\n"
        "fn keep(state: Text, _now: Time) : (Text, Int)\n(state, 0)\nend\n");
    struct mo_store *store;
    MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
    struct observations o = {0};
    struct mo_service_loop *s = open_service(probe, store, &o);
    size_t room =
        MO_LIMIT_SAVED_BYTES -
        text_overhead(mo_service_heap(s), mo_image_bytecode(probe)->services[0].state_type);
    mo_service_free(s);
    mo_store_close(store);
    mo_image_free(probe);
    for (unsigned fits = 0; fits < 2; fits++) {
        char source[256];
        MO_ASSERT(snprintf(source, sizeof source,
                           "module Main\nservice\nstate Text = \"a\".repeat(%zu)\ntools keep\n"
                           "token from env \"TOKEN\"\nend\n"
                           "fn keep(state: Text, _now: Time) : (Text, Int)\n(state, 0)\nend\n",
                           room + (fits ? 0 : 1)) > 0);
        struct mo_image *image = compile(source);
        MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
        struct mo_service_hooks hooks = {clock_now, reply, event, &o};
        s = NULL;
        CHECK(mo_service_open(image, 0, &hooks, store, &s) ==
              (fits ? MO_SERVICE_OK : MO_SERVICE_MEMORY));
        mo_service_free(s);
        struct inventory i = {0};
        CHECK(mo_store_call(store, MO_STORE_LOAD, NULL, count_record, &i) == MO_STORE_OK);
        CHECK(i.logs == 0 && i.last == 0);
        if (fits) {
            s = open_service(image, store, &o);
            size_t length;
            const uint8_t *data = mo_value_data(mo_service_state(s), &length);
            CHECK(length == room && data[room - 1] == 'a');
            mo_service_free(s);
        }
        mo_store_close(store);
        mo_image_free(image);
    }
}

/* Read failures that a retry may survive are store failures; SQLite's
 * corruption reports are invalid saved data. Neither publishes a service. */
static void read_failures(struct mo_image *image) {
    struct mo_store *store;
    MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
    struct observations o = {0};
    struct mo_service_loop *s = open_service(image, store, &o);
    add(s, 1);
    add(s, 2);
    mo_service_free(s);
    struct mo_service_hooks hooks = {clock_now, reply, event, &o};
    const unsigned points[] = {7, 8, 9, 10, 11, 12};
    const enum mo_service_status expected[] = {MO_SERVICE_STORE,         MO_SERVICE_STORE,
                                               MO_SERVICE_STORE,         MO_SERVICE_STORE,
                                               MO_SERVICE_SAVED_INVALID, MO_SERVICE_SAVED_INVALID};
    for (unsigned i = 0; i < sizeof points / sizeof points[0]; i++) {
        crash_point = points[i];
        log_steps = 0;
        s = NULL;
        CHECK(mo_service_open(image, 0, &hooks, store, &s) == expected[i] && s == NULL);
        crash_point = 0;
    }
    s = open_service(image, store, &o);
    CHECK(mo_cell_get_int(mo_service_state(s)) == 10);
    mo_service_free(s);
    mo_store_close(store);
}

/* A store written by one image is refused by a service from another image,
 * and the refusal writes nothing to the file. */
static void foreign_image(struct mo_image *image, const char *path) {
    struct mo_store *store;
    MO_ASSERT(mo_store_open(path, &store) == MO_STORE_OK);
    struct observations o = {0};
    struct mo_service_loop *s = open_service(image, store, &o);
    add(s, 11);
    mo_service_free(s);
    mo_store_close(store);
    FILE *file = fopen(path, "rb");
    MO_ASSERT(file != NULL);
    static unsigned char before[1 << 16], after[1 << 16];
    size_t length = fread(before, 1, sizeof before, file);
    MO_ASSERT(length > 0 && length < sizeof before && fclose(file) == 0);
    struct mo_image *other =
        compile("module Main\nservice\nstate Int = 7\ntools add\ntoken from env \"TOKEN\"\nend\n"
                "fn add(state: Int, _now: Time, n: Int) : (Int, Int)\n(state - n, state)\nend\n");
    CHECK(mo_image_bytecode(other)->header.checksum != mo_image_bytecode(image)->header.checksum);
    MO_ASSERT(mo_store_open(path, &store) == MO_STORE_OK);
    struct mo_service_hooks hooks = {clock_now, reply, event, &o};
    s = NULL;
    CHECK(mo_service_open(other, 0, &hooks, store, &s) == MO_SERVICE_SAVED_INVALID && s == NULL);
    mo_store_close(store);
    file = fopen(path, "rb");
    MO_ASSERT(file != NULL);
    CHECK(fread(after, 1, sizeof after, file) == length && memcmp(before, after, length) == 0);
    MO_ASSERT(fclose(file) == 0);
    MO_ASSERT(mo_store_open(path, &store) == MO_STORE_OK);
    s = open_service(image, store, &o);
    CHECK(mo_cell_get_int(mo_service_state(s)) == 18);
    mo_service_free(s);
    mo_store_close(store);
    mo_image_free(other);
    CHECK(unlink(path) == 0);
}

static void hostile_database(struct mo_image *image, const char *path) {
    struct mo_store *store = NULL;
    CHECK(mo_store_open("", &store) == MO_STORE_CORRUPT && store == NULL);
    CHECK(mo_store_open(":memory:", &store) == MO_STORE_CORRUPT && store == NULL);
    MO_ASSERT(mo_store_open(path, &store) == MO_STORE_OK);
    struct mo_store *second = NULL;
    CHECK(mo_store_open(path, &second) != MO_STORE_OK && second == NULL);
    struct observations o = {0};
    struct mo_service_loop *s = open_service(image, store, &o);
    add(s, 11);
    mo_service_free(s);
    mo_store_close(store);
    sqlite3 *db;
    MO_ASSERT(sqlite3_open(path, &db) == SQLITE_OK);
    MO_ASSERT(sqlite3_exec(db,
                           "CREATE TRIGGER discard_log AFTER INSERT ON log BEGIN "
                           "DELETE FROM log WHERE seq=NEW.seq; END",
                           NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    MO_ASSERT(mo_store_open(path, &store) == MO_STORE_OK);
    s = open_service(image, store, &o);
    add(s, 13);
    mo_service_free(s);
    mo_store_close(store);
    MO_ASSERT(mo_store_open(path, &store) == MO_STORE_OK);
    s = open_service(image, store, &o);
    CHECK(mo_cell_get_int(mo_service_state(s)) == 31);
    mo_service_free(s);
    mo_store_close(store);
    MO_ASSERT(sqlite3_open(path, &db) == SQLITE_OK);
    MO_ASSERT(sqlite3_exec(db,
                           "ALTER TABLE log RENAME TO other;"
                           "CREATE TABLE log(seq INTEGER, record BLOB)",
                           NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    store = NULL;
    CHECK(mo_store_open(path, &store) != MO_STORE_OK && store == NULL);
    CHECK(truncate(path, 19) == 0);
    CHECK(mo_store_open(path, &store) != MO_STORE_OK && store == NULL);
    CHECK(truncate(path, 0) == 0);
    CHECK(mo_store_open(path, &store) == MO_STORE_CORRUPT && store == NULL);
    CHECK(unlink(path) == 0);
    struct mo_store *all[MO_LIMIT_STORES];
    for (unsigned i = 0; i < MO_LIMIT_STORES; i++)
        CHECK(mo_store_open(NULL, &all[i]) == MO_STORE_OK);
    for (unsigned i = 0; i < MO_LIMIT_STORES; i++)
        mo_store_close(all[i]);
}

struct saved_record {
    enum mo_store_record_kind kind;
    struct mo_buffer bytes;
};
static bool copy_record(const struct mo_store_record *record, void *context) {
    struct saved_record *saved = context;
    return record->kind != saved->kind ||
           mo_buffer_append(&saved->bytes, record->bytes, record->length);
}

static void invalid_payloads(struct mo_image *image) {
    for (unsigned kind = 1; kind <= 2; kind++) {
        struct saved_record saved = {.kind = (enum mo_store_record_kind)kind};
        mo_buffer_init(&saved.bytes, MO_LIMIT_SAVED_BYTES);
        struct mo_store *store;
        MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
        struct observations o = {0};
        struct mo_service_loop *s = open_service(image, store, &o);
        add(s, 11);
        MO_ASSERT(mo_store_call(store, MO_STORE_LOAD, NULL, copy_record, &saved) == MO_STORE_OK);
        mo_service_free(s);
        mo_store_close(store);
        for (unsigned fault = 0; fault < 9; fault++) {
            MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
            s = open_service(image, store, &o);
            mo_service_free(s);
            struct mo_buffer bytes;
            mo_buffer_init(&bytes, MO_LIMIT_SAVED_BYTES);
            MO_ASSERT(mo_buffer_append(&bytes, saved.bytes.bytes, saved.bytes.length));
            if (fault == 1)
                bytes.length--;
            else if (fault == 2)
                MO_ASSERT(mo_buffer_append_byte(&bytes, 0));
            else if (fault != 0) {
                size_t offset = kind == MO_STORE_STATE ? 9
                                : fault == 3           ? 0
                                : fault == 4           ? 1
                                : fault == 5           ? 12
                                : fault == 6           ? 13
                                : fault == 7           ? 38
                                                       : 17;
                bytes.bytes[offset] = 255;
            }
            struct mo_store_record record = {
                (enum mo_store_record_kind)kind, kind == MO_STORE_STATE ? 0 : 1,
                mo_image_bytecode(image)->header.checksum, bytes.bytes, bytes.length};
            MO_ASSERT(mo_store_call(store,
                                    kind == MO_STORE_STATE ? MO_STORE_SNAPSHOT : MO_STORE_APPEND,
                                    &record, NULL, NULL) == MO_STORE_OK);
            struct mo_service_hooks hooks = {clock_now, reply, event, &o};
            unsigned replies = o.replies;
            s = NULL;
            enum mo_service_status status = mo_service_open(image, 0, &hooks, store, &s);
            CHECK(status == (fault == 0 ? MO_SERVICE_OK : MO_SERVICE_SAVED_INVALID));
            CHECK(o.replies == replies);
            if (fault == 0) {
                MO_ASSERT(s != NULL);
                CHECK(mo_cell_get_int(mo_service_state(s)) == (kind == MO_STORE_STATE ? 7 : 18));
            } else {
                CHECK(s == NULL);
                CHECK(strcmp(mo_service_status_text(status),
                             "saved service data is damaged or was written by a different program; "
                             "recovery refused") == 0);
            }
            mo_service_free(s);
            mo_store_close(store);
            mo_buffer_free(&bytes);
        }
        mo_buffer_free(&saved.bytes);
    }
}

static void replay_limit(void) {
    struct mo_image *image = compile(
        "module Main\nservice\nstate Int = 0\ntools spin, grow\ntoken from env \"TOKEN\"\nend\n"
        "fn spin(state: Int, _now: Time, n: Int) : (Int, Int)\n"
        "var total = state\nfor _ in 0..n\ntotal = total + 1\nend\n(total, 0)\nend\n"
        "fn grow(state: Int, _now: Time, n: Int) : (Int, Int)\n"
        "(state + \"a\".repeat(n).bytes.size, 0)\nend\n");
    const enum mo_service_status expected[] = {MO_SERVICE_WORK, MO_SERVICE_MEMORY};
    const uint64_t sizes[] = {UINT64_C(1) << 40, UINT64_C(1) << 27};
    for (uint32_t tool = 0; tool < 2; tool++) {
        struct saved_record saved = {.kind = MO_STORE_LOG};
        mo_buffer_init(&saved.bytes, MO_LIMIT_SAVED_BYTES);
        struct mo_store *store;
        MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
        struct observations o = {0};
        struct mo_service_loop *s = open_service(image, store, &o);
        struct mo_cell n = mo_cell_int(3);
        CHECK(mo_service_send(s, tool, &n, 1, 0) == MO_SERVICE_OK);
        CHECK(mo_service_step(s));
        CHECK(mo_cell_get_int(mo_service_state(s)) == 3);
        MO_ASSERT(mo_store_call(store, MO_STORE_LOAD, NULL, copy_record, &saved) == MO_STORE_OK);
        mo_service_free(s);
        mo_store_close(store);
        /* The argument is the payload's last field; replace its eight value bytes. */
        for (unsigned i = 0; i < 8; i++)
            saved.bytes.bytes[saved.bytes.length - 8 + i] = (uint8_t)(sizes[tool] >> (8 * i));
        MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
        s = open_service(image, store, &o);
        mo_service_free(s);
        struct mo_store_record record = {MO_STORE_LOG, 1, mo_image_bytecode(image)->header.checksum,
                                         saved.bytes.bytes, saved.bytes.length};
        MO_ASSERT(mo_store_call(store, MO_STORE_APPEND, &record, NULL, NULL) == MO_STORE_OK);
        struct mo_service_hooks hooks = {clock_now, reply, event, &o};
        s = NULL;
        CHECK(mo_service_open(image, 0, &hooks, store, &s) == expected[tool] && s == NULL);
        mo_store_close(store);
        mo_buffer_free(&saved.bytes);
    }
    mo_image_free(image);
}

/* A tail of valid messages longer than the snapshot interval cannot come from
 * the runtime, so it is refused even though each message would replay. */
static void long_tail(struct mo_image *image) {
    struct saved_record saved = {.kind = MO_STORE_LOG};
    mo_buffer_init(&saved.bytes, MO_LIMIT_SAVED_BYTES);
    struct mo_store *store;
    MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
    struct observations o = {0};
    struct mo_service_loop *s = open_service(image, store, &o);
    add(s, 0);
    MO_ASSERT(mo_store_call(store, MO_STORE_LOAD, NULL, copy_record, &saved) == MO_STORE_OK);
    mo_service_free(s);
    mo_store_close(store);
    for (unsigned extra = 0; extra < 2; extra++) {
        MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
        s = open_service(image, store, &o);
        mo_service_free(s);
        for (uint64_t seq = 1; seq <= MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES + extra; seq++) {
            struct mo_store_record record = {MO_STORE_LOG, seq,
                                             mo_image_bytecode(image)->header.checksum,
                                             saved.bytes.bytes, saved.bytes.length};
            MO_ASSERT(mo_store_call(store, MO_STORE_APPEND, &record, NULL, NULL) == MO_STORE_OK);
        }
        struct mo_service_hooks hooks = {clock_now, reply, event, &o};
        s = NULL;
        CHECK(mo_service_open(image, 0, &hooks, store, &s) ==
              (extra ? MO_SERVICE_SAVED_INVALID : MO_SERVICE_OK));
        mo_service_free(s);
        mo_store_close(store);
    }
    mo_buffer_free(&saved.bytes);
}

static void composite_recovery(void) {
    struct mo_image *image = compile(
        "module Main\nstruct State\nvalues: List(Text)\nseen: Time\nend\n"
        "service\nstate State = initial()\ntools add\n"
        "every 1.second tick\ntoken from env \"TOKEN\"\nend\n"
        "fn initial() : State\ncase Time.parse(\"1970-01-01T00:00:00Z\")\n"
        "Some(t): State(values: [], seen: t)\nNone: stop \"invalid time\"\nend\nend\n"
        "fn add(state: State, now: Time, text: Text) : (State, Int)\n"
        "(State(values: state.values.append(text), seen: now), 1)\nend\n"
        "fn tick(state: State, now: Time) : State\nState(values: state.values, seen: now)\nend\n");
    struct mo_store *store;
    MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
    struct observations o = {.time = {123, 0}};
    struct mo_service_loop *s = open_service(image, store, &o);
    CHECK(mo_service_step(s));
    struct mo_cell text;
    MO_ASSERT(mo_value_text(mo_service_heap(s), "alpha", 5, &text));
    CHECK(mo_service_send(s, 0, &text, 1, 0) == MO_SERVICE_OK);
    o.time.now = 999;
    CHECK(mo_service_step(s));
    CHECK(mo_cell_get_time(mo_value_field(mo_cell_get_payload(mo_service_state(s)), 1)) == 123);
    mo_service_free(s);
    s = open_service(image, store, &o);
    struct mo_cell state = mo_cell_get_payload(mo_service_state(s));
    CHECK(mo_cell_get_time(mo_value_field(state, 1)) == 123);
    struct mo_cell list = mo_value_field(state, 0);
    CHECK(mo_list_size(list) == 1);
    struct mo_cell element;
    MO_ASSERT(mo_list_get(list, 0, &element));
    size_t length;
    const uint8_t *value = mo_value_data(element, &length);
    CHECK(length == 5 && memcmp(value, "alpha", 5) == 0);
    CHECK(mo_service_step(s));
    CHECK(mo_cell_get_time(mo_value_field(mo_cell_get_payload(mo_service_state(s)), 1)) == 999);
    mo_service_free(s);
    mo_store_close(store);
    mo_image_free(image);
}

static void put(struct mo_service_loop *s, struct mo_image *image, const char *key, int64_t n) {
    struct mo_cell arguments[2];
    arguments[1] = mo_cell_int(n);
    MO_ASSERT(mo_value_text(mo_service_heap(s), key, strlen(key), &arguments[0]));
    CHECK(mo_service_send(s, 0, arguments, 2, 0) == MO_SERVICE_OK);
    mo_value_drop(mo_service_heap(s),
                  mo_image_bytecode(image)->services[0].tools[0].argument_types[0], arguments[0]);
    CHECK(mo_service_step(s));
}

static void table_recovery(void) {
    const char *source = "module Main\nstruct State\ncounts: Map(Text, Int)\nseen: Set(Int)\nend\n"
                         "service\nstate State = State(counts: {}, seen: Set.empty)\ntools put\n"
                         "token from env \"TOKEN\"\nend\n"
                         "fn put(state: State, _now: Time, key: Text, n: Int) : (State, Int)\n"
                         "(State(counts: state.counts.set(key, n), seen: state.seen.add(n)), "
                         "state.counts.size)\nend\n";
    struct mo_image *first = compile(source);
    struct mo_store *store;
    MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
    struct observations o = {0};
    struct mo_service_loop *s = open_service(first, store, &o);
    put(s, first, "a", 1);
    put(s, first, "b", 2);
    put(s, first, "a", 3);
    mo_service_free(s);
    mo_image_free(first);
    struct mo_image *second = compile(source);
    s = open_service(second, store, &o);
    put(s, second, "b", 4);
    struct mo_cell state = mo_cell_get_payload(mo_service_state(s));
    struct mo_cell counts = mo_value_field(state, 0), seen = mo_value_field(state, 1);
    CHECK(mo_table_size(counts) == 2 && mo_table_size(seen) == 4);
    uint32_t position = 0;
    struct mo_cell key, value;
    int64_t sum = 0;
    while (mo_table_next(counts, &position, &key, &value)) {
        size_t length;
        const uint8_t *bytes = mo_value_data(key, &length);
        CHECK(length == 1);
        sum += (bytes[0] == 'a' ? 10 : 100) * mo_cell_get_int(value);
    }
    CHECK(sum == 3 * 10 + 4 * 100);
    mo_service_free(s);
    mo_store_close(store);
    mo_image_free(second);
}

static void allocator_bound(void) {
    const int half = MO_LIMIT_STORE_MEMORY / 2;
    void *a = mo_store_malloc(half + (1 << 20));
    MO_ASSERT(a != NULL);
    memset(a, 0x73, 16);
    CHECK(mo_store_malloc(half) == NULL);
    CHECK(mo_store_realloc(a, half + (1 << 20)) == NULL);
    CHECK(((unsigned char *)a)[0] == 0x73 && mo_store_size(a) == half + (1 << 20));
    mo_store_free(a);
    a = mo_store_malloc(half + (1 << 20));
    CHECK(a != NULL);
    mo_store_free(a);
}

static void shared_recovery_bound(void) {
    struct mo_image *image = compile(
        "module Main\nservice\nstate List(Text) = []\ntools replace\n"
        "token from env \"TOKEN\"\nend\n"
        "fn replace(_state: List(Text), _now: Time, values: List(Text)) : (List(Text), Int)\n"
        "(values, 1)\nend\n");
    for (unsigned boundary = 0; boundary < 2; boundary++) {
        struct mo_store *store;
        MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
        struct observations o = {0};
        struct mo_service_loop *s = open_service(image, store, &o);
        uint32_t type = mo_image_bytecode(image)->services[0].state_type;
        struct mo_heap *heap = mo_service_heap(s);
        struct mo_cell empty = mo_service_state(s);
        for (unsigned i = 0; i < (boundary ? MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES - 1 : 0); i++) {
            CHECK(mo_service_send(s, 0, &empty, 1, i) == MO_SERVICE_OK);
            CHECK(mo_service_step(s));
            empty = mo_service_state(s);
        }
        struct mo_cell text, list;
        MO_ASSERT(mo_value_text(heap, "", 0, &text));
        struct mo_arena arena;
        mo_arena_init(&arena, 32 << 20);
        const uint32_t count = 900000;
        struct mo_cell *items =
            mo_arena_allocate_array(&arena, count, sizeof *items, _Alignof(struct mo_cell));
        MO_ASSERT(items != NULL);
        for (uint32_t i = 0; i < count; i++)
            items[i] = text;
        MO_ASSERT(mo_value_list(heap, type, items, count, &list));
        mo_arena_free(&arena);
        enum mo_service_status sent = mo_service_send(s, 0, &list, 1, 777);
        CHECK(sent == MO_SERVICE_MEMORY);
        if (sent == MO_SERVICE_OK)
            CHECK(mo_service_step(s));
        mo_service_free(s);
        struct mo_service_hooks hooks = {clock_now, reply, event, &o};
        s = NULL;
        enum mo_service_status reopened = mo_service_open(image, 0, &hooks, store, &s);
        CHECK(reopened == MO_SERVICE_OK);
        if (s != NULL)
            CHECK(mo_list_size(mo_service_state(s)) == 0);
        mo_service_free(s);
        mo_store_close(store);
    }
    mo_image_free(image);
}

static void shared_result_bound(void) {
    struct mo_image *image =
        compile("module Main\nservice\nstate List(List(Text)) = []\ntools spread\n"
                "token from env \"TOKEN\"\nend\n"
                "fn spread(_state: List(List(Text)), _now: Time, values: List(Text)) : "
                "(List(List(Text)), Int)\n([values, values, values, values], 1)\nend\n");
    struct mo_store *store;
    MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
    struct observations o = {0};
    struct mo_service_loop *s = open_service(image, store, &o);
    uint32_t type = mo_image_bytecode(image)->services[0].tools[0].argument_types[0];
    struct mo_heap *heap = mo_service_heap(s);
    struct mo_cell text, list;
    MO_ASSERT(mo_value_text(heap, "", 0, &text));
    struct mo_arena arena;
    mo_arena_init(&arena, 8 << 20);
    const uint32_t count = 200000;
    struct mo_cell *items =
        mo_arena_allocate_array(&arena, count, sizeof *items, _Alignof(struct mo_cell));
    MO_ASSERT(items != NULL);
    for (uint32_t i = 0; i < count; i++)
        items[i] = text;
    MO_ASSERT(mo_value_list(heap, type, items, count, &list));
    mo_arena_free(&arena);
    CHECK(mo_service_send(s, 0, &list, 1, 9) == MO_SERVICE_OK);
    CHECK(mo_service_step(s));
    CHECK(o.status == MO_SERVICE_MEMORY && o.sequence == 0);
    CHECK(mo_list_size(mo_service_state(s)) == 0);
    mo_service_free(s);
    struct mo_service_hooks hooks = {clock_now, reply, event, &o};
    s = NULL;
    CHECK(mo_service_open(image, 0, &hooks, store, &s) == MO_SERVICE_OK);
    if (s != NULL)
        CHECK(mo_list_size(mo_service_state(s)) == 0);
    mo_service_free(s);
    mo_store_close(store);
    mo_image_free(image);
}

static void run(void *unused) {
    (void)unused;
    allocator_bound();
    struct mo_image *image = compile(
        "module Main\nservice\nstate Int = 7\ntools add, read, reject, broken\n"
        "token from env \"TOKEN\"\nend\n"
        "fn add(state: Int, _now: Time, n: Int) : (Int, Int)\n(state + n, state + n)\nend\n"
        "fn read(state: Int, _now: Time) : List(Int)\n[state]\nend\n"
        "fn reject(_state: Int, _now: Time) : Result((Int, Int), Text)\nError(\"no\")\nend\n"
        "fn broken(_state: Int, _now: Time) : (Int, Int)\nstop \"broken\"\nend\n");
    memory_store(image);
    char dir[] = "/tmp/mo-store-XXXXXX", path[256];
    MO_ASSERT(mkdtemp(dir) != NULL);
    MO_ASSERT(snprintf(path, sizeof path, "%s/state.db", dir) > 0);
    crashes(image, path);
    corruption(image, path);
    uncertain_commit(image);
    read_failures(image);
    long_tail(image);
    failed_snapshot(image);
    payload_limit(path);
    message_limit();
    state_limit();
    replay_limit();
    foreign_image(image, path);
    hostile_database(image, path);
    invalid_payloads(image);
    composite_recovery();
    shared_recovery_bound();
    shared_result_bound();
    table_recovery();
    CHECK(rmdir(dir) == 0);
    mo_image_free(image);
}

int main(void) {
    CHECK(mo_with_stack(MO_SOURCE_STACK_SIZE, run, NULL) == 0);
    return mo_test_report("service store");
}
