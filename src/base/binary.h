#ifndef MO_BINARY_H
#define MO_BINARY_H

#include "base/buffer.h"

/* Bounded byte spans. Failed reads preserve the cursor and output. */
struct mo_binary {
    const uint8_t *bytes;
    size_t length, position;
};
MO_MUST_USE bool mo_binary_read(struct mo_binary *input, size_t count, const uint8_t **out);
MO_MUST_USE bool mo_binary_uint(struct mo_binary *input, unsigned width, uint64_t *out);
MO_MUST_USE bool mo_binary_write(struct mo_buffer *out, uint64_t value, unsigned width);
/* FNV-1a-64 detects accidental corruption; this is not authentication. */
MO_MUST_USE uint64_t mo_binary_checksum(const uint8_t *bytes, size_t length);

#endif
