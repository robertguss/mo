/* Signature matching and the fix-check verdict, cell by cell against the tables of the
 * introspection design: section 6.2 (when two signatures match) and section 7.1 (the verdict).
 * Every expected value below is written from those tables, not from the code. */
#include "base/verdict.h"
#include "test.h"
#include <string.h>

static struct mo_failure_slice text(const char *s) {
    return (struct mo_failure_slice){(const uint8_t *)s, strlen(s)};
}

static struct mo_signature defect(const char *reason, uint32_t function, uint32_t file,
                                  uint32_t start, uint32_t end) {
    return (struct mo_signature){.class = MO_SIGNATURE_DEFECT,
                                 .reason = text(reason),
                                 .limit = text(""),
                                 .function = function,
                                 .file = file,
                                 .start = start,
                                 .end = end};
}

static struct mo_signature limited(enum mo_signature_class class, const char *reason,
                                   const char *limit, uint64_t value, uint32_t function,
                                   uint32_t start) {
    return (struct mo_signature){.class = class,
                                 .reason = text(reason),
                                 .limit = text(limit),
                                 .limit_value = value,
                                 .function = function,
                                 .file = 0,
                                 .start = start,
                                 .end = start + 4};
}

static struct mo_signature work(uint64_t value, uint32_t function, uint32_t start) {
    return limited(MO_SIGNATURE_WORK, "work limit", "MO_LIMIT_SERVICE_WORK", value, function,
                   start);
}
static struct mo_signature memory(const char *limit, uint64_t value, uint32_t function,
                                  uint32_t start) {
    return limited(MO_SIGNATURE_MEMORY, "running out of memory", limit, value, function, start);
}
static struct mo_signature state_size(const char *limit, uint64_t value) {
    return limited(MO_SIGNATURE_STATE_SIZE, "saved state size", limit, value, 3, 0);
}

static struct mo_run_outcome failed(struct mo_signature s) {
    static const enum mo_outcome_status status[] = {MO_OUTCOME_DEFECT, MO_OUTCOME_WORK,
                                                    MO_OUTCOME_MEMORY, MO_OUTCOME_STATE_SIZE};
    return (struct mo_run_outcome){.status = status[s.class], .signature = s};
}
static struct mo_run_outcome succeeded(enum mo_reply_kind reply, bool changed) {
    return (struct mo_run_outcome){
        .status = MO_OUTCOME_SUCCEEDED, .reply = reply, .state_changed = changed};
}
static struct mo_run_outcome refused(void) {
    return (struct mo_run_outcome){.status = MO_OUTCOME_REFUSED};
}

struct classify_case {
    const char *row;
    struct mo_signature recorded;
    struct mo_run_outcome offline;
    enum mo_replay_class expected;
};

/* Section 6.2, one case per "matches when" condition and per field it names or leaves out. */
static void classification(void) {
    const struct classify_case cases[] = {
        {"defect: class, reason, function and span equal", defect("integer overflow", 7, 1, 10, 20),
         failed(defect("integer overflow", 7, 1, 10, 20)), MO_REPLAY_REPRODUCED},
        {"defect: different reason", defect("integer overflow", 7, 1, 10, 20),
         failed(defect("division by zero", 7, 1, 10, 20)), MO_REPLAY_DIFFERENT},
        {"defect: different function", defect("integer overflow", 7, 1, 10, 20),
         failed(defect("integer overflow", 8, 1, 10, 20)), MO_REPLAY_DIFFERENT},
        {"defect: different span file", defect("integer overflow", 7, 1, 10, 20),
         failed(defect("integer overflow", 7, 2, 10, 20)), MO_REPLAY_DIFFERENT},
        {"defect: different span start", defect("integer overflow", 7, 1, 10, 20),
         failed(defect("integer overflow", 7, 1, 11, 20)), MO_REPLAY_DIFFERENT},
        {"defect: different span end", defect("integer overflow", 7, 1, 10, 20),
         failed(defect("integer overflow", 7, 1, 10, 21)), MO_REPLAY_DIFFERENT},
        {"defect: a reason that is a prefix of the recorded one", defect("stop", 7, 1, 10, 20),
         failed(defect("stopped", 7, 1, 10, 20)), MO_REPLAY_DIFFERENT},
        {"defect against work: class differs", defect("integer overflow", 7, 1, 10, 20),
         failed(work(500, 7, 10)), MO_REPLAY_DIFFERENT},
        {"work: class and value equal", work(500, 7, 10), failed(work(500, 7, 10)),
         MO_REPLAY_REPRODUCED},
        {"work: value equal, cancelled at another function and span", work(500, 7, 10),
         failed(work(500, 9, 40)), MO_REPLAY_REPRODUCED},
        {"work: different value", work(500, 7, 10), failed(work(501, 7, 10)), MO_REPLAY_DIFFERENT},
        {"work against memory with the same value: class differs", work(500, 7, 10),
         failed(memory("--max-heap", 500, 7, 10)), MO_REPLAY_DIFFERENT},
        {"memory: class, name and value equal", memory("--max-heap", 4096, 7, 10),
         failed(memory("--max-heap", 4096, 7, 10)), MO_REPLAY_REPRODUCED},
        {"memory: name and value equal at another function and span",
         memory("--max-heap", 4096, 7, 10), failed(memory("--max-heap", 4096, 9, 40)),
         MO_REPLAY_REPRODUCED},
        {"memory: --max-heap against --max-stack, same value", memory("--max-heap", 4096, 7, 10),
         failed(memory("--max-stack", 4096, 7, 10)), MO_REPLAY_DIFFERENT},
        {"memory: --max-heap against --max-depth, same value", memory("--max-heap", 4096, 7, 10),
         failed(memory("--max-depth", 4096, 7, 10)), MO_REPLAY_DIFFERENT},
        {"memory: --max-heap against native allocation, same value",
         memory("--max-heap", 4096, 7, 10), failed(memory("native allocation", 4096, 7, 10)),
         MO_REPLAY_DIFFERENT},
        {"memory: native allocation against --max-heap, same value",
         memory("native allocation", 0, 7, 10), failed(memory("--max-heap", 0, 7, 10)),
         MO_REPLAY_DIFFERENT},
        {"memory: native allocation matches itself", memory("native allocation", 0, 7, 10),
         failed(memory("native allocation", 0, 7, 10)), MO_REPLAY_REPRODUCED},
        {"memory: same name, different value", memory("--max-heap", 4096, 7, 10),
         failed(memory("--max-heap", 8192, 7, 10)), MO_REPLAY_DIFFERENT},
        {"state_size: saved copy failed again under the same limit",
         state_size("MO_LIMIT_SAVED_BYTES", 1048576),
         failed(state_size("MO_LIMIT_SAVED_BYTES", 1048576)), MO_REPLAY_REPRODUCED},
        {"state_size: a different saved-copy limit, same value",
         state_size("MO_LIMIT_SAVED_BYTES", 64), failed(state_size("MO_LIMIT_SAVED_DEPTH", 64)),
         MO_REPLAY_DIFFERENT},
        {"state_size: same limit name, different value",
         state_size("MO_LIMIT_SAVED_BYTES", 1048576),
         failed(state_size("MO_LIMIT_SAVED_BYTES", 2097152)), MO_REPLAY_DIFFERENT},
        {"offline run succeeded: not reproduced", defect("integer overflow", 7, 1, 10, 20),
         succeeded(MO_REPLY_OK, true), MO_REPLAY_NOT_REPRODUCED},
        {"offline run returned an Error reply: not reproduced", work(500, 7, 10),
         succeeded(MO_REPLY_ERROR, false), MO_REPLAY_NOT_REPRODUCED},
        {"offline run refused before it started", memory("--max-heap", 4096, 7, 10), refused(),
         MO_REPLAY_REFUSED},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        enum mo_replay_class got = mo_replay_classify(&cases[i].recorded, &cases[i].offline);
        if (got != cases[i].expected)
            (void)fprintf(stderr, "classification row \"%s\": got %d, expected %d\n", cases[i].row,
                          (int)got, (int)cases[i].expected);
        CHECK(got == cases[i].expected);
    }
}

/* Matching is symmetric on every pair the classification table uses. */
static void symmetry(void) {
    struct mo_signature a = memory("--max-heap", 4096, 7, 10);
    struct mo_signature b = memory("--max-stack", 4096, 7, 10);
    CHECK(mo_signature_match(&a, &a));
    CHECK(!mo_signature_match(&a, &b) && !mo_signature_match(&b, &a));
    struct mo_signature c = defect("integer overflow", 7, 1, 10, 20);
    struct mo_signature d = defect("integer overflow", 7, 1, 10, 21);
    CHECK(!mo_signature_match(&c, &d) && !mo_signature_match(&d, &c));
}

struct verdict_case {
    const char *row;
    enum mo_replay_class original;
    struct mo_run_outcome revised;
    enum mo_verdict expected;
};

/* Section 7.1's verdict table: every original classification against every way the revision can
 * end. The row names quote the design table's Original and Revised columns. */
static void verdicts(void) {
    struct mo_run_outcome revised_failures[] = {
        failed(defect("integer overflow", 7, 1, 10, 20)),
        failed(work(500, 7, 10)),
        failed(memory("--max-heap", 4096, 7, 10)),
        /* The handler returned, but the new state failed the saved-copy size check. */
        failed(state_size("MO_LIMIT_SAVED_BYTES", 1048576)),
    };
    struct mo_run_outcome ok = succeeded(MO_REPLY_OK, true);
    struct mo_run_outcome error = succeeded(MO_REPLY_ERROR, false);
    struct mo_run_outcome unchanged = succeeded(MO_REPLY_OK, false);
    const struct verdict_case cases[] = {
        {"reproduced | succeeded with an Ok reply", MO_REPLAY_REPRODUCED, ok, MO_VERDICT_VERIFIED},
        {"reproduced | succeeded with an Ok reply, state unchanged", MO_REPLAY_REPRODUCED,
         unchanged, MO_VERDICT_VERIFIED},
        {"reproduced | succeeded with an Error reply", MO_REPLAY_REPRODUCED, error,
         MO_VERDICT_VERIFIED},
        {"reproduced | failed: defect", MO_REPLAY_REPRODUCED, revised_failures[0],
         MO_VERDICT_NOT_FIXED},
        {"reproduced | exceeded the recorded budget: work", MO_REPLAY_REPRODUCED,
         revised_failures[1], MO_VERDICT_NOT_FIXED},
        {"reproduced | exceeded the recorded budget: memory", MO_REPLAY_REPRODUCED,
         revised_failures[2], MO_VERDICT_NOT_FIXED},
        {"reproduced | returned, state failed the size check", MO_REPLAY_REPRODUCED,
         revised_failures[3], MO_VERDICT_NOT_FIXED},
        {"different outcome | succeeded", MO_REPLAY_DIFFERENT, ok, MO_VERDICT_UNVERIFIED},
        {"different outcome | succeeded with an Error reply", MO_REPLAY_DIFFERENT, error,
         MO_VERDICT_UNVERIFIED},
        {"different outcome | failed: defect", MO_REPLAY_DIFFERENT, revised_failures[0],
         MO_VERDICT_UNVERIFIED},
        {"different outcome | failed: work", MO_REPLAY_DIFFERENT, revised_failures[1],
         MO_VERDICT_UNVERIFIED},
        {"different outcome | failed: memory", MO_REPLAY_DIFFERENT, revised_failures[2],
         MO_VERDICT_UNVERIFIED},
        {"different outcome | failed: state size", MO_REPLAY_DIFFERENT, revised_failures[3],
         MO_VERDICT_UNVERIFIED},
        {"not reproduced | succeeded", MO_REPLAY_NOT_REPRODUCED, ok, MO_VERDICT_UNVERIFIED},
        {"not reproduced | succeeded with an Error reply", MO_REPLAY_NOT_REPRODUCED, error,
         MO_VERDICT_UNVERIFIED},
        {"not reproduced | failed: defect", MO_REPLAY_NOT_REPRODUCED, revised_failures[0],
         MO_VERDICT_UNVERIFIED},
        {"not reproduced | failed: work", MO_REPLAY_NOT_REPRODUCED, revised_failures[1],
         MO_VERDICT_UNVERIFIED},
        {"not reproduced | failed: memory", MO_REPLAY_NOT_REPRODUCED, revised_failures[2],
         MO_VERDICT_UNVERIFIED},
        {"not reproduced | failed: state size", MO_REPLAY_NOT_REPRODUCED, revised_failures[3],
         MO_VERDICT_UNVERIFIED},
        {"refused | not run", MO_REPLAY_REFUSED, refused(), MO_VERDICT_REFUSED},
        {"refused | an outcome supplied anyway", MO_REPLAY_REFUSED, ok, MO_VERDICT_REFUSED},
        {"reproduced | revision refused", MO_REPLAY_REPRODUCED, refused(), MO_VERDICT_REFUSED},
        {"different outcome | revision refused", MO_REPLAY_DIFFERENT, refused(),
         MO_VERDICT_REFUSED},
        {"not reproduced | revision refused", MO_REPLAY_NOT_REPRODUCED, refused(),
         MO_VERDICT_REFUSED},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        enum mo_verdict got = mo_verdict(cases[i].original, &cases[i].revised);
        if (got != cases[i].expected)
            (void)fprintf(stderr, "verdict row \"%s\": got %d, expected %d\n", cases[i].row,
                          (int)got, (int)cases[i].expected);
        CHECK(got == cases[i].expected);
    }
    /* The verdict reports the revision's reply kind and state change unaltered. */
    CHECK(error.reply == MO_REPLY_ERROR && !error.state_changed);
}

/* The acceptance examples the plan pins on these functions, run through both steps. */
static void acceptance(void) {
    /* Heap exhausted live by queued arguments; offline the handler succeeds. The unchanged handler
     * as the fix succeeds too, and the verdict is unverified. */
    struct mo_signature heap = memory("--max-heap", 67108864, 7, 10);
    struct mo_run_outcome offline = succeeded(MO_REPLY_OK, true);
    enum mo_replay_class original = mo_replay_classify(&heap, &offline);
    CHECK(original == MO_REPLAY_NOT_REPRODUCED);
    CHECK(mo_verdict(original, &offline) == MO_VERDICT_UNVERIFIED);
    /* A work-limit failure that reproduces, with a revision that still exceeds the budget. */
    struct mo_signature budget = work(1000000, 7, 10);
    struct mo_run_outcome again = failed(work(1000000, 7, 10));
    struct mo_run_outcome still = failed(work(1000000, 8, 30));
    original = mo_replay_classify(&budget, &again);
    CHECK(original == MO_REPLAY_REPRODUCED);
    CHECK(mo_verdict(original, &still) == MO_VERDICT_NOT_FIXED);
    /* A revision that changes the state type is refused before the original runs. */
    struct mo_run_outcome before = refused();
    original = mo_replay_classify(&budget, &before);
    CHECK(original == MO_REPLAY_REFUSED);
    CHECK(mo_verdict(original, &before) == MO_VERDICT_REFUSED);
}

static void from_record(void) {
    static const uint8_t bytes[] = "integer overflow--max-heap";
    struct mo_failure_record record = {0};
    record.class = 0;
    record.reason = (struct mo_failure_slice){bytes, 16};
    record.limit = (struct mo_failure_slice){bytes + 16, 10};
    record.limit_value = 123456789;
    record.function = 11;
    record.file = 2;
    record.start = 300;
    record.end = 345;
    record.handler = 99;
    record.work = 77;
    struct mo_signature s = mo_signature_from_record(&record);
    CHECK(s.class == MO_SIGNATURE_DEFECT);
    CHECK(s.reason.bytes == bytes && s.reason.length == 16);
    CHECK(s.limit.bytes == bytes + 16 && s.limit.length == 10);
    CHECK(s.limit_value == 123456789);
    CHECK(s.function == 11 && s.file == 2 && s.start == 300 && s.end == 345);
    struct mo_signature same = defect("integer overflow", 11, 2, 300, 345);
    CHECK(mo_signature_match(&s, &same));
    static const enum mo_signature_class classes[] = {MO_SIGNATURE_DEFECT, MO_SIGNATURE_WORK,
                                                      MO_SIGNATURE_MEMORY, MO_SIGNATURE_STATE_SIZE};
    for (uint8_t c = 0; c < 4; c++) {
        record.class = c;
        CHECK(mo_signature_from_record(&record).class == classes[c]);
    }
}

int main(void) {
    classification();
    symmetry();
    verdicts();
    acceptance();
    from_record();
    return mo_test_report("verdict");
}
