#include "base/limits.h"
#include "base/stack.h"
#include "check/check.h"
#include "test.h"
#include "tools/mutants.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>

struct loaded {
    struct mo_arena arena;
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_program *program;
    struct mo_mutants mutants;
};

static bool list(struct loaded *l, struct mo_module_set *set) {
    l->program =
        set != NULL && !mo_diags_any(&l->diags) ? mo_check(&l->arena, set, &l->diags) : NULL;
    if (l->program == NULL || mo_diags_any(&l->diags))
        return false;
    return mo_list_mutants(&l->arena, l->program, &l->sources, &l->mutants) == NULL;
}

static void start(struct loaded *l) {
    *l = (struct loaded){0};
    MO_ASSERT(mo_sources_init(&l->sources) && mo_diags_init(&l->diags));
    mo_arena_init(&l->arena, 256 << 20);
}

static bool load_text(struct loaded *l, const char *text) {
    start(l);
    uint32_t file;
    struct mo_source_error error;
    MO_ASSERT(mo_sources_add_bytes(&l->sources, "tests/mutants/main.mo", (const uint8_t *)text,
                                   strlen(text), &file, &error));
    return list(l, mo_load_bundle_sources(&l->arena, &l->sources, "lib", &l->diags));
}

static bool load_path(struct loaded *l, const char *path) {
    start(l);
    return list(l, mo_load_program(&l->arena, &l->sources, path, "lib", &l->diags));
}

static void unload(struct loaded *l) {
    mo_arena_free(&l->arena);
    mo_diags_free(&l->diags);
    mo_sources_free(&l->sources);
}

static bool changed(const struct loaded *l, uint32_t i, const char *expected) {
    struct mo_sources out;
    MO_ASSERT(mo_sources_init(&out));
    const struct mo_mutant *m = &l->mutants.items[i];
    bool same = mo_mutant_sources(l->program, &l->sources, m, &out) &&
                out.files[m->module].length == strlen(expected) &&
                memcmp(out.files[m->module].bytes, expected, strlen(expected)) == 0;
    mo_sources_free(&out);
    return same;
}

static bool loads(const struct loaded *l, uint32_t i, bool check) {
    struct mo_sources out;
    struct mo_diags diags;
    struct mo_arena arena;
    MO_ASSERT(mo_sources_init(&out) && mo_diags_init(&diags));
    mo_arena_init(&arena, 256 << 20);
    bool ok = mo_mutant_sources(l->program, &l->sources, &l->mutants.items[i], &out);
    struct mo_module_set *set =
        ok ? mo_load_program_sources(&arena, &out, l->program->set->library, &diags) : NULL;
    ok = set != NULL && !mo_diags_any(&diags) && set->count == l->program->set->count;
    if (ok && check)
        ok = mo_check(&arena, set, &diags) != NULL && !mo_diags_any(&diags);
    mo_arena_free(&arena);
    mo_diags_free(&diags);
    mo_sources_free(&out);
    return ok;
}

static bool is(const struct mo_mutant *m, const char *kind, const char *original,
               const char *replacement, uint32_t line, uint32_t column) {
    return strcmp(m->kind, kind) == 0 && strcmp(m->original, original) == 0 &&
           strcmp(m->replacement, replacement) == 0 && m->position.line == line &&
           m->position.column == column;
}

#define LESS "module Main\n\nfn less(a: Int, b: Int) : Bool\n  a "

static void comparison(void) {
    struct loaded l;
    CHECK(load_text(&l, LESS "< b\nend\n"));
    CHECK(l.mutants.count == 2);
    if (l.mutants.count == 2) {
        CHECK(is(&l.mutants.items[0], "boundary", "<", "<=", 4, 5));
        CHECK(is(&l.mutants.items[1], "negate", "<", ">=", 4, 5));
        CHECK(l.mutants.items[0].module == 0 && l.mutants.items[0].span.start == 48);
        CHECK(changed(&l, 0, LESS "<= b\nend\n"));
        CHECK(changed(&l, 1, LESS ">= b\nend\n"));
        CHECK(loads(&l, 0, true) && loads(&l, 1, true));
    }
    unload(&l);
}

static void specs(void) {
    struct loaded l;
    CHECK(load_text(&l,
                    "module Main\n\nfn less(a: Int, b: Int) : Bool\n"
                    "  promise \"a smaller left side\"\n"
                    "    not result or a < b\n"
                    "    allows a: 1, b: 2, result: true\n"
                    "    forbids a: 2, b: 1, result: true\n"
                    "  end\n\n"
                    "  examples\n    less(1, 2) == true\n    not less(2, 1)\n  end\n\n"
                    "  a < b\nend\n\n"
                    "test \"one and one\"\n  assert 1 + 1 == 2\nend\n\n"
                    "service\n  state Int = 0\n  tools add\n  token from env \"COUNTER_TOKEN\"\n\n"
                    "  promise \"the count never falls\" on add\n"
                    "    after >= before and true\n"
                    "    allows before: 1, amount: 2, reply: Unit, after: 3, now: t0\n"
                    "    forbids before: 3, amount: 2, reply: Unit, after: 1, now: t0\n"
                    "  end\nend\n\n"
                    "fn add(state: Int, _now: Time, amount: Int) : (Int, Unit)\n"
                    "  (state.max(amount), Unit)\nend\n\n"
                    "t0 = Time.parse(\"2026-01-01T00:00:00Z\") or stop \"time\"\n"));
    CHECK(l.mutants.count == 2);
    for (uint32_t i = 0; i < l.mutants.count; i++)
        CHECK(l.mutants.items[i].position.line == 15);
    unload(&l);
}

static void comments_and_text(void) {
    struct loaded l;
    const char *before = "module Main\n\nfn more(a: Int, b: Int) : Bool\n  a # not a < b\n    ";
    char source[256], boundary[256], negated[256];
    MO_ASSERT(snprintf(source, sizeof source, "%s> b\nend\n", before) > 0);
    MO_ASSERT(snprintf(boundary, sizeof boundary, "%s>= b\nend\n", before) > 0);
    MO_ASSERT(snprintf(negated, sizeof negated, "%s<= b\nend\n", before) > 0);
    CHECK(load_text(&l, source));
    CHECK(l.mutants.count == 2);
    if (l.mutants.count == 2) {
        CHECK(is(&l.mutants.items[0], "boundary", ">", ">=", 5, 5));
        CHECK(changed(&l, 0, boundary) && changed(&l, 1, negated));
    }
    unload(&l);

    CHECK(load_text(&l, "module Main\n\nfn same() : Text\n  \"a == b and not c\"\nend\n\n"
                        "fn greet(name: Text) : Text\n  \"Hello, #{name.trim}!\".trim\nend\n\n"
                        "fn second(pair: (Int, Int)) : Int\n  pair.1\nend\n"));
    CHECK(l.mutants.count == 0);
    unload(&l);
}

static void literals(void) {
    struct loaded l;
    CHECK(load_text(&l, "module Main\n\nfn pick(n: Int, flag: Bool) : Int\n"
                        "  return 0x9F if not flag and false\n"
                        "  return 1 if n is 4\n"
                        "  case n\n    7: 9_999\n    _: 0b1\n  end\nend\n"));
    const char *expected[][3] = {
        {"int", "0x9F", "0xA0"},   {"not", "not", ""}, {"logic", "and", "or"},
        {"bool", "false", "true"}, {"int", "1", "2"},  {"int", "9_999", "10_000"},
        {"int", "0b1", "0b10"},
    };
    CHECK(l.mutants.count == 7);
    for (uint32_t i = 0; i < l.mutants.count && i < 7; i++)
        CHECK(strcmp(l.mutants.items[i].kind, expected[i][0]) == 0 &&
              strcmp(l.mutants.items[i].original, expected[i][1]) == 0 &&
              strcmp(l.mutants.items[i].replacement, expected[i][2]) == 0);
    if (l.mutants.count == 7) {
        CHECK(changed(&l, 1,
                      "module Main\n\nfn pick(n: Int, flag: Bool) : Int\n"
                      "  return 0x9F if  flag and false\n"
                      "  return 1 if n is 4\n"
                      "  case n\n    7: 9_999\n    _: 0b1\n  end\nend\n"));
        CHECK(loads(&l, 1, true));
    }
    unload(&l);
}

static void arithmetic(void) {
    struct loaded l;
    CHECK(
        load_text(&l, "module Main\n\nfn mix(a: Int, b: Int) : Int\n  a + b - a * b / -a\nend\n"));
    const char *expected[][2] = {{"+", "-"}, {"-", "+"}, {"*", "/"}, {"/", "*"}};
    CHECK(l.mutants.count == 4);
    for (uint32_t i = 0; i < l.mutants.count && i < 4; i++)
        CHECK(strcmp(l.mutants.items[i].kind, "arithmetic") == 0 &&
              strcmp(l.mutants.items[i].original, expected[i][0]) == 0 &&
              strcmp(l.mutants.items[i].replacement, expected[i][1]) == 0);
    if (l.mutants.count == 4)
        CHECK(changed(&l, 1,
                      "module Main\n\nfn mix(a: Int, b: Int) : Int\n"
                      "  a + b + a * b / -a\nend\n"));
    unload(&l);
}

static void library(void) {
    struct loaded l;
    CHECK(load_text(&l, "module Main\n\nfn evens(items: List(Int)) : Int\n"
                        "  items.count(fn(n) n % 2 == 0 end)\nend\n"));
    CHECK(l.program == NULL || l.program->set->count > 1);
    CHECK(l.mutants.count == 3);
    for (uint32_t i = 0; i < l.mutants.count; i++)
        CHECK(l.mutants.items[i].module == 0);
    unload(&l);

    CHECK(load_path(&l, "lib/list.mo"));
    CHECK(l.program != NULL && l.program->set->library[0]);
    CHECK(l.mutants.count > 0);
    bool count = false;
    for (uint32_t i = 0; i < l.mutants.count; i++) {
        const struct mo_mutant *m = &l.mutants.items[i];
        CHECK(m->module == 0);
        count = count ||
                strcmp(l.program->functions[m->function].signature->function->name, "count") == 0;
    }
    CHECK(count);
    if (l.mutants.count > 0)
        CHECK(loads(&l, 0, true));
    unload(&l);
}

static void overloads(void) {
    struct loaded l;
    CHECK(load_text(&l, "module Main\n\nstruct Box\n  n: Int\nend\n\n"
                        "struct Pair\n  a: Int\n  b: Int\nend\n\n"
                        "fn total(box: Box) : Int\n  box.n + 1\nend\n\n"
                        "fn total(pair: Pair) : Int\n  pair.a * pair.b\nend\n"));
    CHECK(l.mutants.count == 3);
    if (l.mutants.count == 3) {
        const struct mo_symbol *box =
            l.program->functions[l.mutants.items[0].function].signature->function;
        const struct mo_symbol *pair =
            l.program->functions[l.mutants.items[2].function].signature->function;
        CHECK(l.mutants.items[0].function == l.mutants.items[1].function);
        CHECK(box != pair && strcmp(box->name, "total") == 0 && strcmp(pair->name, "total") == 0);
        const struct mo_symbol *symbols = l.program->interfaces->scopes[0].symbols;
        CHECK(box->receiver != UINT32_MAX && strcmp(symbols[box->receiver].name, "Box") == 0);
        CHECK(pair->receiver != UINT32_MAX && strcmp(symbols[pair->receiver].name, "Pair") == 0);
        CHECK(is(&l.mutants.items[2], "arithmetic", "*", "/", 17, 10));
    }
    unload(&l);
}

static void one_span(const struct loaded *l, uint32_t i) {
    const struct mo_mutant *m = &l->mutants.items[i];
    const struct mo_source *before = &l->sources.files[m->span.file];
    const struct mo_syntax *fn =
        l->program->functions[m->function].signature->function->declaration;
    CHECK(m->module < l->program->set->count && !l->program->set->library[m->module]);
    CHECK(fn->kind == MO_S_FN && m->span.start >= fn->children[5]->span.start &&
          m->span.end <= fn->children[5]->span.end);
    size_t original = strlen(m->original), replacement = strlen(m->replacement);
    CHECK(original == m->span.end - m->span.start &&
          memcmp(before->bytes + m->span.start, m->original, original) == 0);
    CHECK(original != replacement || memcmp(m->original, m->replacement, original) != 0);
    struct mo_sources out;
    MO_ASSERT(mo_sources_init(&out));
    CHECK(mo_mutant_sources(l->program, &l->sources, m, &out));
    const struct mo_source *after = &out.files[m->module];
    CHECK(out.count == l->program->set->count &&
          after->length == before->length - original + replacement &&
          memcmp(after->bytes, before->bytes, m->span.start) == 0 &&
          memcmp(after->bytes + m->span.start, m->replacement, replacement) == 0 &&
          memcmp(after->bytes + m->span.start + replacement, before->bytes + m->span.end,
                 before->length - m->span.end) == 0);
    mo_sources_free(&out);
    CHECK(loads(l, i, false));
}

static void examples(void) {
    DIR *dir = opendir("examples");
    MO_ASSERT(dir != NULL);
    uint32_t files = 0, total = 0;
    for (struct dirent *entry; (entry = readdir(dir)) != NULL;) {
        size_t length = strlen(entry->d_name);
        if (length < 4 || strcmp(entry->d_name + length - 3, ".mo") != 0)
            continue;
        char path[300];
        MO_ASSERT(snprintf(path, sizeof path, "examples/%s", entry->d_name) > 0);
        struct loaded l;
        CHECK(load_path(&l, path));
        files++;
        total += l.mutants.count;
        for (uint32_t i = 0; i < l.mutants.count; i++) {
            one_span(&l, i);
            if (i > 0) {
                const struct mo_mutant *a = &l.mutants.items[i - 1], *b = &l.mutants.items[i];
                CHECK(a->module < b->module ||
                      (a->module == b->module && a->span.start <= b->span.start));
            }
        }
        unload(&l);
    }
    MO_ASSERT(closedir(dir) == 0);
    CHECK(files >= 10 && total > 100);
}

static void run_tests(void *unused) {
    (void)unused;
    comparison();
    specs();
    comments_and_text();
    literals();
    arithmetic();
    library();
    overloads();
    examples();
}

int main(void) {
    CHECK(mo_with_stack(MO_SOURCE_STACK_SIZE, run_tests, NULL) == 0);
    return mo_test_report("mutants");
}
