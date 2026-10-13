#include "prim/prim.h"
#include "test.h"

#include <string.h>

static void integer_power(void) {
    struct mo_arena arena;
    struct mo_types types;
    mo_arena_init(&arena, 1 << 20);
    mo_types_init(&types, &arena);
    uint32_t integer, signature;
    struct mo_type t = {.kind = MO_TYPE_INT};
    MO_ASSERT(mo_types_intern(&types, &t, &integer) == MO_TYPES_OK);
    uint32_t params[] = {integer, integer};
    t = (struct mo_type){
        .kind = MO_TYPE_FUNCTION, .parameters = params, .parameter_count = 2, .result = integer};
    MO_ASSERT(mo_types_intern(&types, &t, &signature) == MO_TYPES_OK);
    MO_ASSERT(mo_types_finish(&types) == MO_TYPES_OK);
    struct mo_limits limits = mo_limits_default();
    struct mo_heap_image *image = mo_heap_image_new(&types, &limits);
    MO_ASSERT(image != NULL);
    struct mo_heap *heap = mo_heap_new(image, &limits);
    MO_ASSERT(heap != NULL);
    struct mo_cell args[] = {mo_cell_int(-2), mo_cell_int(63)};
    struct mo_cell out = mo_cell_int(41);
    const char *reason = NULL;
    CHECK(mo_prim_call(heap, &types, MO_PRIM_INT_POW, signature, args, &out, &reason) ==
          MO_PRIM_RETURNED);
    CHECK(mo_cell_get_int(out) == INT64_MIN);
    args[0] = mo_cell_int(2);
    out = mo_cell_int(41);
    struct mo_cell before[2];
    memcpy(before, args, sizeof before);
    CHECK(mo_prim_call(heap, &types, MO_PRIM_INT_POW, signature, args, &out, &reason) ==
          MO_PRIM_OVERFLOW);
    CHECK(reason != NULL && strcmp(reason, "Int") == 0);
    CHECK(mo_cell_get_int(out) == 41 && memcmp(args, before, sizeof before) == 0);
    mo_heap_free(heap);
    mo_heap_image_free(image);
    mo_arena_free(&arena);
}

int main(void) {
    integer_power();
    return mo_test_report("primitives");
}
