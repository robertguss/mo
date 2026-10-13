#define _POSIX_C_SOURCE 200809L

#include "base/buffer.h"
#include "base/codes.h"
#include "base/limits.h"
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
    enum phase { INTERFACES, LOADING, SIGNATURES, BODIES };
    const struct {
        const char *input;
        const char *expected;
        enum phase phase;
    } cases[] = {
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
        {"e0051.mo", "e0051.diag", BODIES},
        {"e0052.mo", "e0052.diag", BODIES},
        {"e0053.mo", "e0053.diag", BODIES},
        {"e0054.mo", "e0054.diag", BODIES},
        {"e0055.mo", "e0055.diag", BODIES},
        {"e0056.mo", "e0056.diag", BODIES},
        {"e0057.mo", "e0057.diag", BODIES},
        {"e0058.mo", "e0058.diag", BODIES},
        {"e0059.mo", "e0059.diag", BODIES},
        {"e0060.mo", "e0060.diag", BODIES},
        {"e0061.mo", "e0061.diag", BODIES},
        {"e0072.mo", "e0072.diag", BODIES},
        {"e0073.mo", "e0073.diag", BODIES},
        {"e0074.mo", "e0074.diag", BODIES},
        {"e0075.mo", "e0075.diag", BODIES},
        {"e0076.mo", "e0076.diag", BODIES},
        {"e0077.mo", "e0077.diag", BODIES},
        {"e0078.mo", "e0078.diag", BODIES},
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
            struct mo_module_set set = {.count = 2, .modules = modules, .uses = uses};
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
                                mo_infer_function(&f.arena, names, signatures, j, &f.diags) == NULL)
                                break;
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
    MO_ASSERT(symlink("/dev/stdin", linked) == 0);
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
    CHECK(strcmp(f.diags.items[0].primary.text, lib_helper) == 0);
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
    for (size_t limit = 0; limit <= 16384; limit += 16) {
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

int main(void) {
    load_graph();
    interfaces();
    corpus();
    samples();
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
    return mo_test_report("check");
}
