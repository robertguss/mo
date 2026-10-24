#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "base/buffer.h"
#include "base/codes.h"
#include "base/limits.h"
#include "base/stack.h"
#include "check/check.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void load_graph(void) {
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_arena arena;
    MO_ASSERT(mo_sources_init(&sources));
    MO_ASSERT(mo_diags_init(&diags));
    mo_arena_init(&arena, 16 << 20);
    struct mo_module_set *set =
        mo_load_program(&arena, &sources, "tests/check/load/main.mo", NULL, &diags);
    CHECK(set != NULL && diags.count == 0);
    if (set != NULL) {
        CHECK(strcmp(set->root, "tests/check/load/") == 0);
        CHECK(set->count == 3 && sources.count == 3);
        CHECK(strcmp(sources.files[1].path, "tests/check/load/shop/order-book.mo") == 0);
        CHECK(strcmp(sources.files[2].path, "tests/check/load/shared.mo") == 0);
        CHECK(set->order[0] == 2 && set->order[1] == 1 && set->order[2] == 0);
        CHECK(set->modules[1]->tree->children[1]->children[0]->kind == MO_S_STRUCT);
    }
    mo_arena_free(&arena);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
}

static void interfaces(void) {
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_arena arena;
    MO_ASSERT(mo_sources_init(&sources));
    MO_ASSERT(mo_diags_init(&diags));
    mo_arena_init(&arena, 16 << 20);
    struct mo_module_set *set = mo_load_program(&arena, &sources, "tests/check/interfaces/main.mo",
                                                "tests/check/library", &diags);
    CHECK(set != NULL && diags.count == 0);
    if (set != NULL) {
        CHECK(set->count == 4);
        CHECK(strcmp(set->names[1], "Json") == 0 && strcmp(set->names[2], "LibSupport.Extra") == 0);
        CHECK(set->library[1] && set->library[2] && !set->library[0] && !set->library[3]);
        CHECK(set->uses[0][0] == 3);
        struct mo_module_interfaces *names = mo_resolve_modules(&arena, set, &diags);
        CHECK(names != NULL && diags.count == 0);
        if (names != NULL && names->scopes[0].imported_count == 5) {
            const struct mo_module_scope *scope = &names->scopes[0];
            CHECK(scope->alias_count == 1 && strcmp(scope->aliases[0].name, "Money") == 0);
            CHECK(scope->imported_count == 5);
            const char *expected[] = {"Choice", "Done", "Money", "Ready", "total"};
            for (uint32_t i = 0; i < scope->imported_count && i < 5; i++)
                CHECK(strcmp(scope->imported[i].name, expected[i]) == 0);
            CHECK(scope->imported[2].symbols->exported && !scope->imported[2].symbols->insides);
            CHECK(scope->imported[0].symbols->insides);
            const struct mo_symbol *member = scope->imported[1].symbols;
            const struct mo_module_scope *owner_scope = &names->scopes[member->module];
            CHECK(owner_scope->symbols[member->owner].declaration->kind == MO_S_ENUM);
            CHECK(scope->imported[4].count == 2);
            CHECK(scope->imported[4].symbols[0].declaration->kind == MO_S_FN);
            CHECK(scope->imported[4].symbols[1].declaration->kind == MO_S_FN);
            CHECK(strcmp(owner_scope->symbols[scope->imported[4].symbols[0].receiver].name,
                         "Money") == 0);
            CHECK(strcmp(owner_scope->symbols[scope->imported[4].symbols[1].receiver].name,
                         "Line") == 0);
            const struct mo_symbol *choice = scope->imported[0].symbols;
            CHECK(choice->case_count == 2);
            CHECK(strcmp(owner_scope->symbols[choice->cases[0]].name, "Ready") == 0);
            CHECK(strcmp(owner_scope->symbols[choice->cases[1]].name, "Done") == 0);
            const uint8_t not_terminated[] = {'M', 'o', 'n', 'e', 'y', 'X'};
            CHECK(mo_find_symbol(owner_scope, not_terminated, 5) == scope->imported[2].symbols);
            CHECK(mo_find_symbol(owner_scope, not_terminated, 6) == NULL);
            CHECK(mo_find_symbol(owner_scope, not_terminated, 3) == NULL);
        }
    }
    mo_arena_free(&arena);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
}

struct fixture {
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_arena syntax;
    struct mo_arena arena;
};

static void init(struct fixture *f, size_t limit) {
    MO_ASSERT(mo_sources_init(&f->sources));
    MO_ASSERT(mo_diags_init(&f->diags));
    mo_arena_init(&f->syntax, 1 << 30);
    mo_arena_init(&f->arena, limit);
}

static void finish(struct fixture *f) {
    mo_arena_free(&f->arena);
    mo_arena_free(&f->syntax);
    mo_diags_free(&f->diags);
    mo_sources_free(&f->sources);
}

static MO_MUST_USE struct mo_module_syntax *parse_file(struct fixture *f, const char *path) {
    uint32_t file;
    struct mo_source_error problem;
    MO_ASSERT(mo_sources_add(&f->sources, path, &file, &problem));
    struct mo_module_syntax *module = mo_parse(&f->syntax, &f->sources, file, &f->diags);
    MO_ASSERT(module != NULL && f->diags.count == 0);
    return module;
}

static void span_text(struct mo_buffer *b, struct mo_span span) {
    MO_ASSERT(mo_buffer_append_uint(b, span.file));
    MO_ASSERT(mo_buffer_append_text(b, ":"));
    MO_ASSERT(mo_buffer_append_uint(b, span.start));
    MO_ASSERT(mo_buffer_append_text(b, ".."));
    MO_ASSERT(mo_buffer_append_uint(b, span.end));
}

static void line(struct mo_buffer *b, const char *text) {
    MO_ASSERT(mo_buffer_append_text(b, text == NULL ? "-" : text));
    MO_ASSERT(mo_buffer_append_text(b, "\n"));
}

static void snapshot(struct mo_buffer *b, const struct mo_diag *diag) {
    char code[16];
    int n = snprintf(code, sizeof(code), "E%04u ", (unsigned)diag->code);
    MO_ASSERT(n > 0 && (size_t)n < sizeof(code));
    MO_ASSERT(mo_buffer_append_text(b, code));
    line(b, diag->section);
    span_text(b, diag->primary.span);
    MO_ASSERT(mo_buffer_append_text(b, "\n"));
    line(b, diag->message);
    line(b, diag->reason);
    line(b, diag->primary.text);
    for (uint32_t i = 0; i < diag->secondary_count; i++) {
        MO_ASSERT(mo_buffer_append_text(b, "secondary "));
        span_text(b, diag->secondary[i].span);
        MO_ASSERT(mo_buffer_append_text(b, " "));
        line(b, diag->secondary[i].text);
    }
    MO_ASSERT(mo_buffer_append_text(b, "fixes "));
    MO_ASSERT(mo_buffer_append_uint(b, diag->fix_count));
    MO_ASSERT(mo_buffer_append_text(b, "\n"));
    for (uint32_t i = 0; i < diag->fix_count; i++) {
        line(b, diag->fixes[i].title);
        for (uint32_t j = 0; j < diag->fixes[i].edit_count; j++) {
            span_text(b, diag->fixes[i].edits[j].span);
            MO_ASSERT(mo_buffer_append_text(b, "\n"));
            MO_ASSERT(mo_buffer_append_byte(b, '"'));
            MO_ASSERT(mo_buffer_append_text(b, diag->fixes[i].edits[j].replacement));
            line(b, "\"");
        }
    }
}

static void corpus(void) {
    enum phase { INTERFACES, LOADING, SIGNATURES, BODIES, PROGRAM };
    const struct {
        const char *input;
        const char *expected;
        enum phase phase;
    } cases[] = {
        {"e0025-and/main.mo", "e0025-and.diag", LOADING},
        {"e0025-not/main.mo", "e0025-not.diag", LOADING},
        {"e0025-or-lambda/main.mo", "e0025-or-lambda.diag", LOADING},
        {"e0025-or-assign/main.mo", "e0025-or-assign.diag", LOADING},
        {"e0034/main.mo", "e0034.diag", LOADING},
        {"e0035/main.mo", "e0035.diag", LOADING},
        {"e0036/main.mo", "e0036.diag", LOADING},
        {"e0037/json.mo", "e0037.diag", LOADING},
        {"e0038.mo", "e0038.diag", INTERFACES},
        {"e0038-import-repeat.mo", "e0038-import-repeat.diag", INTERFACES},
        {"e0039.mo", "e0039.diag", INTERFACES},
        {"e0040.mo", "e0040.diag", INTERFACES},
        {"e0041.mo", "e0041.diag", INTERFACES},
        {"e0042.mo", "e0042.diag", INTERFACES},
        {"e0043.mo", "e0043.diag", INTERFACES},
        {"e0044.mo", "e0044.diag", INTERFACES},
        {"e0045.mo", "e0045.diag", INTERFACES},
        {"e0046.mo", "e0046.diag", INTERFACES},
        {"e0047.mo", "e0047.diag", INTERFACES},
        {"e0048.mo", "e0048.diag", SIGNATURES},
        {"e0049.mo", "e0049.diag", SIGNATURES},
        {"e0050.mo", "e0050.diag", BODIES},
        {"e0050-len.mo", "e0050-len.diag", BODIES},
        {"e0050-length.mo", "e0050-length.diag", BODIES},
        {"e0051.mo", "e0051.diag", BODIES},
        {"e0051-var.mo", "e0051-var.diag", BODIES},
        {"e0051-new-name.mo", "e0051-new-name.diag", BODIES},
        {"e0051-lambda.mo", "e0051-lambda.diag", BODIES},
        {"e0052.mo", "e0052.diag", BODIES},
        {"e0053.mo", "e0053.diag", BODIES},
        {"e0054.mo", "e0054.diag", BODIES},
        {"e0054-tuple-part.mo", "e0054-tuple-part.diag", BODIES},
        {"e0055.mo", "e0055.diag", BODIES},
        {"e0056.mo", "e0056.diag", BODIES},
        {"e0057.mo", "e0057.diag", BODIES},
        {"e0058.mo", "e0058.diag", BODIES},
        {"e0059.mo", "e0059.diag", BODIES},
        {"e0060.mo", "e0060.diag", BODIES},
        {"e0061.mo", "e0061.diag", BODIES},
        {"e0062.mo", "e0062.diag", SIGNATURES},
        {"e0063.mo", "e0063.diag", BODIES},
        {"e0064.mo", "e0064.diag", BODIES},
        {"e0065.mo", "e0065.diag", BODIES},
        {"e0066.mo", "e0066.diag", SIGNATURES},
        {"e0067.mo", "e0067.diag", BODIES},
        {"e0068.mo", "e0068.diag", BODIES},
        {"e0069.mo", "e0069.diag", BODIES},
        {"e0070.mo", "e0070.diag", BODIES},
        {"e0071.mo", "e0071.diag", BODIES},
        {"e0072.mo", "e0072.diag", BODIES},
        {"e0073.mo", "e0073.diag", BODIES},
        {"e0074.mo", "e0074.diag", BODIES},
        {"e0075.mo", "e0075.diag", BODIES},
        {"e0075-bool.mo", "e0075-bool.diag", BODIES},
        {"e0076.mo", "e0076.diag", BODIES},
        {"e0077.mo", "e0077.diag", BODIES},
        {"e0078.mo", "e0078.diag", BODIES},
        {"e0079.mo", "e0079.diag", PROGRAM},
        {"e0080.mo", "e0080.diag", PROGRAM},
        {"e0081.mo", "e0081.diag", SIGNATURES},
        {"e0082.mo", "e0082.diag", SIGNATURES},
        {"e0083.mo", "e0083.diag", PROGRAM},
        {"e0084.mo", "e0084.diag", SIGNATURES},
        {"e0085.mo", "e0085.diag", PROGRAM},
        {"e0086.mo", "e0086.diag", PROGRAM},
        {"e0087.mo", "e0087.diag", PROGRAM},
        {"e0087-ambiguous.mo", "e0087-ambiguous.diag", PROGRAM},
        {"e0094.mo", "e0094.diag", BODIES},
        {"e0095.mo", "e0095.diag", BODIES},
        {"e0412.mo", "e0412.diag", BODIES},
        {"e1103.mo", "e1103.diag", BODIES},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct fixture f;
        init(&f, 64 << 20);
        char path[256];
        int n = snprintf(path, sizeof(path), "tests/check/diagnostics/%s", cases[i].input);
        MO_ASSERT(n > 0 && (size_t)n < sizeof(path));
        if (cases[i].phase == LOADING)
            CHECK(mo_load_program(&f.arena, &f.sources, path, NULL, &f.diags) == NULL);
        else {
            struct mo_module_syntax *modules[] = {
                parse_file(&f, path), parse_file(&f, strcmp(cases[i].input, "e0047.mo") == 0
                                                         ? "tests/check/interfaces/hidden.mo"
                                                         : "tests/check/interfaces/money.mo")};
            uint32_t count = modules[0]->header.uses->count;
            uint32_t *targets =
                mo_arena_allocate_array(&f.syntax, count, sizeof(*targets), _Alignof(uint32_t));
            MO_ASSERT(targets != NULL);
            for (uint32_t j = 0; j < count; j++)
                targets[j] = 1;
            uint32_t *uses[] = {targets, NULL};
            struct mo_module_set set = {
                .count = cases[i].phase == PROGRAM ? 1u : 2u, .modules = modules, .uses = uses};
            if (cases[i].phase == PROGRAM) {
                CHECK(mo_check(&f.arena, &set, &f.diags) == NULL);
            } else {
                struct mo_module_interfaces *names = mo_resolve_modules(&f.arena, &set, &f.diags);
                if (cases[i].phase == INTERFACES)
                    CHECK(names == NULL);
                else {
                    CHECK(names != NULL);
                    if (names != NULL) {
                        struct mo_signatures *signatures =
                            mo_prepare_signatures(&f.arena, &set, names, &f.diags);
                        if (cases[i].phase == SIGNATURES)
                            CHECK(signatures == NULL);
                        else {
                            MO_ASSERT(signatures != NULL);
                            for (uint32_t j = 0; j < signatures->count; j++)
                                if (signatures->items[j].function->module == 0 &&
                                    mo_infer_function(&f.arena, names, signatures, j, &f.diags) ==
                                        NULL)
                                    break;
                        }
                    }
                }
            }
        }
        CHECK(f.diags.count == 1 && f.diags.dropped == 0);
        if (f.diags.count == 1) {
            struct mo_buffer actual;
            mo_buffer_init(&actual, 1 << 20);
            snapshot(&actual, &f.diags.items[0]);
            n = snprintf(path, sizeof(path), "tests/check/diagnostics/%s", cases[i].expected);
            MO_ASSERT(n > 0 && (size_t)n < sizeof(path));
            uint32_t file;
            struct mo_source_error problem;
            MO_ASSERT(mo_sources_add(&f.sources, path, &file, &problem));
            const struct mo_source *expected = &f.sources.files[file];
            bool same = actual.length == expected->length &&
                        memcmp(actual.bytes, expected->bytes, actual.length) == 0;
            if (!same)
                MO_ASSERT(fprintf(stderr, "%s: actual diagnostic\n%.*s", cases[i].input,
                                  (int)actual.length, actual.bytes) >= 0);
            CHECK(same);
            mo_buffer_free(&actual);
        }
        finish(&f);
    }
}

static void samples(void) {
    static const char *const files[] = {"account",    "expenses",   "find",       "greet",
                                        "inventory",  "job-queue",  "json-query", "kv",
                                        "notes-tool", "rate-limit", "todo",       "wc"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        char path[256];
        int n = snprintf(path, sizeof(path), "examples/%s.mo", files[i]);
        MO_ASSERT(n > 0 && (size_t)n < sizeof(path));
        struct fixture f;
        init(&f, 64 << 20);
        struct mo_module_set *set = mo_load_program(&f.arena, &f.sources, path, NULL, &f.diags);
        CHECK(set != NULL);
        if (set != NULL) {
            struct mo_module_interfaces *names = mo_resolve_modules(&f.arena, set, &f.diags);
            CHECK(names != NULL);
            if (names != NULL)
                CHECK(mo_prepare_signatures(&f.arena, set, names, &f.diags) != NULL);
        }
        CHECK(f.diags.count == 0);
        finish(&f);
    }
}

static void arena_limits(void) {
    for (size_t limit = 0; limit < (1 << 20); limit = limit < 1024 ? limit + 16 : limit + 4096) {
        struct fixture f;
        init(&f, limit);
        struct mo_module_set *set =
            mo_load_program(&f.arena, &f.sources, "tests/check/load/main.mo", NULL, &f.diags);
        if (set == NULL) {
            CHECK(f.diags.count == 1);
            CHECK(f.diags.items[0].code == MO_CODE_CHECK_NO_MEMORY ||
                  f.diags.items[0].code == MO_CODE_SYNTAX_NO_MEMORY);
        } else
            CHECK(f.diags.count == 0);
        finish(&f);
    }
    for (size_t limit = 0; limit <= 8192; limit += 16) {
        struct fixture f;
        init(&f, limit);
        struct mo_module_syntax *modules[] = {parse_file(&f, "tests/check/interfaces/main.mo"),
                                              parse_file(&f, "tests/check/interfaces/money.mo")};
        uint32_t targets[] = {1}, *uses[] = {targets, NULL};
        struct mo_module_set set = {.count = 2, .modules = modules, .uses = uses};
        struct mo_module_interfaces *names = mo_resolve_modules(&f.arena, &set, &f.diags);
        CHECK(names != NULL ||
              (f.diags.count == 1 && f.diags.items[0].code == MO_CODE_CHECK_NO_MEMORY));
        finish(&f);
    }
}

static void write_file(const char *path, const uint8_t *bytes, size_t length) {
    FILE *file = fopen(path, "wb");
    MO_ASSERT(file != NULL);
    if (length != 0)
        MO_ASSERT(fwrite(bytes, 1, length, file) == length);
    MO_ASSERT(fclose(file) == 0);
}

static void imported_function(void) {
    char folder[] = "/tmp/mo-imports-XXXXXX";
    MO_ASSERT(mkdtemp(folder) != NULL);
    char main_path[128], other_path[128];
    int n = snprintf(main_path, sizeof(main_path), "%s/main.mo", folder);
    MO_ASSERT(n > 0 && (size_t)n < sizeof(main_path));
    n = snprintf(other_path, sizeof(other_path), "%s/shapes.mo", folder);
    MO_ASSERT(n > 0 && (size_t)n < sizeof(other_path));
    const char main_source[] = "module Main\nuse Shapes{Shape, make, area}\n"
                               "fn measure(value: Shape) : Int\narea(value)\nend\n"
                               "fn main() : Int\nmeasure(make(19))\nend\n";
    const char other_source[] = "module Shapes\nexpose Shape(..), make, area\n"
                                "struct Shape\nvalue: Int\nend\n"
                                "fn make(value: Int) : Shape\nShape(value: value)\nend\n"
                                "fn area(value: Shape) : Int\nvalue.value\nend\n";
    write_file(main_path, (const uint8_t *)main_source, sizeof(main_source) - 1);
    write_file(other_path, (const uint8_t *)other_source, sizeof(other_source) - 1);
    struct fixture f;
    init(&f, MO_CLI_CHECK_SIZE);
    struct mo_module_set *set = mo_load_program(&f.syntax, &f.sources, main_path, NULL, &f.diags);
    MO_ASSERT(set != NULL && f.diags.count == 0);
    struct mo_program *program = mo_check(&f.arena, set, &f.diags);
    CHECK(program != NULL && f.diags.count == 0);
    if (program != NULL) {
        const struct mo_module_scope *owner = &program->interfaces->scopes[1];
        const struct mo_symbol *shape = mo_find_symbol(owner, (const uint8_t *)"Shape", 5);
        MO_ASSERT(shape != NULL);
        const struct mo_checked_function *measure = NULL;
        for (uint32_t i = 0; i < program->function_count; i++)
            if (strcmp(program->functions[i].signature->function->name, "measure") == 0)
                measure = &program->functions[i];
        MO_ASSERT(measure != NULL);
        CHECK(measure->signature->type->arguments[0]->declaration == shape);
        bool saw_area = false;
        const struct mo_checked_body *body = &measure->body;
        for (uint32_t i = 0; i < body->count; i++)
            if (body->items[i].symbol != NULL && strcmp(body->items[i].symbol->name, "area") == 0) {
                saw_area = true;
                CHECK(body->items[i].symbol == mo_find_symbol(owner, (const uint8_t *)"area", 4));
            }
        CHECK(saw_area);
    }
    finish(&f);
    MO_ASSERT(unlink(main_path) == 0 && unlink(other_path) == 0 && rmdir(folder) == 0);
}

static MO_MUST_USE bool same_source_type(const struct mo_source_type *a,
                                         const struct mo_source_type *b) {
    if (a == b)
        return true;
    if (a->kind != b->kind || a->module != b->module || a->declaration != b->declaration ||
        a->count != b->count || strcmp(a->name, b->name) != 0)
        return false;
    for (uint32_t i = 0; i < a->count; i++)
        if (!same_source_type(a->arguments[i], b->arguments[i]))
            return false;
    return true;
}

static void imported_constants(void) {
    char folder[] = "/tmp/mo-constant-import-XXXXXX", path[128], other[128];
    MO_ASSERT(mkdtemp(folder) != NULL);
    int n = snprintf(path, sizeof(path), "%s/main.mo", folder);
    MO_ASSERT(n > 0 && (size_t)n < sizeof(path));
    n = snprintf(other, sizeof(other), "%s/lib.mo", folder);
    MO_ASSERT(n > 0 && (size_t)n < sizeof(other));
    const char main[] = "module Main\nuse Lib\nseed = 11\nanswer = Lib.seed * 7 + seed\n"
                        "fn main() : Int\nanswer\nend\n";
    const char library[] = "module Lib\nexpose seed\nseed = 13\n";
    write_file(path, (const uint8_t *)main, sizeof(main) - 1);
    write_file(other, (const uint8_t *)library, sizeof(library) - 1);
    struct fixture f;
    init(&f, MO_CLI_CHECK_SIZE);
    struct mo_module_set *set = mo_load_program(&f.syntax, &f.sources, path, NULL, &f.diags);
    MO_ASSERT(set != NULL && !mo_diags_any(&f.diags));
    struct mo_program *program = mo_check(&f.arena, set, &f.diags);
    mo_sources_free(&f.sources);
    CHECK(program != NULL && !mo_diags_any(&f.diags));
    if (program != NULL) {
        MO_ASSERT(program->constant_count == 3);
        const struct mo_symbol *seeds[2];
        for (uint32_t i = 0; i < 2; i++) {
            seeds[i] = mo_find_symbol(&program->interfaces->scopes[i], (const uint8_t *)"seed", 4);
            CHECK(program->constants[i].symbol == seeds[i]);
        }
        CHECK(seeds[0] != seeds[1]);
        const struct mo_checked_constant *answer = &program->constants[2];
        CHECK(strcmp(answer->symbol->name, "answer") == 0 && answer->symbol->module == 0);
        CHECK(strcmp(answer->type->name, "Int") == 0 && answer->type->parameters == 0);
        bool reads[2] = {false, false};
        for (uint32_t i = 0; i < answer->initializer.count; i++) {
            const struct mo_checked_expression *row = &answer->initializer.items[i];
            CHECK(row->syntax->span.file == 0);
            for (uint32_t j = 0; j < 2; j++)
                reads[j] = reads[j] || row->symbol == seeds[j];
        }
        CHECK(reads[0] && reads[1]);
    } else {
        MO_ASSERT(f.diags.count != 0);
        MO_ASSERT(fprintf(stderr, "constant import: E%04u %u:%u..%u %s\n",
                          (unsigned)f.diags.items[0].code, f.diags.items[0].primary.span.file,
                          f.diags.items[0].primary.span.start, f.diags.items[0].primary.span.end,
                          f.diags.items[0].message) > 0);
    }
    finish(&f);
    MO_ASSERT(unlink(path) == 0 && unlink(other) == 0 && rmdir(folder) == 0);
}

static void constant_import_sources(void) {
    static const struct {
        const char *name, *files[4], *order[8];
        uint16_t code;
    } cases[] = {
        {"listed reads and aliases",
         {"module Main\nuse Lib{seed}\ncopy = seed\nanswer = copy * 7\n"
          "fn main() : Int\nanswer + seed\nend\n",
          "module Lib\nexpose seed\nseed = 13\n"},
         {"Lib.seed", "Main.copy", "Main.answer"},
         0},
        {"transitive qualified aliases",
         {"module Main\nuse Bridge\nanswer = Bridge.copy + 11\nfn main() : Int\nanswer\nend\n",
          "module Lib\nexpose seed\nseed = 13\n",
          "module Bridge\nexpose copy\nuse Lib\ncopy = Lib.seed * 7\n"},
         {"Lib.seed", "Bridge.copy", "Main.answer"},
         0},
        {"diamond and ready ties",
         {"module Main\nuse Lib\nuse Bridge\nanswer = Lib.value + Bridge.value\n"
          "independent = 11\nfn main() : Int\nanswer + independent\nend\n",
          "module Lib\nexpose value\nuse Leaf\nvalue = Leaf.seed * 7\n",
          "module Bridge\nexpose value\nuse Leaf\nvalue = Leaf.seed - 3\n",
          "module Leaf\nexpose seed\nseed = 13\n"},
         {"Main.independent", "Leaf.seed", "Lib.value", "Bridge.value", "Main.answer"},
         0},
        {"call reaches a private later constant",
         {"module Main\nuse Lib{read}\nanswer = read()\nindependent = 11\n"
          "fn main() : Int\nanswer + independent\nend\n",
          "module Lib\nexpose read\nfn read() : Int\nbase * 7\nend\nbase = 13\n"},
         {"Main.independent", "Lib.base", "Main.answer"},
         0},
        {"stored function is invoked",
         {"module Main\nuse Lib\nanswer = handler()\nhandler = Lib.read\n"
          "fn main() : Int\nanswer\nend\n",
          "module Lib\nexpose read\nfn read() : Int\nbase * 7\nend\nbase = 13\n"},
         {"Main.handler", "Lib.base", "Main.answer"},
         0},
        {"foreign stored function chain",
         {"module Main\nuse Bridge{handler}\nanswer = handler()\nfn main() : Int\nanswer\nend\n",
          "module Lib\nexpose read\nfn read() : Int\nbase\nend\nbase = 13\n",
          "module Bridge\nexpose handler\nuse Lib\nhandler = Lib.read\n"},
         {"Bridge.handler", "Lib.base", "Main.answer"},
         0},
        {"constructing a function is not calling it",
         {"module Main\nuse Lib\nhandler = Lib.read\nlambda = fn() Lib.base end\n"
          "fn main() : Int\nhandler() + lambda()\nend\n",
          "module Lib\nexpose read, base\nbase = 13\nfn read() : Int\nbase\nend\n"},
         {"Main.handler", "Main.lambda", "Lib.base"},
         0},
        {"foreign anonymous function is not executed",
         {"module Main\nuse Lib{handler}\ncopy = handler\nfn main() : Int\ncopy()\nend\n",
          "module Lib\nexpose handler\nhandler = fn() base end\nbase = 13\n"},
         {"Lib.handler", "Main.copy", "Lib.base"},
         0},
        {"known local alias in an imported body",
         {"module Main\nuse Lib\nanswer = Lib.read()\nfn main() : Int\nanswer\nend\n",
          "module Lib\nexpose read\nbase = 13\nfn fetch() : Int\nbase\nend\n"
          "fn read() : Int\nreader = fetch\nreader()\nend\n"},
         {"Lib.base", "Main.answer"},
         0},
        {"owned generic nominal result",
         {"module Main\nuse Lib\nanswer = Lib.make(13).value\ncopy = Lib.box\n"
          "fn main() : Int\nanswer + copy.value.byte_count\nend\n",
          "module Lib\nexpose Box(..), make, box\nstruct Box(T)\nvalue: T\nend\n"
          "fn make(value: T) : Box(T)\nBox(value: value)\nend\nbox = Box(value: \"owned\")\n"},
         {"Main.answer", "Lib.box", "Main.copy"},
         0},
        {"selected customizer execution dependency",
         {"module Main\nuse Lib\nanswer = \"#{Lib.Label(value: 3)}\"\n"
          "fn main() : Int\nanswer.byte_count\nend\n",
          "module Lib\nexpose Label(..)\nstruct Label\nvalue: Int\nend\n"
          "fn to_text(value: Label) : Text\n\"#{base}:#{value.value}\"\nend\nbase = 13\n"},
         {"Lib.base", "Main.answer"},
         0},
        {"unused proof is not execution",
         {"module Main\nuse Lib\nanswer = Lib.ignore(Lib.Label(value: 3))\n"
          "fn main() : Int\nanswer\nend\n",
          "module Lib\nexpose Label(..), ignore\nstruct Label\nvalue: Int\nend\n"
          "fn to_text(_value: Label) : Text\ncycle.to_text\nend\n"
          "cycle = ignore(Label(value: 3))\n"
          "fn ignore(_value: T) : Int where T: Text\n42\nend\n"},
         {"Main.answer", "Lib.cycle"},
         0},
        {"private constant",
         {"module Main\nuse Lib\nfn main() : Int\nLib.seed\nend\n", "module Lib\nseed = 13\n"},
         {NULL},
         MO_CODE_IMPORT_PRIVATE},
        {"private initializer function",
         {"module Main\nuse Lib\nanswer = Lib.read()\nfn main() : Int\nanswer\nend\n",
          "module Lib\nfn read() : Int\n13\nend\n"},
         {NULL},
         MO_CODE_IMPORT_PRIVATE},
        {"qualified read does not credit an unused listed name",
         {"module Main\nuse Lib{seed}\nanswer = Lib.seed\nfn main() : Int\nanswer\nend\n",
          "module Lib\nexpose seed\nseed = 13\n"},
         {NULL},
         MO_CODE_UNUSED_IMPORT},
        {"foreign value cannot be constrained by its user",
         {"module Main\nuse Lib{values}\nfn wanted() : List(Int)\nvalues\nend\nfn main()\nend\n",
          "module Lib\nexpose values\nvalues = []\n"},
         {NULL},
         MO_CODE_CANNOT_INFER},
        {"imported call reaches a value cycle",
         {"module Main\nuse Lib{read}\nanswer = read()\nfn main() : Int\nanswer\nend\n",
          "module Lib\nexpose read\nfirst = read()\nsecond = first + 1\n"
          "fn read() : Int\nsecond\nend\n"},
         {NULL},
         MO_CODE_CONSTANT_CYCLE},
        {"foreign aliases retain a cycle",
         {"module Main\nuse Lib{handler}\nanswer = handler()\nfn main() : Int\nanswer\nend\n",
          "module Lib\nexpose handler\nhandler = read\nbase = handler()\n"
          "fn read() : Int\nbase\nend\n"},
         {NULL},
         MO_CODE_CONSTANT_CYCLE},
        {"module cycles are rejected by loading",
         {"module Main\nexpose answer\nuse Lib\nanswer = Lib.seed\n"
          "fn main() : Int\nanswer\nend\n",
          "module Lib\nexpose seed\nuse Main\nseed = Main.answer\n"},
         {NULL},
         MO_CODE_MODULE_CYCLE},
        {"unresolved imported higher-order call",
         {"module Main\nuse Lib\nanswer = Lib.apply(Lib.read)\n"
          "fn main() : Int\nanswer\nend\n",
          "module Lib\nexpose apply, read\nfn apply(f: fn() : Int) : Int\nf()\nend\n"
          "fn read() : Int\n13\nend\n"},
         {NULL},
         MO_CODE_CHECK_UNFINISHED},
        {"unresolved anonymous initializer call",
         {"module Main\nuse Lib\nanswer = Lib.handler()\nfn main() : Int\nanswer\nend\n",
          "module Lib\nexpose handler\nhandler = fn() 13 end\n"},
         {NULL},
         MO_CODE_CHECK_UNFINISHED},
        {"abstract executed Text proof stays staged",
         {"module Main\nuse Lib\nanswer = Lib.render(13)\n"
          "fn main() : Int\nanswer.byte_count\nend\n",
          "module Lib\nexpose render\nfn render(value: T) : Text where T: Text\n"
          "value.to_text\nend\n"},
         {NULL},
         MO_CODE_CHECK_UNFINISHED},
    };
    const char *filenames[] = {"main.mo", "lib.mo", "bridge.mo", "leaf.mo"};
    char folder[] = "/tmp/mo-constant-sources-XXXXXX", paths[4][128];
    MO_ASSERT(mkdtemp(folder) != NULL);
    for (uint32_t i = 0; i < 4; i++) {
        int n = snprintf(paths[i], sizeof(paths[i]), "%s/%s", folder, filenames[i]);
        MO_ASSERT(n > 0 && (size_t)n < sizeof(paths[i]));
    }
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        for (uint32_t j = 0; j < 4; j++)
            if (cases[i].files[j] != NULL)
                write_file(paths[j], (const uint8_t *)cases[i].files[j], strlen(cases[i].files[j]));
        struct fixture f;
        init(&f, MO_CLI_CHECK_SIZE);
        struct mo_module_set *set =
            mo_load_program(&f.syntax, &f.sources, paths[0], NULL, &f.diags);
        struct mo_program *program = set == NULL ? NULL : mo_check(&f.arena, set, &f.diags);
        mo_sources_free(&f.sources);
        bool passed = cases[i].code == 0 ? program != NULL && !mo_diags_any(&f.diags)
                                         : program == NULL && f.diags.count == 1 &&
                                               f.diags.items[0].code == cases[i].code;
        if (!passed)
            MO_ASSERT(fprintf(stderr,
                              "constant source %s: expected E%04u, found E%04u (%u errors)\n",
                              cases[i].name, (unsigned)cases[i].code,
                              f.diags.count == 0 ? 0u : (unsigned)f.diags.items[0].code,
                              f.diags.count) >= 0);
        CHECK(passed);
        if (program != NULL) {
            uint32_t count = 0;
            while (count < 8 && cases[i].order[count] != NULL)
                count++;
            CHECK(program->constant_count == count);
            for (uint32_t j = 0; j < program->constant_count; j++) {
                const struct mo_checked_constant *constant = &program->constants[j];
                const struct mo_symbol *symbol = constant->symbol;
                const struct mo_module_scope *scope = &program->interfaces->scopes[symbol->module];
                char name[128];
                int n = snprintf(name, sizeof(name), "%s.%s", scope->name, symbol->name);
                MO_ASSERT(n > 0 && (size_t)n < sizeof(name));
                CHECK(j < count && strcmp(name, cases[i].order[j]) == 0);
                CHECK(symbol ==
                      mo_find_symbol(scope, (const uint8_t *)symbol->name, strlen(symbol->name)));
                CHECK(constant->type->parameters == 0);
                for (uint32_t k = 0; k < constant->initializer.count; k++) {
                    const struct mo_checked_expression *row = &constant->initializer.items[k];
                    CHECK(row->syntax->span.file == symbol->declaration->span.file);
                    CHECK(row->type->parameters == 0);
                    if (row->symbol != NULL && row->symbol->declaration->kind == MO_S_FN)
                        for (uint32_t a = 0; a < row->arguments.count; a++)
                            CHECK(row->arguments.items[a].parameter->declaration == row->symbol);
                }
            }
        }
        finish(&f);
        for (uint32_t j = 0; j < 4; j++)
            if (cases[i].files[j] != NULL)
                MO_ASSERT(unlink(paths[j]) == 0);
    }
    printf("constant sources: %zu public load/check controls\n", sizeof(cases) / sizeof(cases[0]));
    MO_ASSERT(rmdir(folder) == 0);
}

static void imported_nominal(void) {
    char folder[] = "/tmp/mo-nominal-import-XXXXXX", path[128], other[128];
    MO_ASSERT(mkdtemp(folder) != NULL);
    int n = snprintf(path, sizeof(path), "%s/main.mo", folder);
    MO_ASSERT(n > 0 && (size_t)n < sizeof(path));
    n = snprintf(other, sizeof(other), "%s/shapes.mo", folder);
    MO_ASSERT(n > 0 && (size_t)n < sizeof(other));
    const char main[] = "module Main\nuse Shapes{Pair}\n"
                        "fn score(pair: Pair(Int, Bool)) : Int\n"
                        "if pair.second: pair.first * 10 + 7 else: pair.first - 3\nend\n"
                        "fn main() : Int\nscore(Pair(second: true, first: 3))\nend\n";
    const char shapes[] = "module Shapes\nexpose Pair(..)\n"
                          "struct Pair(Z, A)\nfirst: Z\nsecond: A\nend\n";
    write_file(path, (const uint8_t *)main, sizeof(main) - 1);
    write_file(other, (const uint8_t *)shapes, sizeof(shapes) - 1);
    struct fixture f;
    init(&f, MO_CLI_CHECK_SIZE);
    struct mo_module_set *set = mo_load_program(&f.syntax, &f.sources, path, NULL, &f.diags);
    MO_ASSERT(set != NULL && !mo_diags_any(&f.diags));
    struct mo_program *program = mo_check(&f.arena, set, &f.diags);
    mo_sources_free(&f.sources);
    CHECK(program != NULL && !mo_diags_any(&f.diags));
    if (program != NULL) {
        const struct mo_symbol *pair =
            mo_find_symbol(&program->interfaces->scopes[1], (const uint8_t *)"Pair", 4);
        MO_ASSERT(program->signatures->data_count == 1 && pair != NULL);
        const struct mo_data_signature *data = &program->signatures->data[0];
        CHECK(data->symbol == pair && data->type->declaration == pair);
        MO_ASSERT(data->type->count == 2);
        CHECK(strcmp(data->type->arguments[0]->name, "Z") == 0 &&
              strcmp(data->type->arguments[1]->name, "A") == 0);
        CHECK(data->type->arguments[0]->declaration == pair &&
              data->type->arguments[1]->declaration == pair);
        bool constructed = false;
        uint32_t projections = 0;
        for (uint32_t i = 0; i < program->function_count; i++) {
            const struct mo_checked_function *fn = &program->functions[i];
            if (strcmp(fn->signature->function->name, "score") == 0)
                CHECK(fn->signature->type->arguments[0]->declaration == pair);
            for (uint32_t j = 0; j < fn->body.count; j++) {
                const struct mo_checked_expression *row = &fn->body.items[j];
                if (row->symbol == pair) {
                    MO_ASSERT(row->operand_count == 2 && row->arguments.count == 2);
                    CHECK(row->type->declaration == pair && row->syntax->span.file == 0);
                    CHECK(fn->body.items[row->operands[0]].field == 1 &&
                          fn->body.items[row->operands[1]].field == 0);
                    constructed = true;
                } else if (strcmp(fn->signature->function->name, "score") == 0 &&
                           row->field != UINT32_MAX) {
                    MO_ASSERT(row->operand_count == 1 && row->arguments.count == 2);
                    CHECK(fn->body.items[row->operands[0]].type->declaration == pair);
                    CHECK(row->field < 2 &&
                          strcmp(row->type->name, row->field == 0 ? "Int" : "Bool") == 0);
                    projections++;
                } else {
                    continue;
                }
                for (uint32_t k = 0; k < 2; k++) {
                    CHECK(row->arguments.items[k].parameter == data->type->arguments[k]);
                    CHECK(strcmp(row->arguments.items[k].type->name, k == 0 ? "Int" : "Bool") == 0);
                }
            }
        }
        CHECK(constructed && projections == 3);
    } else {
        MO_ASSERT(f.diags.count != 0);
        MO_ASSERT(fprintf(stderr, "nominal import: E%04u %u:%u..%u %s\n",
                          (unsigned)f.diags.items[0].code, f.diags.items[0].primary.span.file,
                          f.diags.items[0].primary.span.start, f.diags.items[0].primary.span.end,
                          f.diags.items[0].message) > 0);
    }
    finish(&f);
    MO_ASSERT(unlink(path) == 0 && unlink(other) == 0 && rmdir(folder) == 0);
}

static void nominal_pattern(const struct mo_program *program,
                            const struct mo_checked_pattern *pattern,
                            const struct mo_checked_function *function) {
    if (pattern->constructor.symbol != NULL) {
        const struct mo_symbol *symbol = pattern->constructor.symbol;
        const struct mo_module_scope *scope = &program->interfaces->scopes[symbol->module];
        const struct mo_symbol *owner =
            symbol->owner == UINT32_MAX ? symbol : &scope->symbols[symbol->owner];
        const struct mo_data_signature *data = NULL;
        for (uint32_t i = 0; i < program->signatures->data_count; i++)
            if (program->signatures->data[i].symbol == symbol)
                data = &program->signatures->data[i];
        MO_ASSERT(data != NULL && data->type->count == 2 && pattern->arguments.count == 2);
        CHECK(owner == mo_find_symbol(scope, (const uint8_t *)owner->name, strlen(owner->name)));
        CHECK(pattern->type->declaration == owner && data->type->declaration == owner);
        for (uint32_t i = 0; i < 2; i++) {
            const struct mo_type_argument *argument = &pattern->arguments.items[i];
            CHECK(argument->parameter == data->type->arguments[i] &&
                  argument->parameter->declaration == owner);
            CHECK(strcmp(argument->parameter->name, i == 0 ? "Z" : "A") == 0);
            CHECK(same_source_type(argument->type, pattern->type->arguments[i]));
            if (strcmp(function->signature->function->name, "relay") == 0) {
                CHECK(argument->type->declaration == function->signature->function &&
                      strcmp(argument->type->name, i == 0 ? "A" : "Z") == 0);
            } else {
                CHECK(strcmp(argument->type->name, i == 0 ? "Int" : "Bool") == 0);
            }
        }
        if (strcmp(symbol->name, "Named") == 0 || strcmp(symbol->name, "Pair") == 0) {
            MO_ASSERT(pattern->part_count == 2);
            for (uint32_t i = 0; i < 2; i++) {
                CHECK(pattern->parts[i].index == i);
                CHECK(same_source_type(pattern->parts[i].pattern->type,
                                       pattern->arguments.items[i].type));
            }
        }
        CHECK(pattern->syntax->span.file == function->signature->function->module);
    }
    for (uint32_t i = 0; i < pattern->part_count; i++)
        nominal_pattern(program, pattern->parts[i].pattern, function);
}

static void imported_choice(void) {
    char folder[] = "/tmp/mo-choice-import-XXXXXX", path[128], other[128];
    MO_ASSERT(mkdtemp(folder) != NULL);
    int n = snprintf(path, sizeof(path), "%s/main.mo", folder);
    MO_ASSERT(n > 0 && (size_t)n < sizeof(path));
    n = snprintf(other, sizeof(other), "%s/shapes.mo", folder);
    MO_ASSERT(n > 0 && (size_t)n < sizeof(other));
    const char main[] = "module Main\nuse Shapes{Choice, Pair}\n"
                        "fn score(value: Choice(Int, Bool)) : Int\ncase value\n"
                        "Named(second: flag, first: n): if flag: n * 10 + 7 else: n - 3\n"
                        "Payload(Pair(first: n)): n\nVacant: 0\nend\nend\n"
                        "fn relay(value: Choice(A, Z)) : A\ncase value\nNamed(first: n): n\n"
                        "Payload(Pair(first: n)): n\nVacant: stop \"empty\"\nend\nend\n"
                        "fn main() : Int\nassert relay(Named(second: 29, first: 13)) == 13\n"
                        "score(Named(second: true, first: 3))\nend\n";
    const char shapes[] =
        "module Shapes\nexpose Choice(..), Pair(..)\n"
        "struct Pair(Z, A)\nfirst: Z\nsecond: A\nend\n"
        "enum Choice(Z, A)\nVacant\nNamed(first: Z, second: A)\nPayload(Pair(Z, A))\nend\n";
    write_file(path, (const uint8_t *)main, sizeof(main) - 1);
    write_file(other, (const uint8_t *)shapes, sizeof(shapes) - 1);
    struct fixture f;
    init(&f, MO_CLI_CHECK_SIZE);
    struct mo_module_set *set = mo_load_program(&f.syntax, &f.sources, path, NULL, &f.diags);
    MO_ASSERT(set != NULL && !mo_diags_any(&f.diags));
    struct mo_program *program = mo_check(&f.arena, set, &f.diags);
    mo_sources_free(&f.sources);
    CHECK(program != NULL && !mo_diags_any(&f.diags));
    if (program != NULL) {
        const struct mo_module_scope *scope = &program->interfaces->scopes[1];
        const struct mo_symbol *choice = mo_find_symbol(scope, (const uint8_t *)"Choice", 6);
        MO_ASSERT(choice != NULL && choice->case_count == 3);
        const char *cases[] = {"Vacant", "Named", "Payload"};
        for (uint32_t i = 0; i < 3; i++) {
            const struct mo_symbol *symbol = &scope->symbols[choice->cases[i]];
            CHECK(strcmp(symbol->name, cases[i]) == 0 && &scope->symbols[symbol->owner] == choice);
        }
        uint32_t matches = 0, constructions = 0;
        for (uint32_t i = 0; i < program->function_count; i++) {
            const struct mo_checked_function *fn = &program->functions[i];
            for (uint32_t j = 0; j < fn->body.count; j++) {
                const struct mo_checked_expression *row = &fn->body.items[j];
                if (row->symbol != NULL && strcmp(row->symbol->name, "Named") == 0) {
                    MO_ASSERT(row->arguments.count == 2 && row->operand_count == 2);
                    CHECK(row->arguments.items[0].parameter != row->arguments.items[1].parameter);
                    CHECK(fn->body.items[row->operands[0]].field == 1 &&
                          fn->body.items[row->operands[1]].field == 0);
                    for (uint32_t k = 0; k < 2; k++)
                        CHECK(row->arguments.items[k].parameter->declaration == choice);
                    constructions++;
                }
                if (row->match == NULL)
                    continue;
                MO_ASSERT(row->match->arm_count == 3);
                for (uint32_t k = 0; k < row->match->arm_count; k++) {
                    const struct mo_checked_arm *arm = &row->match->arms[k];
                    MO_ASSERT(arm->pattern_count == 1);
                    nominal_pattern(program, arm->patterns[0], fn);
                }
                matches++;
            }
        }
        CHECK(matches == 2 && constructions == 2);
    } else {
        MO_ASSERT(f.diags.count != 0);
        MO_ASSERT(fprintf(stderr, "choice import: E%04u %s\n", (unsigned)f.diags.items[0].code,
                          f.diags.items[0].message) > 0);
    }
    finish(&f);
    MO_ASSERT(unlink(path) == 0 && unlink(other) == 0 && rmdir(folder) == 0);
}

static void imported_generic(void) {
    char folder[] = "/tmp/mo-generic-imports-XXXXXX", main_path[128], other_path[128];
    MO_ASSERT(mkdtemp(folder) != NULL);
    int n = snprintf(main_path, sizeof(main_path), "%s/main.mo", folder);
    MO_ASSERT(n > 0 && (size_t)n < sizeof(main_path));
    n = snprintf(other_path, sizeof(other_path), "%s/numbers.mo", folder);
    MO_ASSERT(n > 0 && (size_t)n < sizeof(other_path));
    const char other[] = "module Numbers\nexpose twice\n"
                         "fn twice(value: T) : T where T: Number\nvalue + value\nend\n";
    write_file(other_path, (const uint8_t *)other, sizeof(other) - 1);
    for (unsigned variant = 0; variant < 4; variant++) {
        char main[512];
        n = snprintf(main, sizeof(main),
                     "module Main\nuse Numbers%s\n"
                     "fn relay(value: T) : T%s\n%s\nend\n"
                     "fn main() : Int\nrelay(19)\nend\n",
                     variant == 3 ? "" : "{twice}", variant != 0 ? " where T: Number" : "",
                     variant == 2   ? "apply = twice\napply(value)"
                     : variant == 3 ? "Numbers.twice(value)"
                                    : "twice(value)");
        MO_ASSERT(n > 0 && (size_t)n < sizeof(main));
        write_file(main_path, (const uint8_t *)main, (size_t)n);
        struct fixture f;
        init(&f, MO_CLI_CHECK_SIZE);
        struct mo_module_set *set =
            mo_load_program(&f.syntax, &f.sources, main_path, NULL, &f.diags);
        MO_ASSERT(set != NULL && !mo_diags_any(&f.diags));
        struct mo_program *program = mo_check(&f.arena, set, &f.diags);
        if (variant == 0) {
            CHECK(program == NULL && f.diags.count == 1 &&
                  f.diags.items[0].code == MO_CODE_ABILITY_MISSING);
        } else {
            CHECK(program != NULL && !mo_diags_any(&f.diags));
            if (program != NULL) {
                const struct mo_symbol *twice =
                    mo_find_symbol(&program->interfaces->scopes[1], (const uint8_t *)"twice", 5);
                const struct mo_checked_function *callee = NULL, *caller = NULL;
                for (uint32_t i = 0; i < program->function_count; i++) {
                    const struct mo_checked_function *fn = &program->functions[i];
                    if (fn->signature->function == twice)
                        callee = fn;
                    else if (strcmp(fn->signature->function->name, "relay") == 0)
                        caller = fn;
                }
                MO_ASSERT(callee != NULL && caller != NULL);
                const struct mo_source_type *key = callee->signature->type->arguments[0];
                const struct mo_source_type *actual = caller->signature->type->arguments[0];
                CHECK(key != actual && key->declaration == twice &&
                      actual->declaration == caller->signature->function);
                bool found = false;
                for (uint32_t i = 0; i < caller->body.count; i++) {
                    const struct mo_checked_expression *row = &caller->body.items[i];
                    if (row->symbol != twice)
                        continue;
                    MO_ASSERT(row->arguments.count == 1 && row->ability_count == 1);
                    CHECK(row->arguments.items[0].parameter == key &&
                          row->arguments.items[0].type == actual);
                    CHECK(row->abilities[0].parameter == key &&
                          row->abilities[0].operand == UINT32_MAX &&
                          row->abilities[0].implementation->kind == MO_ABILITY_PARAMETER &&
                          row->abilities[0].implementation->ability == MO_SOURCE_NUMBER &&
                          row->abilities[0].implementation->type == actual);
                    found = true;
                }
                CHECK(found);
            }
        }
        finish(&f);
    }
    MO_ASSERT(unlink(main_path) == 0 && unlink(other_path) == 0 && rmdir(folder) == 0);
}

static void generic_import_sources(void) {
    static const char library[] =
        "module Numbers\nexpose twice, identity, show, same, hashed, ordered, mix\n"
        "fn twice(value: T) : T where T: Number\nvalue + value\nend\n"
        "fn identity(value: T) : T\nvalue\nend\n"
        "fn show(value: T) : Text where T: Text\n\"#{value}\"\nend\n"
        "fn same(a: A, b: B) : Bool where A: Equal, B: Equal\na == a and b == b\nend\n"
        "fn hashed(value: T) : T where T: Hash\nvalue\nend\n"
        "fn ordered(a: T, b: T) : Bool where T: Order\na < b\nend\n"
        "fn mix(first: Z, second: A) : Z where Z: Number, A: Text\n"
        "_label = \"#{second}\"\nfirst + first\nend\n";
    static const struct {
        const char *name, *source;
        uint16_t code;
    } cases[] = {
        {"Number scalars",
         "fn big(value: BigInt) : BigInt\nNumbers.twice(value)\nend\n"
         "fn decimal(value: Decimal) : Decimal\nNumbers.twice(value)\nend\n"
         "fn float(value: Float) : Float\nNumbers.twice(value)\nend\n"
         "fn main() : Int\nNumbers.twice(19)\nend\n",
         0},
        {"first-class return",
         "fn make() : fn(Int) : Int\nNumbers.twice\nend\n"
         "fn main() : Int\napply = make()\napply(19)\nend\n",
         0},
        {"fresh first-class references",
         "fn main() : Int\na = Numbers.identity\nb = Numbers.identity\n"
         "assert b(\"text\") == \"text\"\na(19)\nend\n",
         0},
        {"higher-order context",
         "fn apply(action: fn(Int) : Int) : Int\naction(19)\nend\n"
         "fn main() : Int\napply(Numbers.twice)\nend\n",
         0},
        {"permuted parameters",
         "fn relay(first: U, second: T) : U where U: Number, T: Text\n"
         "apply = Numbers.mix\napply(first, second)\nend\n"
         "fn main() : Int\nassert Numbers.same(13, 29)\nrelay(19, true)\nend\n",
         0},
        {"derived and custom Text",
         "struct Box(T)\nvalue: T\nend\n"
         "fn to_text(box: Box(U)) : Text where U: Text\n\"[#{box.value}]\"\nend\n"
         "fn relay(value: U) : Text where U: Text\nNumbers.show([value])\nend\n"
         "fn main() : Int\nNumbers.show(Box(value: 19)).byte_count + relay(true).byte_count\nend\n",
         0},
        {"Hash Order and implied Equal",
         "fn ordered_same(value: U) : Bool where U: Order\nNumbers.same(value, value)\nend\n"
         "fn main() : Int\nassert Numbers.ordered(13, 29) and ordered_same(19)\n"
         "Numbers.hashed(19)\nend\n",
         0},
        {"Bool lacks Number",
         "fn bad(value: Bool) : Bool\nNumbers.twice(value)\nend\n"
         "fn main() : Int\n0\nend\n",
         MO_CODE_ABILITY_MISSING},
        {"Equal does not imply Number",
         "fn bad(value: T) : T where T: Equal\nNumbers.twice(value)\nend\n"
         "fn main() : Int\n0\nend\n",
         MO_CODE_ABILITY_MISSING},
        {"first-class missing proof",
         "fn bad() : fn(Bool) : Bool\nNumbers.twice\nend\n"
         "fn main() : Int\n0\nend\n",
         MO_CODE_ABILITY_MISSING},
        {"functions lack Equal",
         "fn bad(value: fn(Int) : Int) : Bool\nNumbers.same(value, value)\nend\n"
         "fn main() : Int\n0\nend\n",
         MO_CODE_ABILITY_MISSING},
        {"capabilities lack Text",
         "fn bad(value: Console) : Text\nNumbers.show(value)\nend\n"
         "fn main() : Int\n0\nend\n",
         MO_CODE_ABILITY_MISSING},
        {"Float lacks Hash",
         "fn bad(value: Float) : Float\nNumbers.hashed(value)\nend\n"
         "fn main() : Int\n0\nend\n",
         MO_CODE_ABILITY_MISSING},
        {"Range lacks Order",
         "fn bad(value: Range) : Bool\nNumbers.ordered(value, value)\nend\n"
         "fn main() : Int\n0\nend\n",
         MO_CODE_ABILITY_MISSING},
        {"Bytes lack Text",
         "fn bad(value: Bytes) : Text\nNumbers.show(value)\nend\n"
         "fn main() : Int\n0\nend\n",
         MO_CODE_ABILITY_MISSING},
        {"generic direct initializer", "answer = Numbers.twice(19)\nfn main() : Int\nanswer\nend\n",
         0},
        {"generic initializer alias needs a closed type",
         "handler = Numbers.twice\nfn main() : Int\nhandler(19)\nend\n", MO_CODE_CANNOT_INFER},
    };
    char folder[] = "/tmp/mo-generic-sources-XXXXXX", path[128], other[128];
    MO_ASSERT(mkdtemp(folder) != NULL);
    int n = snprintf(path, sizeof(path), "%s/main.mo", folder);
    MO_ASSERT(n > 0 && (size_t)n < sizeof(path));
    n = snprintf(other, sizeof(other), "%s/numbers.mo", folder);
    MO_ASSERT(n > 0 && (size_t)n < sizeof(other));
    write_file(other, (const uint8_t *)library, sizeof(library) - 1);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char source[2048];
        n = snprintf(source, sizeof(source), "module Main\nuse Numbers\n%s", cases[i].source);
        MO_ASSERT(n > 0 && (size_t)n < sizeof(source));
        write_file(path, (const uint8_t *)source, (size_t)n);
        struct fixture f;
        init(&f, MO_CLI_CHECK_SIZE);
        struct mo_module_set *set = mo_load_program(&f.syntax, &f.sources, path, NULL, &f.diags);
        struct mo_program *program = set == NULL ? NULL : mo_check(&f.arena, set, &f.diags);
        bool passed = cases[i].code == 0 ? program != NULL && !mo_diags_any(&f.diags)
                                         : program == NULL && f.diags.count == 1 &&
                                               f.diags.items[0].code == cases[i].code;
        if (!passed)
            MO_ASSERT(fprintf(stderr,
                              "generic import %s: expected E%04u, found E%04u (%u errors)\n",
                              cases[i].name, (unsigned)cases[i].code,
                              f.diags.count == 0 ? 0u : (unsigned)f.diags.items[0].code,
                              f.diags.count) >= 0);
        CHECK(passed);
        if (program != NULL) {
            for (uint32_t j = 0; j < program->function_count; j++) {
                const struct mo_checked_function *fn = &program->functions[j];
                for (uint32_t k = 0; k < fn->body.count; k++) {
                    const struct mo_checked_expression *row = &fn->body.items[k];
                    if (row->symbol == NULL || row->symbol->declaration->kind != MO_S_FN ||
                        row->symbol->module == fn->signature->function->module)
                        continue;
                    for (uint32_t a = 0; a < row->arguments.count; a++)
                        CHECK(row->arguments.items[a].parameter->declaration == row->symbol);
                    for (uint32_t a = 0; a < row->ability_count; a++) {
                        const struct mo_checked_ability_use *use = &row->abilities[a];
                        bool keyed = false;
                        for (uint32_t b = 0; b < row->arguments.count; b++) {
                            const struct mo_type_argument *argument = &row->arguments.items[b];
                            if (use->parameter == argument->parameter) {
                                CHECK(use->operand == UINT32_MAX &&
                                      same_source_type(use->implementation->type, argument->type));
                                keyed = true;
                            }
                        }
                        CHECK(keyed);
                    }
                    if (strcmp(row->symbol->name, "mix") == 0) {
                        MO_ASSERT(row->arguments.count == 2 && row->ability_count == 2);
                        for (uint32_t a = 0; a < 2; a++) {
                            const struct mo_type_argument *argument = &row->arguments.items[a];
                            bool text = strcmp(argument->parameter->name, "A") == 0;
                            CHECK(strcmp(argument->parameter->name, text ? "A" : "Z") == 0);
                            CHECK(argument->type == fn->signature->type->arguments[text ? 1 : 0]);
                            CHECK(row->abilities[a].implementation->kind == MO_ABILITY_PARAMETER);
                            CHECK(row->abilities[a].implementation->ability ==
                                  (text ? MO_SOURCE_TEXT : MO_SOURCE_NUMBER));
                        }
                    }
                    if (strcmp(row->symbol->name, "same") == 0) {
                        MO_ASSERT(row->arguments.count == 2 && row->ability_count == 2);
                        CHECK(row->abilities[0].parameter != row->abilities[1].parameter);
                    }
                    if (strcmp(row->symbol->name, "show") == 0 && row->ability_count != 0 &&
                        row->abilities[0].implementation->kind == MO_ABILITY_FUNCTION) {
                        const struct mo_ability_implementation *proof =
                            row->abilities[0].implementation;
                        MO_ASSERT(proof->part_count == 1 && proof->arguments.count == 1);
                        CHECK(proof->function->module == fn->signature->function->module);
                        CHECK(proof->part_parameters[0] == proof->arguments.items[0].parameter &&
                              proof->part_parameters[0]->declaration == proof->function &&
                              proof->part_parameters[0] != row->abilities[0].parameter);
                    }
                }
            }
        }
        finish(&f);
    }
    printf("generic import sources: %zu public load/check controls\n",
           sizeof(cases) / sizeof(cases[0]));
    MO_ASSERT(unlink(path) == 0 && unlink(other) == 0 && rmdir(folder) == 0);
}

static void import_sources(void) {
    static const char library[] =
        "module Shapes\nexpose Shape(..), make, area, Token, open, read, Choice(..), pick, _extra\n"
        "struct Shape\nvalue: Int\nend\nstruct Token\nvalue: Int\nend\n"
        "enum Choice\nLeft\nRight\nend\nstruct Hidden\nvalue: Int\nend\n"
        "fn make(value: Int) : Shape\nShape(value: value)\nend\n"
        "fn area(value: Shape) : Int\nvalue.value\nend\n"
        "fn open(value: Int) : Token\nToken(value: value)\nend\n"
        "fn read(value: Token) : Int\nvalue.value\nend\n"
        "fn pick() : Choice\nLeft\nend\nfn secret() : Int\n71\nend\n"
        "fn _extra() : Int\n73\nend\n";
    static const char generic[] =
        "module Shapes\nexpose Pair(..), Token, open, read\n"
        "struct Pair(Z, A)\nfirst: Z\nsecond: A\nend\n"
        "struct Token(T)\nvalue: T\nend\nstruct Hidden(T)\nvalue: T\nend\n"
        "fn open(value: T) : Token(T)\nToken(value: value)\nend\n"
        "fn read(value: Token(T)) : T\nvalue.value\nend\n"
        "fn to_text(value: Token(U)) : Text where U: Text\n\"#{value.value}\"\nend\n";
    static const struct {
        const char *name, *source, *other, *site;
        uint16_t code;
    } cases[] = {
        {"qualified calls", "use Shapes\nfn main() : Int\nShapes.area(Shapes.make(19))\nend\n",
         NULL, NULL, 0},
        {"explicit calls", "use Shapes{make, area}\nfn main() : Int\narea(make(19))\nend\n", NULL,
         NULL, 0},
        {"explicit annotation",
         "use Shapes{Shape}\nfn keep(value: Shape) : Shape\nvalue\nend\n"
         "fn main() : Int\n0\nend\n",
         NULL, NULL, 0},
        {"qualified annotation",
         "use Shapes\nfn keep(value: Shapes.Shape) : Shapes.Shape\n"
         "value\nend\nfn main() : Int\n0\nend\n",
         NULL, NULL, 0},
        {"field annotation",
         "use Shapes{Shape}\nstruct Wrap\nvalue: Shape\nend\n"
         "fn main() : Int\n0\nend\n",
         NULL, NULL, 0},
        {"constructor", "use Shapes{Shape}\nfn main() : Int\nShape(value: 19).value\nend\n", NULL,
         NULL, 0},
        {"qualified constructor",
         "use Shapes\nfn main() : Int\nShapes.Shape(value: 19).value\nend\n", NULL, NULL, 0},
        {"first class",
         "use Shapes{make, area}\nfn main() : Int\n"
         "build = make\nmeasure = area\nmeasure(build(19))\nend\n",
         NULL, NULL, 0},
        {"case credits type",
         "use Shapes{Choice}\nfn main() : Int\n"
         "case Left\nLeft: 13\nRight: 29\nend\nend\n",
         NULL, NULL, 0},
        {"qualified cases",
         "use Shapes\nfn main() : Int\ncase Shapes.pick()\n"
         "Shapes.Left: 13\nShapes.Right: 29\nend\nend\n",
         NULL, NULL, 0},
        {"opaque factory",
         "use Shapes{Token, open, read}\nfn inspect(value: Token) : Int\n"
         "read(value)\nend\nfn main() : Int\ninspect(open(19))\nend\n",
         NULL, NULL, 0},
        {"test only",
         "use Shapes\ntest \"import\"\n"
         "assert Shapes.area(Shapes.make(19)) == 19\nend\nfn main()\nend\n",
         NULL, NULL, 0},
        {"example only",
         "use Shapes\nfn answer() : Int\nexamples\n"
         "answer() == Shapes.area(Shapes.make(19))\nend\n19\nend\n"
         "fn main() : Int\nanswer()\nend\n",
         NULL, NULL, 0},
        {"purposefully unused", "use Shapes{_extra}\nfn main()\nend\n", NULL, NULL, 0},
        {"unused module", "use Shapes\nfn main()\nend\n", NULL, "Shapes\n", MO_CODE_UNUSED_IMPORT},
        {"unused name", "use Shapes{Shape, make, area}\nfn main() : Int\narea(make(19))\nend\n",
         NULL, "Shape,", MO_CODE_UNUSED_IMPORT},
        {"qualification does not read listed name",
         "use Shapes{make}\n"
         "fn main() : Int\nShapes.area(Shapes.make(19))\nend\n",
         NULL, "make}", MO_CODE_UNUSED_IMPORT},
        {"local spelling does not read import",
         "use Shapes{area}\n"
         "fn main() : Int\narea = 19\narea\nend\n",
         NULL, "area}", MO_CODE_UNUSED_IMPORT},
        {"dot owner does not read listed name",
         "use Shapes{make, area}\n"
         "fn main() : Int\nmake(19).area\nend\n",
         NULL, "area}", MO_CODE_UNUSED_IMPORT},
        {"private function", "use Shapes\nfn main() : Int\nShapes.secret()\nend\n", NULL,
         "secret()", MO_CODE_IMPORT_PRIVATE},
        {"private explicit name", "use Shapes{secret}\nfn main() : Int\nsecret()\nend\n", NULL,
         "secret}", MO_CODE_IMPORT_PRIVATE},
        {"private type",
         "use Shapes\nfn keep(value: Shapes.Hidden) : Shapes.Hidden\nvalue\nend\n"
         "fn main()\nend\n",
         NULL, "Shapes.Hidden", MO_CODE_IMPORT_PRIVATE},
        {"opaque construction",
         "use Shapes{Token, read}\nfn main() : Int\n"
         "read(Token(value: 19))\nend\n",
         NULL, NULL, MO_CODE_DATA_PRIVATE},
        {"opaque field", "use Shapes{open}\nfn main() : Int\nopen(19).value\nend\n", NULL,
         "value\n", MO_CODE_DATA_PRIVATE},
        {"opaque pattern",
         "use Shapes{Token, open}\nfn main() : Int\n"
         "case open(19)\nToken(value: n): n\nend\nend\n",
         NULL, NULL, MO_CODE_DATA_PRIVATE},
        {"distinct nominal owners",
         "use Shapes\nstruct Shape\nvalue: Int\nend\n"
         "fn main() : Int\nShapes.area(Shape(value: 19))\nend\n",
         NULL, NULL, MO_CODE_TYPE_MISMATCH},
        {"generic function", "use Shapes{identity}\nfn main() : Int\nidentity(19)\nend\n",
         "module Shapes\nexpose identity\nfn identity(value: T) : T\nvalue\nend\n", NULL, 0},
        {"generic annotation",
         "use Shapes{Box}\nfn keep(value: Box(Int)) : Box(Int)\n"
         "value\nend\nfn main()\nend\n",
         "module Shapes\nexpose Box(..)\nstruct Box(T)\nvalue: T\nend\n", NULL, 0},
        {"generic construction",
         "use Shapes{Box}\nfn main() : Int\n"
         "Box(value: 19).value\nend\n",
         "module Shapes\nexpose Box(..)\nstruct Box(T)\nvalue: T\nend\n", NULL, 0},
        {"generic qualified nested annotation",
         "use Shapes\nfn first(pair: Shapes.Pair(List(Int), (Bool, Int))) : List(Int)\n"
         "pair.first\nend\nfn main() : Int\n"
         "assert first(Shapes.Pair(second: (true, 7), first: [13])) == [13]\n19\nend\n",
         generic, NULL, 0},
        {"generic opaque flow and customizer",
         "use Shapes{Token, open, read}\nfn keep(value: Token(Int)) : Token(Int)\nvalue\nend\n"
         "fn main() : Int\nvalue = keep(open(13))\nassert \"#{value}\" == \"13\"\n"
         "read(value)\nend\n",
         generic, NULL, 0},
        {"generic opaque construction",
         "use Shapes{Token, read}\nfn main() : Int\nread(Token(value: 13))\nend\n", generic, NULL,
         MO_CODE_DATA_PRIVATE},
        {"generic opaque projection", "use Shapes{open}\nfn main() : Int\nopen(13).value\nend\n",
         generic, "value\n", MO_CODE_DATA_PRIVATE},
        {"generic opaque pattern",
         "use Shapes{Token, open}\nfn main() : Int\ncase open(13)\nToken(value: n): n\nend\nend\n",
         generic, NULL, MO_CODE_DATA_PRIVATE},
        {"generic private annotation",
         "use Shapes\nfn keep(value: Shapes.Hidden(Int)) : Shapes.Hidden(Int)\nvalue\nend\n"
         "fn main()\nend\n",
         generic, "Shapes.Hidden", MO_CODE_IMPORT_PRIVATE},
        {"generic distinct nominal owners",
         "use Shapes\nstruct Pair(Z, A)\nfirst: Z\nsecond: A\nend\n"
         "fn wrong(value: Pair(Int, Bool)) : Shapes.Pair(Int, Bool)\nvalue\nend\n"
         "fn main()\nend\n",
         generic, NULL, MO_CODE_RESULT_TYPE},
        {"generic wrong arity",
         "use Shapes{Pair}\nfn keep(value: Pair(Int)) : Pair(Int)\nvalue\nend\n"
         "fn main()\nend\n",
         generic, NULL, MO_CODE_TYPE_ARGUMENTS},
        {"generic missing field", "use Shapes{Pair}\nfn main() : Int\nPair(first: 13).first\nend\n",
         generic, NULL, MO_CODE_DATA_ARGUMENTS},
        {"generic regular recursion",
         "use Shapes{Tree}\nfn keep(value: Tree(Int)) : Tree(Int)\nvalue\nend\n"
         "fn main() : Int\nassert keep(Tree(children: [])) == Tree(children: [])\n19\nend\n",
         "module Shapes\nexpose Tree(..)\nstruct Tree(T)\nchildren: List(Tree(T))\nend\n", NULL, 0},
        {"generic growing recursion",
         "use Shapes{Tree}\nfn keep(value: Tree(Int)) : Tree(Int)\nvalue\nend\n"
         "fn main()\nend\n",
         "module Shapes\nexpose Tree(..)\nstruct Tree(T)\nchildren: List(Tree(List(T)))\nend\n",
         NULL, MO_CODE_DATA_RECURSION},
        {"imported constant", "use Shapes{answer}\nfn main() : Int\nanswer\nend\n",
         "module Shapes\nexpose answer\nanswer = 19\n", NULL, 0},
        {"initializer import",
         "use Shapes{value}\nanswer = value()\n"
         "fn main() : Int\nanswer\nend\n",
         "module Shapes\nexpose value\nfn value() : Int\n19\nend\n", NULL, 0},
    };
    char folder[] = "/tmp/mo-import-sources-XXXXXX";
    MO_ASSERT(mkdtemp(folder) != NULL);
    char path[128], other[128];
    int n = snprintf(path, sizeof(path), "%s/main.mo", folder);
    MO_ASSERT(n > 0 && (size_t)n < sizeof(path));
    n = snprintf(other, sizeof(other), "%s/shapes.mo", folder);
    MO_ASSERT(n > 0 && (size_t)n < sizeof(other));
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char source[2048];
        n = snprintf(source, sizeof(source), "module Main\n%s", cases[i].source);
        MO_ASSERT(n > 0 && (size_t)n < sizeof(source));
        const char *text = cases[i].other == NULL ? library : cases[i].other;
        write_file(path, (const uint8_t *)source, (size_t)n);
        write_file(other, (const uint8_t *)text, strlen(text));
        struct fixture f;
        init(&f, MO_CLI_CHECK_SIZE);
        struct mo_module_set *set = mo_load_program(&f.syntax, &f.sources, path, NULL, &f.diags);
        struct mo_program *program = set == NULL ? NULL : mo_check(&f.arena, set, &f.diags);
        mo_sources_free(&f.sources);
        bool passed = cases[i].code == 0 ? program != NULL && !mo_diags_any(&f.diags)
                                         : program == NULL && f.diags.count == 1 &&
                                               f.diags.items[0].code == cases[i].code;
        if (!passed)
            MO_ASSERT(fprintf(stderr, "import source %s: expected E%04u, found E%04u (%u errors)\n",
                              cases[i].name, (unsigned)cases[i].code,
                              f.diags.count == 0 ? 0u : (unsigned)f.diags.items[0].code,
                              f.diags.count) >= 0);
        CHECK(passed);
        if (cases[i].site != NULL && f.diags.count != 0) {
            const char *site = strstr(source, cases[i].site);
            MO_ASSERT(site != NULL);
            CHECK(f.diags.items[0].primary.span.file == 0 &&
                  f.diags.items[0].primary.span.start == (uint32_t)(site - source));
        }
        finish(&f);
    }
    printf("import sources: %zu public load/check controls\n", sizeof(cases) / sizeof(cases[0]));
    MO_ASSERT(unlink(path) == 0 && unlink(other) == 0 && rmdir(folder) == 0);
}

static void import_cap(void) {
    char folder[] = "/tmp/mo-import-cap-XXXXXX", path[128], other[128];
    MO_ASSERT(mkdtemp(folder) != NULL);
    int n = snprintf(path, sizeof(path), "%s/main.mo", folder);
    MO_ASSERT(n > 0 && (size_t)n < sizeof(path));
    n = snprintf(other, sizeof(other), "%s/shapes.mo", folder);
    MO_ASSERT(n > 0 && (size_t)n < sizeof(other));
    struct mo_buffer main, library, functions;
    mo_buffer_init(&main, MO_LIMIT_FILE_SIZE);
    mo_buffer_init(&library, MO_LIMIT_FILE_SIZE);
    mo_buffer_init(&functions, MO_LIMIT_FILE_SIZE);
    MO_ASSERT(mo_buffer_append_text(&main, "module Main\nuse Shapes{"));
    MO_ASSERT(mo_buffer_append_text(&library, "module Shapes\nexpose "));
    for (uint32_t i = 0; i < 1500; i++) {
        if (i != 0)
            MO_ASSERT(mo_buffer_append_text(&main, ", ") && mo_buffer_append_text(&library, ", "));
        MO_ASSERT(mo_buffer_append_text(&main, "f") && mo_buffer_append_uint(&main, i) &&
                  mo_buffer_append_text(&library, "f") && mo_buffer_append_uint(&library, i));
        MO_ASSERT(mo_buffer_append_text(&functions, "fn f") &&
                  mo_buffer_append_uint(&functions, i) &&
                  mo_buffer_append_text(&functions, "() : Int\n19\nend\n"));
    }
    MO_ASSERT(mo_buffer_append_text(&main, "}\nfn main()\nend\n") &&
              mo_buffer_append_text(&library, "\n") &&
              mo_buffer_append(&library, functions.bytes, functions.length));
    write_file(path, main.bytes, main.length);
    write_file(other, library.bytes, library.length);
    struct fixture f;
    init(&f, MO_CLI_CHECK_SIZE);
    struct mo_module_set *set = mo_load_program(&f.syntax, &f.sources, path, NULL, &f.diags);
    MO_ASSERT(set != NULL && f.diags.count == 0);
    CHECK(mo_check(&f.arena, set, &f.diags) == NULL);
    CHECK(f.diags.count == 1000 && f.diags.dropped == 500);
    for (uint32_t i = 0; i < f.diags.count; i++)
        CHECK(f.diags.items[i].code == MO_CODE_UNUSED_IMPORT);
    finish(&f);
    mo_buffer_free(&main);
    mo_buffer_free(&library);
    mo_buffer_free(&functions);
    MO_ASSERT(unlink(path) == 0 && unlink(other) == 0 && rmdir(folder) == 0);
}

static void captured_parameter(void) {
    char folder[] = "/tmp/mo-captures-XXXXXX";
    MO_ASSERT(mkdtemp(folder) != NULL);
    char path[128];
    int length = snprintf(path, sizeof(path), "%s/main.mo", folder);
    MO_ASSERT(length > 0 && (size_t)length < sizeof(path));
    const char source[] = "module Main\nfn make(value: Int) : fn() : Int\n"
                          "fn() value + value end\nend\nfn main() : Int\nmake(19)()\nend\n";
    write_file(path, (const uint8_t *)source, sizeof(source) - 1);
    struct fixture f;
    init(&f, 64 << 20);
    struct mo_module_set *set = mo_load_program(&f.syntax, &f.sources, path, NULL, &f.diags);
    MO_ASSERT(set != NULL && f.diags.count == 0);
    struct mo_program *program = mo_check(&f.arena, set, &f.diags);
    CHECK(program != NULL && f.diags.count == 0);
    if (program != NULL) {
        const struct mo_checked_function *make = &program->functions[0];
        const struct mo_checked_lambda *lambda = NULL;
        for (uint32_t i = 0; i < make->body.count; i++)
            if (make->body.items[i].lambda != NULL)
                lambda = make->body.items[i].lambda;
        MO_ASSERT(lambda != NULL);
        CHECK(lambda->capture_count == 1);
        if (lambda->capture_count == 1)
            CHECK(lambda->captures[0] == make->parameters[0]);
    }
    finish(&f);
    MO_ASSERT(unlink(path) == 0 && rmdir(folder) == 0);
}

static void capture_sources(void) {
    static const struct {
        const char *name, *source, *site;
        uint16_t code;
    } cases[] = {
        {"copied snapshot",
         "fn main() : Int\nvar count = 7\ncurrent = count\n"
         "show = fn() current end\ncount += 19\nshow() * 100 + count\nend\n",
         NULL, 0},
        {"stable var",
         "fn main() : Int\nvar count = 7\ncount += 19\n"
         "show = fn() count end\nshow()\nend\n",
         NULL, 0},
        {"later write",
         "fn main() : Int\nvar count = 7\nshow = fn() count end\n"
         "count += 19\nshow()\nend\n",
         "count end", MO_CODE_CAPTURE_ASSIGNMENT},
        {"assignment RHS capture",
         "fn invoke(action: fn() : Int) : Int\naction()\nend\n"
         "fn main() : Int\nvar count = 7\ncount = invoke(fn() count end)\ncount\nend\n",
         "count end", MO_CODE_CAPTURE_ASSIGNMENT},
        {"transitive later write",
         "fn main() : Int\nvar count = 7\n"
         "show = fn() fn() count end end\ncount = 29\nshow()()\nend\n",
         "count end", MO_CODE_CAPTURE_ASSIGNMENT},
        {"previous loop write",
         "fn main()\nvar count = 7\nfor _ in 0..3\n"
         "count += 1\nshow = fn() count end\nassert show() > 0\nend\nend\n",
         "count end", MO_CODE_CAPTURE_ASSIGNMENT},
        {"per-round var",
         "fn main()\nfor n in 0..3\nvar count = n\ncount += 1\n"
         "show = fn() count end\nassert show() > 0\nend\nend\n",
         NULL, 0},
        {"per-round copy",
         "fn main()\nvar count = 7\nfor _ in 0..3\ncurrent = count\n"
         "show = fn() current end\ncount += 1\nassert show() > 0\nend\nend\n",
         NULL, 0},
        {"nested loop write",
         "fn main()\nfor n in 0..3\nvar count = n\n"
         "for _ in 0..2\ncount += 1\nshow = fn() count end\n"
         "assert show() > 0\nend\nend\nend\n",
         "count end", MO_CODE_CAPTURE_ASSIGNMENT},
        {"exclusive branches",
         "fn make(flag: Bool) : fn() : Int\nvar count = 7\n"
         "if flag\nfn() count end\nelse\ncount += 19\nfn() count end\nend\n"
         "end\nfn main() : Int\nmake(true)() + make(false)()\nend\n",
         NULL, 0},
        {"exclusive rounds",
         "fn main()\nvar count = 7\nfor n in 0..3\n"
         "if n == 0\nshow = fn() count end\nassert show() > 0\n"
         "else\ncount += 1\nend\nend\nend\n",
         "count end", MO_CODE_CAPTURE_ASSIGNMENT},
        {"own var write", "fn main() : Int\nf = fn()\nvar n = 3\nn += 1\nn\nend\nf()\nend\n",
         "n +=", MO_CODE_LAMBDA_WRITE},
        {"own field write",
         "struct Box\nvalue: Int\nend\nfn main() : Int\n"
         "f = fn()\nvar box = Box(value: 3)\nbox.value = 7\nbox.value\n"
         "end\nf()\nend\n",
         "box.value =", MO_CODE_LAMBDA_WRITE},
        {"outer write", "fn main() : Int\nvar n = 3\nf = fn()\nn = 7\nend\nf()\nn\nend\n", "n = 7",
         MO_CODE_NOT_VARIABLE},
        {"captured field update",
         "struct Box\nvalue: Int\nend\nfn main() : Int\n"
         "var box = Box(value: 3)\nf = fn() box.value end\n"
         "box.value += 19\nf()\nend\n",
         "box.value end", MO_CODE_CAPTURE_ASSIGNMENT},
        {"copied record",
         "struct Box\nvalue: Int\nend\nfn main() : Int\n"
         "var box = Box(value: 3)\ncopy = box\nf = fn() copy.value end\n"
         "box.value += 19\nf()\nend\n",
         NULL, 0},
        {"capability parameter",
         "fn make() : fn(Console)\n"
         "fn(console) console.print(\"ok\") end\nend\n"
         "fn main(console: Console)\nmake()(console)\nend\n",
         NULL, 0},
        {"direct capability",
         "fn main(console: Console)\n"
         "f = fn() console.print(\"bad\") end\nf()\nend\n",
         "console.print", MO_CODE_CAPABILITY_CAPTURE},
        {"capture before body error",
         "fn make(console: Console) : fn() : Text\n"
         "fn()\nconsole.print(\"bad\")\nunknown\nend\nend\n"
         "fn main()\nend\n",
         "console.print", MO_CODE_CAPABILITY_CAPTURE},
        {"stored capability",
         "struct Holder\nconsole: Console\nend\n"
         "fn make(holder: Holder) : fn()\n"
         "fn() holder.console.print(\"bad\") end\nend\nfn main()\nend\n",
         "holder.console", MO_CODE_CAPABILITY_CAPTURE},
        {"stored generic",
         "struct Box(T)\nvalue: T\nend\n"
         "fn make(value: Box(Console)) : fn() : Box(Console)\n"
         "fn() value end\nend\nfn main()\nend\n",
         "value end", MO_CODE_CAPABILITY_CAPTURE},
        {"recursive safe",
         "enum Ring(T)\nEmpty\nNext(Ring(T))\nCell(T)\nend\n"
         "fn make(value: Ring(Int)) : fn() : Ring(Int)\n"
         "fn() value end\nend\nfn main()\nend\n",
         NULL, 0},
        {"recursive capability",
         "enum Ring(T)\nEmpty\nNext(Ring(T))\nCell(T)\nend\n"
         "fn make(value: Ring(Console)) : fn() : Ring(Console)\n"
         "fn() value end\nend\nfn main()\nend\n",
         "value end", MO_CODE_CAPABILITY_CAPTURE},
        {"closed recursive capability",
         "enum A\nNilA\nFromB(B)\nend\n"
         "enum B\nNilB\nFromA(A)\nWithConsole(Console)\nend\n"
         "fn make(value: A) : fn() : A\n"
         "fn() value end\nend\nfn main()\nend\n",
         "value end", MO_CODE_CAPABILITY_CAPTURE},
        {"phantom capability",
         "enum Mark(T)\nPresent\nend\n"
         "fn make(value: Mark(Console)) : fn() : Mark(Console)\n"
         "fn() value end\nend\nfn main()\nend\n",
         NULL, 0},
        {"list capability",
         "fn make(value: List(Console)) : fn() : List(Console)\n"
         "fn() value end\nend\nfn main()\nend\n",
         "value end", MO_CODE_CAPABILITY_CAPTURE},
        {"tuple capability",
         "fn make(value: (Int, Option(Console))) : fn() : Int\n"
         "fn() value.0 end\nend\nfn main()\nend\n",
         "value.0", MO_CODE_CAPABILITY_CAPTURE},
        {"function parameter types",
         "fn make(action: fn(Console)) : fn(Console)\n"
         "fn(console) action(console) end\nend\nfn main()\nend\n",
         NULL, 0},
        {"Number capture",
         "fn make(value: T) : fn() : T where T: Number\n"
         "fn() value end\nend\nfn main() : Int\nmake(13)()\nend\n",
         NULL, 0},
        {"nested Number capture",
         "fn make(value: T) : fn(T) : fn() : T where T: Number\n"
         "fn(extra) fn() value + extra end end\nend\n"
         "fn main() : Int\nmake(13)(29)()\nend\n",
         NULL, 0},
        {"abstract capture",
         "fn make(value: T) : fn() : T\n"
         "fn() value end\nend\nfn main() : Int\nmake(13)()\nend\n",
         NULL, 0},
        {"abstract capture of a list",
         "fn count(items: List(T)) : Int\n"
         "f = fn(extra) items.size + extra end\nf(1)\nend\n"
         "fn main() : Int\ncount([\"a\", \"b\"])\nend\n",
         NULL, 0},
        {"abstract capture with a capability",
         "fn count(items: List(T)) : Int\n"
         "f = fn(extra) items.size + extra end\nf(1)\nend\n"
         "fn main(console: Console) : Int\ncount([console])\nend\n",
         "count([console])", MO_CODE_CAPABILITY_CAPTURE},
        {"abstract capture with a stored capability",
         "struct Holder\nconsole: Console\nend\n"
         "fn count(items: List(T)) : Int\n"
         "f = fn(extra) items.size + extra end\nf(1)\nend\n"
         "fn main(console: Console) : Int\ncount([Holder(console: console)])\nend\n",
         "count([Holder", MO_CODE_CAPABILITY_CAPTURE},
        {"abstract capture passed on",
         "fn count(items: List(T)) : Int\n"
         "f = fn(extra) items.size + extra end\nf(1)\nend\n"
         "fn relay(items: List(U)) : Int\ncount(items)\nend\n"
         "fn main() : Int\nrelay([1, 2])\nend\n",
         NULL, 0},
        {"abstract capture passed on with a capability",
         "fn count(items: List(T)) : Int\n"
         "f = fn(extra) items.size + extra end\nf(1)\nend\n"
         "fn relay(items: List(U)) : Int\ncount(items)\nend\n"
         "fn main(console: Console) : Int\nrelay([console])\nend\n",
         "relay([console])", MO_CODE_CAPABILITY_CAPTURE},
        {"abstract phantom capability",
         "enum Mark(T)\nPresent\nend\n"
         "fn count(mark: Mark(T)) : Int\n"
         "f = fn(extra) extra + mark_size(mark) end\nf(1)\nend\n"
         "fn mark_size(_mark: Mark(T)) : Int\n1\nend\n"
         "fn marked() : Mark(Console)\nPresent\nend\n"
         "fn main() : Int\ncount(marked())\nend\n",
         NULL, 0},
        {"Text isn't capability-free",
         "fn make(value: T) : fn() : Text where T: Text\n"
         "fn() value.to_text end\nend\nfn main()\nend\n",
         NULL, 0},
        {"higher-order initializer",
         "answer = invoke(fn() 13 end)\n"
         "fn invoke(action: fn() : Int) : Int\naction()\nend\n"
         "fn main() : Int\nanswer\nend\n",
         NULL, MO_CODE_CHECK_UNFINISHED},
    };
    char folder[] = "/tmp/mo-capture-sources-XXXXXX";
    MO_ASSERT(mkdtemp(folder) != NULL);
    char path[128];
    int length = snprintf(path, sizeof(path), "%s/main.mo", folder);
    MO_ASSERT(length > 0 && (size_t)length < sizeof(path));
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char source[4096];
        length = snprintf(source, sizeof(source), "module Main\n%s", cases[i].source);
        MO_ASSERT(length > 0 && (size_t)length < sizeof(source));
        write_file(path, (const uint8_t *)source, (size_t)length);
        struct fixture f;
        init(&f, MO_CLI_CHECK_SIZE);
        struct mo_module_set *set = mo_load_program(&f.syntax, &f.sources, path, NULL, &f.diags);
        MO_ASSERT(set != NULL && f.diags.count == 0);
        struct mo_program *program = mo_check(&f.arena, set, &f.diags);
        bool passed = cases[i].code == 0 ? program != NULL && !mo_diags_any(&f.diags)
                                         : program == NULL && f.diags.count == 1 &&
                                               f.diags.items[0].code == cases[i].code;
        if (!passed)
            MO_ASSERT(fprintf(stderr, "capture source %s: expected E%04u, found E%04u\n",
                              cases[i].name, (unsigned)cases[i].code,
                              f.diags.count == 0 ? 0u : (unsigned)f.diags.items[0].code) >= 0);
        CHECK(passed);
        if (cases[i].site != NULL && f.diags.count != 0) {
            const char *site = strstr(source, cases[i].site);
            MO_ASSERT(site != NULL);
            CHECK(f.diags.items[0].primary.span.start == (uint32_t)(site - source));
            if (cases[i].code == MO_CODE_CAPTURE_ASSIGNMENT ||
                cases[i].code == MO_CODE_CAPABILITY_CAPTURE)
                CHECK(f.diags.items[0].secondary_count == 1);
        }
        finish(&f);
    }
    printf("capture sources: %zu public load/check controls\n", sizeof(cases) / sizeof(cases[0]));
    MO_ASSERT(unlink(path) == 0 && rmdir(folder) == 0);
}

static void header_lookahead(void) {
    char folder[] = "/tmp/mo-check-header-XXXXXX";
    MO_ASSERT(mkdtemp(folder) != NULL);
    char path[256];
    int n = snprintf(path, sizeof(path), "%s/main.mo", folder);
    MO_ASSERT(n > 0 && (size_t)n < sizeof(path));
    const uint8_t text[] = "module Main\n\r*";
    write_file(path, text, sizeof(text) - 1);
    struct fixture f;
    init(&f, 16 << 20);
    CHECK(mo_load_program(&f.arena, &f.sources, path, NULL, &f.diags) == NULL);
    CHECK(f.diags.count == 1);
    finish(&f);
    MO_ASSERT(unlink(path) == 0);
    MO_ASSERT(rmdir(folder) == 0);
}

static void library_collision(void) {
    struct fixture f;
    init(&f, 16 << 20);
    CHECK(mo_load_program(&f.arena, &f.sources, "tests/check/load/main.mo",
                          "tests/check/interfaces", &f.diags) == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_MODULE_RESERVED);
    CHECK(f.diags.items[0].primary.span.file == 0);
    finish(&f);
}

static void path_policy(void) {
    char folder[] = "/tmp/mo-check-path-XXXXXX", library[] = "/tmp/mo-check-links-XXXXXX";
    MO_ASSERT(mkdtemp(folder) != NULL && mkdtemp(library) != NULL);
    char target[256], entry[256], loop[256], support[256], root[256];
    int a = snprintf(target, sizeof(target), "%s/source", folder);
    int b = snprintf(entry, sizeof(entry), "%s/xml-reader.mo", folder);
    int c = snprintf(loop, sizeof(loop), "%s/loop", library);
    int d = snprintf(support, sizeof(support), "%s/unicode16-tool.mo", library);
    int e = snprintf(root, sizeof(root), "%s/", folder);
    MO_ASSERT(a > 0 && (size_t)a < sizeof(target));
    MO_ASSERT(b > 0 && (size_t)b < sizeof(entry));
    MO_ASSERT(c > 0 && (size_t)c < sizeof(loop));
    MO_ASSERT(d > 0 && (size_t)d < sizeof(support));
    MO_ASSERT(e > 0 && (size_t)e < sizeof(root));
    const uint8_t source[] = "module XMLReader\n";
    const uint8_t other[] = "module Unicode16Tool\n";
    write_file(target, source, sizeof(source) - 1);
    write_file(support, other, sizeof(other) - 1);
    MO_ASSERT(symlink(target, entry) == 0 && symlink(".", loop) == 0);
    struct fixture f;
    init(&f, 16 << 20);
    struct mo_module_set *set = mo_load_program(&f.arena, &f.sources, entry, library, &f.diags);
    CHECK(set != NULL && f.diags.count == 0);
    if (set != NULL) {
        CHECK(strcmp(set->root, root) == 0);
        CHECK(strcmp(set->names[0], "XMLReader") == 0);
        CHECK(set->count == 2 && strcmp(set->names[1], "Unicode16Tool") == 0);
    }
    finish(&f);
    /* A library source link is confined to the library, not the program root. */
    MO_ASSERT(unlink(support) == 0 && symlink(target, support) == 0);
    init(&f, 16 << 20);
    CHECK(mo_load_program(&f.arena, &f.sources, entry, library, &f.diags) == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_MODULE_OUTSIDE_ROOT);
    CHECK(f.sources.count == 1);
    finish(&f);
    MO_ASSERT(unlink(entry) == 0 && unlink(target) == 0);
    MO_ASSERT(unlink(support) == 0 && unlink(loop) == 0);
    MO_ASSERT(rmdir(folder) == 0 && rmdir(library) == 0);
}

static void confined_files(void) {
    char root[] = "/tmp/mo-check-confined-XXXXXX";
    MO_ASSERT(mkdtemp(root) != NULL);
    char outside[256], entry[256], linked[256], target[256], normalized[256];
    int a = snprintf(outside, sizeof(outside), "%s-outside", root);
    int b = snprintf(entry, sizeof(entry), "%s/main.mo", root);
    int c = snprintf(linked, sizeof(linked), "%s/linked.mo", root);
    int d = snprintf(target, sizeof(target), "%s/linked.mo", outside);
    int e = snprintf(normalized, sizeof(normalized), "%s//./main.mo", root);
    MO_ASSERT(a > 0 && (size_t)a < sizeof(outside));
    MO_ASSERT(b > 0 && (size_t)b < sizeof(entry));
    MO_ASSERT(c > 0 && (size_t)c < sizeof(linked));
    MO_ASSERT(d > 0 && (size_t)d < sizeof(target));
    MO_ASSERT(e > 0 && (size_t)e < sizeof(normalized));
    MO_ASSERT(mkdir(outside, 0700) == 0);
    const uint8_t main_source[] = "module Main\nuse Linked\n";
    const uint8_t linked_source[] = "module Linked\n";
    write_file(entry, main_source, sizeof(main_source) - 1);
    write_file(target, linked_source, sizeof(linked_source) - 1);
    MO_ASSERT(symlink(target, linked) == 0);
    struct fixture f;
    init(&f, 16 << 20);
    CHECK(mo_load_program(&f.arena, &f.sources, normalized, NULL, &f.diags) == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_MODULE_OUTSIDE_ROOT);
    CHECK(f.sources.count == 1 && strcmp(f.sources.files[0].path, entry) == 0);
    finish(&f);
    MO_ASSERT(unlink(linked) == 0);
    MO_ASSERT(symlink("/dev/null", linked) == 0);
    init(&f, 16 << 20);
    CHECK(mo_load_program(&f.arena, &f.sources, entry, NULL, &f.diags) == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_MODULE_OUTSIDE_ROOT);
    CHECK(f.sources.count == 1);
    finish(&f);
    MO_ASSERT(unlink(linked) == 0);
    MO_ASSERT(mkfifo(linked, 0600) == 0);
    init(&f, 16 << 20);
    CHECK(mo_load_program(&f.arena, &f.sources, entry, NULL, &f.diags) == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_MODULE_MISSING);
    CHECK(f.sources.count == 1);
    finish(&f);
    MO_ASSERT(unlink(linked) == 0);
    write_file(linked, linked_source, sizeof(linked_source) - 1);
    init(&f, 16 << 20);
    struct mo_module_set *set = mo_load_program(&f.arena, &f.sources, normalized, NULL, &f.diags);
    CHECK(set != NULL && f.diags.count == 0);
    if (set != NULL)
        CHECK(strcmp(f.sources.files[0].path, entry) == 0 && set->count == 2);
    finish(&f);
    MO_ASSERT(unlink(entry) == 0);
    write_file(target, main_source, sizeof(main_source) - 1);
    MO_ASSERT(symlink(target, entry) == 0);
    init(&f, 16 << 20);
    CHECK(mo_load_program(&f.arena, &f.sources, entry, NULL, &f.diags) != NULL);
    CHECK(f.diags.count == 0);
    finish(&f);
    MO_ASSERT(unlink(entry) == 0 && unlink(linked) == 0 && unlink(target) == 0);
    MO_ASSERT(rmdir(root) == 0 && rmdir(outside) == 0);
}

static void library_roots(void) {
    char root[] = "/tmp/mo-check-roots-XXXXXX";
    MO_ASSERT(mkdtemp(root) != NULL);
    char entry[256], helper[256], library[256], support[256], lib_helper[256];
    int a = snprintf(entry, sizeof(entry), "%s/main.mo", root);
    int b = snprintf(helper, sizeof(helper), "%s/helper.mo", root);
    int c = snprintf(library, sizeof(library), "%s/lib", root);
    int d = snprintf(support, sizeof(support), "%s/support.mo", library);
    int e = snprintf(lib_helper, sizeof(lib_helper), "%s/helper.mo", library);
    MO_ASSERT(a > 0 && (size_t)a < sizeof(entry));
    MO_ASSERT(b > 0 && (size_t)b < sizeof(helper));
    MO_ASSERT(c > 0 && (size_t)c < sizeof(library));
    MO_ASSERT(d > 0 && (size_t)d < sizeof(support));
    MO_ASSERT(e > 0 && (size_t)e < sizeof(lib_helper));
    const uint8_t main_source[] = "module Main\nuse Helper\n";
    const uint8_t support_source[] = "module Support\nuse Helper\n";
    const uint8_t program_source[] = "module Helper\nanswer = 1\n";
    const uint8_t library_source[] = "module Helper\nanswer = 2\n";
    write_file(entry, main_source, sizeof(main_source) - 1);
    write_file(helper, program_source, sizeof(program_source) - 1);
    struct fixture f;
    init(&f, 16 << 20);
    CHECK(mo_load_program(&f.arena, &f.sources, entry, library, &f.diags) == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_LIBRARY_UNREADABLE);
    finish(&f);
    MO_ASSERT(mkdir(library, 0700) == 0);
    write_file(support, support_source, sizeof(support_source) - 1);
    init(&f, 16 << 20);
    CHECK(mo_load_program(&f.arena, &f.sources, entry, library, &f.diags) == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_MODULE_MISSING);
    CHECK(f.diags.items[0].primary.span.file == 1);
    char canonical_library[MO_LIMIT_PATH_SIZE + 1], expected_helper[MO_LIMIT_PATH_SIZE + 1];
    MO_ASSERT(realpath(library, canonical_library) != NULL);
    int expected_length =
        snprintf(expected_helper, sizeof(expected_helper), "%s/helper.mo", canonical_library);
    MO_ASSERT(expected_length > 0 && (size_t)expected_length < sizeof(expected_helper));
    CHECK(strcmp(f.diags.items[0].primary.text, expected_helper) == 0);
    finish(&f);
    write_file(lib_helper, library_source, sizeof(library_source) - 1);
    init(&f, 16 << 20);
    struct mo_module_set *set = mo_load_program(&f.arena, &f.sources, entry, library, &f.diags);
    CHECK(set != NULL && f.diags.count == 0);
    if (set != NULL) {
        CHECK(set->library[set->uses[0][0]]);
        CHECK(set->uses[2][0] == set->uses[0][0]);
        CHECK(f.sources.files[1].length == sizeof(library_source) - 1 &&
              memcmp(f.sources.files[1].bytes, library_source, sizeof(library_source) - 1) == 0);
    }
    finish(&f);
    write_file(lib_helper, (const uint8_t *)"module Wrong\n", 13);
    init(&f, 16 << 20);
    CHECK(mo_load_program(&f.arena, &f.sources, entry, library, &f.diags) == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_MODULE_NAME);
    CHECK(f.diags.items[0].primary.span.file == 1);
    finish(&f);
    /* A library file as the entry is a library module itself, and is not read twice. */
    write_file(lib_helper, library_source, sizeof(library_source) - 1);
    init(&f, 16 << 20);
    set = mo_load_program(&f.arena, &f.sources, support, library, &f.diags);
    CHECK(set != NULL && f.diags.count == 0);
    if (set != NULL) {
        CHECK(set->count == 2 && strcmp(set->names[0], "Support") == 0);
        CHECK(set->library[0] && set->library[1] && strcmp(set->names[1], "Helper") == 0);
        CHECK(set->uses[0][0] == 1 && f.sources.count == 2);
        CHECK(strncmp(set->root, library, strlen(library)) == 0);
    }
    finish(&f);
    /* The program's own file of the same name is not the library's. */
    init(&f, 16 << 20);
    set = mo_load_program(&f.arena, &f.sources, helper, library, &f.diags);
    CHECK(set == NULL && f.diags.count == 1 && f.diags.items[0].code == MO_CODE_MODULE_RESERVED);
    finish(&f);
    const uint8_t json_source[] = "module Main\nuse Json\n";
    write_file(entry, json_source, sizeof(json_source) - 1);
    init(&f, 16 << 20);
    CHECK(mo_load_program(&f.arena, &f.sources, entry, NULL, &f.diags) == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_MODULE_MISSING);
    finish(&f);
    MO_ASSERT(unlink(entry) == 0 && unlink(helper) == 0 && unlink(support) == 0 &&
              unlink(lib_helper) == 0);
    MO_ASSERT(rmdir(library) == 0 && rmdir(root) == 0);
}

static void loading_labels(void) {
    char root[] = "/tmp/mo-check-labels-XXXXXX";
    MO_ASSERT(mkdtemp(root) != NULL);
    char entry[256], second[256], third[256], xml[256];
    int a = snprintf(entry, sizeof(entry), "%s/main.mo", root);
    int b = snprintf(second, sizeof(second), "%s/second.mo", root);
    int c = snprintf(third, sizeof(third), "%s/third.mo", root);
    int d = snprintf(xml, sizeof(xml), "%s/xml-reader.mo", root);
    MO_ASSERT(a > 0 && (size_t)a < sizeof(entry));
    MO_ASSERT(b > 0 && (size_t)b < sizeof(second));
    MO_ASSERT(c > 0 && (size_t)c < sizeof(third));
    MO_ASSERT(d > 0 && (size_t)d < sizeof(xml));
    const char main_source[] = "module Main\nuse Second\n";
    const char second_source[] = "module Second\nuse Third\n";
    const char third_source[] = "module Third\nuse Main\n";
    write_file(entry, (const uint8_t *)main_source, sizeof(main_source) - 1);
    write_file(second, (const uint8_t *)second_source, sizeof(second_source) - 1);
    write_file(third, (const uint8_t *)third_source, sizeof(third_source) - 1);
    struct fixture f;
    init(&f, 16 << 20);
    CHECK(mo_load_program(&f.arena, &f.sources, entry, NULL, &f.diags) == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_MODULE_CYCLE);
    if (f.diags.count == 1) {
        const struct mo_diag *diag = &f.diags.items[0];
        CHECK(diag->primary.span.file == 2 && diag->primary.span.start == 17);
        CHECK(strcmp(diag->primary.text, "Main") == 0 && diag->secondary_count == 2);
        if (diag->secondary_count == 2) {
            CHECK(diag->secondary[0].span.file == 0 && diag->secondary[0].span.start == 16);
            CHECK(diag->secondary[1].span.file == 1 && diag->secondary[1].span.start == 18);
            CHECK(strcmp(diag->secondary[0].text, "Second") == 0);
            CHECK(strcmp(diag->secondary[1].text, "Third") == 0);
        }
    }
    finish(&f);
    const char mismatch[] = "module Main\nuse XmlReader\n";
    write_file(entry, (const uint8_t *)mismatch, sizeof(mismatch) - 1);
    write_file(xml, (const uint8_t *)"module XMLReader\n", 17);
    init(&f, 16 << 20);
    CHECK(mo_load_program(&f.arena, &f.sources, entry, NULL, &f.diags) == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_MODULE_NAME);
    if (f.diags.count == 1) {
        const struct mo_diag *diag = &f.diags.items[0];
        CHECK(diag->primary.span.file == 0 && diag->primary.span.start == 16);
        CHECK(strcmp(diag->primary.text, "XmlReader") == 0 && diag->secondary_count == 1);
        if (diag->secondary_count == 1)
            CHECK(diag->secondary[0].span.file == 1 && diag->secondary[0].span.start == 7);
    }
    finish(&f);
    MO_ASSERT(unlink(xml) == 0);
    init(&f, 16 << 20);
    CHECK(mo_load_program(&f.arena, &f.sources, entry, NULL, &f.diags) == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_MODULE_MISSING);
    CHECK(strcmp(f.diags.items[0].primary.text, xml) == 0);
    finish(&f);
    /* Conversion overflows even though the source line is within its limit. */
    struct mo_buffer long_use;
    mo_buffer_init(&long_use, 8192);
    MO_ASSERT(mo_buffer_append_text(&long_use, "module Main\nuse "));
    for (uint32_t i = 0; i < MO_LIMIT_PATH_SIZE - 2; i++)
        MO_ASSERT(mo_buffer_append_byte(&long_use, 'A'));
    MO_ASSERT(mo_buffer_append_byte(&long_use, '\n'));
    write_file(entry, long_use.bytes, long_use.length);
    mo_buffer_free(&long_use);
    init(&f, 16 << 20);
    CHECK(mo_load_program(&f.arena, &f.sources, entry, NULL, &f.diags) == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_PATH_TOO_LONG);
    CHECK(f.diags.items[0].primary.span.file == 0 && f.diags.items[0].primary.span.start == 16);
    finish(&f);
    MO_ASSERT(unlink(entry) == 0 && unlink(second) == 0 && unlink(third) == 0 && rmdir(root) == 0);
}

static void module_limit(void) {
    char folder[] = "/tmp/mo-check-modules-XXXXXX";
    MO_ASSERT(mkdtemp(folder) != NULL);
    char path[256], text[128], entry[256];
    int n = snprintf(entry, sizeof(entry), "%s/m0.mo", folder);
    MO_ASSERT(n > 0 && (size_t)n < sizeof(entry));
    for (uint32_t i = 0; i <= MO_LIMIT_FILES; i++) {
        n = snprintf(path, sizeof(path), "%s/m%u.mo", folder, i);
        MO_ASSERT(n > 0 && (size_t)n < sizeof(path));
        n = i < MO_LIMIT_FILES - 1 ? snprintf(text, sizeof(text), "module M%u\nuse M%u\n", i, i + 1)
                                   : snprintf(text, sizeof(text), "module M%u\n", i);
        MO_ASSERT(n > 0 && (size_t)n < sizeof(text));
        write_file(path, (const uint8_t *)text, (size_t)n);
    }
    struct fixture f;
    init(&f, 64 << 20);
    struct mo_module_set *set = mo_load_program(&f.arena, &f.sources, entry, NULL, &f.diags);
    CHECK(set != NULL && f.diags.count == 0);
    if (set != NULL) {
        CHECK(set->count == MO_LIMIT_FILES);
        for (uint32_t i = 0; i < set->count; i++)
            CHECK(set->order[i] == MO_LIMIT_FILES - 1 - i);
    }
    finish(&f);
    n = snprintf(path, sizeof(path), "%s/m%u.mo", folder, MO_LIMIT_FILES - 1);
    MO_ASSERT(n > 0 && (size_t)n < sizeof(path));
    n = snprintf(text, sizeof(text), "module M%u\nuse M%u\n", MO_LIMIT_FILES - 1, MO_LIMIT_FILES);
    MO_ASSERT(n > 0 && (size_t)n < sizeof(text));
    write_file(path, (const uint8_t *)text, (size_t)n);
    init(&f, 64 << 20);
    CHECK(mo_load_program(&f.arena, &f.sources, entry, NULL, &f.diags) == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_TOO_MANY_FILES);
    finish(&f);
    for (uint32_t i = 0; i <= MO_LIMIT_FILES; i++) {
        n = snprintf(path, sizeof(path), "%s/m%u.mo", folder, i);
        MO_ASSERT(n > 0 && (size_t)n < sizeof(path));
        MO_ASSERT(unlink(path) == 0);
    }
    MO_ASSERT(rmdir(folder) == 0);
}

static void library_limit(void) {
    char folder[] = "/tmp/mo-check-library-XXXXXX";
    MO_ASSERT(mkdtemp(folder) != NULL);
    char path[256];
    for (uint32_t i = 0; i < MO_LIMIT_LIBRARY_ENTRIES; i++) {
        int n = snprintf(path, sizeof(path), "%s/junk%u", folder, i);
        MO_ASSERT(n > 0 && (size_t)n < sizeof(path));
        write_file(path, NULL, 0);
    }
    struct fixture f;
    init(&f, 64 << 20);
    struct mo_module_set *set =
        mo_load_program(&f.arena, &f.sources, "tests/check/load/main.mo", folder, &f.diags);
    CHECK(set != NULL && f.diags.count == 0);
    if (set != NULL)
        CHECK(set->count == 3);
    finish(&f);
    int n = snprintf(path, sizeof(path), "%s/extra", folder);
    MO_ASSERT(n > 0 && (size_t)n < sizeof(path));
    write_file(path, NULL, 0);
    init(&f, 64 << 20);
    CHECK(mo_load_program(&f.arena, &f.sources, "tests/check/load/main.mo", folder, &f.diags) ==
          NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_LIBRARY_TOO_BIG);
    finish(&f);
    MO_ASSERT(unlink(path) == 0);
    for (uint32_t i = 0; i < MO_LIMIT_LIBRARY_ENTRIES; i++) {
        n = snprintf(path, sizeof(path), "%s/junk%u", folder, i);
        MO_ASSERT(n > 0 && (size_t)n < sizeof(path));
        MO_ASSERT(unlink(path) == 0);
    }
    MO_ASSERT(rmdir(folder) == 0);
}

static MO_MUST_USE struct mo_module_syntax *parse_bytes(struct fixture *f, const char *path,
                                                        const uint8_t *bytes, size_t length) {
    uint32_t file;
    struct mo_source_error problem;
    MO_ASSERT(mo_sources_add_bytes(&f->sources, path, bytes, length, &file, &problem));
    struct mo_module_syntax *module = mo_parse(&f->syntax, &f->sources, file, &f->diags);
    MO_ASSERT(module != NULL && f->diags.count == 0);
    return module;
}

static void independent_errors(void) {
    struct fixture f;
    init(&f, 16 << 20);
    const char first[] = "module Main\nzeta = 1\nzeta = 2\nalpha = 1\nalpha = 2\n";
    const char second[] = "module Elsewhere\nexpose missing\n";
    struct mo_module_syntax *modules[] = {
        parse_bytes(&f, "main.mo", (const uint8_t *)first, sizeof(first) - 1),
        parse_bytes(&f, "elsewhere.mo", (const uint8_t *)second, sizeof(second) - 1)};
    uint32_t *uses[] = {NULL, NULL};
    struct mo_module_set set = {.count = 2, .modules = modules, .uses = uses};
    CHECK(mo_resolve_modules(&f.arena, &set, &f.diags) == NULL);
    CHECK(f.diags.count == 3);
    if (f.diags.count == 3) {
        CHECK(f.diags.items[0].code == MO_CODE_NAME_CLASH);
        CHECK(f.diags.items[0].primary.span.start == (uint32_t)(strstr(first, "zeta = 2") - first));
        CHECK(f.diags.items[1].primary.span.start ==
              (uint32_t)(strstr(first, "alpha = 2") - first));
        CHECK(f.diags.items[2].code == MO_CODE_EXPOSE_NAME);
        CHECK(f.diags.items[2].primary.span.file == 1);
    }
    finish(&f);
}

static void name_rules(void) {
    const char money[] = "module Money\nexpose Choice(..), Money\n"
                         "enum Choice\n Ready\n Done\nend\nstruct Money\n value: Int\nend\n";
    const char opaque[] = "module Money\nexpose Choice\nenum Choice\n Ready\n Done\nend\n";
    const char another[] = "module Another\nexpose Status(..)\nenum Status\n Ready\n Quit\nend\n";
    const struct {
        const char *main;
        const char *other;
        uint16_t code;
        uint32_t imported;
        const char *label;
    } cases[] = {
        {"module Main\nuse Money{Choice}\n", opaque, 0, 1, NULL},
        {"module Main\nuse Money{Money}\nstruct Money\n value: Int\nend\n", money,
         MO_CODE_NAME_CLASH, 0, "Money"},
        {"module Main\nuse Money{Choice}\nuse Another{Status}\n", money, MO_CODE_NAME_CLASH, 0,
         "Ready"},
        {"module Main\nuse Money\nuse Shop.Money\n", money, MO_CODE_NAME_CLASH, 0, "Money"},
        {"module Main\nstruct Text\n value: Int\nend\n", money, MO_CODE_ALWAYS_IN_SCOPE, 0, "Text"},
        {"module Main\nenum Result\n Fine\n Bad\nend\n", money, MO_CODE_ALWAYS_IN_SCOPE, 0,
         "Result"},
        {"module Main\nuse Shop.Set\n", "module Shop.Set\n", MO_CODE_ALWAYS_IN_SCOPE, 0, "Set"},
        {"module Main\nuse Money{Time}\n", money, MO_CODE_ALWAYS_IN_SCOPE, 0, "Time"},
        {"module Main\nstruct Choice\n value: Int\nend\nenum State\n Choice\nend\n", money,
         MO_CODE_NAME_CLASH, 0, "Choice"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct fixture f;
        init(&f, 16 << 20);
        struct mo_module_syntax *modules[] = {
            parse_bytes(&f, "main.mo", (const uint8_t *)cases[i].main, strlen(cases[i].main)),
            parse_bytes(&f, "money.mo", (const uint8_t *)cases[i].other, strlen(cases[i].other)),
            parse_bytes(&f, "another.mo",
                        (const uint8_t *)(i == 3 ? "module Shop.Money\n" : another),
                        i == 3 ? strlen("module Shop.Money\n") : sizeof(another) - 1)};
        uint32_t targets[] = {1, 2}, *uses[] = {targets, NULL, NULL};
        struct mo_module_set set = {.count = 3, .modules = modules, .uses = uses};
        struct mo_module_interfaces *names = mo_resolve_modules(&f.arena, &set, &f.diags);
        CHECK((names != NULL) == (cases[i].code == 0));
        if (cases[i].code == 0 && names != NULL) {
            CHECK(f.diags.count == 0 && names->scopes[0].imported_count == cases[i].imported);
            CHECK(strcmp(names->scopes[0].imported[0].name, "Choice") == 0);
        } else {
            CHECK(f.diags.count == 1);
            if (f.diags.count == 1) {
                CHECK(f.diags.items[0].code == cases[i].code);
                CHECK(strcmp(f.diags.items[0].primary.text, cases[i].label) == 0);
            }
        }
        finish(&f);
    }
    const char *imports[] = {"Ready", "Ready, Choice", "Choice, Ready"};
    for (size_t i = 0; i < sizeof(imports) / sizeof(imports[0]); i++) {
        char input[128];
        int n = snprintf(input, sizeof(input), "module Main\nuse Money{%s}\n", imports[i]);
        MO_ASSERT(n > 0 && (size_t)n < sizeof(input));
        struct fixture f;
        init(&f, 16 << 20);
        struct mo_module_syntax *modules[] = {
            parse_bytes(&f, "main.mo", (const uint8_t *)input, (size_t)n),
            parse_bytes(&f, "money.mo", (const uint8_t *)money, sizeof(money) - 1)};
        uint32_t targets[] = {1}, *uses[] = {targets, NULL};
        struct mo_module_set set = {.count = 2, .modules = modules, .uses = uses};
        CHECK(mo_resolve_modules(&f.arena, &set, &f.diags) == NULL);
        CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_IMPORT_CASE);
        if (f.diags.count == 1 && f.diags.items[0].fix_count == 1) {
            const struct mo_edit *edit = f.diags.items[0].fixes[0].edits;
            struct mo_buffer fixed;
            mo_buffer_init(&fixed, 1024);
            MO_ASSERT(mo_buffer_append(&fixed, (const uint8_t *)input, edit->span.start));
            MO_ASSERT(mo_buffer_append_text(&fixed, edit->replacement));
            MO_ASSERT(mo_buffer_append(&fixed, (const uint8_t *)input + edit->span.end,
                                       (size_t)n - edit->span.end));
            mo_diags_free(&f.diags);
            MO_ASSERT(mo_diags_init(&f.diags));
            modules[0] = parse_bytes(&f, "fixed.mo", fixed.bytes, fixed.length);
            CHECK(mo_resolve_modules(&f.arena, &set, &f.diags) != NULL);
            CHECK(f.diags.count == 0);
            mo_buffer_free(&fixed);
        } else
            CHECK(false);
        finish(&f);
    }
}

static void function_signatures(void) {
    const uint8_t text[] = "module Main\n"
                           "fn identity(value: T) : T\nvalue\nend\n"
                           "fn other(value: T) : T\nvalue\nend\n"
                           "fn visit(value: fn(Int))\nvalue(1)\nend\n";
    struct fixture f;
    init(&f, 16 << 20);
    struct mo_module_syntax *modules[] = {parse_bytes(&f, "main.mo", text, sizeof(text) - 1)};
    uint32_t *uses[] = {NULL}, order[] = {0};
    struct mo_module_set set = {.count = 1, .modules = modules, .uses = uses, .order = order};
    struct mo_module_interfaces *names = mo_resolve_modules(&f.arena, &set, &f.diags);
    MO_ASSERT(names != NULL);
    struct mo_signatures *signatures = mo_prepare_signatures(&f.arena, &set, names, &f.diags);
    CHECK(signatures != NULL && f.diags.count == 0);
    if (signatures != NULL && signatures->count == 3) {
        const struct mo_source_type *identity = signatures->items[0].type;
        CHECK(strcmp(signatures->items[0].function->name, "identity") == 0);
        CHECK(identity->kind == MO_SOURCE_FUNCTION && identity->count == 2);
        CHECK(identity->arguments[0]->kind == MO_SOURCE_PARAMETER);
        CHECK(strcmp(identity->arguments[0]->name, "T") == 0);
        CHECK(identity->arguments[0] == identity->arguments[1]);
        const struct mo_source_type *other = signatures->items[1].type;
        CHECK(other->arguments[0] != identity->arguments[0]);
        CHECK(other->arguments[0] == other->arguments[1]);
        const struct mo_source_type *visit = signatures->items[2].type;
        CHECK(visit->arguments[0]->kind == MO_SOURCE_FUNCTION);
        CHECK(strcmp(visit->arguments[0]->arguments[1]->name, "Unit") == 0);
        CHECK(visit->arguments[0]->arguments[1] == visit->arguments[1]);
    } else
        CHECK(false);
    bool succeeded = false, failed = false;
    /* Preparation now includes every native table signature. Keep the fine
     * sweep through the original small allocations, then cross that table. */
    for (size_t limit = 0; limit <= (1 << 20); limit += limit < 16384 ? 16u : 1024u) {
        struct mo_arena arena;
        mo_arena_init(&arena, limit);
        struct mo_signatures *out = mo_prepare_signatures(&arena, &set, names, &f.diags);
        if (out == NULL) {
            failed = true;
            CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_CHECK_NO_MEMORY);
        } else {
            succeeded = true;
            CHECK(f.diags.count == 0 && out->count == 3);
            CHECK(out->items[0].type->arguments[0] == out->items[0].type->arguments[1]);
            CHECK(strcmp(out->items[2].type->arguments[1]->name, "Unit") == 0);
        }
        mo_arena_free(&arena);
        mo_diags_free(&f.diags);
        MO_ASSERT(mo_diags_init(&f.diags));
    }
    CHECK(succeeded && failed);
    finish(&f);
}

static void body_inference(void) {
    const uint8_t text[] = "module Main\nfn add(value: Int) : Int\n"
                           "total = value + 1\ntotal\nend\n";
    struct fixture f;
    init(&f, 16 << 20);
    struct mo_module_syntax *modules[] = {parse_bytes(&f, "main.mo", text, sizeof(text) - 1)};
    uint32_t *uses[] = {NULL};
    struct mo_module_set set = {.count = 1, .modules = modules, .uses = uses};
    struct mo_module_interfaces *names = mo_resolve_modules(&f.arena, &set, &f.diags);
    MO_ASSERT(names != NULL);
    struct mo_signatures *signatures = mo_prepare_signatures(&f.arena, &set, names, &f.diags);
    MO_ASSERT(signatures != NULL);
    struct mo_inferred_body *body = mo_infer_function(&f.arena, names, signatures, 0, &f.diags);
    CHECK(body != NULL && f.diags.count == 0);
    if (body != NULL) {
        const struct mo_syntax *syntax = signatures->items[0].function->declaration->children[5];
        const struct mo_syntax *binding = syntax->children[0]->children[0];
        const struct mo_syntax *reference = syntax->children[1]->children[0];
        bool found = false;
        for (uint32_t i = 0; i < body->count; i++)
            if (body->items[i].syntax == reference) {
                CHECK(strcmp(body->items[i].type->name, "Int") == 0);
                CHECK(body->items[i].binding == binding);
                found = true;
            }
        CHECK(found);
    }
    finish(&f);
}

static void import_limit(void) {
    struct mo_buffer source;
    mo_buffer_init(&source, MO_LIMIT_FILE_SIZE);
    MO_ASSERT(mo_buffer_append_text(&source, "module Library\nexpose "));
    for (uint32_t i = 0; i < 16; i++) {
        MO_ASSERT(mo_buffer_append_text(&source, i == 0 ? "E" : ", E"));
        MO_ASSERT(mo_buffer_append_uint(&source, i));
        MO_ASSERT(mo_buffer_append_text(&source, "(..)"));
    }
    MO_ASSERT(mo_buffer_append_text(&source, ", extra\n"));
    uint32_t id = 0;
    for (uint32_t i = 0; i < 16; i++) {
        MO_ASSERT(mo_buffer_append_text(&source, "enum E"));
        MO_ASSERT(mo_buffer_append_uint(&source, i));
        MO_ASSERT(mo_buffer_append_byte(&source, '\n'));
        for (uint32_t j = 0; j < MO_LIMIT_CHOICE_CASES; j++) {
            MO_ASSERT(mo_buffer_append_byte(&source, 'C'));
            MO_ASSERT(mo_buffer_append_uint(&source, id++));
            MO_ASSERT(mo_buffer_append_byte(&source, '\n'));
        }
        MO_ASSERT(mo_buffer_append_text(&source, "end\n"));
    }
    MO_ASSERT(mo_buffer_append_text(&source, "extra = 1\n"));
    struct fixture f;
    init(&f, 256 << 20);
    struct mo_module_syntax *other = parse_bytes(&f, "library.mo", source.bytes, source.length);
    mo_buffer_free(&source);
    mo_buffer_init(&source, MO_LIMIT_FILE_SIZE);
    MO_ASSERT(mo_buffer_append_text(&source, "module Main\nuse Library{"));
    for (uint32_t i = 0; i < 16; i++) {
        MO_ASSERT(mo_buffer_append_text(&source, i == 0 ? "E" : ", E"));
        MO_ASSERT(mo_buffer_append_uint(&source, i));
    }
    size_t end = source.length;
    MO_ASSERT(mo_buffer_append_text(&source, "}\n"));
    struct mo_module_syntax *modules[] = {parse_bytes(&f, "main.mo", source.bytes, source.length),
                                          other};
    uint32_t target[] = {1}, *uses[] = {target, NULL}, order[] = {1, 0};
    struct mo_module_set set = {.count = 2, .modules = modules, .uses = uses, .order = order};
    struct mo_module_interfaces *names = mo_resolve_modules(&f.arena, &set, &f.diags);
    CHECK(names != NULL && f.diags.count == 0);
    if (names != NULL) {
        CHECK(names->scopes[0].imported_count == MO_LIMIT_MODULE_IMPORTS);
        CHECK(strcmp(names->scopes[0].imported[0].name, "C0") == 0);
        CHECK(strcmp(names->scopes[0].imported[MO_LIMIT_MODULE_IMPORTS - 1].name, "E9") == 0);
    }
    mo_arena_free(&f.arena);
    /* Four maximum-sized imports exactly fill the program budget. A fifth
     * must be refused before allocating or walking another million entries. */
    struct mo_module_syntax *many[6] = {modules[0]};
    uint32_t destination = 4;
    uint32_t *many_uses[6] = {&destination, &destination, &destination, &destination, NULL, NULL};
    for (uint32_t i = 1; i < 5; i++) {
        struct mo_buffer input;
        mo_buffer_init(&input, 1024);
        MO_ASSERT(mo_buffer_append_text(&input, "module M"));
        MO_ASSERT(mo_buffer_append_uint(&input, i));
        MO_ASSERT(mo_buffer_append_byte(&input, '\n'));
        MO_ASSERT(mo_buffer_append(&input, source.bytes + 12, source.length - 12));
        many[i] = parse_bytes(&f, "importer.mo", input.bytes, input.length);
        mo_buffer_free(&input);
    }
    struct mo_module_syntax *fifth = many[4];
    many[4] = other;
    struct mo_module_set batch = {.count = 5, .modules = many, .uses = many_uses};
    names = mo_resolve_modules(&f.arena, &batch, &f.diags);
    CHECK(names != NULL && f.diags.count == 0);
    if (names != NULL) {
        CHECK(names->scopes[3].imported_count == MO_LIMIT_MODULE_IMPORTS);
        CHECK(names->scopes[0].imported[0].symbols == names->scopes[3].imported[0].symbols);
        CHECK(names->scopes[0].imported[0].name == names->scopes[3].imported[0].name);
    }
    mo_arena_free(&f.arena);
    many[4] = fifth;
    many[5] = other;
    many_uses[4] = &destination;
    destination = 5;
    batch.count = 6;
    CHECK(mo_resolve_modules(&f.arena, &batch, &f.diags) == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_TOO_MANY_PROGRAM_IMPORTS);
    mo_arena_free(&f.arena);
    mo_diags_free(&f.diags);
    MO_ASSERT(mo_diags_init(&f.diags));
    struct mo_buffer more;
    mo_buffer_init(&more, MO_LIMIT_FILE_SIZE);
    MO_ASSERT(mo_buffer_append(&more, source.bytes, end));
    MO_ASSERT(mo_buffer_append_text(&more, ", extra}\n"));
    modules[0] = parse_bytes(&f, "extra.mo", more.bytes, more.length);
    CHECK(mo_resolve_modules(&f.arena, &set, &f.diags) == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_TOO_MANY_IMPORTS);
    finish(&f);
    mo_buffer_free(&more);
    mo_buffer_free(&source);
}

static void import_scaling(void) {
    struct fixture f;
    init(&f, 64 << 20);
    struct mo_buffer source;
    mo_buffer_init(&source, MO_LIMIT_FILE_SIZE);
    MO_ASSERT(mo_buffer_append_text(&source, "module Library\nexpose Choice(..)\nenum Choice\n"));
    uint8_t prefix[512];
    memset(prefix, 'a', sizeof(prefix));
    prefix[0] = 'C';
    for (uint32_t i = 0; i < 4095; i++) {
        MO_ASSERT(mo_buffer_append(&source, prefix, sizeof(prefix)));
        MO_ASSERT(mo_buffer_append_uint(&source, i));
        MO_ASSERT(mo_buffer_append_byte(&source, '\n'));
    }
    MO_ASSERT(mo_buffer_append_text(&source, "end\n"));
    struct mo_module_syntax *modules[129];
    modules[0] = parse_bytes(&f, "library.mo", source.bytes, source.length);
    mo_buffer_free(&source);
    uint32_t destination = 0, *uses[129] = {NULL};
    for (uint32_t i = 1; i < 129; i++) {
        char input[128];
        int n = snprintf(input, sizeof(input), "module M%u\nuse Library{Choice}\n", i);
        MO_ASSERT(n > 0 && (size_t)n < sizeof(input));
        modules[i] = parse_bytes(&f, "importer.mo", (const uint8_t *)input, (size_t)n);
        uses[i] = &destination;
    }
    const uint32_t sizes[] = {1, 32, 128};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        mo_arena_free(&f.arena);
        size_t budget = (8 << 20) + (size_t)sizes[i] * 4096 * sizeof(struct mo_imported_name);
        mo_arena_init(&f.arena, budget);
        struct mo_module_set set = {.count = sizes[i] + 1, .modules = modules, .uses = uses};
        struct mo_module_interfaces *names = mo_resolve_modules(&f.arena, &set, &f.diags);
        CHECK(names != NULL && f.diags.count == 0);
        if (names != NULL) {
            CHECK(names->scopes[sizes[i]].imported_count == 4096);
            CHECK(names->scopes[1].imported[0].symbols ==
                  names->scopes[sizes[i]].imported[0].symbols);
            CHECK(names->scopes[1].imported[0].name == names->scopes[sizes[i]].imported[0].name);
            MO_ASSERT(printf("check import scaling: %u importers, %zu checking arena bytes\n",
                             sizes[i], f.arena.used) >= 0);
        }
    }
    finish(&f);
}

static void source_limits(void) {
    char folder[] = "/tmp/mo-check-source-XXXXXX";
    MO_ASSERT(mkdtemp(folder) != NULL);
    char entry[256], path[256], header[128];
    int n = snprintf(entry, sizeof(entry), "%s/m0.mo", folder);
    MO_ASSERT(n > 0 && (size_t)n < sizeof(entry));
    struct mo_arena scratch;
    mo_arena_init(&scratch, 16 << 20);
    uint8_t *bytes = mo_arena_allocate(&scratch, MO_LIMIT_FILE_SIZE + 1, 1);
    MO_ASSERT(bytes != NULL);
    memset(bytes, ' ', MO_LIMIT_FILE_SIZE + 1);
    for (size_t i = 4095; i < MO_LIMIT_FILE_SIZE; i += 4096)
        bytes[i] = '\n';
    memcpy(bytes, "module M0\n", 10);
    write_file(entry, bytes, MO_LIMIT_FILE_SIZE + 1);
    struct fixture f;
    init(&f, 64 << 20);
    CHECK(mo_load_program(&f.arena, &f.sources, entry, NULL, &f.diags) == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_FILE_TOO_BIG);
    finish(&f);
    for (uint32_t i = 0; i < 8; i++) {
        n = snprintf(path, sizeof(path), "%s/m%u.mo", folder, i);
        MO_ASSERT(n > 0 && (size_t)n < sizeof(path));
        n = i < 7 ? snprintf(header, sizeof(header), "module M%u\nuse M%u\n", i, i + 1)
                  : snprintf(header, sizeof(header), "module M%u\n", i);
        MO_ASSERT(n > 0 && (size_t)n < sizeof(header));
        memset(bytes, ' ', sizeof(header));
        memcpy(bytes, header, (size_t)n);
        write_file(path, bytes, MO_LIMIT_FILE_SIZE);
    }
    init(&f, 64 << 20);
    struct mo_module_set *set = mo_load_program(&f.arena, &f.sources, entry, NULL, &f.diags);
    CHECK(set != NULL && f.diags.count == 0);
    CHECK(f.sources.size == MO_LIMIT_PROGRAM_SIZE);
    finish(&f);
    memcpy(bytes, "module M7\nuse M8\n", 17);
    write_file(path, bytes, MO_LIMIT_FILE_SIZE);
    n = snprintf(path, sizeof(path), "%s/m8.mo", folder);
    MO_ASSERT(n > 0 && (size_t)n < sizeof(path));
    write_file(path, (const uint8_t *)"module M8\n", 10);
    init(&f, 64 << 20);
    CHECK(mo_load_program(&f.arena, &f.sources, entry, NULL, &f.diags) == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_PROGRAM_TOO_BIG);
    finish(&f);
    for (uint32_t i = 0; i < 9; i++) {
        n = snprintf(path, sizeof(path), "%s/m%u.mo", folder, i);
        MO_ASSERT(n > 0 && (size_t)n < sizeof(path));
        MO_ASSERT(unlink(path) == 0);
    }
    MO_ASSERT(rmdir(folder) == 0);
    mo_arena_free(&scratch);
    char long_path[MO_LIMIT_PATH_SIZE + 2];
    memset(long_path, 'x', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    init(&f, 64 << 20);
    CHECK(mo_load_program(&f.arena, &f.sources, long_path, NULL, &f.diags) == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_PATH_TOO_LONG);
    finish(&f);
}

static void run_tests(void *argument) {
    (void)argument;
    load_graph();
    interfaces();
    corpus();
    samples();
    imported_function();
    imported_constants();
    constant_import_sources();
    imported_nominal();
    imported_choice();
    imported_generic();
    generic_import_sources();
    import_sources();
    import_cap();
    captured_parameter();
    capture_sources();
    arena_limits();
    header_lookahead();
    library_collision();
    path_policy();
    confined_files();
    library_roots();
    loading_labels();
    module_limit();
    library_limit();
    independent_errors();
    name_rules();
    function_signatures();
    body_inference();
    import_limit();
    import_scaling();
    source_limits();
}

int main(void) {
    CHECK(mo_with_stack(MO_SOURCE_STACK_SIZE, run_tests, NULL) == 0);
    return mo_test_report("check");
}
