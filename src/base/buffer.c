#include "base/buffer.h"

#include <string.h>

/* The first block a buffer takes. It doubles from there. */
enum { FIRST_CAPACITY = 256 };

void mo_buffer_init(struct mo_buffer *buffer, size_t limit) {
    MO_ASSERT(buffer != NULL);
    /* The buffer's block is the only thing in its arena, so the arena never needs more than the
     * block and one chunk's header (src/base/arena.h). */
    size_t arena_limit =
        limit > SIZE_MAX - MO_ARENA_CHUNK_HEADER ? SIZE_MAX : limit + MO_ARENA_CHUNK_HEADER;
    mo_arena_init(&buffer->arena, arena_limit);
    buffer->bytes = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
    buffer->limit = limit;
}

void mo_buffer_free(struct mo_buffer *buffer) {
    MO_ASSERT(buffer != NULL);
    mo_arena_free(&buffer->arena);
    buffer->bytes = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
}

void mo_buffer_clear(struct mo_buffer *buffer) {
    MO_ASSERT(buffer != NULL);
    buffer->length = 0;
}

/* Makes room for more bytes after the ones held, within the limit. */
static bool reserve(struct mo_buffer *buffer, size_t more) {
    MO_ASSERT(buffer->length <= buffer->capacity && buffer->capacity <= buffer->limit);
    if (more > buffer->limit - buffer->length) {
        return false;
    }
    size_t needed = buffer->length + more;
    if (needed <= buffer->capacity) {
        return true;
    }
    size_t capacity = buffer->capacity < FIRST_CAPACITY ? FIRST_CAPACITY : buffer->capacity;
    while (capacity < needed && capacity <= buffer->limit / 2) {
        capacity *= 2;
    }
    if (capacity < needed || capacity > buffer->limit) {
        capacity = buffer->limit;
    }
    uint8_t *bytes = mo_arena_resize(&buffer->arena, buffer->bytes, buffer->capacity, capacity, 1);
    if (bytes == NULL) {
        return false;
    }
    buffer->bytes = bytes;
    buffer->capacity = capacity;
    return true;
}

bool mo_buffer_append(struct mo_buffer *buffer, const void *bytes, size_t length) {
    MO_ASSERT(buffer != NULL && (bytes != NULL || length == 0));
    /* Bytes of the buffer's own move with it if it grows, so they are found again by offset. */
    uintptr_t from = (uintptr_t)bytes;
    uintptr_t start = (uintptr_t)buffer->bytes;
    bool own = buffer->bytes != NULL && from >= start && from - start < buffer->capacity;
    size_t offset = own ? (size_t)(from - start) : 0;
    MO_ASSERT(!own || (offset <= buffer->length && length <= buffer->length - offset));
    if (!reserve(buffer, length)) {
        return false;
    }
    if (length > 0) {
        const uint8_t *source = own ? buffer->bytes + offset : bytes;
        memcpy(buffer->bytes + buffer->length, source, length);
        buffer->length += length;
    }
    return true;
}

bool mo_buffer_append_byte(struct mo_buffer *buffer, uint8_t byte) {
    return mo_buffer_append(buffer, &byte, 1);
}

bool mo_buffer_append_text(struct mo_buffer *buffer, const char *text) {
    MO_ASSERT(text != NULL);
    return mo_buffer_append(buffer, text, strlen(text));
}

/* The decimal digits of a number, written backward from the end of digits; returns the first. */
static char *decimal(char *end, uint64_t value) {
    char *first = end;
    do {
        first -= 1;
        *first = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    return first;
}

bool mo_buffer_append_int(struct mo_buffer *buffer, int64_t value) {
    char digits[21]; /* a sign and 19 digits, with room to spare */
    char *end = digits + sizeof digits;
    /* The magnitude, computed without negating INT64_MIN, which has no positive int64_t. */
    uint64_t magnitude = value < 0 ? (uint64_t)0 - (uint64_t)value : (uint64_t)value;
    char *first = decimal(end, magnitude);
    if (value < 0) {
        first -= 1;
        *first = '-';
    }
    return mo_buffer_append(buffer, first, (size_t)(end - first));
}

bool mo_buffer_append_uint(struct mo_buffer *buffer, uint64_t value) {
    char digits[20];
    char *end = digits + sizeof digits;
    char *first = decimal(end, value);
    return mo_buffer_append(buffer, first, (size_t)(end - first));
}

bool mo_buffer_append_hex(struct mo_buffer *buffer, uint64_t value, unsigned min_digits) {
    MO_ASSERT(min_digits <= 16);
    char digits[16];
    char *end = digits + sizeof digits;
    char *first = end;
    do {
        first -= 1;
        *first = "0123456789abcdef"[value % 16];
        value /= 16;
    } while (value != 0);
    while ((size_t)(end - first) < min_digits) {
        first -= 1;
        *first = '0';
    }
    return mo_buffer_append(buffer, first, (size_t)(end - first));
}

bool mo_buffer_append_le32(struct mo_buffer *buffer, uint32_t value) {
    uint8_t bytes[4] = {
        (uint8_t)value,
        (uint8_t)(value >> 8),
        (uint8_t)(value >> 16),
        (uint8_t)(value >> 24),
    };
    return mo_buffer_append(buffer, bytes, sizeof bytes);
}
