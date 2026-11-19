#define _POSIX_C_SOURCE 200809L

#include "tools/changes.h"

#include "tools/gates.h"
#include "tools/mutants.h"

#include <stdio.h>
#include <string.h>

enum change { NO_LONGER, NEWLY, NEW, NOT_COMPARED, CHANGES };
static const char *const names[CHANGES] = {"no longer true", "newly true", "about something new",
                                           "could not be compared"};
static const char *const keys[CHANGES] = {"no_longer_true", "newly_true", "about_something_new",
                                          "not_compared"};
enum moved { PASSES, FAILS, UNCHECKED };

struct summary {
    const struct mo_cli *cli;
    struct mo_arena *arena;
    const struct mo_version *old, *now;
    FILE *output, *quiet;
    struct mo_buffer buffer;
    uint32_t counts[CHANGES], passing;
    bool ok;
};

static void put(struct summary *s, const char *text) {
    s->ok = s->ok && mo_buffer_append_text(&s->buffer, text);
}

static void put_text(struct summary *s, const char *text) {
    s->ok = s->ok && (s->cli->json ? mo_render_json_text(&s->buffer, text, strlen(text))
                                   : mo_render_terminal_text(&s->buffer, text, strlen(text)));
}

static void put_count(struct summary *s, uint32_t n) {
    s->ok = s->ok && mo_buffer_append_uint(&s->buffer, n);
}

static void flush(struct summary *s) {
    s->ok = s->ok && fwrite(s->buffer.bytes, 1, s->buffer.length, s->output) == s->buffer.length &&
            fflush(s->output) == 0;
    mo_buffer_clear(&s->buffer);
}

static void entry(struct summary *s, enum change change, const char *name, const char *reason) {
    bool json = s->cli->json;
    s->counts[change]++;
    put(s, json ? "{\"record\":\"change\",\"change\":\"" : "change: ");
    put(s, names[change]);
    put(s, json ? "\",\"name\":" : ": ");
    put_text(s, name);
    if (reason != NULL) {
        put(s, json ? ",\"reason\":" : ": ");
        put_text(s, reason);
    }
    put(s, json ? "}\n" : "\n");
    flush(s);
}

static void totals(struct summary *s, const char *digest) {
    if (s->cli->json) {
        put(s, "{\"record\":\"changes\",\"previous\":\"");
        put(s, digest);
        put(s, "\"");
        for (size_t i = 0; i < CHANGES; i++) {
            put(s, ",\"");
            put(s, keys[i]);
            put(s, "\":");
            put_count(s, s->counts[i]);
        }
        put(s, ",\"passing\":");
        put_count(s, s->passing);
        put(s, "}\n");
    } else {
        put(s, "changes against previous version ");
        put(s, digest);
        put(s, ": ");
        for (size_t i = 0; i < CHANGES; i++) {
            put_count(s, s->counts[i]);
            put(s, " ");
            put(s, names[i]);
            put(s, ", ");
        }
        put_count(s, s->passing);
        put(s, " pass on both\n");
    }
    flush(s);
}

static const char *joined(struct summary *s, const char *a, const char *b, const char *c) {
    size_t x = strlen(a), y = strlen(b), z = strlen(c);
    char *out = mo_arena_allocate(s->arena, x + y + z + 1, 1);
    if (out == NULL)
        return a;
    memcpy(out, a, x);
    memcpy(out + x, b, y);
    memcpy(out + x + y, c, z + 1);
    return out;
}

static const char *first_diagnostic(struct summary *s, const struct mo_diags *diags) {
    if (diags->count == 0)
        return "it could not be run";
    char code[16];
    int n = snprintf(code, sizeof code, "E%04u: ", (unsigned)diags->items[0].code);
    MO_ASSERT(n >= 0);
    return joined(s, code, diags->items[0].message, "");
}

static const char *absent(struct summary *s, const char *what, const struct mo_version *to) {
    return joined(s, what, " is not in the ", to == s->now ? "new version" : "previous version");
}

static uint32_t module_named(const struct mo_program *program, const char *name) {
    for (uint32_t m = 0; m < program->set->count; m++)
        if (strcmp(program->set->names[m], name) == 0)
            return m;
    return UINT32_MAX;
}

static uint32_t function_named(struct summary *s, const struct mo_program *program,
                               const char *name) {
    for (uint32_t i = 0; i < program->function_count; i++) {
        const char *other = mo_function_name(s->arena, program, i);
        if (other != NULL && strcmp(other, name) == 0)
            return i;
    }
    return UINT32_MAX;
}

static uint32_t line_after(const struct mo_source *source, uint32_t at) {
    while (at < source->length && source->bytes[at] != '\n')
        at++;
    return at < source->length ? at + 1 : UINT32_MAX;
}

static bool raw(struct mo_buffer *out, const struct mo_version *from,
                const struct mo_syntax *syntax) {
    const struct mo_source *source = &from->sources->files[syntax->span.file];
    return mo_buffer_append(out, source->bytes + syntax->span.start,
                            syntax->span.end - syntax->span.start);
}

static const char *place_service(struct summary *s, const struct mo_version *from,
                                 const struct mo_observation *o, const struct mo_version *to,
                                 uint32_t *module, uint32_t *at, struct mo_buffer *text) {
    const char *name = from->program->set->names[o->service->module];
    *module = module_named(to->program, name);
    for (uint32_t i = 0; *module != UINT32_MAX && i < to->program->service_count; i++) {
        const struct mo_checked_service *service = &to->program->services[i];
        if (service->module != *module)
            continue;
        const struct mo_syntax *clauses = service->syntax->children[0];
        *at = line_after(&to->sources->files[service->syntax->span.file],
                         clauses->children[clauses->count - 1]->span.end - 1);
        if (o->promise == NULL)
            return "a recovery check is compared only by its name";
        return raw(text, from, o->promise->spec.syntax) && mo_buffer_append_text(text, "\n")
                   ? NULL
                   : "it could not be placed";
    }
    return absent(s, joined(s, name, "'s service", ""), to);
}

static const char *place(struct summary *s, const struct mo_version *from,
                         const struct mo_observation *o, const struct mo_version *to,
                         uint32_t *module, uint32_t *at, struct mo_buffer *text) {
    if (o->source == NULL)
        return place_service(s, from, o, to, module, at, text);
    const struct mo_checked_spec *spec = o->source;
    if (spec->kind == MO_SPEC_TEST) {
        const char *name = from->program->set->names[spec->module];
        *module = module_named(to->program, name);
        if (*module == UINT32_MAX)
            return absent(s, joined(s, "module ", name, ""), to);
        *at = to->sources->files[to->program->set->modules[*module]->span.file].length;
        return mo_buffer_append_text(text, "\n") && raw(text, from, spec->syntax) &&
                       mo_buffer_append_text(text, "\n")
                   ? NULL
                   : "it could not be placed";
    }
    const char *owner = mo_function_name(s->arena, from->program, spec->subject);
    uint32_t function = owner == NULL ? UINT32_MAX : function_named(s, to->program, owner);
    if (function == UINT32_MAX)
        return owner == NULL ? "it could not be placed" : absent(s, owner, to);
    const struct mo_symbol *symbol = to->program->functions[function].signature->function;
    const struct mo_syntax *fn = symbol->declaration;
    *module = symbol->module;
    uint32_t header = fn->span.start;
    for (uint32_t i = 0; i < 4; i++)
        if (fn->children[i] != NULL)
            header = fn->children[i]->span.end;
    *at = line_after(&to->sources->files[fn->span.file], header);
    if (*at > fn->span.end)
        return "it could not be placed";
    bool example = spec->kind == MO_SPEC_EXAMPLE;
    return (!example || mo_buffer_append_text(text, "examples\n")) &&
                   raw(text, from, spec->syntax) &&
                   mo_buffer_append_text(text, example ? "\nend\n" : "\n")
               ? NULL
               : "it could not be placed";
}

static bool inserted(const struct mo_observation *o, uint32_t file, uint32_t start, uint32_t end) {
    const struct mo_checked_spec *spec = o->source != NULL    ? o->source
                                         : o->promise != NULL ? &o->promise->spec
                                                              : NULL;
    return spec != NULL && spec->syntax->span.file == file && spec->syntax->span.start >= start &&
           spec->syntax->span.start < end;
}

static enum moved run_moved(struct summary *s, struct mo_arena *arena,
                            const struct mo_program *program, const struct mo_sources *sources,
                            const struct mo_version *to, uint64_t steps, uint32_t file,
                            uint32_t start, uint32_t end, struct mo_diags *diags,
                            const char **reason) {
    struct mo_test_selection *selection = mo_select_tests(arena, program, s->cli, diags);
    if (selection == NULL) {
        *reason = first_diagnostic(s, diags);
        return UNCHECKED;
    }
    uint32_t count = 0;
    for (uint32_t i = 0; i < selection->count; i++)
        if (inserted(&selection->items[i], file, start, end))
            selection->items[count++] = selection->items[i];
    selection->count = count;
    if (count == 0) {
        *reason = "it could not be placed";
        return UNCHECKED;
    }
    struct mo_gated_run run = {.start_limit = mo_gate_limit(to->run->start_steps),
                               .limit = mo_gate_limit(steps)};
    struct mo_test_totals totals = {0};
    mo_run_gated(s->cli, arena, program, sources, selection, s->quiet, diags, &totals, &run);
    if (!run.started && mo_gate_start_failed(diags))
        return FAILS;
    if (!run.started || run.ran != run.count) {
        *reason = first_diagnostic(s, diags);
        return UNCHECKED;
    }
    for (uint32_t c = 0; c < run.ran; c++)
        if (run.checks[c].outcome != MO_CHECK_PASSED)
            return FAILS;
    return PASSES;
}

static enum moved move(struct summary *s, const struct mo_version *from, uint32_t k,
                       const struct mo_version *to, const char **reason) {
    const struct mo_gated_check *check = &from->run->checks[k];
    struct mo_buffer text;
    mo_buffer_init(&text, MO_LIMIT_FILE_SIZE);
    uint32_t module = 0, at = 0;
    *reason = place(s, from, check->observation, to, &module, &at, &text);
    struct mo_arena arena;
    mo_arena_init(&arena, MO_CLI_CHECK_SIZE);
    struct mo_sources sources;
    struct mo_diags diags;
    bool ready = mo_sources_init(&sources);
    ready = mo_diags_init(&diags) && ready;
    enum moved result = UNCHECKED;
    if (*reason != NULL)
        goto done;
    uint32_t end = at + (uint32_t)text.length;
    if (!ready || !mo_buffer_append_byte(&text, 0)) {
        *reason = "it could not be placed";
        goto done;
    }
    struct mo_mutant insert = {.module = module,
                               .span = {to->program->set->modules[module]->span.file, at, at},
                               .replacement = (const char *)text.bytes};
    if (!mo_mutant_sources(to->program, to->sources, &insert, &sources)) {
        *reason = "it could not be placed";
        goto done;
    }
    struct mo_module_set *set =
        mo_load_program_sources(&arena, &sources, to->program->set->library, &diags);
    struct mo_program *checked =
        set == NULL || mo_diags_any(&diags) ? NULL : mo_check(&arena, set, &diags);
    if (checked == NULL || mo_diags_any(&diags)) {
        *reason = first_diagnostic(s, &diags);
        goto done;
    }
    result = run_moved(s, &arena, checked, &sources, to, check->steps,
                       checked->set->modules[module]->span.file, at, end, &diags, reason);
done:
    mo_diags_free(&diags);
    mo_sources_free(&sources);
    mo_arena_free(&arena);
    mo_buffer_free(&text);
    return result;
}

static bool passed(const struct mo_version *version, uint32_t k) {
    return version->run->checks[k].outcome == MO_CHECK_PASSED;
}

static size_t stem(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot == NULL ? 0 : (size_t)(dot - name);
}

static bool pairs(const struct mo_version *old, uint32_t k, const struct mo_version *now,
                  uint32_t m, bool named) {
    const char *a = old->run->checks[k].name, *b = now->run->checks[m].name;
    size_t length = stem(a);
    return strcmp(old->run->checks[k].kind, now->run->checks[m].kind) == 0 &&
           mo_same_text(old->code.checks[k].text, now->code.checks[m].text) &&
           (named ? strcmp(a, b) == 0 : length == stem(b) && memcmp(a, b, length) == 0);
}

static void compare(struct summary *s, const uint32_t *partner) {
    const struct mo_version *old = s->old, *now = s->now;
    for (uint32_t k = 0; s->ok && k < old->run->count; k++) {
        const char *reason = NULL;
        uint32_t m = partner[k];
        if (m != UINT32_MAX) {
            const char *name = now->run->checks[m].name;
            const char *helper = mo_changed_helper(&old->code.checks[k], &now->code.checks[m]);
            if (helper != NULL)
                entry(s, NOT_COMPARED, name, joined(s, "it reads ", helper, ", which changed"));
            else if (passed(old, k) && passed(now, m))
                s->passing++;
            else if (passed(now, m))
                entry(s, NEWLY, name, NULL);
            else if (passed(old, k))
                entry(s, NO_LONGER, name, NULL);
            continue;
        }
        const char *name = old->run->checks[k].name;
        if (!passed(old, k)) {
            entry(s, NOT_COMPARED, name, "the previous version did not pass it");
            continue;
        }
        switch (move(s, old, k, now, &reason)) {
        case PASSES:
            s->passing++;
            break;
        case FAILS:
            entry(s, NO_LONGER, name, NULL);
            break;
        case UNCHECKED:
            entry(s, NOT_COMPARED, name, reason);
            break;
        }
    }
}

static void compare_new(struct summary *s, const bool *taken) {
    const struct mo_version *old = s->old, *now = s->now;
    for (uint32_t m = 0; s->ok && m < now->run->count; m++) {
        if (taken[m] || !passed(now, m))
            continue;
        const char *reason = NULL;
        const char *name = now->run->checks[m].name;
        switch (move(s, now, m, old, &reason)) {
        case PASSES:
            s->passing++;
            break;
        case FAILS:
            entry(s, NEWLY, name, NULL);
            break;
        case UNCHECKED:
            entry(s, NEW, name, NULL);
            break;
        }
    }
}

void mo_changes(const struct mo_cli *cli, struct mo_arena *arena, const struct mo_version *old,
                const char *digest, const struct mo_version *now, FILE *output,
                struct mo_diags *diags) {
    struct summary s = {.cli = cli,
                        .arena = arena,
                        .old = old,
                        .now = now,
                        .output = output,
                        .quiet = fopen("/dev/null", "w"),
                        .ok = true};
    uint32_t a = old->run->count, b = now->run->count;
    uint32_t *partner =
        mo_arena_allocate_array(arena, a == 0 ? 1 : a, sizeof *partner, _Alignof(uint32_t));
    bool *taken = mo_arena_allocate_array(arena, b == 0 ? 1 : b, sizeof *taken, _Alignof(bool));
    if (s.quiet == NULL || partner == NULL || taken == NULL) {
        mo_test_report_error(diags, "the change summary could not be made",
                             (struct mo_span){UINT32_MAX, 0, 0}, "it exceeds tool resources");
        if (s.quiet != NULL)
            (void)fclose(s.quiet);
        return;
    }
    memset(taken, 0, (b == 0 ? 1 : b) * sizeof *taken);
    for (uint32_t k = 0; k < a; k++)
        partner[k] = UINT32_MAX;
    for (int named = 1; named >= 0; named--)
        for (uint32_t k = 0; k < a; k++)
            for (uint32_t m = 0; partner[k] == UINT32_MAX && m < b; m++)
                if (!taken[m] && pairs(old, k, now, m, named)) {
                    partner[k] = m;
                    taken[m] = true;
                }
    mo_buffer_init(&s.buffer, MO_CLI_DIAGNOSTIC_SIZE);
    compare(&s, partner);
    compare_new(&s, taken);
    if (s.ok)
        totals(&s, digest);
    mo_buffer_free(&s.buffer);
    (void)fclose(s.quiet);
    if (!s.ok)
        mo_test_report_error(diags, "the change summary could not be written",
                             (struct mo_span){UINT32_MAX, 0, 0}, NULL);
}
