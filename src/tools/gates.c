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
    FILE *quiet, *output;
    struct mo_diags *diags;
    struct mo_mutant_totals *totals;
};

static const struct mo_span nowhere = {UINT32_MAX, 0, 0};

uint64_t mo_gate_limit(uint64_t steps) {
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
                .name = check->name, .kind = check->kind, .limit = mo_gate_limit(check->steps)};
    }
    return count;
}

bool mo_gate_start_failed(const struct mo_diags *diags) {
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

static enum verdict try_mutant(const struct gate *g, const struct mo_mutant *m, const char **kind,
                               const char **check) {
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
                               .start_limit = mo_gate_limit(g->first->start_steps)};
    struct mo_test_totals totals = {0};
    mo_run_gated(g->cli, &arena, checked, &sources, selection, g->quiet, &diags, &totals, &run);
    if (!run.started && mo_gate_start_failed(&diags)) {
        *kind = "start";
        verdict = KILLED;
    } else if (run.started && run.ran != 0 && run.checks[run.ran - 1].outcome != MO_CHECK_PASSED) {
        *kind = run.checks[run.ran - 1].kind;
        *check = run.checks[run.ran - 1].name;
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

static void count_kill(struct mo_mutant_totals *totals, const char *kind) {
    for (size_t i = 0; i < MO_GATE_KILL_KINDS; i++)
        if (strcmp(kind, kinds[i]) == 0) {
            totals->killed[i]++;
            return;
        }
    MO_UNREACHABLE();
}

static bool describe(struct mo_buffer *out, const struct mo_program *program,
                     const struct mo_mutant *m) {
    bool removed = m->replacement[0] == '\0';
    return mo_append_function_name(out, program, m->function) && mo_buffer_append_text(out, ": ") &&
           mo_buffer_append_text(out, m->kind) && mo_buffer_append_text(out, ", `") &&
           mo_buffer_append_text(out, m->original) &&
           mo_buffer_append_text(out, removed ? "` removed" : "` to `") &&
           (removed ||
            (mo_buffer_append_text(out, m->replacement) && mo_buffer_append_text(out, "`")));
}

static bool json_name(struct mo_buffer *out, const char *name) {
    return name == NULL ? mo_buffer_append_text(out, "null")
                        : mo_render_json_text(out, name, strlen(name));
}

static bool write_mutant(FILE *output, bool json, const struct mo_program *program,
                         const struct mo_sources *sources, const struct mo_mutant *m,
                         const char *outcome, const char *by, const char *check) {
    struct mo_buffer name, out;
    mo_buffer_init(&name, MO_CLI_DIAGNOSTIC_SIZE);
    mo_buffer_init(&out, MO_CLI_DIAGNOSTIC_SIZE);
    const char *path = sources->files[m->span.file].path;
    bool ok;
    if (json) {
        ok = mo_append_function_name(&name, program, m->function) &&
             mo_buffer_append_text(&out, "{\"record\":\"mutant\",\"function\":") &&
             mo_render_json_text(&out, name.bytes, name.length) &&
             mo_buffer_append_text(&out, ",\"file\":") && json_name(&out, path) &&
             mo_buffer_append_text(&out, ",\"line\":") &&
             mo_buffer_append_uint(&out, m->position.line) &&
             mo_buffer_append_text(&out, ",\"column\":") &&
             mo_buffer_append_uint(&out, m->position.column) &&
             mo_buffer_append_text(&out, ",\"kind\":") && json_name(&out, m->kind) &&
             mo_buffer_append_text(&out, ",\"original\":") && json_name(&out, m->original) &&
             mo_buffer_append_text(&out, ",\"replacement\":") && json_name(&out, m->replacement);
        if (outcome != NULL)
            ok = ok && mo_buffer_append_text(&out, ",\"outcome\":") && json_name(&out, outcome) &&
                 mo_buffer_append_text(&out, ",\"by\":") && json_name(&out, by) &&
                 mo_buffer_append_text(&out, ",\"check\":") && json_name(&out, check);
        ok = ok && mo_buffer_append_text(&out, "}\n");
    } else {
        ok = describe(&name, program, m) && mo_buffer_append_text(&out, "mutant ") &&
             mo_render_terminal_text(&out, path, strlen(path)) &&
             mo_buffer_append_text(&out, ":") && mo_buffer_append_uint(&out, m->position.line) &&
             mo_buffer_append_text(&out, ":") && mo_buffer_append_uint(&out, m->position.column) &&
             mo_buffer_append_text(&out, " ") &&
             mo_render_terminal_text(&out, name.bytes, name.length) &&
             mo_buffer_append_text(&out, "\n");
    }
    ok = ok && fwrite(out.bytes, 1, out.length, output) == out.length;
    mo_buffer_free(&name);
    mo_buffer_free(&out);
    return ok;
}

static void survived(const struct gate *g, const struct mo_mutant *m, bool unrun) {
    struct mo_buffer detail;
    mo_buffer_init(&detail, MO_CLI_DIAGNOSTIC_SIZE);
    bool ok = describe(&detail, g->program, m) &&
              (!unrun || mo_buffer_append_text(&detail, "; no check runs this function")) &&
              mo_buffer_append_byte(&detail, 0);
    struct mo_diag diag = {.code = MO_CODE_MUTANT_SURVIVED,
                           .message = "a mutant survived: no check noticed this change",
                           .primary = {m->span, ok ? (const char *)detail.bytes : m->kind}};
    bool added = mo_diags_add(g->diags, &diag);
    (void)added;
    mo_buffer_free(&detail);
}

static bool try_all(struct gate *g, const struct mo_mutants *mutants) {
    for (uint32_t i = 0; i < mutants->count; i++) {
        const struct mo_mutant *m = &mutants->items[i];
        const char *kind = NULL, *check = NULL;
        enum verdict verdict = try_mutant(g, m, &kind, &check);
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
        static const char *const outcomes[] = {"dropped", "killed", "survived", "survived"};
        if (g->cli->json && !write_mutant(g->output, true, g->program, g->sources, m,
                                          outcomes[verdict], kind, check)) {
            mo_test_report_error(g->diags, "a mutant's record could not be written", m->span, NULL);
            return false;
        }
    }
    return true;
}

static void try_mutants(struct gate *g, struct mo_arena *arena, const struct mo_gated_run *first,
                        const struct mo_mutants *mutants) {
    size_t count = first->count == 0 ? 1 : first->count;
    g->sorted =
        mo_arena_allocate_array(arena, count, sizeof *g->sorted, _Alignof(struct mo_gated_check));
    g->plan =
        mo_arena_allocate_array(arena, count, sizeof *g->plan, _Alignof(struct mo_gated_check));
    g->quiet = fopen("/dev/null", "w");
    if (g->sorted == NULL || g->plan == NULL || g->quiet == NULL) {
        mo_test_report_error(g->diags, "the mutants could not be tried", nowhere,
                             g->quiet == NULL ? "could not open /dev/null for their results"
                                              : "they exceed tool resources");
        if (g->quiet != NULL)
            (void)fclose(g->quiet);
        return;
    }
    memcpy(g->sorted, first->checks, first->count * sizeof *g->sorted);
    qsort(g->sorted, first->count, sizeof *g->sorted, cheaper);
    bool finished = try_all(g, mutants);
    (void)fclose(g->quiet);
    if (finished)
        g->totals->ran = true;
}

void mo_gates(const struct mo_cli *cli, struct mo_arena *arena, const struct mo_program *program,
              const struct mo_sources *sources, const struct mo_test_selection *selection,
              FILE *output, struct mo_diags *diags, struct mo_test_totals *totals) {
    struct mo_mutant_totals *mutants = &totals->mutants;
    *mutants = (struct mo_mutant_totals){.asked = true};
    struct mo_gated_run first = {.record = true, .shrink = true, .start_limit = UINT64_MAX};
    mo_run_gated(cli, arena, program, sources, selection, output, diags, totals, &first);
    bool ran = first.started && first.ran == first.count && !mo_diags_any(diags);
    if (!ran || totals->failed != 0 || totals->stopped != 0)
        return;
    struct mo_mutants listed;
    const char *problem = mo_list_mutants(arena, program, sources, &listed);
    if (problem != NULL) {
        mo_test_report_error(diags, "the mutants could not be listed", nowhere, problem);
        return;
    }
    struct gate g = {.cli = cli,
                     .program = program,
                     .sources = sources,
                     .first = &first,
                     .output = output,
                     .diags = diags,
                     .totals = mutants};
    try_mutants(&g, arena, &first, &listed);
}

bool mo_render_gates(struct mo_buffer *out, const struct mo_mutant_totals *mutants, bool json) {
    uint32_t killed = 0;
    for (size_t i = 0; i < MO_GATE_KILL_KINDS; i++)
        killed += mutants->killed[i];
    if (!mutants->ran)
        return json ? mo_buffer_append_text(out, ",\"mutants\":null")
                    : mo_buffer_append_text(out, "mutants: did not run: a check did not pass\n");
    bool ok;
    if (json) {
        ok = mo_buffer_append_text(out, ",\"mutants\":{\"tried\":") &&
             mo_buffer_append_uint(out, mutants->tried) &&
             mo_buffer_append_text(out, ",\"killed\":{");
        for (size_t i = 0; i < MO_GATE_KILL_KINDS; i++)
            ok = ok && mo_buffer_append_text(out, i == 0 ? "\"" : ",\"") &&
                 mo_buffer_append_text(out, kinds[i]) && mo_buffer_append_text(out, "\":") &&
                 mo_buffer_append_uint(out, mutants->killed[i]);
        return ok && mo_buffer_append_text(out, "},\"survived\":") &&
               mo_buffer_append_uint(out, mutants->survived) &&
               mo_buffer_append_text(out, ",\"dropped\":") &&
               mo_buffer_append_uint(out, mutants->dropped) && mo_buffer_append_text(out, "}");
    }
    ok = mo_buffer_append_text(out, "mutants: ") && mo_buffer_append_uint(out, mutants->tried) &&
         mo_buffer_append_text(out, " tried, ") && mo_buffer_append_uint(out, killed) &&
         mo_buffer_append_text(out, " killed (");
    for (size_t i = 0; i < MO_GATE_KILL_KINDS; i++)
        ok = ok && mo_buffer_append_text(out, i == 0 ? "" : ", ") &&
             mo_buffer_append_uint(out, mutants->killed[i]) && mo_buffer_append_text(out, " ") &&
             mo_buffer_append_text(out, kinds[i]);
    return ok && mo_buffer_append_text(out, "), ") &&
           mo_buffer_append_uint(out, mutants->survived) &&
           mo_buffer_append_text(out, " survived, ") &&
           mo_buffer_append_uint(out, mutants->dropped) && mo_buffer_append_text(out, " dropped\n");
}

void mo_gates_list(struct mo_arena *arena, const struct mo_program *program,
                   const struct mo_sources *sources, bool json, FILE *output,
                   struct mo_diags *diags) {
    struct mo_mutants mutants;
    const char *problem = mo_list_mutants(arena, program, sources, &mutants);
    if (problem != NULL) {
        mo_test_report_error(diags, "the mutants could not be listed", nowhere, problem);
        return;
    }
    bool ok = true;
    for (uint32_t i = 0; ok && i < mutants.count; i++)
        ok = write_mutant(output, json, program, sources, &mutants.items[i], NULL, NULL, NULL);
    for (uint32_t f = 0; ok && f < program->function_count; f++) {
        const struct mo_symbol *symbol = program->functions[f].signature->function;
        if (!mo_module_tested(program, symbol->module) || symbol->declaration == NULL ||
            symbol->declaration->kind != MO_S_FN)
            continue;
        uint32_t count = 0;
        for (uint32_t i = 0; i < mutants.count; i++)
            count += mutants.items[i].function == f;
        struct mo_buffer name, out;
        mo_buffer_init(&name, MO_CLI_DIAGNOSTIC_SIZE);
        mo_buffer_init(&out, MO_CLI_DIAGNOSTIC_SIZE);
        ok = mo_append_function_name(&name, program, f) &&
             (json ? mo_buffer_append_text(&out, "{\"record\":\"function\",\"name\":") &&
                         mo_render_json_text(&out, name.bytes, name.length) &&
                         mo_buffer_append_text(&out, ",\"mutants\":") &&
                         mo_buffer_append_uint(&out, count) && mo_buffer_append_text(&out, "}\n")
                   : mo_buffer_append_text(&out, "function ") &&
                         mo_render_terminal_text(&out, name.bytes, name.length) &&
                         mo_buffer_append_text(&out, ": ") && mo_buffer_append_uint(&out, count) &&
                         mo_buffer_append_text(&out, count == 1 ? " mutant\n" : " mutants\n")) &&
             fwrite(out.bytes, 1, out.length, output) == out.length;
        mo_buffer_free(&name);
        mo_buffer_free(&out);
    }
    if (!ok)
        mo_test_report_error(diags, "the mutants could not be listed", nowhere,
                             "their records could not be written");
}
