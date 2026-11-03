/* The recovery check's negative control. This test's service.c reaches the store through
 * mo_recovery_fault_call, which holds back the newest log record of every load and then hands it
 * to recovery once, never, or twice. Delivered once, nothing changes and the check passes; never,
 * recovery rebuilds a state without the last committed update and the comparison must fail;
 * twice, replay refuses the store and the check must fail through that refusal. */
#define _POSIX_C_SOURCE 200809L

#include "base/stack.h"
#include "service_compile.h"
#include "test.h"
#include "tools/recovery.h"
#include "tools/tools.h"

#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

MO_MUST_USE enum mo_store_status
mo_recovery_fault_call(struct mo_store *store, enum mo_store_operation operation,
                       const struct mo_store_record *record,
                       bool (*consume)(const struct mo_store_record *, void *), void *context);

/* How many times a load hands its newest log record to recovery, for the first faulty_loads loads
 * that hold one back; later ones hand it on once. */
static unsigned deliveries = 1, faulty_loads = UINT_MAX;
/* Loads that ended with a log record held back. */
static unsigned held_loads;

struct held {
    bool (*consume)(const struct mo_store_record *, void *);
    void *context;
    bool holding;
    struct mo_store_record record;
    struct mo_buffer bytes;
};

/* Passes every record on except the latest log record, which waits until the next one arrives. */
static bool hold_newest(const struct mo_store_record *record, void *context) {
    struct held *h = context;
    if (record->kind != MO_STORE_LOG)
        return h->consume(record, h->context);
    if (h->holding && !h->consume(&h->record, h->context))
        return false;
    mo_buffer_clear(&h->bytes);
    MO_ASSERT(mo_buffer_append(&h->bytes, record->bytes, record->length));
    h->record = *record;
    h->record.bytes = h->bytes.bytes;
    h->holding = true;
    return true;
}

enum mo_store_status mo_recovery_fault_call(struct mo_store *store,
                                            enum mo_store_operation operation,
                                            const struct mo_store_record *record,
                                            bool (*consume)(const struct mo_store_record *, void *),
                                            void *context) {
    if (operation != MO_STORE_LOAD)
        return mo_store_call(store, operation, record, consume, context);
    struct held h = {.consume = consume, .context = context};
    mo_buffer_init(&h.bytes, MO_LIMIT_SAVED_BYTES);
    enum mo_store_status status = mo_store_call(store, operation, record, hold_newest, &h);
    if (status == MO_STORE_OK && h.holding) {
        unsigned times = held_loads < faulty_loads ? deliveries : 1;
        held_loads++;
        for (unsigned i = 0; i < times && status == MO_STORE_OK; i++)
            if (!consume(&h.record, context))
                status = MO_STORE_CORRUPT; /* what the store reports when consume refuses */
    }
    mo_buffer_free(&h.bytes);
    return status;
}

/* The design note's counter: add is not idempotent, so losing or repeating one changes the state.
 */
static const char counter[] = "module Main\n\nservice\n  state Int = 0\n  tools add, total\n"
                              "  token from env \"COUNTER_TOKEN\"\nend\n\n"
                              "struct Total\n  count: Int\nend\n\n"
                              "fn add(state: Int, _now: Time, amount: Int) : (Int, Unit)\n"
                              "  (state + amount, Unit)\nend\n\n"
                              "fn total(state: Int, _now: Time) : Total\n"
                              "  Total(count: state)\nend\n";

static int64_t state_of(struct mo_recovery *recovery, uint32_t type, const struct mo_buffer *b) {
    struct mo_cell value;
    MO_ASSERT(mo_value_decode(mo_recovery_heap(recovery), type, b->bytes, b->length, &value));
    int64_t n = mo_cell_get_int(value);
    mo_value_drop(mo_recovery_heap(recovery), type, value);
    return n;
}

/* Shrinks sequence, which failed as expected: a committed add, a crash and a restart remain, and
 * they fail the same way each time they run again. */
static void shrunk_failure(struct mo_recovery *recovery, const struct mo_bytecode_service *service,
                           uint32_t add, struct mo_recovery_sequence *sequence,
                           enum mo_recovery_outcome expected, struct mo_deadline *deadline) {
    uint32_t runs = mo_recovery_shrink(recovery, sequence, expected, deadline);
    CHECK(runs > 0 && runs < MO_CLI_RECOVERY_SHRINK_RUNS);
    const struct mo_recovery_step *steps = sequence->steps;
    bool shape = sequence->count == 3 && steps[0].kind == MO_RECOVERY_SEND &&
                 steps[0].tool == add && steps[1].kind == MO_RECOVERY_CRASH &&
                 steps[2].kind == MO_RECOVERY_RESTART;
    CHECK(shape);
    if (!shape)
        return;
    int64_t amount = mo_cell_get_int(steps[0].arguments[0]);
    struct mo_buffer first_after;
    mo_buffer_init(&first_after, MO_LIMIT_SAVED_BYTES);
    for (int round = 0; round < 2; round++) {
        struct mo_recovery_result rerun;
        CHECK(mo_recovery_replay(recovery, sequence, NULL, &rerun) == expected);
        CHECK(rerun.step == 2);
        CHECK(steps[0].sent == MO_SENT_REPLIED && steps[0].updated);
        CHECK(state_of(recovery, service->state_type, &rerun.before) == amount);
        if (expected == MO_RECOVERY_MISMATCH) {
            /* The recovered state is the one before the add. */
            CHECK(amount != 0);
            CHECK(state_of(recovery, service->state_type, &rerun.after) == 0);
            if (round == 0)
                MO_ASSERT(mo_buffer_append(&first_after, rerun.after.bytes, rerun.after.length));
            else
                CHECK(rerun.after.length == first_after.length &&
                      memcmp(rerun.after.bytes, first_after.bytes, first_after.length) == 0);
        } else {
            CHECK(steps[2].outcome == MO_SIMULATION_DOWN);
            CHECK(rerun.failure.status == MO_SERVICE_SAVED_INVALID);
        }
        mo_recovery_result_free(recovery, &rerun);
    }
    mo_buffer_free(&first_after);
}

/* The checker's own entry points, as mo test calls them: search, shrink, then run the shrunk
 * sequence again, twice, from fresh stores. */
static void check_directly(unsigned delivered, enum mo_recovery_outcome expected) {
    deliveries = delivered;
    held_loads = 0;
    struct mo_image *image = compile(counter);
    const struct mo_bytecode_service *service = &mo_image_bytecode(image)->services[0];
    uint32_t add = 0;
    while (!named(mo_image_bytecode(image)->strings[service->tools[add].name], "add"))
        add++;
    struct mo_limits limits = mo_limits_default();
    struct mo_recovery *recovery = NULL;
    MO_ASSERT(mo_recovery_new(image, 0, &limits, &recovery) == MO_INPUT_OK);
    struct mo_deadline deadline;
    MO_ASSERT(mo_deadline_start(&deadline, 600));
    struct mo_recovery_result found;
    enum mo_recovery_outcome outcome = mo_recovery_search(recovery, 200, &deadline, NULL, &found);
    CHECK(outcome == expected);
    /* Every mode ran restarts whose load held a log record back. */
    CHECK(held_loads > 0);
    if (outcome == MO_RECOVERY_PASSED)
        CHECK(found.counts.restarts > 0 && found.counts.sequences == 200);
    if (outcome == expected && expected != MO_RECOVERY_PASSED)
        shrunk_failure(recovery, service, add, &found.sequence, expected, &deadline);
    mo_recovery_result_free(recovery, &found);
    mo_recovery_free(recovery);
    mo_image_free(image);
}

static void directly(void *unused) {
    (void)unused;
    check_directly(1, MO_RECOVERY_PASSED);
    check_directly(0, MO_RECOVERY_MISMATCH);
    check_directly(2, MO_RECOVERY_REFUSED);
}

/* A counter that only a timer changes, and only from ten minutes after the checker's clock starts
 * (2026-01-01T00:00:00Z), so a failing sequence needs a clock advance and shrinking has one to
 * shorten. */
static const char ticker[] = "module Main\n\nservice\n  state Int = 0\n  tools total\n"
                             "  every 1.minute tick\n  token from env \"COUNTER_TOKEN\"\nend\n\n"
                             "struct Total\n  count: Int\nend\n\n"
                             "fn total(state: Int, _now: Time) : Total\n"
                             "  Total(count: state)\nend\n\n"
                             "fn tick(state: Int, now: Time) : Int\n"
                             "  from = Time.parse(\"2026-01-01T00:10:00Z\") or stop \"time\"\n"
                             "  if now >= from: state + 1 else: state\nend\n";

/* Losing the ticker's newest log record fails the comparison, and shrinking halves the advance
 * until halving again would leave no counting tick: one advance of at least ten minutes and
 * under twenty, a crash and a restart, failing the same way each time they run again. */
static void shrunk_advance(void *unused) {
    (void)unused;
    deliveries = 0;
    held_loads = 0;
    struct mo_image *image = compile(ticker);
    struct mo_limits limits = mo_limits_default();
    struct mo_recovery *recovery = NULL;
    MO_ASSERT(mo_recovery_new(image, 0, &limits, &recovery) == MO_INPUT_OK);
    struct mo_deadline deadline;
    MO_ASSERT(mo_deadline_start(&deadline, 600));
    struct mo_recovery_result found;
    enum mo_recovery_outcome outcome = mo_recovery_search(recovery, 200, &deadline, NULL, &found);
    CHECK(outcome == MO_RECOVERY_MISMATCH);
    if (outcome == MO_RECOVERY_MISMATCH) {
        uint32_t runs = mo_recovery_shrink(recovery, &found.sequence, outcome, &deadline);
        CHECK(runs > 0 && runs < MO_CLI_RECOVERY_SHRINK_RUNS);
        const struct mo_recovery_step *steps = found.sequence.steps;
        const int64_t minute = INT64_C(60000000);
        bool shape = found.sequence.count == 3 && steps[0].kind == MO_RECOVERY_ADVANCE &&
                     steps[1].kind == MO_RECOVERY_CRASH && steps[2].kind == MO_RECOVERY_RESTART;
        CHECK(shape);
        CHECK(shape && steps[0].duration >= 10 * minute && steps[0].duration < 20 * minute);
        for (int round = 0; shape && round < 2; round++) {
            struct mo_recovery_result rerun;
            CHECK(mo_recovery_replay(recovery, &found.sequence, NULL, &rerun) ==
                  MO_RECOVERY_MISMATCH);
            CHECK(rerun.step == 2);
            /* Only the ticks from ten minutes on counted, and recovery lost the newest one. */
            int64_t counted = steps[0].duration / minute - 9;
            CHECK(state_of(recovery, mo_image_bytecode(image)->services[0].state_type,
                           &rerun.before) == counted);
            CHECK(state_of(recovery, mo_image_bytecode(image)->services[0].state_type,
                           &rerun.after) == counted - 1);
            mo_recovery_result_free(recovery, &rerun);
        }
    }
    mo_recovery_result_free(recovery, &found);
    mo_recovery_free(recovery);
    mo_image_free(image);
}

/* Every committed update changes this counter's state, so losing the newest one always shows. */
static const char increment[] = "module Main\n\nservice\n  state Int = 0\n  tools add\n"
                                "  token from env \"COUNTER_TOKEN\"\nend\n\n"
                                "fn add(state: Int, _now: Time) : (Int, Unit)\n"
                                "  (state + 1, Unit)\nend\n";

/* What mo test printed for program, on its output and its errors, and its exit status. */
struct printed {
    char path[96];
    char out[8192], err[8192];
    int status;
};

static void run_test_command(const char *program, struct printed *p) {
    char folder[] = "/tmp/mo-recovery-faults-XXXXXX", output[96], errors[96];
    MO_ASSERT(mkdtemp(folder) != NULL);
    MO_ASSERT(snprintf(p->path, sizeof p->path, "%s/main.mo", folder) > 0 &&
              snprintf(output, sizeof output, "%s/out", folder) > 0 &&
              snprintf(errors, sizeof errors, "%s/err", folder) > 0);
    FILE *source = fopen(p->path, "w");
    MO_ASSERT(source != NULL && fputs(program, source) >= 0 && fclose(source) == 0);
    const char *arguments[] = {"mo", "test", "--seconds", "60", p->path};
    struct mo_cli cli;
    const char *error;
    MO_ASSERT(mo_cli_parse(5, arguments, &cli, &error));
    FILE *out = fopen(output, "w+"), *err = fopen(errors, "w+");
    MO_ASSERT(out != NULL && err != NULL);
    p->status = mo_cli_execute(&cli, out, err);
    rewind(out);
    p->out[fread(p->out, 1, sizeof p->out - 1, out)] = '\0';
    rewind(err);
    p->err[fread(p->err, 1, sizeof p->err - 1, err)] = '\0';
    MO_ASSERT(fclose(out) == 0 && fclose(err) == 0);
    MO_ASSERT(unlink(p->path) == 0 && unlink(output) == 0 && unlink(errors) == 0 &&
              rmdir(folder) == 0);
}

static bool same_text(const char *printed, const char *expected) {
    if (strcmp(printed, expected) == 0)
        return true;
    (void)fprintf(stderr, "printed:\n%s\nexpected:\n%s\n", printed, expected);
    return false;
}

/* mo test on the counter's file: its exit status, and exactly what it printed, a header line
 * that says word and then body, which ends with the totals. */
static void check_command(unsigned delivered, int status, const char *word, const char *body) {
    deliveries = delivered;
    static struct printed p;
    run_test_command(counter, &p);
    CHECK(p.status == status);
    static char expected[8192];
    int written = snprintf(expected, sizeof expected, "%s Main.service.recovery  %s:3:1\n%s", word,
                           p.path, body);
    MO_ASSERT(written > 0 && (size_t)written < sizeof expected);
    CHECK(same_text(p.out, expected));
    CHECK(same_text(p.err, ""));
}

/* A failure that does not happen again: the newest record is lost at the first restart only, so
 * the shrunk sequence passes when it runs once more, and mo test reports E1404, not a result. */
static void check_unrepeatable(void) {
    deliveries = 0;
    faulty_loads = 1;
    held_loads = 0;
    static struct printed p;
    run_test_command(increment, &p);
    CHECK(held_loads > 1);
    CHECK(p.status == 1);
    CHECK(same_text(p.out, ""));
    char expected[512];
    int written = snprintf(expected, sizeof expected,
                           "error E1404: the shrunk recovery sequence did not fail the same way "
                           "when it ran again  %s:3:1\n",
                           p.path);
    MO_ASSERT(written > 0 && (size_t)written < sizeof expected);
    /* The source excerpt that follows ends with the check's name. */
    CHECK(strncmp(p.err, expected, strlen(expected)) == 0);
    CHECK(strstr(p.err, "^^^ Main.service.recovery\n") != NULL);
    faulty_loads = UINT_MAX;
}

int main(void) {
    CHECK(mo_with_stack(MO_SOURCE_STACK_SIZE, directly, NULL) == 0);
    CHECK(mo_with_stack(MO_SOURCE_STACK_SIZE, shrunk_advance, NULL) == 0);
    check_command(1, 0, "passed", "1 passed, 0 failed, 0 stopped\n");
    check_command(0, 1, "failed",
                  "  sequences: 3\n"
                  "  1. add(1)  replied Unit, committed\n"
                  "  2. crash\n"
                  "  3. restart  recovered a different state\n"
                  "  state before step 2: 1\n"
                  "  state after step 3: 0\n"
                  "0 passed, 1 failed, 0 stopped\n");
    char refused[512];
    int written = snprintf(refused, sizeof refused,
                           "  sequences: 1\n"
                           "  1. add(0)  replied Unit, committed\n"
                           "  2. crash\n"
                           "  3. restart  recovery refused the store: %s\n"
                           "  state before step 2: 0\n"
                           "0 passed, 1 failed, 0 stopped\n",
                           mo_service_status_text(MO_SERVICE_SAVED_INVALID));
    MO_ASSERT(written > 0 && (size_t)written < sizeof refused);
    check_command(2, 1, "failed", refused);
    check_unrepeatable();
    return mo_test_report("recovery faults");
}
