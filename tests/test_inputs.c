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
    CHECK(mo_inputs_init(&first, &fixture.types, parameters, 2, "Example.checked", 15, &limits) ==
          MO_INPUT_OK);
    CHECK(mo_inputs_init(&second, &fixture.types, parameters, 2, "Example.checked", 15, &limits) ==
          MO_INPUT_OK);
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

int main(void) {
    integer_inputs();
    return mo_test_report("promise inputs");
}
