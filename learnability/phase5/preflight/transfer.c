#define _POSIX_C_SOURCE 200809L
#include "service_compile.h"
#include <sqlite3.h>
#include <time.h>

static double now(void) {
    struct timespec t;
    MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void entry_controls(void) {
    const char *sources[] = {
        "module Main\nservice\nstate Int = 0\ntools read\ntoken from env \"TOKEN\"\nend\n"
        "fn read(state: Int, _now: Time) : Bool\nstate == 0\nend\n"
        "fn migrate(state: Int) : (Int, Int)\n(state, 1)\nend\n",
        "module Main\nservice\nstate Int = 0\ntools read\ntoken from env \"TOKEN\"\nend\n"
        "fn read(state: Int, _now: Time) : Bool\nstate == 0\nend\n"
        "fn migrate(state: Text) : Text\nstate\nend\n",
        "module Main\nservice\nstate Int = 0\ntools read\ntoken from env \"TOKEN\"\nend\n"
        "fn read(state: Int, _now: Time) : Bool\nstate == 0\nend\n"
        "fn migrate(state: Int, _console: Console) : Int\nstate\nend\n"};
    for (unsigned i = 0; i < 3; i++) {
        struct fixture f;
        init(&f, sources[i]);
        for (uint32_t d = 0; d < f.diags.count; d++)
            (void)fprintf(stderr, "entry %u: %s\n", i, f.diags.items[d].message);
        MO_ASSERT(f.program != NULL && !mo_diags_any(&f.diags));
        uint32_t function;
        enum mo_query_entry result =
            mo_check_query_entry(&f.arena, f.program, (const uint8_t *)"migrate", 7, MO_QUERY_STATE,
                                 UINT32_MAX, &f.diags, &function);
        MO_ASSERT(result == (i == 0 ? MO_QUERY_ENTRY_ACCEPTED : MO_QUERY_ENTRY_REFUSED));
        free_fixture(&f);
    }
}

int main(int argc, char **argv) {
    MO_ASSERT(argc == 2 || argc == 3 || argc == 4);
    entry_controls();
    const char *source =
        "module Main\nstruct Job\nid: Int\npayload: Text\nattempts: Int\nstate: JobState\nend\n"
        "enum JobState\nReady\nLeased(until: Time)\nDone\nDead\nend\n"
        "struct Queue\njobs: Map(Int, Job)\nnext_id: Int\nend\n"
        "service\nstate Queue = Queue(jobs: {}, next_id: 1)\ntools read\n"
        "token from env \"TOKEN\"\nend\n"
        "fn identity(state: Queue) : Queue\nstate\nend\n"
        "fn read(state: Queue, _now: Time) : Int\nidentity(state).next_id\nend\n";
    char input[65536];
    if (argc == 4) {
        FILE *file = fopen(argv[2], "rb");
        MO_ASSERT(file != NULL);
        size_t length = fread(input, 1, sizeof input - 1, file);
        MO_ASSERT(!ferror(file) && feof(file) && fclose(file) == 0);
        input[length] = '\0';
        source = input;
    }
    struct mo_image *image = compile(source);
    const struct mo_bytecode *code = mo_image_bytecode(image);
    const struct mo_types *types = mo_image_types(image);
    uint32_t queue = code->services[0].state_type;
    if (argc >= 3) {
        struct mo_limits limits = mo_limits_default();
        struct mo_process *process = mo_process_new(image, &limits);
        MO_ASSERT(process != NULL);
        struct mo_buffer schema;
        mo_buffer_init(&schema, MO_LIMIT_SAVED_BYTES);
        MO_ASSERT(mo_type_schema(mo_process_heap(process), queue, &schema));
        FILE *file = fopen(argv[argc == 4 ? 3 : 2], "wb");
        MO_ASSERT(file != NULL);
        MO_ASSERT(fwrite(schema.bytes, 1, schema.length, file) == schema.length);
        MO_ASSERT(fclose(file) == 0);
        mo_buffer_free(&schema);
        mo_process_free(process);
        if (argc == 4) {
            mo_image_free(image);
            return 0;
        }
    }
    uint32_t map = types->items[queue].cases[0].fields[0].type;
    uint32_t job = types->items[map].parameters[1];
    uint32_t state = types->items[job].cases[0].fields[3].type;
    uint32_t identity = UINT32_MAX;
    for (uint32_t i = 0; i < code->function_count; i++)
        if (named(code->strings[code->functions[i].name], "Main.identity"))
            identity = i;
    MO_ASSERT(identity != UINT32_MAX);
    struct mo_limits limits = mo_limits_default();
    limits.heap_size = 64 << 20;
    struct mo_process *old = mo_process_new(image, &limits);
    struct mo_process *next = mo_process_new(image, &limits);
    MO_ASSERT(old != NULL && next != NULL);
    struct mo_heap *a = mo_process_heap(old), *b = mo_process_heap(next);
    struct mo_cell table;
    MO_ASSERT(mo_value_map(a, map, NULL, NULL, 0, &table));
    char payload[2048];
    memset(payload, 'q', sizeof payload);
    for (int64_t i = 1; i <= 5000; i++) {
        struct mo_cell text, status, value;
        MO_ASSERT(mo_value_text(a, payload, sizeof payload, &text));
        struct mo_cell until = mo_cell_time(INT64_C(1794094566000000));
        MO_ASSERT(mo_value_case(a, state, i <= 500 ? 1u : 0u, i <= 500 ? &until : NULL,
                                i <= 500 ? 1u : 0u, &status));
        struct mo_cell fields[] = {mo_cell_int(i), text, mo_cell_int(i <= 500 ? 1 : 0), status};
        MO_ASSERT(mo_value_case(a, job, 0, fields, 4, &value));
        MO_ASSERT(mo_table_set(a, map, &table, mo_cell_int(i), value));
        mo_value_drop(a, job, value);
        mo_value_drop(a, state, status);
        mo_value_drop(a, types->items[job].cases[0].fields[1].type, text);
    }
    struct mo_cell fields[] = {table, mo_cell_int(5001)}, value, moved, result, validated;
    MO_ASSERT(mo_value_case(a, queue, 0, fields, 2, &value));
    struct mo_buffer first, second;
    mo_buffer_init(&first, MO_LIMIT_SAVED_BYTES);
    mo_buffer_init(&second, MO_LIMIT_SAVED_BYTES);
    struct mo_store *store;
    MO_ASSERT(mo_store_open(argv[1], &store) == MO_STORE_OK);
    double start = now();
    MO_ASSERT(mo_value_encode(a, queue, value, &first));
    double encoded = now();
    MO_ASSERT(mo_value_decode(b, queue, first.bytes, first.length, &moved));
    double decoded = now();
    mo_process_instruction_limit(next, UINT64_C(1048576));
    struct mo_defect defect;
    MO_ASSERT(mo_call(next, identity, &moved, 1, &result, &defect) == MO_RETURNED);
    uint64_t instructions = UINT64_C(1048576) - mo_process_instructions_remaining(next);
    double migrated = now();
    MO_ASSERT(mo_value_encode(b, queue, result, &second));
    MO_ASSERT(first.length == second.length &&
              memcmp(first.bytes, second.bytes, first.length) == 0);
    MO_ASSERT(mo_value_decode(a, queue, second.bytes, second.length, &validated));
    uint32_t text_type = types->items[job].cases[0].fields[1].type;
    char argument_bytes[15000];
    memset(argument_bytes, 'a', sizeof argument_bytes);
    struct mo_cell queued[64];
    for (unsigned i = 0; i < 64; i++) {
        struct mo_cell argument;
        size_t encoded_length;
        const char *failure = NULL;
        MO_ASSERT(mo_value_text(a, argument_bytes, sizeof argument_bytes, &argument));
        MO_ASSERT(
            mo_value_copy_saved(a, b, text_type, argument, &queued[i], &encoded_length, &failure));
        mo_value_drop(a, text_type, argument);
    }
    double checked = now();
    struct mo_store_record record = {.kind = MO_STORE_STATE,
                                     .sequence = 0,
                                     .image = mo_image_key(image, 0),
                                     .bytes = second.bytes,
                                     .length = second.length};
    MO_ASSERT(mo_store_call(store, MO_STORE_SNAPSHOT, &record, NULL, NULL) == MO_STORE_OK);
    mo_store_close(store);
    sqlite3 *db = NULL;
    MO_ASSERT(sqlite3_open(argv[1], &db) == SQLITE_OK);
    /* A second synced transaction deliberately overcounts one activation's sync cost. */
    const char *sql =
        "PRAGMA journal_mode=DELETE; PRAGMA synchronous=EXTRA; PRAGMA cache_size=-256;"
        "CREATE TABLE program(id INTEGER PRIMARY KEY, bundle BLOB NOT NULL) STRICT;"
        "CREATE TABLE previous_programs(ordinal INTEGER PRIMARY KEY, bundle BLOB NOT NULL) STRICT;"
        "CREATE TABLE previous_snapshots(ordinal INTEGER PRIMARY KEY, record BLOB NOT NULL) STRICT;"
        "CREATE TABLE upgrades(ordinal INTEGER PRIMARY KEY, report TEXT NOT NULL) STRICT;"
        "BEGIN IMMEDIATE;"
        "INSERT INTO program VALUES(1,zeroblob(1048576));"
        "INSERT INTO previous_programs VALUES(1,zeroblob(1048576));"
        "INSERT INTO previous_snapshots SELECT 1,record FROM snapshot;"
        "INSERT INTO upgrades VALUES(1,'identity migration');"
        "DELETE FROM log; PRAGMA user_version=2; COMMIT;";
    MO_ASSERT(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    MO_ASSERT(sqlite3_close(db) == SQLITE_OK);
    double committed = now();
    (void)printf("{\"encoded_bytes\":%zu,\"instructions\":%llu,\"old_heap_peak\":%zu,"
                 "\"new_heap_peak\":%zu,\"encode_s\":%.6f,\"decode_s\":%.6f,"
                 "\"identity_s\":%.6f,\"validate_s\":%.6f,\"snapshot_commit_s\":%.6f,"
                 "\"total_s\":%.6f}\n",
                 first.length, (unsigned long long)instructions, mo_heap_peak(a), mo_heap_peak(b),
                 encoded - start, decoded - encoded, migrated - decoded, checked - migrated,
                 committed - checked, committed - start);
    mo_buffer_free(&first);
    mo_buffer_free(&second);
    mo_process_free(next);
    mo_process_free(old);
    mo_image_free(image);
    return 0;
}
