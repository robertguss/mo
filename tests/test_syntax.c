#include "syntax/syntax.h"
#include "test.h"

#include <string.h>

int main(void) {
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_arena arena;
    CHECK(mo_sources_init(&sources));
    CHECK(mo_diags_init(&diags));
    mo_arena_init(&arena, 1 << 24);
    const char *text =
        "module Shop.Orders\nexpose Order(..), total\nuse Shop.Money{Money}\nn = 42\n";
    uint32_t file = 0;
    struct mo_source_error problem;
    CHECK(mo_sources_add_bytes(&sources, "test.mo", (const uint8_t *)text, strlen(text), &file,
                               &problem));
    struct mo_header header;
    CHECK(mo_read_header(&arena, &sources, file, &header, &diags));
    CHECK(header.path->count == 2);
    CHECK(header.expose->count == 2);
    CHECK(header.uses->count == 1);
    struct mo_module_syntax *module = mo_parse(&arena, &sources, file, &diags);
    CHECK(module != NULL && !mo_diags_any(&diags));
    CHECK(module != NULL && module->tree->children[1]->count == 1);
    mo_arena_free(&arena);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
    return mo_test_report("test_syntax");
}
