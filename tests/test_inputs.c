#include "test.h"

#include "heap_fixture.h"
#include "input_values.h"
#include "tools/inputs.h"

#include <string.h>

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
            unsigned left = i == 0 ? 0 : i < 5 ? i : i < 9 ? 0 : 1 + (i - 9) / 4;
            unsigned right = i < 5 ? 0 : i < 9 ? i - 4 : 1 + (i - 9) % 4;
            CHECK(mo_cell_get_int(a[0]) == boundaries[left]);
            CHECK(mo_cell_get_int(a[1]) == boundaries[right]);
        } else {
            CHECK(ka == MO_INPUT_RANDOM);
        }
    }
    mo_inputs_free(&second);
    mo_inputs_free(&first);
    limits.combinations = 0;
    MO_ASSERT(mo_inputs_init(&first, &fixture.types, parameters, 2, "Example.fn.checkA", 17,
                             &limits, NULL, 0) == MO_INPUT_OK);
    MO_ASSERT(mo_inputs_init(&second, &fixture.types, parameters, 2, "Example.fn.checkB", 17,
                             &limits, NULL, 0) == MO_INPUT_OK);
    bool different = false;
    for (unsigned i = 0; i < 16; i++) {
        struct mo_cell a[2], b[2];
        enum mo_input_kind ka, kb;
        MO_ASSERT(mo_inputs_next(&first, heap, a, &ka) == MO_INPUT_OK);
        MO_ASSERT(mo_inputs_next(&second, heap, b, &kb) == MO_INPUT_OK);
        if (memcmp(a, b, sizeof a) != 0)
            different = true;
    }
    CHECK(different);
    mo_inputs_free(&second);
    mo_inputs_free(&first);
    MO_ASSERT(mo_inputs_init(&first, &fixture.types, parameters, 2, "Example.stream", 14, &limits,
                             NULL, 0) == MO_INPUT_OK);
    int64_t previous = 0;
    unsigned shifted = 0;
    for (unsigned i = 0; i < 600; i++) {
        struct mo_cell pair[2];
        enum mo_input_kind kind;
        MO_ASSERT(mo_inputs_next(&first, heap, pair, &kind) == MO_INPUT_OK);
        if (i >= 300 && mo_cell_get_int(pair[0]) == previous)
            shifted++;
        previous = mo_cell_get_int(pair[1]);
    }
    /* Once Ints use all 64 bits, adjacent streams must not be shifted copies. */
    CHECK(shifted < 4);
    mo_inputs_free(&first);
    mo_heap_free(heap);
    mo_heap_image_free(fixture.image);
    mo_arena_free(&fixture.arena);
}

static void boundary_coverage(void) {
    struct heap_fixture fixture;
    heap_fixture_init(&fixture);
    struct mo_limits heap_limits = mo_limits_default();
    struct mo_heap *heap = mo_heap_new(fixture.image, &heap_limits);
    MO_ASSERT(heap != NULL);
    uint32_t parameters[8];
    for (unsigned p = 0; p < 8; p++)
        parameters[p] = fixture.builtin[MO_TYPE_INT];
    const int64_t boundaries[] = {0, 1, -1, INT64_MIN, INT64_MAX};
    struct mo_input_limits limits = mo_input_limits_default();
    for (uint32_t count = 1; count <= 8; count++) {
        struct mo_inputs inputs;
        MO_ASSERT(mo_inputs_init(&inputs, &fixture.types, parameters, count, "M.coverage", 10,
                                 &limits, NULL, 0) == MO_INPUT_OK);
        unsigned seen[8] = {0};
        for (unsigned trial = 0; trial < 200; trial++) {
            struct mo_cell values[8];
            enum mo_input_kind kind;
            MO_ASSERT(mo_inputs_next(&inputs, heap, values, &kind) == MO_INPUT_OK);
            if (kind != MO_INPUT_BOUNDARY)
                break;
            for (uint32_t p = 0; p < count; p++) {
                int64_t value = mo_cell_get_int(values[p]);
                for (unsigned b = 0; b < 5; b++)
                    if (value == boundaries[b])
                        seen[p] |= 1U << b;
            }
        }
        for (uint32_t p = 0; p < count; p++)
            CHECK(seen[p] == 31);
        mo_inputs_free(&inputs);
    }
    mo_heap_free(heap);
    mo_heap_image_free(fixture.image);
    mo_arena_free(&fixture.arena);
}

static void scalar_boundaries(void) {
    struct heap_fixture fixture;
    heap_fixture_init(&fixture);
    struct mo_limits heap_limits = mo_limits_default();
    struct mo_heap *heap = mo_heap_new(fixture.image, &heap_limits);
    MO_ASSERT(heap != NULL);
    const enum mo_type_kind kinds[] = {MO_TYPE_DECIMAL, MO_TYPE_TIME, MO_TYPE_DURATION,
                                       MO_TYPE_TEXT};
    const int64_t times[] = {0, -INT64_C(62135596800000000), -INT64_C(62104060800000001),
                             INT64_C(253370764800000000), INT64_C(253402300799999999)};
    const int64_t durations[] = {0, INT64_MIN, INT64_MAX};
    mo_i128 max = (mo_i128)(((mo_u128)1 << 127) - 1);
    const mo_i128 decimals[] = {
        0, 1, INT64_C(1000000000000000000), -INT64_C(1000000000000000000), -max, max};
    const uint32_t counts[] = {6, 5, 3, 4};
    struct mo_input_limits limits = mo_input_limits_default();
    for (unsigned k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
        uint32_t type = fixture.builtin[kinds[k]];
        struct mo_inputs inputs;
        MO_ASSERT(mo_inputs_init(&inputs, &fixture.types, &type, 1, "M.scalars", 9, &limits, NULL,
                                 0) == MO_INPUT_OK);
        for (uint32_t i = 0; i <= counts[k]; i++) {
            struct mo_cell value;
            enum mo_input_kind kind;
            MO_ASSERT(mo_inputs_next(&inputs, heap, &value, &kind) == MO_INPUT_OK);
            CHECK(kind == (i == counts[k] ? MO_INPUT_RANDOM : MO_INPUT_BOUNDARY));
            if (i < counts[k]) {
                if (kinds[k] == MO_TYPE_DECIMAL)
                    CHECK(mo_cell_get_decimal(value) == decimals[i]);
                else if (kinds[k] == MO_TYPE_TIME)
                    CHECK(mo_cell_get_time(value) == times[i]);
                else if (kinds[k] == MO_TYPE_DURATION)
                    CHECK(mo_cell_get_duration(value) == durations[i]);
                else {
                    size_t length;
                    const uint8_t *bytes = mo_value_data(value, &length);
                    if (i == 0)
                        CHECK(length == 0);
                    else if (i == 1)
                        CHECK(length == 1 && bytes[0] == 'a');
                    else if (i == 2)
                        CHECK(length == 2 && memcmp(bytes, "\xc3\xa9", 2) == 0);
                    else {
                        CHECK(length == 1024);
                        for (size_t b = 0; b < length; b++)
                            CHECK(bytes[b] == 'x');
                    }
                }
                struct mo_input_extreme extreme = {.parameter = 99};
                bool expected = kinds[k] == MO_TYPE_DECIMAL ? i >= 4
                                : kinds[k] == MO_TYPE_TIME  ? i == 1 || i == 4
                                                           : kinds[k] == MO_TYPE_DURATION && i != 0;
                enum mo_input_result result = mo_inputs_extreme(&inputs, &value, 0, &extreme);
                CHECK(result == (expected ? MO_INPUT_OK : MO_INPUT_DONE));
                if (expected) {
                    CHECK(extreme.parameter == 0 && extreme.type == kinds[k]);
                    CHECK(extreme.end ==
                          (i + 1 == counts[k] ? MO_INPUT_LARGEST : MO_INPUT_SMALLEST));
                } else
                    CHECK(extreme.parameter == 99);
            }
            mo_value_drop(heap, type, value);
        }
        mo_inputs_free(&inputs);
    }
    CHECK(mo_heap_objects(heap) == 0);
    mo_heap_free(heap);
    mo_heap_image_free(fixture.image);
    mo_arena_free(&fixture.arena);
}

static void shrink_numbers(void) {
    struct heap_fixture fixture;
    heap_fixture_init(&fixture);
    struct mo_limits heap_limits = mo_limits_default();
    struct mo_heap *heap = mo_heap_new(fixture.image, &heap_limits);
    MO_ASSERT(heap != NULL);
    uint32_t parameters[] = {fixture.builtin[MO_TYPE_INT], fixture.builtin[MO_TYPE_INT]};
    struct mo_input_limits limits = mo_input_limits_default();
    struct mo_inputs inputs;
    MO_ASSERT(mo_inputs_init(&inputs, &fixture.types, parameters, 2, "M.shrink", 8, &limits, NULL,
                             0) == MO_INPUT_OK);
    struct mo_cell best[] = {mo_cell_int(1000), mo_cell_int(-1000)};
    struct mo_input_shrinker shrinker;
    mo_input_shrink_init(&shrinker, &inputs, 200);
    unsigned calls = 0;
    for (;;) {
        struct mo_cell candidate[2];
        enum mo_input_result result = mo_input_shrink_next(&shrinker, best, heap, candidate);
        if (result == MO_INPUT_DONE)
            break;
        MO_ASSERT(result == MO_INPUT_OK);
        calls++;
        bool fails = mo_cell_get_int(candidate[0]) >= 17 && mo_cell_get_int(candidate[1]) <= -23;
        if (fails)
            memcpy(best, candidate, sizeof best);
        mo_input_shrink_feedback(&shrinker, fails);
    }
    CHECK(calls > 0 && calls < 200);
    CHECK(mo_cell_get_int(best[0]) == 17 && mo_cell_get_int(best[1]) == -23);
    mo_input_shrink_init(&shrinker, &inputs, 1);
    struct mo_cell candidate[2];
    MO_ASSERT(mo_input_shrink_next(&shrinker, best, heap, candidate) == MO_INPUT_OK);
    mo_input_shrink_feedback(&shrinker, false);
    CHECK(mo_input_shrink_next(&shrinker, best, heap, candidate) == MO_INPUT_DONE);
    mo_inputs_free(&inputs);
    mo_heap_free(heap);
    mo_heap_image_free(fixture.image);
    mo_arena_free(&fixture.arena);
}

static void shrink_thresholds(void) {
    struct heap_fixture fixture;
    heap_fixture_init(&fixture);
    struct mo_limits heap_limits = mo_limits_default();
    struct mo_heap *heap = mo_heap_new(fixture.image, &heap_limits);
    MO_ASSERT(heap != NULL);
    struct mo_input_limits limits = mo_input_limits_default();
    for (unsigned model = 0; model < 3; model++) {
        uint32_t type = fixture.builtin[model == 2 ? MO_TYPE_DECIMAL : MO_TYPE_INT];
        uint32_t parameters[] = {type, type};
        uint32_t count = model == 1 ? 2U : 1U;
        struct mo_inputs inputs;
        MO_ASSERT(mo_inputs_init(&inputs, &fixture.types, parameters, count, "M.threshold", 11,
                                 &limits, NULL, 0) == MO_INPUT_OK);
        mo_i128 decimal_max = (mo_i128)(((mo_u128)1 << 127) - 1);
        struct mo_cell best[] = {model == 2   ? mo_cell_decimal(decimal_max)
                                 : model == 0 ? mo_cell_int(INT64_MAX)
                                              : mo_cell_int(INT64_C(1) << 40),
                                 mo_cell_int(INT64_C(1) << 40)};
        struct mo_input_shrinker shrinker;
        mo_input_shrink_init(&shrinker, &inputs, 200);
        for (;;) {
            struct mo_cell candidate[2];
            enum mo_input_result result = mo_input_shrink_next(&shrinker, best, heap, candidate);
            if (result == MO_INPUT_DONE)
                break;
            MO_ASSERT(result == MO_INPUT_OK);
            bool fails =
                model == 2 ? mo_cell_get_decimal(candidate[0]) > INT64_C(1500000000000000000)
                : model == 0
                    ? mo_cell_get_int(candidate[0]) > 150
                    : mo_cell_get_int(candidate[0]) > 100 && mo_cell_get_int(candidate[1]) > 100;
            if (fails)
                memcpy(best, candidate, count * sizeof *best);
            mo_input_shrink_feedback(&shrinker, fails);
        }
        if (model == 2)
            CHECK(mo_cell_get_decimal(best[0]) == INT64_C(1500000000000000001));
        else {
            CHECK(mo_cell_get_int(best[0]) == (model == 0 ? 151 : 101));
            if (count == 2)
                CHECK(mo_cell_get_int(best[1]) == 101);
        }
        mo_inputs_free(&inputs);
    }
    mo_heap_free(heap);
    mo_heap_image_free(fixture.image);
    mo_arena_free(&fixture.arena);
}

static MO_MUST_USE bool has_beta(struct mo_cell value) {
    size_t length;
    const uint8_t *bytes = mo_value_data(value, &length);
    for (size_t i = 1; i < length; i++) {
        if (bytes[i - 1] == 0xce && bytes[i] == 0xb2)
            return true;
    }
    return false;
}

static MO_MUST_USE bool record_fails(struct mo_cell value) {
    if (!has_beta(mo_value_field(value, 0)))
        return false;
    struct mo_cell list = mo_value_field(value, 1);
    for (uint32_t i = 0; i < mo_list_size(list); i++) {
        struct mo_cell item;
        MO_ASSERT(mo_list_get(list, i, &item));
        if (has_beta(item))
            return true;
    }
    return false;
}

static void shrink_record_across_heaps(void) {
    struct heap_fixture fixture;
    heap_fixture_init(&fixture);
    struct mo_limits heap_limits = mo_limits_default();
    struct mo_heap *heaps[] = {mo_heap_new(fixture.image, &heap_limits),
                               mo_heap_new(fixture.image, &heap_limits)};
    MO_ASSERT(heaps[0] != NULL && heaps[1] != NULL);
    struct mo_input_limits limits = mo_input_limits_default();
    struct mo_inputs inputs;
    MO_ASSERT(mo_inputs_init(&inputs, &fixture.types, &fixture.record, 1, "M.record", 8, &limits,
                             NULL, 0) == MO_INPUT_OK);
    struct mo_cell elements[3], fields[2], best;
    MO_ASSERT(mo_value_text(heaps[0], "discard", 7, &elements[0]));
    MO_ASSERT(mo_value_text(heaps[0], "\xce\xb2\xce\xb2", 4, &elements[1]));
    MO_ASSERT(mo_value_text(heaps[0], "also discard", 12, &elements[2]));
    MO_ASSERT(mo_value_text(heaps[0], "left\xce\xb2right", 11, &fields[0]));
    MO_ASSERT(mo_value_list(heaps[0], fixture.list, elements, 3, &fields[1]));
    MO_ASSERT(mo_value_construct(heaps[0], fixture.record_layout, fields, 2, &best));
    for (unsigned i = 0; i < 3; i++)
        mo_value_drop(heaps[0], fixture.builtin[MO_TYPE_TEXT], elements[i]);
    mo_value_drop(heaps[0], fixture.builtin[MO_TYPE_TEXT], fields[0]);
    mo_value_drop(heaps[0], fixture.list, fields[1]);
    struct mo_input_shrinker shrinker;
    mo_input_shrink_init(&shrinker, &inputs, 200);
    unsigned current = 0, calls = 0;
    for (;;) {
        struct mo_cell candidate;
        size_t before = mo_heap_used(heaps[current]);
        enum mo_input_result result =
            mo_input_shrink_next(&shrinker, &best, heaps[1 - current], &candidate);
        CHECK(mo_heap_used(heaps[current]) == before);
        if (result == MO_INPUT_DONE)
            break;
        MO_ASSERT(result == MO_INPUT_OK);
        calls++;
        bool fails = record_fails(candidate);
        if (fails) {
            mo_value_drop(heaps[current], fixture.record, best);
            current = 1 - current;
            best = candidate;
        } else
            mo_value_drop(heaps[1 - current], fixture.record, candidate);
        mo_input_shrink_feedback(&shrinker, fails);
    }
    CHECK(calls > 0 && calls < 200 && record_fails(best));
    size_t length;
    const uint8_t *bytes = mo_value_data(mo_value_field(best, 0), &length);
    CHECK(length == 2 && memcmp(bytes, "\xce\xb2", 2) == 0);
    struct mo_cell list = mo_value_field(best, 1), item;
    CHECK(mo_list_size(list) == 1);
    MO_ASSERT(mo_list_get(list, 0, &item));
    bytes = mo_value_data(item, &length);
    CHECK(length == 2 && memcmp(bytes, "\xce\xb2", 2) == 0);
    mo_value_drop(heaps[current], fixture.record, best);
    CHECK(mo_heap_objects(heaps[0]) == 0 && mo_heap_objects(heaps[1]) == 0);
    mo_inputs_free(&inputs);
    mo_heap_free(heaps[1]);
    mo_heap_free(heaps[0]);
    mo_heap_image_free(fixture.image);
    mo_arena_free(&fixture.arena);
}

static void all_types(void) {
    struct heap_fixture fixture;
    heap_fixture_init(&fixture);
    struct mo_limits heap_limits = mo_limits_default();
    struct mo_heap *heap = mo_heap_new(fixture.image, &heap_limits);
    MO_ASSERT(heap != NULL);
    uint32_t types[32];
    unsigned count = 0;
    for (unsigned i = 0; i <= MO_TYPE_BIGINT; i++)
        types[count++] = fixture.builtin[i];
    const uint32_t composite[] = {
        fixture.list,     fixture.map,    fixture.set,    fixture.integer_set, fixture.map_keys,
        fixture.node,     fixture.option, fixture.tuple,  fixture.record,      fixture.tree,
        fixture.int_pair, fixture.pairs,  fixture.forest, fixture.forest_set};
    for (unsigned i = 0; i < sizeof composite / sizeof composite[0]; i++)
        types[count++] = composite[i];
    struct mo_input_limits limits = mo_input_limits_default();
    for (unsigned t = 0; t < count; t++) {
        struct mo_inputs inputs;
        MO_ASSERT(mo_inputs_init(&inputs, &fixture.types, &types[t], 1, "M.types", 7, &limits, NULL,
                                 0) == MO_INPUT_OK);
        bool random = false;
        unsigned boundaries = 0;
        for (unsigned i = 0; i < 120; i++) {
            struct mo_cell value;
            enum mo_input_kind kind;
            enum mo_input_result result = mo_inputs_next(&inputs, heap, &value, &kind);
            if (result == MO_INPUT_LIMIT) {
                mo_inputs_skip(&inputs);
                continue;
            }
            MO_ASSERT(result == MO_INPUT_OK);
            CHECK(input_value_typed(heap, &fixture.types, types[t], value));
            if (kind == MO_INPUT_RANDOM)
                random = true;
            else {
                CHECK(!random);
                boundaries++;
                if (types[t] == fixture.option && i < 4)
                    CHECK(mo_cell_get_tag(value) == i);
                if ((types[t] == fixture.node || types[t] == fixture.tree) && i < 2)
                    CHECK(mo_cell_get_tag(value) == i);
                if (types[t] == fixture.list && i < 3) {
                    CHECK(mo_list_size(value) == (i == 2 ? 3 : i));
                    if (i == 2) {
                        static const char *const expected[] = {"", "a", "\xc3\xa9"};
                        for (uint32_t e = 0; e < 3; e++) {
                            struct mo_cell item;
                            MO_ASSERT(mo_list_get(value, e, &item));
                            size_t length;
                            const uint8_t *bytes = mo_value_data(item, &length);
                            CHECK(length == strlen(expected[e]) &&
                                  (length == 0 || memcmp(bytes, expected[e], length) == 0));
                        }
                    }
                }
                if ((types[t] == fixture.set || types[t] == fixture.map ||
                     types[t] == fixture.integer_set) &&
                    i < 3)
                    CHECK(mo_table_size(value) == (i == 2 ? 3 : i));
            }
            mo_value_drop(heap, types[t], value);
            CHECK(mo_heap_objects(heap) == 0);
        }
        CHECK(random && boundaries != 0);
        mo_inputs_free(&inputs);
    }
    mo_heap_free(heap);
    mo_heap_image_free(fixture.image);
    mo_arena_free(&fixture.arena);
}

static MO_MUST_USE enum mo_input_result text_factory(void *context, struct mo_heap *heap,
                                                     const struct mo_input_request *request,
                                                     struct mo_input_limits *remaining,
                                                     struct mo_cell *out) {
    (void)context;
    (void)request;
    if (remaining->bytes < 2 || remaining->work < 2)
        return MO_INPUT_LIMIT;
    remaining->bytes -= 2;
    remaining->work -= 2;
    return mo_value_text(heap, "zz", 2, out) ? MO_INPUT_OK : MO_INPUT_NO_MEMORY;
}

static void factory_budget(void) {
    struct heap_fixture fixture;
    heap_fixture_init(&fixture);
    struct mo_limits heap_limits = mo_limits_default();
    struct mo_heap *heap = mo_heap_new(fixture.image, &heap_limits);
    MO_ASSERT(heap != NULL);
    size_t baseline = mo_heap_used(heap);
    uint32_t text = fixture.builtin[MO_TYPE_TEXT];
    uint32_t parameters[] = {text, text};
    struct mo_input_factory factory = {
        .type = text, .boundaries = 1, .hashable = true, .build = text_factory};
    struct mo_input_limits limits = mo_input_limits_default();
    limits.bytes = 3;
    struct mo_inputs inputs;
    MO_ASSERT(mo_inputs_init(&inputs, &fixture.types, parameters, 2, "M.factory", 9, &limits,
                             &factory, 1) == MO_INPUT_OK);
    struct mo_cell out[] = {mo_cell_int(41), mo_cell_int(42)};
    enum mo_input_kind kind = MO_INPUT_RANDOM;
    CHECK(mo_inputs_next(&inputs, heap, out, &kind) == MO_INPUT_LIMIT);
    CHECK(mo_cell_get_int(out[0]) == 41 && mo_cell_get_int(out[1]) == 42);
    CHECK(kind == MO_INPUT_RANDOM && mo_heap_used(heap) == baseline);
    mo_inputs_free(&inputs);
    limits.bytes = 4;
    MO_ASSERT(mo_inputs_init(&inputs, &fixture.types, parameters, 2, "M.factory", 9, &limits,
                             &factory, 1) == MO_INPUT_OK);
    MO_ASSERT(mo_inputs_next(&inputs, heap, out, &kind) == MO_INPUT_OK);
    for (unsigned i = 0; i < 2; i++) {
        size_t length;
        const uint8_t *bytes = mo_value_data(out[i], &length);
        CHECK(length == 2 && memcmp(bytes, "zz", 2) == 0);
        mo_value_drop(heap, text, out[i]);
    }
    mo_inputs_free(&inputs);
    CHECK(mo_heap_used(heap) == baseline);
    mo_heap_free(heap);
    mo_heap_image_free(fixture.image);
    mo_arena_free(&fixture.arena);
}

static void limits_and_retry(void) {
    struct heap_fixture fixture;
    heap_fixture_init(&fixture);
    struct mo_limits heap_limits = mo_limits_default();
    struct mo_heap *heap = mo_heap_new(fixture.image, &heap_limits);
    MO_ASSERT(heap != NULL);
    struct mo_input_limits limits = mo_input_limits_default();
    limits.combinations = 2;
    struct mo_inputs inputs;
    uint32_t text = fixture.builtin[MO_TYPE_TEXT];
    MO_ASSERT(mo_inputs_init(&inputs, &fixture.types, &text, 1, "M.retry", 7, &limits, NULL, 0) ==
              MO_INPUT_OK);
    struct mo_cell out = mo_cell_int(41);
    enum mo_input_kind kind = MO_INPUT_RANDOM;
    size_t baseline = mo_heap_used(heap);
    struct mo_scratch full;
    mo_scratch_init(&full, heap);
    MO_ASSERT(mo_scratch_resize(&full, NULL, 0, full.arena.limit - MO_ARENA_CHUNK_HEADER) != NULL);
    size_t occupied = mo_heap_used(heap);
    CHECK(mo_inputs_next(&inputs, heap, &out, &kind) == MO_INPUT_NO_MEMORY);
    CHECK(mo_cell_get_int(out) == 41 && kind == MO_INPUT_RANDOM && mo_heap_used(heap) == occupied);
    mo_scratch_free(&full);
    MO_ASSERT(mo_inputs_next(&inputs, heap, &out, &kind) == MO_INPUT_OK);
    size_t length;
    const uint8_t *data = mo_value_data(out, &length);
    (void)data;
    CHECK(kind == MO_INPUT_BOUNDARY && length == 0);
    mo_value_drop(heap, text, out);
    MO_ASSERT(mo_inputs_next(&inputs, heap, &out, &kind) == MO_INPUT_OK);
    data = mo_value_data(out, &length);
    CHECK(kind == MO_INPUT_BOUNDARY && length == 1 && data[0] == 'a');
    mo_value_drop(heap, text, out);
    MO_ASSERT(mo_inputs_next(&inputs, heap, &out, &kind) == MO_INPUT_OK);
    CHECK(kind == MO_INPUT_RANDOM);
    mo_value_drop(heap, text, out);
    mo_inputs_free(&inputs);
    CHECK(mo_heap_used(heap) == baseline);
    const uint32_t unsupported[] = {fixture.function, fixture.env,
                                    fixture.capabilities[MO_CAP_FILES], fixture.no_hash_map};
    limits = mo_input_limits_default();
    for (unsigned i = 0; i < sizeof unsupported / sizeof unsupported[0]; i++) {
        CHECK(mo_inputs_init(&inputs, &fixture.types, &unsupported[i], 1, "M.no", 4, &limits, NULL,
                             0) == MO_INPUT_UNSUPPORTED);
        mo_inputs_free(&inputs);
    }
    limits.depth = 1;
    MO_ASSERT(mo_inputs_init(&inputs, &fixture.types, &fixture.option, 1, "M.shallow", 9, &limits,
                             NULL, 0) == MO_INPUT_OK);
    for (unsigned i = 0; i < 16; i++) {
        MO_ASSERT(mo_inputs_next(&inputs, heap, &out, &kind) == MO_INPUT_OK);
        CHECK(mo_cell_get_tag(out) == 0);
        CHECK(kind == (i == 0 ? MO_INPUT_BOUNDARY : MO_INPUT_RANDOM));
        mo_value_drop(heap, fixture.option, out);
    }
    mo_inputs_free(&inputs);
    limits = mo_input_limits_default();
    limits.work = 0;
    MO_ASSERT(mo_inputs_init(&inputs, &fixture.types, &text, 1, "M.work", 6, &limits, NULL, 0) ==
              MO_INPUT_OK);
    out = mo_cell_int(41);
    CHECK(mo_inputs_next(&inputs, heap, &out, &kind) == MO_INPUT_LIMIT);
    CHECK(mo_cell_get_int(out) == 41 && mo_heap_used(heap) == baseline);
    mo_inputs_free(&inputs);
    limits = mo_input_limits_default();
    limits.metadata = 0;
    CHECK(mo_inputs_init(&inputs, &fixture.types, &text, 1, "M.no", 4, &limits, NULL, 0) ==
          MO_INPUT_NO_MEMORY);
    mo_inputs_free(&inputs);
    limits = mo_input_limits_default();
    limits.depth = MO_LIMIT_INPUT_DEPTH + 1;
    CHECK(mo_inputs_init(&inputs, &fixture.types, &text, 1, "M.no", 4, &limits, NULL, 0) ==
          MO_INPUT_LIMIT);
    mo_inputs_free(&inputs);
    limits = mo_input_limits_default();
    MO_ASSERT(mo_inputs_init(&inputs, &fixture.types, NULL, 0, "M.unit", 6, &limits, NULL, 0) ==
              MO_INPUT_OK);
    CHECK(mo_inputs_next(&inputs, heap, NULL, &kind) == MO_INPUT_OK && kind == MO_INPUT_BOUNDARY);
    CHECK(mo_inputs_next(&inputs, heap, NULL, &kind) == MO_INPUT_DONE);
    mo_inputs_free(&inputs);
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

static void random_growth(void) {
    struct heap_fixture fixture;
    heap_fixture_init(&fixture);
    struct mo_limits heap_limits = mo_limits_default();
    struct mo_heap *heap = mo_heap_new(fixture.image, &heap_limits);
    MO_ASSERT(heap != NULL);
    struct mo_input_limits limits = mo_input_limits_default();
    uint32_t types[] = {fixture.map, fixture.integer_set, fixture.forest};
    for (unsigned t = 0; t < 3; t++) {
        struct mo_inputs inputs;
        MO_ASSERT(mo_inputs_init(&inputs, &fixture.types, &types[t], 1, "M.growing", 9, &limits,
                                 NULL, 0) == MO_INPUT_OK);
        unsigned refused = 0;
        uint32_t largest = 0;
        for (unsigned trial = 0; trial < 1000; trial++) {
            struct mo_cell value;
            enum mo_input_kind kind;
            enum mo_input_result result = mo_inputs_next(&inputs, heap, &value, &kind);
            if (result != MO_INPUT_OK) {
                refused++;
                mo_inputs_skip(&inputs);
                continue;
            }
            CHECK(input_value_typed(heap, &fixture.types, types[t], value));
            if (t == 1 && mo_table_size(value) > largest)
                largest = mo_table_size(value);
            mo_value_drop(heap, types[t], value);
        }
        CHECK(refused == 0);
        if (t == 1)
            CHECK(largest > 150);
        mo_inputs_free(&inputs);
    }
    CHECK(mo_heap_objects(heap) == 0);
    mo_heap_free(heap);
    mo_heap_image_free(fixture.image);
    mo_arena_free(&fixture.arena);
}

static void decimal_magnitudes(void) {
    struct heap_fixture fixture;
    heap_fixture_init(&fixture);
    struct mo_limits heap_limits = mo_limits_default();
    struct mo_heap *heap = mo_heap_new(fixture.image, &heap_limits);
    MO_ASSERT(heap != NULL);
    struct mo_input_limits limits = mo_input_limits_default();
    uint32_t decimal = fixture.builtin[MO_TYPE_DECIMAL];
    struct mo_inputs inputs;
    MO_ASSERT(mo_inputs_init(&inputs, &fixture.types, &decimal, 1, "M.decimal", 9, &limits, NULL,
                             0) == MO_INPUT_OK);
    unsigned positive = 0, negative = 0, fractional = 0, large = 0;
    const mo_i128 unit = INT64_C(1000000000000000000);
    for (unsigned trial = 0; trial < 200; trial++) {
        struct mo_cell cell;
        enum mo_input_kind kind;
        MO_ASSERT(mo_inputs_next(&inputs, heap, &cell, &kind) == MO_INPUT_OK);
        if (kind != MO_INPUT_RANDOM)
            continue;
        mo_i128 value = mo_cell_get_decimal(cell);
        if (value > unit)
            positive++;
        if (value < -unit)
            negative++;
        if (value != 0 && value % unit != 0)
            fractional++;
        if (value > unit * 1000000 || value < -unit * 1000000)
            large++;
    }
    CHECK(positive > 10 && negative > 10 && fractional > 10 && large > 10);
    mo_inputs_free(&inputs);
    mo_heap_free(heap);
    mo_heap_image_free(fixture.image);
    mo_arena_free(&fixture.arena);
}

static void range_extremes(void) {
    struct heap_fixture fixture;
    heap_fixture_init(&fixture);
    struct mo_input_limits limits = mo_input_limits_default();
    struct mo_inputs inputs;
    uint32_t type = fixture.builtin[MO_TYPE_RANGE];
    MO_ASSERT(mo_inputs_init(&inputs, &fixture.types, &type, 1, "M.range", 7, &limits, NULL, 0) ==
              MO_INPUT_OK);
    struct mo_cell value = mo_cell_range(42, INT64_MAX);
    struct mo_input_extreme extreme = {.parameter = 99};
    CHECK(mo_inputs_extreme(&inputs, &value, 0, &extreme) == MO_INPUT_OK);
    CHECK(extreme.parameter == 0 && extreme.type == MO_TYPE_INT && extreme.end == MO_INPUT_LARGEST);
    value = mo_cell_range(INT64_MIN, INT64_MAX);
    CHECK(mo_inputs_extreme(&inputs, &value, 0, &extreme) == MO_INPUT_OK);
    CHECK(extreme.end == MO_INPUT_SMALLEST);
    value = mo_cell_range(-1, 1);
    CHECK(mo_inputs_extreme(&inputs, &value, 0, &extreme) == MO_INPUT_DONE);
    mo_inputs_free(&inputs);
    mo_heap_image_free(fixture.image);
    mo_arena_free(&fixture.arena);
}

static void bigint_candidates(void) {
    struct heap_fixture fixture;
    heap_fixture_init(&fixture);
    struct mo_limits heap_limits = mo_limits_default();
    struct mo_heap *heap = mo_heap_new(fixture.image, &heap_limits);
    MO_ASSERT(heap != NULL);
    struct mo_input_limits limits = mo_input_limits_default();
    struct mo_inputs inputs;
    uint32_t type = fixture.builtin[MO_TYPE_BIGINT];
    MO_ASSERT(mo_inputs_init(&inputs, &fixture.types, &type, 1, "M.bigint", 8, &limits, NULL, 0) ==
              MO_INPUT_OK);
    for (unsigned magnitude = 1; magnitude <= 2; magnitude++) {
        uint8_t byte = (uint8_t)magnitude;
        struct mo_cell best;
        MO_ASSERT(mo_value_bigint(heap, MO_BIGINT_POSITIVE, &byte, 1, &best));
        struct mo_input_shrinker shrinker;
        mo_input_shrink_init(&shrinker, &inputs, 10);
        unsigned emitted = 0;
        for (;;) {
            struct mo_cell candidate;
            enum mo_input_result result = mo_input_shrink_next(&shrinker, &best, heap, &candidate);
            if (result == MO_INPUT_DONE)
                break;
            MO_ASSERT(result == MO_INPUT_OK);
            size_t length;
            const uint8_t *data = mo_value_data(candidate, &length);
            CHECK(emitted == 0 ? length == 0 : length == 1 && data[0] == 1);
            emitted++;
            mo_value_drop(heap, type, candidate);
            mo_input_shrink_feedback(&shrinker, false);
        }
        CHECK(emitted == magnitude);
        mo_value_drop(heap, type, best);
    }
    CHECK(mo_heap_objects(heap) == 0);
    mo_inputs_free(&inputs);
    mo_heap_free(heap);
    mo_heap_image_free(fixture.image);
    mo_arena_free(&fixture.arena);
}

static void partial_types(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, 1 << 20);
    struct mo_types types;
    mo_types_init(&types, &arena);
    uint32_t integer = heap_intern(&types, (struct mo_type){.kind = MO_TYPE_INT});
    uint32_t text = heap_intern(&types, (struct mo_type){.kind = MO_TYPE_TEXT});
    uint32_t loop, option, forest;
    MO_ASSERT(mo_types_declare(&types, MO_TYPE_RECORD, "Loop", &loop) == MO_TYPES_OK);
    MO_ASSERT(mo_types_declare(&types, MO_TYPE_CHOICE, "Option", &option) == MO_TYPES_OK);
    MO_ASSERT(mo_types_declare(&types, MO_TYPE_RECORD, "Forest", &forest) == MO_TYPES_OK);
    struct mo_type_field again = {"again", loop};
    MO_ASSERT(mo_types_define_record(&types, loop, &again, 1) == MO_TYPES_OK);
    uint32_t chain[300], previous = integer;
    for (unsigned i = 0; i < 300; i++) {
        uint32_t members[] = {previous, integer};
        chain[i] = heap_intern(
            &types,
            (struct mo_type){.kind = MO_TYPE_TUPLE, .parameters = members, .parameter_count = 2});
        previous = chain[i];
    }
    struct mo_type_field deep = {"deep", previous};
    struct mo_type_field fields[] = {{"number", integer}, {"loop", loop}};
    struct mo_type_case cases[] = {{.name = "Some", .fields = fields, .field_count = 2},
                                   {.name = "None"},
                                   {.name = "Deep", .fields = &deep, .field_count = 1}};
    MO_ASSERT(mo_types_define_choice(&types, option, cases, 3) == MO_TYPES_OK);
    uint32_t children = heap_intern(
        &types,
        (struct mo_type){.kind = MO_TYPE_LIST, .parameters = &forest, .parameter_count = 1});
    struct mo_type_field child_field = {"children", children};
    MO_ASSERT(mo_types_define_record(&types, forest, &child_field, 1) == MO_TYPES_OK);
    uint32_t parameters[] = {text, integer};
    uint32_t map = heap_intern(
        &types,
        (struct mo_type){.kind = MO_TYPE_MAP, .parameters = parameters, .parameter_count = 2});
    uint32_t empty_list = heap_intern(
        &types, (struct mo_type){.kind = MO_TYPE_LIST, .parameters = &loop, .parameter_count = 1});
    uint32_t empty_parameters[] = {integer, loop};
    uint32_t empty_map = heap_intern(&types, (struct mo_type){.kind = MO_TYPE_MAP,
                                                              .parameters = empty_parameters,
                                                              .parameter_count = 2});
    MO_ASSERT(mo_types_finish(&types) == MO_TYPES_OK);
    for (unsigned i = 0; i < 300; i++) {
        uint32_t ignored =
            heap_layout(&types, (struct mo_layout){.kind = MO_LAYOUT_TUPLE,
                                                   .type = chain[i],
                                                   .case_index = MO_NO_INDEX,
                                                   .function = MO_NO_INDEX,
                                                   .fields = types.items[chain[i]].parameters,
                                                   .field_count = 2});
        (void)ignored;
    }
    /* Dead branches need no layout. Forest is the only constructible record. */
    uint32_t layout = heap_layout(&types, (struct mo_layout){.kind = MO_LAYOUT_RECORD,
                                                             .type = forest,
                                                             .case_index = MO_NO_INDEX,
                                                             .function = MO_NO_INDEX,
                                                             .fields = &children,
                                                             .field_count = 1});
    (void)layout;
    struct mo_limits heap_limits = mo_limits_default();
    struct mo_heap_image *image = mo_heap_image_new(&types, &heap_limits);
    MO_ASSERT(image != NULL);
    struct mo_heap *heap = mo_heap_new(image, &heap_limits);
    MO_ASSERT(heap != NULL);
    struct mo_input_limits limits = mo_input_limits_default();
    const uint32_t roots[] = {option, map, forest, empty_list, empty_map};
    for (unsigned root = 0; root < 5; root++) {
        struct mo_inputs inputs;
        enum mo_input_result initialized =
            mo_inputs_init(&inputs, &types, &roots[root], 1, "M.partial", 9, &limits, NULL, 0);
        CHECK(initialized == MO_INPUT_OK);
        if (initialized != MO_INPUT_OK) {
            mo_inputs_free(&inputs);
            continue;
        }
        for (unsigned trial = 0; trial < 200; trial++) {
            struct mo_cell value;
            enum mo_input_kind kind;
            MO_ASSERT(mo_inputs_next(&inputs, heap, &value, &kind) == MO_INPUT_OK);
            CHECK(input_value_typed(heap, &types, roots[root], value));
            if (root == 0) {
                CHECK(mo_cell_get_tag(value) == 1);
                CHECK(kind == (trial == 0 ? MO_INPUT_BOUNDARY : MO_INPUT_RANDOM));
            }
            if (root >= 3) {
                CHECK(kind == (trial == 0 ? MO_INPUT_BOUNDARY : MO_INPUT_RANDOM));
                CHECK(root == 3 ? mo_list_size(value) == 0 : mo_table_size(value) == 0);
            }
            mo_value_drop(heap, roots[root], value);
        }
        mo_inputs_free(&inputs);
    }
    struct mo_inputs inputs;
    CHECK(mo_inputs_init(&inputs, &types, &loop, 1, "M.loop", 6, &limits, NULL, 0) ==
          MO_INPUT_UNINHABITED);
    mo_inputs_free(&inputs);
    limits.depth = 1;
    CHECK(mo_inputs_init(&inputs, &types, &forest, 1, "M.deep", 6, &limits, NULL, 0) ==
          MO_INPUT_LIMIT);
    mo_inputs_free(&inputs);
    CHECK(mo_heap_objects(heap) == 0);
    mo_heap_free(heap);
    mo_heap_image_free(image);
    mo_arena_free(&arena);
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
    boundary_coverage();
    scalar_boundaries();
    shrink_numbers();
    shrink_thresholds();
    shrink_record_across_heaps();
    all_types();
    factory_budget();
    limits_and_retry();
    minimal_tuple_without_heap();
    recursive_boundaries();
    random_growth();
    decimal_magnitudes();
    partial_types();
    range_extremes();
    bigint_candidates();
    return mo_test_report("promise inputs");
}
