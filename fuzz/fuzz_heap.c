#include "../tests/heap_fixture.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    struct heap_fixture fixture;
    heap_fixture_init(&fixture);
    struct mo_limits limits = mo_limits_default();
    limits.heap_size = 64 << 10;
    struct mo_process *p = mo_heap_new(&fixture.types, &limits);
    MO_ASSERT(p != NULL);
    size_t baseline = mo_heap_used(p);
    struct mo_cell values[32] = {0};
    uint32_t types[32];
    for (uint32_t i = 0; i < 32; i++) {
        types[i] = fixture.builtin[MO_TYPE_UNIT];
    }
    size_t steps = size < 4096 ? size : 4096;
    for (size_t i = 0; i + 3 < steps; i += 4) {
        uint32_t dst = data[i + 1] % 32, src = data[i + 2] % 32;
        uint32_t other = data[i + 3] % 32;
        struct mo_cell result = mo_cell_int(31);
        struct mo_cell unchanged = result;
        uint32_t type = types[src];
        bool built = false;
        switch (data[i] % 14) {
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
            if (types[src] == fixture.builtin[MO_TYPE_TEXT] && types[other] == fixture.list) {
                struct mo_cell fields[] = {values[src], values[other]};
                bool closure = data[i] % 14 == 13;
                type = closure ? fixture.function : fixture.tuple;
                built = closure ? mo_value_closure(p, 7, fields, 2, &result)
                                : mo_value_construct(p, fixture.tuple_layout, fields, 2, &result);
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
    mo_heap_free(p);
    mo_arena_free(&fixture.arena);
    return 0;
}
