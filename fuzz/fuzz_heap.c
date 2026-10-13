#include "../tests/heap_fixture.h"

#include <math.h>
#include <string.h>

static const int operations = 1;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    struct heap_fixture fixture;
    heap_fixture_init(&fixture);
    struct mo_limits limits = mo_limits_default();
    limits.heap_size = 64 << 10;
    struct mo_heap *p = mo_heap_new(fixture.image, &limits),
                   *q = mo_heap_new(fixture.image, &limits);
    MO_ASSERT(p != NULL && q != NULL);
    size_t baseline = mo_heap_used(p);
    struct mo_cell values[32] = {0};
    uint32_t types[32];
    for (uint32_t i = 0; i < 32; i++) {
        types[i] = fixture.builtin[MO_TYPE_UNIT];
    }
    /* Start with distinct equal and unequal compound values. Even a short
     * input reaches the walk rather than only the identical-cell fast path. */
    for (uint32_t i = 0; i < 3; i++) {
        types[i] = fixture.builtin[MO_TYPE_TEXT];
        MO_ASSERT(mo_value_text(p, i == 2 ? "b" : "a", 1, &values[i]));
        types[i + 3] = fixture.list;
        MO_ASSERT(mo_value_list(p, fixture.list, &values[i], 1, &values[i + 3]));
        types[i + 6] = fixture.map;
        MO_ASSERT(mo_value_map(p, fixture.map, &values[i], &values[i + 3], 1, &values[i + 6]));
        types[i + 9] = fixture.set;
        MO_ASSERT(mo_value_set(p, fixture.set, &values[i], 1, &values[i + 9]));
        types[i + 12] = fixture.map_keys;
        MO_ASSERT(
            mo_value_map(p, fixture.map_keys, &values[i + 6], &values[i], 1, &values[i + 12]));
    }
    for (uint32_t i = 3; i <= 12; i += 3) {
        bool equal;
        uint64_t a, b;
        MO_ASSERT(mo_value_equal(p, types[i], values[i], values[i + 1], &equal) && equal);
        MO_ASSERT(mo_value_hash(p, types[i], values[i], &a));
        MO_ASSERT(mo_value_hash(q, types[i], values[i + 1], &b) && a == b);
        MO_ASSERT(mo_value_equal(p, types[i], values[i], values[i + 2], &equal) && !equal);
    }
    size_t steps = size < 4096 ? size : 4096;
    for (size_t i = 0; i + 3 < steps; i += 4) {
        uint32_t dst = data[i + 1] % 32, src = data[i + 2] % 32;
        uint32_t other = data[i + 3] % 32;
        struct mo_cell result = mo_cell_int(31);
        struct mo_cell unchanged = result;
        uint32_t type = types[src];
        bool built = false;
        switch (data[i] % 25) {
        case 0:
            type = fixture.builtin[MO_TYPE_TEXT];
            built = mo_value_text(p, (const char *)data + i + 2, 2, &result);
            break;
        case 1:
            result = mo_value_dup(p, type, values[src]);
            built = true;
            break;
        case 2:
            mo_value_drop(p, types[dst], values[dst]);
            values[dst] = mo_cell_unit();
            types[dst] = fixture.builtin[MO_TYPE_UNIT];
            break;
        case 3:
            type = fixture.list;
            built = mo_value_list(p, type,
                                  types[src] == fixture.builtin[MO_TYPE_TEXT] ? &values[src] : NULL,
                                  types[src] == fixture.builtin[MO_TYPE_TEXT] ? 1 : 0, &result);
            break;
        case 4:
            if (types[src] == fixture.list && types[other] == fixture.builtin[MO_TYPE_TEXT]) {
                bool ok = mo_list_append(p, fixture.list, &values[src], values[other]);
                (void)ok;
            }
            break;
        case 5:
            if (types[src] == fixture.list && types[other] == fixture.builtin[MO_TYPE_TEXT]) {
                bool ok =
                    mo_list_set(p, fixture.list, &values[src], data[i + 1] % 8, values[other]);
                (void)ok;
            }
            break;
        case 6:
            type = fixture.node;
            built = mo_value_case(p, type, types[src] == type ? 1 : 0,
                                  types[src] == type ? &values[src] : NULL,
                                  types[src] == type ? 1 : 0, &result);
            break;
        case 7:
            type = data[i + 1] % 2 == 0 ? fixture.map : fixture.set;
            built = type == fixture.map ? mo_value_map(p, type, NULL, NULL, 0, &result)
                                        : mo_value_set(p, type, NULL, 0, &result);
            break;
        case 8:
            if (types[src] == fixture.map && types[dst] == fixture.builtin[MO_TYPE_TEXT] &&
                types[other] == fixture.list) {
                bool ok = mo_table_set(p, fixture.map, &values[src], values[dst], values[other]);
                (void)ok;
            } else if (types[src] == fixture.set && types[other] == fixture.builtin[MO_TYPE_TEXT]) {
                bool ok = mo_table_set(p, fixture.set, &values[src], values[other], values[other]);
                (void)ok;
            }
            break;
        case 9:
            if ((types[src] == fixture.map || types[src] == fixture.set) &&
                types[other] == fixture.builtin[MO_TYPE_TEXT]) {
                bool ok = mo_table_remove(p, types[src], &values[src], values[other]);
                (void)ok;
            }
            break;
        case 10:
            type = fixture.builtin[MO_TYPE_BYTES];
            built = mo_value_bytes(p, data + i + 2, 2, &result);
            break;
        case 11:
            type = fixture.builtin[MO_TYPE_BIGINT];
            built =
                mo_value_bigint(p, data[i + 1] % 2 == 0 ? MO_BIGINT_POSITIVE : MO_BIGINT_NEGATIVE,
                                data + i + 2, 2, &result);
            break;
        case 12:
        case 13:
        case 14:
            if (types[src] == fixture.builtin[MO_TYPE_TEXT] && types[other] == fixture.list) {
                struct mo_cell fields[] = {values[src], values[other]};
                bool closure = data[i] % 25 == 13;
                bool record = data[i] % 25 == 14;
                type = closure ? fixture.function : record ? fixture.record : fixture.tuple;
                built = closure ? mo_value_closure(p, 7, fields, 2, &result)
                                : mo_value_construct(
                                      p, record ? fixture.record_layout : fixture.tuple_layout,
                                      fields, 2, &result);
            }
            break;
        case 15:
            if (types[src] == fixture.tuple || types[src] == fixture.record) {
                struct mo_cell fields[] = {
                    mo_value_dup(p, fixture.builtin[MO_TYPE_TEXT], mo_value_field(values[src], 0)),
                    mo_value_dup(p, fixture.list, mo_value_field(values[src], 1))};
                struct mo_token token = mo_value_reset(p, type, values[src]);
                values[src] = mo_cell_unit();
                types[src] = fixture.builtin[MO_TYPE_UNIT];
                built = mo_value_reuse(p, &token, fields, 2, &result);
                mo_token_free(p, &token);
                mo_value_drop(p, fixture.builtin[MO_TYPE_TEXT], fields[0]);
                mo_value_drop(p, fixture.list, fields[1]);
            }
            break;
        case 16:
            if (type == fixture.builtin[MO_TYPE_TEXT]) {
                struct mo_state *state;
                if (mo_state_new(p, &type, &values[src], 1, &state)) {
                    struct mo_capability_info info = {.operations = &operations,
                                                      .state = state,
                                                      .prefix = values[src],
                                                      .has_prefix = true};
                    type = fixture.capabilities[data[i + 1] % 7];
                    built = mo_value_capability(p, type, &info, &result);
                    mo_state_drop(p, state);
                }
            }
            break;
        case 17:
            if (fixture.types.items[type].kind == MO_TYPE_CAPABILITY &&
                types[other] == fixture.builtin[MO_TYPE_TEXT]) {
                mo_state_set(p, mo_capability_get(values[src])->state, 0, values[other]);
            }
            break;
        case 18: {
            bool equal = true;
            if (type == types[other] &&
                mo_value_equal(p, type, values[src], values[other], &equal)) {
                bool reverse;
                MO_ASSERT(mo_value_equal(p, type, values[other], values[src], &reverse) &&
                          reverse == equal);
                uint64_t a, b;
                if (equal && mo_value_hash(p, type, values[src], &a)) {
                    MO_ASSERT(mo_value_hash(q, type, values[other], &b) && a == b);
                }
                int order;
                if (mo_value_compare(p, type, values[src], values[other], &order)) {
                    int reversed;
                    MO_ASSERT(mo_value_compare(p, type, values[other], values[src], &reversed) &&
                              order == -reversed);
                }
            }
            uint64_t hash = 37;
            if (mo_value_hash(p, type, values[src], &hash)) {
                uint64_t again;
                MO_ASSERT(mo_value_hash(p, type, values[src], &again) && hash == again);
            } else {
                MO_ASSERT(hash == 37);
            }
            struct mo_buffer out;
            mo_buffer_init(&out, 512);
            MO_ASSERT(mo_buffer_append_text(&out, "kept"));
            if (!mo_value_print(p, type, values[src], &out)) {
                MO_ASSERT(out.length == 4 && memcmp(out.bytes, "kept", 4) == 0);
            }
            mo_buffer_free(&out);
            break;
        }
        case 19: {
            uint64_t bits = 0;
            size_t length = steps - i < 8 ? steps - i : 8;
            memcpy(&bits, data + i, length);
            double number;
            memcpy(&number, &bits, sizeof number);
            type = fixture.builtin[MO_TYPE_FLOAT];
            result = mo_cell_float(number);
            built = true;
            break;
        }
        case 20:
            type = fixture.tree;
            if (types[src] == type && types[other] == type) {
                struct mo_cell fields[] = {values[src], values[other]};
                built = mo_value_case(p, type, 1, fields, 2, &result);
            } else {
                built = mo_value_case(p, type, 0, NULL, 0, &result);
            }
            break;
        case 21:
            type = fixture.map_keys;
            if (types[src] == fixture.map && types[other] == fixture.builtin[MO_TYPE_TEXT]) {
                built = mo_value_map(p, type, &values[src], &values[other], 1, &result);
            }
            break;
        case 22: {
            enum mo_type_kind kind = fixture.types.items[type].kind;
            if (kind == MO_TYPE_MAP || kind == MO_TYPE_SET) {
                uint32_t position = 0, count = 0;
                struct mo_cell key, element;
                while (mo_table_next(values[src], &position, &key, &element)) {
                    bool found;
                    struct mo_cell got;
                    if (mo_table_get(p, type, values[src], key, &found, &got)) {
                        MO_ASSERT(found && memcmp(got.bytes, element.bytes, sizeof got.bytes) == 0);
                    }
                    count++;
                }
                MO_ASSERT(count == mo_table_size(values[src]));
                if (types[other] == fixture.types.items[type].parameters[0]) {
                    bool found;
                    struct mo_cell got;
                    bool ok = mo_table_get(p, type, values[src], values[other], &found, &got);
                    (void)ok;
                    if (kind == MO_TYPE_MAP &&
                        types[dst] == fixture.types.items[type].parameters[1]) {
                        ok = mo_table_set(p, type, &values[src], values[other], values[dst]);
                        (void)ok;
                    }
                }
            }
            break;
        }
        case 23: {
            struct mo_heap *image = mo_heap_image_constants(fixture.image);
            struct mo_cell text, list, map, copy;
            char byte = (char)('a' + data[i + 1] % 26);
            MO_ASSERT(mo_value_text(image, &byte, 1, &text));
            MO_ASSERT(mo_value_list(image, fixture.list, &text, 1, &list));
            MO_ASSERT(mo_value_map(image, fixture.map, &text, &list, 1, &map));
            MO_ASSERT(mo_value_make_immortal(image, fixture.map, map));
            uint64_t a, b;
            MO_ASSERT(mo_value_hash(p, fixture.map, map, &a));
            MO_ASSERT(mo_value_hash(q, fixture.map, map, &b) && a == b);
            MO_ASSERT(mo_value_map(q, fixture.map, &text, &list, 1, &copy));
            bool equal;
            MO_ASSERT(mo_value_equal(q, fixture.map, map, copy, &equal) && equal);
            mo_value_drop(q, fixture.map, copy);
            copy = mo_value_dup(q, fixture.map, map);
            if (mo_table_remove(q, fixture.map, &copy, text)) {
                MO_ASSERT(mo_table_size(copy) == 0 && mo_table_size(map) == 1);
            }
            mo_value_drop(q, fixture.map, copy);
            mo_value_drop(image, fixture.map, map);
            mo_value_drop(image, fixture.list, list);
            mo_value_drop(image, fixture.builtin[MO_TYPE_TEXT], text);
            break;
        }
        case 24:
            if (fixture.types.items[type].kind == MO_TYPE_LIST && mo_list_size(values[src]) <= 32) {
                struct mo_cell items[32];
                uint32_t count = mo_list_size(values[src]);
                for (uint32_t j = 0; j < count; j++) {
                    MO_ASSERT(mo_list_get(values[src], j, &items[j]));
                }
                built = mo_value_list(p, type, items, count, &result);
                if (built) {
                    bool equal;
                    MO_ASSERT(mo_value_equal(p, type, values[src], result, &equal) && equal);
                }
            }
            break;
        }
        if (built) {
            mo_value_drop(p, types[dst], values[dst]);
            values[dst] = result;
            types[dst] = type;
        } else {
            MO_ASSERT(memcmp(&result, &unchanged, sizeof result) == 0);
        }
        MO_ASSERT(mo_heap_used(p) <= limits.heap_size);
    }
    for (uint32_t i = 0; i < 32; i++) {
        mo_value_drop(p, types[i], values[i]);
    }
    MO_ASSERT(mo_heap_objects(p) == 0 && mo_heap_used(p) == baseline);
    MO_ASSERT(mo_heap_objects(q) == 0 && mo_heap_used(q) == baseline);
    mo_heap_free(p);
    mo_heap_free(q);
    mo_heap_image_free(fixture.image);
    mo_arena_free(&fixture.arena);
    return 0;
}
