#ifndef MO_SHA256_H
#define MO_SHA256_H
#include "base/base.h"
#include <stddef.h>
#include <stdint.h>
/* Streaming SHA-256, constant space. The byte count is modulo 2^64. */
struct mo_sha256 {
    uint32_t state[8];
    uint64_t length;
    uint8_t block[64];
    size_t used;
};
void mo_sha256_init(struct mo_sha256 *hash);
void mo_sha256_update(struct mo_sha256 *hash, const void *bytes, size_t length);
void mo_sha256_finish(struct mo_sha256 *hash, uint8_t digest[32]);
#endif
