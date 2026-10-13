#include "base/limits.h"
#include "format-contract.h"
#include "syntax/format.h"
#include "test.h"

#include <glob.h>
#include <string.h>

static bool fixed_points;
static unsigned accepted_inputs;

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
    CHECK(mo_format(module, NULL,
                    (struct mo_format_options){MO_FORMAT_WIDTH, 100, format_display_width},
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
        CHECK(mo_format(formatted, NULL,
                        (struct mo_format_options){MO_FORMAT_WIDTH, 100, format_display_width},
                        &twice));
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
        /* Syntax fixtures vary layout, trailing commas, lambda parameter
         * breaks and literal/interpolation spacing; they are not style examples.
         * Exempt only byte equality: all still reparse and format twice. */
        bool canonical = fixed && strcmp(s->path, "grammar/tests/good/layout.mo") != 0 &&
                         strcmp(s->path, "grammar/tests/good/declarations.mo") != 0 &&
                         strcmp(s->path, "grammar/tests/good/lambda-list.mo") != 0 &&
                         strcmp(s->path, "grammar/tests/good/literals.mo") != 0;
        bool accepted = check_bytes(s->path, s->bytes, s->length, NULL, canonical);
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
                 "module Main\nfn f()\n  var x = 1\n  x += 2\n  x\nend\n");
    check_source("fn f()\ncart.total.net=cart.total.net+item.price\n"
                 "cart.total.net=cart.total.gross+item.price\nend\n",
                 "fn f()\n  cart.total.net += item.price\n"
                 "  cart.total.net = cart.total.gross + item.price\nend\n");
    const char *inline_comment =
        "fn total(order: Order) : Decimal\n"
        "  order.subtotal + order.tax  # the tax is already rounded\nend\n";
    check_source(inline_comment, inline_comment);
    /* A block callback keeps its header inside the call, as reference 5.6
     * does; its mandatory body lines do not break the enclosing brackets. */
    check_source("result=items.map(fn(item)\nitem.price\nend)\n",
                 "result = items.map(fn(item)\n  item.price\nend)\n");
    check_source("values=f([\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
                 "\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"])\n",
                 "values = f([\n  \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\n"
                 "  \"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"\n])\n");
    const char *nested_case =
        "case outer\n  Some(value):\n    case value\n      One: 1\n      Two: 2\n    end\nend\n";
    check_source(nested_case, nested_case);
    check_source("## module docs\nmodule M\n# function docs\nfn f()\n  examples # header\n"
                 "    true\n  end\n\n  if true\n    1\n  else # alternative\n    2\n  end\n"
                 "  # last\nend # close\n# eof\n",
                 NULL);
    check_source("grade=if score>90\n\"A\"\nelse\n\"B\"\nend\n",
                 "grade = if score > 90\n  \"A\"\nelse\n  \"B\"\nend\n");
    check_source("not # prefix\ntrue\nfn( # open\nx, # comma\ny) # parameters\nx\nend # close\n",
                 NULL);
    check_source("module A. # dot first\nB. # dot second\nC\n"
                 "fn f(a: Int, b: Bool) where # where\nA: Copy, B: Copy\n"
                 "  examples # examples\n    true\n  end # examples close\n"
                 "  text = \"#{Pair(x: # colon\n1)}\"\n"
                 "  x = # assign\n    not # not\n    true\n"
                 "  y = a + # binary\n    b\n"
                 "  z = a + # chain first\n    b + # chain second\n    c\n"
                 "  xs. # field dot\n    first. # chain dot\n    other( # call open\n"
                 "    1, 2) # call close\n"
                 "  ( # group open\n    1) # group close\n"
                 "  [ # list open\n    1, 2] # list close\n"
                 "  { # map open\n    1: 2} # map close\n"
                 "  map_text = \"#{{1: # map colon\n2}}\"\n"
                 "  if true\n    1\n  else if false\n    2\n  else # else\n    3\n"
                 "  end # conditional close\nend # function close\n",
                 NULL);
    const char *boundaries[] = {"[1, # final\n]\n",
                                "[1 # final\n]\n",
                                "f(1, # final\n)\n",
                                "f(1 # final\n)\n",
                                "Pair(a: 1, b: 2, # final\n)\n",
                                "Pair(a: 1, b: 2 # final\n)\n",
                                "(1, 2, # final\n)\n",
                                "(1, 2 # final\n)\n",
                                "{1: 2, # final\n}\n",
                                "{1: 2 # final\n}\n",
                                "fn f(x: Int, # final\n)\nend\n",
                                "fn f(x: Int # final\n)\nend\n"};
    for (size_t i = 0; i < sizeof boundaries / sizeof boundaries[0]; i++)
        check_source(boundaries[i], NULL);
    /* 77 display columns, but 101 bytes: a byte-count callback would break. */
    check_source(
        "x=f(\"界界界界界界界界界界界界界界界界界界界界界界界界\",\"abcdefghijklmnop\")\n",
        "x = f(\"界界界界界界界界界界界界界界界界界界界界界界界界\", \"abcdefghijklmnop\")\n");
    char literal[89], source[128], expected[128];
    memset(literal, 'x', sizeof literal - 1);
    literal[sizeof literal - 1] = '\0';
    int written = snprintf(source, sizeof source, "x=\"%s\".length\n", literal);
    MO_ASSERT(written > 0 && (size_t)written < sizeof source);
    written = snprintf(expected, sizeof expected, "x = \"%s\"\n  .length\n", literal);
    MO_ASSERT(written > 0 && (size_t)written < sizeof expected);
    check_source(source, expected);
    files("grammar/tests/good/*.mo", true, true, true);
    files("examples/*.mo", true, true, true);
    files("lib/*.mo", false, true, true);
    files("fuzz/corpus/fuzz_syntax/*", false, false, false);
    reference();
    if (printf("format corpus: %u accepted inputs checked\n", accepted_inputs) < 0)
        return 1;
    return mo_test_report("test_format");
}
