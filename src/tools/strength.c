#define _POSIX_C_SOURCE 200809L

#include "tools/strength.h"

#include "base/codes.h"
#include "base/sha256.h"
#include "check/check.h"
#include "tools/offline.h"
#include "tools/tests.h"
#include "tools/upgrade.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum outcome {
    INVALID,
    CHANGES_INTENT,
    KILLED_BY_PROMISE,
    KILLED_BY_EXAMPLE,
    KILLED_BY_BOTH,
    KILLED_BY_TEST,
    SURVIVED,
    DEFECT,
    UNFINISHED,
    NOT_COUNTED,
    OUTCOMES
};

static const char *const names[OUTCOMES] = {
    "invalid",        "changes_intent", "killed_by_promise", "killed_by_example",
    "killed_by_both", "killed_by_test", "survived",          "defect",
    "unfinished",     "not_counted"};

static const char *const sentences[OUTCOMES] = {"is invalid",
                                                "changes intent",
                                                "is killed by a promise",
                                                "is killed by an example",
                                                "is killed by a promise and an example",
                                                "is killed by a test",
                                                "survives",
                                                "stops on a defect",
                                                "did not finish its plan",
                                                "is not counted"};

struct version {
    char *name;
    uint8_t sha256[32];
    bool listed;
    enum outcome outcome;
    char reason[160];
};

struct set {
    uint32_t count;
    struct version *versions;
    bool counted;
};

struct measure {
    const struct mo_strength_run *run;
    const uint8_t *digests;
    uint32_t digest_count;
    FILE *quiet;
};

static int by_name(const void *a, const void *b) {
    return strcmp(((const struct version *)a)->name, ((const struct version *)b)->name);
}

static int by_digest(const void *a, const void *b) {
    return memcmp(a, b, 32);
}

static bool path(char *out, const char *folder, const char *name) {
    int n = snprintf(out, MO_LIMIT_PATH_SIZE + 1, "%s/%s", folder, name);
    return n >= 0 && n <= MO_LIMIT_PATH_SIZE;
}

static const char *read_counted(struct set *set, const char *folder) {
    char file[MO_LIMIT_PATH_SIZE + 1];
    if (!path(file, folder, "counted"))
        return "--strength names a folder whose path is too long";
    FILE *in = fopen(file, "r");
    if (in == NULL)
        return errno == ENOENT ? NULL : "the counted file could not be read";
    set->counted = true;
    const char *mistake = NULL;
    char line[NAME_MAX + 2];
    while (mistake == NULL && fgets(line, sizeof line, in) != NULL) {
        size_t length = strlen(line);
        if (length == 0 || line[length - 1] != '\n') {
            mistake = "a line of the counted file is too long or does not end";
            break;
        }
        line[length - 1] = '\0';
        struct version key = {.name = line};
        struct version *v = bsearch(&key, set->versions, set->count, sizeof key, by_name);
        if (v == NULL || v->listed)
            mistake = "the counted file names a wrong version the folder lacks, or names one twice";
        else
            v->listed = true;
    }
    if (ferror(in) && mistake == NULL)
        mistake = "the counted file could not be read";
    (void)fclose(in);
    return mistake;
}

static const char *read_set(struct mo_arena *arena, const char *folder, struct set *set) {
    *set = (struct set){0};
    DIR *dir = opendir(folder);
    if (dir == NULL)
        return "--strength names a folder that could not be read";
    set->versions = mo_arena_allocate_array(arena, MO_LIMIT_STRENGTH_VERSIONS,
                                            sizeof(struct version), _Alignof(struct version));
    const char *mistake = set->versions == NULL ? "wrong versions exceed tool resources" : NULL;
    for (struct dirent *entry; mistake == NULL && (entry = readdir(dir)) != NULL;) {
        size_t length = strlen(entry->d_name);
        if (length < 4 || strcmp(entry->d_name + length - 3, ".mo") != 0)
            continue;
        if (set->count == MO_LIMIT_STRENGTH_VERSIONS)
            mistake = "the folder holds more wrong versions than mo test --strength measures";
        else {
            char *name = mo_arena_copy(arena, entry->d_name, length + 1, 1);
            if (name == NULL)
                mistake = "wrong versions exceed tool resources";
            else
                set->versions[set->count++] = (struct version){.name = name};
        }
    }
    (void)closedir(dir);
    if (mistake == NULL && set->count == 0)
        mistake = "--strength names a folder with no wrong versions (.mo files)";
    if (mistake != NULL)
        return mistake;
    qsort(set->versions, set->count, sizeof *set->versions, by_name);
    return read_counted(set, folder);
}

static void reason(struct version *v, const char *text) {
    int n = snprintf(v->reason, sizeof v->reason, "%s", text);
    MO_ASSERT(n >= 0);
}

static bool named(const struct mo_syntax *a, const struct mo_syntax *b) {
    return a != NULL && b != NULL && a->span.end - a->span.start == b->span.end - b->span.start &&
           memcmp(a->text, b->text, a->span.end - a->span.start) == 0;
}

static uint32_t body_start(const struct mo_syntax *fn) {
    uint32_t at = fn->span.start;
    for (uint32_t i = 0; i < 5; i++) {
        const struct mo_syntax *part = fn->children[i];
        if (part != NULL && part->kind == MO_S_SEQUENCE && i == 4)
            for (uint32_t j = 0; j < part->count; j++)
                at = part->children[j]->span.end > at ? part->children[j]->span.end : at;
        else if (part != NULL && part->span.end > at)
            at = part->span.end;
    }
    return at;
}

struct splice {
    uint32_t start, end;
    const uint8_t *bytes;
    uint32_t length;
};

static int by_start(const void *a, const void *b) {
    const struct splice *x = a, *y = b;
    return (x->start > y->start) - (x->start < y->start);
}

static bool is_declaration(const struct mo_syntax *n) {
    return n->kind == MO_S_FN || n->kind == MO_S_CONST || n->kind == MO_S_STRUCT ||
           n->kind == MO_S_ENUM;
}

static bool module_named(const struct mo_syntax *path, const char *name) {
    size_t at = 0, length = strlen(name);
    for (uint32_t i = 0; path != NULL && i < path->count; i++) {
        const struct mo_syntax *part = path->children[i];
        size_t n = part->span.end - part->span.start;
        if (i != 0 && (at >= length || name[at++] != '.'))
            return false;
        if (at + n > length || memcmp(name + at, part->text, n) != 0)
            return false;
        at += n;
    }
    return path != NULL && path->count != 0 && at == length;
}

static bool apply(const struct measure *m, struct mo_arena *arena, struct version *v,
                  const struct mo_sources *wrong, const struct mo_module_syntax *replacement,
                  struct mo_sources *out) {
    const struct mo_program *p = m->run->program;
    const struct mo_module_set *set = p->set;
    uint32_t module = 0;
    while (module < set->count &&
           (set->library[module] || !module_named(replacement->header.path, set->names[module])))
        module++;
    if (module == set->count) {
        reason(v, "its module header names none of the program's modules");
        return false;
    }
    uint32_t file = set->modules[module]->span.file;
    struct mo_diags diags;
    if (!mo_diags_init(&diags))
        return false;
    struct mo_module_syntax *original = mo_parse(arena, m->run->sources, file, &diags);
    bool parsed = original != NULL && !mo_diags_any(&diags);
    mo_diags_free(&diags);
    const struct mo_syntax *items = replacement->tree->children[1];
    const struct mo_syntax *own = parsed ? original->tree->children[1] : NULL;
    struct splice *splices = mo_arena_allocate_array(arena, items->count == 0 ? 1 : items->count,
                                                     sizeof *splices, _Alignof(struct splice));
    if (own == NULL || splices == NULL)
        return false;
    if (items->count == 0) {
        reason(v, "it replaces no declarations");
        return false;
    }
    for (uint32_t i = 0; i < items->count; i++) {
        const struct mo_syntax *item = items->children[i];
        if (!is_declaration(item)) {
            reason(v, "it holds something other than functions, constants and types");
            return false;
        }
        const struct mo_syntax *target = NULL;
        uint32_t matches = 0;
        for (uint32_t j = 0; j < own->count; j++)
            if (own->children[j]->kind == item->kind &&
                named(own->children[j]->children[0], item->children[0]) && matches++ == 0)
                target = own->children[j];
        for (uint32_t j = 0; j < i; j++)
            if (named(items->children[j]->children[0], item->children[0]))
                matches = 0;
        if (matches != 1) {
            reason(v, "a declaration it replaces is missing, overloaded or replaced twice");
            return false;
        }
        bool body = item->kind == MO_S_FN;
        uint32_t from = body ? body_start(item) : item->span.start;
        splices[i] =
            (struct splice){body ? body_start(target) : target->span.start, target->span.end,
                            wrong->files[0].bytes + from, item->span.end - from};
    }
    qsort(splices, items->count, sizeof *splices, by_start);
    const struct mo_source *source = &m->run->sources->files[file];
    struct mo_buffer bytes;
    mo_buffer_init(&bytes, MO_LIMIT_FILE_SIZE);
    bool ok = true;
    uint32_t at = 0;
    for (uint32_t i = 0; i < items->count; i++) {
        ok = ok && mo_buffer_append(&bytes, source->bytes + at, splices[i].start - at) &&
             mo_buffer_append(&bytes, splices[i].bytes, splices[i].length);
        at = splices[i].end;
    }
    ok = ok && mo_buffer_append(&bytes, source->bytes + at, source->length - at);
    for (uint32_t k = 0; ok && k < set->count; k++) {
        if (set->library[k])
            continue;
        const struct mo_source *f = &m->run->sources->files[set->modules[k]->span.file];
        uint32_t index;
        struct mo_source_error error;
        ok = k == module
                 ? mo_sources_add_bytes(out, f->path, bytes.bytes, bytes.length, &index, &error)
                 : mo_sources_add_bytes(out, f->path, f->bytes, f->length, &index, &error);
    }
    mo_buffer_free(&bytes);
    if (!ok)
        reason(v, "the replaced program exceeds the source limits");
    return ok;
}

static struct mo_test_selection *checks(struct mo_arena *arena, const struct mo_program *program,
                                        const struct mo_cli *cli, struct mo_diags *diags) {
    struct mo_test_selection *selection = mo_select_tests(arena, program, cli, diags);
    if (selection != NULL)
        selection->mistake = NULL;
    return selection;
}

static enum outcome classify(const struct mo_test_totals *totals, const struct mo_diags *diags) {
    bool promise = totals->failed > totals->failed_examples + totals->failed_tests,
         example = totals->failed_examples != 0;
    if (promise && example)
        return KILLED_BY_BOTH;
    if (promise)
        return KILLED_BY_PROMISE;
    if (example)
        return KILLED_BY_EXAMPLE;
    bool unfinished = false;
    for (uint32_t i = 0; i < diags->count; i++)
        unfinished = unfinished || diags->items[i].code != MO_CODE_TOOL_TEST_INITIALIZATION;
    if (unfinished || diags->dropped != 0)
        return UNFINISHED;
    if (totals->stopped > totals->stopped_tests)
        return DEFECT;
    if (totals->failed_tests != 0 || totals->stopped_tests != 0)
        return KILLED_BY_TEST;
    return diags->count != 0 ? DEFECT : SURVIVED;
}

struct reach {
    bool *fns;
    bool *consts;
    bool **types;
};

static void mark_type(const struct mo_program *p, const struct mo_source_type *t, struct reach *r) {
    if (t == NULL)
        return;
    if (t->kind == MO_SOURCE_NAMED && t->declaration != NULL && t->module < p->set->count &&
        !p->set->library[t->module]) {
        const struct mo_module_scope *scope = &p->interfaces->scopes[t->module];
        ptrdiff_t idx = t->declaration - scope->symbols;
        if (idx >= 0 && (uint32_t)idx < scope->count)
            r->types[t->module][idx] = true;
    }
    for (uint32_t i = 0; i < t->count; i++)
        mark_type(p, t->arguments[i], r);
}

static void mark_symbol(const struct mo_program *p, const struct mo_symbol *sym,
                        uint32_t subject_fn, struct reach *r, uint32_t *work_fns,
                        uint32_t *work_fn_n, uint32_t *work_consts, uint32_t *work_const_n) {
    if (sym == NULL || sym->module >= p->set->count || p->set->library[sym->module])
        return;
    for (uint32_t f = 0; f < p->function_count; f++) {
        if (p->functions[f].signature->function == sym) {
            if (f != subject_fn && !r->fns[f]) {
                r->fns[f] = true;
                work_fns[(*work_fn_n)++] = f;
            }
            return;
        }
    }
    for (uint32_t c = 0; c < p->constant_count; c++) {
        if (p->constants[c].symbol == sym) {
            if (!r->consts[c]) {
                r->consts[c] = true;
                work_consts[(*work_const_n)++] = c;
            }
            return;
        }
    }
    const struct mo_module_scope *scope = &p->interfaces->scopes[sym->module];
    uint32_t target = sym->owner != UINT32_MAX ? sym->owner : (uint32_t)(sym - scope->symbols);
    if (target < scope->count)
        r->types[sym->module][target] = true;
}

static void mark_pattern(const struct mo_program *p, const struct mo_checked_pattern *pat,
                         uint32_t subject_fn, struct reach *r, uint32_t *work_fns,
                         uint32_t *work_fn_n, uint32_t *work_consts, uint32_t *work_const_n) {
    if (pat == NULL)
        return;
    mark_type(p, pat->type, r);
    mark_symbol(p, pat->constructor.symbol, subject_fn, r, work_fns, work_fn_n, work_consts,
                work_const_n);
    for (uint32_t i = 0; i < pat->part_count; i++)
        mark_pattern(p, pat->parts[i].pattern, subject_fn, r, work_fns, work_fn_n, work_consts,
                     work_const_n);
}

static void scan_body(const struct mo_program *p, const struct mo_checked_body *body,
                      uint32_t subject_fn, struct reach *r, uint32_t *work_fns, uint32_t *work_fn_n,
                      uint32_t *work_consts, uint32_t *work_const_n) {
    for (uint32_t i = 0; i < body->count; i++) {
        const struct mo_checked_expression *e = &body->items[i];
        mark_type(p, e->type, r);
        mark_symbol(p, e->symbol, subject_fn, r, work_fns, work_fn_n, work_consts, work_const_n);
        mark_pattern(p, e->pattern, subject_fn, r, work_fns, work_fn_n, work_consts, work_const_n);
        if (e->match != NULL)
            for (uint32_t a = 0; a < e->match->arm_count; a++)
                for (uint32_t k = 0; k < e->match->arms[a].pattern_count; k++)
                    mark_pattern(p, e->match->arms[a].patterns[k], subject_fn, r, work_fns,
                                 work_fn_n, work_consts, work_const_n);
    }
}

static void hash_str(struct mo_sha256 *h, const char *s) {
    size_t n = strlen(s);
    uint8_t len[4] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
    mo_sha256_update(h, len, 4);
    mo_sha256_update(h, s, n);
}

static void hash_bytes(struct mo_sha256 *h, const uint8_t *b, size_t n) {
    uint8_t len[4] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
    mo_sha256_update(h, len, 4);
    if (n != 0)
        mo_sha256_update(h, b, n);
}

static void hash_fn_without_specs(struct mo_sha256 *h, const struct mo_sources *sources,
                                  const struct mo_syntax *decl) {
    const struct mo_source *src = &sources->files[decl->span.file];
    const struct mo_syntax *specs = decl->children[4];
    if (specs == NULL || specs->count == 0) {
        hash_bytes(h, src->bytes + decl->span.start, decl->span.end - decl->span.start);
        return;
    }
    uint32_t first_spec = specs->children[0]->span.start;
    uint32_t last_spec = specs->children[specs->count - 1]->span.end;
    hash_bytes(h, src->bytes + decl->span.start, first_spec - decl->span.start);
    hash_bytes(h, src->bytes + last_spec, decl->span.end - last_spec);
}

static bool spec_digests(struct mo_arena *arena, const struct mo_program *program,
                         const struct mo_sources *sources, uint8_t **out_digests,
                         uint32_t *out_count) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < program->spec_count; i++) {
        const struct mo_checked_spec *s = &program->specs[i];
        if (!program->set->library[s->module] &&
            (s->kind == MO_SPEC_PROMISE || s->kind == MO_SPEC_EXAMPLE))
            total++;
    }
    for (uint32_t i = 0; i < program->service_count; i++)
        if (!program->set->library[program->services[i].module])
            total += program->services[i].promise_count;
    *out_count = total;
    uint8_t *digests = mo_arena_allocate_array(arena, total == 0 ? 1 : total, 32, 1);
    if (digests == NULL)
        return false;
    if (total == 0) {
        *out_digests = digests;
        return true;
    }
    uint32_t fn_cap = program->function_count == 0 ? 1 : program->function_count;
    uint32_t const_cap = program->constant_count == 0 ? 1 : program->constant_count;
    uint32_t mod_cap = program->set->count == 0 ? 1 : program->set->count;
    struct reach r = {
        .fns = mo_arena_allocate_array(arena, fn_cap, sizeof(bool), _Alignof(bool)),
        .consts = mo_arena_allocate_array(arena, const_cap, sizeof(bool), _Alignof(bool)),
        .types = mo_arena_allocate_array(arena, mod_cap, sizeof(bool *), _Alignof(bool *)),
    };
    uint32_t *work_fns =
        mo_arena_allocate_array(arena, fn_cap, sizeof(uint32_t), _Alignof(uint32_t));
    uint32_t *work_consts =
        mo_arena_allocate_array(arena, const_cap, sizeof(uint32_t), _Alignof(uint32_t));
    if (r.fns == NULL || r.consts == NULL || r.types == NULL || work_fns == NULL ||
        work_consts == NULL)
        return false;
    for (uint32_t m = 0; m < program->set->count; m++) {
        uint32_t sc = program->interfaces->scopes[m].count;
        r.types[m] = mo_arena_allocate_array(arena, sc == 0 ? 1 : sc, sizeof(bool), _Alignof(bool));
        if (r.types[m] == NULL)
            return false;
    }
    uint32_t at = 0;
    for (uint32_t i = 0; i < program->spec_count + program->service_count; i++) {
        bool is_service_group = i >= program->spec_count;
        uint32_t svc_idx = is_service_group ? i - program->spec_count : 0;
        uint32_t sub_count = is_service_group ? program->services[svc_idx].promise_count : 1;
        if (is_service_group && program->set->library[program->services[svc_idx].module])
            continue;
        for (uint32_t sub = 0; sub < sub_count; sub++) {
            const struct mo_checked_service_promise *sp =
                is_service_group ? &program->services[svc_idx].promises[sub] : NULL;
            const struct mo_checked_spec *s = is_service_group ? &sp->spec : &program->specs[i];
            if (!is_service_group && (program->set->library[s->module] ||
                                      (s->kind != MO_SPEC_PROMISE && s->kind != MO_SPEC_EXAMPLE)))
                continue;
            memset(r.fns, 0, fn_cap * sizeof(bool));
            memset(r.consts, 0, const_cap * sizeof(bool));
            for (uint32_t m = 0; m < program->set->count; m++)
                memset(r.types[m], 0,
                       (program->interfaces->scopes[m].count == 0
                            ? 1
                            : program->interfaces->scopes[m].count) *
                           sizeof(bool));
            uint32_t wf_n = 0, wc_n = 0;
            uint32_t subject_fn = is_service_group ? UINT32_MAX : s->subject;
            scan_body(program, &s->body, subject_fn, &r, work_fns, &wf_n, work_consts, &wc_n);
            if (!is_service_group && s->kind == MO_SPEC_PROMISE) {
                for (uint32_t a = 0; a < program->spec_count; a++)
                    if (program->specs[a].kind == MO_SPEC_ANCHOR && program->specs[a].promise == i)
                        scan_body(program, &program->specs[a].body, subject_fn, &r, work_fns, &wf_n,
                                  work_consts, &wc_n);
            } else if (sp != NULL) {
                for (uint32_t a = 0; a < sp->anchor_count; a++)
                    scan_body(program, &sp->anchors[a].body, subject_fn, &r, work_fns, &wf_n,
                              work_consts, &wc_n);
            }
            uint32_t fi = 0, ci = 0;
            while (fi < wf_n || ci < wc_n) {
                while (fi < wf_n) {
                    uint32_t f = work_fns[fi++];
                    scan_body(program, &program->functions[f].body, subject_fn, &r, work_fns, &wf_n,
                              work_consts, &wc_n);
                }
                while (ci < wc_n) {
                    uint32_t c = work_consts[ci++];
                    scan_body(program, &program->constants[c].initializer, subject_fn, &r, work_fns,
                              &wf_n, work_consts, &wc_n);
                }
            }
            struct mo_sha256 h;
            mo_sha256_init(&h);
            uint8_t kind_tag = is_service_group ? 3 : (uint8_t)s->kind;
            mo_sha256_update(&h, &kind_tag, 1);
            hash_str(&h, program->set->names[s->module]);
            if (!is_service_group && s->subject != UINT32_MAX)
                hash_str(&h, program->functions[s->subject].signature->function->name);
            hash_bytes(&h, s->text.bytes, s->text.length);
            for (uint32_t f = 0; f < program->function_count; f++)
                if (r.fns[f]) {
                    const struct mo_symbol *sym = program->functions[f].signature->function;
                    hash_str(&h, program->set->names[sym->module]);
                    hash_fn_without_specs(&h, sources, sym->declaration);
                }
            for (uint32_t c = 0; c < program->constant_count; c++)
                if (r.consts[c]) {
                    const struct mo_symbol *sym = program->constants[c].symbol;
                    const struct mo_source *src = &sources->files[sym->declaration->span.file];
                    hash_str(&h, program->set->names[sym->module]);
                    hash_bytes(&h, src->bytes + sym->declaration->span.start,
                               sym->declaration->span.end - sym->declaration->span.start);
                }
            for (uint32_t m = 0; m < program->set->count; m++) {
                const struct mo_module_scope *scope = &program->interfaces->scopes[m];
                for (uint32_t k = 0; k < scope->count; k++)
                    if (r.types[m][k] && scope->symbols[k].declaration != NULL) {
                        const struct mo_syntax *decl = scope->symbols[k].declaration;
                        const struct mo_source *src = &sources->files[decl->span.file];
                        hash_str(&h, program->set->names[m]);
                        hash_bytes(&h, src->bytes + decl->span.start,
                                   decl->span.end - decl->span.start);
                    }
            }
            mo_sha256_finish(&h, digests + 32 * (size_t)at++);
        }
    }
    MO_ASSERT(at == total);
    qsort(digests, total, 32, by_digest);
    *out_digests = digests;
    return true;
}

static bool same_intent(const struct measure *m, struct mo_arena *arena,
                        const struct mo_program *program, const struct mo_sources *sources,
                        bool *same) {
    uint8_t *digests = NULL;
    uint32_t count = 0;
    if (!spec_digests(arena, program, sources, &digests, &count))
        return false;
    *same = count == m->digest_count && memcmp(digests, m->digests, 32 * (size_t)count) == 0;
    return true;
}

static void measure_one(const struct measure *m, struct version *v, bool counted) {
    const struct mo_strength_run *run = m->run;
    struct mo_arena arena;
    mo_arena_init(&arena, MO_CLI_CHECK_SIZE);
    struct mo_sources wrong, program;
    struct mo_diags diags;
    bool ready = mo_sources_init(&wrong);
    ready = mo_sources_init(&program) && ready;
    ready = mo_diags_init(&diags) && ready;
    char file[MO_LIMIT_PATH_SIZE + 1];
    uint32_t index = 0;
    struct mo_source_error error;
    v->outcome = INVALID;
    if (!ready || !path(file, run->cli->strength, v->name) ||
        !mo_sources_add(&wrong, file, &index, &error)) {
        reason(v, "it could not be read");
        goto done;
    }
    struct mo_sha256 hash;
    mo_sha256_init(&hash);
    mo_sha256_update(&hash, wrong.files[0].bytes, wrong.files[0].length);
    mo_sha256_finish(&hash, v->sha256);
    if (!counted) {
        v->outcome = NOT_COUNTED;
        goto done;
    }
    struct mo_module_syntax *replacement = mo_parse(&arena, &wrong, 0, &diags);
    if (replacement == NULL || mo_diags_any(&diags) || replacement->header.path == NULL) {
        reason(v, replacement != NULL && !mo_diags_any(&diags) ? "it has no module header"
                                                               : "it does not parse");
        goto done;
    }
    if (!apply(m, &arena, v, &wrong, replacement, &program))
        goto done;
    struct mo_module_set *set = mo_load_bundle_with_library(
        &arena, &program, run->library, run->library == NULL ? mo_default_embedded_library() : NULL,
        &diags);
    struct mo_program *checked =
        set == NULL || mo_diags_any(&diags) ? NULL : mo_check(&arena, set, &diags);
    if (checked == NULL || mo_diags_any(&diags)) {
        int n = diags.count != 0 ? snprintf(v->reason, sizeof v->reason, "E%04u: %s",
                                            (unsigned)diags.items[0].code, diags.items[0].message)
                                 : snprintf(v->reason, sizeof v->reason, "it does not check");
        MO_ASSERT(n >= 0);
        goto done;
    }
    bool same = false;
    if (!same_intent(m, &arena, checked, &program, &same)) {
        v->outcome = UNFINISHED;
        goto done;
    }
    if (!same) {
        v->outcome = CHANGES_INTENT;
        goto done;
    }
    struct mo_test_selection *selection = checks(&arena, checked, run->cli, &diags);
    struct mo_test_totals totals = {0};
    if (selection != NULL)
        mo_run_tests(run->cli, &arena, checked, &program, selection, m->quiet, &diags, &totals);
    v->outcome = classify(&totals, &diags);
done:
    mo_diags_free(&diags);
    mo_sources_free(&program);
    mo_sources_free(&wrong);
    mo_arena_free(&arena);
}

static bool render(const struct mo_strength_run *run, const struct set *set, bool original,
                   struct mo_buffer *out) {
    uint32_t counts[OUTCOMES] = {0};
    uint32_t killed = 0, any = 0, scored = 0;
    for (uint32_t i = 0; original && i < set->count; i++) {
        const struct version *v = &set->versions[i];
        counts[v->outcome]++;
        bool kill = v->outcome == KILLED_BY_PROMISE || v->outcome == KILLED_BY_EXAMPLE ||
                    v->outcome == KILLED_BY_BOTH;
        killed += kill;
        any += kill || v->outcome == KILLED_BY_TEST;
        scored += kill || v->outcome == KILLED_BY_TEST || v->outcome == SURVIVED ||
                  (v->listed && (v->outcome == CHANGES_INTENT || v->outcome == INVALID));
    }
    bool json = run->cli->json, ok = true;
    char rate[32] = "null";
    if (scored != 0) {
        int n = snprintf(rate, sizeof rate, json ? "%.4f" : "%.1f%%",
                         (json ? 1.0 : 100.0) * (double)killed / (double)scored);
        MO_ASSERT(n > 0 && (size_t)n < sizeof rate);
    }
    if (json) {
        ok = mo_buffer_append_text(out, "{\"record\":\"strength\",\"version\":2,\"set\":") &&
             mo_render_json_text(out, run->cli->strength, strlen(run->cli->strength)) &&
             mo_buffer_append_text(out, ",\"counted_file\":") &&
             mo_buffer_append_text(out, set->counted ? "true" : "false") &&
             mo_buffer_append_text(out, ",\"original\":") &&
             mo_buffer_append_text(out, original ? "\"passed\"" : "\"failed\"") &&
             mo_buffer_append_text(out, ",\"versions\":[");
        for (uint32_t i = 0; original && i < set->count; i++) {
            const struct version *v = &set->versions[i];
            ok = ok && mo_buffer_append_text(out, i == 0 ? "{\"name\":" : ",{\"name\":") &&
                 mo_render_json_text(out, v->name, strlen(v->name)) &&
                 mo_buffer_append_text(out, ",\"sha256\":\"");
            ok = ok && mo_offline_hex(out, v->sha256, 32) &&
                 mo_buffer_append_text(out, "\",\"outcome\":\"") &&
                 mo_buffer_append_text(out, names[v->outcome]) && mo_buffer_append_text(out, "\"");
            if (v->outcome == INVALID)
                ok = ok && mo_buffer_append_text(out, ",\"reason\":") &&
                     mo_render_json_text(out, v->reason, strlen(v->reason));
            ok = ok && mo_buffer_append_text(out, "}");
        }
        ok = ok && mo_buffer_append_text(out, "],\"counts\":{");
        for (uint32_t i = 0; i < OUTCOMES; i++)
            ok = ok && mo_buffer_append_text(out, i == 0 ? "\"" : ",\"") &&
                 mo_buffer_append_text(out, names[i]) && mo_buffer_append_text(out, "\":") &&
                 mo_buffer_append_uint(out, counts[i]);
        ok = ok && mo_buffer_append_text(out, "},\"killed\":") &&
             mo_buffer_append_uint(out, killed) && mo_buffer_append_text(out, ",\"killed_any\":") &&
             mo_buffer_append_uint(out, any) && mo_buffer_append_text(out, ",\"scored\":") &&
             mo_buffer_append_uint(out, scored) && mo_buffer_append_text(out, ",\"rate\":") &&
             mo_buffer_append_text(out, rate) && mo_buffer_append_text(out, ",\"partial\":") &&
             mo_buffer_append_text(out, original && counts[UNFINISHED] == 0 ? "false" : "true") &&
             mo_buffer_append_text(out, "}\n");
        return ok;
    }
    if (!original)
        return mo_buffer_append_text(out, "strength: the program fails its own checks, so no "
                                          "wrong version was measured\n");
    for (uint32_t i = 0; i < set->count; i++) {
        const struct version *v = &set->versions[i];
        ok = ok && mo_buffer_append_text(out, "strength: ") &&
             mo_render_terminal_text(out, v->name, strlen(v->name)) &&
             mo_buffer_append_text(out, " ") && mo_buffer_append_text(out, sentences[v->outcome]);
        if (v->outcome == INVALID)
            ok = ok && mo_buffer_append_text(out, ": ") &&
                 mo_render_terminal_text(out, v->reason, strlen(v->reason));
        ok = ok && mo_buffer_append_text(out, "\n");
    }
    ok = ok && mo_buffer_append_text(out, "strength: ") && mo_buffer_append_uint(out, killed) &&
         mo_buffer_append_text(out, " of ") && mo_buffer_append_uint(out, scored) &&
         mo_buffer_append_text(out, " wrong versions killed (") &&
         mo_buffer_append_text(out, scored == 0 ? "no rate" : rate) &&
         mo_buffer_append_text(out, "), ") && mo_buffer_append_uint(out, any) &&
         mo_buffer_append_text(out, " by any check; ");
    for (uint32_t i = 0; i < OUTCOMES; i++)
        if (i == INVALID || i == CHANGES_INTENT || i == DEFECT || i == UNFINISHED ||
            i == NOT_COUNTED)
            ok = ok && mo_buffer_append_uint(out, counts[i]) && mo_buffer_append_text(out, " ") &&
                 mo_buffer_append_text(out, names[i]) &&
                 mo_buffer_append_text(out, i == NOT_COUNTED ? "\n" : ", ");
    return ok;
}

static void unfinished(struct mo_diags *diags, const char *detail) {
    struct mo_diag diag = {.code = MO_CODE_PLAN_UNFINISHED,
                           .message = "the plan did not finish",
                           .primary = {{UINT32_MAX, 0, 0}, detail}};
    bool added = mo_diags_add(diags, &diag);
    (void)added;
}

void mo_strength(const struct mo_strength_run *run) {
    struct set set;
    *run->mistake = read_set(run->arena, run->cli->strength, &set);
    if (*run->mistake != NULL)
        return;
    uint8_t *digests = NULL;
    uint32_t digest_count = 0;
    if (!spec_digests(run->arena, run->program, run->sources, &digests, &digest_count)) {
        mo_test_report_error(run->diags, "spec digests exceed tool resources",
                             (struct mo_span){UINT32_MAX, 0, 0}, NULL);
        return;
    }
    if (digest_count == 0) {
        *run->mistake = "the program has no promises or examples for --strength to measure";
        return;
    }
    struct mo_test_selection *selection = checks(run->arena, run->program, run->cli, run->diags);
    if (selection == NULL)
        return;
    if (run->cli->json && !(*run->version = mo_write_version(run->output, run->cli->command)))
        return;
    uint32_t before = run->diags->count;
    mo_run_tests(run->cli, run->arena, run->program, run->sources, selection, run->output,
                 run->diags, run->totals);
    bool original = run->totals->failed == 0 && run->totals->stopped == 0 &&
                    run->diags->count == before && run->diags->dropped == 0;
    struct measure m = {run, digests, digest_count, NULL};
    if (original) {
        m.quiet = fopen("/dev/null", "w");
        if (m.quiet == NULL) {
            mo_test_report_error(run->diags, "could not open /dev/null for wrong versions' results",
                                 (struct mo_span){UINT32_MAX, 0, 0}, NULL);
            return;
        }
        for (uint32_t i = 0; i < set.count; i++)
            measure_one(&m, &set.versions[i], !set.counted || set.versions[i].listed);
        (void)fclose(m.quiet);
    }
    struct mo_buffer out;
    mo_buffer_init(&out, MO_CLI_DIAGNOSTIC_SIZE);
    bool rendered = render(run, &set, original, &out);
    if (!rendered || fwrite(out.bytes, 1, out.length, run->output) != out.length ||
        fflush(run->output) != 0)
        mo_test_report_error(run->diags, "the strength record could not be written",
                             (struct mo_span){UINT32_MAX, 0, 0}, NULL);
    mo_buffer_free(&out);
    uint32_t open = 0;
    for (uint32_t i = 0; original && i < set.count; i++)
        open += set.versions[i].outcome == UNFINISHED;
    if (!original && run->diags->count != before)
        unfinished(run->diags, "the measured program's own checks did not finish");
    else if (open != 0)
        unfinished(run->diags, "a wrong version's checks did not finish, so the rate is partial");
}
