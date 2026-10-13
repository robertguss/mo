#include "base/limits.h"
#include "syntax/format.h"
#include "test.h"

#include <string.h>

static uint32_t ascii_width(const uint8_t *bytes, size_t length) {
    (void)bytes;
    return (uint32_t)length;
}

int main(void) {
    const char *source = "module Main\nfn f()\nvar x=1\nx=x+2\nx\nend\n";
    const char *expected = "module Main\n\nfn f()\n  var x = 1\n  x += 2\n  x\nend\n";
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_arena arena;
    MO_ASSERT(mo_sources_init(&sources));
    MO_ASSERT(mo_diags_init(&diags));
    mo_arena_init(&arena, 1 << 26);
    uint32_t file;
    struct mo_source_error error;
    MO_ASSERT(mo_sources_add_bytes(&sources, "test.mo", (const uint8_t *)source, strlen(source),
                                   &file, &error));
    struct mo_module_syntax *module = mo_parse(&arena, &sources, file, &diags);
    MO_ASSERT(module != NULL && !mo_diags_any(&diags));
    struct mo_buffer output;
    mo_buffer_init(&output, MO_LIMIT_FILE_SIZE);
    CHECK(mo_format(module, NULL, (struct mo_format_options){MO_FORMAT_WIDTH, 100, ascii_width},
                    &output));
    CHECK(output.length == strlen(expected));
    if (output.length == strlen(expected))
        CHECK(memcmp(output.bytes, expected, output.length) == 0);
    mo_buffer_free(&output);
    mo_arena_free(&arena);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
    return mo_test_report("test_format");
}
