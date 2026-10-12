/* Run the actual grammar fixtures, samples and every reference code block. */
#include "syntax/syntax.h"
#include "test.h"

#include <glob.h>
#include <string.h>

static unsigned checked = 0;

static void check_bytes(const char *path, const uint8_t *bytes, size_t size, bool fragment,
                        bool good) {
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_arena arena;
    MO_ASSERT(mo_sources_init(&sources));
    MO_ASSERT(mo_diags_init(&diags));
    mo_arena_init(&arena, 1 << 29);
    uint32_t file;
    struct mo_source_error problem;
    MO_ASSERT(mo_sources_add_bytes(&sources, path, bytes, size, &file, &problem));
    struct mo_module_syntax *tree = fragment ? mo_parse_fragment(&arena, &sources, file, &diags)
                                             : mo_parse(&arena, &sources, file, &diags);
    bool accepted = tree != NULL && !mo_diags_any(&diags);
    if (accepted != good) {
        (void)fprintf(stderr, "%s: expected %s; got %s\n", path, good ? "accept" : "reject",
                      accepted ? "accept" : "reject");
        for (uint32_t i = 0; i < diags.count; i++)
            (void)fprintf(stderr, "  %u: %s at %u\n", diags.items[i].code, diags.items[i].message,
                          diags.items[i].primary.span.start);
    }
    CHECK(accepted == good);
    checked++;
    mo_arena_free(&arena);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
}

static void files(const char *pattern, bool fragment, bool good) {
    glob_t paths = {0};
    MO_ASSERT(glob(pattern, 0, NULL, &paths) == 0);
    for (size_t i = 0; i < paths.gl_pathc; i++) {
        struct mo_sources sources;
        MO_ASSERT(mo_sources_init(&sources));
        uint32_t file;
        struct mo_source_error problem;
        MO_ASSERT(mo_sources_add(&sources, paths.gl_pathv[i], &file, &problem));
        check_bytes(paths.gl_pathv[i], sources.files[file].bytes, sources.files[file].length,
                    fragment, good);
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
    uint32_t pos = 0, start = 0;
    bool inside = false;
    while (pos < source->length) {
        uint32_t end = pos;
        while (end < source->length && source->bytes[end] != '\n')
            end++;
        if (!inside && end - pos == 5 && memcmp(source->bytes + pos, "```mo", 5) == 0) {
            inside = true;
            start = end + 1;
        } else if (inside && end - pos == 3 && memcmp(source->bytes + pos, "```", 3) == 0) {
            check_bytes("reference code block", source->bytes + start, pos - start, true, true);
            inside = false;
        }
        pos = end < source->length ? end + 1 : end;
    }
    CHECK(!inside);
    mo_sources_free(&sources);
}

int main(void) {
    files("grammar/tests/good/*.mo", true, true);
    files("grammar/tests/bad/*.mo", true, false);
    files("grammar/tests/bad-files/*.mo", false, false);
    files("examples/*.mo", false, true);
    reference();
    if (printf("syntax corpus: %u inputs checked\n", checked) < 0)
        return 1;
    return mo_test_report("test_syntax_corpus");
}
