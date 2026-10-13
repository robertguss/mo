#include "heap/internal.h"

#include "base/hash.h"

uint64_t mo_heap_mix(const uint64_t key[2], uint64_t left, uint64_t right) {
    uint8_t bytes[16];
    for (unsigned i = 0; i < 8; i++) {
        bytes[i] = (uint8_t)(left >> (8 * i));
        bytes[i + 8] = (uint8_t)(right >> (8 * i));
    }
    return mo_hash(key, bytes, sizeof bytes);
}
