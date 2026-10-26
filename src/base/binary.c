#include "base/binary.h"

bool mo_binary_read(struct mo_binary *input, size_t count, const uint8_t **out) {
    MO_ASSERT(input->position <= input->length);
    if (count > input->length - input->position)
        return false;
    *out = count == 0 ? input->bytes : input->bytes + input->position;
    input->position += count;
    return true;
}

bool mo_binary_uint(struct mo_binary *input, unsigned width, uint64_t *out) {
    MO_ASSERT(width > 0 && width <= 8);
    const uint8_t *bytes;
    if (!mo_binary_read(input, width, &bytes))
        return false;
    uint64_t value = 0;
    for (unsigned i = 0; i < width; i++)
        value |= (uint64_t)bytes[i] << (8 * i);
    *out = value;
    return true;
}

bool mo_binary_write(struct mo_buffer *out, uint64_t value, unsigned width) {
    MO_ASSERT(width > 0 && width <= 8);
    uint8_t bytes[8];
    for (unsigned i = 0; i < width; i++)
        bytes[i] = (uint8_t)(value >> (8 * i));
    return mo_buffer_append(out, bytes, width);
}

uint64_t mo_binary_checksum(const uint8_t *bytes, size_t length) {
    uint64_t sum = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < length; i++)
        sum = (sum ^ bytes[i]) * UINT64_C(1099511628211);
    return sum;
}
