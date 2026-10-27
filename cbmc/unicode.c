/* CBMC proofs of UTF-8 in the Unicode16 primitives, src/prim/unicode.c.
 *
 * Unicode16.display_width and the grapheme splitter that Text's terminal output uses read spans
 * that may be malformed; the other Unicode16 primitives read texts, which are always
 * well-formed. The proofs show, against the standard's own table (cbmc/utf8.h):
 *
 * - checked_scalar accepts exactly the well-formed sequences, giving the value and the offset
 *   past it, and on refusal changes nothing;
 * - scalar gives the same for any well-formed sequence, the only input it is given;
 * - encode counts and spells every scalar value as the one well-formed sequence for it;
 * - mo_unicode16_next_grapheme and mo_unicode16_display_width read only inside their span, never
 *   fail an MO_ASSERT, and succeed exactly on well-formed spans; a cluster ends on a scalar
 *   boundary, and a width is at most two columns per scalar.
 *
 * Which scalars join into a cluster, and how wide each one is, follow the Unicode data tables;
 * ci/check_unicode16.py runs the standard's conformance tests over them.
 *
 * unicode.c is included rather than linked, so that its static functions can be proved. */
#include "prim/unicode.c"

#include <stdlib.h>

#pragma CPROVER check push
#pragma CPROVER check disable "bounds"
#pragma CPROVER check disable "pointer"
#pragma CPROVER check disable "pointer-overflow"
#pragma CPROVER check disable "pointer-primitive"
#pragma CPROVER check disable "signed-overflow"
#pragma CPROVER check disable "conversion"
#pragma CPROVER check disable "undefined-shift"
#pragma CPROVER check disable "div-by-zero"

#include "utf8.h"

/* The longest span a proof reads: room for a four-byte sequence that does not start it. */
#ifndef MO_CBMC_SPAN
#define MO_CBMC_SPAN 6
#endif
#define SPAN MO_CBMC_SPAN

size_t nondet_size(void);
uint32_t nondet_u32(void);
uint64_t nondet_u64(void);

/* An MO_ASSERT that fails is a failed proof, not the end of a path. */
_Noreturn void mo_assert_failed(const char *expression, const char *file, int line) {
    (void)expression;
    (void)file;
    (void)line;
    __CPROVER_assert(0, "MO_ASSERT holds");
    __CPROVER_assume(0);
    abort();
}

/* Every harness ends here, so prove.sh --reach can check that some input gets to its end. */
void reached_end(void) {
}

/* Any span of least to SPAN bytes, as an object of exactly its length so that a read past it is
 * a bounds failure, or NULL when empty. */
static const uint8_t *any_span(size_t least, size_t *length) {
    *length = nondet_size();
    __CPROVER_assume(*length >= least && *length <= SPAN);
    if (*length == 0) {
        return NULL;
    }
    uint8_t *bytes = malloc(*length);
    __CPROVER_assume(bytes != NULL);
    return bytes;
}

/* checked_scalar, from any offset inside any span. */
void harness_checked_scalar(void) {
    size_t length;
    const uint8_t *bytes = any_span(1, &length);
    size_t start = nondet_size();
    __CPROVER_assume(start < length);
    size_t at = start;
    uint32_t out = nondet_u32(), before = out;
    unsigned n = well_formed(bytes, length, start);
    if (checked_scalar(bytes, length, &at, &out)) {
        __CPROVER_assert(n != 0, "checked_scalar: accepts only well-formed UTF-8");
        __CPROVER_assert(at == start + n, "checked_scalar: moves past the sequence");
        __CPROVER_assert(out == encoded(bytes, start, n), "checked_scalar: the encoded value");
        __CPROVER_assert(scalar_value(out), "checked_scalar: a scalar value");
    } else {
        __CPROVER_assert(n == 0, "checked_scalar: refuses only ill-formed UTF-8");
        __CPROVER_assert(at == start && out == before, "checked_scalar: a refusal changes nothing");
    }
    reached_end();
}

/* scalar, from the start of any well-formed sequence inside any span. */
void harness_unicode_scalar(void) {
    size_t length;
    const uint8_t *bytes = any_span(1, &length);
    size_t start = nondet_size();
    __CPROVER_assume(start < length);
    unsigned n = well_formed(bytes, length, start);
    __CPROVER_assume(n != 0);
    size_t at = start;
    uint32_t c = scalar(bytes, length, &at);
    __CPROVER_assert(at == start + n, "scalar: moves past the sequence");
    __CPROVER_assert(c == encoded(bytes, start, n), "scalar: the encoded value");
    reached_end();
}

/* encode, for any scalar value, counting and spelling. */
void harness_unicode_encode(void) {
    uint32_t c = nondet_u32();
    __CPROVER_assume(scalar_value(c));
    uint8_t *bytes = malloc(4);
    __CPROVER_assume(bytes != NULL);
    unsigned counted = encode(c, NULL);
    unsigned n = encode(c, bytes);
    __CPROVER_assert(counted == n && n >= 1 && n <= 4, "encode: counts what it spells");
    __CPROVER_assert(well_formed(bytes, n, 0) == n, "encode: well-formed");
    __CPROVER_assert(encoded(bytes, 0, n) == c, "encode: encodes the value");
    reached_end();
}

/* properties, for any scalar value: its binary search stays inside the table, and finds the
 * range that holds the scalar (the table's ranges are sorted and apart), whose properties it
 * gives, or none for a scalar in no range, with a Hangul syllable's LV or LVT break class. */
void harness_properties(void) {
    uint32_t c = nondet_u32();
    __CPROVER_assume(scalar_value(c));
    size_t i = nondet_size();
    uint32_t p = properties(c);
    if (c < 128) {
        __CPROVER_assert(p == mo_unicode_ascii[c], "properties: an ASCII scalar's");
    } else if (i < COUNT(mo_unicode_ranges) && mo_unicode_ranges[i].first <= c &&
               c <= mo_unicode_ranges[i].last) {
        uint32_t expected = mo_unicode_ranges[i].properties;
        if (c >= 0xac00 && c <= 0xd7a3) {
            expected = (expected & ~15U) | ((c - 0xac00) % 28 == 0 ? MO_GCB_LV : MO_GCB_LVT);
        }
        __CPROVER_assert(p == expected, "properties: its range's");
    } else if (i <= COUNT(mo_unicode_ranges) && (i == 0 || mo_unicode_ranges[i - 1].last < c) &&
               (i == COUNT(mo_unicode_ranges) || c < mo_unicode_ranges[i].first)) {
        /* Before range i and after the one before it, or past the last: no properties but a
         * Hangul syllable's. */
        uint32_t expected = 0;
        if (c >= 0xac00 && c <= 0xd7a3) {
            expected = (c - 0xac00) % 28 == 0 ? MO_GCB_LV : MO_GCB_LVT;
        }
        __CPROVER_assert(p == expected, "properties: none outside every range");
    }
    reached_end();
}

/* What the walks below rely on of a scalar's properties: nothing, so any bits will do. Looking
 * them up in the real tables, once per scalar, takes CBMC more memory than CI has. */
uint32_t model_properties(uint32_t c) {
    __CPROVER_assert(scalar_value(c), "properties: of a scalar value");
    return nondet_u32();
}

/* mo_unicode16_next_grapheme, on any span. */
void harness_next_grapheme(void) {
    size_t length;
    const uint8_t *bytes = any_span(0, &length);
    size_t out = nondet_size(), before = out, scalars;
    bool whole = all_well_formed(bytes, 0, length, &scalars);
    if (mo_unicode16_next_grapheme(bytes, length, &out)) {
        size_t prefix;
        __CPROVER_assert(out > 0 && out <= length, "next_grapheme: a cluster inside the span");
        __CPROVER_assert(all_well_formed(bytes, 0, out, &prefix),
                         "next_grapheme: whole well-formed scalars");
        __CPROVER_assert(out == length || well_formed(bytes, length, out) != 0,
                         "next_grapheme: ends before a well-formed scalar");
    } else {
        __CPROVER_assert(length == 0 || !whole, "next_grapheme: refuses only empty or ill-formed");
        __CPROVER_assert(out == before, "next_grapheme: a refusal leaves out alone");
    }
    reached_end();
}

/* mo_unicode16_display_width, on any span. */
void harness_display_width(void) {
    size_t length;
    const uint8_t *bytes = any_span(0, &length);
    uint64_t width = nondet_u64(), before = width;
    size_t scalars;
    bool whole = all_well_formed(bytes, 0, length, &scalars);
    if (mo_unicode16_display_width(bytes, length, &width)) {
        __CPROVER_assert(whole, "display_width: accepts only well-formed UTF-8");
        __CPROVER_assert(width <= 2 * scalars, "display_width: at most two columns a scalar");
    } else {
        __CPROVER_assert(!whole, "display_width: refuses only ill-formed UTF-8");
        __CPROVER_assert(width == before, "display_width: a refusal leaves width alone");
    }
    reached_end();
}

#pragma CPROVER check pop
