#include "bytecode/io.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void round_trip(const uint8_t *data, size_t size) {
    struct mo_arena arena;
    struct mo_load_error load_error;
    mo_arena_init(&arena, MO_LIMIT_BYTECODE_SIZE);
    const struct mo_bytecode *file = mo_bytecode_decode(&arena, data, size, &load_error);
    if (file != NULL) {
        struct mo_buffer out;
        struct mo_write_error write_error;
        mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
        MO_ASSERT(mo_bytecode_write(file, &out, &write_error));
        MO_ASSERT(size == out.length && memcmp(data, out.bytes, size) == 0);
        mo_buffer_free(&out);
    } else {
        MO_ASSERT(load_error.reason != NULL && load_error.offset <= size);
    }
    mo_arena_free(&arena);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    round_trip(data, size);
    /* Repair the checksum too, so mutations reach the tables rather than stopping at FNV.
     * Still test the untouched input above: a bad checksum must never be accepted. */
    if (size >= MO_BYTECODE_HEADER_SIZE && size <= MO_LIMIT_BYTECODE_SIZE) {
        struct mo_buffer copy;
        mo_buffer_init(&copy, MO_LIMIT_BYTECODE_SIZE);
        if (mo_buffer_append(&copy, data, size)) {
            uint64_t hash = MO_BYTECODE_FNV_OFFSET;
            for (size_t i = MO_BYTECODE_HEADER_SIZE; i < size; i++) {
                hash = (hash ^ copy.bytes[i]) * MO_BYTECODE_FNV_PRIME;
            }
            for (unsigned i = 0; i < 8; i++) {
                copy.bytes[12 + i] = (uint8_t)(hash >> (i * 8));
            }
            round_trip(copy.bytes, size);
        }
        mo_buffer_free(&copy);
    }
    return 0;
}
