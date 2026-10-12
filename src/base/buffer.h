/* A growable buffer of bytes, within a limit: the output of mo_core_print, mo_compile and
 * mo_render_defect (architecture section 12), and of anything else that writes text or bytes.
 *
 * A buffer holds its bytes in an arena of its own, until mo_buffer_free. They move when the buffer
 * grows, so a pointer into them lasts only until the next append: keep offsets instead. Every
 * append either adds all of what it is given, or nothing: past the limit, or when the system has
 * no memory, it returns false and leaves the buffer as it was. */
#ifndef MO_BUFFER_H
#define MO_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/arena.h"
#include "base/base.h"

/* A buffer. bytes and length may be read; the other fields are private. */
struct mo_buffer {
    struct mo_arena arena; /* holds bytes, and nothing else */
    uint8_t *bytes;        /* length bytes; NULL before the first append */
    size_t length;
    size_t capacity; /* bytes that fit before it grows */
    size_t limit;    /* the most bytes it may hold */
};

/* Prepares an empty buffer that may hold up to limit bytes. It takes no memory yet, so it can't
 * fail. */
void mo_buffer_init(struct mo_buffer *buffer, size_t limit);

/* Frees the bytes. The buffer is then empty, with the same limit, and can be used again. */
void mo_buffer_free(struct mo_buffer *buffer);

/* Empties the buffer, keeping its memory for what comes next. */
void mo_buffer_clear(struct mo_buffer *buffer);

/* Appends length bytes, which may be some of the buffer's own. */
MO_MUST_USE bool mo_buffer_append(struct mo_buffer *buffer, const void *bytes, size_t length);

/* Appends one byte. */
MO_MUST_USE bool mo_buffer_append_byte(struct mo_buffer *buffer, uint8_t byte);

/* Appends a NUL-terminated text, without its NUL. */
MO_MUST_USE bool mo_buffer_append_text(struct mo_buffer *buffer, const char *text);

/* Appends a number in decimal, with a - before a negative one: 0, 42, -9223372036854775808. */
MO_MUST_USE bool mo_buffer_append_int(struct mo_buffer *buffer, int64_t value);

/* Appends a number in decimal: 0, 18446744073709551615. */
MO_MUST_USE bool mo_buffer_append_uint(struct mo_buffer *buffer, uint64_t value);

/* Appends a number in hexadecimal, in lowercase and without a prefix, with zeros before it to make
 * at least min_digits digits, at most 16: 3ff0000000000000 for the bits of the Float 1.0. */
MO_MUST_USE bool mo_buffer_append_hex(struct mo_buffer *buffer, uint64_t value,
                                      unsigned min_digits);

/* Append a number in 2, 4 or 8 bytes, the lowest first, as the bytecode format writes its integers
 * (bytecode.md section 3). */
MO_MUST_USE bool mo_buffer_append_le16(struct mo_buffer *buffer, uint16_t value);
MO_MUST_USE bool mo_buffer_append_le32(struct mo_buffer *buffer, uint32_t value);
MO_MUST_USE bool mo_buffer_append_le64(struct mo_buffer *buffer, uint64_t value);

#endif
