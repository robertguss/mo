#include "base/hash.h"
#include "base/stack.h"
#include "service_compile.h"
#include "test.h"
#include "tools/recovery.h"
#include "tools/tests.h"

/* A whole program from its entry file, with the toolchain's lib/ folder, as mo test compiles one.
 */
static struct mo_image *compile_path(const char *path) {
    struct mo_arena arena, lowered;
    mo_arena_init(&arena, MO_CLI_CHECK_SIZE);
    mo_arena_init(&lowered, MO_LIMIT_LOWER_WORK);
    struct mo_sources sources;
    struct mo_diags diags;
    MO_ASSERT(mo_sources_init(&sources) && mo_diags_init(&diags));
    struct mo_module_set *set = mo_load_program(&arena, &sources, path, "lib", &diags);
    MO_ASSERT(set != NULL && !mo_diags_any(&diags));
    struct mo_program *program = mo_check(&arena, set, &diags);
    MO_ASSERT(program != NULL && !mo_diags_any(&diags));
    struct mo_services services;
    struct mo_core *core = mo_lower_services(&lowered, program, &sources, &diags, NULL, &services);
    MO_ASSERT(core != NULL && !mo_diags_any(&diags));
    struct mo_buffer wire;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    MO_ASSERT(mo_compile_services(core, &services, &wire, &diags));
    struct mo_load_error error;
    struct mo_image *image = mo_load(wire.bytes, wire.length, &error);
    MO_ASSERT(image != NULL);
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    MO_ASSERT(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    mo_buffer_free(&wire);
    mo_arena_free(&lowered);
    mo_arena_free(&arena);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
    return image;
}

/* 2026-01-01T00:00:00Z. */
static const int64_t epoch = INT64_C(1767225600000000);

static uint32_t tool_named(const struct mo_image *image, uint32_t service, const char *name) {
    const struct mo_bytecode *file = mo_image_bytecode(image);
    const struct mo_bytecode_service *s = &file->services[service];
    for (uint32_t i = 0; i < s->tool_count; i++)
        if (named(file->strings[s->tools[i].name], name))
            return i;
    MO_ASSERT(false);
    return MO_NO_INDEX;
}

static struct mo_recovery *checker(const struct mo_image *image, uint32_t service) {
    struct mo_limits limits = mo_limits_default();
    struct mo_recovery *recovery = NULL;
    MO_ASSERT(mo_recovery_new(image, service, &limits, &recovery) == MO_INPUT_OK);
    return recovery;
}

/* Runs a check's first sequences. */
static enum mo_recovery_outcome search(struct mo_recovery *recovery, uint64_t sequences,
                                       const struct mo_recovery_hooks *hooks,
                                       struct mo_recovery_result *result) {
    return mo_recovery_search(recovery, 0, sequences, hooks, result);
}

/* What a choice holds: false unless value is the case named. */
static bool case_field(const struct mo_types *types, uint32_t type, struct mo_cell value,
                       const char *name, uint32_t field, struct mo_cell *out, uint32_t *out_type) {
    const struct mo_type *t = &types->items[type];
    MO_ASSERT(t->kind == MO_TYPE_CHOICE);
    uint32_t tag = mo_cell_get_tag(value);
    if (strcmp(t->cases[tag].name, name) != 0)
        return false;
    MO_ASSERT(field < t->cases[tag].field_count);
    *out_type = t->cases[tag].fields[field].type;
    struct mo_cell payload = mo_cell_get_payload(value);
    *out =
        mo_case_shape(types, type, tag) == MO_CASE_BOXED ? mo_value_field(payload, field) : payload;
    return true;
}

/* A struct is a choice of one case at run time. */
static struct mo_cell record_field(const struct mo_types *types, uint32_t type,
                                   struct mo_cell value, const char *name, uint32_t *out_type) {
    const struct mo_type *t = &types->items[type];
    MO_ASSERT(t->kind == MO_TYPE_CHOICE && t->case_count == 1);
    struct mo_cell field;
    for (uint32_t i = 0; i < t->cases[0].field_count; i++)
        if (strcmp(t->cases[0].fields[i].name, name) == 0 &&
            case_field(types, type, value, t->cases[0].name, i, &field, out_type))
            return field;
    MO_ASSERT(false);
    return (struct mo_cell){0};
}

/* The task queue's states each sequence reaches, read from what the checker saw. */
struct queue_watch {
    const struct mo_types *types;
    struct mo_heap *heap;
    uint32_t state_type, lease, acknowledge, fail;
    uint64_t sequence;
    uint32_t restart; /* the latest restart's step, or MO_NO_INDEX */
    struct lease {
        int64_t id, attempt;
        uint32_t step;
        bool failed; /* fail took it back */
    } leases[MO_CLI_RECOVERY_STEPS];
    uint32_t lease_count;
    uint64_t dead, leased_again, acknowledged_after_restart, refused_arguments, steps;
};

static bool queue_has_dead_job(struct queue_watch *w, struct mo_simulation *simulation) {
    struct mo_buffer bytes;
    mo_buffer_init(&bytes, MO_LIMIT_SAVED_BYTES);
    struct mo_cell queue;
    MO_ASSERT(mo_simulation_encode(simulation, &bytes) &&
              mo_value_decode(w->heap, w->state_type, bytes.bytes, bytes.length, &queue));
    uint32_t jobs_type, job_type, state_type;
    struct mo_cell jobs = record_field(w->types, w->state_type, queue, "jobs", &jobs_type);
    job_type = w->types->items[jobs_type].parameters[1];
    bool dead = false;
    uint32_t position = 0;
    struct mo_cell id, job;
    while (!dead && mo_table_next(jobs, &position, &id, &job)) {
        struct mo_cell state = record_field(w->types, job_type, job, "state", &state_type);
        uint32_t tag = mo_cell_get_tag(state);
        dead = strcmp(w->types->items[state_type].cases[tag].name, "Dead") == 0;
    }
    mo_value_drop(w->heap, w->state_type, queue);
    mo_buffer_free(&bytes);
    return dead;
}

static void watch_queue(const struct mo_recovery_observation *o, void *context) {
    struct queue_watch *w = context;
    if (o->sequence != w->sequence || o->index == 0) {
        w->sequence = o->sequence;
        w->restart = MO_NO_INDEX;
        w->lease_count = 0;
    }
    w->steps++;
    const struct mo_recovery_step *step = o->step;
    if (step->kind == MO_RECOVERY_RESTART && step->outcome == MO_SIMULATION_OK)
        w->restart = o->index;
    if (step->kind == MO_RECOVERY_SEND &&
        (step->sent == MO_SENT_ARGUMENT || step->sent == MO_SENT_TOO_LARGE ||
         step->sent == MO_SENT_REFUSED))
        w->refused_arguments++;
    if (step->kind != MO_RECOVERY_CRASH && step->kind != MO_RECOVERY_RESTART &&
        mo_simulation_running(o->simulation) && queue_has_dead_job(w, o->simulation))
        w->dead++;
    if (step->kind != MO_RECOVERY_SEND || step->sent != MO_SENT_REPLIED)
        return;
    if (step->tool == w->lease) {
        struct mo_cell taken, job;
        uint32_t taken_type, job_type, field_type;
        if (!case_field(w->types, o->reply->type, o->reply->reply, "Ok", 0, &taken, &taken_type))
            return;
        uint32_t option_type = w->types->items[taken_type].parameters[1];
        if (!case_field(w->types, option_type, mo_value_field(taken, 1), "Some", 0, &job,
                        &job_type))
            return;
        int64_t id = mo_cell_get_int(record_field(w->types, job_type, job, "id", &field_type));
        int64_t attempt =
            mo_cell_get_int(record_field(w->types, job_type, job, "attempts", &field_type));
        /* A job's earlier attempt that fail never took back ran out instead. */
        for (uint32_t i = 0; i < w->lease_count; i++)
            if (w->leases[i].id == id && w->leases[i].attempt + 1 == attempt &&
                !w->leases[i].failed)
                w->leased_again++;
        MO_ASSERT(w->lease_count < MO_CLI_RECOVERY_STEPS);
        w->leases[w->lease_count++] = (struct lease){id, attempt, o->index, false};
    } else if ((step->tool == w->fail || step->tool == w->acknowledge) && step->updated) {
        int64_t id = mo_cell_get_int(step->arguments[0]);
        int64_t attempt = mo_cell_get_int(step->arguments[1]);
        for (uint32_t i = 0; i < w->lease_count; i++) {
            if (w->leases[i].id != id || w->leases[i].attempt != attempt)
                continue;
            w->leases[i].failed |= step->tool == w->fail;
            if (step->tool == w->acknowledge && w->restart != MO_NO_INDEX &&
                w->leases[i].step < w->restart)
                w->acknowledged_after_restart++;
        }
    }
}

static void task_queue(void) {
    struct mo_image *image = compile_path("services/task-queue/main.mo");
    const struct mo_bytecode *file = mo_image_bytecode(image);
    MO_ASSERT(file->service_count == 1);
    struct mo_limits limits = mo_limits_default();
    struct mo_process *process = mo_process_new(image, &limits);
    MO_ASSERT(process != NULL);
    struct queue_watch watch = {.types = mo_image_types(image),
                                .heap = mo_process_heap(process),
                                .state_type = file->services[0].state_type,
                                .lease = tool_named(image, 0, "lease"),
                                .acknowledge = tool_named(image, 0, "acknowledge"),
                                .fail = tool_named(image, 0, "fail"),
                                .sequence = UINT64_MAX};
    const struct mo_recovery_hooks hooks = {watch_queue, NULL, &watch};

    /* mo test's 200 sequences, watched. */
    struct mo_recovery *recovery = checker(image, 0);
    CHECK(strcmp(mo_recovery_name(recovery), "Main.service.recovery") == 0);
    struct mo_recovery_result result;
    CHECK(search(recovery, 200, &hooks, &result) == MO_RECOVERY_PASSED);
    CHECK(result.counts.sequences == 200 && result.counts.restarts >= 400);
    /* Some restarts recover past a snapshot taken at the hundredth commit. */
    CHECK(result.counts.snapshots > 0 && result.counts.replayed > 0);
    CHECK(watch.dead > 0);
    CHECK(watch.leased_again > 0);
    CHECK(watch.acknowledged_after_restart > 0);
    CHECK(watch.refused_arguments == 0);
    mo_recovery_result_free(recovery, &result);
    mo_recovery_free(recovery);
    mo_process_free(process);
    mo_image_free(image);
}

static const char counter_service[] =
    "module Main\nservice\nstate Int = 0\ntools add, read\nevery 1.minute tick\n"
    "token from env \"TOKEN\"\nend\n"
    "fn add(state: Int, _now: Time, n: Int) : Result((Int, Int), Text)\n"
    "if n < 0 or n > 1000: Error(\"out of range\") else: Ok((state + n, state + n))\nend\n"
    "fn read(state: Int, _now: Time) : List(Int)\n[state]\nend\n"
    "fn tick(state: Int, _now: Time) : Int\nstate + 100\nend\n";

static bool keep_going(const struct mo_simulation_failure *failure, void *context) {
    (void)failure;
    (void)context;
    return true;
}

/* The oracle compares encodings byte for byte. */
static void comparison(void) {
    struct mo_image *image = compile(counter_service);
    uint32_t type = mo_image_bytecode(image)->services[0].state_type;
    struct mo_limits limits = mo_limits_default();
    struct mo_process *process = mo_process_new(image, &limits);
    MO_ASSERT(process != NULL);
    struct mo_heap *heap = mo_process_heap(process);
    const struct mo_simulation_hooks hooks = {keep_going, NULL};
    struct mo_simulation *simulation = NULL;
    MO_ASSERT(mo_simulation_new(image, 0, epoch, &simulation) == MO_STORE_OK);
    struct mo_buffer before, after, expected;
    mo_buffer_init(&before, MO_LIMIT_SAVED_BYTES);
    mo_buffer_init(&after, MO_LIMIT_SAVED_BYTES);
    mo_buffer_init(&expected, MO_LIMIT_SAVED_BYTES);
    MO_ASSERT(mo_value_encode(heap, type, mo_cell_int(100), &before));
    /* Never opened: there is no state to read. */
    CHECK(mo_recovery_compare(simulation, &before, &after) == MO_RECOVERY_UNREADABLE);
    MO_ASSERT(mo_simulation_open(simulation, &hooks) == MO_SIMULATION_OK);
    MO_ASSERT(mo_simulation_drain(simulation, &hooks) == MO_SIMULATION_OK);
    /* The startup tick made it 100. */
    CHECK(mo_recovery_compare(simulation, &before, &after) == MO_RECOVERY_EQUAL);
    CHECK(after.length == before.length && memcmp(after.bytes, before.bytes, after.length) == 0);
    /* A pair that differs by one Int is a mismatch, and after holds what was read. */
    mo_buffer_clear(&before);
    MO_ASSERT(mo_value_encode(heap, type, mo_cell_int(101), &before));
    CHECK(mo_recovery_compare(simulation, &before, &after) == MO_RECOVERY_DIFFERENT);
    MO_ASSERT(mo_value_encode(heap, type, mo_cell_int(100), &expected));
    CHECK(after.length == expected.length &&
          memcmp(after.bytes, expected.bytes, after.length) == 0);
    mo_simulation_crash(simulation);
    CHECK(mo_recovery_compare(simulation, &before, &after) == MO_RECOVERY_UNREADABLE);
    mo_simulation_free(simulation);
    mo_buffer_free(&expected);
    mo_buffer_free(&after);
    mo_buffer_free(&before);
    mo_process_free(process);
    mo_image_free(image);
}

/* A given sequence runs exactly as written, on a fresh store each time. */
static void replay(void) {
    struct mo_image *image = compile(counter_service);
    struct mo_recovery *recovery = checker(image, 0);
    uint32_t add = tool_named(image, 0, "add");
    struct mo_recovery_sequence sequence;
    mo_recovery_sequence_init(&sequence);
    struct mo_cell *n = mo_arena_allocate(&sequence.arena, sizeof *n, _Alignof(struct mo_cell));
    MO_ASSERT(n != NULL);
    *n = mo_cell_int(7);
    const struct mo_recovery_step steps[] = {
        {.kind = MO_RECOVERY_SEND, .tool = add, .arguments = n},
        {.kind = MO_RECOVERY_ADVANCE, .duration = 90 * INT64_C(1000000)},
        {.kind = MO_RECOVERY_CRASH},
        {.kind = MO_RECOVERY_RESTART},
        {.kind = MO_RECOVERY_SEND, .tool = add, .arguments = n},
        {.kind = MO_RECOVERY_CRASH},
        {.kind = MO_RECOVERY_RESTART}};
    sequence.count = sizeof steps / sizeof steps[0];
    memcpy(sequence.steps, steps, sizeof steps);
    for (int round = 0; round < 2; round++) {
        struct mo_recovery_result result;
        CHECK(mo_recovery_replay(recovery, &sequence, NULL, &result) == MO_RECOVERY_PASSED);
        CHECK(result.counts.restarts == 2 && result.counts.sequences == 1);
        /* The startup tick, add, one tick in 90 s, then after each restart its startup tick,
         * and add between them. */
        CHECK(result.counts.commits == 6);
        CHECK(sequence.steps[0].ran && sequence.steps[0].sent == MO_SENT_REPLIED &&
              sequence.steps[0].updated);
        CHECK(sequence.steps[1].outcome == MO_SIMULATION_OK && sequence.steps[1].firings == 1);
        CHECK(sequence.steps[3].outcome == MO_SIMULATION_OK);
        CHECK(sequence.steps[4].sent == MO_SENT_REPLIED && sequence.steps[6].ran);
        mo_recovery_result_free(recovery, &result);
    }
    mo_recovery_sequence_clear(recovery, &sequence);
    mo_recovery_free(recovery);
    mo_image_free(image);
}

/* Counts what the steps did, and compares each restart's recovered state with what the observer
 * expects: the state at the stop when the service had stopped, or a fixed value. */
struct tally {
    uint64_t steps, sends, replied, rejected, tool_failed, down, refused, advances, restarts;
    uint64_t stops, compared_at_stop, compared_to_fixed, crossed;
    uint64_t supervisor_restarts, supervisor_stops; /* as the steps recorded them */
    struct mo_buffer at_stop, fixed;
    bool stopped;
    uint32_t tools[4];
    uint64_t hash;
    /* With service set, the tallest argument sent. */
    const struct mo_types *types;
    const struct mo_bytecode_service *service;
    uint32_t tallest;
    /* With heap set, the hash covers the arguments' values too. */
    struct mo_heap *heap;
    const struct mo_bytecode_service *hashed;
};

/* 1 for a scalar, one more than its tallest part otherwise. */
static uint32_t height(const struct mo_types *types, uint32_t type, struct mo_cell value) {
    const struct mo_type *t = &types->items[type];
    uint32_t tallest = 0, child_type, h;
    struct mo_cell child;
    if (t->kind == MO_TYPE_LIST)
        for (uint32_t i = 0; i < mo_list_size(value); i++) {
            MO_ASSERT(mo_list_get(value, i, &child));
            h = height(types, t->parameters[0], child);
            tallest = h > tallest ? h : tallest;
        }
    if (t->kind == MO_TYPE_CHOICE) {
        uint32_t tag = mo_cell_get_tag(value);
        for (uint32_t f = 0; f < t->cases[tag].field_count; f++) {
            MO_ASSERT(case_field(types, type, value, t->cases[tag].name, f, &child, &child_type));
            h = height(types, child_type, child);
            tallest = h > tallest ? h : tallest;
        }
    }
    return tallest + 1;
}

static void tally_init(struct tally *t) {
    *t = (struct tally){0};
    mo_buffer_init(&t->at_stop, MO_LIMIT_SAVED_BYTES);
    mo_buffer_init(&t->fixed, MO_LIMIT_SAVED_BYTES);
}

static void tally_free(struct tally *t) {
    mo_buffer_free(&t->fixed);
    mo_buffer_free(&t->at_stop);
}

static bool same(const struct mo_buffer *a, const struct mo_buffer *b) {
    return a->length == b->length && memcmp(a->bytes, b->bytes, a->length) == 0;
}

static void count(const struct mo_recovery_observation *o, void *context) {
    struct tally *t = context;
    const struct mo_recovery_step *step = o->step;
    t->steps++;
    struct mo_buffer state;
    mo_buffer_init(&state, MO_LIMIT_SAVED_BYTES);
    switch (step->kind) {
    case MO_RECOVERY_SEND:
        t->sends++;
        for (uint32_t a = 0; t->service != NULL && a < t->service->tools[step->tool].argument_count;
             a++) {
            uint32_t h = height(t->types, t->service->tools[step->tool].argument_types[a],
                                step->arguments[a]);
            t->tallest = h > t->tallest ? h : t->tallest;
        }
        t->tools[step->tool < 4 ? step->tool : 3]++;
        t->replied += step->sent == MO_SENT_REPLIED;
        t->rejected += step->sent == MO_SENT_REJECTED;
        t->tool_failed += step->sent == MO_SENT_TOOL_FAILED;
        t->down += step->sent == MO_SENT_DOWN;
        t->supervisor_restarts += step->supervisor == MO_SUPERVISOR_RESTARTED;
        t->supervisor_stops += step->supervisor == MO_SUPERVISOR_STOPPED;
        t->refused += step->sent == MO_SENT_ARGUMENT || step->sent == MO_SENT_TOO_LARGE ||
                      step->sent == MO_SENT_REFUSED || step->sent == MO_SENT_REPLY_COPY ||
                      step->sent == MO_SENT_FAILED;
        break;
    case MO_RECOVERY_ADVANCE:
        t->advances++;
        break;
    case MO_RECOVERY_CRASH:
    case MO_RECOVERY_RESTART:
        break;
    }
    if (step->kind != MO_RECOVERY_CRASH && step->kind != MO_RECOVERY_RESTART && !t->stopped &&
        !mo_simulation_running(o->simulation)) {
        t->stops++;
        t->stopped = true;
        mo_buffer_clear(&t->at_stop);
        MO_ASSERT(mo_simulation_encode(o->simulation, &t->at_stop));
    }
    if (step->kind == MO_RECOVERY_RESTART && step->outcome == MO_SIMULATION_OK) {
        t->restarts++;
        MO_ASSERT(mo_simulation_encode(o->simulation, &state));
        if (t->stopped && same(&state, &t->at_stop))
            t->compared_at_stop++;
        if (t->fixed.length != 0 && same(&state, &t->fixed))
            t->compared_to_fixed++;
        t->stopped = false;
    }
    /* Everything that decides the sequence, for comparing two runs. */
    uint64_t words[4] = {o->sequence, step->kind, step->tool, (uint64_t)step->duration};
    static const uint64_t key[2] = {1, 2};
    t->hash ^= mo_hash(key, words, sizeof words) + t->steps;
    for (uint32_t a = 0; t->heap != NULL && step->kind == MO_RECOVERY_SEND &&
                         a < t->hashed->tools[step->tool].argument_count;
         a++) {
        mo_buffer_clear(&state);
        MO_ASSERT(mo_value_encode(t->heap, t->hashed->tools[step->tool].argument_types[a],
                                  step->arguments[a], &state));
        t->hash ^= mo_hash(key, state.bytes, state.length) + a;
    }
    mo_buffer_free(&state);
}

/* Read-only tools commit nothing: every restart recovers the start value. */
static void read_only(void) {
    struct mo_image *image =
        compile("module Main\nservice\nstate Int = 5\ntools read, twice\n"
                "token from env \"TOKEN\"\nend\n"
                "fn read(state: Int, _now: Time) : List(Int)\n[state]\nend\n"
                "fn twice(state: Int, _now: Time, n: Int) : List(Int)\n[state, n]\nend\n");
    struct mo_recovery *recovery = checker(image, 0);
    struct tally t;
    tally_init(&t);
    struct mo_limits limits = mo_limits_default();
    struct mo_process *process = mo_process_new(image, &limits);
    MO_ASSERT(process != NULL && mo_value_encode(mo_process_heap(process),
                                                 mo_image_bytecode(image)->services[0].state_type,
                                                 mo_cell_int(5), &t.fixed));
    const struct mo_recovery_hooks hooks = {count, NULL, &t};
    struct mo_recovery_result result;
    CHECK(search(recovery, 50, &hooks, &result) == MO_RECOVERY_PASSED);
    CHECK(result.counts.sequences == 50 && result.counts.restarts == t.restarts);
    CHECK(t.restarts > 50 && t.compared_to_fixed == t.restarts);
    CHECK(t.replied > 0 && t.replied == t.sends && result.counts.commits == 0);
    mo_recovery_result_free(recovery, &result);
    mo_recovery_free(recovery);
    tally_free(&t);
    mo_process_free(process);
    mo_image_free(image);
}

/* A handler that stops is one ordinary step, and so is the supervisor restarting the service. */
static void failing_tool(void) {
    struct mo_image *image =
        compile("module Main\nservice\nstate Int = 0\ntools add, boom\n"
                "token from env \"TOKEN\"\nend\n"
                "fn add(state: Int, _now: Time) : (Int, Int)\n(state + 1, state + 1)\nend\n"
                "fn boom(_state: Int, _now: Time) : (Int, Int)\nstop \"boom\"\nend\n");
    struct mo_recovery *recovery = checker(image, 0);
    struct tally t;
    tally_init(&t);
    const struct mo_recovery_hooks hooks = {count, NULL, &t};
    struct mo_recovery_result result;
    CHECK(search(recovery, 100, &hooks, &result) == MO_RECOVERY_PASSED);
    CHECK(t.tool_failed > 0 && t.replied > 0 && t.refused == 0);
    CHECK(result.counts.restarts == t.restarts && t.restarts >= 100);
    mo_recovery_result_free(recovery, &result);
    mo_recovery_free(recovery);
    tally_free(&t);
    mo_image_free(image);
}

/* Fifteen failures within ten minutes stop the service. The restart that follows recovers the
 * state the service stopped with, which its startup ticks moved on from the start value. */
static void supervisor_stop(void) {
    struct mo_image *image =
        compile("module Main\nservice\nstate Int = 0\ntools boom\nevery 1.day tick\n"
                "token from env \"TOKEN\"\nend\n"
                "fn boom(_state: Int, _now: Time, n: Int) : (Int, Int)\n"
                "if n == n: stop \"boom\" else: (0, 0)\nend\n"
                "fn tick(state: Int, _now: Time) : Int\nstate + 1\nend\n");
    struct mo_recovery *recovery = checker(image, 0);
    struct tally t;
    tally_init(&t);
    const struct mo_recovery_hooks hooks = {count, NULL, &t};
    struct mo_recovery_result result;
    CHECK(search(recovery, 400, &hooks, &result) == MO_RECOVERY_PASSED);
    CHECK(t.tool_failed == t.sends - t.down && t.stops >= 5 && t.compared_at_stop == t.stops);
    /* Each stop is a send's, and two restarts come before each stop's restart. */
    CHECK(t.supervisor_stops == t.stops && t.supervisor_restarts >= 2 * t.stops);
    mo_recovery_result_free(recovery, &result);
    mo_recovery_free(recovery);
    tally_free(&t);
    mo_image_free(image);
}

/* A start value that fails is the service's own failure, reported as a stop. */
static void start_failure(void) {
    struct mo_image *image = compile("module Main\nservice\nstate Int = initial()\ntools read\n"
                                     "token from env \"TOKEN\"\nend\n"
                                     "fn initial() : Int\nstop \"boom\"\nend\n"
                                     "fn read(state: Int, _now: Time) : List(Int)\n[state]\nend\n");
    struct mo_recovery *recovery = checker(image, 0);
    struct mo_recovery_result result;
    CHECK(search(recovery, 200, NULL, &result) == MO_RECOVERY_STOPPED);
    CHECK(result.outcome == MO_RECOVERY_STOPPED && result.counts.restarts == 0);
    CHECK(result.failure.status == MO_SERVICE_DEFECT &&
          result.failure.function == mo_image_bytecode(image)->services[0].start);
    CHECK(strcmp(result.failure.reason, "stop") == 0 && result.failure.detail_length == 4 &&
          memcmp(result.failure.detail, "boom", 4) == 0);
    mo_recovery_result_free(recovery, &result);
    mo_recovery_free(recovery);
    mo_image_free(image);
}

/* Each service of a program gets its own check, on its own store, drawing only its own tools. */
static void two_services(void) {
    struct mo_image *image = compile_path("tests/recovery/two-services/main.mo");
    const struct mo_bytecode *file = mo_image_bytecode(image);
    MO_ASSERT(file->service_count == 2);
    const char *names[2];
    uint64_t hashes[2];
    struct mo_recovery *recoveries[2] = {checker(image, 0), checker(image, 1)};
    struct tally tallies[2];
    for (uint32_t i = 0; i < 2; i++) {
        tally_init(&tallies[i]);
        names[i] = mo_recovery_name(recoveries[i]);
    }
    /* Both checkers live at once, alternating, so each holds its own state. */
    for (uint32_t round = 0; round < 3; round++)
        for (uint32_t i = 0; i < 2; i++) {
            const struct mo_recovery_hooks hooks = {count, NULL, &tallies[i]};
            struct mo_recovery_result result;
            CHECK(search(recoveries[i], 20, &hooks, &result) == MO_RECOVERY_PASSED);
            CHECK(result.counts.sequences == 20 && result.counts.restarts >= 20);
            mo_recovery_result_free(recoveries[i], &result);
        }
    for (uint32_t i = 0; i < 2; i++) {
        uint32_t tools = file->services[i].tool_count;
        CHECK(tallies[i].sends > 0 && tallies[i].tools[tools < 4 ? tools : 3] == 0);
        hashes[i] = tallies[i].hash;
    }
    bool main_first = strcmp(names[0], "Main.service.recovery") == 0;
    CHECK(strcmp(names[main_first ? 0 : 1], "Main.service.recovery") == 0);
    CHECK(strcmp(names[main_first ? 1 : 0], "Tally.service.recovery") == 0);
    CHECK(hashes[0] != hashes[1]);
    for (uint32_t i = 0; i < 2; i++) {
        mo_recovery_free(recoveries[i]);
        tally_free(&tallies[i]);
    }
    mo_image_free(image);
}

/* A recursive argument type stays inside the saved depth: no step is refused at copy-in. */
static void recursive_argument(void) {
    struct mo_image *image = compile(
        "module Main\nexpose Nest(..)\nenum Nest\n  Leaf(n: Int)\n  Node(items: List(Nest))\nend\n"
        "service\nstate Int = 0\ntools put, nested\ntoken from env \"TOKEN\"\nend\n"
        "fn put(state: Int, _now: Time, _nest: Nest) : (Int, Int)\n(state + 1, state + 1)\nend\n"
        "fn nested(state: Int, _now: Time, lists: List(List(Int))) : (Int, Int)\n"
        "(state + lists.size, lists.size)\nend\n");
    struct mo_recovery *recovery = checker(image, 0);
    struct tally t;
    tally_init(&t);
    t.types = mo_image_types(image);
    t.service = &mo_image_bytecode(image)->services[0];
    const struct mo_recovery_hooks hooks = {count, NULL, &t};
    struct mo_recovery_result result;
    CHECK(search(recovery, 200, &hooks, &result) == MO_RECOVERY_PASSED);
    CHECK(t.tools[0] > 0 && t.tools[1] > 0 && t.refused == 0 && t.replied == t.sends);
    /* The recursion is exercised, and stays well inside the saved depth. */
    CHECK(t.tallest >= 4 && t.tallest <= MO_LIMIT_SAVED_DEPTH / 2);
    mo_recovery_result_free(recovery, &result);
    mo_recovery_free(recovery);
    tally_free(&t);
    mo_image_free(image);
}

/* Two runs of one check generate the same sequences, and every store a sequence opens is closed
 * again: many more sequences than MO_LIMIT_STORES leave every store free. */
static void deterministic(void) {
    struct mo_image *image = compile(counter_service);
    uint64_t hashes[2];
    uint64_t steps[2];
    for (uint32_t run = 0; run < 2; run++) {
        struct mo_recovery *recovery = checker(image, 0);
        struct tally t;
        tally_init(&t);
        t.heap = mo_recovery_heap(recovery);
        t.hashed = &mo_image_bytecode(image)->services[0];
        const struct mo_recovery_hooks hooks = {count, NULL, &t};
        struct mo_recovery_result result;
        CHECK(search(recovery, 10 * MO_LIMIT_STORES, &hooks, &result) == MO_RECOVERY_PASSED);
        CHECK(result.counts.sequences == 10 * MO_LIMIT_STORES);
        CHECK(t.advances > 0 && t.replied > 0 && t.rejected > 0);
        hashes[run] = t.hash;
        steps[run] = t.steps;
        mo_recovery_result_free(recovery, &result);
        mo_recovery_free(recovery);
        tally_free(&t);
    }
    CHECK(hashes[0] == hashes[1] && steps[0] == steps[1]);
    struct mo_store *stores[MO_LIMIT_STORES];
    for (uint32_t i = 0; i < MO_LIMIT_STORES; i++)
        CHECK(mo_store_open(NULL, &stores[i]) == MO_STORE_OK);
    for (uint32_t i = 0; i < MO_LIMIT_STORES; i++)
        mo_store_close(stores[i]);
    mo_image_free(image);
}

/* A failing timer handler is counted on the advance that fired it. */
static void failed_firings(void) {
    struct mo_image *image =
        compile("module Main\nservice\nstate Int = 0\ntools read\n"
                "every 1.minute tick\ntoken from env \"TOKEN\"\nend\n"
                "fn read(state: Int, _now: Time) : List(Int)\n[state]\nend\n"
                "fn tick(_state: Int, _now: Time) : Int\nstop \"tick\"\nend\n");
    struct mo_recovery *recovery = checker(image, 0);
    struct mo_recovery_sequence sequence;
    mo_recovery_sequence_init(&sequence);
    const struct mo_recovery_step steps[] = {{.kind = MO_RECOVERY_ADVANCE, .duration = 180000000},
                                             {.kind = MO_RECOVERY_CRASH},
                                             {.kind = MO_RECOVERY_RESTART}};
    sequence.count = sizeof steps / sizeof steps[0];
    memcpy(sequence.steps, steps, sizeof steps);
    struct mo_recovery_result result;
    CHECK(mo_recovery_replay(recovery, &sequence, NULL, &result) == MO_RECOVERY_PASSED);
    CHECK(sequence.steps[0].firings == 3 && sequence.steps[0].failed_firings == 3);
    mo_recovery_result_free(recovery, &result);
    mo_recovery_sequence_clear(recovery, &sequence);
    mo_recovery_free(recovery);
    mo_image_free(image);
}

/* Shrinking a sequence that does not fail tries candidates within its run budget, keeps the
 * sequence as it was and leaves nothing behind in the checker's heap. */
static void shrink_keeps_passing(void) {
    struct mo_image *image = compile(counter_service);
    struct mo_recovery *recovery = checker(image, 0);
    uint32_t add = tool_named(image, 0, "add");
    struct mo_recovery_sequence sequence;
    mo_recovery_sequence_init(&sequence);
    struct mo_cell *n =
        mo_arena_allocate_array(&sequence.arena, 2, sizeof *n, _Alignof(struct mo_cell));
    MO_ASSERT(n != NULL);
    n[0] = mo_cell_int(7);
    n[1] = mo_cell_int(900);
    const struct mo_recovery_step steps[] = {
        {.kind = MO_RECOVERY_SEND, .tool = add, .arguments = &n[0]},
        {.kind = MO_RECOVERY_ADVANCE, .duration = 90 * INT64_C(1000000)},
        {.kind = MO_RECOVERY_CRASH},
        {.kind = MO_RECOVERY_RESTART},
        {.kind = MO_RECOVERY_SEND, .tool = add, .arguments = &n[1]},
        {.kind = MO_RECOVERY_CRASH},
        {.kind = MO_RECOVERY_RESTART}};
    sequence.count = sizeof steps / sizeof steps[0];
    memcpy(sequence.steps, steps, sizeof steps);
    uint32_t runs = mo_recovery_shrink(recovery, &sequence, MO_RECOVERY_MISMATCH, NULL,
                                       (struct mo_recovery_verdict){MO_NO_INDEX, 0});
    CHECK(runs > sequence.count && runs <= MO_CLI_RECOVERY_SHRINK_RUNS);
    CHECK(sequence.count == sizeof steps / sizeof steps[0]);
    for (uint32_t i = 0; i < sequence.count; i++)
        CHECK(sequence.steps[i].kind == steps[i].kind &&
              sequence.steps[i].duration == steps[i].duration);
    CHECK(mo_cell_get_int(sequence.steps[0].arguments[0]) == 7 &&
          mo_cell_get_int(sequence.steps[4].arguments[0]) == 900);
    mo_recovery_sequence_clear(recovery, &sequence);
    mo_recovery_free(recovery);
    mo_image_free(image);
}

/* A check that fails on line 2 at a read from step 8 on, and on line 1 at a read at step 4 or 5. */
static struct mo_recovery_verdict two_lines(const struct mo_recovery_observation *observation,
                                            void *context) {
    (void)context;
    bool read = observation->step->kind == MO_RECOVERY_SEND;
    uint32_t index = observation->index;
    uint32_t line = read && index >= 8 ? 2 : read && (index == 4 || index == 5) ? 1 : 0;
    return (struct mo_recovery_verdict){line == 0 ? MO_NO_INDEX : 0, line};
}

/* Shrinking keeps a candidate only when the same check fails on the same line: deleting four of
 * the advances before the read would fail line 1 instead. */
static void shrink_same_line(void) {
    struct mo_image *image =
        compile("module Main\nservice\nstate Int = 0\ntools read\ntoken from env \"TOKEN\"\nend\n"
                "fn read(state: Int, _now: Time) : List(Int)\n[state]\nend\n");
    struct mo_recovery *recovery = checker(image, 0);
    struct mo_recovery_sequence sequence;
    mo_recovery_sequence_init(&sequence);
    for (uint32_t i = 0; i < 8; i++)
        sequence.steps[i] = (struct mo_recovery_step){.kind = MO_RECOVERY_ADVANCE, .duration = 1};
    sequence.steps[8] = (struct mo_recovery_step){.kind = MO_RECOVERY_SEND};
    sequence.count = 9;
    const struct mo_recovery_hooks hooks = {NULL, two_lines, NULL};
    const struct mo_recovery_verdict verdict = {0, 2};
    uint32_t runs = mo_recovery_shrink(recovery, &sequence, MO_RECOVERY_CHECK, &hooks, verdict);
    CHECK(runs > 0 && sequence.count == 9);
    struct mo_recovery_result result;
    CHECK(mo_recovery_replay(recovery, &sequence, &hooks, &result) == MO_RECOVERY_CHECK);
    CHECK(result.step == 8 && result.verdict.check == 0 && result.verdict.line == 2);
    mo_recovery_result_free(recovery, &result);
    mo_recovery_sequence_clear(recovery, &sequence);
    mo_recovery_free(recovery);
    mo_image_free(image);
}

/* An image of a fixture's program, which stays checked so its spans can be reported. */
static struct mo_image *compile_fixture(struct fixture *f, const char *source) {
    static const char *modules[] = {"Main"};
    init(f, source);
    MO_ASSERT(f->program != NULL && !mo_diags_any(&f->diags));
    f->set.names = modules;
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_LOWER_WORK);
    struct mo_services services;
    struct mo_core *core =
        mo_lower_services(&arena, f->program, &f->sources, &f->diags, NULL, &services);
    MO_ASSERT(core != NULL && !mo_diags_any(&f->diags));
    struct mo_buffer wire;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    MO_ASSERT(mo_compile_services(core, &services, &wire, &f->diags));
    struct mo_load_error error;
    struct mo_image *image = mo_load(wire.bytes, wire.length, &error);
    MO_ASSERT(image != NULL);
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    MO_ASSERT(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    mo_buffer_free(&wire);
    mo_arena_free(&arena);
    return image;
}

/* Each step's reply, copied into the checker's heap as mo test keeps it for the report. */
struct kept {
    struct mo_recovery *recovery;
    struct mo_cell values[MO_CLI_RECOVERY_STEPS];
    uint32_t types[MO_CLI_RECOVERY_STEPS];
};

static void keep(const struct mo_recovery_observation *o, void *context) {
    struct kept *k = context;
    if (o->reply == NULL)
        return;
    uint32_t type;
    struct mo_cell value =
        mo_recovery_reply(k->recovery, o->step->tool, o->step->sent, o->reply, &type);
    k->values[o->index] = mo_value_dup(mo_recovery_heap(k->recovery), type, value);
    k->types[o->index] = type;
}

static void drop_kept(struct kept *k) {
    for (uint32_t i = 0; i < MO_CLI_RECOVERY_STEPS; i++)
        if (k->types[i] != MO_NO_INDEX)
            mo_value_drop(mo_recovery_heap(k->recovery), k->types[i], k->values[i]);
}

static bool rendered(const struct mo_sources *sources, const struct mo_test_result *result,
                     bool json, const char *expected) {
    struct mo_buffer out;
    mo_buffer_init(&out, MO_CLI_DIAGNOSTIC_SIZE);
    bool ok = mo_render_test_result(sources, result, json, &out);
    size_t length = strlen(expected);
    /* JSON is compared from the outcome on, after the span's path bytes. */
    const char *from = json ? "\"outcome\":" : "";
    const uint8_t *at = out.bytes;
    while (json && ok && (size_t)(at - out.bytes) + strlen(from) <= out.length &&
           memcmp(at, from, strlen(from)) != 0)
        at++;
    size_t left = out.length - (size_t)(at - out.bytes);
    ok = ok && left == length && memcmp(at, expected, length) == 0;
    if (!ok)
        (void)fprintf(stderr, "rendered %d:\n%.*s\nexpected:\n%s\n", ok, (int)out.length,
                      (const char *)out.bytes, expected);
    mo_buffer_free(&out);
    return ok;
}

/* The design note's counter, whose recovery here is made to look as if it lost the newest log
 * record: a failure's report numbers the steps, gives literal arguments, each step's outcome and
 * both states. The runtime that really loses one is tested with the store loader's faults. */
static void failure_report(void) {
    struct fixture f;
    struct mo_image *image = compile_fixture(
        &f, "module Main\n\nservice\n  state Int = 0\n  tools add, total\n"
            "  token from env \"COUNTER_TOKEN\"\nend\n\n"
            "struct Total\n  count: Int\nend\n\n"
            "fn add(state: Int, _now: Time, amount: Int) : (Int, Unit)\n"
            "  (state + amount, Unit)\nend\n\n"
            "fn total(state: Int, _now: Time) : Total\n  Total(count: state)\nend\n");
    struct mo_recovery *recovery = checker(image, 0);
    struct mo_heap *heap = mo_recovery_heap(recovery);
    uint32_t state_type = mo_image_bytecode(image)->services[0].state_type;
    struct mo_recovery_sequence sequence;
    mo_recovery_sequence_init(&sequence);
    struct mo_cell *amount =
        mo_arena_allocate(&sequence.arena, sizeof *amount, _Alignof(struct mo_cell));
    MO_ASSERT(amount != NULL);
    *amount = mo_cell_int(1);
    const struct mo_recovery_step steps[] = {
        {.kind = MO_RECOVERY_SEND, .tool = tool_named(image, 0, "add"), .arguments = amount},
        {.kind = MO_RECOVERY_SEND, .tool = tool_named(image, 0, "total")},
        {.kind = MO_RECOVERY_ADVANCE, .duration = 1500000},
        {.kind = MO_RECOVERY_CRASH},
        {.kind = MO_RECOVERY_RESTART}};
    sequence.count = sizeof steps / sizeof steps[0];
    memcpy(sequence.steps, steps, sizeof steps);
    struct kept kept = {.recovery = recovery};
    for (uint32_t i = 0; i < MO_CLI_RECOVERY_STEPS; i++)
        kept.types[i] = MO_NO_INDEX;
    const struct mo_recovery_hooks hooks = {keep, NULL, &kept};
    struct mo_recovery_result result;
    CHECK(mo_recovery_replay(recovery, &sequence, &hooks, &result) == MO_RECOVERY_PASSED);
    /* The recovered state reads 0, as if the add were lost. */
    result.outcome = MO_RECOVERY_MISMATCH;
    result.step = 4;
    mo_buffer_clear(&result.after);
    MO_ASSERT(mo_value_encode(heap, state_type, mo_cell_int(0), &result.after));
    struct mo_recovery_report report = {.recovery = recovery,
                                        .counts = {.sequences = 3, .restarts = 2},
                                        .sequence = &sequence,
                                        .result = &result,
                                        .replies = kept.values,
                                        .reply_types = kept.types};
    const struct mo_observation observation = {.name = "Main.service.recovery",
                                               .service = &f.program->services[0]};
    struct mo_test_result test = {.observation = &observation,
                                  .file = mo_image_bytecode(image),
                                  .outcome = MO_FAILED,
                                  .microseconds = 5,
                                  .line.file = UINT32_MAX,
                                  .recovery = &report};
    CHECK(rendered(&f.sources, &test, false,
                   "failed Main.service.recovery  main.mo:3:1\n"
                   "  sequences: 3\n"
                   "  1. add(1)  replied Unit, committed\n"
                   "  2. total()  replied Total(count: 1)\n"
                   "  3. advance 1.5s  0 timer firings\n"
                   "  4. crash\n"
                   "  5. restart  recovered a different state\n"
                   "  state before step 4: 1\n"
                   "  state after step 5: 0\n"));
    CHECK(rendered(&f.sources, &test, true,
                   "\"outcome\":\"failed\",\"microseconds\":5,\"sequences\":3,\"restarts\":2,"
                   "\"steps\":[{\"action\":\"send\",\"tool\":\"add\",\"arguments\":[\"1\"],"
                   "\"outcome\":\"replied\",\"reply\":\"Unit\",\"committed\":true},"
                   "{\"action\":\"send\",\"tool\":\"total\",\"arguments\":[],"
                   "\"outcome\":\"replied\",\"reply\":\"Total(count: 1)\",\"committed\":false},"
                   "{\"action\":\"advance\",\"by\":\"1.5s\",\"firings\":0},"
                   "{\"action\":\"crash\"},{\"action\":\"restart\",\"recovered\":\"different\"}],"
                   "\"before\":\"1\",\"after\":\"0\"}\n"));

    /* A handler failure the supervisor stopped on, and a refused recovery. */
    mo_value_drop(heap, kept.types[1], kept.values[1]);
    kept.types[1] = MO_NO_INDEX;
    sequence.steps[1].sent = MO_SENT_TOOL_FAILED;
    sequence.steps[1].supervisor = MO_SUPERVISOR_STOPPED;
    sequence.steps[2].firings = 2;
    sequence.steps[2].failed_firings = 1;
    sequence.steps[4].outcome = MO_SIMULATION_DOWN;
    result.outcome = MO_RECOVERY_REFUSED;
    result.failure = (struct mo_recovery_failure){
        .status = MO_SERVICE_SAVED_INVALID, .span.file = MO_NO_INDEX, .function = MO_NO_INDEX};
    char expected[1024];
    int written = snprintf(expected, sizeof expected,
                           "failed Main.service.recovery  main.mo:3:1\n"
                           "  sequences: 3\n"
                           "  1. add(1)  replied Unit, committed\n"
                           "  2. total()  ToolFailed, service stopped\n"
                           "  3. advance 1.5s  2 timer firings, 1 failed\n"
                           "  4. crash\n"
                           "  5. restart  recovery refused the store: %s\n"
                           "  state before step 4: 1\n",
                           mo_service_status_text(MO_SERVICE_SAVED_INVALID));
    MO_ASSERT(written > 0 && (size_t)written < sizeof expected);
    CHECK(rendered(&f.sources, &test, false, expected));
    written =
        snprintf(expected, sizeof expected,
                 "\"outcome\":\"failed\",\"microseconds\":5,\"sequences\":3,\"restarts\":2,"
                 "\"steps\":[{\"action\":\"send\",\"tool\":\"add\",\"arguments\":[\"1\"],"
                 "\"outcome\":\"replied\",\"reply\":\"Unit\",\"committed\":true},"
                 "{\"action\":\"send\",\"tool\":\"total\",\"arguments\":[],"
                 "\"outcome\":\"ToolFailed\",\"committed\":false,\"supervisor\":\"stopped\"},"
                 "{\"action\":\"advance\",\"by\":\"1.5s\",\"firings\":2,\"failed_firings\":1},"
                 "{\"action\":\"crash\"},{\"action\":\"restart\",\"recovered\":\"refused\"}],"
                 "\"before\":\"1\",\"refusal\":\"%s\"}\n",
                 mo_service_status_text(MO_SERVICE_SAVED_INVALID));
    MO_ASSERT(written > 0 && (size_t)written < sizeof expected);
    CHECK(rendered(&f.sources, &test, true, expected));

    /* A pass shows no counts in text; JSON carries them. */
    test.outcome = MO_RETURNED;
    report.sequence = NULL;
    CHECK(rendered(&f.sources, &test, false, "passed Main.service.recovery  main.mo:3:1\n"));
    CHECK(rendered(&f.sources, &test, true,
                   "\"outcome\":\"passed\",\"microseconds\":5,\"sequences\":3,"
                   "\"restarts\":2}\n"));
    drop_kept(&kept);
    mo_recovery_result_free(recovery, &result);
    mo_recovery_sequence_clear(recovery, &sequence);
    mo_recovery_free(recovery);
    mo_image_free(image);
    free_fixture(&f);
}

/* States whose text is the same, as NaNs with different bits, also give the first byte that
 * differs. */
static void same_text(void) {
    struct fixture f;
    struct mo_image *image = compile_fixture(
        &f, "module Main\nservice\nstate Float = 0.0\ntools read\ntoken from env \"T\"\nend\n"
            "fn read(state: Float, _now: Time) : List(Float)\n[state]\nend\n");
    struct mo_recovery *recovery = checker(image, 0);
    struct mo_heap *heap = mo_recovery_heap(recovery);
    uint32_t state_type = mo_image_bytecode(image)->services[0].state_type;
    struct mo_recovery_sequence sequence;
    mo_recovery_sequence_init(&sequence);
    const struct mo_recovery_step steps[] = {{.kind = MO_RECOVERY_CRASH},
                                             {.kind = MO_RECOVERY_RESTART}};
    sequence.count = 2;
    memcpy(sequence.steps, steps, sizeof steps);
    struct mo_recovery_result result;
    CHECK(mo_recovery_replay(recovery, &sequence, NULL, &result) == MO_RECOVERY_PASSED);
    result.outcome = MO_RECOVERY_MISMATCH;
    result.step = 1;
    mo_buffer_clear(&result.before);
    mo_buffer_clear(&result.after);
    double one, other;
    uint64_t bits = UINT64_C(0x7ff8000000000001);
    memcpy(&one, &bits, sizeof one);
    bits = UINT64_C(0x7ff8000000000002);
    memcpy(&other, &bits, sizeof other);
    MO_ASSERT(mo_value_encode(heap, state_type, mo_cell_float(one), &result.before) &&
              mo_value_encode(heap, state_type, mo_cell_float(other), &result.after));
    size_t at = 0;
    while (result.before.bytes[at] == result.after.bytes[at])
        at++;
    uint32_t types[MO_CLI_RECOVERY_STEPS];
    for (uint32_t i = 0; i < MO_CLI_RECOVERY_STEPS; i++)
        types[i] = MO_NO_INDEX;
    struct mo_recovery_report report = {.recovery = recovery,
                                        .counts = {.sequences = 1},
                                        .sequence = &sequence,
                                        .result = &result,
                                        .reply_types = types};
    const struct mo_observation observation = {.name = "Main.service.recovery",
                                               .service = &f.program->services[0]};
    struct mo_test_result test = {.observation = &observation,
                                  .file = mo_image_bytecode(image),
                                  .outcome = MO_FAILED,
                                  .line.file = UINT32_MAX,
                                  .recovery = &report};
    char expected[512];
    int written = snprintf(expected, sizeof expected,
                           "failed Main.service.recovery  main.mo:2:1\n"
                           "  sequences: 1\n"
                           "  1. crash\n"
                           "  2. restart  recovered a different state\n"
                           "  state before step 1: nan\n"
                           "  state after step 2: nan\n"
                           "  the saved encodings first differ at byte %zu\n",
                           at);
    MO_ASSERT(written > 0 && (size_t)written < sizeof expected);
    CHECK(rendered(&f.sources, &test, false, expected));
    mo_recovery_result_free(recovery, &result);
    mo_recovery_sequence_clear(recovery, &sequence);
    mo_recovery_free(recovery);
    mo_image_free(image);
    free_fixture(&f);
}

static void run(void *unused) {
    (void)unused;
    failed_firings();
    shrink_keeps_passing();
    shrink_same_line();
    failure_report();
    same_text();
    comparison();
    replay();
    read_only();
    failing_tool();
    supervisor_stop();
    start_failure();
    two_services();
    recursive_argument();
    deterministic();
    task_queue();
}

int main(void) {
    CHECK(mo_with_stack(MO_SOURCE_STACK_SIZE, run, NULL) == 0);
    return mo_test_report("recovery");
}
