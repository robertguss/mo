#include "prim_oracle.h"
#include "test.h"

#include <fenv.h>
#include <math.h>
#include <string.h>

static void empty_constants_and_rounding(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, 1 << 20);
    const enum mo_primitive_id ids[] = {MO_PRIM_LIST_EMPTY, MO_PRIM_MAP_EMPTY, MO_PRIM_SET_EMPTY};
    const char *reason;
    struct mo_cell out;
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        const struct mo_type *fn = &f.types.items[f.signature[ids[i]]];
        MO_ASSERT(fn->kind == MO_TYPE_FUNCTION && fn->parameter_count == 0);
        CHECK(mo_prim_call(f.heap, &f.types, ids[i], f.signature[ids[i]], NULL, &out, &reason) ==
              MO_PRIM_RETURNED);
        CHECK(reason == NULL);
        CHECK(ids[i] == MO_PRIM_LIST_EMPTY ? mo_list_size(out) == 0 : mo_table_size(out) == 0);
        mo_value_drop(f.heap, fn->result, out);
    }
    MO_ASSERT(fesetround(FE_UPWARD) == 0);
    struct mo_cell arg = mo_cell_float(2.0);
    MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_FLOAT_SQRT, f.signature[MO_PRIM_FLOAT_SQRT],
                           &arg, &out, &reason) == MO_PRIM_RETURNED);
    CHECK(fegetround() == FE_TONEAREST && mo_cell_get_float(out) == sqrt(2.0));
    prim_fixture_free(&f);
}

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

static void text_split(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, 1 << 20);
    struct mo_cell args[2], out = mo_cell_int(41);
    CHECK(mo_value_text(f.heap, "ababa", 5, &args[0]));
    CHECK(mo_value_text(f.heap, "aba", 3, &args[1]));
    const char *reason;
    enum mo_prim_status status = mo_prim_call(f.heap, &f.types, MO_PRIM_TEXT_SPLIT,
                                              f.signature[MO_PRIM_TEXT_SPLIT], args, &out, &reason);
    CHECK(status == MO_PRIM_RETURNED);
    if (status == MO_PRIM_RETURNED) {
        struct mo_cell piece;
        size_t length;
        CHECK(mo_list_size(out) == 2);
        MO_ASSERT(mo_list_get(out, 0, &piece));
        const uint8_t *bytes = mo_value_data(piece, &length);
        CHECK(length == 0);
        CHECK(mo_list_get(out, 1, &piece));
        bytes = mo_value_data(piece, &length);
        CHECK(length == 2 && memcmp(bytes, "ba", 2) == 0);
        mo_value_drop(f.heap, f.types.items[f.signature[MO_PRIM_TEXT_SPLIT]].result, out);
    }
    mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], args[0]);
    mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], args[1]);
    prim_fixture_free(&f);
}

static void collection_ownership(void) {
    struct prim_fixture f;
    prim_fixture_init_type(&f, 1 << 20, MO_TYPE_TEXT);
    size_t baseline = mo_heap_used(f.heap);
    uint32_t type = f.types.items[f.signature[MO_PRIM_LIST_APPEND]].result;
    uint32_t text = f.builtin[MO_TYPE_TEXT];
    const char *reason;
    for (unsigned shared = 0; shared < 2; shared++) {
        struct mo_cell a, b, args[3], out;
        MO_ASSERT(mo_value_text(f.heap, "a", 1, &a));
        MO_ASSERT(mo_value_text(f.heap, "b", 1, &b));
        MO_ASSERT(mo_value_list(f.heap, type, &a, 1, &args[0]));
        mo_value_drop(f.heap, text, a);
        struct mo_cell retained = shared ? mo_value_dup(f.heap, type, args[0]) : mo_cell_unit();
        args[1] = b;
        MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_LIST_APPEND,
                               f.signature[MO_PRIM_LIST_APPEND], args, &out,
                               &reason) == MO_PRIM_RETURNED);
        CHECK(reason == NULL && mo_list_size(out) == 2);
        if (shared) {
            CHECK(mo_list_size(retained) == 1);
            mo_value_drop(f.heap, type, retained);
        }
        args[0] = out;
        args[1] = mo_cell_int(0);
        MO_ASSERT(mo_value_text(f.heap, "replacement", 11, &args[2]));
        MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_LIST_SET, f.signature[MO_PRIM_LIST_SET],
                               args, &out, &reason) == MO_PRIM_RETURNED);
        CHECK(mo_cell_get_tag(out) == 1);
        struct mo_cell value;
        MO_ASSERT(mo_list_get(mo_cell_get_payload(out), 0, &value));
        size_t length;
        const uint8_t *bytes = mo_value_data(value, &length);
        CHECK(length == 11 && memcmp(bytes, "replacement", 11) == 0);
        mo_value_drop(f.heap, f.types.items[f.signature[MO_PRIM_LIST_SET]].result, out);
        CHECK(mo_heap_objects(f.heap) == 0 && mo_heap_used(f.heap) == baseline);
    }
    prim_fixture_free(&f);
}

static void refusal_preserves_operands(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, 16384);
    size_t baseline = mo_heap_used(f.heap);
    uint32_t map = f.types.items[f.signature[MO_PRIM_MAP_SET]].result;
    uint32_t text = f.builtin[MO_TYPE_TEXT];
    struct mo_cell args[3], out = mo_cell_int(41);
    const char *reason;
    MO_ASSERT(mo_value_map(f.heap, map, NULL, NULL, 0, &args[0]));
    args[1] = mo_cell_int(7);
    MO_ASSERT(mo_value_text(f.heap, "original", 8, &args[2]));
    MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_MAP_SET, f.signature[MO_PRIM_MAP_SET], args,
                           &out, &reason) == MO_PRIM_RETURNED);
    args[0] = out;
    MO_ASSERT(mo_value_text(f.heap, "duplicate", 9, &args[2]));
    out = mo_cell_int(41);
    CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_MAP_LITERAL_INSERT,
                       f.signature[MO_PRIM_MAP_LITERAL_INSERT], args, &out,
                       &reason) == MO_PRIM_REPEATED_KEY);
    CHECK(mo_cell_get_int(out) == 41 && reason != NULL && mo_table_size(args[0]) == 1);
    struct mo_cell value;
    bool found;
    MO_ASSERT(mo_table_get(f.heap, map, args[0], args[1], &found, &value) && found);
    size_t length;
    const uint8_t *bytes = mo_value_data(value, &length);
    CHECK(length == 8 && memcmp(bytes, "original", 8) == 0);
    bytes = mo_value_data(args[2], &length);
    CHECK(length == 9 && memcmp(bytes, "duplicate", 9) == 0);
    mo_value_drop(f.heap, map, args[0]);
    mo_value_drop(f.heap, text, args[2]);

    MO_ASSERT(mo_value_text(f.heap, "ab", 2, &args[0]));
    for (unsigned pass = 0; pass < 2; pass++) {
        args[1] = mo_cell_int(pass == 0 ? -1 : 20000);
        enum mo_prim_status status =
            mo_prim_call(f.heap, &f.types, MO_PRIM_TEXT_REPEAT, f.signature[MO_PRIM_TEXT_REPEAT],
                         args, &out, &reason);
        CHECK(status == (pass == 0 ? MO_PRIM_OUTSIDE : MO_PRIM_OUT_OF_MEMORY));
        CHECK(reason != NULL && mo_cell_get_int(out) == 41);
        bytes = mo_value_data(args[0], &length);
        CHECK(length == 2 && memcmp(bytes, "ab", 2) == 0);
    }
    mo_value_drop(f.heap, text, args[0]);
    CHECK(mo_heap_objects(f.heap) == 0 && mo_heap_used(f.heap) == baseline);
    prim_fixture_free(&f);
}

/* These sizes distinguish constant-amortized updates and KMP from repeated
 * copies or rescanning long equal prefixes. ci/check_primitives.py times them. */
static void primitive_work(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, MO_DEFAULT_HEAP_SIZE);
    size_t baseline = mo_heap_used(f.heap);
    uint32_t list = f.types.items[f.signature[MO_PRIM_LIST_APPEND]].result;
    uint32_t map = f.types.items[f.signature[MO_PRIM_MAP_SET]].result;
    struct mo_cell args[3], out;
    const char *reason;
    MO_ASSERT(mo_value_list(f.heap, list, NULL, 0, &args[0]));
    for (uint32_t i = 0; i < 100000; i++) {
        args[1] = mo_cell_int(i);
        MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_LIST_APPEND,
                               f.signature[MO_PRIM_LIST_APPEND], args, &out,
                               &reason) == MO_PRIM_RETURNED);
        args[0] = out;
    }
    MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_LIST_UNIQUE, f.signature[MO_PRIM_LIST_UNIQUE],
                           args, &out, &reason) == MO_PRIM_RETURNED);
    CHECK(mo_list_size(out) == 100000);
    struct mo_cell last;
    MO_ASSERT(mo_list_get(out, 99999, &last));
    CHECK(mo_cell_get_int(last) == 99999);
    mo_value_drop(f.heap, list, out);

    MO_ASSERT(mo_value_map(f.heap, map, NULL, NULL, 0, &args[0]));
    for (uint32_t i = 0; i < 100000; i++) {
        args[1] = mo_cell_int(i);
        MO_ASSERT(mo_value_text(f.heap, "x", 1, &args[2]));
        MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_MAP_SET, f.signature[MO_PRIM_MAP_SET],
                               args, &out, &reason) == MO_PRIM_RETURNED);
        args[0] = out;
    }
    CHECK(mo_table_size(args[0]) == 100000);
    mo_value_drop(f.heap, map, args[0]);

    struct mo_arena arena;
    mo_arena_init(&arena, 8 << 20);
    size_t count = 4 << 20;
    uint8_t *bytes = mo_arena_allocate(&arena, count, 1);
    MO_ASSERT(bytes != NULL);
    memset(bytes, 'a', count);
    MO_ASSERT(mo_value_text(f.heap, (const char *)bytes, count, &args[0]));
    bytes[(1 << 20) - 1] = 'b';
    MO_ASSERT(mo_value_text(f.heap, (const char *)bytes, 1 << 20, &args[1]));
    MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_TEXT_CONTAINS,
                           f.signature[MO_PRIM_TEXT_CONTAINS], args, &out,
                           &reason) == MO_PRIM_RETURNED);
    CHECK(!mo_cell_get_bool(out));
    mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], args[0]);
    mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], args[1]);

    count = 16384;
    memset(bytes, 255, count);
    MO_ASSERT(mo_value_bigint(f.heap, MO_BIGINT_POSITIVE, bytes, count, &args[0]));
    args[1] = args[0];
    MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_BIGINT_MUL, f.signature[MO_PRIM_BIGINT_MUL],
                           args, &out, &reason) == MO_PRIM_RETURNED);
    size_t length;
    const uint8_t *result = mo_value_data(out, &length);
    CHECK(length == 2 * count && result[0] == 1 && result[count] == 254);
    for (size_t i = 1; i < count; i++)
        CHECK(result[i] == 0 && result[count + i] == 255);
    mo_value_drop(f.heap, f.builtin[MO_TYPE_BIGINT], args[0]);
    mo_value_drop(f.heap, f.builtin[MO_TYPE_BIGINT], out);
    mo_arena_free(&arena);
    CHECK(mo_heap_objects(f.heap) == 0 && mo_heap_used(f.heap) == baseline);
    prim_fixture_free(&f);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--oracle") == 0) {
        primitive_oracle();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--work") == 0) {
        primitive_work();
        return mo_test_report("primitive work");
    }
    MO_ASSERT(argc == 1);
    empty_constants_and_rounding();
    integer_power();
    integer_floor();
    decimal_rounding();
    text_split();
    collection_ownership();
    refusal_preserves_operands();
    return mo_test_report("primitives");
}
