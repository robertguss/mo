#include "prim_fixture.h"
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

static void integer_floor(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, 1 << 20);
    struct mo_cell args[] = {mo_cell_int(7), mo_cell_int(-3)}, out = mo_cell_int(41);
    const char *reason = NULL;
    CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_INT_FLOOR_DIV, f.signature[MO_PRIM_INT_FLOOR_DIV],
                       args, &out, &reason) == MO_PRIM_RETURNED);
    CHECK(mo_cell_get_int(out) == -3);
    CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_INT_FLOOR_MOD, f.signature[MO_PRIM_INT_FLOOR_MOD],
                       args, &out, &reason) == MO_PRIM_RETURNED);
    CHECK(mo_cell_get_int(out) == -2);
    args[0] = mo_cell_int(INT64_MIN);
    args[1] = mo_cell_int(-1);
    CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_INT_FLOOR_MOD, f.signature[MO_PRIM_INT_FLOOR_MOD],
                       args, &out, &reason) == MO_PRIM_RETURNED);
    CHECK(mo_cell_get_int(out) == 0);
    CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_INT_FLOOR_DIV, f.signature[MO_PRIM_INT_FLOOR_DIV],
                       args, &out, &reason) == MO_PRIM_OVERFLOW);
    CHECK(strcmp(reason, "Int") == 0 && mo_cell_get_int(out) == 0);
    args[1] = mo_cell_int(0);
    CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_INT_FLOOR_DIV, f.signature[MO_PRIM_INT_FLOOR_DIV],
                       args, &out, &reason) == MO_PRIM_DIV_BY_ZERO);
    CHECK(strcmp(reason, "dividing by zero") == 0 && mo_cell_get_int(out) == 0);
    prim_fixture_free(&f);
}

static void decimal_rounding(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, 1 << 20);
    struct mo_cell args[] = {mo_cell_decimal(INT64_C(-50000000000000000)), mo_cell_int(1)};
    struct mo_cell out = mo_cell_int(41);
    const char *reason = NULL;
    CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_DECIMAL_ROUND, f.signature[MO_PRIM_DECIMAL_ROUND],
                       args, &out, &reason) == MO_PRIM_RETURNED);
    CHECK(mo_cell_get_decimal(out) == INT64_C(-100000000000000000));
    args[0] = mo_cell_decimal((mo_i128)(((mo_u128)1 << 127) - 1));
    args[1] = mo_cell_int(0);
    out = mo_cell_int(41);
    CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_DECIMAL_ROUND, f.signature[MO_PRIM_DECIMAL_ROUND],
                       args, &out, &reason) == MO_PRIM_OVERFLOW);
    CHECK(strcmp(reason, "Decimal") == 0 && mo_cell_get_int(out) == 41);
    CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_DECIMAL_TO_FIXED,
                       f.signature[MO_PRIM_DECIMAL_TO_FIXED], args, &out,
                       &reason) == MO_PRIM_RETURNED);
    size_t length;
    const uint8_t *bytes = mo_value_data(out, &length);
    CHECK(length == 21 && memcmp(bytes, "170141183460469231732", 21) == 0);
    mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], out);
    prim_fixture_free(&f);
}

int main(void) {
    integer_power();
    integer_floor();
    decimal_rounding();
    return mo_test_report("primitives");
}
