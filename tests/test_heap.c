#define _POSIX_C_SOURCE 200809L
#include "heap_fixture.h"
#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void test_values(struct heap_fixture *f) {
    struct mo_limits limits = mo_limits_default();
    struct mo_heap *p = mo_heap_new(f->image, &limits);
    MO_ASSERT(p != NULL);
    size_t baseline = mo_heap_used(p);
    uint32_t text = f->builtin[MO_TYPE_TEXT];
    struct mo_cell value;
    CHECK(mo_value_text(p, "a\0\xc3\xa9", 4, &value));
    size_t length;
    CHECK(memcmp(mo_value_data(value, &length), "a\0\xc3\xa9", 4) == 0 && length == 4);
    struct mo_cell duplicate = mo_value_dup(p, text, value);
    mo_value_drop(p, text, value);
    CHECK(mo_cell_get_ref(duplicate)->count == 1);
    struct mo_cell unchanged = mo_cell_int(93);
    struct mo_cell out = unchanged;
    CHECK(!mo_value_text(p, "\xc0\x80", 2, &out));
    CHECK(!mo_value_text(p, "\xed\xa0\x80", 3, &out));
    CHECK(!mo_value_text(p, "\xf4\x90\x80\x80", 4, &out));
    CHECK(!mo_value_text(p, NULL, 1, &out));
    CHECK(!mo_value_bytes(p, NULL, MO_LIMIT_STRING_SIZE + 1, &out));
    CHECK(memcmp(&out, &unchanged, sizeof out) == 0);
    CHECK(mo_value_bytes(p, (const uint8_t *)"\xff\0", 2, &out));
    CHECK(mo_value_data(out, &length)[0] == 255 && length == 2);
    mo_value_drop(p, f->builtin[MO_TYPE_BYTES], out);
    CHECK(mo_value_bigint(p, MO_BIGINT_ZERO, NULL, 0, &out));
    CHECK(mo_value_bigint_sign(out) == MO_BIGINT_ZERO);
    mo_value_drop(p, f->builtin[MO_TYPE_BIGINT], out);
    const uint8_t magnitude[] = {0, 255};
    for (unsigned sign = MO_BIGINT_POSITIVE; sign <= MO_BIGINT_NEGATIVE; sign++) {
        CHECK(mo_value_bigint(p, (enum mo_bigint_sign)sign, magnitude, 2, &out));
        CHECK(mo_value_bigint_sign(out) == (enum mo_bigint_sign)sign);
        CHECK(memcmp(mo_value_data(out, &length), magnitude, 2) == 0 && length == 2);
        mo_value_drop(p, f->builtin[MO_TYPE_BIGINT], out);
    }
    out = unchanged;
    CHECK(!mo_value_bigint(p, MO_BIGINT_ZERO, magnitude, 2, &out));
    CHECK(!mo_value_bigint(p, MO_BIGINT_NEGATIVE, NULL, 0, &out));
    CHECK(!mo_value_bigint(p, MO_BIGINT_POSITIVE, magnitude, 1, &out));
    CHECK(!mo_value_bigint(p, (enum mo_bigint_sign)3, magnitude, 2, &out));
    CHECK(memcmp(&out, &unchanged, sizeof out) == 0);

    struct mo_cell list;
    CHECK(mo_value_list(p, f->list, &duplicate, 1, &list));
    struct mo_cell fields[] = {duplicate, list};
    CHECK(mo_value_construct(p, f->tuple_layout, fields, 2, &out));
    CHECK(mo_cell_get_ref(mo_value_field(out, 1)) == mo_cell_get_ref(list));
    mo_value_drop(p, f->tuple, out);
    CHECK(mo_value_closure(p, 7, fields, 2, &out));
    struct mo_cell environment = mo_value_dup(p, f->env, out);
    mo_value_drop(p, f->function, out);
    CHECK(mo_cell_get_ref(environment)->count == 1);
    mo_value_drop(p, f->env, environment);
    CHECK(!mo_value_closure(p, 7, fields, 1, &out));
    CHECK(!mo_value_construct(p, f->tuple_layout, fields, 1, &out));
    CHECK(mo_value_case(p, f->option, 0, NULL, 0, &out));
    mo_value_drop(p, f->option, out);
    struct mo_cell number = mo_cell_int(-73);
    CHECK(mo_value_case(p, f->option, 1, &number, 1, &out));
    CHECK(mo_cell_get_int(mo_cell_get_payload(out)) == -73);
    mo_value_drop(p, f->option, out);
    CHECK(mo_value_case(p, f->option, 2, &duplicate, 1, &out));
    CHECK(mo_cell_get_ref(mo_cell_get_payload(out)) == mo_cell_get_ref(duplicate));
    mo_value_drop(p, f->option, out);
    mo_value_drop(p, f->list, list);
    mo_value_drop(p, text, duplicate);
    CHECK(mo_heap_objects(p) == 0 && mo_heap_used(p) == baseline);
    /* Teardown must also release leaked and immortal allocations. */
    CHECK(mo_value_closure(p, 8, NULL, 0, &out));
    CHECK(mo_cell_get_ref(out)->count == MO_IMMORTAL_COUNT);
    struct mo_object *captureless = mo_cell_get_ref(out);
    mo_value_drop(p, f->function, out);
    CHECK(mo_value_closure(p, 8, NULL, 0, &out));
    CHECK(mo_cell_get_ref(out) == captureless);
    mo_value_drop(p, f->function, out);
    CHECK(mo_value_text(p, "saturated", 9, &value));
    mo_cell_get_ref(value)->count = MO_IMMORTAL_COUNT - 1;
    duplicate = mo_value_dup(p, text, value);
    CHECK(mo_cell_get_ref(value)->count == MO_IMMORTAL_COUNT);
    mo_value_drop(p, text, duplicate);
    mo_value_drop(p, text, value);
    CHECK(mo_heap_objects(p) == 1);
    mo_heap_free(p);
}

static void test_lists(struct heap_fixture *f) {
    struct mo_limits limits = mo_limits_default();
    struct mo_heap *p = mo_heap_new(f->image, &limits);
    MO_ASSERT(p != NULL);
    struct mo_cell a, b, list;
    CHECK(mo_value_text(p, "first", 5, &a));
    CHECK(mo_value_text(p, "second", 6, &b));
    CHECK(mo_value_list(p, f->list, NULL, 0, &list));
    for (uint32_t i = 0; i < 4096; i++) {
        CHECK(mo_list_append(p, f->list, &list, a));
    }
    CHECK(mo_list_size(list) == 4096);
    struct mo_cell shared = mo_value_dup(p, f->list, list);
    CHECK(mo_list_set(p, f->list, &list, 3, b));
    struct mo_cell got;
    CHECK(mo_list_get(shared, 3, &got) && mo_cell_get_ref(got) == mo_cell_get_ref(a));
    CHECK(mo_list_get(list, 3, &got) && mo_cell_get_ref(got) == mo_cell_get_ref(b));
    struct mo_object *unique = mo_cell_get_ref(list);
    CHECK(mo_list_set(p, f->list, &list, 4, b));
    CHECK(mo_cell_get_ref(list) == unique);
    got = mo_cell_int(7);
    CHECK(!mo_list_get(list, 4096, &got) && mo_cell_get_int(got) == 7);
    CHECK(!mo_list_set(p, f->list, &list, 4096, b));
    mo_value_drop(p, f->list, shared);
    mo_value_drop(p, f->list, list);
    mo_value_drop(p, f->builtin[MO_TYPE_TEXT], a);
    mo_value_drop(p, f->builtin[MO_TYPE_TEXT], b);
    CHECK(mo_heap_objects(p) == 0);
    mo_heap_free(p);
}

static void test_deep_drop(struct heap_fixture *f) {
    struct mo_limits limits = mo_limits_default();
    struct mo_heap *p = mo_heap_new(f->image, &limits);
    MO_ASSERT(p != NULL);
    struct mo_cell node;
    CHECK(mo_value_case(p, f->node, 0, NULL, 0, &node));
    for (uint32_t i = 0; i < 100000; i++) {
        struct mo_cell next;
        CHECK(mo_value_case(p, f->node, 1, &node, 1, &next));
        mo_value_drop(p, f->node, node);
        node = next;
    }
    CHECK(mo_heap_objects(p) == 100000);
    struct mo_cell box;
    CHECK(mo_value_construct(p, f->case_layout, &node, 1, &box));
    mo_value_drop(p, f->node, node);
    mo_value_drop(p, f->option, box);
    CHECK(mo_heap_objects(p) == 0);
    mo_heap_free(p);
}

static void test_limit(struct heap_fixture *f) {
    struct mo_limits limits = mo_limits_default();
    limits.heap_size = 4096;
    struct mo_heap *p = mo_heap_new(f->image, &limits);
    MO_ASSERT(p != NULL);
    struct mo_cell out = mo_cell_int(19);
    char bytes[4096] = {0};
    CHECK(!mo_value_text(p, bytes, sizeof bytes, &out));
    CHECK(mo_cell_get_int(out) == 19 && mo_heap_objects(p) == 0);
    struct mo_cell value, list;
    CHECK(mo_value_text(p, "value", 5, &value));
    CHECK(mo_value_list(p, f->list, NULL, 0, &list));
    while (mo_list_append(p, f->list, &list, value)) {
        CHECK(mo_heap_used(p) <= limits.heap_size);
    }
    uint32_t length = mo_list_size(list);
    struct mo_object *old = mo_cell_get_ref(list);
    CHECK(!mo_list_append(p, f->list, &list, value));
    CHECK(mo_list_size(list) == length && mo_cell_get_ref(list) == old);
    mo_value_drop(p, f->list, list);
    mo_value_drop(p, f->builtin[MO_TYPE_TEXT], value);
    CHECK(mo_heap_objects(p) == 0);
    mo_heap_free(p);
}

static void test_tables(struct heap_fixture *f) {
    struct mo_limits limits = mo_limits_default();
    struct mo_heap *p = mo_heap_new(f->image, &limits);
    MO_ASSERT(p != NULL);
    struct mo_cell keys[3], lists[3];
    const char *names[] = {"a", "b", "c"};
    for (uint32_t i = 0; i < 3; i++) {
        CHECK(mo_value_text(p, names[i], 1, &keys[i]));
        CHECK(mo_value_list(p, f->list, keys, i + 1, &lists[i]));
    }
    struct mo_cell duplicates[] = {keys[1], keys[0], keys[1], keys[2]};
    struct mo_cell values[] = {lists[0], lists[1], lists[2], lists[0]};
    struct mo_cell map, other, set;
    CHECK(mo_value_map(p, f->map, duplicates, values, 4, &map));
    CHECK(mo_table_size(map) == 3);
    bool found;
    struct mo_cell got;
    CHECK(mo_table_get(p, f->map, map, keys[1], &found, &got) && found);
    CHECK(mo_cell_get_ref(got) == mo_cell_get_ref(lists[2]));
    uint32_t pos = 0;
    struct mo_cell key;
    const uint32_t expected[] = {1, 0, 2};
    for (uint32_t i = 0; i < 3; i++) {
        CHECK(mo_table_next(map, &pos, &key, &got));
        CHECK(mo_cell_get_ref(key) == mo_cell_get_ref(keys[expected[i]]));
    }
    CHECK(!mo_table_next(map, &pos, &key, &got));
    struct mo_cell reordered_values[] = {lists[1], lists[2], lists[0]};
    CHECK(mo_value_map(p, f->map, keys, reordered_values, 3, &other));
    bool equal;
    uint64_t h1, h2;
    CHECK(mo_value_equal(p, f->map, map, other, &equal) && equal);
    CHECK(mo_value_hash(p, f->map, map, &h1));
    CHECK(mo_value_hash(p, f->map, other, &h2) && h1 == h2);
    struct mo_cell outer1, outer2;
    CHECK(mo_value_map(p, f->map_keys, &map, keys, 1, &outer1));
    CHECK(mo_value_map(p, f->map_keys, &other, keys, 1, &outer2));
    CHECK(mo_value_equal(p, f->map_keys, outer1, outer2, &equal) && equal);
    mo_value_drop(p, f->map_keys, outer1);
    mo_value_drop(p, f->map_keys, outer2);
    struct mo_cell shared = mo_value_dup(p, f->map, map);
    CHECK(mo_table_set(p, f->map, &map, keys[0], lists[0]));
    CHECK(mo_cell_get_ref(map) != mo_cell_get_ref(shared));
    CHECK(mo_table_get(p, f->map, shared, keys[0], &found, &got) && found);
    CHECK(mo_cell_get_ref(got) == mo_cell_get_ref(lists[1]));
    struct mo_object *unique = mo_cell_get_ref(map);
    CHECK(mo_table_set(p, f->map, &map, keys[0], lists[1]));
    CHECK(mo_cell_get_ref(map) == unique);
    CHECK(mo_value_equal(p, f->map, map, other, &equal) && equal);
    CHECK(mo_table_remove(p, f->map, &map, keys[1]));
    CHECK(mo_table_set(p, f->map, &map, keys[1], lists[2]));
    pos = 0;
    const uint32_t after[] = {0, 2, 1};
    for (uint32_t i = 0; i < 3; i++) {
        CHECK(mo_table_next(map, &pos, &key, &got));
        CHECK(mo_cell_get_ref(key) == mo_cell_get_ref(keys[after[i]]));
    }
    CHECK(mo_value_equal(p, f->map, map, other, &equal) && equal);
    CHECK(mo_value_hash(p, f->map, map, &h2) && h1 == h2);
    CHECK(mo_value_set(p, f->set, duplicates, 4, &set));
    CHECK(mo_table_size(set) == 3);
    struct mo_cell reversed[] = {keys[2], keys[1], keys[0]}, other_set;
    CHECK(mo_value_set(p, f->set, reversed, 3, &other_set));
    CHECK(mo_value_equal(p, f->set, set, other_set, &equal) && equal);
    CHECK(mo_value_hash(p, f->set, set, &h1));
    CHECK(mo_value_hash(p, f->set, other_set, &h2) && h1 == h2);
    struct mo_cell shared_set = mo_value_dup(p, f->set, set);
    CHECK(mo_table_remove(p, f->set, &set, keys[0]));
    CHECK(mo_table_size(set) == 2 && mo_table_size(shared_set) == 3);
    CHECK(mo_table_set(p, f->set, &set, keys[0], keys[0]));
    CHECK(mo_value_hash(p, f->set, set, &h2) && h1 == h2);
    mo_value_drop(p, f->set, other_set);
    mo_value_drop(p, f->set, shared_set);
    mo_value_drop(p, f->set, set);
    mo_value_drop(p, f->map, map);
    mo_value_drop(p, f->map, shared);
    mo_value_drop(p, f->map, other);
    for (uint32_t i = 0; i < 3; i++) {
        mo_value_drop(p, f->list, lists[i]);
        mo_value_drop(p, f->builtin[MO_TYPE_TEXT], keys[i]);
    }
    CHECK(mo_heap_objects(p) == 0);
    mo_heap_free(p);
}

static void test_generic(struct heap_fixture *f) {
    struct mo_limits limits = mo_limits_default();
    struct mo_heap *p = mo_heap_new(f->image, &limits);
    MO_ASSERT(p != NULL);
    bool equal;
    int order;
    uint64_t hash;
    struct scalar_pair {
        enum mo_type_kind kind;
        struct mo_cell a, b;
    };
    const struct scalar_pair pairs[] = {
        {MO_TYPE_BOOL, mo_cell_bool(false), mo_cell_bool(true)},
        {MO_TYPE_BYTE, mo_cell_byte(2), mo_cell_byte(17)},
        {MO_TYPE_INT, mo_cell_int(-73), mo_cell_int(14)},
        {MO_TYPE_CHAR, mo_cell_char(0x100), mo_cell_char(0x1f600)},
        {MO_TYPE_TIME, mo_cell_time(-1), mo_cell_time(0)},
        {MO_TYPE_DURATION, mo_cell_duration(-3), mo_cell_duration(2)},
        {MO_TYPE_DECIMAL, mo_cell_decimal(-8), mo_cell_decimal(3)},
    };
    for (unsigned i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
        uint32_t type = f->builtin[pairs[i].kind];
        CHECK(mo_value_equal(p, type, pairs[i].a, pairs[i].b, &equal) && !equal);
        CHECK(mo_value_compare(p, type, pairs[i].a, pairs[i].b, &order) && order == -1);
        CHECK(mo_value_compare(p, type, pairs[i].b, pairs[i].a, &order) && order == 1);
        CHECK(mo_value_hash(p, type, pairs[i].a, &hash));
    }
    uint32_t range = f->builtin[MO_TYPE_RANGE], unit = f->builtin[MO_TYPE_UNIT];
    CHECK(mo_value_equal(p, unit, mo_cell_unit(), mo_cell_unit(), &equal) && equal);
    CHECK(mo_value_hash(p, unit, mo_cell_unit(), &hash));
    CHECK(mo_value_compare(p, unit, mo_cell_unit(), mo_cell_unit(), &order) && order == 0);
    CHECK(mo_value_equal(p, range, mo_cell_range(-2, 3), mo_cell_range(-2, 4), &equal) && !equal);
    CHECK(mo_value_hash(p, range, mo_cell_range(-2, 3), &hash));
    CHECK(!mo_value_compare(p, range, mo_cell_range(-2, 3), mo_cell_range(-2, 4), &order));
    const uint8_t small[] = {255}, large[] = {0, 1};
    for (unsigned sign = MO_BIGINT_POSITIVE; sign <= MO_BIGINT_NEGATIVE; sign++) {
        struct mo_cell x, y;
        uint32_t type = f->builtin[MO_TYPE_BIGINT];
        CHECK(mo_value_bigint(p, (enum mo_bigint_sign)sign, small, 1, &x));
        CHECK(mo_value_bigint(p, (enum mo_bigint_sign)sign, large, 2, &y));
        CHECK(mo_value_equal(p, type, x, y, &equal) && !equal);
        CHECK(mo_value_compare(p, type, x, y, &order) &&
              order == (sign == MO_BIGINT_NEGATIVE ? 1 : -1));
        CHECK(mo_value_hash(p, type, x, &hash));
        mo_value_drop(p, type, x);
        mo_value_drop(p, type, y);
    }
    uint32_t floating = f->builtin[MO_TYPE_FLOAT];
    CHECK(mo_value_equal(p, floating, mo_cell_float(NAN), mo_cell_float(NAN), &equal) && !equal);
    CHECK(mo_value_compare(p, floating, mo_cell_float(NAN), mo_cell_float(1), &order) &&
          order == 1);
    CHECK(mo_value_compare(p, floating, mo_cell_float(NAN), mo_cell_float(NAN), &order) &&
          order == 0);
    CHECK(mo_value_equal(p, floating, mo_cell_float(0), mo_cell_float(-0.0), &equal) && equal);
    CHECK(!mo_value_hash(p, floating, mo_cell_float(1), &hash));
    struct mo_cell a, b;
    CHECK(mo_value_case(p, f->node, 0, NULL, 0, &a));
    b = a;
    for (uint32_t i = 0; i < 10000; i++) {
        struct mo_cell next;
        CHECK(mo_value_case(p, f->node, 1, &a, 1, &next));
        mo_value_drop(p, f->node, a);
        a = next;
        CHECK(mo_value_case(p, f->node, 1, &b, 1, &next));
        mo_value_drop(p, f->node, b);
        b = next;
    }
    CHECK(mo_value_equal(p, f->node, a, b, &equal) && equal);
    uint64_t other_hash;
    CHECK(mo_value_hash(p, f->node, a, &hash));
    CHECK(mo_value_hash(p, f->node, b, &other_hash) && hash == other_hash);
    CHECK(mo_value_compare(p, f->node, a, b, &order) && order == 0);
    struct mo_buffer printed;
    mo_buffer_init(&printed, 100000);
    CHECK(mo_value_print(p, f->node, a, &printed));
    CHECK(printed.length == 60003 && memcmp(printed.bytes, "Next(Next(", 10) == 0 &&
          printed.bytes[printed.length - 1] == ')');
    mo_buffer_free(&printed);
    mo_value_drop(p, f->node, a);
    mo_value_drop(p, f->node, b);
    CHECK(mo_heap_objects(p) == 0);
    mo_heap_free(p);
}

static void expect_text(struct mo_heap *p, uint32_t type, struct mo_cell value, const char *text) {
    struct mo_buffer out;
    mo_buffer_init(&out, MO_LIMIT_HEAP_WORK);
    CHECK(mo_value_print(p, type, value, &out));
    CHECK(out.length == strlen(text) && memcmp(out.bytes, text, out.length) == 0);
    mo_buffer_free(&out);
}

static void test_reuse_and_images(struct heap_fixture *f) {
    struct mo_limits limits = mo_limits_default();
    struct mo_heap *p = mo_heap_new(f->image, &limits);
    MO_ASSERT(p != NULL);
    struct mo_cell text, list, record, result;
    CHECK(mo_value_text(p, "x", 1, &text));
    CHECK(mo_value_list(p, f->list, &text, 1, &list));
    struct mo_cell fields[] = {text, list};
    CHECK(mo_value_construct(p, f->record_layout, fields, 2, &record));
    expect_text(p, f->record, record, "Item(label: \"x\", entries: [\"x\"])");
    struct mo_object *memory = mo_cell_get_ref(record);
    struct mo_token token = mo_value_reset(p, f->record, record);
    CHECK(token.memory == memory);
    result = mo_cell_int(71);
    CHECK(!mo_value_reuse(p, &token, fields, 1, &result));
    CHECK(token.memory == memory && mo_cell_get_int(result) == 71);
    CHECK(mo_value_reuse(p, &token, fields, 2, &record));
    CHECK(token.memory == NULL && mo_cell_get_ref(record) == memory);
    struct mo_cell shared = mo_value_dup(p, f->record, record);
    token = mo_value_reset(p, f->record, record);
    CHECK(token.memory == NULL && token.layout == f->record_layout);
    CHECK(mo_value_reuse(p, &token, fields, 2, &record));
    CHECK(mo_cell_get_ref(record) != mo_cell_get_ref(shared));
    bool equal;
    int order;
    uint64_t a, b;
    CHECK(mo_value_equal(p, f->record, record, shared, &equal) && equal);
    CHECK(mo_value_compare(p, f->record, record, shared, &order) && order == 0);
    CHECK(mo_value_hash(p, f->record, record, &a));
    CHECK(mo_value_hash(p, f->record, shared, &b) && a == b);
    token = mo_value_reset(p, f->record, record);
    mo_token_free(p, &token);
    mo_value_drop(p, f->record, shared);
    struct mo_cell end;
    CHECK(mo_value_case(p, f->node, 0, NULL, 0, &end));
    CHECK(mo_value_case(p, f->option, 3, &end, 1, &result));
    memory = mo_cell_get_ref(mo_cell_get_payload(result));
    token = mo_value_reset(p, f->option, result);
    CHECK(mo_value_reuse(p, &token, &end, 1, &result));
    CHECK(mo_cell_get_tag(result) == 3 && mo_cell_get_ref(mo_cell_get_payload(result)) == memory);
    mo_value_drop(p, f->option, result);
    struct mo_cell image;
    CHECK(mo_value_map(p, f->map, &text, &list, 1, &image));
    /* Saturation alone must not masquerade as transitive image immortality. */
    mo_cell_get_ref(image)->count = MO_IMMORTAL_COUNT - 1;
    image = mo_value_dup(p, f->map, image);
    CHECK(mo_value_make_immortal(p, f->map, image));
    CHECK(mo_cell_get_ref(text)->count == MO_IMMORTAL_COUNT &&
          mo_cell_get_ref(list)->count == MO_IMMORTAL_COUNT);
    CHECK(mo_value_hash(p, f->map, image, &a));
    struct mo_heap *q = mo_heap_new(f->image, &limits);
    MO_ASSERT(q != NULL);
    bool found;
    CHECK(mo_table_get(q, f->map, image, text, &found, &result) && found &&
          mo_list_size(result) == 1);
    struct mo_cell copy;
    CHECK(mo_value_map(q, f->map, &text, &list, 1, &copy));
    CHECK(mo_value_equal(q, f->map, image, copy, &equal) && equal);
    CHECK(mo_value_hash(q, f->map, image, &a));
    CHECK(mo_value_hash(q, f->map, copy, &b) && a == b);
    mo_value_drop(q, f->map, copy);
    copy = mo_value_dup(q, f->map, image);
    struct mo_cell empty;
    CHECK(mo_value_list(q, f->list, NULL, 0, &empty));
    CHECK(mo_table_set(q, f->map, &copy, text, empty));
    CHECK(mo_table_get(q, f->map, image, text, &found, &result) && found &&
          mo_list_size(result) == 1);
    CHECK(mo_table_get(q, f->map, copy, text, &found, &result) && found &&
          mo_list_size(result) == 0);
    mo_value_drop(q, f->list, empty);
    mo_value_drop(q, f->map, copy);
    CHECK(mo_heap_objects(q) == 0);
    mo_heap_free(q);
    mo_value_drop(p, f->map, image);
    mo_value_drop(p, f->list, list);
    mo_value_drop(p, f->builtin[MO_TYPE_TEXT], text);
    CHECK(mo_heap_objects(p) == 3);
    mo_heap_free(p);
}

static void test_adversarial(struct heap_fixture *f) {
    struct mo_limits limits = mo_limits_default();
    struct mo_heap *p = mo_heap_new(f->image, &limits);
    MO_ASSERT(p != NULL);
    struct mo_cell a, b;
    CHECK(mo_value_case(p, f->tree, 0, NULL, 0, &a));
    b = a;
    for (unsigned i = 0; i < 80; i++) {
        struct mo_cell fields[] = {a, a}, next;
        CHECK(mo_value_case(p, f->tree, 1, fields, 2, &next));
        mo_value_drop(p, f->tree, a);
        a = next;
        fields[0] = b;
        fields[1] = b;
        CHECK(mo_value_case(p, f->tree, 1, fields, 2, &next));
        mo_value_drop(p, f->tree, b);
        b = next;
    }
    bool equal;
    int order;
    uint64_t hash, other;
    CHECK(mo_value_equal(p, f->tree, a, b, &equal) && equal);
    CHECK(mo_value_compare(p, f->tree, a, b, &order) && order == 0);
    CHECK(mo_value_hash(p, f->tree, a, &hash));
    CHECK(mo_value_hash(p, f->tree, b, &other) && hash == other);
    CHECK(mo_value_make_immortal(p, f->tree, a));
    CHECK(mo_value_make_immortal(p, f->tree, a));
    mo_value_drop(p, f->tree, b);
    struct mo_cell invalid, unchanged = mo_cell_int(71);
    invalid = unchanged;
    CHECK(!mo_value_map(p, f->no_hash_map, NULL, NULL, 0, &invalid));
    CHECK(memcmp(invalid.bytes, unchanged.bytes, 16) == 0);
    CHECK(mo_value_list(p, f->no_equal_list, NULL, 0, &b));
    equal = true;
    CHECK(!mo_value_equal(p, f->no_equal_list, b, b, &equal) && equal);
    mo_value_drop(p, f->no_equal_list, b);
    struct mo_buffer output;
    mo_buffer_init(&output, 2 * MO_LIMIT_HEAP_WORK);
    CHECK(mo_buffer_append_text(&output, "kept"));
    CHECK(!mo_value_print(p, f->tree, a, &output));
    CHECK(output.length == 4 && memcmp(output.bytes, "kept", 4) == 0);
    uint8_t *magnitude = calloc(1, MO_LIMIT_BIGINT_TEXT_BYTES + 1);
    MO_ASSERT(magnitude != NULL);
    magnitude[MO_LIMIT_BIGINT_TEXT_BYTES] = 1;
    MO_ASSERT(
        mo_value_bigint(p, MO_BIGINT_POSITIVE, magnitude, MO_LIMIT_BIGINT_TEXT_BYTES + 1, &b));
    CHECK(!mo_value_print(p, f->builtin[MO_TYPE_BIGINT], b, &output));
    CHECK(output.length == 4 && memcmp(output.bytes, "kept", 4) == 0);
    mo_value_drop(p, f->builtin[MO_TYPE_BIGINT], b);
    free(magnitude);
    mo_buffer_free(&output);
    mo_heap_free(p);
}

static int released_handles;
static void release_handle(intptr_t handle) {
    CHECK(handle == 37);
    released_handles++;
}

static void close_descriptor(intptr_t handle) {
    MO_ASSERT(handle >= 0 && handle <= INT_MAX);
    CHECK(close((int)handle) == 0);
    released_handles++;
}

static void test_host_storage(struct heap_fixture *f) {
    struct mo_limits limits = mo_limits_default();
    struct mo_heap *p = mo_heap_new(f->image, &limits);
    MO_ASSERT(p != NULL);
    struct mo_cell text, list, capability;
    CHECK(mo_value_text(p, "root", 4, &text));
    CHECK(mo_value_list(p, f->list, &text, 1, &list));
    struct mo_state *state = NULL;
    CHECK(mo_state_new(p, &f->list, &list, 1, &state));
    mo_value_drop(p, f->list, list);
    struct mo_capability_info info = {.operations = &released_handles,
                                      .state = state,
                                      .prefix = text,
                                      .has_prefix = true,
                                      .read_only = true,
                                      .handle = 37,
                                      .release = release_handle};
    CHECK(!mo_state_new(p, &f->function, &text, 1, &state));
    CHECK(state != NULL);
    capability = mo_cell_int(71);
    CHECK(!mo_value_capability(p, f->list, &info, &capability));
    CHECK(mo_cell_get_int(capability) == 71);
    const char *names[] = {"<Args>",      "<Env>",   "<Console>", "<Files>",
                           "<ReadFiles>", "<Clock>", "<Random>"};
    for (unsigned i = 0; i <= MO_CAP_RANDOM; i++) {
        int before = released_handles;
        CHECK(mo_value_capability(p, f->capabilities[i], &info, &capability));
        expect_text(p, f->capabilities[i], capability, names[i]);
        const struct mo_capability_info *got = mo_capability_get(capability);
        CHECK(got->state == state && got->read_only && got->operations == info.operations);
        struct mo_cell duplicate = mo_value_dup(p, f->capabilities[i], capability);
        mo_value_drop(p, f->capabilities[i], capability);
        CHECK(released_handles == before);
        mo_value_drop(p, f->capabilities[i], duplicate);
        CHECK(released_handles == before + 1);
    }
    struct mo_state *shared = mo_state_dup(state);
    list = mo_value_dup(p, f->list, mo_state_get(state, 0));
    CHECK(mo_list_append(p, f->list, &list, text));
    CHECK(mo_list_size(mo_state_get(state, 0)) == 1);
    mo_state_set(p, shared, 0, list);
    CHECK(mo_list_size(mo_state_get(state, 0)) == 2);
    mo_value_drop(p, f->list, list);
    mo_state_drop(p, shared);
    mo_state_drop(p, state);
    mo_value_drop(p, f->builtin[MO_TYPE_TEXT], text);
    CHECK(mo_heap_objects(p) == 0);
    info.state = NULL;
    info.has_prefix = false;
    int descriptors[2];
    MO_ASSERT(pipe(descriptors) == 0);
    CHECK(close(descriptors[1]) == 0);
    info.handle = descriptors[0];
    info.release = close_descriptor;
    int before = released_handles;
    CHECK(mo_value_capability(p, f->capabilities[MO_CAP_FILES], &info, &capability));
    mo_heap_free(p);
    CHECK(released_handles == before + 1);
    errno = 0;
    CHECK(fcntl(descriptors[0], F_GETFD) == -1 && errno == EBADF);
}

static void test_print(struct heap_fixture *f) {
    struct mo_buffer small;
    mo_buffer_init(&small, 2);
    CHECK(mo_buffer_append_byte(&small, 'x'));
    CHECK(!mo_float_text(-12.5, &small));
    CHECK(small.length == 1 && small.bytes[0] == 'x');
    mo_buffer_free(&small);
    struct mo_limits limits = mo_limits_default();
    struct mo_heap *p = mo_heap_new(f->image, &limits);
    MO_ASSERT(p != NULL);
    expect_text(p, f->builtin[MO_TYPE_UNIT], mo_cell_unit(), "Unit");
    expect_text(p, f->builtin[MO_TYPE_BOOL], mo_cell_bool(false), "false");
    expect_text(p, f->builtin[MO_TYPE_BYTE], mo_cell_byte(255), "255");
    expect_text(p, f->builtin[MO_TYPE_INT], mo_cell_int(INT64_MIN), "-9223372036854775808");
    expect_text(p, f->builtin[MO_TYPE_CHAR], mo_cell_char(0x1f600), "\xf0\x9f\x98\x80");
    expect_text(p, f->builtin[MO_TYPE_TIME], mo_cell_time(-1), "1969-12-31T23:59:59.999999Z");
    expect_text(p, f->builtin[MO_TYPE_TIME], mo_cell_time(INT64_C(-62135596800000000)),
                "0001-01-01T00:00:00Z");
    expect_text(p, f->builtin[MO_TYPE_TIME], mo_cell_time(INT64_C(253402300799999999)),
                "9999-12-31T23:59:59.999999Z");
    expect_text(p, f->builtin[MO_TYPE_DURATION], mo_cell_duration(-1500000), "-1.5s");
    expect_text(p, f->builtin[MO_TYPE_DURATION], mo_cell_duration(500000), "0.5s");
    expect_text(p, f->builtin[MO_TYPE_DURATION], mo_cell_duration(INT64_MIN),
                "-9223372036854.775808s");
    expect_text(p, f->builtin[MO_TYPE_DECIMAL], mo_cell_decimal(1), "0.000000000000000001");
    expect_text(p, f->builtin[MO_TYPE_DECIMAL], mo_cell_decimal(INT64_C(-500000000000000000)),
                "-0.5");
    expect_text(p, f->builtin[MO_TYPE_DECIMAL],
                mo_cell_decimal((mo_i128)3 * INT64_C(1000000000000000000)), "3");
    expect_text(p, f->builtin[MO_TYPE_RANGE], mo_cell_range(-2, 13), "-2..13");
    const double floats[] = {0, -0.0, 1, 1e-6, 1e-7, 1e20, 1e21, 0.1, INFINITY, -INFINITY, NAN};
    const char *texts[] = {"0",    "-0",  "1",   "0.000001", "1e-7", "100000000000000000000",
                           "1e21", "0.1", "inf", "-inf",     "nan"};
    for (unsigned i = 0; i < sizeof floats / sizeof floats[0]; i++) {
        expect_text(p, f->builtin[MO_TYPE_FLOAT], mo_cell_float(floats[i]), texts[i]);
    }
    struct mo_cell text, list, tuple, map, set, value;
    CHECK(mo_value_text(p, "a\n\0#{x}", 7, &text));
    CHECK(mo_value_list(p, f->list, &text, 1, &list));
    expect_text(p, f->list, list, "[\"a\\n\\u{0}\\#{x}\"]");
    struct mo_cell fields[] = {text, list};
    CHECK(mo_value_construct(p, f->tuple_layout, fields, 2, &tuple));
    expect_text(p, f->tuple, tuple, "(\"a\\n\\u{0}\\#{x}\", [\"a\\n\\u{0}\\#{x}\"])");
    CHECK(mo_value_map(p, f->map, &text, &list, 1, &map));
    expect_text(p, f->map, map, "{\"a\\n\\u{0}\\#{x}\": [\"a\\n\\u{0}\\#{x}\"]}");
    CHECK(mo_value_set(p, f->set, &text, 1, &set));
    expect_text(p, f->set, set, "[\"a\\n\\u{0}\\#{x}\"].to_set");
    CHECK(mo_value_case(p, f->option, 2, &text, 1, &value));
    expect_text(p, f->option, value, "Text(\"a\\n\\u{0}\\#{x}\")");
    mo_value_drop(p, f->option, value);
    const uint8_t bytes[] = {0, 255, 3};
    CHECK(mo_value_bytes(p, bytes, 3, &value));
    expect_text(p, f->builtin[MO_TYPE_BYTES], value, "bytes[0, 255, 3]");
    mo_value_drop(p, f->builtin[MO_TYPE_BYTES], value);
    CHECK(mo_value_bigint(p, MO_BIGINT_NEGATIVE, bytes, 3, &value));
    expect_text(p, f->builtin[MO_TYPE_BIGINT], value, "-261888");
    mo_value_drop(p, f->builtin[MO_TYPE_BIGINT], value);
    struct mo_buffer out;
    mo_buffer_init(&out, 8);
    CHECK(mo_buffer_append_text(&out, "kept"));
    CHECK(!mo_value_print(p, f->tuple, tuple, &out));
    CHECK(out.length == 4 && memcmp(out.bytes, "kept", 4) == 0);
    mo_buffer_free(&out);
    mo_value_drop(p, f->set, set);
    mo_value_drop(p, f->map, map);
    mo_value_drop(p, f->tuple, tuple);
    mo_value_drop(p, f->list, list);
    mo_value_drop(p, f->builtin[MO_TYPE_TEXT], text);
    CHECK(mo_heap_objects(p) == 0);
    mo_heap_free(p);
}

static void test_large_values(struct heap_fixture *f) {
    struct mo_limits limits = mo_limits_default();
    struct mo_heap *p = mo_heap_new(f->image, &limits);
    MO_ASSERT(p != NULL);
    struct mo_arena inputs;
    mo_arena_init(&inputs, 16 << 20);
    struct mo_cell *items =
        mo_arena_allocate_array(&inputs, 263000, sizeof *items, _Alignof(struct mo_cell));
    MO_ASSERT(items != NULL);
    bool equal;
    int order;
    uint64_t a_hash, b_hash;
    struct mo_cell lists[2];
    for (unsigned run = 0; run < 2; run++) {
        for (uint32_t i = 0; i < 263000; i++) {
            struct mo_cell fields[] = {mo_cell_int(i), mo_cell_int(-(int64_t)i - 7)};
            MO_ASSERT(mo_value_construct_owned(p, f->int_pair_layout, fields, 2, &items[i]));
        }
        MO_ASSERT(mo_value_list(p, f->pairs, items, 263000, &lists[run]));
        for (uint32_t i = 0; i < 263000; i++) {
            mo_value_drop(p, f->int_pair, items[i]);
        }
    }
    CHECK(mo_value_equal(p, f->pairs, lists[0], lists[1], &equal) && equal);
    CHECK(mo_value_compare(p, f->pairs, lists[0], lists[1], &order) && order == 0);
    for (unsigned run = 0; run < 2; run++) {
        mo_value_drop(p, f->pairs, lists[run]);
    }
    char bytes[1000];
    memset(bytes, 'x', sizeof bytes);
    for (unsigned run = 0; run < 2; run++) {
        for (uint32_t i = 0; i < 70000; i++) {
            for (unsigned digit = 0; digit < 8; digit++) {
                bytes[digit] = (char)('a' + ((i >> (4 * digit)) & 15));
            }
            MO_ASSERT(mo_value_text(p, bytes, sizeof bytes, &items[i]));
        }
        MO_ASSERT(mo_value_list(p, f->list, items, 70000, &lists[run]));
        for (uint32_t i = 0; i < 70000; i++) {
            mo_value_drop(p, f->builtin[MO_TYPE_TEXT], items[i]);
        }
    }
    CHECK(mo_value_equal(p, f->list, lists[0], lists[1], &equal) && equal);
    CHECK(mo_value_hash(p, f->list, lists[0], &a_hash));
    CHECK(mo_value_hash(p, f->list, lists[1], &b_hash) && a_hash == b_hash);
    for (unsigned run = 0; run < 2; run++) {
        mo_value_drop(p, f->list, lists[run]);
        MO_ASSERT(mo_value_case(p, f->node, 0, NULL, 0, &lists[run]));
        for (unsigned i = 0; i < 150000; i++) {
            struct mo_cell next;
            MO_ASSERT(mo_value_case_owned(p, f->node, 1, &lists[run], 1, &next));
            lists[run] = next;
        }
    }
    CHECK(mo_value_equal(p, f->node, lists[0], lists[1], &equal) && equal);
    CHECK(mo_value_compare(p, f->node, lists[0], lists[1], &order) && order == 0);
    for (unsigned run = 0; run < 2; run++) {
        mo_value_drop(p, f->node, lists[run]);
    }
    CHECK(mo_heap_objects(p) == 0);
    mo_arena_free(&inputs);
    mo_heap_free(p);
}

static void test_sparse_tables(struct heap_fixture *f) {
    struct mo_limits limits = mo_limits_default();
    limits.heap_size = 16 << 20;
    struct mo_heap *p = mo_heap_new(f->image, &limits);
    MO_ASSERT(p != NULL);
    struct mo_cell set;
    MO_ASSERT(mo_value_set(p, f->integer_set, NULL, 0, &set));
    for (int64_t i = 0; i < 100000; i++) {
        struct mo_cell key = mo_cell_int(i);
        MO_ASSERT(mo_table_set(p, f->integer_set, &set, key, key));
    }
    for (int64_t i = 1; i < 100000; i++) {
        MO_ASSERT(mo_table_remove(p, f->integer_set, &set, mo_cell_int(i)));
    }
    CHECK(mo_table_size(set) == 1 && mo_heap_used(p) < 4096);
    size_t before = mo_heap_used(p);
    struct mo_cell copies[30];
    for (unsigned i = 0; i < 30; i++) {
        copies[i] = mo_value_dup(p, f->integer_set, set);
        struct mo_cell key = mo_cell_int(100000 + i);
        MO_ASSERT(mo_table_set(p, f->integer_set, &copies[i], key, key));
        CHECK(mo_table_size(copies[i]) == 2 && mo_table_size(set) == 1);
        uint32_t position = 0;
        struct mo_cell got, element;
        CHECK(mo_table_next(copies[i], &position, &got, &element) && mo_cell_get_int(got) == 0);
        CHECK(mo_table_next(copies[i], &position, &got, &element) &&
              mo_cell_get_int(got) == 100000 + i);
        CHECK(!mo_table_next(copies[i], &position, &got, &element));
    }
    CHECK(mo_heap_used(p) - before < 64 << 10);
    for (unsigned i = 0; i < 30; i++) {
        mo_value_drop(p, f->integer_set, copies[i]);
    }
    mo_value_drop(p, f->integer_set, set);
    CHECK(mo_heap_objects(p) == 0);
    mo_heap_free(p);
}

static void test_full_heap(struct heap_fixture *f) {
    struct mo_limits limits = mo_limits_default();
    limits.heap_size = 16 << 10;
    struct mo_heap *p = mo_heap_new(f->image, &limits);
    MO_ASSERT(p != NULL);
    struct mo_cell nodes[2], text, list, set, padding;
    for (unsigned run = 0; run < 2; run++) {
        MO_ASSERT(mo_value_case(p, f->node, 0, NULL, 0, &nodes[run]));
        for (unsigned i = 0; i < 40; i++) {
            struct mo_cell next;
            MO_ASSERT(mo_value_case_owned(p, f->node, 1, &nodes[run], 1, &next));
            nodes[run] = next;
        }
    }
    MO_ASSERT(mo_value_text(p, "x", 1, &text));
    MO_ASSERT(mo_value_list(p, f->list, &text, 1, &list));
    struct mo_cell keys[] = {mo_cell_int(1), mo_cell_int(2), mo_cell_int(3), mo_cell_int(4)};
    MO_ASSERT(mo_value_set(p, f->integer_set, keys, 4, &set));
    size_t before = mo_heap_used(p);
    MO_ASSERT(mo_value_text(p, NULL, 0, &padding));
    size_t header = mo_heap_used(p) - before;
    mo_value_drop(p, f->builtin[MO_TYPE_TEXT], padding);
    char bytes[16 << 10] = {0};
    MO_ASSERT(mo_value_text(p, bytes, limits.heap_size - before - header, &padding));
    CHECK(mo_heap_used(p) == limits.heap_size);
    expect_text(p, f->builtin[MO_TYPE_INT], mo_cell_int(-71), "-71");
    expect_text(p, f->builtin[MO_TYPE_FLOAT], mo_cell_float(-0.0), "-0");
    expect_text(p, f->builtin[MO_TYPE_TEXT], text, "x");
    expect_text(p, f->list, list, "[\"x\"]");
    bool equal = true, found = true;
    int order = 37;
    uint64_t hash = 71;
    CHECK(!mo_value_equal(p, f->node, nodes[0], nodes[1], &equal) && equal);
    CHECK(!mo_value_compare(p, f->node, nodes[0], nodes[1], &order) && order == 37);
    CHECK(!mo_value_hash(p, f->node, nodes[0], &hash) && hash == 71);
    CHECK(!mo_value_make_immortal(p, f->node, nodes[0]));
    CHECK(mo_cell_get_ref(mo_cell_get_payload(nodes[0]))->count == 1);
    struct mo_cell out = mo_cell_int(91), absent = mo_cell_int(9);
    CHECK(mo_table_get(p, f->integer_set, set, absent, &found, &out) && !found &&
          mo_cell_get_int(out) == 91);
    CHECK(mo_table_remove(p, f->integer_set, &set, absent));
    CHECK(!mo_table_set(p, f->integer_set, &set, absent, absent) && mo_table_size(set) == 4);
    struct mo_buffer buffer;
    mo_buffer_init(&buffer, 1024);
    CHECK(mo_buffer_append_text(&buffer, "kept"));
    CHECK(!mo_value_print(p, f->node, nodes[0], &buffer));
    CHECK(buffer.length == 4 && memcmp(buffer.bytes, "kept", 4) == 0);
    mo_buffer_free(&buffer);
    mo_value_drop(p, f->node, nodes[0]);
    mo_value_drop(p, f->node, nodes[1]);
    mo_value_drop(p, f->builtin[MO_TYPE_TEXT], padding);
    mo_value_drop(p, f->integer_set, set);
    mo_value_drop(p, f->list, list);
    mo_value_drop(p, f->builtin[MO_TYPE_TEXT], text);
    CHECK(mo_heap_objects(p) == 0);
    mo_heap_free(p);
}

static void test_order_and_owned(struct heap_fixture *f) {
    struct mo_limits limits = mo_limits_default();
    struct mo_heap *p = mo_heap_new(f->image, &limits);
    MO_ASSERT(p != NULL);
    struct mo_cell a, b, left, right;
    uint32_t text = f->builtin[MO_TYPE_TEXT];
    MO_ASSERT(mo_value_text(p, "\xc3\xa9", 2, &a));
    MO_ASSERT(mo_value_text(p, "\xf0\x9f\x98\x80", 4, &b));
    int order;
    CHECK(mo_value_compare(p, text, a, b, &order) && order < 0);
    struct mo_cell elements[] = {a, b};
    MO_ASSERT(mo_value_list(p, f->list, elements, 1, &left));
    MO_ASSERT(mo_value_list(p, f->list, elements, 2, &right));
    CHECK(mo_value_compare(p, f->list, left, right, &order) && order < 0);
    CHECK(mo_value_compare(p, f->list, right, left, &order) && order > 0);
    mo_value_drop(p, f->list, left);
    mo_value_drop(p, f->list, right);
    MO_ASSERT(mo_value_case(p, f->option, 1, &(struct mo_cell){0}, 1, &left));
    MO_ASSERT(mo_value_case(p, f->option, 2, &a, 1, &right));
    CHECK(mo_value_compare(p, f->option, left, right, &order) && order < 0);
    mo_value_drop(p, f->option, left);
    mo_value_drop(p, f->option, right);
    const uint8_t first[] = {0, 255}, second[] = {1};
    MO_ASSERT(mo_value_bytes(p, first, sizeof first, &left));
    MO_ASSERT(mo_value_bytes(p, second, sizeof second, &right));
    CHECK(mo_value_compare(p, f->builtin[MO_TYPE_BYTES], left, right, &order) && order < 0);
    mo_value_drop(p, f->builtin[MO_TYPE_BYTES], left);
    mo_value_drop(p, f->builtin[MO_TYPE_BYTES], right);
    mo_value_drop(p, text, a);
    mo_value_drop(p, text, b);
    for (unsigned kind = 0; kind < 3; kind++) {
        MO_ASSERT(mo_value_text(p, "owned", 5, &a));
        MO_ASSERT(mo_value_list(p, f->list, NULL, 0, &b));
        struct mo_cell fields[] = {a, b};
        uint32_t type = kind == 0 ? f->tuple : kind == 1 ? f->record : f->function;
        bool built =
            kind == 2 ? mo_value_closure_owned(p, 7, fields, 2, &left)
                      : mo_value_construct_owned(p, kind == 0 ? f->tuple_layout : f->record_layout,
                                                 fields, 2, &left);
        MO_ASSERT(built);
        CHECK(mo_cell_get_ref(a)->count == 1 && mo_cell_get_ref(b)->count == 1);
        if (kind != 2) {
            fields[0] = mo_value_dup(p, text, a);
            fields[1] = mo_value_dup(p, f->list, b);
            struct mo_token token = mo_value_reset(p, type, left);
            MO_ASSERT(mo_value_reuse_owned(p, &token, fields, 2, &left));
            CHECK(mo_cell_get_ref(a)->count == 1 && mo_cell_get_ref(b)->count == 1);
            uint64_t hash;
            CHECK(mo_value_hash(p, type, left, &hash));
            struct mo_object *original = mo_cell_get_ref(left);
            struct mo_cell retained = mo_value_dup(p, text, a), replacement;
            MO_ASSERT(mo_value_text(p, "changed", 7, &replacement));
            CHECK(mo_value_set_field_owned(p, type, left, 0, replacement, &left));
            CHECK(mo_cell_get_ref(left) == original && mo_cell_get_ref(retained)->count == 1 &&
                  mo_cell_get_ref(replacement)->count == 1);
            fields[0] = replacement;
            fields[1] = b;
            MO_ASSERT(mo_value_construct(p, kind == 0 ? f->tuple_layout : f->record_layout, fields,
                                         2, &right));
            uint64_t expected;
            CHECK(mo_value_hash(p, type, right, &expected));
            CHECK(mo_value_hash(p, type, left, &hash) && hash == expected);
            bool equal;
            CHECK(mo_value_equal(p, type, left, right, &equal) && equal);
            mo_value_drop(p, type, right);
            mo_value_drop(p, text, retained);
        }
        mo_value_drop(p, type, left);
    }
    MO_ASSERT(mo_value_text(p, "case", 4, &a));
    MO_ASSERT(mo_value_case_owned(p, f->option, 2, &a, 1, &left));
    CHECK(mo_cell_get_ref(a)->count == 1);
    mo_value_drop(p, f->option, left);
    uint32_t layout = heap_layout(&f->types, (struct mo_layout){.kind = MO_LAYOUT_CLOSURE,
                                                                .type = f->function,
                                                                .case_index = MO_NO_INDEX,
                                                                .function = 99});
    CHECK(layout != MO_NO_INDEX && mo_heap_image_refresh(f->image));
    MO_ASSERT(mo_value_closure(p, 99, NULL, 0, &left));
    CHECK(mo_heap_objects(p) == 0);
    mo_heap_free(p);
    p = mo_heap_new(f->image, &limits);
    MO_ASSERT(p != NULL && mo_value_closure(p, 99, NULL, 0, &right));
    CHECK(mo_cell_get_ref(left) == mo_cell_get_ref(right));
    CHECK(mo_heap_objects(p) == 0);
    mo_heap_free(p);
}

static MO_MUST_USE struct mo_cell forest_chain(struct heap_fixture *f, struct mo_heap *p,
                                               struct mo_cell empty, unsigned depth) {
    struct mo_cell tree;
    MO_ASSERT(mo_value_construct(p, f->forest_layout, &empty, 1, &tree));
    for (unsigned i = 0; i < depth; i++) {
        struct mo_cell set, next;
        if (i % 2 == 0) {
            set = mo_value_dup(p, f->forest_set, empty);
            MO_ASSERT(mo_table_set(p, f->forest_set, &set, tree, tree));
        } else {
            MO_ASSERT(mo_value_set(p, f->forest_set, &tree, 1, &set));
        }
        MO_ASSERT(mo_value_construct_owned(p, f->forest_layout, &set, 1, &next));
        mo_value_drop(p, f->forest, tree);
        tree = next;
    }
    return tree;
}

static void test_nested_keys(struct heap_fixture *f, bool benchmark) {
    struct mo_limits limits = mo_limits_default();
    struct mo_heap *p = mo_heap_new(f->image, &limits), *q = mo_heap_new(f->image, &limits);
    MO_ASSERT(p != NULL && q != NULL);
    struct mo_heap *image = mo_heap_image_constants(f->image);
    struct mo_cell empty;
    MO_ASSERT(mo_value_set(image, f->forest_set, NULL, 0, &empty));
    MO_ASSERT(mo_value_make_immortal(image, f->forest_set, empty));
    for (unsigned depth = 2000; depth <= (benchmark ? 16000U : 2000U); depth *= 2) {
        struct mo_cell a = forest_chain(f, p, empty, depth), b = forest_chain(f, q, empty, depth);
        struct timespec start, end;
        MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
        bool equal;
        CHECK(mo_value_equal(p, f->forest, a, b, &equal) && equal);
        MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
        uint64_t h1, h2;
        CHECK(mo_value_hash(p, f->forest, a, &h1));
        CHECK(mo_value_hash(q, f->forest, b, &h2) && h1 == h2);
        struct mo_cell set;
        MO_ASSERT(mo_value_set(p, f->forest_set, &a, 1, &set));
        bool found;
        struct mo_cell got;
        CHECK(mo_table_get(p, f->forest_set, set, b, &found, &got) && found);
        if (benchmark) {
            double seconds =
                (double)(end.tv_sec - start.tv_sec) + (double)(end.tv_nsec - start.tv_nsec) / 1e9;
            CHECK(printf("nested keys depth %u: equality %.6f s\n", depth, seconds) > 0);
        }
        mo_value_drop(p, f->forest_set, set);
        mo_value_drop(p, f->forest, a);
        mo_value_drop(q, f->forest, b);
    }
    CHECK(mo_heap_objects(p) == 0 && mo_heap_objects(q) == 0);
    mo_heap_free(p);
    mo_heap_free(q);
}

static MO_MUST_USE int benchmark_heap(struct heap_fixture *f) {
    struct mo_limits limits = mo_limits_default();
    struct mo_heap *p = mo_heap_new(f->image, &limits);
    MO_ASSERT(p != NULL);
    struct mo_cell *pairs = malloc(1000000 * sizeof *pairs);
    MO_ASSERT(pairs != NULL);
    size_t before = mo_heap_used(p);
    struct timespec start, end;
    MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    struct mo_cell fields[] = {mo_cell_int(17), mo_cell_int(-9)};
    for (unsigned i = 0; i < 1000000; i++) {
        MO_ASSERT(mo_value_construct_owned(p, f->int_pair_layout, fields, 2, &pairs[i]));
    }
    MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    double seconds =
        (double)(end.tv_sec - start.tv_sec) + (double)(end.tv_nsec - start.tv_nsec) / 1e9;
    CHECK(printf("1000000 (Int, Int): %zu charged bytes, %.6f s allocation\n",
                 mo_heap_used(p) - before, seconds) > 0);
    for (unsigned i = 0; i < 1000000; i++) {
        mo_value_drop(p, f->int_pair, pairs[i]);
    }
    free(pairs);
    mo_heap_free(p);
    MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    for (unsigned i = 0; i < 100000; i++) {
        p = mo_heap_new(f->image, &limits);
        MO_ASSERT(p != NULL);
        mo_heap_free(p);
    }
    MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    seconds = (double)(end.tv_sec - start.tv_sec) + (double)(end.tv_nsec - start.tv_nsec) / 1e9;
    CHECK(printf("100000 prepared-image heap creations: %.6f s\n", seconds) > 0);
    test_nested_keys(f, true);
    return mo_test_report("heap benchmark");
}

static MO_MUST_USE int number_driver(struct heap_fixture *f, bool bigint) {
    struct mo_limits limits = mo_limits_default();
    struct mo_heap *p = mo_heap_new(f->image, &limits);
    MO_ASSERT(p != NULL);
    struct mo_buffer out;
    mo_buffer_init(&out, MO_LIMIT_HEAP_WORK);
    char line[2 * MO_LIMIT_BIGINT_TEXT_BYTES + 2];
    uint8_t magnitude[MO_LIMIT_BIGINT_TEXT_BYTES];
    while (fgets(line, sizeof line, stdin) != NULL) {
        struct mo_cell value;
        uint32_t type = f->builtin[bigint ? MO_TYPE_BIGINT : MO_TYPE_FLOAT];
        if (bigint) {
            size_t length = strcspn(line, "\n");
            if (length == 0 || length % 2 != 0) {
                goto failed;
            }
            for (size_t i = 0; i < length / 2; i++) {
                char pair[] = {line[2 * i], line[2 * i + 1], '\0'};
                char *end;
                errno = 0;
                unsigned long byte = strtoul(pair, &end, 16);
                if (errno != 0 || *end != '\0' || byte > 255) {
                    goto failed;
                }
                magnitude[i] = (uint8_t)byte;
            }
            if (!mo_value_bigint(p, MO_BIGINT_POSITIVE, magnitude, length / 2, &value)) {
                goto failed;
            }
        } else {
            char *end;
            errno = 0;
            uint64_t bits = (uint64_t)strtoull(line, &end, 10);
            if (errno != 0 || (*end != '\n' && *end != '\0')) {
                goto failed;
            }
            double number;
            memcpy(&number, &bits, sizeof number);
            value = mo_cell_float(number);
        }
        mo_buffer_clear(&out);
        if (!mo_value_print(p, type, value, &out) ||
            fwrite(out.bytes, 1, out.length, stdout) != out.length || putchar('\n') == EOF) {
            goto failed;
        }
        mo_value_drop(p, type, value);
    }
    bool ok = !ferror(stdin) && fflush(stdout) == 0;
    mo_buffer_free(&out);
    mo_heap_free(p);
    return ok ? 0 : 1;
failed:
    mo_buffer_free(&out);
    mo_heap_free(p);
    return 1;
}

int main(int argc, char **argv) {
    struct heap_fixture fixture;
    heap_fixture_init(&fixture);
    if (argc == 2) {
        int status = strcmp(argv[1], "--bench") == 0
                         ? benchmark_heap(&fixture)
                         : number_driver(&fixture, strcmp(argv[1], "--bigint") == 0);
        mo_heap_image_free(fixture.image);
        mo_arena_free(&fixture.arena);
        return status;
    }
    test_values(&fixture);
    test_lists(&fixture);
    test_deep_drop(&fixture);
    test_limit(&fixture);
    test_tables(&fixture);
    test_generic(&fixture);
    test_print(&fixture);
    test_reuse_and_images(&fixture);
    test_adversarial(&fixture);
    test_host_storage(&fixture);
    test_large_values(&fixture);
    test_sparse_tables(&fixture);
    test_full_heap(&fixture);
    test_order_and_owned(&fixture);
    test_nested_keys(&fixture, false);
    mo_heap_image_free(fixture.image);
    mo_arena_free(&fixture.arena);
    return mo_test_report("heap");
}
