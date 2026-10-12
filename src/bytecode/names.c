#include "bytecode/names.h"

#include <stdlib.h>
#include <string.h>

static MO_MUST_USE int compare_bytes(const uint8_t *a, size_t an, const uint8_t *b, size_t bn) {
    size_t n = an < bn ? an : bn;
    int result = n == 0 ? 0 : memcmp(a, b, n);
    return result != 0 ? result : (an > bn) - (an < bn);
}

static MO_MUST_USE int compare_names(const void *a, const void *b) {
    const struct mo_bytecode_name *left = a;
    const struct mo_bytecode_name *right = b;
    int result = compare_bytes(left->string->bytes, left->string->length, right->string->bytes,
                               right->string->length);
    return result != 0 ? result : (left->index > right->index) - (left->index < right->index);
}

bool mo_bytecode_names_init(struct mo_bytecode_names *names, struct mo_arena *arena,
                            const struct mo_bytecode *file) {
    MO_ASSERT(names != NULL && arena != NULL && file != NULL);
    *names = (struct mo_bytecode_names){.count = file->string_count};
    if (names->count == 0) {
        return true;
    }
    names->items = mo_arena_allocate_array(arena, names->count, sizeof *names->items,
                                           _Alignof(struct mo_bytecode_name));
    if (names->items == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < names->count; i++) {
        names->items[i] = (struct mo_bytecode_name){.string = &file->strings[i], .index = i};
    }
    qsort(names->items, names->count, sizeof *names->items, compare_names);
    return true;
}

uint32_t mo_bytecode_name_find(const struct mo_bytecode_names *names, const uint8_t *bytes,
                               size_t length) {
    MO_ASSERT(names != NULL && (bytes != NULL || length == 0));
    uint32_t low = 0;
    uint32_t high = names->count;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        const struct mo_bytecode_string *s = names->items[mid].string;
        if (compare_bytes(s->bytes, s->length, bytes, length) < 0) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    if (low != names->count) {
        const struct mo_bytecode_string *s = names->items[low].string;
        if (compare_bytes(s->bytes, s->length, bytes, length) == 0) {
            return names->items[low].index;
        }
    }
    return MO_BYTECODE_NO_INDEX;
}
