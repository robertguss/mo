/* UTF-8 as the Unicode Standard defines it (version 16, table 3-7, "Well-Formed UTF-8 Byte
 * Sequences"), the specification cbmc/text.c and cbmc/unicode.c hold Mo's decoders and encoders
 * to. It is written from the table's byte ranges, not from the decoders' own arithmetic, so a
 * decoder that agrees with it agrees with the standard. */
#ifndef MO_CBMC_UTF8_H
#define MO_CBMC_UTF8_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool in(uint8_t byte, uint8_t first, uint8_t last) {
    return byte >= first && byte <= last;
}

/* The length of the well-formed sequence that starts bytes[at] and ends inside length, or 0 if
 * none does. */
static unsigned well_formed(const uint8_t *bytes, size_t length, size_t at) {
    size_t left = length - at;
    uint8_t b0 = bytes[at];
    if (b0 <= 0x7f)
        return 1;
    if (left < 2)
        return 0;
    uint8_t b1 = bytes[at + 1];
    if (in(b0, 0xc2, 0xdf))
        return in(b1, 0x80, 0xbf) ? 2U : 0U;
    bool second = (b0 == 0xe0 && in(b1, 0xa0, 0xbf)) ||
                  ((in(b0, 0xe1, 0xec) || in(b0, 0xee, 0xef)) && in(b1, 0x80, 0xbf)) ||
                  (b0 == 0xed && in(b1, 0x80, 0x9f));
    if (second)
        return left >= 3 && in(bytes[at + 2], 0x80, 0xbf) ? 3U : 0U;
    second = (b0 == 0xf0 && in(b1, 0x90, 0xbf)) || (in(b0, 0xf1, 0xf3) && in(b1, 0x80, 0xbf)) ||
             (b0 == 0xf4 && in(b1, 0x80, 0x8f));
    if (second)
        return left >= 4 && in(bytes[at + 2], 0x80, 0xbf) && in(bytes[at + 3], 0x80, 0xbf) ? 4U
                                                                                           : 0U;
    return 0;
}

/* The scalar value a well-formed sequence of n bytes encodes (table 3-6): the low bits of each
 * byte, after the lead byte's n marker bits and each continuation byte's two. */
static uint32_t encoded(const uint8_t *bytes, size_t at, unsigned n) {
    if (n == 1)
        return bytes[at];
    uint32_t value = bytes[at] & (0x7fU >> n);
    for (unsigned i = 1; i < n; i++)
        value = (value << 6) | (bytes[at + i] & 0x3fU);
    return value;
}

/* Whether bytes[from, to) is a run of whole well-formed sequences, and how many. */
static bool all_well_formed(const uint8_t *bytes, size_t from, size_t to, size_t *count) {
    size_t at = from;
    *count = 0;
    while (at < to) {
        unsigned n = well_formed(bytes, to, at);
        if (n == 0)
            return false;
        at += n;
        (*count)++;
    }
    return true;
}

/* A Unicode scalar value: a code point that is not a surrogate. */
static bool scalar_value(uint32_t c) {
    return c <= 0x10ffff && (c < 0xd800 || c > 0xdfff);
}

#endif
