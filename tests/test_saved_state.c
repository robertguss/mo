#include "heap_fixture.h"
#include "test.h"

#include <string.h>

/* tag is the value's first schema byte from the saved-state.md tag table. */
static void roundtrip(struct mo_heap *heap, uint32_t type, struct mo_cell original, uint8_t tag) {
    struct mo_buffer bytes;
    mo_buffer_init(&bytes, MO_LIMIT_SAVED_BYTES);
    MO_ASSERT(mo_value_encode(heap, type, original, &bytes));
    CHECK(bytes.length > 9 && bytes.bytes[9] == tag);
    size_t used = mo_heap_used(heap);
    struct mo_cell value;
    bool decoded = mo_value_decode(heap, type, bytes.bytes, bytes.length, &value);
    if (!decoded)
        (void)fprintf(stderr, "saved roundtrip failed for type %u\n", type);
    MO_ASSERT(decoded);
    bool equal = false;
    CHECK(mo_value_equal(heap, type, original, value, &equal) && equal);
    struct mo_buffer again;
    mo_buffer_init(&again, MO_LIMIT_SAVED_BYTES);
    CHECK(mo_value_encode(heap, type, value, &again));
    CHECK(bytes.length == again.length && memcmp(bytes.bytes, again.bytes, bytes.length) == 0);
    mo_value_drop(heap, type, value);
    mo_buffer_free(&again);
    for (size_t n = 0; n < bytes.length; n++) {
        value = mo_cell_int(73);
        CHECK(!mo_value_decode(heap, type, bytes.bytes, n, &value));
        CHECK(mo_cell_get_int(value) == 73 && mo_heap_used(heap) == used);
    }
    MO_ASSERT(mo_buffer_append_byte(&bytes, 0));
    CHECK(!mo_value_decode(heap, type, bytes.bytes, bytes.length, &value));
    CHECK(mo_heap_used(heap) == used);
    mo_buffer_free(&bytes);
}

static void malformed_scalar(struct heap_fixture *f, struct mo_heap *heap, enum mo_type_kind kind,
                             struct mo_cell valid, const uint8_t *bad, size_t count) {
    struct mo_buffer bytes;
    mo_buffer_init(&bytes, MO_LIMIT_SAVED_BYTES);
    MO_ASSERT(mo_value_encode(heap, f->builtin[kind], valid, &bytes));
    memcpy(bytes.bytes + bytes.length - count, bad, count);
    struct mo_cell value = mo_cell_int(17);
    CHECK(!mo_value_decode(heap, f->builtin[kind], bytes.bytes, bytes.length, &value));
    CHECK(mo_cell_get_int(value) == 17);
    mo_buffer_free(&bytes);
}

static void values(struct heap_fixture *f, struct mo_heap *heap) {
    struct mo_cell scalars[] = {mo_cell_unit(),
                                mo_cell_bool(true),
                                mo_cell_byte(213),
                                mo_cell_int(INT64_MIN),
                                mo_cell_float(-1.375),
                                mo_cell_char(0x1f980),
                                mo_cell_time(MO_TIME_MAX_US),
                                mo_cell_duration(-1234567),
                                mo_cell_decimal(-((mo_i128)1 << 100)),
                                mo_cell_range(-17, 9)};
    for (unsigned i = 0; i < sizeof scalars / sizeof scalars[0]; i++)
        roundtrip(heap, f->builtin[i], scalars[i], (uint8_t)i);
    struct mo_cell a, b, list, map, set, record;
    MO_ASSERT(mo_value_text(heap, "z\0\xc3\xa9", 4, &a));
    MO_ASSERT(mo_value_text(heap, "alpha", 5, &b));
    roundtrip(heap, f->builtin[MO_TYPE_TEXT], a, 10);
    struct mo_cell elements[] = {a, b, a};
    MO_ASSERT(mo_value_list(heap, f->list, elements, 3, &list));
    roundtrip(heap, f->list, list, 14);
    struct mo_cell keys[] = {b, a}, lists[] = {list, list};
    MO_ASSERT(mo_value_map(heap, f->map, keys, lists, 2, &map));
    CHECK(mo_table_remove(heap, f->map, &map, b));
    CHECK(mo_table_set(heap, f->map, &map, b, list));
    roundtrip(heap, f->map, map, 16);
    MO_ASSERT(mo_value_set(heap, f->set, keys, 2, &set));
    roundtrip(heap, f->set, set, 15);
    struct mo_cell fields[] = {a, list};
    MO_ASSERT(mo_value_construct(heap, f->record_layout, fields, 2, &record));
    roundtrip(heap, f->record, record, 17);
    mo_value_drop(heap, f->record, record);
    MO_ASSERT(mo_value_construct(heap, f->tuple_layout, fields, 2, &record));
    roundtrip(heap, f->tuple, record, 13);
    mo_value_drop(heap, f->tuple, record);
    struct mo_cell node;
    MO_ASSERT(mo_value_case(heap, f->node, 0, NULL, 0, &node));
    for (unsigned i = 1; i < MO_LIMIT_SAVED_DEPTH; i++) {
        struct mo_cell next;
        MO_ASSERT(mo_value_case(heap, f->node, 1, &node, 1, &next));
        mo_value_drop(heap, f->node, node);
        node = next;
    }
    roundtrip(heap, f->node, node, 18);
    struct mo_cell next;
    MO_ASSERT(mo_value_case(heap, f->node, 1, &node, 1, &next));
    struct mo_buffer bytes;
    mo_buffer_init(&bytes, MO_LIMIT_SAVED_BYTES);
    CHECK(!mo_value_encode(heap, f->node, next, &bytes) && bytes.length == 0);
    mo_value_drop(heap, f->node, next);
    mo_value_drop(heap, f->node, node);
    MO_ASSERT(mo_value_list(heap, f->no_equal_list, NULL, 0, &node));
    CHECK(!mo_value_encode(heap, f->no_equal_list, node, &bytes));
    mo_value_drop(heap, f->no_equal_list, node);
    mo_buffer_free(&bytes);
    uint8_t magnitude[] = {0, 2, 7};
    MO_ASSERT(mo_value_bigint(heap, MO_BIGINT_NEGATIVE, magnitude, 3, &node));
    roundtrip(heap, f->builtin[MO_TYPE_BIGINT], node, 12);
    mo_value_drop(heap, f->builtin[MO_TYPE_BIGINT], node);
    MO_ASSERT(mo_value_bytes(heap, magnitude, 3, &node));
    roundtrip(heap, f->builtin[MO_TYPE_BYTES], node, 11);
    mo_value_drop(heap, f->builtin[MO_TYPE_BYTES], node);
    mo_value_drop(heap, f->map, map);
    mo_value_drop(heap, f->set, set);
    mo_value_drop(heap, f->list, list);
    mo_value_drop(heap, f->builtin[MO_TYPE_TEXT], a);
    mo_value_drop(heap, f->builtin[MO_TYPE_TEXT], b);
    const uint8_t bad_bool[] = {2}, bad_char[] = {0, 0xd8, 0, 0};
    const uint8_t bad_time[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f};
    uint8_t bad_decimal[16] = {0};
    bad_decimal[15] = 0x80;
    malformed_scalar(f, heap, MO_TYPE_BOOL, mo_cell_bool(false), bad_bool, sizeof bad_bool);
    malformed_scalar(f, heap, MO_TYPE_CHAR, mo_cell_char(65), bad_char, sizeof bad_char);
    malformed_scalar(f, heap, MO_TYPE_TIME, mo_cell_time(0), bad_time, sizeof bad_time);
    malformed_scalar(f, heap, MO_TYPE_DECIMAL, mo_cell_decimal(0), bad_decimal, sizeof bad_decimal);
}

int main(void) {
    struct heap_fixture f;
    heap_fixture_init(&f);
    struct mo_limits limits = mo_limits_default();
    struct mo_heap *heap = mo_heap_new(f.image, &limits);
    MO_ASSERT(heap != NULL);
    struct mo_buffer bytes;
    mo_buffer_init(&bytes, MO_LIMIT_SAVED_BYTES);
    uint32_t type = f.builtin[MO_TYPE_INT];
    CHECK(mo_value_encode(heap, type, mo_cell_int(-913), &bytes));
    const uint8_t expected[] = {'M', 'O', 'S',  'V',  1,    13,   0,    0,    0,    3,
                                0,   0,   0,    0,    1,    0,    0,    0,    0,    0,
                                0,   0,   0x6f, 0xfc, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    CHECK(bytes.length == sizeof expected && memcmp(bytes.bytes, expected, sizeof expected) == 0);
    struct mo_cell value = mo_cell_int(77);
    CHECK(mo_value_decode(heap, type, bytes.bytes, bytes.length, &value));
    CHECK(mo_cell_get_int(value) == -913);
    for (size_t n = 0; n < bytes.length; n++) {
        value = mo_cell_int(77);
        CHECK(!mo_value_decode(heap, type, bytes.bytes, n, &value));
        CHECK(mo_cell_get_int(value) == 77);
    }
    CHECK(!mo_value_decode(heap, f.builtin[MO_TYPE_TIME], bytes.bytes, bytes.length, &value));
    mo_buffer_free(&bytes);
    size_t used = mo_heap_used(heap);
    values(&f, heap);
    CHECK(mo_heap_used(heap) == used);
    mo_heap_free(heap);
    mo_heap_image_free(f.image);
    mo_arena_free(&f.arena);
    return mo_test_report("saved state");
}
