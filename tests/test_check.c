#include "base/codes.h"
#include "check/check.h"
#include "test.h"

#include <string.h>

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

int main(void) {
    load_graph();
    return mo_test_report("check");
}
