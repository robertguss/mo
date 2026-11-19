#define _POSIX_C_SOURCE 200809L

#include "tools/gates.h"

#include "base/codes.h"
#include "tools/mutants.h"

#include <stdlib.h>
#include <string.h>

enum verdict { DROPPED, KILLED, SURVIVED, UNRUN, BROKEN };

struct gate {
    const struct mo_cli *cli;
    const struct mo_program *program;
    const struct mo_sources *sources;
    const struct mo_gated_run *first;
    struct mo_gated_check *sorted, *plan;
    FILE *quiet;
    struct mo_diags *diags;
    struct mo_gate_totals *totals;
};

static const struct mo_span nowhere = {UINT32_MAX, 0, 0};

static uint64_t limit(uint64_t steps) {
    uint64_t scaled = steps > UINT64_MAX / MO_LIMIT_GATE_STEP_MULTIPLE
                          ? UINT64_MAX
                          : steps * MO_LIMIT_GATE_STEP_MULTIPLE;
    return scaled < MO_LIMIT_GATE_STEP_FLOOR ? MO_LIMIT_GATE_STEP_FLOOR : scaled;
}

static int cheaper(const void *a, const void *b) {
    const struct mo_gated_check *x = a, *y = b;
    if (x->steps != y->steps)
        return x->steps < y->steps ? -1 : 1;
    return strcmp(x->name, y->name);
}

static uint32_t plan(const struct gate *g, uint32_t function) {
    bool every = g->first->start_entered[function];
    uint32_t count = 0;
    for (uint32_t i = 0; i < g->first->count; i++) {
        const struct mo_gated_check *check = &g->sorted[i];
        if (every || check->entered[function])
            g->plan[count++] = (struct mo_gated_check){
                .name = check->name, .kind = check->kind, .limit = limit(check->steps)};
    }
    return count;
}

static bool started_badly(const struct mo_diags *diags) {
    for (uint32_t i = 0; i < diags->count; i++)
        if (diags->items[i].code != MO_CODE_TOOL_ANCHOR &&
            diags->items[i].code != MO_CODE_TOOL_TEST_INITIALIZATION)
            return false;
    return diags->count != 0 && diags->dropped == 0;
}

static void broken(const struct gate *g, const struct mo_mutant *m, const struct mo_diags *diags,
                   const char *detail) {
    if (diags->count != 0)
        detail = diags->items[0].message;
    mo_test_report_error(g->diags, "a mutant could not be tried", m->span, detail);
}

static enum verdict try_mutant(const struct gate *g, const struct mo_mutant *m, const char **kind) {
    uint32_t count = plan(g, m->function);
    struct mo_arena arena;
    mo_arena_init(&arena, MO_CLI_CHECK_SIZE);
    struct mo_sources sources;
    struct mo_diags diags;
    bool ready = mo_sources_init(&sources);
    ready = mo_diags_init(&diags) && ready;
    enum verdict verdict = BROKEN;
    if (!ready || !mo_mutant_sources(g->program, g->sources, m, &sources)) {
        broken(g, m, &diags, "its sources could not be built");
        goto done;
    }
    struct mo_module_set *set =
        mo_load_program_sources(&arena, &sources, g->program->set->library, &diags);
    if (set == NULL || mo_diags_any(&diags)) {
        broken(g, m, &diags, "it could not be loaded");
        goto done;
    }
    struct mo_program *checked = mo_check(&arena, set, &diags);
    if (checked == NULL || mo_diags_any(&diags)) {
        verdict = DROPPED;
        goto done;
    }
    if (count == 0 && !g->first->start_entered[m->function]) {
        verdict = UNRUN;
        goto done;
    }
    struct mo_test_selection *selection = mo_select_tests(&arena, checked, g->cli, &diags);
    if (selection == NULL) {
        broken(g, m, &diags, "its checks could not be selected");
        goto done;
    }
    struct mo_gated_run run = {.checks = g->plan,
                               .count = count,
                               .stop = true,
                               .start_limit = limit(g->first->start_steps)};
    struct mo_test_totals totals = {0};
    mo_run_gated(g->cli, &arena, checked, &sources, selection, g->quiet, &diags, &totals, &run);
    if (!run.started && started_badly(&diags)) {
        *kind = "start";
        verdict = KILLED;
    } else if (run.started && run.ran != 0 && run.checks[run.ran - 1].outcome != MO_CHECK_PASSED) {
        *kind = run.checks[run.ran - 1].kind;
        verdict = KILLED;
    } else if (run.started && !mo_diags_any(&diags) && run.ran == run.count)
        verdict = SURVIVED;
    else
        broken(g, m, &diags, "its checks did not finish");
done:
    mo_diags_free(&diags);
    mo_sources_free(&sources);
    mo_arena_free(&arena);
    return verdict;
}

static const char *const kinds[MO_GATE_KILL_KINDS] = {"promise", "example", "test", "recovery",
                                                      "start"};

static void count_kill(struct mo_gate_totals *totals, const char *kind) {
    for (size_t i = 0; i < MO_GATE_KILL_KINDS; i++)
        if (strcmp(kind, kinds[i]) == 0) {
            totals->killed[i]++;
            return;
        }
    MO_UNREACHABLE();
}

static void survived(const struct gate *g, const struct mo_mutant *m, bool unrun) {
    struct mo_buffer detail;
    mo_buffer_init(&detail, MO_CLI_DIAGNOSTIC_SIZE);
    bool removed = m->replacement[0] == '\0';
    bool ok =
        mo_append_function_name(&detail, g->program, m->function) &&
        mo_buffer_append_text(&detail, ": ") && mo_buffer_append_text(&detail, m->kind) &&
        mo_buffer_append_text(&detail, ", `") && mo_buffer_append_text(&detail, m->original) &&
        mo_buffer_append_text(&detail, removed ? "` removed" : "` to `") &&
        (removed ||
         (mo_buffer_append_text(&detail, m->replacement) && mo_buffer_append_text(&detail, "`"))) &&
        (!unrun || mo_buffer_append_text(&detail, "; no check runs this function")) &&
        mo_buffer_append_byte(&detail, 0);
    struct mo_diag diag = {.code = MO_CODE_MUTANT_SURVIVED,
                           .message = "a mutant survived: no check noticed this change",
                           .primary = {m->span, ok ? (const char *)detail.bytes : m->kind}};
    bool added = mo_diags_add(g->diags, &diag);
    (void)added;
    mo_buffer_free(&detail);
}

static bool exposed(const struct mo_program *program, uint32_t function) {
    const struct mo_symbol *symbol = program->functions[function].signature->function;
    return symbol->exported && mo_module_tested(program, symbol->module) &&
           function != program->entry.function && symbol->declaration != NULL &&
           symbol->declaration->kind == MO_S_FN;
}

static void unstated(struct mo_diags *diags, const struct mo_program *program, uint32_t function,
                     bool tool) {
    const struct mo_symbol *symbol = program->functions[function].signature->function;
    struct mo_buffer detail;
    mo_buffer_init(&detail, MO_CLI_DIAGNOSTIC_SIZE);
    bool ok = mo_append_function_name(&detail, program, function) &&
              mo_buffer_append_text(&detail, ": ") &&
              (!tool || (mo_buffer_append_text(&detail, "write a service promise on ") &&
                         mo_buffer_append_text(&detail, symbol->name) &&
                         mo_buffer_append_text(&detail, ", or "))) &&
              mo_buffer_append_text(&detail, "give it a promise or an example") &&
              mo_buffer_append_byte(&detail, 0);
    struct mo_diag diag = {.code = MO_CODE_INTENT_UNSTATED,
                           .message = tool ? "a service tool states no intent"
                                           : "an exposed function states no intent",
                           .primary = {symbol->declaration->children[0]->span,
                                       ok ? (const char *)detail.bytes : symbol->name}};
    bool added = mo_diags_add(diags, &diag);
    (void)added;
    mo_buffer_free(&detail);
}

static const char *intent(struct mo_arena *arena, const struct mo_program *program,
                          struct mo_diags *diags) {
    bool *stated = mo_arena_allocate_array(
        arena, program->function_count == 0 ? 1 : program->function_count, 1, _Alignof(bool));
    if (stated == NULL) {
        mo_test_report_error(diags, "stated intent could not be read", nowhere,
                             "it exceeds tool resources");
        return "stated intent could not be read";
    }
    memset(stated, 0, program->function_count);
    for (uint32_t i = 0; i < program->spec_count; i++) {
        const struct mo_checked_spec *spec = &program->specs[i];
        if ((spec->kind == MO_SPEC_PROMISE || spec->kind == MO_SPEC_EXAMPLE) &&
            spec->subject != UINT32_MAX)
            stated[spec->subject] = true;
    }
    bool missing = false;
    for (uint32_t i = 0; i < program->function_count; i++)
        if (exposed(program, i) && !stated[i]) {
            unstated(diags, program, i, false);
            missing = true;
        }
    for (uint32_t i = 0; i < program->service_count; i++) {
        const struct mo_checked_service *service = &program->services[i];
        if (!mo_module_tested(program, service->module))
            continue;
        for (uint32_t t = 0; t < service->tool_count; t++) {
            uint32_t function = service->tools[t].function;
            bool met = stated[function] || exposed(program, function);
            for (uint32_t p = 0; p < service->promise_count; p++)
                met = met || service->promises[p].tool == t;
            if (!met) {
                unstated(diags, program, function, true);
                missing = true;
            }
        }
    }
    return missing ? "a function or tool states no intent" : NULL;
}

static bool try_all(struct gate *g, const struct mo_mutants *mutants) {
    for (uint32_t i = 0; i < mutants->count; i++) {
        const struct mo_mutant *m = &mutants->items[i];
        const char *kind = NULL;
        enum verdict verdict = try_mutant(g, m, &kind);
        switch (verdict) {
        case DROPPED:
            g->totals->dropped++;
            break;
        case KILLED:
            g->totals->tried++;
            count_kill(g->totals, kind);
            break;
        case SURVIVED:
        case UNRUN:
            g->totals->tried++;
            g->totals->survived++;
            survived(g, m, verdict == UNRUN);
            break;
        case BROKEN:
            return false;
        }
    }
    return true;
}

void mo_gates(const struct mo_cli *cli, struct mo_arena *arena, const struct mo_program *program,
              const struct mo_sources *sources, const struct mo_test_selection *selection,
              FILE *output, struct mo_diags *diags, struct mo_test_totals *totals) {
    struct mo_gate_totals *gates = &totals->gates;
    *gates = (struct mo_gate_totals){.asked = true, .skipped = "a check did not pass"};
    struct mo_gated_run first = {.record = true, .shrink = true, .start_limit = UINT64_MAX};
    mo_run_gated(cli, arena, program, sources, selection, output, diags, totals, &first);
    if (!first.started || first.ran != first.count || totals->failed != 0 || totals->stopped != 0 ||
        mo_diags_any(diags))
        return;
    gates->skipped = intent(arena, program, diags);
    if (gates->skipped != NULL)
        return;
    struct mo_mutants mutants;
    const char *problem = mo_list_mutants(arena, program, sources, &mutants);
    gates->skipped = "a mutant could not be tried";
    if (problem != NULL) {
        mo_test_report_error(diags, "the mutants could not be listed", nowhere, problem);
        return;
    }
    size_t count = first.count == 0 ? 1 : first.count;
    struct gate g = {
        .cli = cli,
        .program = program,
        .sources = sources,
        .first = &first,
        .sorted = mo_arena_allocate_array(arena, count, sizeof *g.sorted,
                                          _Alignof(struct mo_gated_check)),
        .plan =
            mo_arena_allocate_array(arena, count, sizeof *g.plan, _Alignof(struct mo_gated_check)),
        .quiet = fopen("/dev/null", "w"),
        .diags = diags,
        .totals = gates,
    };
    if (g.sorted == NULL || g.plan == NULL || g.quiet == NULL) {
        mo_test_report_error(diags, "the mutants could not be tried", nowhere,
                             g.quiet == NULL ? "could not open /dev/null for their results"
                                             : "they exceed tool resources");
        if (g.quiet != NULL)
            (void)fclose(g.quiet);
        return;
    }
    memcpy(g.sorted, first.checks, first.count * sizeof *g.sorted);
    qsort(g.sorted, first.count, sizeof *g.sorted, cheaper);
    bool finished = try_all(&g, &mutants);
    (void)fclose(g.quiet);
    if (finished) {
        gates->ran = true;
        gates->skipped = NULL;
    }
}

bool mo_render_gates(struct mo_buffer *out, const struct mo_gate_totals *gates, bool json) {
    uint32_t killed = 0;
    for (size_t i = 0; i < MO_GATE_KILL_KINDS; i++)
        killed += gates->killed[i];
    if (!gates->ran)
        return json ? mo_buffer_append_text(out, ",\"gates\":\"did not run\"")
                    : mo_buffer_append_text(out, "gates: did not run: ") &&
                          mo_buffer_append_text(out, gates->skipped) &&
                          mo_buffer_append_text(out, "\n");
    bool ok;
    if (json) {
        ok = mo_buffer_append_text(out, ",\"gates\":\"ran\",\"mutants\":{\"tried\":") &&
             mo_buffer_append_uint(out, gates->tried) &&
             mo_buffer_append_text(out, ",\"killed\":{");
        for (size_t i = 0; i < MO_GATE_KILL_KINDS; i++)
            ok = ok && mo_buffer_append_text(out, i == 0 ? "\"" : ",\"") &&
                 mo_buffer_append_text(out, kinds[i]) && mo_buffer_append_text(out, "\":") &&
                 mo_buffer_append_uint(out, gates->killed[i]);
        return ok && mo_buffer_append_text(out, "},\"survived\":") &&
               mo_buffer_append_uint(out, gates->survived) &&
               mo_buffer_append_text(out, ",\"dropped\":") &&
               mo_buffer_append_uint(out, gates->dropped) && mo_buffer_append_text(out, "}");
    }
    ok = mo_buffer_append_text(out, "gates: ") && mo_buffer_append_uint(out, gates->tried) &&
         mo_buffer_append_text(out, " mutants tried, ") && mo_buffer_append_uint(out, killed) &&
         mo_buffer_append_text(out, " killed (");
    for (size_t i = 0; i < MO_GATE_KILL_KINDS; i++)
        ok = ok && mo_buffer_append_text(out, i == 0 ? "" : ", ") &&
             mo_buffer_append_uint(out, gates->killed[i]) && mo_buffer_append_text(out, " ") &&
             mo_buffer_append_text(out, kinds[i]);
    return ok && mo_buffer_append_text(out, "), ") && mo_buffer_append_uint(out, gates->survived) &&
           mo_buffer_append_text(out, " survived, ") &&
           mo_buffer_append_uint(out, gates->dropped) && mo_buffer_append_text(out, " dropped\n");
}
