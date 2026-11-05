#include "base/sha256.h"
#include <string.h>

static const uint32_t constants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
static uint32_t rotate(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}
static void compress(struct mo_sha256 *h) {
    uint32_t w[64];
    for (size_t i = 0; i < 16; i++) {
        const uint8_t *p = h->block + 4 * i;
        w[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    }
    for (size_t i = 16; i < 64; i++) {
        uint32_t x = w[i - 15], y = w[i - 2];
        w[i] = w[i - 16] + (rotate(x, 7) ^ rotate(x, 18) ^ (x >> 3)) + w[i - 7] +
               (rotate(y, 17) ^ rotate(y, 19) ^ (y >> 10));
    }
    uint32_t a = h->state[0], b = h->state[1], c = h->state[2], d = h->state[3], e = h->state[4],
             f = h->state[5], g = h->state[6], z = h->state[7];
    for (size_t i = 0; i < 64; i++) {
        uint32_t t1 = z + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) + ((e & f) ^ (~e & g)) +
                      constants[i] + w[i];
        uint32_t t2 =
            (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        z = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h->state[0] += a;
    h->state[1] += b;
    h->state[2] += c;
    h->state[3] += d;
    h->state[4] += e;
    h->state[5] += f;
    h->state[6] += g;
    h->state[7] += z;
}
void mo_sha256_init(struct mo_sha256 *h) {
    MO_ASSERT(h != NULL);
    *h = (struct mo_sha256){.state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f,
                                      0x9b05688c, 0x1f83d9ab, 0x5be0cd19}};
}
void mo_sha256_update(struct mo_sha256 *h, const void *bytes, size_t length) {
    MO_ASSERT(h != NULL && (bytes != NULL || length == 0));
    const uint8_t *p = bytes;
    h->length += (uint64_t)length;
    while (length != 0) {
        size_t n = 64 - h->used;
        if (n > length)
            n = length;
        memcpy(h->block + h->used, p, n);
        h->used += n;
        p += n;
        length -= n;
        if (h->used == 64) {
            compress(h);
            h->used = 0;
        }
    }
}
void mo_sha256_finish(struct mo_sha256 *h, uint8_t digest[32]) {
    MO_ASSERT(h != NULL && digest != NULL);
    uint64_t bits = h->length * 8;
    h->block[h->used++] = 0x80;
    if (h->used > 56) {
        memset(h->block + h->used, 0, 64 - h->used);
        compress(h);
        h->used = 0;
    }
    memset(h->block + h->used, 0, 56 - h->used);
    for (size_t i = 0; i < 8; i++)
        h->block[63 - i] = (uint8_t)(bits >> (8 * i));
    compress(h);
    for (size_t i = 0; i < 32; i++)
        digest[i] = (uint8_t)(h->state[i / 4] >> (24 - 8 * (i % 4)));
    memset(h, 0, sizeof *h);
}
