#include "base/limits.h"
#include "format-contract.h"
#include "syntax/format.h"
#include "test.h"

#include <glob.h>
#include <string.h>

static bool fixed_points;
static unsigned accepted_inputs;

static uint32_t ascii_width(const uint8_t *bytes, size_t length) {
    (void)bytes;
    return (uint32_t)length;
}

static bool check_bytes(const char *path, const uint8_t *source, size_t size, const char *expected,
                        bool fixed) {
    int failures_before = mo_test_failures;
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_arena arena;
    MO_ASSERT(mo_sources_init(&sources));
    MO_ASSERT(mo_diags_init(&diags));
    mo_arena_init(&arena, 1 << 26);
    uint32_t file;
    struct mo_source_error error;
    MO_ASSERT(mo_sources_add_bytes(&sources, path, source, size, &file, &error));
    struct mo_module_syntax *module = mo_parse_fragment(&arena, &sources, file, &diags);
    if (module == NULL || mo_diags_any(&diags)) {
        mo_arena_free(&arena);
        mo_diags_free(&diags);
        mo_sources_free(&sources);
        return false;
    }
    accepted_inputs++;
    struct mo_buffer output;
    mo_buffer_init(&output, MO_LIMIT_FILE_SIZE);
    CHECK(mo_format(module, &sources, (struct mo_format_options){MO_FORMAT_WIDTH, 100, ascii_width},
                    &output));
    if (fixed_points && fixed)
        CHECK(size == output.length && (size == 0 || memcmp(source, output.bytes, size) == 0));
    if (expected != NULL) {
        CHECK(output.length == strlen(expected));
        if (output.length == strlen(expected))
            CHECK(memcmp(output.bytes, expected, output.length) == 0);
    }
    MO_ASSERT(
        mo_sources_add_bytes(&sources, "formatted.mo", output.bytes, output.length, &file, &error));
    struct mo_module_syntax *formatted = mo_parse_fragment(&arena, &sources, file, &diags);
    CHECK(formatted != NULL && !mo_diags_any(&diags));
    if (formatted != NULL && !mo_diags_any(&diags)) {
        struct mo_buffer before, after, twice;
        mo_buffer_init(&before, 1 << 27);
        mo_buffer_init(&after, 1 << 27);
        mo_buffer_init(&twice, MO_LIMIT_FILE_SIZE);
        format_tree(&before, module->tree);
        format_tree(&after, formatted->tree);
        CHECK(format_buffer_equal(&before, &after));
        CHECK(mo_format(formatted, &sources,
                        (struct mo_format_options){MO_FORMAT_WIDTH, 100, ascii_width}, &twice));
        CHECK(format_buffer_equal(&output, &twice));
        mo_buffer_free(&before);
        mo_buffer_free(&after);
        mo_buffer_free(&twice);
    }
    if (mo_test_failures != failures_before) {
        (void)fprintf(stderr, "%s input:\n", path);
        (void)fwrite(source, 1, size, stderr);
        (void)fputs("Output:\n", stderr);
        (void)fwrite(output.bytes, 1, output.length, stderr);
        for (uint32_t i = 0; i < diags.count; i++)
            (void)fprintf(stderr, "E%04u: %s\n", diags.items[i].code, diags.items[i].message);
    }
    mo_buffer_free(&output);
    mo_arena_free(&arena);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
    return true;
}

static void check_source(const char *source, const char *expected) {
    CHECK(check_bytes("fixture.mo", (const uint8_t *)source, strlen(source), expected, false));
}

static void files(const char *pattern, bool required, bool good, bool fixed) {
    glob_t paths = {0};
    int status = glob(pattern, 0, NULL, &paths);
    MO_ASSERT(status == 0 || (!required && status == GLOB_NOMATCH));
    for (size_t i = 0; i < paths.gl_pathc; i++) {
        struct mo_sources sources;
        MO_ASSERT(mo_sources_init(&sources));
        uint32_t file;
        struct mo_source_error problem;
        MO_ASSERT(mo_sources_add(&sources, paths.gl_pathv[i], &file, &problem));
        const struct mo_source *s = &sources.files[file];
        bool accepted = check_bytes(s->path, s->bytes, s->length, NULL,
                                    fixed && strcmp(s->path, "grammar/tests/good/layout.mo") != 0);
        CHECK(!good || accepted);
        mo_sources_free(&sources);
    }
    globfree(&paths);
}

static void reference(void) {
    struct mo_sources sources;
    MO_ASSERT(mo_sources_init(&sources));
    uint32_t file;
    struct mo_source_error problem;
    MO_ASSERT(mo_sources_add(&sources, "docs/reference/README.md", &file, &problem));
    const struct mo_source *source = &sources.files[file];
    uint32_t pos = 0, start = 0, block = 0;
    bool inside = false;
    while (pos < source->length) {
        uint32_t end = pos;
        while (end < source->length && source->bytes[end] != '\n')
            end++;
        if (!inside && end - pos == 5 && memcmp(source->bytes + pos, "```mo", 5) == 0) {
            inside = true;
            start = end + 1;
        } else if (inside && end - pos == 3 && memcmp(source->bytes + pos, "```", 3) == 0) {
            char path[80];
            int written = snprintf(path, sizeof path, "docs/reference/README.md block %u", ++block);
            MO_ASSERT(written > 0 && (size_t)written < sizeof path);
            CHECK(check_bytes(path, source->bytes + start, pos - start, NULL, true));
            inside = false;
        }
        pos = end < source->length ? end + 1 : end;
    }
    CHECK(!inside);
    mo_sources_free(&sources);
}

int main(int argc, char **argv) {
    MO_ASSERT(argc == 1 || (argc == 2 && strcmp(argv[1], "--fixed-points") == 0));
    fixed_points = argc == 2;
    check_source("module Main\nfn f()\nvar x=1\nx=x+2\nx\nend\n",
                 "module Main\n\nfn f()\n  var x = 1\n  x += 2\n  x\nend\n");
    check_source("## module docs\nmodule M\n# function docs\nfn f()\n  examples # header\n"
                 "    true\n  end\n\n  if true\n    1\n  else # alternative\n    2\n  end\n"
                 "  # last\nend # close\n# eof\n",
                 NULL);
    check_source("grade=if score>90\n\"A\"\nelse\n\"B\"\nend\n",
                 "grade = if score > 90\n  \"A\"\nelse\n  \"B\"\nend\n");
    files("grammar/tests/good/*.mo", true, true, true);
    files("examples/*.mo", true, true, true);
    files("lib/*.mo", false, true, true);
    files("fuzz/corpus/fuzz_syntax/*", false, false, false);
    reference();
    if (printf("format corpus: %u accepted inputs checked\n", accepted_inputs) < 0)
        return 1;
    return mo_test_report("test_format");
}
