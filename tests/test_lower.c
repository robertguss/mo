#include "base/codes.h"
#include "base/limits.h"
#include "lower/lower.h"
#include "test.h"

static void test_refusal(void) {
    struct mo_arena arena;
    struct mo_sources sources;
    struct mo_diags diags;
    mo_arena_init(&arena, 1 << 20);
    CHECK(mo_sources_init(&sources));
    CHECK(mo_diags_init(&diags));
    struct mo_program program = {0};
    CHECK(mo_lower(&arena, &program, &sources, &diags) == NULL);
    CHECK(diags.count == 1 && diags.items[0].code == MO_CODE_LOWER_UNFINISHED);
    mo_arena_free(&arena);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
}

int main(void) {
    test_refusal();
    return mo_test_report("lower");
}
