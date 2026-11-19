#define _POSIX_C_SOURCE 200809L

#include "tools/previous.h"

#include "base/path.h"
#include "base/sha256.h"
#include "tools/mutants.h"
#include "tools/upgrade.h"

#include <stdio.h>
#include <string.h>

static const struct mo_span nowhere = {UINT32_MAX, 0, 0};

static bool refuse(struct mo_diags *diags, struct mo_arena *arena, const char *message,
                   const struct mo_diags *old) {
    char detail[512] = "";
    if (old->count != 0) {
        int n = snprintf(detail, sizeof detail, "E%04u: %s", (unsigned)old->items[0].code,
                         old->items[0].message);
        MO_ASSERT(n >= 0);
    }
    mo_test_report_error(diags, message, nowhere,
                         detail[0] == '\0' ? NULL
                                           : mo_arena_copy(arena, detail, strlen(detail) + 1, 1));
    return false;
}

static const char *library_of(const char *against, const struct mo_program *program,
                              char folder[MO_LIMIT_PATH_SIZE + 1]) {
    char relative[MO_LIMIT_PATH_SIZE + 1];
    const char *name = program->set->names[0];
    size_t n = strlen(against), r = 0;
    bool named = mo_module_path((const uint8_t *)name, strlen(name), relative) &&
                 (r = strlen(relative)) <= n && strcmp(against + n - r, relative) == 0 &&
                 (n == r || against[n - r - 1] == '/');
    if (named)
        n -= r;
    else {
        const char *slash = strrchr(against, '/');
        n = slash == NULL ? 0 : (size_t)(slash - against) + 1;
    }
    if (n == 0)
        return ".";
    if (n > MO_LIMIT_PATH_SIZE)
        return NULL;
    memcpy(folder, against, n);
    folder[n] = '\0';
    return folder;
}

static bool digest(struct mo_previous *old, const struct mo_module_set *set) {
    struct mo_buffer bundle;
    mo_buffer_init(&bundle, MO_LIMIT_BUNDLE_BYTES);
    bool ok = mo_source_bundle(set, &old->sources, true, &bundle, &old->diags);
    if (ok) {
        uint8_t hash[32];
        struct mo_sha256 h;
        mo_sha256_init(&h);
        mo_sha256_update(&h, bundle.bytes, bundle.length);
        mo_sha256_finish(&h, hash);
        static const char digits[] = "0123456789abcdef";
        for (size_t i = 0; i < 32; i++) {
            old->digest[2 * i] = digits[hash[i] >> 4];
            old->digest[2 * i + 1] = digits[hash[i] & 15];
        }
        old->digest[64] = '\0';
    }
    mo_buffer_free(&bundle);
    return ok;
}

static bool run(struct mo_previous *old, const struct mo_cli *cli) {
    struct mo_test_selection *selection =
        mo_select_tests(&old->arena, old->version.program, cli, &old->diags);
    if (selection == NULL)
        return false;
    if (selection->count == 0) {
        old->run.started = true;
        return true;
    }
    FILE *quiet = fopen("/dev/null", "w");
    if (quiet == NULL)
        return false;
    struct mo_test_totals totals = {0};
    mo_run_gated(cli, &old->arena, old->version.program, &old->sources, selection, quiet,
                 &old->diags, &totals, &old->run);
    return fclose(quiet) == 0;
}

bool mo_previous_open(struct mo_previous *old, const struct mo_cli *cli, struct mo_arena *arena,
                      const struct mo_program *program, const char *library,
                      struct mo_diags *diags) {
    mo_arena_init(&old->arena, MO_CLI_CHECK_SIZE);
    bool ready = mo_sources_init(&old->sources);
    ready = mo_diags_init(&old->diags) && ready;
    old->run = (struct mo_gated_run){.record = true, .start_limit = UINT64_MAX};
    old->version = (struct mo_version){.sources = &old->sources, .run = &old->run};
    if (!ready)
        return refuse(diags, arena, "the --against program could not be read", &old->diags);
    char folder[MO_LIMIT_PATH_SIZE + 1];
    if (program->set->library[0])
        library = library_of(cli->against, program, folder);
    struct mo_module_set *set =
        mo_load_program(&old->arena, &old->sources, cli->against, library, &old->diags);
    const struct mo_program *checked =
        set == NULL || mo_diags_any(&old->diags) ? NULL : mo_check(&old->arena, set, &old->diags);
    if (checked == NULL || mo_diags_any(&old->diags))
        return refuse(diags, arena, "the --against program does not check", &old->diags);
    old->version.program = checked;
    if (!digest(old, set))
        return refuse(diags, arena, "the --against program could not be bundled", &old->diags);
    if (!run(old, cli) || !old->run.started || old->run.ran != old->run.count ||
        mo_diags_any(&old->diags))
        return refuse(diags, arena, "the --against program's checks did not run", &old->diags);
    if (!mo_intent_code(&old->arena, checked, old->run.checks, old->run.count, &old->version.code))
        return refuse(diags, arena, "the --against program's code could not be read", &old->diags);
    return true;
}

void mo_previous_close(struct mo_previous *old) {
    mo_diags_free(&old->diags);
    mo_sources_free(&old->sources);
    mo_arena_free(&old->arena);
}

char *mo_function_name(struct mo_arena *arena, const struct mo_program *program,
                       uint32_t function) {
    struct mo_buffer buffer;
    mo_buffer_init(&buffer, MO_CLI_DIAGNOSTIC_SIZE);
    char *name =
        mo_append_function_name(&buffer, program, function) && mo_buffer_append_byte(&buffer, 0)
            ? mo_arena_copy(arena, buffer.bytes, buffer.length, 1)
            : NULL;
    mo_buffer_free(&buffer);
    return name;
}

bool mo_same_text(struct mo_intent_text a, struct mo_intent_text b) {
    return a.length == b.length && (a.length == 0 || memcmp(a.bytes, b.bytes, a.length) == 0);
}

static const struct mo_intent_read *read_named(const struct mo_check_text *check,
                                               const char *label) {
    for (uint32_t i = 0; i < check->read_count; i++)
        if (strcmp(check->reads[i].label, label) == 0)
            return &check->reads[i];
    return NULL;
}

const char *mo_changed_helper(const struct mo_check_text *a, const struct mo_check_text *b) {
    for (uint32_t i = 0; i < a->read_count; i++) {
        const struct mo_intent_read *other = read_named(b, a->reads[i].label);
        if (other == NULL || !mo_same_text(a->reads[i].text, other->text))
            return a->reads[i].label;
    }
    for (uint32_t i = 0; i < b->read_count; i++)
        if (read_named(a, b->reads[i].label) == NULL)
            return b->reads[i].label;
    return NULL;
}

static bool coded(const struct mo_program *program, uint32_t function) {
    const struct mo_symbol *symbol = program->functions[function].signature->function;
    return mo_module_tested(program, symbol->module) && symbol->declaration != NULL &&
           symbol->declaration->kind == MO_S_FN;
}

static bool same_check(const struct mo_version *old, uint32_t k, const struct mo_version *now,
                       uint32_t m) {
    const struct mo_check_text *a = &old->code.checks[k], *b = &now->code.checks[m];
    return strcmp(old->run->checks[k].name, now->run->checks[m].name) == 0 &&
           mo_same_text(a->text, b->text) && mo_changed_helper(a, b) == NULL;
}

static bool runs(const struct mo_gated_run *run, uint32_t check, uint32_t function) {
    return run->checks[check].entered[function] || run->start_entered[function];
}

bool *mo_previous_select(struct mo_arena *arena, const struct mo_version *old,
                         const struct mo_version *now) {
    const struct mo_program *p = now->program, *q = old->program;
    const struct mo_gated_run *a = old->run, *b = now->run;
    bool *tried = mo_arena_allocate_array(arena, p->function_count == 0 ? 1 : p->function_count,
                                          sizeof *tried, _Alignof(bool));
    uint32_t *partner = mo_arena_allocate_array(arena, a->count == 0 ? 1 : a->count,
                                                sizeof *partner, _Alignof(uint32_t));
    char **names = mo_arena_allocate_array(arena, q->function_count == 0 ? 1 : q->function_count,
                                           sizeof *names, _Alignof(char *));
    if (tried == NULL || partner == NULL || names == NULL)
        return NULL;
    for (uint32_t k = 0; k < a->count; k++) {
        partner[k] = UINT32_MAX;
        for (uint32_t m = 0; partner[k] == UINT32_MAX && m < b->count; m++)
            if (same_check(old, k, now, m))
                partner[k] = m;
    }
    for (uint32_t j = 0; j < q->function_count; j++) {
        names[j] = NULL;
        if (coded(q, j) && (names[j] = mo_function_name(arena, q, j)) == NULL)
            return NULL;
    }
    for (uint32_t i = 0; i < p->function_count; i++) {
        tried[i] = false;
        if (!coded(p, i))
            continue;
        char *name = mo_function_name(arena, p, i);
        if (name == NULL)
            return NULL;
        uint32_t j = 0;
        while (j < q->function_count && (names[j] == NULL || strcmp(names[j], name) != 0))
            j++;
        tried[i] =
            j == q->function_count || !mo_same_text(now->code.functions[i], old->code.functions[j]);
        bool ran = false;
        for (uint32_t k = 0; !tried[i] && k < a->count; k++)
            if (runs(a, k, j)) {
                ran = true;
                tried[i] = partner[k] == UINT32_MAX || !runs(b, partner[k], i);
            }
        tried[i] = tried[i] || !ran;
    }
    return tried;
}
