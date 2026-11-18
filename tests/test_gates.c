#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "tools/tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const calls[] = {"gates/main.mo", "module Main\n\n"
                                                     "fn first(n: Int) : Int\n"
                                                     "  examples\n    first(1) == 2\n  end\n"
                                                     "  n + 1\nend\n\n"
                                                     "fn second(n: Int) : Int\n"
                                                     "  examples\n    second(1) == 3\n  end\n"
                                                     "  n + 2\nend\n\n"
                                                     "fn pick(x: T, _y: T) : T\n  x\nend\n\n"
                                                     "test \"pick an Int\"\n"
                                                     "  assert pick(1, 2) == 1\nend\n\n"
                                                     "test \"pick a Text\"\n"
                                                     "  assert pick(\"a\", \"b\") == \"a\"\nend\n\n"
                                                     "test \"one\"\n  assert first(2) == 3\nend\n\n"
                                                     "test \"two fails\"\n"
                                                     "  assert second(2) == 5\nend\n\n"
                                                     "test \"three\"\n"
                                                     "  assert first(3) == 4\nend\n\n"
                                                     "fn spin(n: Int) : Int\n  var i = n\n"
                                                     "  while true\n    i = i + 1\n  end\n"
                                                     "  i\nend\n\n"
                                                     "test \"loops\"\n"
                                                     "  assert spin(1) == 0\nend\n\n"
                                                     "fn main()\nend\n"};

static const char *const counter[] = {
    "gates/main.mo",
    "module Main\n\nuse Counter{Counter}\n\n"
    "fn total(c: Counter) : Int\n"
    "  promise \"a count is never negative\"\n    result >= 0\n"
    "    allows c: Counter.zero(), result: 0\n"
    "    forbids c: Counter.zero(), result: -1\n  end\n\n"
    "  examples\n    total(Counter.zero()) == 0\n  end\n\n"
    "  Counter.count(c)\nend\n\nfn main()\nend\n",
    "gates/counter.mo",
    "module Counter\nexpose Counter, zero, bump, count\n\n"
    "struct Counter\n  n: Int\nend\n\n"
    "fn zero() : Counter\n  Counter(n: 0)\nend\n\n"
    "fn bump(c: Counter) : Counter\n  Counter(n: c.n + 1)\nend\n\n"
    "fn count(c: Counter) : Int\n  c.n\nend\n"};

static const char *const ticking[] = {
    "gates/main.mo", "module Main\n\n"
                     "service\n  state Int = 0\n  tools add\n  every 1.minute tick\n"
                     "  token from env \"TOKEN\"\nend\n\n"
                     "fn add(state: Int, _now: Time, amount: Int) : (Int, Int)\n"
                     "  (state + amount, state)\nend\n\n"
                     "fn tick(state: Int, _now: Time) : Int\n  state + 1\nend\n\n"
                     "fn bump(sim: Simulation) : Result((Int, Int), SimulationError)\n"
                     "  Simulation.send(sim, Main.add, 10)\nend\n\n"
                     "t0 = Time.parse(\"2026-01-01T00:00:00Z\") or stop \"time\"\n\n"
                     "test \"a helper sends while the clock runs\"\n"
                     "  sim = Simulation.start(Main, t0)\n"
                     "  Simulation.advance(sim, 3.minutes)\n"
                     "  assert bump(sim) == Ok((14, 4))\nend\n\n"
                     "fn main()\nend\n"};

static const char *const generic[] = {
    "gates/main.mo", "module Main\n\n"
                     "fn twice(x: T) : List(T)\n"
                     "  promise \"two of them\"\n    result.size == 2\n"
                     "    allows x: 1, result: [1, 1]\n    forbids x: 1, result: [1]\n  end\n"
                     "  [x, x]\nend\n\n"
                     "test \"both types\"\n  assert twice(1) == [1, 1]\n"
                     "  assert twice(\"a\") == [\"a\", \"a\"]\nend\n\n"
                     "fn main()\nend\n"};

static const char *const failing[] = {
    "gates/main.mo", "module Main\n\n"
                     "service\n  state Int = 0\n  tools add\n  token from env \"TOKEN\"\n\n"
                     "  promise \"the state stays small\"\n    after < 50\n"
                     "    allows before: 0, after: 0, now: t0\n"
                     "    forbids before: 0, after: 60, now: t0\n  end\nend\n\n"
                     "fn add(state: Int, _now: Time, amount: Int) : (Int, Int)\n"
                     "  (state + amount.abs, state)\nend\n\n"
                     "fn same(n: Int) : Int\n"
                     "  promise \"stays below a hundred\"\n    result < 100\n"
                     "    allows n: 1, result: 1\n    forbids n: 1, result: 100\n  end\n"
                     "  n\nend\n\n"
                     "t0 = Time.parse(\"2026-01-01T00:00:00Z\") or stop \"time\"\n\n"
                     "fn main()\nend\n"};

struct checked {
    struct mo_arena arena;
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_program *program;
    struct mo_cli cli;
    struct mo_test_selection *selection;
    struct mo_test_totals totals;
    FILE *quiet;
};

static void load(struct checked *c, const char *const *files, uint32_t count) {
    *c = (struct checked){0};
    mo_arena_init(&c->arena, MO_CLI_CHECK_SIZE);
    MO_ASSERT(mo_sources_init(&c->sources) && mo_diags_init(&c->diags));
    for (uint32_t i = 0; i < count; i++) {
        uint32_t file;
        struct mo_source_error error;
        MO_ASSERT(mo_sources_add_bytes(&c->sources, files[2 * i], (const uint8_t *)files[2 * i + 1],
                                       strlen(files[2 * i + 1]), &file, &error));
    }
    struct mo_module_set *set = mo_load_bundle_sources(&c->arena, &c->sources, "lib", &c->diags);
    MO_ASSERT(set != NULL && !mo_diags_any(&c->diags));
    c->program = mo_check(&c->arena, set, &c->diags);
    MO_ASSERT(c->program != NULL && !mo_diags_any(&c->diags));
    const char *const argv[] = {"mo", "test", files[0]};
    const char *error;
    MO_ASSERT(mo_cli_parse(3, argv, &c->cli, &error));
    c->selection = mo_select_tests(&c->arena, c->program, &c->cli, &c->diags);
    MO_ASSERT(c->selection != NULL && c->selection->mistake == NULL);
    c->quiet = fopen("/dev/null", "w");
    MO_ASSERT(c->quiet != NULL);
}

static void run(struct checked *c, struct mo_gated_run *gated) {
    mo_run_gated(&c->cli, &c->arena, c->program, &c->sources, c->selection, c->quiet, &c->diags,
                 &c->totals, gated);
    CHECK(!mo_diags_any(&c->diags) && gated->started);
}

static void unload(struct checked *c) {
    MO_ASSERT(fclose(c->quiet) == 0);
    mo_diags_free(&c->diags);
    mo_sources_free(&c->sources);
    mo_arena_free(&c->arena);
}

static uint32_t function(const struct mo_program *program, const char *module, const char *name) {
    for (uint32_t i = 0; i < program->function_count; i++) {
        const struct mo_symbol *symbol = program->functions[i].signature->function;
        if (strcmp(program->set->names[symbol->module], module) == 0 &&
            strcmp(symbol->name, name) == 0)
            return i;
    }
    MO_ASSERT(false);
    return MO_NO_INDEX;
}

static uint32_t entered_in(const struct mo_program *program, const bool *entered,
                           const char *module) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < program->function_count; i++)
        count += entered[i] &&
                 strcmp(program->set->names[program->functions[i].signature->function->module],
                        module) == 0;
    return count;
}

static const struct mo_gated_check *named_check(const struct mo_gated_run *gated,
                                                const char *name) {
    for (uint32_t i = 0; i < gated->count; i++)
        if (strcmp(gated->checks[i].name, name) == 0)
            return &gated->checks[i];
    MO_ASSERT(false);
    return NULL;
}

static void examples_and_generics(void) {
    struct checked c;
    load(&c, calls, 1);
    struct mo_gated_check checks[] = {{.name = "Main.first.example.1", .limit = UINT64_MAX},
                                      {.name = "Main.second.example.1", .limit = UINT64_MAX},
                                      {.name = "Main.test.1", .limit = UINT64_MAX},
                                      {.name = "Main.test.2", .limit = UINT64_MAX}};
    struct mo_gated_run gated = {
        .checks = checks, .count = 4, .record = true, .start_limit = UINT64_MAX};
    run(&c, &gated);
    uint32_t first = function(c.program, "Main", "first");
    uint32_t second = function(c.program, "Main", "second");
    uint32_t pick = function(c.program, "Main", "pick");
    CHECK(gated.ran == 4);
    for (uint32_t i = 0; i < 4; i++)
        CHECK(checks[i].outcome == MO_CHECK_PASSED && checks[i].steps != 0);
    CHECK(checks[0].entered[first] && entered_in(c.program, checks[0].entered, "Main") == 1);
    CHECK(checks[1].entered[second] && entered_in(c.program, checks[1].entered, "Main") == 1);
    CHECK(checks[2].entered[pick] && entered_in(c.program, checks[2].entered, "Main") == 1);
    CHECK(checks[3].entered[pick] && entered_in(c.program, checks[3].entered, "Main") == 1);
    CHECK(entered_in(c.program, gated.start_entered, "Main") == 0);
    unload(&c);
}

static void generic_promises(void) {
    struct checked c;
    load(&c, generic, 1);
    struct mo_gated_run gated = {.record = true, .start_limit = UINT64_MAX};
    run(&c, &gated);
    uint32_t twice = function(c.program, "Main", "twice");
    const struct mo_gated_check *ints = named_check(&gated, "Main::Main.twice(Int).promise.1");
    const struct mo_gated_check *texts = named_check(&gated, "Main::Main.twice(Text).promise.1");
    CHECK(gated.ran == 3 && ints->outcome == MO_CHECK_PASSED && texts->outcome == MO_CHECK_PASSED);
    CHECK(ints->entered[twice] && texts->entered[twice] && ints->steps != texts->steps);
    struct mo_gated_check one = {.name = "Main::Main.twice(Text).promise.1", .limit = UINT64_MAX};
    struct mo_gated_run planned = {.checks = &one, .count = 1, .start_limit = UINT64_MAX};
    run(&c, &planned);
    CHECK(planned.ran == 1 && one.outcome == MO_CHECK_PASSED && one.steps == texts->steps);
    unload(&c);
}

static void step_limits(void) {
    struct checked c;
    load(&c, calls, 1);
    struct mo_gated_check one = {.name = "Main.test.3", .limit = UINT64_MAX};
    struct mo_gated_run gated = {.checks = &one, .count = 1, .start_limit = UINT64_MAX};
    run(&c, &gated);
    uint64_t steps = one.steps;
    CHECK(gated.ran == 1 && one.outcome == MO_CHECK_PASSED && steps != 0);
    for (uint64_t limit = steps - 1; limit <= steps; limit++) {
        one.limit = limit;
        run(&c, &gated);
        CHECK(gated.ran == 1 &&
              one.outcome == (limit == steps ? MO_CHECK_PASSED : MO_CHECK_OUT_OF_STEPS));
    }
    struct mo_gated_check loops = {.name = "Main.test.6", .limit = 100000};
    gated = (struct mo_gated_run){.checks = &loops, .count = 1, .start_limit = UINT64_MAX};
    run(&c, &gated);
    CHECK(gated.ran == 1 && loops.outcome == MO_CHECK_OUT_OF_STEPS && loops.steps == 100001);
    unload(&c);
}

static void stop_at_first(void) {
    for (unsigned stop = 0; stop < 2; stop++) {
        struct checked c;
        load(&c, calls, 1);
        struct mo_gated_check checks[] = {{.name = "Main.test.3", .limit = UINT64_MAX},
                                          {.name = "Main.test.4", .limit = UINT64_MAX},
                                          {.name = "Main.test.5", .limit = UINT64_MAX}};
        struct mo_gated_run gated = {
            .checks = checks, .count = 3, .stop = stop == 1, .start_limit = UINT64_MAX};
        run(&c, &gated);
        CHECK(checks[0].outcome == MO_CHECK_PASSED && checks[1].outcome == MO_CHECK_FAILED);
        CHECK(gated.ran == (stop == 1 ? 2u : 3u) && c.totals.failed == 1);
        CHECK(c.totals.passed == (stop == 1 ? 1u : 2u));
        unload(&c);
    }
}

static void promise_builders(void) {
    struct checked c;
    load(&c, counter, 2);
    c.cli.seconds = 0;
    struct mo_gated_run gated = {.record = true, .start_limit = UINT64_MAX};
    run(&c, &gated);
    uint32_t zero = function(c.program, "Counter", "zero");
    uint32_t bump = function(c.program, "Counter", "bump");
    uint32_t count = function(c.program, "Counter", "count");
    uint32_t total = function(c.program, "Main", "total");
    const struct mo_gated_check *promise = named_check(&gated, "Main.total.promise.1");
    const struct mo_gated_check *example = named_check(&gated, "Main.total.example.1");
    CHECK(gated.ran == gated.count && strcmp(promise->kind, "promise") == 0);
    CHECK(promise->outcome == MO_CHECK_PASSED && example->outcome == MO_CHECK_PASSED);
    CHECK(promise->entered[zero] && promise->entered[bump] && promise->entered[count] &&
          promise->entered[total]);
    CHECK(!example->entered[bump] && example->entered[zero] && example->entered[total]);
    CHECK(gated.start_entered[zero] && !gated.start_entered[bump] && gated.start_steps != 0);

    struct checked again;
    load(&again, counter, 2);
    again.cli.seconds = 0;
    struct mo_gated_run second = {.record = true, .start_limit = UINT64_MAX};
    run(&again, &second);
    CHECK(second.count == gated.count && second.start_steps == gated.start_steps);
    CHECK(memcmp(second.start_entered, gated.start_entered, c.program->function_count) == 0);
    for (uint32_t i = 0; i < gated.count && i < second.count; i++)
        CHECK(strcmp(second.checks[i].name, gated.checks[i].name) == 0 &&
              second.checks[i].steps == gated.checks[i].steps &&
              memcmp(second.checks[i].entered, gated.checks[i].entered,
                     c.program->function_count) == 0);
    unload(&again);
    unload(&c);
}

static void services(void) {
    struct checked c;
    load(&c, ticking, 1);
    struct mo_gated_run gated = {.record = true, .start_limit = UINT64_MAX};
    run(&c, &gated);
    const struct mo_gated_check *test = named_check(&gated, "Main.test.1");
    const struct mo_gated_check *recovery = named_check(&gated, "Main.service.recovery");
    CHECK(gated.ran == gated.count && test->outcome == MO_CHECK_PASSED &&
          recovery->outcome == MO_CHECK_PASSED && strcmp(recovery->kind, "recovery") == 0);
    CHECK(test->entered[function(c.program, "Main", "tick")] &&
          test->entered[function(c.program, "Main", "add")] &&
          test->entered[function(c.program, "Main", "bump")]);
    CHECK(recovery->entered[function(c.program, "Main", "add")] &&
          !recovery->entered[function(c.program, "Main", "bump")]);
    unload(&c);
}

static void failures_as_found(void) {
    struct checked c;
    load(&c, failing, 1);
    char *text = NULL;
    size_t length = 0;
    MO_ASSERT(fclose(c.quiet) == 0);
    c.quiet = open_memstream(&text, &length);
    MO_ASSERT(c.quiet != NULL);
    struct mo_gated_run gated = {.start_limit = UINT64_MAX};
    mo_run_gated(&c.cli, &c.arena, c.program, &c.sources, c.selection, c.quiet, &c.diags, &c.totals,
                 &gated);
    MO_ASSERT(fflush(c.quiet) == 0);
    CHECK(gated.ran == 3 && gated.checks[0].outcome == MO_CHECK_FAILED &&
          gated.checks[1].outcome == MO_CHECK_FAILED && gated.checks[2].outcome == MO_CHECK_PASSED);
    CHECK(strstr(text, "input n = 9223372036854775807\n") != NULL &&
          strstr(text, "calls: 5\n") != NULL);
    CHECK(strstr(text, "13. add(173)  replied 0, committed  broke") != NULL);
    unload(&c);
    free(text);
}

int main(void) {
    examples_and_generics();
    generic_promises();
    step_limits();
    stop_at_first();
    promise_builders();
    services();
    failures_as_found();
    return mo_test_report("gates");
}
