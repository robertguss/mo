#include "../tests/heap_fixture.h"
#include "base/binary.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0 || size > 65536)
        return 0;
    struct heap_fixture f;
    heap_fixture_init(&f);
    struct mo_limits limits = mo_limits_default();
    limits.heap_size = 8 << 20;
    struct mo_heap *heap = mo_heap_new(f.image, &limits);
    MO_ASSERT(heap != NULL);
    struct mo_cell text, list, map, set, record, node;
    MO_ASSERT(mo_value_text(heap, "", 0, &text));
    MO_ASSERT(mo_value_list(heap, f.list, NULL, 0, &list));
    MO_ASSERT(mo_value_map(heap, f.map, NULL, NULL, 0, &map));
    MO_ASSERT(mo_value_set(heap, f.set, NULL, 0, &set));
    struct mo_cell fields[] = {text, list};
    MO_ASSERT(mo_value_construct(heap, f.record_layout, fields, 2, &record));
    MO_ASSERT(mo_value_case(heap, f.node, 0, NULL, 0, &node));
    struct mo_cell values[] = {mo_cell_bool(false),
                               mo_cell_char(65),
                               mo_cell_time(0),
                               mo_cell_decimal(0),
                               text,
                               list,
                               map,
                               set,
                               record,
                               node};
    uint32_t types[] = {f.builtin[MO_TYPE_BOOL],
                        f.builtin[MO_TYPE_CHAR],
                        f.builtin[MO_TYPE_TIME],
                        f.builtin[MO_TYPE_DECIMAL],
                        f.builtin[MO_TYPE_TEXT],
                        f.list,
                        f.map,
                        f.set,
                        f.record,
                        f.node};
    unsigned which = data[0] % (sizeof types / sizeof types[0]);
    uint32_t type = types[which];
    struct mo_buffer bytes;
    mo_buffer_init(&bytes, MO_LIMIT_SAVED_BYTES);
    MO_ASSERT(mo_value_encode(heap, type, values[which], &bytes));
    struct mo_binary header = {bytes.bytes, bytes.length, 5};
    uint64_t schema_length;
    MO_ASSERT(mo_binary_uint(&header, 4, &schema_length));
    bytes.length = 9 + (size_t)schema_length;
    MO_ASSERT(mo_buffer_append(&bytes, data + 1, size - 1));
    size_t used = mo_heap_used(heap);
    for (unsigned raw = 0; raw < 2; raw++) {
        struct mo_cell result = mo_cell_int(713);
        const uint8_t *input = raw ? data + 1 : bytes.bytes;
        size_t length = raw ? size - 1 : bytes.length;
        if (mo_value_decode(heap, type, input, length, &result)) {
            struct mo_buffer encoded;
            mo_buffer_init(&encoded, MO_LIMIT_SAVED_BYTES);
            MO_ASSERT(mo_value_encode(heap, type, result, &encoded));
            MO_ASSERT(encoded.length == length && memcmp(encoded.bytes, input, length) == 0);
            mo_buffer_free(&encoded);
            mo_value_drop(heap, type, result);
        } else
            MO_ASSERT(mo_cell_get_int(result) == 713);
        MO_ASSERT(mo_heap_used(heap) == used);
    }
    mo_buffer_free(&bytes);
    mo_heap_free(heap);
    mo_heap_image_free(f.image);
    mo_arena_free(&f.arena);
    return 0;
}
