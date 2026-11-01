#define _POSIX_C_SOURCE 200809L
#include "base/stack.h"
#include "base/store-memory.h"
#include "bytecode/io.h"
#include "service_compile.h"
#include "test.h"
#include "vm/vm.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned crash_point, log_steps, simulation_appends;
static bool check_recovery_heap, refuse_store_allocation;
static unsigned recovery_heap_checks;
static struct {
    bool enabled, forward, refuse, refuse_case;
    struct mo_service_hooks hooks;
    struct mo_service_loop *loop;
    struct mo_store *store;
    struct mo_service_time initial_time, final_time;
    unsigned sends, replies, failures, restarts, memory, opens, fail_open, open_fault;
} observed;
MO_MUST_USE enum mo_store_status
mo_store_fault_call(struct mo_store *store, enum mo_store_operation operation,
                    const struct mo_store_record *record,
                    bool (*consume)(const struct mo_store_record *, void *), void *context);
int mo_store_fault_step(sqlite3_stmt *stmt);
void mo_store_fault_process_free(struct mo_process *process);
MO_MUST_USE void *mo_store_fault_allocate(struct mo_arena *arena, size_t size, size_t alignment);
MO_MUST_USE enum mo_service_status mo_simulation_observe_open(const struct mo_image *image,
                                                              uint32_t index,
                                                              const struct mo_service_hooks *hooks,
                                                              struct mo_store *store,
                                                              struct mo_service_loop **out);
MO_MUST_USE enum mo_service_status mo_simulation_observe_send(struct mo_service_loop *loop,
                                                              uint32_t tool,
                                                              const struct mo_cell *arguments,
                                                              uint32_t count, uint64_t id);
MO_MUST_USE bool mo_simulation_fault_case(struct mo_heap *heap, uint32_t type, uint32_t tag,
                                          const struct mo_cell *fields, uint32_t count,
                                          struct mo_cell *out);

void *mo_store_fault_allocate(struct mo_arena *arena, size_t size, size_t alignment) {
    return refuse_store_allocation ? NULL : mo_arena_allocate(arena, size, alignment);
}

bool mo_simulation_fault_case(struct mo_heap *heap, uint32_t type, uint32_t tag,
                              const struct mo_cell *fields, uint32_t count, struct mo_cell *out) {
    return !(observed.enabled && observed.refuse_case) &&
           mo_value_case(heap, type, tag, fields, count, out);
}

static struct mo_service_time observed_clock(void *context) {
    (void)context;
    return observed.hooks.clock(observed.hooks.context);
}

static void observed_reply(const struct mo_service_response *response, void *context) {
    (void)context;
    observed.replies++;
    observed.memory += response->status == MO_SERVICE_MEMORY ? 1u : 0u;
    if (observed.forward)
        observed.hooks.reply(response, observed.hooks.context);
}

static void observed_event(const struct mo_service_event *e, void *context) {
    (void)context;
    observed.failures += e->kind == MO_SERVICE_FAILURE ? 1u : 0u;
    observed.restarts += e->kind == MO_SERVICE_RESTART ? 1u : 0u;
    if (observed.forward)
        observed.hooks.event(e, observed.hooks.context);
}

enum mo_service_status mo_simulation_observe_open(const struct mo_image *image, uint32_t index,
                                                  const struct mo_service_hooks *hooks,
                                                  struct mo_store *store,
                                                  struct mo_service_loop **out) {
    if (!observed.enabled)
        return mo_service_open(image, index, hooks, store, out);
    observed.hooks = *hooks;
    observed.store = store;
    observed.initial_time = hooks->clock(hooks->context);
    if (++observed.opens == observed.fail_open)
        crash_point = observed.open_fault;
    struct mo_service_hooks recording = {observed_clock, observed_reply, observed_event, NULL};
    enum mo_service_status status = mo_service_open(image, index, &recording, store, out);
    if (status == MO_SERVICE_OK)
        observed.loop = *out;
    return status;
}

enum mo_service_status mo_simulation_observe_send(struct mo_service_loop *loop, uint32_t tool,
                                                  const struct mo_cell *arguments, uint32_t count,
                                                  uint64_t id) {
    if (observed.enabled) {
        observed.sends++;
        if (observed.refuse)
            return MO_SERVICE_MEMORY;
    }
    return mo_service_send(loop, tool, arguments, count, id);
}

void mo_store_fault_process_free(struct mo_process *process) {
    if (check_recovery_heap && process != NULL) {
        CHECK(mo_heap_objects(mo_process_heap(process)) == 0);
        recovery_heap_checks++;
    }
    mo_process_free(process);
}

enum mo_store_status mo_store_fault_call(struct mo_store *store, enum mo_store_operation operation,
                                         const struct mo_store_record *record,
                                         bool (*consume)(const struct mo_store_record *, void *),
                                         void *context) {
    if (operation == MO_STORE_APPEND)
        simulation_appends++;
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
    struct mo_bytecode_string handler;
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
    o->handler = e->handler;
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
    CHECK(mo_store_open(NULL, &extra) == MO_STORE_HANDLES && extra == NULL);
    char message[128];
    MO_ASSERT(snprintf(message, sizeof message,
                       "too many open service stores; close one of the %d open stores",
                       MO_LIMIT_STORES) > 0);
    CHECK(strcmp(mo_store_status_text(MO_STORE_HANDLES), message) == 0);
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
    CHECK(mo_store_open("", &store) == MO_STORE_EMPTY_PATH && store == NULL);
    CHECK(strcmp(mo_store_status_text(MO_STORE_EMPTY_PATH),
                 "service store path is empty; use a file path") == 0);
    CHECK(mo_store_open(":memory:", &store) == MO_STORE_MEMORY_PATH && store == NULL);
    CHECK(strcmp(mo_store_status_text(MO_STORE_MEMORY_PATH),
                 ":memory: is not a service store path; use a file path") == 0);
    CHECK(mo_store_status_text(MO_STORE_OK) == NULL);
    CHECK(strcmp(mo_store_status_text(MO_STORE_IO),
                 "service store failed; reopen it to see whether the last change was saved") == 0);
    CHECK(strcmp(mo_store_status_text(MO_STORE_CORRUPT),
                 "saved service data is damaged or was written by a different program; recovery "
                 "refused") == 0);
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
    refuse_store_allocation = true;
    for (unsigned i = 0; i <= MO_LIMIT_STORES; i++)
        CHECK(mo_store_open(NULL, &store) == MO_STORE_MEMORY && store == NULL);
    refuse_store_allocation = false;
    CHECK(strcmp(mo_store_status_text(MO_STORE_MEMORY), "service store memory allocation failed") ==
          0);
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

static void wrong_state_checksum(void) {
    struct mo_image *image =
        compile("module Main\nservice\nstate List(Int) = [3, 8]\ntools add\n"
                "token from env \"TOKEN\"\nend\n"
                "fn add(state: List(Int), _now: Time, n: Int) : (List(Int), Int)\n"
                "(state.append(n), n)\nend\n");
    struct saved_record saved = {.kind = MO_STORE_LOG};
    mo_buffer_init(&saved.bytes, MO_LIMIT_SAVED_BYTES);
    struct mo_store *store;
    MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
    struct observations o = {0};
    struct mo_service_loop *s = open_service(image, store, &o);
    add(s, 11);
    MO_ASSERT(mo_store_call(store, MO_STORE_LOAD, NULL, copy_record, &saved) == MO_STORE_OK);
    mo_service_free(s);
    s = open_service(image, store, &o);
    struct mo_cell element;
    CHECK(mo_list_size(mo_service_state(s)) == 3);
    MO_ASSERT(mo_list_get(mo_service_state(s), 2, &element));
    CHECK(mo_cell_get_int(element) == 11);
    mo_service_free(s);
    mo_store_close(store);
    MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
    s = open_service(image, store, &o);
    mo_service_free(s);
    MO_ASSERT(saved.bytes.length > 17);
    saved.bytes.bytes[17] ^= 1;
    struct mo_store_record record = {MO_STORE_LOG, 1, mo_image_bytecode(image)->header.checksum,
                                     saved.bytes.bytes, saved.bytes.length};
    MO_ASSERT(mo_store_call(store, MO_STORE_APPEND, &record, NULL, NULL) == MO_STORE_OK);
    struct mo_service_hooks hooks = {clock_now, reply, event, &o};
    unsigned replies = o.replies, failures = o.failures;
    s = NULL;
    check_recovery_heap = true;
    CHECK(mo_service_open(image, 0, &hooks, store, &s) == MO_SERVICE_SAVED_INVALID);
    check_recovery_heap = false;
    CHECK(s == NULL && o.replies == replies && o.failures == failures + 1);
    CHECK(recovery_heap_checks == 1);
    mo_service_free(s);
    mo_store_close(store);
    mo_buffer_free(&saved.bytes);
    mo_image_free(image);
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
        CHECK(named(o.handler, tool == 0 ? "spin" : "grow"));
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
    CHECK(named(o.handler, "spread"));
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

static uint32_t simulation_test(const struct mo_bytecode *file) {
    for (uint32_t i = 0; i < file->function_count; i++)
        if (file->functions[i].kind == MO_FUNCTION_TEST)
            return i;
    MO_UNREACHABLE();
}

static void simulation_run(struct mo_image *image, const char *reason, size_t heap) {
    struct mo_limits limits = mo_limits_default();
    if (heap != 0)
        limits.heap_size = heap;
    struct mo_process *p = mo_process_new(image, &limits);
    MO_ASSERT(p != NULL);
    struct mo_cell result;
    struct mo_defect defect;
    enum mo_outcome outcome =
        mo_call(p, simulation_test(mo_image_bytecode(image)), NULL, 0, &result, &defect);
    CHECK(outcome == (reason == NULL ? MO_RETURNED : MO_STOPPED));
    if (reason != NULL && outcome == MO_STOPPED) {
        CHECK(defect.reason != NULL && strstr(defect.reason, reason) != NULL);
        if (defect.reason == NULL || strstr(defect.reason, reason) == NULL)
            (void)fprintf(stderr, "simulation wanted %s, got %s\n", reason, defect.reason);
    }
    if (observed.enabled)
        observed.final_time = observed.hooks.clock(observed.hooks.context);
    mo_process_free(p);
}

static bool simulation_start_snapshot(const struct mo_store_record *record, void *context) {
    uint32_t type = *(uint32_t *)context;
    struct mo_heap *heap = mo_service_heap(observed.loop);
    struct mo_cell value;
    CHECK(record->kind == MO_STORE_STATE && record->sequence == 0);
    bool decoded = mo_value_decode(heap, type, record->bytes, record->length, &value);
    CHECK(decoded);
    if (decoded) {
        CHECK(mo_cell_get_int(value) == 7);
        mo_value_drop(heap, type, value);
    }
    return true;
}

static void simulation_admission(void) {
    const char *bodies[] = {
        "a = \"x\".repeat(8388608)\n_result = Simulation.send(sim, Main.accept, a, a)\n",
        "a = \"x\".repeat(8388608)\nb = \"y\".repeat(8388523)\n"
        "assert Simulation.send(sim, Main.accept, a, b) == Ok((8, 7))\n",
        "a = \"x\".repeat(8388608)\n"
        "assert Simulation.send(sim, Main.read, a, a) == Ok([7])\n",
        "_result = Simulation.send(sim, Main.accept, \"a\", \"b\")\n",
        "for _i in 0..5\nassert Simulation.send(sim, Main.memory) == Error(ToolFailed)\nend\n"
        "assert Simulation.send(sim, Main.snapshot) == Ok([7])\n",
        "b = \"x\".repeat(16777216)\n_result = Simulation.send(sim, Main.accept, \"first\", b)\n"};
    for (unsigned scenario = 0; scenario < 6; scenario++) {
        char source[4096];
        int n = snprintf(
            source, sizeof source,
            "module Main\nservice\nstate Int = 7\ntools accept, read, broken, memory, snapshot\n"
            "token from env \"TOKEN\"\nend\n"
            "fn accept(state: Int, _now: Time, _a: Text, _b: Text) : (Int, Int)\n"
            "(state + 1, state)\nend\n"
            "fn read(state: Int, _now: Time, _a: Text, _b: Text) : List(Int)\n[state]\nend\n"
            "fn broken(_state: Int, _now: Time) : (Int, Int)\nstop \"broken\"\nend\n"
            "fn memory(state: Int, _now: Time) : (Int, Text)\n"
            "(state + 1, \"x\".repeat(67108864))\nend\n"
            "fn snapshot(state: Int, _now: Time) : List(Int)\n[state]\nend\n"
            "test \"admission and execution differ\"\n"
            "sim = Simulation.start(Main, Time.parse(\"2026-01-01T00:00:00Z\") or stop \"time\")\n"
            "%send\n",
            bodies[scenario]);
        MO_ASSERT(n > 0 && (size_t)n < sizeof source);
        struct mo_image *image = compile(source);
        memset(&observed, 0, sizeof observed);
        observed.enabled = true;
        observed.forward = true;
        observed.refuse = scenario == 3;
        simulation_appends = 0;
        struct mo_limits limits = mo_limits_default();
        struct mo_process *p = mo_process_new(image, &limits);
        MO_ASSERT(p != NULL);
        struct mo_cell result;
        struct mo_defect defect;
        enum mo_outcome outcome =
            mo_call(p, simulation_test(mo_image_bytecode(image)), NULL, 0, &result, &defect);
        bool refused = scenario == 0 || scenario == 3 || scenario == 5;
        CHECK(outcome == (refused ? MO_STOPPED : MO_RETURNED));
        if (refused && outcome == MO_STOPPED) {
            const char *reason =
                scenario == 0   ? "aggregate saved message is 16777301 bytes, exceeding "
                                  "MO_LIMIT_SAVED_BYTES (16777216)"
                : scenario == 5 ? "Simulation argument _b copy-in: MO_LIMIT_SAVED_BYTES (16777216)"
                                : "Simulation admission refused";
            CHECK(strstr(defect.reason, reason) != NULL);
            CHECK(strstr(defect.reason, "nothing was queued or committed") != NULL);
            if (scenario != 5)
                CHECK(strstr(defect.reason, "; no handler ran") != NULL);
            CHECK(observed.sends == (scenario == 3 ? 1u : 0u));
            CHECK(observed.replies == 0 && observed.failures == 0 && observed.restarts == 0);
            CHECK(mo_service_pending(observed.loop) == 0);
            CHECK(mo_cell_get_int(mo_service_state(observed.loop)) == 7);
            check_inventory(observed.store, 0, 0);
            uint32_t type = mo_image_bytecode(image)->services[0].state_type;
            CHECK(mo_store_call(observed.store, MO_STORE_LOAD, NULL, simulation_start_snapshot,
                                &type) == MO_STORE_OK);
            observed.forward = false;
            for (unsigned i = 0; i < 5; i++) {
                CHECK(mo_service_send(observed.loop, 2, NULL, 0, i) == MO_SERVICE_OK);
                CHECK(mo_service_step(observed.loop));
                CHECK(observed.restarts == (i == 4 ? 1u : 0u));
            }
            CHECK(observed.failures == 5 && observed.replies == 5);
            CHECK(mo_cell_get_int(mo_service_state(observed.loop)) == 7);
            check_inventory(observed.store, 0, 0);
        } else if (!refused) {
            CHECK(observed.sends == (scenario == 4 ? 6u : 1u));
            CHECK(observed.replies == observed.sends);
            CHECK(observed.memory == (scenario == 4 ? 5u : 0u));
            CHECK(observed.failures == (scenario == 4 ? 5u : 0u));
            CHECK(observed.restarts == (scenario == 4 ? 1u : 0u));
        }
        CHECK(simulation_appends == (scenario == 1 ? 1u : 0u));
        mo_process_free(p);
        observed.enabled = false;
        mo_image_free(image);
    }
}

static void simulation_persistence(void) {
    const char *source =
        "module Main\nservice\nstate Int = 0\ntools read\nevery 1.minute tick\n"
        "token from env \"TOKEN\"\nend\n"
        "fn tick(state: Int, _now: Time) : Int\nstate + 1\nend\n"
        "fn read(state: Int, _now: Time) : List(Int)\n[state]\nend\n"
        "test \"precount refuses without executing\"\n"
        "sim = Simulation.start(Main, Time.parse(\"2026-01-01T00:00:00Z\") or stop \"time\")\n"
        "Simulation.advance(sim, 10001.minutes)\nend\n";
    struct mo_image *image = compile(source);
    simulation_appends = 0;
    memset(&observed, 0, sizeof observed);
    observed.enabled = true;
    observed.forward = true;
    simulation_run(image, "10001 firings exceed MO_LIMIT_SIMULATION_TICKS (10000)", 0);
    CHECK(simulation_appends == 1);
    CHECK(observed.initial_time.now == observed.final_time.now);
    CHECK(observed.initial_time.elapsed == observed.final_time.elapsed);
    CHECK(observed.replies == 1);
    observed.enabled = false;
    mo_image_free(image);

    image = compile(
        "module Main\nservice\nstate Int = 0\ntools read, reject\n"
        "token from env \"TOKEN\"\nend\n"
        "fn read(state: Int, _now: Time) : List(Int)\n[state]\nend\n"
        "fn reject(_state: Int, _now: Time) : Result((Int, Int), Text)\nError(\"no\")\nend\n"
        "test \"read and rejection never append\"\n"
        "sim = Simulation.start(Main, Time.parse(\"2026-01-01T00:00:00Z\") or stop \"time\")\n"
        "for _i in 0..20\nassert Simulation.send(sim, Main.read) == Ok([0])\n"
        "assert Simulation.send(sim, Main.reject) == Ok(Error(\"no\"))\nend\n"
        "Simulation.crash(sim)\nSimulation.restart(sim)\n"
        "assert Simulation.send(sim, Main.read) == Ok([0])\nend\n");
    simulation_appends = 0;
    const char *store_limit = "Simulation.start: MO_LIMIT_STORES (8) or store memory limit";
    refuse_store_allocation = true;
    simulation_run(image, store_limit, 0);
    refuse_store_allocation = false;
    CHECK(simulation_appends == 0);
    struct mo_store *all[MO_LIMIT_STORES];
    for (unsigned i = 0; i < MO_LIMIT_STORES; i++)
        MO_ASSERT(mo_store_open(NULL, &all[i]) == MO_STORE_OK);
    simulation_run(image, store_limit, 0);
    CHECK(simulation_appends == 0);
    for (unsigned i = 0; i < MO_LIMIT_STORES; i++)
        mo_store_close(all[i]);
    simulation_run(image, NULL, 0);
    CHECK(simulation_appends == 0);
    for (unsigned i = 0; i < 2; i++) {
        memset(&observed, 0, sizeof observed);
        observed.enabled = true;
        observed.forward = true;
        observed.fail_open = 2;
        observed.open_fault = i == 0 ? 7u : 11u;
        simulation_run(image,
                       i == 0 ? "Simulation.restart at 2026-01-01T00:00:00Z: service store failed"
                              : "Simulation.restart at 2026-01-01T00:00:00Z: saved service data",
                       0);
        CHECK(observed.opens == 2);
        observed.enabled = false;
        crash_point = 0;
    }
    crash_point = 6;
    simulation_run(image, "Simulation.start at 2026-01-01T00:00:00Z: service store failed", 0);
    crash_point = 0;
    mo_image_free(image);

    image = compile(
        "module Main\nservice\nstate Int = 0\ntools add\n"
        "token from env \"TOKEN\"\nend\n"
        "fn add(state: Int, _now: Time) : (Int, Text)\n(state + 1, \"x\".repeat(100000))\nend\n"
        "test \"reply allocation does not undo an update\"\n"
        "sim = Simulation.start(Main, Time.parse(\"2026-01-01T00:00:00Z\") or stop \"time\")\n"
        "_result = Simulation.send(sim, Main.add)\nend\n");
    simulation_appends = 0;
    simulation_run(image, "tool add executed; its update was committed; no rollback or retry",
                   65536);
    CHECK(simulation_appends == 1);
    crash_point = 4;
    simulation_run(image, "Simulation.send at 2026-01-01T00:00:00Z: service store failed", 0);
    crash_point = 0;
    mo_image_free(image);

    for (unsigned i = 0; i < 2; i++) {
        char source_text[1024];
        int written =
            snprintf(source_text, sizeof source_text,
                     "module Main\nservice\nstate Int = 0\ntools broken\n"
                     "token from env \"TOKEN\"\nend\n"
                     "fn broken(_state: Int, _now: Time) : List(Int)\nstop \"broken\"\nend\n"
                     "test \"error allocation\"\n"
                     "sim = Simulation.start(Main, Time.parse(\"2026-01-01T00:00:00Z\") "
                     "or stop \"time\")\n%s"
                     "_reply = Simulation.send(sim, Main.broken)\nend\n",
                     i == 0 ? "Simulation.crash(sim)\n" : "");
        MO_ASSERT(written > 0 && (size_t)written < sizeof source_text);
        image = compile(source_text);
        memset(&observed, 0, sizeof observed);
        observed.enabled = true;
        observed.forward = true;
        observed.refuse_case = true;
        simulation_run(image,
                       i == 0 ? "Simulation.send: allocation failed while returning ServiceDown; "
                                "nothing was queued or committed"
                              : "Simulation.send: allocation failed while returning ToolFailed; "
                                "tool broken failed and nothing was committed",
                       0);
        CHECK(observed.sends == i && observed.replies == i && observed.failures == i);
        observed.enabled = false;
        mo_image_free(image);
    }
}

static void simulation_load(const struct mo_bytecode *file, const char *refusal,
                            const char *runtime_reason) {
    struct mo_buffer wire;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    struct mo_write_error written;
    MO_ASSERT(mo_bytecode_encode(file, &wire, &written));
    struct mo_load_error error;
    struct mo_image *image = mo_load(wire.bytes, wire.length, &error);
    if (refusal != NULL) {
        CHECK(image == NULL && error.reason != NULL && strstr(error.reason, refusal) != NULL);
        if (image != NULL || error.reason == NULL || strstr(error.reason, refusal) == NULL)
            (void)fprintf(stderr, "simulation load wanted %s, got %s\n", refusal,
                          image == NULL ? error.reason : "accepted");
    } else {
        MO_ASSERT(image != NULL);
        struct mo_limits limits = mo_limits_default();
        struct mo_defect defect;
        MO_ASSERT(mo_image_init(image, &limits, &defect) == MO_RETURNED);
        simulation_run(image, runtime_reason, 0);
    }
    if (image != NULL)
        mo_image_free(image);
    mo_buffer_free(&wire);
}

static void simulation_verifier(void) {
    struct mo_image *image = compile(
        "module Main\nservice\nstate Int = 0\ntools add\nevery 1.minute tick\n"
        "token from env \"TOKEN\"\nend\n"
        "fn add(state: Int, _now: Time, n: Int) : (Int, Int)\n(state + n, n)\nend\n"
        "fn tick(state: Int, _now: Time) : Int\nstate + 1\nend\n"
        "test \"wire contract\"\n"
        "sim = Simulation.start(Main, Time.parse(\"2026-01-01T00:00:00Z\") or stop \"time\")\n"
        "_result = Simulation.send(sim, Main.add, 7)\nend\n");
    struct mo_bytecode file = *mo_image_bytecode(image);
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_BYTECODE_SIZE);
    struct mo_bytecode_function *functions = mo_arena_allocate_array(
        &arena, file.function_count, sizeof *functions, _Alignof(struct mo_bytecode_function));
    struct mo_bytecode_debug *debug = mo_arena_allocate_array(
        &arena, file.debug_count, sizeof *debug, _Alignof(struct mo_bytecode_debug));
    MO_ASSERT(functions != NULL && debug != NULL);
    memcpy(functions, file.functions, file.function_count * sizeof *functions);
    memcpy(debug, file.debug, file.debug_count * sizeof *debug);
    file.functions = functions;
    file.debug = debug;
    uint32_t test = simulation_test(&file);
    struct mo_bytecode_function *fn = &functions[test];
    uint32_t *words =
        mo_arena_allocate_array(&arena, fn->code_count, sizeof *words, _Alignof(uint32_t));
    MO_ASSERT(words != NULL);
    memcpy(words, fn->code, fn->code_count * sizeof *words);
    fn->code = words;
    simulation_load(&file, NULL, NULL);
    fn->kind = MO_FUNCTION_FUNCTION;
    fn->spec_code = false;
    uint32_t text = debug[test].text;
    debug[test].text = MO_NO_INDEX;
    simulation_load(&file, "simulation start in non-spec code", NULL);
    fn->kind = MO_FUNCTION_TEST;
    fn->spec_code = true;
    debug[test].text = text;
    for (uint32_t pc = 0; pc < fn->code_count;) {
        const struct mo_opcode_info *op = mo_opcode_find(words[pc] & MO_OPCODE_MASK);
        MO_ASSERT(op != NULL);
        uint32_t tail = words[pc] >> MO_OPCODE_VARIABLE_SHIFT;
        if (op->number == MO_OP_SIM) {
            uint32_t descriptor = words[pc + 3];
            words[pc + 3] = file.service_count;
            simulation_load(&file, "descriptor", NULL);
            words[pc + 3] = descriptor;
            if (tail == 2) {
                uint32_t tool = words[pc + 4];
                words[pc + 4] = file.services[0].tool_count;
                simulation_load(&file, "simulation send tool", NULL);
                words[pc + 4] = tool;
                uint32_t argument = words[pc + 6];
                words[pc + 6] = words[pc + 5];
                simulation_load(&file, "simulation send argument types or count", NULL);
                words[pc + 6] = argument;
                uint32_t destination = words[pc + 1];
                words[pc + 1] = argument;
                simulation_load(&file, "simulation send result", NULL);
                words[pc + 1] = destination;
            }
        }
        pc += 1u + op->fixed_count + tail;
    }
    struct mo_bytecode_constant *constants = mo_arena_allocate_array(
        &arena, file.constant_count, sizeof *constants, _Alignof(struct mo_bytecode_constant));
    MO_ASSERT(constants != NULL);
    memcpy(constants, file.constants, file.constant_count * sizeof *constants);
    file.constants = constants;
    uint32_t zero = MO_NO_INDEX;
    for (uint32_t i = 0; i < file.constant_count; i++) {
        if (file.types[constants[i].type].kind == MO_TYPE_DURATION) {
            memset(constants[i].value.immediate, 0, sizeof constants[i].value.immediate);
            zero = i;
            break;
        }
    }
    MO_ASSERT(zero != MO_NO_INDEX);
    uint32_t interval = file.services[0].timers[0].interval;
    uint32_t code[] = {MO_OP_CONST, 0, zero, MO_OP_RET, 0};
    functions[interval].register_count = 1;
    functions[interval].register_types = &functions[interval].result_type;
    functions[interval].code = code;
    functions[interval].code_count = sizeof code / sizeof code[0];
    debug[interval].span_count = 0;
    debug[interval].name_count = 0;
    simulation_load(&file, NULL,
                    "Simulation.start at 2026-01-01T00:00:00Z: interval of timer Main.tick:");
    mo_arena_free(&arena);
    mo_image_free(image);
}

static void run(void *unused) {
    (void)unused;
    simulation_admission();
    simulation_persistence();
    simulation_verifier();
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
    wrong_state_checksum();
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
