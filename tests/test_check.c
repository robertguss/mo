#define _POSIX_C_SOURCE 200809L

#include "base/buffer.h"
#include "base/codes.h"
#include "base/limits.h"
#include "check/check.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void load_graph(void) {
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_arena arena;
    MO_ASSERT(mo_sources_init(&sources));
    MO_ASSERT(mo_diags_init(&diags));
    mo_arena_init(&arena, 16 << 20);
    struct mo_module_set *set = mo_load(&arena, &sources, "tests/check/load/main.mo", NULL, &diags);
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
    struct mo_module_set *set =
        mo_load(&arena, &sources, "tests/check/interfaces/main.mo", "tests/check/library", &diags);
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
            CHECK(scope->imported[1].symbols->owner->kind == MO_S_ENUM);
            CHECK(scope->imported[4].count == 2);
            CHECK(scope->imported[4].symbols[0].declaration->kind == MO_S_FN);
            CHECK(scope->imported[4].symbols[1].declaration->kind == MO_S_FN);
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
    const struct {
        const char *input;
        const char *expected;
        bool loading;
    } cases[] = {
        {"e0034/main.mo", "e0034.diag", true},
        {"e0035/main.mo", "e0035.diag", true},
        {"e0036/main.mo", "e0036.diag", true},
        {"e0037/json.mo", "e0037.diag", true},
        {"e0038.mo", "e0038.diag", false},
        {"e0038-import-repeat.mo", "e0038-import-repeat.diag", false},
        {"e0039.mo", "e0039.diag", false},
        {"e0040.mo", "e0040.diag", false},
        {"e0041.mo", "e0041.diag", false},
        {"e0042.mo", "e0042.diag", false},
        {"e0043.mo", "e0043.diag", false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct fixture f;
        init(&f, 64 << 20);
        char path[256];
        int n = snprintf(path, sizeof(path), "tests/check/diagnostics/%s", cases[i].input);
        MO_ASSERT(n > 0 && (size_t)n < sizeof(path));
        if (cases[i].loading)
            CHECK(mo_load(&f.arena, &f.sources, path, NULL, &f.diags) == NULL);
        else {
            struct mo_module_syntax *modules[] = {
                parse_file(&f, path), parse_file(&f, "tests/check/interfaces/money.mo")};
            uint32_t targets[] = {1}, *uses[] = {targets, NULL};
            struct mo_module_set set = {.count = 2, .modules = modules, .uses = uses};
            CHECK(mo_resolve_modules(&f.arena, &set, &f.diags) == NULL);
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
        struct mo_module_set *set = mo_load(&f.arena, &f.sources, path, NULL, &f.diags);
        CHECK(set != NULL);
        if (set != NULL)
            CHECK(mo_resolve_modules(&f.arena, set, &f.diags) != NULL);
        CHECK(f.diags.count == 0);
        finish(&f);
    }
}

static void arena_limits(void) {
    for (size_t limit = 0; limit < (1 << 20); limit = limit < 1024 ? limit + 16 : limit + 4096) {
        struct fixture f;
        init(&f, limit);
        struct mo_module_set *set =
            mo_load(&f.arena, &f.sources, "tests/check/load/main.mo", NULL, &f.diags);
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
    CHECK(mo_load(&f.arena, &f.sources, path, NULL, &f.diags) == NULL);
    CHECK(f.diags.count == 1);
    finish(&f);
    MO_ASSERT(unlink(path) == 0);
    MO_ASSERT(rmdir(folder) == 0);
}

static void library_collision(void) {
    struct fixture f;
    init(&f, 16 << 20);
    CHECK(mo_load(&f.arena, &f.sources, "tests/check/load/main.mo", "tests/check/interfaces",
                  &f.diags) == NULL);
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
    struct mo_module_set *set = mo_load(&f.arena, &f.sources, entry, library, &f.diags);
    CHECK(set != NULL && f.diags.count == 0);
    if (set != NULL) {
        CHECK(strcmp(set->root, root) == 0);
        CHECK(strcmp(set->names[0], "XMLReader") == 0);
        CHECK(set->count == 2 && strcmp(set->names[1], "Unicode16Tool") == 0);
    }
    finish(&f);
    MO_ASSERT(unlink(entry) == 0 && unlink(target) == 0);
    MO_ASSERT(unlink(support) == 0 && unlink(loop) == 0);
    MO_ASSERT(rmdir(folder) == 0 && rmdir(library) == 0);
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
    struct mo_module_set *set = mo_load(&f.arena, &f.sources, entry, NULL, &f.diags);
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
    CHECK(mo_load(&f.arena, &f.sources, entry, NULL, &f.diags) == NULL);
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
        mo_load(&f.arena, &f.sources, "tests/check/load/main.mo", folder, &f.diags);
    CHECK(set != NULL && f.diags.count == 0);
    if (set != NULL)
        CHECK(set->count == 3);
    finish(&f);
    int n = snprintf(path, sizeof(path), "%s/extra", folder);
    MO_ASSERT(n > 0 && (size_t)n < sizeof(path));
    write_file(path, NULL, 0);
    init(&f, 64 << 20);
    CHECK(mo_load(&f.arena, &f.sources, "tests/check/load/main.mo", folder, &f.diags) == NULL);
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
    CHECK(mo_load(&f.arena, &f.sources, entry, NULL, &f.diags) == NULL);
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
    struct mo_module_set *set = mo_load(&f.arena, &f.sources, entry, NULL, &f.diags);
    CHECK(set != NULL && f.diags.count == 0);
    CHECK(f.sources.size == MO_LIMIT_PROGRAM_SIZE);
    finish(&f);
    memcpy(bytes, "module M7\nuse M8\n", 17);
    write_file(path, bytes, MO_LIMIT_FILE_SIZE);
    n = snprintf(path, sizeof(path), "%s/m8.mo", folder);
    MO_ASSERT(n > 0 && (size_t)n < sizeof(path));
    write_file(path, (const uint8_t *)"module M8\n", 10);
    init(&f, 64 << 20);
    CHECK(mo_load(&f.arena, &f.sources, entry, NULL, &f.diags) == NULL);
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
    CHECK(mo_load(&f.arena, &f.sources, long_path, NULL, &f.diags) == NULL);
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
    module_limit();
    library_limit();
    import_limit();
    source_limits();
    return mo_test_report("check");
}
