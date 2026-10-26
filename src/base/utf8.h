/* Strict UTF-8 scalar decoding: Unicode well-formed sequences only. */
#ifndef MO_UTF8_H
#define MO_UTF8_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/base.h"

/* One Unicode scalar at *pos. Advances *pos only after a complete well-formed
 * sequence; a refusal leaves *pos and *out unchanged. Overlong encodings,
 * surrogates and values above U+10FFFF are refused. bytes may be NULL only when
 * length is zero. */
MO_MUST_USE bool mo_utf8_decode(const uint8_t *bytes, size_t length, size_t *pos, uint32_t *out);

/* Whether every byte is part of a well-formed sequence. The empty span is
 * valid. bytes may be NULL only when length is zero. */
MO_MUST_USE bool mo_utf8_valid(const uint8_t *bytes, size_t length);

#endif
