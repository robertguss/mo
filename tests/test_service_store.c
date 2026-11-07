#define _POSIX_C_SOURCE 200809L
#include "base/binary.h"
#include "base/export.h"
#include "base/failure.h"
#include "base/log-record.h"
#include "base/runtime-identity.h"
#include "base/stack.h"
#include "base/store-memory.h"
#include "base/verdict.h"
#include "bytecode/io.h"
#include "service_compile.h"
#include "test.h"
#include "vm/vm.h"

#include <errno.h>
#include <inttypes.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static unsigned crash_point, log_steps, simulation_appends;
static bool check_recovery_heap, refuse_store_allocation, refuse_cache_copy;
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
int mo_store_fault_exec(sqlite3 *db, const char *sql,
                        int (*callback)(void *, int, char **, char **), void *context,
                        char **error);
void mo_store_fault_process_free(struct mo_process *process);
MO_MUST_USE void *mo_store_fault_allocate(struct mo_arena *arena, size_t size, size_t alignment);
MO_MUST_USE bool mo_service_fault_buffer_append(struct mo_buffer *buffer, const void *bytes,
                                                size_t length);
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

bool mo_service_fault_buffer_append(struct mo_buffer *buffer, const void *bytes, size_t length) {
    return !refuse_cache_copy && mo_buffer_append(buffer, bytes, length);
}

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

int mo_store_fault_exec(sqlite3 *db, const char *sql,
                        int (*callback)(void *, int, char **, char **), void *context,
                        char **error) {
    if (crash_point == 15 && strcmp(sql, "ROLLBACK") == 0)
        return SQLITE_IOERR;
    int result = sqlite3_exec(db, sql, callback, context, error);
    if (strcmp(sql, "COMMIT") == 0 && result == SQLITE_OK) {
        if (crash_point == 14)
            return SQLITE_IOERR;
        if (crash_point == 19)
            _exit(79);
    }
    return result;
}

/* A reader's first read of the schema returns this code instead, and a reader whose log
 * walk starts exits as a killed process would. */
static int reader_code;
static bool reader_killed;

int mo_store_fault_step(sqlite3_stmt *stmt) {
    if (reader_code != 0 && strstr(sqlite3_sql(stmt), "FROM sqlite_schema") != NULL)
        return reader_code;
    if (reader_killed && strstr(sqlite3_sql(stmt), "length(record)") != NULL)
        _exit(80);
    if (crash_point == 30 && strstr(sqlite3_sql(stmt), "INSERT INTO log") != NULL &&
        sqlite3_step(stmt) == SQLITE_DONE)
        _exit(81);
    if (crash_point >= 21 && crash_point <= 27 &&
        strstr(sqlite3_sql(stmt), "INSERT INTO failure_state") != NULL) {
        static const int codes[] = {SQLITE_TOOBIG, SQLITE_NOMEM, SQLITE_CONSTRAINT, SQLITE_CORRUPT,
                                    SQLITE_IOERR,  SQLITE_BUSY,  SQLITE_FULL};
        return codes[crash_point - 21];
    }
    if (crash_point == 27 && strstr(sqlite3_sql(stmt), "SELECT seq FROM head") != NULL)
        return SQLITE_IOERR;
    if (crash_point == 20 && strstr(sqlite3_sql(stmt), "INSERT INTO failure_state") != NULL) {
        CHECK(sqlite3_exec(sqlite3_db_handle(stmt), "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
        return SQLITE_FULL;
    }
    if ((crash_point == 13 || crash_point == 15) &&
        strstr(sqlite3_sql(stmt), "INSERT INTO failure_state") != NULL)
        return SQLITE_FULL;
    int result = sqlite3_step(stmt);
    if (crash_point == 17 && result == SQLITE_DONE &&
        strstr(sqlite3_sql(stmt), "DELETE FROM failure WHERE") != NULL)
        _exit(77);
    if (crash_point == 18 && result == SQLITE_DONE &&
        strstr(sqlite3_sql(stmt), "INSERT INTO failure_state") != NULL)
        _exit(78);
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

static void check_add_log(struct mo_service_loop *service, uint64_t first, uint32_t count) {
    struct mo_service_message_summary entry;
    for (uint32_t i = 0; i < count; i++) {
        bool found = mo_service_log_entry(service, i, &entry);
        CHECK(found);
        if (found) {
            CHECK(entry.id == first + i && entry.handler == 0 && !entry.timer);
            CHECK(entry.arrival == 0 && entry.argument_bytes_available);
            /* MOSV header (9), Int schema (13), and signed payload (8). */
            CHECK(entry.argument_bytes == 30);
        }
    }
    CHECK(!mo_service_log_entry(service, count, &entry));
}

static void check_encoding(struct mo_service_loop *service, struct mo_image *image) {
    const uint8_t *bytes;
    size_t length;
    CHECK(mo_service_encoded_state(service, &bytes, &length));
    struct mo_buffer expected;
    mo_buffer_init(&expected, MO_LIMIT_SAVED_BYTES);
    CHECK(mo_value_encode(mo_service_heap(service),
                          mo_image_bytecode(image)->services[0].state_type,
                          mo_service_state(service), &expected));
    CHECK(length == expected.length);
    CHECK(length == expected.length && memcmp(bytes, expected.bytes, length) == 0);
    struct mo_service_observation view = mo_service_observe(service);
    CHECK(view.encoded_bytes == length && view.encoded_capacity >= length);
    CHECK(view.encoded_capacity <= MO_LIMIT_SAVED_BYTES);
    mo_buffer_free(&expected);
}

static void readonly_mailbox_size(void) {
    struct mo_image *image =
        compile("module Main\nservice\nstate Int = 7\ntools read\n"
                "token from env \"TOKEN\"\nend\n"
                "fn read(_state: Int, _now: Time, n: Int) : Bool\nn > 0\nend\n");
    struct mo_store *store = NULL;
    MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
    struct observations o = {0};
    struct mo_service_loop *s = open_service(image, store, &o);
    struct mo_cell argument = mo_cell_int(42);
    CHECK(mo_service_send(s, 0, &argument, 1, 1) == MO_SERVICE_OK);
    struct mo_service_message_summary message;
    CHECK(mo_service_mailbox_entry(s, 0, &message));
    CHECK(!message.argument_bytes_available);
    mo_service_free(s);
    mo_store_close(store);
    mo_image_free(image);
}

static void cache_copy_failure(void) {
    struct mo_image *image = compile("module Main\nservice\nstate List(Int) = [7]\ntools read\n"
                                     "token from env \"TOKEN\"\nend\n"
                                     "fn read(_state: List(Int), _now: Time) : Bool\ntrue\nend\n");
    struct mo_store *store = NULL;
    MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
    struct observations o = {0};
    struct mo_service_loop *s = open_service(image, store, &o);
    mo_service_free(s);
    struct mo_service_hooks hooks = {clock_now, reply, event, &o};
    s = NULL;
    unsigned checks = recovery_heap_checks;
    check_recovery_heap = true;
    refuse_cache_copy = true;
    CHECK(mo_service_open(image, 0, &hooks, store, &s) == MO_SERVICE_MEMORY);
    refuse_cache_copy = false;
    check_recovery_heap = false;
    CHECK(s == NULL && recovery_heap_checks == checks + 1);
    recovery_heap_checks = checks;
    s = open_service(image, store, &o);
    check_encoding(s, image);
    mo_service_free(s);
    mo_store_close(store);
    mo_image_free(image);
}

static void memory_store(struct mo_image *image) {
    struct mo_store *store = NULL;
    MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
    struct observations o = {0};
    struct mo_service_loop *s = open_service(image, store, &o);
    check_encoding(s, image);
    struct mo_cell argument = mo_cell_int(0);
    CHECK(mo_service_send(s, 0, &argument, 1, 2000) == MO_SERVICE_OK);
    struct mo_service_message_summary message;
    CHECK(mo_service_mailbox_entry(s, 0, &message));
    struct mo_buffer argument_encoding;
    mo_buffer_init(&argument_encoding, MO_LIMIT_SAVED_BYTES);
    CHECK(mo_value_encode(mo_service_heap(s),
                          mo_image_bytecode(image)->services[0].tools[0].argument_types[0],
                          argument, &argument_encoding));
    CHECK(message.argument_bytes_available && message.argument_bytes == argument_encoding.length &&
          message.argument_bytes != 0);
    mo_buffer_free(&argument_encoding);
    /* Discard the queued observation fixture without committing a message. */
    mo_service_free(s);
    s = open_service(image, store, &o);
    for (unsigned i = 0; i < MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES + 3; i++)
        add(s, 11);
    CHECK(o.replies == MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES + 3 && o.status == MO_SERVICE_OK);
    check_inventory(store, MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES, 3);
    struct mo_service_observation view = mo_service_observe(s);
    CHECK(view.sequence == 103 && view.snapshot_sequence == 100 && view.encoded_sequence == 103);
    struct mo_service_message_summary log_entry;
    CHECK(mo_service_log_entry(s, 0, &log_entry));
    CHECK(log_entry.id == 101 && log_entry.argument_bytes_available);
    check_add_log(s, 101, 3);
    CHECK(mo_service_log_entry(s, 2, &log_entry) && log_entry.id == 103);
    CHECK(!mo_service_log_entry(s, 3, &log_entry));

    for (unsigned i = 0; i < 20; i++) {
        CHECK(mo_service_send(s, 1 + i % 2, NULL, 0, i) == MO_SERVICE_OK);
        CHECK(mo_service_step(s));
    }
    check_inventory(store, MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES, 3);
    CHECK(o.failures == 0 && o.stops == 0);
    CHECK(mo_cell_get_int(mo_service_state(s)) ==
          7 + 11 * (MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES + 3));
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
    check_add_log(s, 101, 3);
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
        check_encoding(s, image);
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
        check_encoding(s, image);
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
    check_encoding(s, image);
    crash_point = 4;
    add(s, 11);
    crash_point = 0;
    CHECK(o.status == MO_SERVICE_STORE && !mo_service_running(s) && o.stops == 1);
    CHECK(strcmp(mo_service_status_text(o.status),
                 "service store failed; reopen it to see whether the last change was saved") == 0);
    CHECK(mo_cell_get_int(mo_service_state(s)) == 7);
    check_add_log(s, 1, 0);
    check_encoding(s, image);
    mo_service_free(s);
    s = open_service(image, store, &o);
    check_encoding(s, image);
    CHECK(mo_cell_get_int(mo_service_state(s)) == 18);
    check_add_log(s, 1, 1);
    crash_point = 5;
    add(s, 5);
    crash_point = 0;
    CHECK(o.status == MO_SERVICE_STORE && !mo_service_running(s));
    check_add_log(s, 1, 1);
    check_encoding(s, image);
    mo_service_free(s);
    s = open_service(image, store, &o);
    check_encoding(s, image);
    CHECK(mo_cell_get_int(mo_service_state(s)) == 18);
    check_add_log(s, 1, 1);
    check_inventory(store, 0, 1);
    mo_service_free(s);
    crash_point = 7;
    struct mo_service_hooks hooks = {clock_now, reply, event, &o};
    s = NULL;
    CHECK(mo_service_open(image, 0, &hooks, store, &s) == MO_SERVICE_STORE && s == NULL);
    crash_point = 0;
    s = open_service(image, store, &o);
    CHECK(mo_cell_get_int(mo_service_state(s)) == 18);
    check_add_log(s, 1, 1);
    mo_service_free(s);
    mo_store_close(store);
}

static void failed_snapshot(struct mo_image *image) {
    struct mo_store *store;
    MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
    struct observations o = {0};
    struct mo_service_loop *s = open_service(image, store, &o);
    check_encoding(s, image);
    for (unsigned i = 1; i < MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES; i++)
        add(s, 1);
    crash_point = 6;
    add(s, 1);
    crash_point = 0;
    CHECK(o.status == MO_SERVICE_STORE && !mo_service_running(s));
    CHECK(mo_cell_get_int(mo_service_state(s)) == 6 + MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES);
    check_inventory(store, 0, MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES);
    struct mo_service_observation before_recovery = mo_service_observe(s);
    CHECK(before_recovery.sequence == 100 && before_recovery.snapshot_sequence == 0 &&
          before_recovery.encoded_sequence == 99);
    check_add_log(s, 1, MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES);
    const uint8_t *saved;
    size_t saved_size;
    MO_ASSERT(mo_service_encoded_state(s, &saved, &saved_size));
    struct mo_export_identity identity = {.kind = 1,
                                          .label = "snapshot-failure",
                                          .sequence = before_recovery.sequence,
                                          .encoded_sequence = before_recovery.encoded_sequence};
    struct mo_export_pin pin = {0};
    MO_ASSERT(mo_export_create(&pin, &identity, saved, saved_size, 0));
    struct mo_binary framing = {pin.bytes, pin.length, 70 + strlen(identity.label)};
    uint64_t durable, encoded;
    CHECK(mo_binary_uint(&framing, 8, &durable) && durable == 100);
    CHECK(mo_binary_uint(&framing, 8, &encoded) && encoded == 99);

    check_encoding(s, image);
    mo_service_free(s);
    s = open_service(image, store, &o);
    check_encoding(s, image);
    CHECK(mo_cell_get_int(mo_service_state(s)) == 7 + MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES);
    check_inventory(store, MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES, 0);
    struct mo_service_observation recovered = mo_service_observe(s);
    CHECK(recovered.sequence == 100 && recovered.snapshot_sequence == 100 &&
          recovered.encoded_sequence == 100);
    check_add_log(s, 101, 0);
    /* Recovery changes current encoding; the earlier exported bytes keep their provenance. */
    CHECK(pin.sequence == 100 && pin.encoded_sequence == 99);
    framing.position = 70 + strlen(identity.label);
    CHECK(mo_binary_uint(&framing, 8, &durable) && durable == 100);
    CHECK(mo_binary_uint(&framing, 8, &encoded) && encoded == 99);
    mo_export_release(&pin);

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
    check_encoding(s, image);
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
    check_encoding(s, image);
    mo_service_free(s);
    s = open_service(image, store, &o);
    check_encoding(s, image);
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
        char source[512];
        MO_ASSERT(
            snprintf(source, sizeof source,
                     "module Main\nservice\nstate Text = \"a\".repeat(%zu)\ntools keep, broken\n"
                     "token from env \"TOKEN\"\nend\n"
                     "fn keep(state: Text, _now: Time) : (Text, Int)\n(state, 0)\nend\n"
                     "fn broken(_state: Text, _now: Time) : Bool\nstop \"near-limit\"\nend\n",
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
            CHECK(mo_service_send(s, 1, NULL, 0, 1) == MO_SERVICE_OK);
            CHECK(mo_service_step(s));
            struct mo_service_event_summary event;
            CHECK(mo_service_event_entry(s, 0, &event));
            CHECK(event.record_id == 1 && event.record_unavailable == NULL);
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
    check_encoding(s, image);
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
    check_encoding(s, image);
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
    CHECK(mo_image_key(other, 0) != mo_image_key(image, 0));
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

static uint64_t elapsed_us(const struct timespec *start) {
    struct timespec now;
    MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    int64_t us = (int64_t)(now.tv_sec - start->tv_sec) * 1000000 +
                 (int64_t)(now.tv_nsec - start->tv_nsec) / 1000;
    MO_ASSERT(us >= 0);
    return (uint64_t)us;
}

static size_t file_bytes(const char *path, unsigned char *out, size_t capacity) {
    FILE *file = fopen(path, "rb");
    MO_ASSERT(file != NULL);
    size_t length = fread(out, 1, capacity, file);
    MO_ASSERT(length < capacity && fclose(file) == 0);
    return length;
}

/* A child process holds the store at path as a second mo serve does (exclusive, through
 * mo_store_open) or as a reader does (a shared read transaction). It lets go after
 * hold_us, or, when hold_us is zero, when the parent closes *release. */
static pid_t hold_store(const char *path, bool reader, unsigned hold_us, int *release) {
    int ready[2], hold[2];
    MO_ASSERT(pipe(ready) == 0 && pipe(hold) == 0);
    pid_t pid = fork();
    MO_ASSERT(pid >= 0);
    if (pid == 0) {
        MO_ASSERT(close(ready[0]) == 0 && close(hold[1]) == 0);
        struct mo_store *store = NULL;
        sqlite3 *db = NULL;
        if (reader ? sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK ||
                         sqlite3_exec(db, "BEGIN; SELECT count(*) FROM log", NULL, NULL, NULL) !=
                             SQLITE_OK
                   : mo_store_open(path, &store) != MO_STORE_OK)
            _exit(1);
        char byte = 'r';
        if (write(ready[1], &byte, 1) != 1)
            _exit(2);
        struct timespec pause = {hold_us / 1000000, (long)(hold_us % 1000000) * 1000};
        if (hold_us != 0)
            MO_ASSERT(nanosleep(&pause, NULL) == 0);
        else if (read(hold[0], &byte, 1) != 0)
            _exit(3);
        if (reader)
            MO_ASSERT(sqlite3_close(db) == SQLITE_OK);
        else
            mo_store_close(store);
        _exit(0);
    }
    MO_ASSERT(close(ready[1]) == 0 && close(hold[0]) == 0);
    char byte = 0;
    MO_ASSERT(read(ready[0], &byte, 1) == 1 && byte == 'r');
    MO_ASSERT(close(ready[0]) == 0);
    if (release != NULL)
        *release = hold[1];
    else
        MO_ASSERT(close(hold[1]) == 0);
    return pid;
}

/* A first-start child: CREATE, exclusive BEGIN, one table. The database file stays
 * zero bytes until COMMIT. Signals ready, then sleeps hold_us or waits on *release.
 * commit true: COMMIT before close. commit false: ROLLBACK, so the file stays empty. */
static pid_t hold_first_commit(const char *path, bool commit, unsigned hold_us, int *release) {
    int ready[2], hold[2];
    MO_ASSERT(pipe(ready) == 0 && pipe(hold) == 0);
    pid_t pid = fork();
    MO_ASSERT(pid >= 0);
    if (pid == 0) {
        MO_ASSERT(close(ready[0]) == 0 && close(hold[1]) == 0);
        sqlite3 *db = NULL;
        if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) !=
                SQLITE_OK ||
            sqlite3_exec(db,
                         "PRAGMA journal_mode=DELETE;PRAGMA locking_mode=EXCLUSIVE;"
                         "BEGIN EXCLUSIVE;"
                         "CREATE TABLE head(id INTEGER PRIMARY KEY CHECK(id=1), seq INTEGER "
                         "NOT NULL) STRICT",
                         NULL, NULL, NULL) != SQLITE_OK)
            _exit(1);
        char byte = 'r';
        if (write(ready[1], &byte, 1) != 1)
            _exit(2);
        struct timespec pause = {hold_us / 1000000, (long)(hold_us % 1000000) * 1000};
        if (hold_us != 0)
            MO_ASSERT(nanosleep(&pause, NULL) == 0);
        else if (read(hold[0], &byte, 1) != 0)
            _exit(3);
        if (sqlite3_exec(db, commit ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL) != SQLITE_OK ||
            sqlite3_close(db) != SQLITE_OK)
            _exit(4);
        _exit(0);
    }
    MO_ASSERT(close(ready[1]) == 0 && close(hold[0]) == 0);
    char byte = 0;
    MO_ASSERT(read(ready[0], &byte, 1) == 1 && byte == 'r');
    MO_ASSERT(close(ready[0]) == 0);
    if (release != NULL)
        *release = hold[1];
    else
        MO_ASSERT(close(hold[1]) == 0);
    return pid;
}

static void child_done(pid_t pid) {
    int status;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* mo serve's open waits for a store another process holds, and gives up with BUSY when the
 * holder outlasts the wait. This build shortens the wait to one second. */
static void store_wait(struct mo_image *image, const char *path) {
    CHECK(MO_LIMIT_SERVE_STORE_WAIT_US == 1000000); /* the Makefile shortens the wait */
    CHECK(strcmp(mo_store_status_text(MO_STORE_BUSY),
                 "another process holds the service store, such as a second mo serve or a "
                 "reader of its data folder") == 0);
    struct mo_store *store = NULL;
    /* sqlite3_open in the first-start child needs the store allocator. */
    MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
    mo_store_close(store);
    store = NULL;
    {
        int release_first = -1;
        pid_t first = hold_first_commit(path, false, 0, &release_first);
        struct stat held;
        CHECK(stat(path, &held) == 0 && held.st_size == 0);
        struct timespec first_start;
        MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &first_start) == 0);
        CHECK(mo_store_open_wait(path, &store) == MO_STORE_BUSY && store == NULL);
        uint64_t first_waited = elapsed_us(&first_start);
        CHECK(first_waited >= MO_LIMIT_SERVE_STORE_WAIT_US * 9 / 10 &&
              first_waited < 3 * MO_LIMIT_SERVE_STORE_WAIT_US);
        CHECK(stat(path, &held) == 0 && held.st_size == 0);
        MO_ASSERT(close(release_first) == 0);
        child_done(first);
        CHECK(stat(path, &held) == 0 && held.st_size == 0);
        MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &first_start) == 0);
        CHECK(mo_store_open_wait(path, &store) == MO_STORE_CORRUPT && store == NULL);
        CHECK(elapsed_us(&first_start) < MO_LIMIT_SERVE_STORE_WAIT_US / 4);
        CHECK(stat(path, &held) == 0 && held.st_size == 0);
        MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &first_start) == 0);
        CHECK(mo_store_open(path, &store) == MO_STORE_CORRUPT && store == NULL);
        CHECK(elapsed_us(&first_start) < MO_LIMIT_SERVE_STORE_WAIT_US / 4);
        CHECK(stat(path, &held) == 0 && held.st_size == 0);
        CHECK(unlink(path) == 0);
        first = hold_first_commit(path, true, 300000, NULL);
        CHECK(stat(path, &held) == 0 && held.st_size == 0);
        MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &first_start) == 0);
        CHECK(mo_store_open_wait(path, &store) == MO_STORE_OK && store != NULL);
        first_waited = elapsed_us(&first_start);
        CHECK(first_waited >= 200000 && first_waited < MO_LIMIT_SERVE_STORE_WAIT_US);
        mo_store_close(store);
        store = NULL;
        child_done(first);
        CHECK(unlink(path) == 0);
    }
    MO_ASSERT(mo_store_open(path, &store) == MO_STORE_OK);
    struct observations o = {0};
    struct mo_service_loop *s = open_service(image, store, &o);
    add(s, 11);
    mo_service_free(s);
    mo_store_close(store);
    for (unsigned reader = 0; reader <= 1; reader++) {
        pid_t pid = hold_store(path, reader != 0, 300000, NULL);
        struct timespec start;
        MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
        store = NULL;
        CHECK(mo_store_open_wait(path, &store) == MO_STORE_OK);
        uint64_t waited = elapsed_us(&start);
        CHECK(waited >= 200000 && waited < MO_LIMIT_SERVE_STORE_WAIT_US);
        if (store == NULL)
            continue;
        s = open_service(image, store, &o);
        CHECK(mo_cell_get_int(mo_service_state(s)) == 18);
        mo_service_free(s);
        mo_store_close(store);
        child_done(pid);
    }
    static unsigned char before[1 << 16], after[1 << 16];
    size_t length = file_bytes(path, before, sizeof before);
    for (unsigned reader = 0; reader <= 1; reader++) {
        int release = -1;
        pid_t pid = hold_store(path, reader != 0, 0, &release);
        for (unsigned attempt = 0; attempt < 3; attempt++) {
            struct timespec start;
            MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
            store = NULL;
            CHECK(mo_store_open_wait(path, &store) == MO_STORE_BUSY && store == NULL);
            uint64_t waited = elapsed_us(&start);
            CHECK(waited >= MO_LIMIT_SERVE_STORE_WAIT_US * 9 / 10 &&
                  waited < 3 * MO_LIMIT_SERVE_STORE_WAIT_US);
            CHECK(file_bytes(path, after, sizeof after) == length &&
                  memcmp(before, after, length) == 0);
        }
        /* Without the wait a held store is refused at once. */
        struct timespec start;
        MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
        CHECK(mo_store_open(path, &store) == MO_STORE_BUSY && store == NULL);
        CHECK(elapsed_us(&start) < MO_LIMIT_SERVE_STORE_WAIT_US / 4);
        MO_ASSERT(close(release) == 0);
        child_done(pid);
        MO_ASSERT(mo_store_open_wait(path, &store) == MO_STORE_OK);
        s = open_service(image, store, &o);
        CHECK(mo_cell_get_int(mo_service_state(s)) == 18);
        mo_service_free(s);
        mo_store_close(store);
    }
    /* A damaged store is never waited for. */
    static const char garbage[4096] = "this is not a SQLite database";
    const char *contents[] = {"", garbage};
    size_t sizes[] = {0, sizeof garbage};
    for (unsigned i = 0; i < 2; i++) {
        FILE *file = fopen(path, "wb");
        MO_ASSERT(file != NULL && fwrite(contents[i], 1, sizes[i], file) == sizes[i] &&
                  fclose(file) == 0);
        struct timespec start;
        MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
        store = NULL;
        enum mo_store_status status = mo_store_open_wait(path, &store);
        CHECK(status == (i == 0 ? MO_STORE_CORRUPT : MO_STORE_IO) && store == NULL);
        CHECK(elapsed_us(&start) < MO_LIMIT_SERVE_STORE_WAIT_US / 4);
        if (i == 0) {
            struct stat leftover;
            CHECK(stat(path, &leftover) == 0 && leftover.st_size == 0);
            MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
            CHECK(mo_store_open(path, &store) == MO_STORE_CORRUPT && store == NULL);
            CHECK(elapsed_us(&start) < MO_LIMIT_SERVE_STORE_WAIT_US / 4);
            CHECK(stat(path, &leftover) == 0 && leftover.st_size == 0);
        }
    }
    CHECK(unlink(path) == 0);
}

static const char history_source[] =
    "module Main\nservice\nstate Int = 7\ntools add, pair, broken\nevery 1.minute tick\n"
    "token from env \"TOKEN\"\nend\n"
    "fn add(state: Int, _now: Time, n: Int) : (Int, Int)\n(state + n, state + n)\nend\n"
    "fn pair(state: Int, _now: Time, _a: Text, b: Int) : (Int, Int)\n(state + b, state)\nend\n"
    "fn broken(_state: Int, _now: Time) : (Int, Int)\nstop \"broken\"\nend\n"
    "fn tick(state: Int, _now: Time) : Int\nstate + 1\nend\n";

enum { HISTORY_T0 = 1767225600 };
static int64_t history_time(unsigned step) {
    return (int64_t)HISTORY_T0 * 1000000 + (int64_t)step * 1000;
}

static size_t encoded_size(struct mo_heap *heap, uint32_t type, struct mo_cell value) {
    struct mo_buffer bytes;
    mo_buffer_init(&bytes, MO_LIMIT_SAVED_BYTES);
    MO_ASSERT(mo_value_encode(heap, type, value, &bytes));
    size_t length = bytes.length;
    mo_buffer_free(&bytes);
    return length;
}

struct history_fixture {
    size_t add_bytes, text_bytes, int_bytes;
    uint8_t snapshot[64];
    size_t snapshot_length;
};

/* A service that ran a tick, add(11), pair("hello", 2) and three failing calls, then
 * stopped. Failure ids 1 and 3 remain: a repeated kind keeps its first and its latest. */
static void history_folder(struct mo_image *image, const char *path, struct history_fixture *f) {
    struct mo_store *store;
    MO_ASSERT(mo_store_open(path, &store) == MO_STORE_OK);
    struct observations o = {.time = {.now = history_time(0)}};
    struct mo_service_loop *s = open_service(image, store, &o);
    const uint8_t *saved = NULL;
    MO_ASSERT(mo_service_encoded_state(s, &saved, &f->snapshot_length));
    MO_ASSERT(f->snapshot_length <= sizeof f->snapshot);
    memcpy(f->snapshot, saved, f->snapshot_length);
    CHECK(mo_service_step(s));
    const struct mo_bytecode_service *d = &mo_image_bytecode(image)->services[0];
    struct mo_heap *heap = mo_service_heap(s);
    o.time.now = history_time(1);
    struct mo_cell n = mo_cell_int(11);
    f->add_bytes = encoded_size(heap, d->tools[0].argument_types[0], n);
    CHECK(mo_service_send(s, 0, &n, 1, 1) == MO_SERVICE_OK && mo_service_step(s));
    o.time.now = history_time(2);
    struct mo_cell pair[2];
    MO_ASSERT(mo_value_text(heap, "hello", 5, &pair[0]));
    pair[1] = mo_cell_int(2);
    f->text_bytes = encoded_size(heap, d->tools[1].argument_types[0], pair[0]);
    f->int_bytes = encoded_size(heap, d->tools[1].argument_types[1], pair[1]);
    CHECK(mo_service_send(s, 1, pair, 2, 2) == MO_SERVICE_OK && mo_service_step(s));
    mo_value_drop(heap, d->tools[1].argument_types[0], pair[0]);
    for (unsigned i = 0; i < 3; i++) {
        o.time.now = history_time(3 + i);
        CHECK(mo_service_send(s, 2, NULL, 0, 3 + i) == MO_SERVICE_OK && mo_service_step(s));
    }
    CHECK(o.failures == 3 && mo_cell_get_int(mo_service_state(s)) == 7 + 1 + 11 + 2);
    mo_service_free(s);
    mo_store_close(store);
}

struct failure_copy {
    uint64_t ids[4];
    char kinds[4][64];
    bool first[4];
    size_t state_bytes[4], message_bytes[4];
    unsigned count;
};
static bool copy_failure(const struct mo_store_failure *f, void *context) {
    struct failure_copy *c = context;
    MO_ASSERT(c->count < 4 && strlen(f->kind) < sizeof c->kinds[0]);
    c->ids[c->count] = f->id;
    strcpy(c->kinds[c->count], f->kind);
    c->first[c->count] = f->first;
    c->state_bytes[c->count] = f->state.length;
    c->message_bytes[c->count] = f->message.length;
    c->count++;
    return true;
}
/* The failures as the read-write store reports them. */
static struct failure_copy stored_failures(const char *path) {
    struct failure_copy c = {0};
    struct mo_store *store;
    MO_ASSERT(mo_store_open(path, &store) == MO_STORE_OK);
    CHECK(mo_store_failures(store, copy_failure, &c) == MO_STORE_OK);
    mo_store_close(store);
    return c;
}

static unsigned char history_before[4 << 20], history_after[4 << 20];
static size_t history_length;
static void remember_bytes(const char *path) {
    history_length = file_bytes(path, history_before, sizeof history_before);
}
static bool bytes_unchanged(const char *path) {
    return file_bytes(path, history_after, sizeof history_after) == history_length &&
           memcmp(history_before, history_after, history_length) == 0;
}

static void history_stopped(struct mo_image *image, const char *path) {
    struct history_fixture f;
    history_folder(image, path, &f);
    struct failure_copy stored = stored_failures(path);
    CHECK(stored.count == 2 && stored.ids[0] == 1 && stored.ids[1] == 3);
    remember_bytes(path);
    struct mo_store_history h;
    CHECK(mo_store_history_read(path, false, &h) == MO_STORE_FOLDER_OK);
    CHECK(bytes_unchanged(path));
    CHECK(!h.recovered && h.failure_tables && h.has_snapshot && h.damaged_sequence == 0);
    CHECK(h.snapshot.kind == MO_STORE_STATE && h.snapshot.sequence == 0 &&
          h.snapshot.image == mo_image_key(image, 0));
    CHECK(h.snapshot.bytes != NULL && h.snapshot.length == f.snapshot_length &&
          memcmp(h.snapshot.bytes, f.snapshot, f.snapshot_length) == 0);
    CHECK(h.head == 3 && h.log_count == 3 && h.entry_count == 3);
    if (h.entry_count == 3) {
        const struct mo_store_history_entry *e = h.entries;
        CHECK(e[0].sequence == 1 && e[0].timer && e[0].handler == 0 && e[0].argument_count == 0 &&
              e[0].arrival == history_time(0));
        CHECK(e[1].sequence == 2 && !e[1].timer && e[1].handler == 0 &&
              e[1].arrival == history_time(1) && e[1].argument_count == 1 &&
              e[1].argument_bytes[0] == f.add_bytes);
        CHECK(e[2].sequence == 3 && !e[2].timer && e[2].handler == 1 &&
              e[2].arrival == history_time(2) && e[2].argument_count == 2 &&
              e[2].argument_bytes[0] == f.text_bytes && e[2].argument_bytes[1] == f.int_bytes);
    }
    CHECK(h.failure_total == 2 && h.failure_count == 2);
    for (unsigned i = 0; i < 2 && i < h.failure_count; i++) {
        const struct mo_store_history_failure *x = &h.failures[i];
        CHECK(x->id == stored.ids[i] && strcmp(x->kind, stored.kinds[i]) == 0 &&
              x->first == stored.first[i] && x->state_bytes == stored.state_bytes[i]);
        CHECK(x->message.kind == MO_STORE_FAILURE_MESSAGE &&
              x->message.length == stored.message_bytes[i] &&
              x->message.image == mo_image_key(image, 0));
        struct mo_failure_record record;
        CHECK(x->state_valid);
        CHECK(mo_failure_read(x->message.bytes, x->message.length, &record) && record.id == x->id &&
              record.handler == 2 && !record.timer);
    }
    CHECK(h.left_bytes == 0 && h.retained_bytes > 0 && h.retained_bytes <= MO_LIMIT_HISTORY_BYTES);
    mo_store_history_free(&h);

    struct mo_store_history_pair pair;
    CHECK(mo_store_history_pair(path, false, 3, &pair) == MO_STORE_FOLDER_OK);
    CHECK(pair.found && pair.failure_tables && pair.id == 3 && !pair.first &&
          strcmp(pair.kind, stored.kinds[1]) == 0);
    CHECK(pair.state.kind == MO_STORE_FAILURE_STATE && pair.state.length == stored.state_bytes[1]);
    CHECK(pair.message.length == stored.message_bytes[1]);
    struct mo_cell state;
    const struct mo_bytecode_service *d = &mo_image_bytecode(image)->services[0];
    struct mo_store *store;
    MO_ASSERT(mo_store_open(path, &store) == MO_STORE_OK);
    struct observations o = {.time = {.now = history_time(9)}};
    struct mo_service_loop *s = open_service(image, store, &o);
    CHECK(mo_cell_get_int(mo_service_state(s)) == 21);
    CHECK(mo_value_decode(mo_service_heap(s), d->state_type, pair.state.bytes, pair.state.length,
                          &state) &&
          mo_cell_get_int(state) == 21);
    mo_service_free(s);
    mo_store_close(store);
    mo_store_history_pair_free(&pair);
    remember_bytes(path);
    /* An evicted id reports the ids the folder holds. */
    CHECK(mo_store_history_pair(path, false, 2, &pair) == MO_STORE_FOLDER_OK);
    CHECK(!pair.found && pair.retained_count == 2 && pair.retained[0] == 1 &&
          pair.retained[1] == 3);
    mo_store_history_pair_free(&pair);
    CHECK(bytes_unchanged(path));
}

static void history_owned(struct mo_image *image, const char *path) {
    struct history_fixture f;
    history_folder(image, path, &f);
    int release = -1;
    pid_t pid = hold_store(path, false, 0, &release);
    struct timespec start;
    MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    struct mo_store_history h;
    CHECK(mo_store_history_read(path, false, &h) == MO_STORE_FOLDER_OWNED);
    CHECK(mo_store_history_read(path, true, &h) == MO_STORE_FOLDER_OWNED);
    struct mo_store_history_pair pair;
    CHECK(mo_store_history_pair(path, false, 1, &pair) == MO_STORE_FOLDER_OWNED);
    CHECK(elapsed_us(&start) < 250000);
    MO_ASSERT(close(release) == 0);
    child_done(pid);
    /* Two readers share the folder. */
    pid = hold_store(path, true, 0, &release);
    CHECK(mo_store_history_read(path, false, &h) == MO_STORE_FOLDER_OK);
    mo_store_history_free(&h);
    MO_ASSERT(close(release) == 0);
    child_done(pid);
    CHECK(unlink(path) == 0);
}

static bool journal_exists(const char *path) {
    char journal[300];
    MO_ASSERT(snprintf(journal, sizeof journal, "%s-journal", path) > 0);
    return access(journal, F_OK) == 0;
}

/* A process killed in the middle of a write that has reached the database file leaves a hot
 * journal. A one-mebibyte record overflows the store's page cache, so SQLite syncs the journal
 * and writes pages before the write is killed. */
static void history_hot_journal(struct mo_image *image, const char *path) {
    struct history_fixture f;
    history_folder(image, path, &f);
    pid_t pid = fork();
    MO_ASSERT(pid >= 0);
    if (pid == 0) {
        struct mo_store *store;
        MO_ASSERT(mo_store_open(path, &store) == MO_STORE_OK);
        static uint8_t large[1 << 20];
        struct mo_store_record r = {MO_STORE_LOG, 4, mo_image_key(image, 0), large, sizeof large};
        crash_point = 30;
        enum mo_store_status unreached = mo_store_call(store, MO_STORE_APPEND, &r, NULL, NULL);
        _exit(unreached == MO_STORE_OK ? 99 : 98);
    }
    int status;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 81);
    CHECK(journal_exists(path));
    remember_bytes(path);
    struct mo_store_history h = {0};
    CHECK(mo_store_history_read(path, false, &h) == MO_STORE_FOLDER_RECOVERY);
    struct mo_store_history_pair pair;
    CHECK(mo_store_history_pair(path, false, 1, &pair) == MO_STORE_FOLDER_RECOVERY);
    CHECK(bytes_unchanged(path) && journal_exists(path));
    CHECK(mo_store_history_read(path, true, &h) == MO_STORE_FOLDER_OK);
    CHECK(h.recovered && h.failure_total == 2 && h.failures[1].id == 3 && h.head == 3 &&
          h.log_count == 3);
    mo_store_history_free(&h);
    /* Rolled back: a reader no longer needs to recover, and the store opens at once. */
    CHECK(mo_store_history_read(path, false, &h) == MO_STORE_FOLDER_OK && !h.recovered);
    mo_store_history_free(&h);
    struct failure_copy stored = stored_failures(path);
    CHECK(stored.count == 2 && stored.ids[1] == 3);
    struct mo_store *store;
    CHECK(mo_store_open(path, &store) == MO_STORE_OK);
    struct observations o = {.time = {.now = history_time(9)}};
    struct mo_service_loop *s = open_service(image, store, &o);
    CHECK(mo_cell_get_int(mo_service_state(s)) == 21);
    mo_service_free(s);
    mo_store_close(store);
    CHECK(unlink(path) == 0);
}

/* A service killed while idle leaves a journal beside the database that is not hot. */
static void history_idle_kill(struct mo_image *image, const char *path) {
    struct history_fixture f;
    history_folder(image, path, &f);
    pid_t pid = fork();
    MO_ASSERT(pid >= 0);
    if (pid == 0) {
        struct mo_store *store;
        MO_ASSERT(mo_store_open(path, &store) == MO_STORE_OK);
        struct observations o = {.time = {.now = history_time(9)}};
        struct mo_service_loop *s = open_service(image, store, &o);
        struct mo_cell n = mo_cell_int(5);
        CHECK(mo_service_send(s, 0, &n, 1, 9) == MO_SERVICE_OK && mo_service_step(s) &&
              mo_service_step(s));
        _exit(0);
    }
    child_done(pid);
    CHECK(journal_exists(path));
    remember_bytes(path);
    struct mo_store_history h;
    CHECK(mo_store_history_read(path, false, &h) == MO_STORE_FOLDER_OK);
    CHECK(!h.recovered && h.head == 5 && h.entry_count == 5 && h.entries[3].timer &&
          h.entries[4].argument_count == 1);
    mo_store_history_free(&h);
    CHECK(bytes_unchanged(path));
    CHECK(unlink(path) == 0);
}

static void history_before_capture(struct mo_image *image, const char *path) {
    struct history_fixture f;
    history_folder(image, path, &f);
    sqlite3 *db;
    MO_ASSERT(sqlite3_open(path, &db) == SQLITE_OK);
    MO_ASSERT(sqlite3_exec(db, "DROP TABLE failure; DROP TABLE failure_state", NULL, NULL, NULL) ==
              SQLITE_OK);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    remember_bytes(path);
    struct mo_store_history h;
    CHECK(mo_store_history_read(path, false, &h) == MO_STORE_FOLDER_OK);
    CHECK(!h.failure_tables && h.failure_total == 0 && h.failure_count == 0 && h.head == 3 &&
          h.entry_count == 3);
    mo_store_history_free(&h);
    struct mo_store_history_pair pair;
    CHECK(mo_store_history_pair(path, false, 1, &pair) == MO_STORE_FOLDER_OK);
    CHECK(!pair.found && !pair.failure_tables && pair.retained_count == 0);
    mo_store_history_pair_free(&pair);
    CHECK(bytes_unchanged(path));
    MO_ASSERT(sqlite3_open(path, &db) == SQLITE_OK);
    sqlite3_stmt *stmt;
    MO_ASSERT(sqlite3_prepare_v2(db, "SELECT count(*) FROM sqlite_schema WHERE name LIKE 'fail%'",
                                 -1, &stmt, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 0);
    CHECK(sqlite3_finalize(stmt) == SQLITE_OK && sqlite3_close(db) == SQLITE_OK);
    /* Only one of the two failure tables is a damaged store. */
    MO_ASSERT(sqlite3_open(path, &db) == SQLITE_OK);
    MO_ASSERT(sqlite3_exec(db,
                           "CREATE TABLE failure_state(id INTEGER PRIMARY KEY, record BLOB NOT "
                           "NULL) STRICT",
                           NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    CHECK(mo_store_history_read(path, false, &h) == MO_STORE_FOLDER_DAMAGED);
    CHECK(unlink(path) == 0);
}

static void history_unreadable(struct mo_image *image, const char *dir, const char *path) {
    struct history_fixture f;
    history_folder(image, path, &f);
    struct mo_store_history h;
    struct mo_store_history_pair pair;
    if (geteuid() != 0) {
        CHECK(chmod(path, 0) == 0);
        CHECK(mo_store_history_read(path, false, &h) == MO_STORE_FOLDER_UNREADABLE);
        CHECK(mo_store_history_read(path, true, &h) == MO_STORE_FOLDER_UNREADABLE);
        CHECK(mo_store_history_pair(path, false, 1, &pair) == MO_STORE_FOLDER_UNREADABLE);
        CHECK(chmod(path, 0600) == 0);
        CHECK(chmod(dir, 0) == 0);
        CHECK(mo_store_history_read(path, false, &h) == MO_STORE_FOLDER_UNREADABLE);
        CHECK(chmod(dir, 0700) == 0);
    }
    /* The classification follows SQLite's result code wherever the first read meets it. */
    const int codes[] = {SQLITE_CANTOPEN, SQLITE_PERM,   SQLITE_BUSY,  SQLITE_READONLY_ROLLBACK,
                         SQLITE_CORRUPT,  SQLITE_NOTADB, SQLITE_IOERR, SQLITE_NOMEM};
    const enum mo_store_folder expected[] = {MO_STORE_FOLDER_UNREADABLE, MO_STORE_FOLDER_UNREADABLE,
                                             MO_STORE_FOLDER_OWNED,      MO_STORE_FOLDER_RECOVERY,
                                             MO_STORE_FOLDER_DAMAGED,    MO_STORE_FOLDER_DAMAGED,
                                             MO_STORE_FOLDER_IO,         MO_STORE_FOLDER_MEMORY};
    remember_bytes(path);
    for (unsigned i = 0; i < sizeof codes / sizeof codes[0]; i++) {
        reader_code = codes[i];
        CHECK(mo_store_history_read(path, false, &h) == expected[i]);
        CHECK(mo_store_history_pair(path, false, 1, &pair) == expected[i]);
        reader_code = 0;
    }
    CHECK(bytes_unchanged(path));
    CHECK(mo_store_history_read(path, false, &h) == MO_STORE_FOLDER_OK);
    mo_store_history_free(&h);
    CHECK(unlink(path) == 0);
    /* No file is no saved data, and the reader creates none. */
    CHECK(mo_store_history_read(path, false, &h) == MO_STORE_FOLDER_MISSING);
    CHECK(mo_store_history_read(path, true, &h) == MO_STORE_FOLDER_MISSING);
    CHECK(mo_store_history_pair(path, true, 1, &pair) == MO_STORE_FOLDER_MISSING);
    struct stat info;
    CHECK(stat(path, &info) != 0 && errno == ENOENT);
    /* A file that is not a SQLite database is damaged. */
    FILE *file = fopen(path, "wb");
    static const char garbage[4096] = "not a store";
    MO_ASSERT(file != NULL && fwrite(garbage, 1, sizeof garbage, file) == sizeof garbage &&
              fclose(file) == 0);
    CHECK(mo_store_history_read(path, false, &h) == MO_STORE_FOLDER_DAMAGED);
    CHECK(unlink(path) == 0);
}

/* Rewrites one byte of the log row at seq, as damage on disk would. */
static void damage_log(const char *path, uint64_t seq, size_t offset) {
    sqlite3 *db;
    MO_ASSERT(sqlite3_open(path, &db) == SQLITE_OK);
    sqlite3_blob *blob;
    MO_ASSERT(sqlite3_blob_open(db, "main", "log", "record", (sqlite3_int64)seq, 1, &blob) ==
              SQLITE_OK);
    uint8_t byte;
    MO_ASSERT(sqlite3_blob_read(blob, &byte, 1, (int)offset) == SQLITE_OK);
    byte ^= 0x40;
    MO_ASSERT(sqlite3_blob_write(blob, &byte, 1, (int)offset) == SQLITE_OK);
    MO_ASSERT(sqlite3_blob_close(blob) == SQLITE_OK && sqlite3_close(db) == SQLITE_OK);
}

/* The runtime numbers failure records from 1. A record moved to id 0 or below is damage that
 * every read refuses: the catalog reads ids above a cursor that starts at 0, so without the
 * check the history would leave the record out and call the folder healthy. */
static void history_nonpositive_failure(struct mo_image *image, const char *path) {
    struct history_fixture f;
    history_folder(image, path, &f);
    sqlite3 *db;
    MO_ASSERT(sqlite3_open(path, &db) == SQLITE_OK);
    MO_ASSERT(sqlite3_exec(db,
                           "UPDATE failure SET id=-3 WHERE id=3;"
                           "UPDATE failure_state SET id=-3 WHERE id=3",
                           NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    struct mo_store_history h;
    CHECK(mo_store_history_read(path, false, &h) == MO_STORE_FOLDER_DAMAGED);
    struct mo_store_history_pair pair;
    CHECK(mo_store_history_pair(path, false, 1, &pair) == MO_STORE_FOLDER_DAMAGED);
    CHECK(mo_store_history_pair(path, false, 2, &pair) == MO_STORE_FOLDER_DAMAGED);
    CHECK(unlink(path) == 0);
}

static void history_damaged_log(struct mo_image *image, const char *path) {
    /* A byte of an argument, of the payload header, and of the frame's own header. */
    const size_t offsets[] = {26 + 25 + 4, 26 + 1, 7};
    for (unsigned i = 0; i < 3; i++) {
        struct history_fixture f;
        history_folder(image, path, &f);
        damage_log(path, 2, offsets[i]);
        struct mo_store_history h = {0};
        CHECK(mo_store_history_read(path, false, &h) == MO_STORE_FOLDER_DAMAGED);
        CHECK(h.damaged_sequence == 2);
        CHECK(unlink(path) == 0);
    }
}

/* Last byte of the MOST checksum, as accidental corruption of a retained state frame. */
static void damage_checksum(const char *path, const char *table, uint64_t rowid) {
    sqlite3 *db;
    sqlite3_blob *blob;
    MO_ASSERT(sqlite3_open(path, &db) == SQLITE_OK);
    MO_ASSERT(sqlite3_blob_open(db, "main", table, "record", (sqlite3_int64)rowid, 1, &blob) ==
              SQLITE_OK);
    int n = sqlite3_blob_bytes(blob);
    MO_ASSERT(n >= 8);
    uint8_t byte;
    MO_ASSERT(sqlite3_blob_read(blob, &byte, 1, n - 1) == SQLITE_OK);
    byte ^= 0x40;
    MO_ASSERT(sqlite3_blob_write(blob, &byte, 1, n - 1) == SQLITE_OK);
    MO_ASSERT(sqlite3_blob_close(blob) == SQLITE_OK && sqlite3_close(db) == SQLITE_OK);
}

static bool catalog_rows(const struct mo_store_failure *f, void *context) {
    CHECK(f->state.bytes == NULL);
    (*(unsigned *)context)++;
    return true;
}

static bool one_failure(const struct mo_store_failure *f, void *context) {
    *(uint64_t *)context = f->id;
    return true;
}

/* A checksum-only flip of one retained pre-call state: history still lists the folder and the
 * sibling, but does not claim that id is replayable. Get of the damaged id is corrupt; the
 * live catalog is still length-only and succeeds. */
static void history_corrupt_state_checksum(struct mo_image *image, const char *path) {
    struct history_fixture f;
    history_folder(image, path, &f);
    struct mo_store_history h;
    CHECK(mo_store_history_read(path, false, &h) == MO_STORE_FOLDER_OK);
    CHECK(h.failure_count == 2 && h.failures[0].state_valid && h.failures[1].state_valid);
    mo_store_history_free(&h);
    damage_checksum(path, "failure_state", 1);
    CHECK(mo_store_history_read(path, false, &h) == MO_STORE_FOLDER_OK);
    CHECK(h.failure_count == 2 && h.failures[0].id == 1 && !h.failures[0].state_valid);
    CHECK(h.failures[1].id == 3 && h.failures[1].state_valid);
    mo_store_history_free(&h);
    struct mo_store_history_pair pair;
    CHECK(mo_store_history_pair(path, false, 1, &pair) == MO_STORE_FOLDER_DAMAGED);
    CHECK(mo_store_history_pair(path, false, 3, &pair) == MO_STORE_FOLDER_OK && pair.found);
    mo_store_history_pair_free(&pair);
    struct mo_store *store = NULL;
    CHECK(mo_store_open(path, &store) == MO_STORE_OK);
    unsigned count = 0;
    uint32_t omitted = 999;
    CHECK(mo_store_failure_catalog(store, 0, catalog_rows, &count, &omitted) == MO_STORE_OK);
    CHECK(count == 2 && omitted == 0);
    uint64_t id = 0;
    CHECK(mo_store_failure_get(store, 1, one_failure, &id) == MO_STORE_CORRUPT);
    CHECK(mo_store_failure_get(store, 3, one_failure, &id) == MO_STORE_OK && id == 3);
    mo_store_close(store);
    CHECK(unlink(path) == 0);
}

/* Writes a store directly: a snapshot of snapshot_bytes, three log records each with one
 * argument of argument_bytes, and failures with message_bytes payloads. */
static void history_sized(const char *path, size_t snapshot_bytes, size_t argument_bytes,
                          unsigned failures, size_t message_bytes) {
    static uint8_t filler[1 << 17];
    memset(filler, 0x2a, sizeof filler);
    MO_ASSERT(snapshot_bytes <= sizeof filler && argument_bytes <= sizeof filler &&
              message_bytes <= sizeof filler);
    struct mo_store *store;
    MO_ASSERT(mo_store_open(path, &store) == MO_STORE_OK);
    struct mo_store_record r = {MO_STORE_STATE, 0, 5, filler, snapshot_bytes};
    CHECK(mo_store_call(store, MO_STORE_SNAPSHOT, &r, NULL, NULL) == MO_STORE_OK);
    struct mo_buffer payload;
    mo_buffer_init(&payload, MO_LIMIT_SAVED_BYTES);
    for (uint64_t seq = 1; seq <= 3; seq++) {
        mo_buffer_clear(&payload);
        struct mo_log_record_header header = {.arrival = 1, .count = 1};
        CHECK(mo_log_record_header_write(&payload, &header) &&
              mo_log_record_argument_write(&payload, filler, argument_bytes));
        r = (struct mo_store_record){MO_STORE_LOG, seq, 5, payload.bytes, payload.length};
        CHECK(mo_store_call(store, MO_STORE_APPEND, &r, NULL, NULL) == MO_STORE_OK);
    }
    mo_buffer_free(&payload);
    for (unsigned i = 0; i < failures; i++) {
        char kind[16];
        MO_ASSERT(snprintf(kind, sizeof kind, "kind-%u", i) > 0);
        struct mo_store_record state = {MO_STORE_FAILURE_STATE, 3, 5, filler, 1};
        struct mo_store_record message = {MO_STORE_FAILURE_MESSAGE, 3, 5, filler, message_bytes};
        uint64_t id = 0;
        CHECK(mo_store_capture(store, kind, &state, &message, &id) == MO_STORE_CAPTURE_OK);
    }
    mo_store_close(store);
}

static void history_bound(const char *path) {
    CHECK(MO_LIMIT_HISTORY_BYTES == 65536); /* the Makefile shortens the bound */
    /* The snapshot and the tail fit; the second 20 KB failure message does not. */
    history_sized(path, 30000, 20000, 3, 20000);
    struct mo_store_history h;
    CHECK(mo_store_history_read(path, false, &h) == MO_STORE_FOLDER_OK);
    CHECK(h.snapshot.bytes != NULL && h.snapshot.length == 30000);
    CHECK(h.entry_count == 3 && h.log_count == 3 && h.entries[2].argument_bytes[0] == 20000);
    CHECK(h.failure_total == 3 && h.failure_count == 1 && h.failures[0].message.length == 20000);
    CHECK(h.retained_bytes <= MO_LIMIT_HISTORY_BYTES && h.retained_bytes > 50000);
    CHECK(h.left_bytes >= 2 * 20000 && h.left_bytes < 2 * 20000 + 1024);
    mo_store_history_free(&h);
    CHECK(unlink(path) == 0);
    /* A snapshot over the bound is left, and nothing after it is copied, but every log row
     * is still checked. */
    history_sized(path, 70000, 10, 1, 10);
    damage_log(path, 3, 26 + 25 + 4);
    CHECK(mo_store_history_read(path, false, &h) == MO_STORE_FOLDER_DAMAGED &&
          h.damaged_sequence == 3);
    CHECK(unlink(path) == 0);
    history_sized(path, 70000, 10, 1, 10);
    CHECK(mo_store_history_read(path, false, &h) == MO_STORE_FOLDER_OK);
    CHECK(h.has_snapshot && h.snapshot.bytes == NULL && h.snapshot.length == 70000);
    CHECK(h.entry_count == 0 && h.log_count == 3 && h.failure_count == 0 && h.failure_total == 1);
    CHECK(h.left_bytes >= 70000 + 10 && h.retained_bytes == 0);
    mo_store_history_free(&h);
    CHECK(unlink(path) == 0);
}

/* A reader killed while it copies leaves no lock behind. */
static void history_killed(struct mo_image *image, const char *path) {
    struct history_fixture f;
    history_folder(image, path, &f);
    pid_t pid = fork();
    MO_ASSERT(pid >= 0);
    if (pid == 0) {
        reader_killed = true;
        struct mo_store_history h;
        enum mo_store_folder unreached = mo_store_history_read(path, false, &h);
        _exit(unreached == MO_STORE_FOLDER_OK ? 99 : 98);
    }
    int status;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 80);
    struct timespec start;
    MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    struct mo_store *store = NULL;
    CHECK(mo_store_open(path, &store) == MO_STORE_OK);
    CHECK(elapsed_us(&start) < 250000);
    mo_store_close(store);
    CHECK(unlink(path) == 0);
}

static void history_reader(const char *dir, const char *path) {
    struct mo_image *image = compile(history_source);
    history_stopped(image, path);
    CHECK(unlink(path) == 0);
    history_owned(image, path);
    history_hot_journal(image, path);
    history_idle_kill(image, path);
    history_before_capture(image, path);
    history_unreadable(image, dir, path);
    history_damaged_log(image, path);
    history_corrupt_state_checksum(image, path);
    history_nonpositive_failure(image, path);
    history_bound(path);
    history_killed(image, path);
    mo_image_free(image);
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
    check_encoding(s, image);
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
        check_encoding(s, image);
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
            struct mo_store_record record = {(enum mo_store_record_kind)kind,
                                             kind == MO_STORE_STATE ? 0 : 1, mo_image_key(image, 0),
                                             bytes.bytes, bytes.length};
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
    check_encoding(s, image);
    add(s, 11);
    MO_ASSERT(mo_store_call(store, MO_STORE_LOAD, NULL, copy_record, &saved) == MO_STORE_OK);
    check_encoding(s, image);
    mo_service_free(s);
    s = open_service(image, store, &o);
    check_encoding(s, image);
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
    struct mo_store_record record = {MO_STORE_LOG, 1, mo_image_key(image, 0), saved.bytes.bytes,
                                     saved.bytes.length};
    MO_ASSERT(mo_store_call(store, MO_STORE_APPEND, &record, NULL, NULL) == MO_STORE_OK);
    struct mo_service_hooks hooks = {clock_now, reply, event, &o};
    unsigned replies = o.replies, failures = o.failures;
    s = NULL;
    check_recovery_heap = true;
    CHECK(mo_service_open(image, 0, &hooks, store, &s) == MO_SERVICE_SAVED_INVALID);
    check_recovery_heap = false;
    CHECK(s == NULL && o.replies == replies && o.failures == failures + 1);
    CHECK(named(o.handler, "add"));
    CHECK(recovery_heap_checks == 1);
    mo_service_free(s);
    mo_store_close(store);
    mo_buffer_free(&saved.bytes);
    mo_image_free(image);
}

static void replayed_timer_is_named(void) {
    struct mo_image *image = compile("module Main\nservice\nstate Int = 0\ntools read\n"
                                     "every 1.minute tick\ntoken from env \"TOKEN\"\nend\n"
                                     "fn read(state: Int, _now: Time) : List(Int)\n[state]\nend\n"
                                     "fn tick(state: Int, _now: Time) : Int\nstate + 1\nend\n");
    struct saved_record saved = {.kind = MO_STORE_LOG};
    mo_buffer_init(&saved.bytes, MO_LIMIT_SAVED_BYTES);
    struct mo_store *store;
    MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
    struct observations o = {0};
    struct mo_service_loop *s = open_service(image, store, &o);
    check_encoding(s, image);
    CHECK(mo_service_step(s));
    MO_ASSERT(mo_store_call(store, MO_STORE_LOAD, NULL, copy_record, &saved) == MO_STORE_OK);
    mo_service_free(s);
    mo_store_close(store);
    MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
    s = open_service(image, store, &o);
    mo_service_free(s);
    MO_ASSERT(saved.bytes.length > 17);
    saved.bytes.bytes[17] ^= 1;
    struct mo_store_record record = {MO_STORE_LOG, 1, mo_image_key(image, 0), saved.bytes.bytes,
                                     saved.bytes.length};
    MO_ASSERT(mo_store_call(store, MO_STORE_APPEND, &record, NULL, NULL) == MO_STORE_OK);
    struct mo_service_hooks hooks = {clock_now, reply, event, &o};
    s = NULL;
    CHECK(mo_service_open(image, 0, &hooks, store, &s) == MO_SERVICE_SAVED_INVALID && s == NULL);
    CHECK(named(o.handler, "tick"));
    mo_store_close(store);
    MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
    s = open_service(image, store, &o);
    mo_service_free(s);
    saved.bytes.bytes[0] = 2;
    MO_ASSERT(mo_store_call(store, MO_STORE_APPEND, &record, NULL, NULL) == MO_STORE_OK);
    s = NULL;
    CHECK(mo_service_open(image, 0, &hooks, store, &s) == MO_SERVICE_SAVED_INVALID && s == NULL);
    CHECK(o.handler.bytes == NULL);
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
        check_encoding(s, image);
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
        struct mo_store_record record = {MO_STORE_LOG, 1, mo_image_key(image, 0), saved.bytes.bytes,
                                         saved.bytes.length};
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
    check_encoding(s, image);
    add(s, 0);
    MO_ASSERT(mo_store_call(store, MO_STORE_LOAD, NULL, copy_record, &saved) == MO_STORE_OK);
    mo_service_free(s);
    mo_store_close(store);
    for (unsigned extra = 0; extra < 2; extra++) {
        MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
        s = open_service(image, store, &o);
        mo_service_free(s);
        for (uint64_t seq = 1; seq <= MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES + extra; seq++) {
            struct mo_store_record record = {MO_STORE_LOG, seq, mo_image_key(image, 0),
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
    check_encoding(s, image);
    CHECK(mo_service_step(s));
    struct mo_cell text;
    MO_ASSERT(mo_value_text(mo_service_heap(s), "alpha", 5, &text));
    CHECK(mo_service_send(s, 0, &text, 1, 0) == MO_SERVICE_OK);
    o.time.now = 999;
    CHECK(mo_service_step(s));
    CHECK(mo_cell_get_time(mo_value_field(mo_cell_get_payload(mo_service_state(s)), 1)) == 123);
    check_encoding(s, image);
    mo_service_free(s);
    s = open_service(image, store, &o);
    check_encoding(s, image);
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
        check_encoding(s, image);
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
    check_encoding(s, image);
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

/* 2026-01-01T00:00:00Z, the start time every Simulation test here uses. */
static const int64_t simulation_epoch = INT64_C(1767225600000000);
enum { SECOND_US = 1000000, MINUTE_US = 60 * SECOND_US };

struct api_failures {
    bool keep_going;
    unsigned count;
    uint32_t function;
    enum mo_service_status status;
    bool boom; /* the defect was stop "boom" */
};

static bool api_failure(const struct mo_simulation_failure *f, void *context) {
    struct api_failures *a = context;
    a->count++;
    a->function = f->function;
    a->status = f->status;
    size_t length = 0;
    const uint8_t *detail = f->defect != NULL && f->defect->has_detail
                                ? mo_value_data(f->defect->detail, &length)
                                : NULL;
    a->boom = f->defect != NULL && strcmp(f->defect->reason, "stop") == 0 && length == 4 &&
              memcmp(detail, "boom", 4) == 0;
    return a->keep_going;
}

/* Whether the simulation's live state encodes as Int n does. */
static bool api_state_is(struct mo_simulation *s, struct mo_heap *heap, uint32_t type, int64_t n) {
    struct mo_buffer live, expected;
    mo_buffer_init(&live, MO_LIMIT_SAVED_BYTES);
    mo_buffer_init(&expected, MO_LIMIT_SAVED_BYTES);
    bool same =
        mo_simulation_encode(s, &live) && mo_value_encode(heap, type, mo_cell_int(n), &expected) &&
        live.length == expected.length && memcmp(live.bytes, expected.bytes, live.length) == 0;
    mo_buffer_free(&expected);
    mo_buffer_free(&live);
    return same;
}

static const char api_service[] =
    "module Main\nservice\nstate Int = 0\ntools add, read\nevery 1.minute tick\n"
    "token from env \"TOKEN\"\nend\n"
    "fn add(state: Int, _now: Time, n: Int) : (Int, Int)\n(state + n, state + n)\nend\n"
    "fn read(state: Int, _now: Time) : List(Int)\n[state]\nend\n"
    "fn tick(state: Int, _now: Time) : Int\nstate + 100\nend\n";

/* One sequence, run by Simulation in Mo and through the C API. Each step's reply is the counter
 * after it; the timer adds 100 at every startup and every minute. */
static const struct api_step {
    char kind; /* add, advance (seconds), crash, down (add while crashed), restart, query */
    int64_t value, reply;
} api_steps[] = {{'a', 1, 101}, {'v', 90, 0}, {'a', 2, 203}, {'c', 0, 0},
                 {'d', 5, 0},   {'r', 0, 0},  {'q', 0, 303}, {'v', 30, 0},
                 {'a', 3, 306}, {'c', 0, 0},  {'r', 0, 0},   {'q', 0, 406}};

static void simulation_api_steps(void) {
    char source[4096];
    int used = snprintf(source, sizeof source,
                        "%stest \"steps\"\nsim = Simulation.start(Main, "
                        "Time.parse(\"2026-01-01T00:00:00Z\") or stop \"time\")\n",
                        api_service);
    for (size_t i = 0; i < sizeof api_steps / sizeof api_steps[0]; i++) {
        const struct api_step *step = &api_steps[i];
        size_t room = sizeof source - (size_t)used;
        int n = 0;
        switch (step->kind) {
        case 'a':
            n = snprintf(source + used, room,
                         "assert Simulation.send(sim, Main.add, %" PRId64 ") == Ok((%" PRId64
                         ", %" PRId64 "))\n",
                         step->value, step->reply, step->reply);
            break;
        case 'v':
            n = snprintf(source + used, room, "Simulation.advance(sim, %" PRId64 ".seconds)\n",
                         step->value);
            break;
        case 'c':
            n = snprintf(source + used, room, "Simulation.crash(sim)\n");
            break;
        case 'd':
            n = snprintf(source + used, room,
                         "assert Simulation.send(sim, Main.add, %" PRId64
                         ") == Error(ServiceDown)\n",
                         step->value);
            break;
        case 'r':
            n = snprintf(source + used, room, "Simulation.restart(sim)\n");
            break;
        case 'q':
            n = snprintf(source + used, room,
                         "assert Simulation.send(sim, Main.read) == Ok([%" PRId64 "])\n",
                         step->reply);
            break;
        default:
            MO_UNREACHABLE();
        }
        MO_ASSERT(n > 0 && (size_t)n < room);
        used += n;
    }
    int tail = snprintf(source + used, sizeof source - (size_t)used, "end\n");
    MO_ASSERT(tail == 4);
    struct mo_image *image = compile(source);
    simulation_run(image, NULL, 0);

    uint32_t type = mo_image_bytecode(image)->services[0].state_type;
    struct mo_limits limits = mo_limits_default();
    struct mo_process *p = mo_process_new(image, &limits);
    MO_ASSERT(p != NULL);
    struct mo_heap *heap = mo_process_heap(p);
    struct api_failures failures = {0};
    const struct mo_simulation_hooks hooks = {api_failure, &failures};
    struct mo_simulation *s = NULL;
    CHECK(mo_simulation_new(image, 0, simulation_epoch, &s) == MO_STORE_OK);
    CHECK(mo_simulation_open(s, &hooks) == MO_SIMULATION_OK);
    CHECK(api_state_is(s, heap, type, 0));
    CHECK(mo_simulation_drain(s, &hooks) == MO_SIMULATION_OK);
    CHECK(api_state_is(s, heap, type, 100));
    CHECK(mo_simulation_open(s, &hooks) == MO_SIMULATION_RUNNING);
    int64_t counter = 100;
    unsigned restarts = 0;
    for (size_t i = 0; i < sizeof api_steps / sizeof api_steps[0]; i++) {
        const struct api_step *step = &api_steps[i];
        struct mo_simulation_reply r;
        struct mo_cell n = mo_cell_int(step->value), element;
        uint64_t firings;
        switch (step->kind) {
        case 'a':
            CHECK(mo_simulation_send(s, &hooks, heap, 0, &n, &r) == MO_SENT_REPLIED);
            CHECK(r.status == MO_SERVICE_OK && r.updated);
            CHECK(mo_cell_get_int(mo_value_field(r.reply, 0)) == step->reply);
            CHECK(mo_cell_get_int(mo_value_field(r.reply, 1)) == step->reply);
            mo_value_drop(heap, r.type, r.reply);
            counter = step->reply;
            break;
        case 'v':
            CHECK(mo_simulation_advance(s, &hooks, step->value * SECOND_US, &firings) ==
                  MO_SIMULATION_OK);
            counter += 100 * (int64_t)firings;
            break;
        case 'c':
            CHECK(api_state_is(s, heap, type, counter));
            mo_simulation_crash(s);
            CHECK(!mo_simulation_running(s));
            break;
        case 'd':
            CHECK(mo_simulation_send(s, &hooks, heap, 0, &n, &r) == MO_SENT_DOWN);
            break;
        case 'r':
            CHECK(mo_simulation_open(s, &hooks) == MO_SIMULATION_OK);
            /* Recovery restores the state the crash left, and the startup tick waits. */
            CHECK(api_state_is(s, heap, type, counter));
            CHECK(mo_simulation_drain(s, &hooks) == MO_SIMULATION_OK);
            counter += 100;
            CHECK(api_state_is(s, heap, type, counter));
            restarts++;
            break;
        case 'q':
            CHECK(mo_simulation_send(s, &hooks, heap, 1, NULL, &r) == MO_SENT_REPLIED);
            CHECK(r.status == MO_SERVICE_OK && !r.updated);
            CHECK(mo_list_size(r.reply) == 1 && mo_list_get(r.reply, 0, &element) &&
                  mo_cell_get_int(element) == step->reply);
            mo_value_drop(heap, r.type, r.reply);
            CHECK(counter == step->reply);
            break;
        default:
            MO_UNREACHABLE();
        }
    }
    CHECK(restarts == 2 && failures.count == 0);
    mo_simulation_free(s);
    mo_process_free(p);
    mo_image_free(image);
}

static void simulation_api_recovery(void) {
    struct mo_image *image = compile(api_service);
    uint32_t type = mo_image_bytecode(image)->services[0].state_type;
    struct mo_limits limits = mo_limits_default();
    struct mo_process *p = mo_process_new(image, &limits);
    MO_ASSERT(p != NULL);
    struct mo_heap *heap = mo_process_heap(p);
    struct api_failures failures = {0};
    const struct mo_simulation_hooks hooks = {api_failure, &failures};
    struct mo_simulation *s = NULL;
    CHECK(mo_simulation_new(image, 0, simulation_epoch, &s) == MO_STORE_OK);
    CHECK(mo_simulation_open(s, &hooks) == MO_SIMULATION_OK);
    CHECK(mo_simulation_drain(s, &hooks) == MO_SIMULATION_OK);
    for (int64_t i = 1; i <= 3; i++) {
        struct mo_cell n = mo_cell_int(i);
        struct mo_simulation_reply r;
        CHECK(mo_simulation_send(s, &hooks, heap, 0, &n, &r) == MO_SENT_REPLIED && r.updated);
        mo_value_drop(heap, r.type, r.reply);
    }
    struct mo_buffer third, recovered;
    mo_buffer_init(&third, MO_LIMIT_SAVED_BYTES);
    mo_buffer_init(&recovered, MO_LIMIT_SAVED_BYTES);
    CHECK(mo_simulation_encode(s, &third));
    mo_simulation_crash(s);
    CHECK(!mo_simulation_encode(s, &recovered) && recovered.length == 0);
    CHECK(mo_simulation_open(s, &hooks) == MO_SIMULATION_OK);
    CHECK(mo_simulation_encode(s, &recovered));
    CHECK(recovered.length == third.length &&
          memcmp(recovered.bytes, third.bytes, third.length) == 0);
    CHECK(api_state_is(s, heap, type, 106));
    CHECK(mo_simulation_drain(s, &hooks) == MO_SIMULATION_OK);
    CHECK(api_state_is(s, heap, type, 206));
    mo_buffer_free(&recovered);
    mo_buffer_free(&third);

    /* The firing count is the precount advance refuses on, before any handler runs. */
    uint64_t firings = 0;
    CHECK(mo_simulation_firings(s, 10000 * (int64_t)MINUTE_US, &firings) && firings == 10000);
    CHECK(mo_simulation_firings(s, 10001 * (int64_t)MINUTE_US, &firings) && firings == 10001);
    CHECK(!mo_simulation_firings(s, -1, &firings));
    firings = 0;
    CHECK(mo_simulation_advance(s, &hooks, 10001 * (int64_t)MINUTE_US, &firings) ==
          MO_SIMULATION_TICKS);
    CHECK(firings == 10001);
    CHECK(api_state_is(s, heap, type, 206));
    CHECK(mo_simulation_firings(s, 10001 * (int64_t)MINUTE_US, &firings) && firings == 10001);
    CHECK(mo_simulation_advance(s, &hooks, -1, &firings) == MO_SIMULATION_DURATION);
    CHECK(mo_simulation_advance(s, &hooks, MINUTE_US, &firings) == MO_SIMULATION_OK);
    CHECK(firings == 1 && api_state_is(s, heap, type, 306));
    CHECK(failures.count == 0);
    mo_simulation_free(s);
    mo_process_free(p);
    mo_image_free(image);
}

static void simulation_api_failures(void) {
    struct mo_image *image = compile(
        "module Main\nservice\nstate Int = 0\ntools read\nevery 1.minute tick\n"
        "token from env \"TOKEN\"\nend\n"
        "fn tick(state: Int, _now: Time) : Int\nif state == 0: stop \"boom\" else: state\nend\n"
        "fn read(state: Int, _now: Time) : List(Int)\n[state]\nend\n"
        "test \"startup failure\"\n"
        "_sim = Simulation.start(Main, Time.parse(\"2026-01-01T00:00:00Z\") or stop \"time\")\n"
        "end\n");
    simulation_run(image,
                   "Simulation handler Main.tick at 2026-01-01T00:00:00Z: service handler failed: "
                   "stop: boom",
                   0);
    const struct mo_bytecode_service *service = &mo_image_bytecode(image)->services[0];
    struct mo_limits limits = mo_limits_default();
    struct mo_process *p = mo_process_new(image, &limits);
    MO_ASSERT(p != NULL);
    struct mo_heap *heap = mo_process_heap(p);
    for (unsigned keep_going = 0; keep_going < 2; keep_going++) {
        struct api_failures failures = {.keep_going = keep_going != 0};
        const struct mo_simulation_hooks hooks = {api_failure, &failures};
        struct mo_simulation *s = NULL;
        CHECK(mo_simulation_new(image, 0, simulation_epoch, &s) == MO_STORE_OK);
        CHECK(mo_simulation_open(s, &hooks) == MO_SIMULATION_OK && failures.count == 0);
        CHECK(mo_simulation_drain(s, &hooks) ==
              (keep_going ? MO_SIMULATION_OK : MO_SIMULATION_STOPPED));
        CHECK(failures.count == 1 && failures.status == MO_SERVICE_DEFECT && failures.boom);
        CHECK(failures.function == service->timers[0].function);
        CHECK(mo_simulation_running(s) && api_state_is(s, heap, service->state_type, 0));
        if (keep_going) {
            /* A failed tick is one step; the next deadline runs the timer again. */
            uint64_t firings;
            CHECK(mo_simulation_advance(s, &hooks, MINUTE_US, &firings) == MO_SIMULATION_OK);
            CHECK(firings == 1 && failures.count == 2);
        }
        mo_simulation_free(s);
    }
    mo_process_free(p);
    mo_image_free(image);

    image = compile("module Main\nservice\nstate Int = initial()\ntools read\n"
                    "token from env \"TOKEN\"\nend\n"
                    "fn initial() : Int\nstop \"boom\"\nend\n"
                    "fn read(state: Int, _now: Time) : List(Int)\n[state]\nend\n");
    struct api_failures failures = {.keep_going = true};
    const struct mo_simulation_hooks hooks = {api_failure, &failures};
    struct mo_simulation *s = NULL;
    CHECK(mo_simulation_new(image, 0, simulation_epoch, &s) == MO_STORE_OK);
    CHECK(mo_simulation_open(s, &hooks) == MO_SIMULATION_DOWN);
    CHECK(failures.count == 1 && failures.status == MO_SERVICE_DEFECT && failures.boom);
    CHECK(failures.function == mo_image_bytecode(image)->services[0].start);
    struct mo_buffer state;
    mo_buffer_init(&state, MO_LIMIT_SAVED_BYTES);
    CHECK(!mo_simulation_running(s) && !mo_simulation_encode(s, &state));
    mo_buffer_free(&state);
    mo_simulation_free(s);
    mo_image_free(image);
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

struct captured {
    unsigned count;
    struct mo_heap *heap;
    uint32_t state_type, argument_type;
    uint64_t program;
    int64_t arrival;
};
static bool captured_runtime(const struct mo_store_failure *f, void *context) {
    struct captured *c = context;
    struct mo_failure_record r;
    CHECK(mo_failure_read(f->message.bytes, f->message.length, &r));
    if (!mo_failure_read(f->message.bytes, f->message.length, &r))
        return false;
    CHECK(r.id == f->id && r.sequence == 0 && r.read_only);
    CHECK(r.argument_count == 1 && r.class == 0 && r.signature_version == 1);
    CHECK(r.name.length == strlen("broken") && memcmp(r.name.bytes, "broken", r.name.length) == 0);
    int64_t arrival;
    memcpy(&arrival, &r.arrival, sizeof arrival);
    CHECK(arrival == c->arrival && !r.timer && r.program == c->program);
    uint8_t build[32];
    CHECK(mo_runtime_identity(build) && memcmp(r.build, build, 32) == 0);
    const char *label = mo_runtime_build_label();
    CHECK(r.label.length == strlen(label) && memcmp(r.label.bytes, label, r.label.length) == 0);
    struct mo_cell state;
    CHECK(mo_value_decode(c->heap, c->state_type, f->state.bytes, f->state.length, &state));
    CHECK(mo_cell_get_int(state) == 7);
    struct mo_binary in = {r.arguments.bytes, r.arguments.length, 0};
    uint64_t length = 0;
    const uint8_t *argument = NULL;
    CHECK(mo_binary_uint(&in, 4, &length) && mo_binary_read(&in, (size_t)length, &argument) &&
          in.position == r.arguments.length);
    struct mo_cell value = mo_cell_int(-1);
    CHECK(argument != NULL &&
          mo_value_decode(c->heap, c->argument_type, argument, (size_t)length, &value));
    CHECK(mo_cell_get_int(value) == 0);
    struct mo_buffer context_text;
    mo_buffer_init(&context_text, MO_LIMIT_FAILURE_CONTEXT_BYTES + 1);
    CHECK(mo_buffer_append(&context_text, r.context.bytes, r.context.length));
    CHECK(mo_buffer_append_byte(&context_text, 0));
    CHECK(strstr((const char *)context_text.bytes, "frame ") != NULL);
    CHECK(strstr((const char *)context_text.bytes, " span ") != NULL);
    CHECK(strstr((const char *)context_text.bytes, "variable _n available: 0") != NULL);
    mo_buffer_free(&context_text);
    c->count++;
    return true;
}
static void runtime_capture(void) {
    mo_runtime_identity_init();
    struct mo_image *image =
        compile("module Main\nservice\nstate Int = 7\ntools broken\n"
                "token from env \"TOKEN\"\nend\n"
                "fn broken(_state: Int, _now: Time, _n: Int) : Bool\nstop \"broken\"\nend\n");
    struct mo_store *store = NULL;
    CHECK(mo_store_open(NULL, &store) == MO_STORE_OK);
    struct observations o = {.time = {.now = INT64_C(1767225600000000), .elapsed = 5}};
    struct mo_service_loop *service = open_service(image, store, &o);
    struct mo_cell zero = mo_cell_int(0);
    CHECK(mo_service_send(service, 0, &zero, 1, 1) == MO_SERVICE_OK);
    o.time.now += 9;
    CHECK(mo_service_step(service));
    CHECK(o.failures == 1 && o.sequence == 0);
    CHECK(mo_cell_get_int(mo_service_state(service)) == 7);
    const struct mo_bytecode_service *descriptor = &mo_image_bytecode(image)->services[0];
    struct captured c = {.heap = mo_service_heap(service),
                         .state_type = descriptor->state_type,
                         .argument_type = descriptor->tools[0].argument_types[0],
                         .program = mo_image_key(image, 0),
                         .arrival = INT64_C(1767225600000000)};
    CHECK(mo_service_failures(service, captured_runtime, &c) == MO_STORE_OK);
    CHECK(c.count == 1);
    const uint8_t *saved = NULL;
    size_t saved_length = 0;
    CHECK(mo_service_encoded_state(service, &saved, &saved_length));
    struct mo_store_record snapshot = {MO_STORE_STATE, 0, mo_image_key(image, 0), saved,
                                       saved_length};
    CHECK(mo_store_call(store, MO_STORE_SNAPSHOT, &snapshot, NULL, NULL) == MO_STORE_OK);
    CHECK(mo_service_failures(service, captured_runtime, &c) == MO_STORE_OK);
    CHECK(c.count == 2);
    struct mo_export_identity pinned = {.kind = 2, .label = "test", .id = {1}};
    CHECK(mo_runtime_identity(pinned.build));
    struct mo_export_pin live = {0};
    CHECK(mo_service_pin_failure(service, 1, &live, &pinned, 0));
    CHECK(live.bytes != NULL && live.failure_id == 1 && live.id[0] == 1);
    pinned.id[0] = 2;
    CHECK(!mo_service_pin_failure(service, 99, &live, &pinned, 0));
    CHECK(live.bytes != NULL && live.failure_id == 1 && live.id[0] == 1);
    mo_export_release(&live);
    struct mo_buffer body;
    mo_buffer_init(&body, MO_LIMIT_FAILURE_RECORD_BYTES);
    CHECK(mo_service_failure_export(service, 1, &body));
    struct mo_export_identity id = {
        .kind = 2, .label = "test", .sequence = 0, .program = mo_image_key(image, 0)};
    CHECK(mo_runtime_identity(id.build));
    struct mo_export_pin pin = {0};
    CHECK(mo_export_create(&pin, &id, body.bytes, body.length, 0));
    if (pin.bytes != NULL) {
        struct mo_export_identity parsed;
        CHECK(mo_export_verify(pin.bytes, pin.length, &parsed));
        CHECK(parsed.kind == 2);
    }
    mo_export_release(&pin);
    mo_buffer_free(&body);
    CHECK(mo_service_send(service, 0, &zero, 1, 2) == MO_SERVICE_OK);
    refuse_cache_copy = true;
    CHECK(mo_service_step(service));
    refuse_cache_copy = false;
    struct mo_service_event_summary refused;
    CHECK(mo_service_event_entry(service, 1, &refused));
    CHECK(refused.record_id == 0 && refused.record_unavailable != NULL &&
          strcmp(refused.record_unavailable, "out of memory for the record") == 0);
    CHECK(o.failures == 2 && mo_service_running(service));
    mo_service_free(service);
    mo_store_close(store);
    mo_image_free(image);
}

/* The newest retained failure, copied out of the store. */
struct retained {
    struct mo_buffer state, message;
    struct mo_failure_record record;
};
static bool keep_newest(const struct mo_store_failure *f, void *context) {
    struct retained *r = context;
    mo_buffer_clear(&r->state);
    mo_buffer_clear(&r->message);
    MO_ASSERT(mo_buffer_append(&r->state, f->state.bytes, f->state.length) &&
              mo_buffer_append(&r->message, f->message.bytes, f->message.length));
    return true;
}
static void retained_failure(struct mo_service_loop *s, struct retained *r) {
    mo_buffer_init(&r->state, MO_LIMIT_SAVED_BYTES);
    mo_buffer_init(&r->message, MO_LIMIT_FAILURE_RECORD_BYTES);
    MO_ASSERT(mo_service_failures(s, keep_newest, r) == MO_STORE_OK && r->message.length != 0);
    MO_ASSERT(mo_failure_read(r->message.bytes, r->message.length, &r->record));
}
static void retained_free(struct retained *r) {
    mo_buffer_free(&r->state);
    mo_buffer_free(&r->message);
}
static struct mo_offline_run replay_retained(struct mo_image *image, const struct retained *r,
                                             struct mo_buffer *context) {
    mo_buffer_init(context, MO_LIMIT_FAILURE_CONTEXT_BYTES);
    return mo_offline_call(image, &mo_image_bytecode(image)->services[0], r->record.handler,
                           r->state.bytes, r->state.length, &r->record, context);
}

static bool same_slice(struct mo_failure_slice a, const char *text) {
    return a.length == strlen(text) && memcmp(a.bytes, text, a.length) == 0;
}
static bool same_slices(struct mo_failure_slice a, struct mo_failure_slice b) {
    return a.length == b.length && (a.length == 0 || memcmp(a.bytes, b.bytes, a.length) == 0);
}
/* The offline run computed the stored signature field by field and the stored context text. */
static void check_reproduced(const struct mo_failure_record *r, const struct mo_offline_run *run,
                             const struct mo_buffer *context) {
    struct mo_signature stored = mo_signature_from_record(r);
    const struct mo_signature *offline = &run->outcome.signature;
    CHECK(offline->class == stored.class && same_slices(offline->reason, stored.reason) &&
          same_slices(offline->limit, stored.limit) && offline->limit_value == stored.limit_value);
    CHECK(offline->function == stored.function && offline->file == stored.file &&
          offline->start == stored.start && offline->end == stored.end);
    CHECK(run->context == (enum mo_failure_context)r->context_status);
    CHECK(context->length == r->context.length &&
          memcmp(context->bytes, r->context.bytes, context->length) == 0);
    CHECK(mo_replay_classify(&stored, &run->outcome) == MO_REPLAY_REPRODUCED);
}

/* A `stop` in a read-only and in an updating tool replays to the same defect, with the context
 * capture rendered, and leaves the state as it was; the same stop one line lower is a different
 * failure. */
static void offline_stop(void) {
    const char *source =
        "module Main\nservice\nstate Int = 7\ntools broken, update\n"
        "token from env \"TOKEN\"\nend\n"
        "fn broken(_state: Int, _now: Time, _n: Int) : Bool\nstop \"broken\"\nend\n"
        "fn update(state: Int, _now: Time, n: Int) : (Int, Int)\n"
        "after = state + n\n(after, stop \"update\")\nend\n";
    const char *moved =
        "module Main\nservice\nstate Int = 7\ntools broken, update\n"
        "token from env \"TOKEN\"\nend\n"
        "fn broken(_state: Int, _now: Time, _n: Int) : Bool\n\nstop \"broken\"\nend\n"
        "fn update(state: Int, _now: Time, n: Int) : (Int, Int)\n"
        "after = state + n\n\n(after, stop \"update\")\nend\n";
    struct mo_image *image = compile(source), *other = compile(moved);
    const struct mo_bytecode_service *service = &mo_image_bytecode(image)->services[0];
    struct mo_store *store = NULL;
    CHECK(mo_store_open(NULL, &store) == MO_STORE_OK);
    struct observations o = {.time = {.now = INT64_C(1767225600000000), .elapsed = 5}};
    struct mo_service_loop *s = open_service(image, store, &o);
    for (uint32_t tool = 0; tool < 2; tool++) {
        struct mo_cell n = mo_cell_int(5);
        CHECK(mo_service_send(s, tool, &n, 1, tool + 1) == MO_SERVICE_OK && mo_service_step(s));
        CHECK(o.failures == tool + 1 && o.status == MO_SERVICE_DEFECT);
        CHECK(o.sequence == 0 && mo_cell_get_int(mo_service_state(s)) == 7);
        struct retained r;
        retained_failure(s, &r);
        CHECK(r.record.handler == tool && r.record.read_only == (tool == 0) && !r.record.timer);
        struct mo_buffer context;
        struct mo_offline_run run = replay_retained(image, &r, &context);
        CHECK(run.outcome.status == MO_OUTCOME_DEFECT && run.refusal == NULL);
        CHECK(run.outcome.signature.class == MO_SIGNATURE_DEFECT &&
              same_slice(run.outcome.signature.reason, "stop"));
        CHECK(run.outcome.signature.function == service->tools[tool].function &&
              run.outcome.signature.file != MO_NO_INDEX);
        CHECK(!run.outcome.state_changed);
        check_reproduced(&r.record, &run, &context);
        /* The record carries this build's budgets; one that differs is named with both values. */
        struct mo_service_budget differs = {0};
        CHECK(mo_service_budgets_match(&r.record, &differs) && differs.name == NULL);
        struct mo_failure_record crafted = r.record;
        crafted.heap++;
        CHECK(!mo_service_budgets_match(&crafted, &differs) && strcmp(differs.name, "heap") == 0);
        CHECK(differs.recorded == MO_LIMIT_SERVICE_MESSAGE_MEMORY - MO_DEFAULT_STACK_SIZE + 1 &&
              differs.expected == MO_LIMIT_SERVICE_MESSAGE_MEMORY - MO_DEFAULT_STACK_SIZE);
        crafted = r.record;
        crafted.saved--;
        CHECK(!mo_service_budgets_match(&crafted, &differs) &&
              strcmp(differs.name, "saved bytes") == 0 && differs.expected == MO_LIMIT_SAVED_BYTES);
        mo_buffer_free(&context);
        struct mo_offline_run elsewhere = replay_retained(other, &r, &context);
        CHECK(elsewhere.outcome.status == MO_OUTCOME_DEFECT);
        struct mo_signature stored = mo_signature_from_record(&r.record);
        CHECK(elsewhere.outcome.signature.start != stored.start);
        CHECK(mo_replay_classify(&stored, &elsewhere.outcome) == MO_REPLAY_DIFFERENT);
        mo_buffer_free(&context);
        retained_free(&r);
    }
    struct inventory i = {0};
    CHECK(mo_store_call(store, MO_STORE_LOAD, NULL, count_record, &i) == MO_STORE_OK);
    CHECK(i.logs == 0 && i.last == 0);
    mo_service_free(s);
    mo_store_close(store);
    mo_image_free(other);
    mo_image_free(image);
}

static bool scratch_fits(struct mo_heap *heap, size_t bytes) {
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, heap);
    bool fits = mo_scratch_resize(&scratch, NULL, 0, bytes) != NULL;
    mo_scratch_free(&scratch);
    return fits;
}
static size_t heap_room(struct mo_heap *heap) {
    size_t low = 0, high = MO_LIMIT_SERVICE_MESSAGE_MEMORY;
    while (high - low > 1) {
        size_t middle = low + (high - low) / 2;
        if (scratch_fits(heap, middle))
            low = middle;
        else
            high = middle;
    }
    return low;
}
/* A handler that ran out of heap only because the mailbox held a queued message's arguments
 * succeeds offline, where nothing else is live: its failure is not reproduced. */
static void offline_memory(void) {
    struct mo_image *image = compile(
        "module Main\nservice\nstate List(Int) = [3, 8]\ntools memory, echo\n"
        "token from env \"TOKEN\"\nend\n"
        "fn memory(_state: List(Int), _now: Time, n: Int) : Int\n\"x\".repeat(n).byte_count\nend\n"
        "fn echo(_state: List(Int), _now: Time, text: Text) : Text\ntext\nend\n");
    struct mo_store *store = NULL;
    CHECK(mo_store_open(NULL, &store) == MO_STORE_OK);
    struct observations o = {0};
    struct mo_service_loop *s = open_service(image, store, &o);
    struct mo_heap *heap = mo_service_heap(s);
    size_t used = mo_heap_used(heap), available = heap_room(heap);
    struct mo_cell n = mo_cell_int(1024), text;
    MO_ASSERT(mo_value_text(heap, "retained", 8, &text));
    CHECK(mo_service_send(s, 0, &n, 1, 1) == MO_SERVICE_OK);
    CHECK(mo_service_send(s, 1, &text, 1, 2) == MO_SERVICE_OK);
    mo_value_drop(heap, mo_image_bytecode(image)->services[0].tools[1].argument_types[0], text);
    size_t queued = mo_heap_used(heap) - used;
    CHECK(queued != 0 && scratch_fits(heap, available - queued));
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, heap);
    MO_ASSERT(mo_scratch_resize(&scratch, NULL, 0, available - queued) != NULL);
    CHECK(mo_service_step(s) && o.status == MO_SERVICE_MEMORY && o.failures == 1);
    mo_scratch_free(&scratch);
    CHECK(mo_service_step(s) && o.status == MO_SERVICE_OK);
    struct retained r;
    retained_failure(s, &r);
    CHECK(r.record.class == MO_SIGNATURE_MEMORY && r.record.handler == 0);
    struct mo_buffer context;
    struct mo_offline_run run = replay_retained(image, &r, &context);
    CHECK(run.outcome.status == MO_OUTCOME_SUCCEEDED && run.refusal == NULL);
    CHECK(run.outcome.reply == MO_REPLY_OK && !run.outcome.state_changed && context.length == 0);
    struct mo_signature stored = mo_signature_from_record(&r.record);
    CHECK(mo_replay_classify(&stored, &run.outcome) == MO_REPLAY_NOT_REPRODUCED);
    mo_buffer_free(&context);
    retained_free(&r);
    mo_service_free(s);
    mo_store_close(store);
    mo_image_free(image);
}

/* An updating tool's and a timer's new state over the saved-bytes limit fails the size check
 * offline as it did live. A revision whose new state fits succeeds and reports the change; one
 * that returns an Error succeeds with an error reply and no change. */
static void offline_state_size(void) {
    char sources[2][512], fixed[2][512], rejected[512];
    MO_ASSERT(snprintf(sources[0], sizeof sources[0],
                       "module Main\nservice\nstate Text = \"\"\ntools grow\n"
                       "token from env \"TOKEN\"\nend\n"
                       "fn grow(_state: Text, _now: Time) : (Text, Int)\n"
                       "(\"a\".repeat(%d), 0)\nend\n",
                       MO_LIMIT_SAVED_BYTES) > 0);
    MO_ASSERT(snprintf(sources[1], sizeof sources[1],
                       "module Main\nservice\nstate Text = \"\"\ntools read\nevery 1.second grow\n"
                       "token from env \"TOKEN\"\nend\n"
                       "fn read(_state: Text, _now: Time) : Bool\ntrue\nend\n"
                       "fn grow(_state: Text, _now: Time) : Text\n\"a\".repeat(%d)\nend\n",
                       MO_LIMIT_SAVED_BYTES) > 0);
    MO_ASSERT(snprintf(fixed[0], sizeof fixed[0],
                       "module Main\nservice\nstate Text = \"\"\ntools grow\n"
                       "token from env \"TOKEN\"\nend\n"
                       "fn grow(_state: Text, _now: Time) : (Text, Int)\n(\"a\", 0)\nend\n") > 0);
    MO_ASSERT(snprintf(fixed[1], sizeof fixed[1],
                       "module Main\nservice\nstate Text = \"\"\ntools read\nevery 1.second grow\n"
                       "token from env \"TOKEN\"\nend\n"
                       "fn read(_state: Text, _now: Time) : Bool\ntrue\nend\n"
                       "fn grow(_state: Text, _now: Time) : Text\n\"a\"\nend\n") > 0);
    MO_ASSERT(snprintf(rejected, sizeof rejected,
                       "module Main\nservice\nstate Text = \"\"\ntools grow\n"
                       "token from env \"TOKEN\"\nend\n"
                       "fn grow(_state: Text, _now: Time) : Result((Text, Int), Text)\n"
                       "Error(\"no\")\nend\n") > 0);
    for (unsigned timer = 0; timer < 2; timer++) {
        struct mo_image *image = compile(sources[timer]);
        struct mo_store *store = NULL;
        CHECK(mo_store_open(NULL, &store) == MO_STORE_OK);
        struct observations o = {0};
        struct mo_service_loop *s = open_service(image, store, &o);
        if (!timer)
            CHECK(mo_service_send(s, 0, NULL, 0, 1) == MO_SERVICE_OK);
        CHECK(mo_service_step(s) && o.failures == 1 && o.status == MO_SERVICE_MEMORY);
        size_t length = 1;
        CHECK(mo_value_data(mo_service_state(s), &length) != NULL || length == 0);
        CHECK(length == 0 && o.sequence == 0);
        struct retained r;
        retained_failure(s, &r);
        CHECK(r.record.class == MO_SIGNATURE_STATE_SIZE && r.record.timer == timer);
        struct mo_buffer context;
        struct mo_offline_run run = replay_retained(image, &r, &context);
        CHECK(run.outcome.status == MO_OUTCOME_STATE_SIZE && run.outcome.state_changed);
        CHECK(same_slice(run.outcome.signature.limit, "MO_LIMIT_SAVED_BYTES") &&
              run.outcome.signature.limit_value == MO_LIMIT_SAVED_BYTES);
        check_reproduced(&r.record, &run, &context);
        mo_buffer_free(&context);
        struct mo_image *revised = compile(fixed[timer]);
        run = replay_retained(revised, &r, &context);
        CHECK(run.outcome.status == MO_OUTCOME_SUCCEEDED && run.outcome.reply == MO_REPLY_OK);
        CHECK(run.outcome.state_changed && context.length == 0);
        mo_buffer_free(&context);
        mo_image_free(revised);
        if (!timer) {
            revised = compile(rejected);
            run = replay_retained(revised, &r, &context);
            CHECK(run.outcome.status == MO_OUTCOME_SUCCEEDED &&
                  run.outcome.reply == MO_REPLY_ERROR && !run.outcome.state_changed);
            mo_buffer_free(&context);
            mo_image_free(revised);
        }
        retained_free(&r);
        mo_service_free(s);
        mo_store_close(store);
        mo_image_free(image);
    }
}

struct capture_check {
    unsigned count;
    uint8_t class;
};
static bool capture_class(const struct mo_store_failure *f, void *context) {
    struct capture_check *c = context;
    struct mo_failure_record r;
    bool ok = mo_failure_read(f->message.bytes, f->message.length, &r);
    CHECK(ok);
    if (ok) {
        CHECK(r.class == c->class && r.work == MO_LIMIT_SERVICE_WORK);
        CHECK(r.context.length >= 6 && memcmp(r.context.bytes, "frame ", 6) == 0);
        if (r.class == 1)
            CHECK(r.limit_value == MO_LIMIT_SERVICE_WORK);
        if (r.class == 2)
            CHECK(r.limit_value == r.heap);
        CHECK(r.heap == MO_LIMIT_SERVICE_MESSAGE_MEMORY - MO_DEFAULT_STACK_SIZE);
        c->count++;
    }
    return ok;
}
static void runtime_store_faults(void) {
    struct mo_image *image =
        compile("module Main\nservice\nstate Int = 7\ntools broken\ntoken from env \"TOKEN\"\nend\n"
                "fn broken(_state: Int, _now: Time) : Bool\nstop \"fault\"\nend\n");
    const unsigned faults[] = {13, 14, 15, 21, 22, 23, 24, 25, 26, 27};
    for (size_t i = 0; i < sizeof faults / sizeof faults[0]; i++) {
        unsigned fault = faults[i];
        bool safe = fault == 13 || fault == 21 || fault == 22 || fault == 23 || fault == 26;
        struct mo_store *store = NULL;
        CHECK(mo_store_open(NULL, &store) == MO_STORE_OK);
        struct observations o = {0};
        struct mo_service_loop *service = open_service(image, store, &o);
        CHECK(mo_service_send(service, 0, NULL, 0, 1) == MO_SERVICE_OK);
        crash_point = fault;
        CHECK(mo_service_step(service));
        crash_point = 0;
        struct mo_service_observation observation = mo_service_observe(service);
        CHECK(o.failures == 1 && observation.failures == 1 && o.sequence == 0);
        CHECK(mo_service_running(service) == safe);
        CHECK(o.stops == (safe ? 0u : 1u));
        struct mo_service_event_summary event;
        CHECK(mo_service_event_entry(service, 0, &event));
        CHECK(event.record_id == 0 && event.record_unavailable != NULL &&
              strcmp(event.record_unavailable,
                     safe ? "the store refused the record" : "store outcome uncertain") == 0);
        mo_service_free(service);
        mo_store_close(store);
    }
    mo_image_free(image);
}

static void resource_capture(void) {
    const char *sources[] = {
        "module Main\nservice\nstate List(Int) = [7]\ntools spin\ntoken from env \"TOKEN\"\nend\n"
        "fn spin(state: List(Int), now: Time) : Int\nspin(state, now)\nend\n",
        "module Main\nservice\nstate List(Int) = [7]\ntools memory\ntoken from env \"TOKEN\"\nend\n"
        "fn memory(_state: List(Int), _now: Time) : Int\n\"x\".repeat(67108864).byte_count\nend\n",
        "module Main\nservice\nstate Int = 7\ntools read\nevery 1.second tick\ntoken from env "
        "\"TOKEN\"\nend\n"
        "fn read(_state: Int, _now: Time) : Bool\ntrue\nend\n"
        "fn tick(_state: Int, _now: Time) : Int\nstop \"timer\"\nend\n"};
    for (unsigned i = 0; i < 3; i++) {
        struct mo_image *image = compile(sources[i]);
        struct mo_store *store = NULL;
        CHECK(mo_store_open(NULL, &store) == MO_STORE_OK);
        struct observations o = {0};
        struct mo_service_loop *service = open_service(image, store, &o);
        if (i < 2)
            CHECK(mo_service_send(service, 0, NULL, 0, 1) == MO_SERVICE_OK);
        CHECK(mo_service_step(service));
        CHECK(o.failures == 1 && o.sequence == 0);
        struct capture_check check = {.class = i == 0 ? 1 : i == 1 ? 2 : 0};
        CHECK(mo_service_failures(service, capture_class, &check) == MO_STORE_OK);
        CHECK(check.count == 1 && mo_service_running(service));
        mo_service_free(service);
        mo_store_close(store);
        mo_image_free(image);
    }
}
static void argument_capture_limits(void) {
    for (unsigned readonly = 0; readonly < 2; readonly++) {
        const char *source =
            readonly
                ? "module Main\nservice\nstate Int = 7\ntools broken\ntoken from env "
                  "\"TOKEN\"\nend\n"
                  "fn broken(_state: Int, _now: Time, _text: Text) : Bool\nstop \"broken\"\nend\n"
                : "module Main\nservice\nstate Int = 7\ntools broken\ntoken from env "
                  "\"TOKEN\"\nend\n"
                  "fn broken(_state: Int, _now: Time, _text: Text) : (Int, Bool)\nstop "
                  "\"broken\"\nend\n";
        struct mo_image *image = compile(source);
        struct mo_store *store = NULL;
        CHECK(mo_store_open(NULL, &store) == MO_STORE_OK);
        struct observations o = {0};
        struct mo_service_loop *service = open_service(image, store, &o);
        struct mo_heap *heap = mo_service_heap(service);
        uint32_t type = mo_image_bytecode(image)->services[0].tools[0].argument_types[0];
        size_t length = MO_LIMIT_SAVED_BYTES - 25 - 4 - text_overhead(heap, type);
        char *bytes = calloc(length, 1);
        MO_ASSERT(bytes != NULL);
        memset(bytes, 'x', length);
        struct mo_cell argument;
        CHECK(mo_value_text(heap, bytes, length, &argument));
        free(bytes);
        CHECK(mo_service_send(service, 0, &argument, 1, 1) == MO_SERVICE_OK);
        mo_value_drop(heap, type, argument);
        CHECK(mo_service_step(service));
        CHECK(o.failures == 1 && o.sequence == 0 && mo_service_running(service));
        struct mo_service_event_summary event;
        CHECK(mo_service_event_entry(service, 0, &event));
        CHECK(event.record_id == 0 && event.record_unavailable != NULL);
        CHECK(strcmp(event.record_unavailable,
                     readonly ? "arguments could not be encoded" : "too large to record") == 0);
        struct capture_check check = {0};
        CHECK(mo_service_failures(service, capture_class, &check) == MO_STORE_OK &&
              check.count == 0);
        mo_service_free(service);
        mo_store_close(store);
        mo_image_free(image);
    }
}

struct text_capture {
    unsigned count, readable;
    const char *expected[2];
};
static bool text_capture_row(const struct mo_store_failure *f, void *context) {
    struct text_capture *t = context;
    t->count++;
    struct mo_failure_record r;
    bool readable = mo_failure_read(f->message.bytes, f->message.length, &r);
    CHECK(readable);
    if (!readable || f->id == 0 || f->id > 2)
        return true;
    t->readable++;
    char context_text[MO_LIMIT_FAILURE_CONTEXT_BYTES + 1];
    memcpy(context_text, r.context.bytes, r.context.length);
    context_text[r.context.length] = 0;
    CHECK(strstr(context_text, t->expected[f->id - 1]) != NULL);
    for (char *line = context_text; *line != 0;) {
        char *end = strchr(line, '\n');
        CHECK(end != NULL);
        if (end == NULL)
            break;
        *end = 0;
        CHECK(strncmp(line, "frame ", 6) == 0 || strncmp(line, "variable ", 9) == 0);
        CHECK(strcmp(line, "frame 99 file 0 span 0..0") != 0);
        line = end + 1;
    }
    return true;
}
/* Text values print as escaped literals, so they cannot add NUL or forge context lines. */
static void text_context(void) {
    struct mo_image *image =
        compile("module Main\nservice\nstate Int = 7\ntools broken\ntoken from env \"TOKEN\"\nend\n"
                "fn broken(_state: Int, _now: Time, _text: Text) : Bool\nstop \"broken\"\nend\n");
    struct mo_store *store = NULL;
    CHECK(mo_store_open(NULL, &store) == MO_STORE_OK);
    struct observations o = {0};
    struct mo_service_loop *service = open_service(image, store, &o);
    struct mo_heap *heap = mo_service_heap(service);
    uint32_t type = mo_image_bytecode(image)->services[0].tools[0].argument_types[0];
    const char forged[] = "x\nframe 99 file 0 span 0..0";
    const struct {
        const char *bytes;
        size_t length;
    } texts[] = {{"a\0b", 3}, {forged, sizeof forged - 1}};
    for (unsigned i = 0; i < 2; i++) {
        struct mo_cell argument;
        CHECK(mo_value_text(heap, texts[i].bytes, texts[i].length, &argument));
        CHECK(mo_service_send(service, 0, &argument, 1, i + 1) == MO_SERVICE_OK);
        mo_value_drop(heap, type, argument);
        CHECK(mo_service_step(service));
        struct mo_service_event_summary event;
        CHECK(mo_service_event_entry(service, i, &event));
        CHECK(event.record_id == i + 1 && event.record_unavailable == NULL);
    }
    CHECK(o.failures == 2 && mo_service_running(service));
    struct text_capture t = {.expected = {"variable _text available: \"a\\u{0}b\"\n",
                                          "variable _text available: \"x\\nframe 99 file 0 span "
                                          "0..0\"\n"}};
    CHECK(mo_service_failures(service, text_capture_row, &t) == MO_STORE_OK);
    CHECK(t.count == 2 && t.readable == 2);
    struct mo_export_identity identity = {.kind = 2, .label = "test", .id = {1}};
    CHECK(mo_runtime_identity(identity.build));
    struct mo_export_pin pin = {0};
    CHECK(mo_service_pin_failure(service, 1, &pin, &identity, 0));
    mo_export_release(&pin);
    mo_service_free(service);
    mo_store_close(store);
    mo_image_free(image);
}

static void tamper(const char *path, const char *sql) {
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK);
    CHECK(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_close(db) == SQLITE_OK);
}
static bool count_failures(const struct mo_store_failure *f, void *context) {
    (void)f;
    (*(unsigned *)context)++;
    return true;
}
/* Diagnostic rows that fail validation refuse new captures; they never close the store. */
static void damaged_service(const char *path, const char *sql) {
    struct mo_image *image = compile(
        "module Main\nservice\nstate Int = 7\ntools add, broken\ntoken from env \"TOKEN\"\nend\n"
        "fn add(state: Int, _now: Time, n: Int) : (Int, Int)\n(state + n, state + n)\nend\n"
        "fn broken(_state: Int, _now: Time) : Bool\nstop \"broken\"\nend\n");
    struct mo_store *store = NULL;
    CHECK(mo_store_open(path, &store) == MO_STORE_OK);
    struct observations o = {0};
    struct mo_service_loop *service = open_service(image, store, &o);
    CHECK(mo_service_send(service, 1, NULL, 0, 1) == MO_SERVICE_OK);
    CHECK(mo_service_step(service));
    mo_service_free(service);
    mo_store_close(store);
    tamper(path, sql);
    for (unsigned restart = 0; restart < 2; restart++) {
        CHECK(mo_store_open(path, &store) == MO_STORE_OK);
        o = (struct observations){0};
        service = open_service(image, store, &o);
        CHECK(mo_service_send(service, 1, NULL, 0, 2) == MO_SERVICE_OK);
        CHECK(mo_service_step(service));
        struct mo_service_event_summary event;
        CHECK(mo_service_event_entry(service, 0, &event));
        CHECK(event.record_id == 0 && event.record_unavailable != NULL &&
              strcmp(event.record_unavailable, "diagnostic records damaged") == 0);
        CHECK(mo_service_running(service) && o.stops == 0);
        add(service, 1);
        CHECK(o.status == MO_SERVICE_OK && o.sequence == restart + 1);
        mo_service_free(service);
        mo_store_close(store);
    }
    mo_image_free(image);
}
static void damaged_diagnostics(void) {
    const char *cases[] = {
        "DELETE FROM failure_state WHERE id=1",
        "UPDATE failure SET kind=substr(hex(zeroblob(1025)),1,1025) WHERE id=1",
        "UPDATE failure SET first=2 WHERE id=1",
        "WITH RECURSIVE n(i) AS (SELECT 2 UNION ALL SELECT i+1 FROM n WHERE i<65) "
        "INSERT INTO failure SELECT i,'k'||i,1,(SELECT message FROM failure WHERE id=1) FROM n;"
        "WITH RECURSIVE n(i) AS (SELECT 2 UNION ALL SELECT i+1 FROM n WHERE i<65) "
        "INSERT INTO failure_state SELECT i,(SELECT record FROM failure_state WHERE id=1) FROM n",
        "UPDATE failure SET kind=CAST(X'FF41' AS TEXT) WHERE id=1"};
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        char path[] = "/tmp/mo-damaged-diagnostics-XXXXXX";
        int fd = mkstemp(path);
        CHECK(fd >= 0 && close(fd) == 0 && unlink(path) == 0);
        struct mo_store *store = NULL;
        CHECK(mo_store_open(path, &store) == MO_STORE_OK);
        const uint8_t a = 42, b = 43;
        struct mo_store_record snapshot = {MO_STORE_STATE, 0, 1, &a, 1};
        CHECK(mo_store_call(store, MO_STORE_SNAPSHOT, &snapshot, NULL, NULL) == MO_STORE_OK);
        struct mo_store_record state = {MO_STORE_FAILURE_STATE, 0, 1, &a, 1};
        struct mo_store_record message = {MO_STORE_FAILURE_MESSAGE, 0, 1, &b, 1};
        uint64_t id = 0;
        CHECK(mo_store_capture(store, "seed", &state, &message, &id) == MO_STORE_CAPTURE_OK);
        mo_store_close(store);
        tamper(path, cases[i]);
        CHECK(mo_store_open(path, &store) == MO_STORE_OK);
        unsigned count = 0;
        CHECK(mo_store_failures(store, count_failures, &count) == MO_STORE_CORRUPT);
        id = 999;
        enum mo_store_capture_status captured =
            mo_store_capture(store, "next", &state, &message, &id);
        CHECK(captured == MO_STORE_CAPTURE_DAMAGED);
        CHECK(id == 999);
        struct mo_store_record log = {MO_STORE_LOG, 1, 1, &b, 1};
        CHECK(mo_store_call(store, MO_STORE_APPEND, &log, NULL, NULL) == MO_STORE_OK);
        uint32_t retained = 0;
        uint64_t evicted = 0;
        CHECK(mo_store_failure_stats(store, &retained, &evicted) != MO_STORE_IO);
        mo_store_close(store);
        CHECK(unlink(path) == 0);
        MO_ASSERT(snprintf(path, sizeof path, "%s", "/tmp/mo-damaged-diagnostics-XXXXXX") > 0);
        fd = mkstemp(path);
        CHECK(fd >= 0 && close(fd) == 0 && unlink(path) == 0);
        damaged_service(path, cases[i]);
        CHECK(unlink(path) == 0);
    }
}

/* A handler name that pushes the kind key past its limit names that reason. */
static void long_kind(void) {
    char name[MO_LIMIT_FAILURE_KIND_BYTES + 1];
    memset(name, 'k', sizeof name - 1);
    name[sizeof name - 1] = 0;
    char source[4 * MO_LIMIT_FAILURE_KIND_BYTES];
    int n = snprintf(source, sizeof source,
                     "module Main\nservice\nstate Int = 7\ntools %s\ntoken from env \"TOKEN\"\n"
                     "end\nfn %s(_state: Int, _now: Time) : Bool\nstop \"broken\"\nend\n",
                     name, name);
    MO_ASSERT(n > 0 && (size_t)n < sizeof source);
    struct mo_image *image = compile(source);
    struct mo_store *store = NULL;
    CHECK(mo_store_open(NULL, &store) == MO_STORE_OK);
    struct observations o = {0};
    struct mo_service_loop *service = open_service(image, store, &o);
    CHECK(mo_service_send(service, 0, NULL, 0, 1) == MO_SERVICE_OK);
    CHECK(mo_service_step(service));
    struct mo_service_event_summary event;
    CHECK(mo_service_event_entry(service, 0, &event));
    CHECK(event.record_id == 0 && event.record_unavailable != NULL &&
          strcmp(event.record_unavailable, "failure kind too long") == 0);
    CHECK(mo_service_running(service));
    mo_service_free(service);
    mo_store_close(store);
    mo_image_free(image);
}

static void run(void *unused) {
    mo_runtime_identity_init();
    text_context();
    damaged_diagnostics();
    long_kind();
    runtime_capture();
    offline_stop();
    offline_memory();
    offline_state_size();
    resource_capture();
    runtime_store_faults();
    argument_capture_limits();
    (void)unused;
    simulation_admission();
    simulation_persistence();
    simulation_verifier();
    simulation_api_recovery();
    simulation_api_steps();
    simulation_api_failures();
    allocator_bound();
    struct mo_image *image = compile(
        "module Main\nservice\nstate Int = 7\ntools add, read, reject, broken\n"
        "token from env \"TOKEN\"\nend\n"
        "fn add(state: Int, _now: Time, n: Int) : (Int, Int)\n(state + n, state + n)\nend\n"
        "fn read(state: Int, _now: Time) : List(Int)\n[state]\nend\n"
        "fn reject(_state: Int, _now: Time) : Result((Int, Int), Text)\nError(\"no\")\nend\n"
        "fn broken(_state: Int, _now: Time) : (Int, Int)\nstop \"broken\"\nend\n");
    cache_copy_failure();
    readonly_mailbox_size();
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
    replayed_timer_is_named();
    foreign_image(image, path);
    hostile_database(image, path);
    store_wait(image, path);
    history_reader(dir, path);
    invalid_payloads(image);
    composite_recovery();
    shared_recovery_bound();
    shared_result_bound();
    table_recovery();
    CHECK(rmdir(dir) == 0);
    mo_image_free(image);
}

struct failure_inventory {
    unsigned count, first, timer;
    uint64_t newest, oldest;
    bool kinds[70];
};
static bool failure_inventory(const struct mo_store_failure *f, void *context) {
    struct failure_inventory *v = context;
    CHECK(f->state.kind == MO_STORE_FAILURE_STATE);
    CHECK(f->message.kind == MO_STORE_FAILURE_MESSAGE);
    CHECK(f->state.sequence == f->message.sequence);
    CHECK(f->state.image == f->message.image);
    CHECK(f->state.length == 1 && f->state.bytes[0] == 42);
    CHECK(f->message.length == 1 && f->message.bytes[0] == 43);
    v->count++;
    v->first += f->first ? 1u : 0u;
    v->timer += strcmp(f->kind, "timer") == 0 ? 1u : 0u;
    unsigned n = 0;
    if (sscanf(f->kind, "kind-%u", &n) == 1 && n < sizeof v->kinds)
        v->kinds[n] = true;
    if (v->oldest == 0)
        v->oldest = f->id;
    v->newest = f->id;
    return true;
}

static void diagnostic_retention(void) {
    struct mo_store *store = NULL;
    CHECK(mo_store_open(NULL, &store) == MO_STORE_OK);
    if (store == NULL)
        return;
    const uint8_t state_byte = 42, message_byte = 43;
    struct mo_store_record state = {MO_STORE_FAILURE_STATE, 0, 123, &state_byte, 1};
    struct mo_store_record message = {MO_STORE_FAILURE_MESSAGE, 0, 123, &message_byte, 1};
    uint64_t id = 0;
    for (unsigned n = 0; n < 100; n++) {
        CHECK(mo_store_capture(store, "timer", &state, &message, &id) == MO_STORE_CAPTURE_OK);
        CHECK(id == n + 1);
    }
    struct failure_inventory v = {0};
    CHECK(mo_store_failures(store, failure_inventory, &v) == MO_STORE_OK);
    CHECK(v.count == 2 && v.first == 1 && v.newest == 100);
    for (unsigned n = 0; n < 70; n++) {
        char kind[32];
        CHECK(snprintf(kind, sizeof kind, "kind-%u", n) > 0);
        CHECK(mo_store_capture(store, kind, &state, &message, &id) == MO_STORE_CAPTURE_OK);
    }
    v = (struct failure_inventory){0};
    CHECK(mo_store_failures(store, failure_inventory, &v) == MO_STORE_OK);
    CHECK(v.count == 64 && v.first == 64 && v.newest == 170);
    /* The victim is the kind whose latest record is oldest: timer (latest 100), then kind-0..5. */
    CHECK(v.timer == 0 && v.oldest == 107);
    for (unsigned n = 0; n < 70; n++)
        CHECK(v.kinds[n] == (n >= 6));
    uint32_t retained = 0;
    uint64_t evicted = 0;
    CHECK(mo_store_failure_stats(store, &retained, &evicted) == MO_STORE_OK);
    CHECK(retained == 64 && evicted == 7);
    crash_point = 13;
    id = 999;
    CHECK(mo_store_capture(store, "refused", &state, &message, &id) == MO_STORE_CAPTURE_REFUSED);
    CHECK(id == 999);
    crash_point = 0;
    struct mo_store_record snapshot_record = {MO_STORE_STATE, 0, 123, &state_byte, 1};
    CHECK(mo_store_call(store, MO_STORE_SNAPSHOT, &snapshot_record, NULL, NULL) == MO_STORE_OK);
    CHECK(mo_store_capture(store, "after-refusal", &state, &message, &id) == MO_STORE_CAPTURE_OK);
    crash_point = 20;
    id = 999;
    CHECK(mo_store_capture(store, "auto-rollback", &state, &message, &id) ==
          MO_STORE_CAPTURE_REFUSED);
    CHECK(id == 999);
    crash_point = 0;
    CHECK(mo_store_call(store, MO_STORE_SNAPSHOT, &snapshot_record, NULL, NULL) == MO_STORE_OK);
    message.length = MO_LIMIT_FAILURE_MESSAGE_BYTES + 1;
    id = 999;
    CHECK(mo_store_capture(store, "oversize", &state, &message, &id) == MO_STORE_CAPTURE_REFUSED);
    CHECK(id == 999);
    mo_store_close(store);
}

static bool failure_ids(const struct mo_store_failure *f, void *context) {
    uint64_t *ids = context;
    *ids |= UINT64_C(1) << f->id;
    return true;
}
static bool catalog_length(const struct mo_store_failure *f, void *context) {
    unsigned *count = context;
    CHECK(f->state.bytes == NULL && f->state.length == 1);
    (*count)++;
    return true;
}
static void diagnostic_catalog(void) {
    char path[] = "/tmp/mo-failure-catalog-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0 && close(fd) == 0 && unlink(path) == 0);
    struct mo_store *store = NULL;
    CHECK(mo_store_open(path, &store) == MO_STORE_OK);
    const uint8_t a = 42, b = 43;
    struct mo_store_record state = {MO_STORE_FAILURE_STATE, 0, 1, &a, 1};
    struct mo_store_record message = {MO_STORE_FAILURE_MESSAGE, 0, 1, &b, 1};
    uint64_t id = 0;
    for (unsigned i = 0; i < 10; i++) {
        char kind[16];
        CHECK(snprintf(kind, sizeof kind, "kind%u", i) > 0);
        CHECK(mo_store_capture(store, kind, &state, &message, &id) == MO_STORE_CAPTURE_OK);
    }
    mo_store_close(store);
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK);
    CHECK(sqlite3_exec(db, "UPDATE failure_state SET record=zeroblob(length(record)) WHERE id=1",
                       NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    CHECK(mo_store_open(path, &store) == MO_STORE_OK);
    unsigned count = 0;
    uint32_t omitted = 999;
    CHECK(mo_store_failure_catalog(store, 0, catalog_length, &count, &omitted) == MO_STORE_OK);
    CHECK(count == MO_LIMIT_FAILURE_VIEW_RECORDS && omitted == 2);
    count = 0;
    CHECK(mo_store_failure_catalog(store, 10, catalog_length, &count, &omitted) == MO_STORE_OK);
    CHECK(count == 0 && omitted == 0);
    uint64_t ids = 0;
    CHECK(mo_store_failure_get(store, 2, failure_ids, &ids) == MO_STORE_OK);
    CHECK(ids == 4);
    CHECK(mo_store_failure_get(store, 11, failure_ids, &ids) == MO_STORE_OK && ids == 4);
    CHECK(mo_store_failure_get(store, 1, failure_ids, &ids) == MO_STORE_CORRUPT);
    mo_store_close(store);
    CHECK(unlink(path) == 0);
}
static void diagnostic_byte_retention(void) {
    struct mo_store *store = NULL;
    CHECK(mo_store_open(NULL, &store) == MO_STORE_OK);
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_SAVED_BYTES + 4096);
    uint8_t *bytes = mo_arena_allocate(&arena, MO_LIMIT_SAVED_BYTES, 1);
    MO_ASSERT(bytes != NULL);
    memset(bytes, 42, MO_LIMIT_SAVED_BYTES);
    const uint8_t b = 43;
    struct mo_store_record state = {MO_STORE_FAILURE_STATE, 0, 1, bytes, 1};
    struct mo_store_record message = {MO_STORE_FAILURE_MESSAGE, 0, 1, &b, 1};
    uint64_t id = 0;
    CHECK(mo_store_capture(store, "A", &state, &message, &id) == MO_STORE_CAPTURE_OK);
    state.length = MO_LIMIT_SAVED_BYTES;
    for (const char *kind = "BCD"; *kind != 0; kind++) {
        char name[] = {*kind, 0};
        CHECK(mo_store_capture(store, name, &state, &message, &id) == MO_STORE_CAPTURE_OK);
    }
    uint64_t ids = 0;
    CHECK(mo_store_failures(store, failure_ids, &ids) == MO_STORE_OK && ids == 30);
    id = 999;
    CHECK(mo_store_capture(store, "A", &state, &message, &id) == MO_STORE_CAPTURE_REFUSED);
    CHECK(id == 999);
    CHECK(mo_store_capture(store, "E", &state, &message, &id) == MO_STORE_CAPTURE_OK && id == 5);
    ids = 0;
    CHECK(mo_store_failures(store, failure_ids, &ids) == MO_STORE_OK && ids == 56);
    uint32_t count = 0;
    uint64_t evicted = 0;
    CHECK(mo_store_failure_stats(store, &count, &evicted) == MO_STORE_OK);
    CHECK(count == 3 && evicted == 2);
    mo_store_close(store);
    mo_arena_free(&arena);
}

static void diagnostic_refusal_matrix(void) {
    for (unsigned fault = 21; fault <= 27; fault++) {
        struct mo_store *store = NULL;
        CHECK(mo_store_open(NULL, &store) == MO_STORE_OK);
        const uint8_t a = 42, b = 43;
        struct mo_store_record state = {MO_STORE_FAILURE_STATE, 0, 1, &a, 1};
        struct mo_store_record message = {MO_STORE_FAILURE_MESSAGE, 0, 1, &b, 1};
        uint64_t id = 0;
        CHECK(mo_store_capture(store, "before", &state, &message, &id) == MO_STORE_CAPTURE_OK);
        bool safe = fault <= 23 || fault == 26;
        crash_point = fault;
        id = 999;
        CHECK(mo_store_capture(store, "fault", &state, &message, &id) ==
              (safe ? MO_STORE_CAPTURE_REFUSED : MO_STORE_CAPTURE_UNSAFE));
        CHECK(id == 999);
        crash_point = 0;
        struct failure_inventory inventory = {0};
        if (safe) {
            CHECK(mo_store_failures(store, failure_inventory, &inventory) == MO_STORE_OK);
            CHECK(inventory.count == 1 && inventory.newest == 1);
            struct mo_store_record snapshot_record = {MO_STORE_STATE, 0, 1, &a, 1};
            CHECK(mo_store_call(store, MO_STORE_SNAPSHOT, &snapshot_record, NULL, NULL) ==
                  MO_STORE_OK);
            CHECK(mo_store_capture(store, "after", &state, &message, &id) == MO_STORE_CAPTURE_OK);
            CHECK(id == 2);
        } else {
            CHECK(mo_store_failures(store, failure_inventory, &inventory) == MO_STORE_IO);
            CHECK(mo_store_capture(store, "after", &state, &message, &id) ==
                  MO_STORE_CAPTURE_UNSAFE);
            CHECK(id == 999);
        }
        mo_store_close(store);
    }
}

static void diagnostic_unsafe(void) {
    for (unsigned fault = 14; fault <= 15; fault++) {
        struct mo_store *store = NULL;
        CHECK(mo_store_open(NULL, &store) == MO_STORE_OK);
        const uint8_t a = 42, b = 43;
        struct mo_store_record state = {MO_STORE_FAILURE_STATE, 0, 1, &a, 1};
        struct mo_store_record message = {MO_STORE_FAILURE_MESSAGE, 0, 1, &b, 1};
        uint64_t id = 999;
        crash_point = fault;
        CHECK(mo_store_capture(store, "unsafe", &state, &message, &id) == MO_STORE_CAPTURE_UNSAFE);
        CHECK(id == 999);
        crash_point = 0;
        mo_store_close(store);
    }
}
static void diagnostic_crashes(void) {
    for (unsigned fault = 17; fault <= 19; fault++) {
        char path[] = "/tmp/mo-failure-crash-XXXXXX";
        int fd = mkstemp(path);
        CHECK(fd >= 0);
        CHECK(close(fd) == 0 && unlink(path) == 0);
        struct mo_store *store = NULL;
        CHECK(mo_store_open(path, &store) == MO_STORE_OK);
        const uint8_t a = 42, b = 43;
        struct mo_store_record state = {MO_STORE_FAILURE_STATE, 0, 1, &a, 1};
        struct mo_store_record message = {MO_STORE_FAILURE_MESSAGE, 0, 1, &b, 1};
        uint64_t id = 0;
        for (unsigned i = 0; i < 3; i++)
            CHECK(mo_store_capture(store, "timer", &state, &message, &id) == MO_STORE_CAPTURE_OK);
        mo_store_close(store);
        pid_t child = fork();
        CHECK(child >= 0);
        if (child == 0) {
            if (mo_store_open(path, &store) != MO_STORE_OK)
                _exit(80);
            crash_point = fault;
            enum mo_store_capture_status status =
                mo_store_capture(store, "timer", &state, &message, &id);
            (void)status;
            _exit(81);
        }
        int status;
        CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == (int)(60 + fault));
        CHECK(mo_store_open(path, &store) == MO_STORE_OK);
        struct failure_inventory v = {0};
        CHECK(mo_store_failures(store, failure_inventory, &v) == MO_STORE_OK);
        CHECK(v.count == 2 && v.first == 1 && v.newest == (fault == 19 ? 4u : 3u));
        mo_store_close(store);
        CHECK(unlink(path) == 0);
    }
}

static void diagnostic_tables(void) {
    char path[] = "/tmp/mo-diagnostic-tables-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    CHECK(close(fd) == 0);
    CHECK(unlink(path) == 0);
    struct mo_store *store = NULL;
    CHECK(mo_store_open(path, &store) == MO_STORE_OK);
    mo_store_close(store);
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK);
    sqlite3_stmt *stmt = NULL;
    CHECK(sqlite3_prepare_v2(
              db, "SELECT count(*) FROM sqlite_schema WHERE name IN ('failure','failure_state')",
              -1, &stmt, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    CHECK(sqlite3_column_int(stmt, 0) == 2);
    CHECK(sqlite3_finalize(stmt) == SQLITE_OK);
    CHECK(sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    CHECK(sqlite3_column_int(stmt, 0) == 0);
    CHECK(sqlite3_finalize(stmt) == SQLITE_OK);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    CHECK(unlink(path) == 0);
}

int main(void) {
    diagnostic_tables();
    diagnostic_retention();
    diagnostic_refusal_matrix();
    diagnostic_catalog();
    diagnostic_byte_retention();
    diagnostic_unsafe();
    diagnostic_crashes();
    CHECK(mo_with_stack(MO_SOURCE_STACK_SIZE, run, NULL) == 0);
    return mo_test_report("service store");
}
