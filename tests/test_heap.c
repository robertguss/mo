#include "heap_fixture.h"
#include "test.h"

#include <math.h>
#include <string.h>

static void test_values(struct heap_fixture *f) {
    struct mo_limits limits = mo_limits_default();
    struct mo_process *p = mo_heap_new(&f->types, &limits);
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
    mo_value_drop(p, f->builtin[MO_TYPE_BIGINT], out);
    const uint8_t magnitude[] = {0, 255};
    for (unsigned sign = MO_BIGINT_POSITIVE; sign <= MO_BIGINT_NEGATIVE; sign++) {
        CHECK(mo_value_bigint(p, (enum mo_bigint_sign)sign, magnitude, 2, &out));
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
    mo_value_drop(p, f->function, out);
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
    mo_value_drop(p, f->function, out);
    CHECK(mo_value_text(p, "saturated", 9, &value));
    mo_cell_get_ref(value)->count = MO_IMMORTAL_COUNT - 1;
    duplicate = mo_value_dup(p, text, value);
    CHECK(mo_cell_get_ref(value)->count == MO_IMMORTAL_COUNT);
    mo_value_drop(p, text, duplicate);
    mo_value_drop(p, text, value);
    CHECK(mo_heap_objects(p) == 2);
    mo_heap_free(p);
}

static void test_lists(struct heap_fixture *f) {
    struct mo_limits limits = mo_limits_default();
    struct mo_process *p = mo_heap_new(&f->types, &limits);
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
    struct mo_process *p = mo_heap_new(&f->types, &limits);
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
    struct mo_process *p = mo_heap_new(&f->types, &limits);
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
    struct mo_process *p = mo_heap_new(&f->types, &limits);
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
    CHECK(mo_table_remove(p, f->set, &set, keys[0]));
    CHECK(mo_table_set(p, f->set, &set, keys[0], keys[0]));
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
    struct mo_process *p = mo_heap_new(&f->types, &limits);
    MO_ASSERT(p != NULL);
    bool equal;
    int order;
    uint64_t hash;
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
    mo_value_drop(p, f->node, a);
    mo_value_drop(p, f->node, b);
    CHECK(mo_heap_objects(p) == 0);
    mo_heap_free(p);
}

int main(void) {
    struct heap_fixture fixture;
    heap_fixture_init(&fixture);
    test_values(&fixture);
    test_lists(&fixture);
    test_deep_drop(&fixture);
    test_limit(&fixture);
    test_tables(&fixture);
    test_generic(&fixture);
    mo_arena_free(&fixture.arena);
    return mo_test_report("heap");
}
