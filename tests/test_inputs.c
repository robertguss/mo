#include "test.h"

#include "heap_fixture.h"
#include "tools/inputs.h"

static void integer_inputs(void) {
    struct heap_fixture fixture;
    heap_fixture_init(&fixture);
    struct mo_limits heap_limits = mo_limits_default();
    struct mo_heap *heap = mo_heap_new(fixture.image, &heap_limits);
    MO_ASSERT(heap != NULL);
    uint32_t parameters[] = {fixture.builtin[MO_TYPE_INT], fixture.builtin[MO_TYPE_INT]};
    struct mo_input_limits limits = mo_input_limits_default();
    struct mo_inputs first, second;
    CHECK(mo_inputs_init(&first, &fixture.types, parameters, 2, "Example.checked", 15, &limits,
                         NULL, 0) == MO_INPUT_OK);
    CHECK(mo_inputs_init(&second, &fixture.types, parameters, 2, "Example.checked", 15, &limits,
                         NULL, 0) == MO_INPUT_OK);
    const int64_t boundaries[] = {0, 1, -1, INT64_MIN, INT64_MAX};
    for (unsigned i = 0; i < 100; i++) {
        struct mo_cell a[2], b[2];
        enum mo_input_kind ka, kb;
        CHECK(mo_inputs_next(&first, heap, a, &ka) == MO_INPUT_OK);
        CHECK(mo_inputs_next(&second, heap, b, &kb) == MO_INPUT_OK);
        CHECK(ka == kb);
        CHECK(mo_cell_get_int(a[0]) == mo_cell_get_int(b[0]));
        CHECK(mo_cell_get_int(a[1]) == mo_cell_get_int(b[1]));
        if (i < 25) {
            CHECK(ka == MO_INPUT_BOUNDARY);
            CHECK(mo_cell_get_int(a[0]) == boundaries[i / 5]);
            CHECK(mo_cell_get_int(a[1]) == boundaries[i % 5]);
        } else {
            CHECK(ka == MO_INPUT_RANDOM);
        }
    }
    mo_inputs_free(&second);
    mo_inputs_free(&first);
    mo_heap_free(heap);
    mo_heap_image_free(fixture.image);
    mo_arena_free(&fixture.arena);
}

static void minimal_tuple_without_heap(void) {
    struct heap_fixture fixture;
    heap_fixture_init(&fixture);
    struct mo_limits heap_limits = mo_limits_default();
    struct mo_heap *heap = mo_heap_new(fixture.image, &heap_limits);
    MO_ASSERT(heap != NULL);
    struct mo_input_limits limits = mo_input_limits_default();
    struct mo_inputs inputs;
    MO_ASSERT(mo_inputs_init(&inputs, &fixture.types, &fixture.int_pair, 1, "M.minimum", 9, &limits,
                             NULL, 0) == MO_INPUT_OK);
    struct mo_cell value, candidate = mo_cell_int(41);
    enum mo_input_kind kind;
    MO_ASSERT(mo_inputs_next(&inputs, heap, &value, &kind) == MO_INPUT_OK);
    struct mo_scratch full;
    mo_scratch_init(&full, heap);
    MO_ASSERT(mo_scratch_resize(&full, NULL, 0, full.arena.limit - MO_ARENA_CHUNK_HEADER) != NULL);
    struct mo_input_shrinker shrinker;
    mo_input_shrink_init(&shrinker, &inputs, 200);
    CHECK(mo_input_shrink_next(&shrinker, &value, heap, &candidate) == MO_INPUT_DONE);
    CHECK(mo_cell_get_int(candidate) == 41);
    mo_scratch_free(&full);
    mo_value_drop(heap, fixture.int_pair, value);
    CHECK(mo_heap_objects(heap) == 0);
    mo_inputs_free(&inputs);
    mo_heap_free(heap);
    mo_heap_image_free(fixture.image);
    mo_arena_free(&fixture.arena);
}

static void recursive_boundaries(void) {
    struct heap_fixture fixture;
    heap_fixture_init(&fixture);
    struct mo_limits heap_limits = mo_limits_default();
    struct mo_heap *heap = mo_heap_new(fixture.image, &heap_limits);
    MO_ASSERT(heap != NULL);
    struct mo_input_limits limits = mo_input_limits_default();
    struct mo_inputs inputs;
    MO_ASSERT(mo_inputs_init(&inputs, &fixture.types, &fixture.forest, 1, "M.forest", 8, &limits,
                             NULL, 0) == MO_INPUT_OK);
    for (uint32_t i = 0; i < 3; i++) {
        struct mo_cell value;
        enum mo_input_kind kind;
        enum mo_input_result result = mo_inputs_next(&inputs, heap, &value, &kind);
        CHECK(result == MO_INPUT_OK);
        if (result != MO_INPUT_OK)
            break;
        CHECK(kind == MO_INPUT_BOUNDARY);
        uint32_t size = mo_table_size(mo_value_field(value, 0));
        CHECK(i == 2 ? size >= 2 : size == i);
        mo_value_drop(heap, fixture.forest, value);
    }
    CHECK(mo_heap_objects(heap) == 0);
    mo_inputs_free(&inputs);
    mo_heap_free(heap);
    mo_heap_image_free(fixture.image);
    mo_arena_free(&fixture.arena);
}

int main(void) {
    integer_inputs();
    minimal_tuple_without_heap();
    recursive_boundaries();
    return mo_test_report("promise inputs");
}
