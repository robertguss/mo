/* SipHash-1-3. Byte loading is explicit so native endian never affects a hash. */
#include "base/hash.h"

static MO_MUST_USE uint64_t rotate(uint64_t value, unsigned bits) {
    return (value << bits) | (value >> (64 - bits));
}

static void round_hash(uint64_t v[4]) {
    v[0] += v[1];
    v[1] = rotate(v[1], 13) ^ v[0];
    v[0] = rotate(v[0], 32);
    v[2] += v[3];
    v[3] = rotate(v[3], 16) ^ v[2];
    v[0] += v[3];
    v[3] = rotate(v[3], 21) ^ v[0];
    v[2] += v[1];
    v[1] = rotate(v[1], 17) ^ v[2];
    v[2] = rotate(v[2], 32);
}

uint64_t mo_hash(const uint64_t key[2], const void *bytes, size_t length) {
    MO_ASSERT(key != NULL && (bytes != NULL || length == 0));
    const uint8_t *data = bytes;
    uint64_t v[] = {key[0] ^ UINT64_C(0x736f6d6570736575), key[1] ^ UINT64_C(0x646f72616e646f6d),
                    key[0] ^ UINT64_C(0x6c7967656e657261), key[1] ^ UINT64_C(0x7465646279746573)};
    size_t offset = 0;
    while (length - offset >= 8) {
        uint64_t word = 0;
        for (unsigned i = 0; i < 8; i++) {
            word |= (uint64_t)data[offset + i] << (8 * i);
        }
        offset += 8;
        v[3] ^= word;
        round_hash(v);
        v[0] ^= word;
    }
    uint64_t tail = (uint64_t)length << 56;
    for (unsigned i = 0; offset < length; i++, offset++) {
        tail |= (uint64_t)data[offset] << (8 * i);
    }
    v[3] ^= tail;
    round_hash(v);
    v[0] ^= tail;
    v[2] ^= 0xff;
    for (unsigned i = 0; i < 3; i++) {
        round_hash(v);
    }
    return v[0] ^ v[1] ^ v[2] ^ v[3];
}
