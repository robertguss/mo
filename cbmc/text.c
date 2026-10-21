/* CBMC proofs of UTF-8 in Text's and Bytes' primitives, src/prim/text.c (V5 part 3, MO-55).
 *
 * Bytes.decode_utf8 decides which bytes are text, and every Text primitive that walks a text
 * scalar by scalar trusts what it accepted. Char.to_text and List.from_chars spell a Char in
 * UTF-8. So the proofs show, against the standard's own table (cbmc/utf8.h):
 *
 * - scalar, the decoder, accepts exactly the well-formed sequences, reads only inside its span,
 *   and returns the scalar value a sequence encodes and the offset just past it;
 * - encode spells every scalar value as the one well-formed sequence that encodes it.
 *
 * text.c is included rather than linked, so that its static functions can be proved. */
#include "prim/text.c"

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

/* The longest span a proof decodes in: a four-byte sequence that need not start the span. */
#ifndef MO_CBMC_SPAN
#define MO_CBMC_SPAN 6
#endif
#define SPAN MO_CBMC_SPAN

size_t nondet_size(void);
uint32_t nondet_u32(void);

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

/* Any span of up to SPAN bytes, as an object of exactly its length so that a read past it is a
 * bounds failure. */
static struct text any_span(void) {
    size_t length = nondet_size();
    __CPROVER_assume(length >= 1 && length <= SPAN);
    uint8_t *bytes = malloc(length);
    __CPROVER_assume(bytes != NULL);
    return (struct text){bytes, length};
}

/* scalar, from any offset inside any span: it accepts exactly when a well-formed sequence starts
 * there, and then gives its value and moves past it. */
void harness_text_scalar(void) {
    struct text t = any_span();
    size_t start = nondet_size();
    __CPROVER_assume(start < t.length);
    size_t at = start;
    uint32_t out = nondet_u32(), before = out;
    unsigned n = well_formed(t.bytes, t.length, start);
    if (scalar(t, &at, &out)) {
        __CPROVER_assert(n != 0, "text scalar: accepts only well-formed UTF-8");
        __CPROVER_assert(at == start + n, "text scalar: moves past the sequence");
        __CPROVER_assert(out == encoded(t.bytes, start, n), "text scalar: the encoded value");
        __CPROVER_assert(scalar_value(out), "text scalar: a scalar value");
    } else {
        __CPROVER_assert(n == 0, "text scalar: refuses only ill-formed UTF-8");
        __CPROVER_assert(out == before, "text scalar: a refusal gives no value");
        __CPROVER_assert(at > start && at <= t.length, "text scalar: stays inside the span");
    }
    reached_end();
}

/* encode, for any scalar value: the one well-formed sequence that encodes it. */
void harness_text_encode(void) {
    uint32_t c = nondet_u32();
    __CPROVER_assume(scalar_value(c));
    uint8_t *bytes = malloc(4);
    __CPROVER_assume(bytes != NULL);
    unsigned n = encode(c, bytes);
    __CPROVER_assert(n >= 1 && n <= 4, "text encode: one to four bytes");
    __CPROVER_assert(well_formed(bytes, n, 0) == n, "text encode: well-formed");
    __CPROVER_assert(encoded(bytes, 0, n) == c, "text encode: encodes the value");
    reached_end();
}

#pragma CPROVER check pop
