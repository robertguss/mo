/* CBMC proofs of Decimal's products and quotients, src/base/decimal.c (V5 part 3, MO-55).
 *
 * Decimal's * and / in the VM go through mo_decimal_mul and mo_decimal_div. Each multiplies two
 * magnitudes into 256 bits, divides that by the denominator one bit at a time, and rounds what is
 * left over once. The proofs show, for every Decimal operand:
 *
 * - multiply adds the products of its operands' 64-bit halves into the exact 256-bit product;
 * - each step of the division keeps numerator-so-far = quotient * denominator + remainder, with
 *   the remainder below the denominator;
 * - round_quotient adds one exactly when the remainder is over half the denominator, or exactly
 *   half with an odd quotient, and refuses exactly when the result is outside Decimal's range;
 * - and no step overflows, shifts out of range or fails an MO_ASSERT.
 *
 * The 256 steps chain by induction (cbmc/README.md): after them the quotient and remainder are
 * those of the whole numerator, so the result is the exact value rounded once, to nearest with
 * ties to even. CBMC cannot check that chain, nor the rounding of a whole product in one proof
 * (each ran for more than ten minutes), so each piece is proved alone and its callers use its
 * model.
 *
 * decimal.c is included rather than linked, so that its static functions can be proved. */
#include "base/decimal.c"

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

/* Unsigned integers wide enough that the specifications below cannot overflow. */
typedef unsigned __CPROVER_bitvector[256] u256;
typedef unsigned __CPROVER_bitvector[260] u260;

#define DECIMAL_MAX ((mo_i128)(((mo_u128)1 << 127) - 1))
#define LIMIT ((mo_u128)1 << 127)

mo_u128 nondet_u128(void);
mo_i128 nondet_i128(void);
uint64_t nondet_u64(void);
bool nondet_bool(void);

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

static u256 value(struct wide w) {
    return (u256)w.words[0] | ((u256)w.words[1] << 64) | ((u256)w.words[2] << 128) |
           ((u256)w.words[3] << 192);
}

static struct wide any_wide(void) {
    struct wide w;
    for (unsigned i = 0; i < 4; i++) {
        w.words[i] = nondet_u64();
    }
    return w;
}

/* ---- multiply ------------------------------------------------------------------------------ */

/* The most the product of two words can be, from whether each is below 2^63: a constant for
 * each case, so that CBMC needs no multiplier to state it. */
static mo_u128 product_bound(uint64_t x, uint64_t y) {
    const mo_u128 small = ((mo_u128)1 << 63) - 1, large = ((mo_u128)1 << 64) - 1;
    bool x_small = x >> 63 == 0, y_small = y >> 63 == 0;
    return x_small && y_small ? small * small : x_small || y_small ? small * large : large * large;
}

/* word_product, for any two words: at most the product of their bounds. That it is the exact
 * product is C's own unsigned arithmetic. */
void harness_word_product(void) {
    uint64_t x = nondet_u64(), y = nondet_u64();
    mo_u128 p = word_product(x, y);
    __CPROVER_assert(p <= product_bound(x, y), "word_product: within the bounds");
    reached_end();
}

/* The model hands out an opaque product for each pair of halves, which the harness adds up.
 * Showing two multiplier circuits equal is too much for CBMC (more than fifteen minutes for 128
 * bits), so multiply's proof checks how it carries the four products into four words, not the
 * products themselves. multiply asks for them in the order a0 b0, a0 b1, a1 b0, a1 b1. */
static uint64_t halves_a[2], halves_b[2];
static mo_u128 products[2][2];
static unsigned products_given;

mo_u128 model_word_product(uint64_t x, uint64_t y) {
    __CPROVER_assert(products_given < 4, "word_product: four products");
    unsigned i = products_given / 2, j = products_given % 2;
    __CPROVER_assert(x == halves_a[i] && y == halves_b[j], "word_product: the next two halves");
    products_given++;
    mo_u128 p = nondet_u128();
    __CPROVER_assume(p <= product_bound(x, y));
    products[i][j] = p;
    return p;
}

/* multiply, for any two 128-bit magnitudes: the sum of the products of their 64-bit halves,
 * each shifted into place, which is their exact product. */
void harness_multiply(void) {
    mo_u128 a = nondet_u128(), b = nondet_u128();
    halves_a[0] = (uint64_t)a;
    halves_a[1] = (uint64_t)(a >> 64);
    halves_b[0] = (uint64_t)b;
    halves_b[1] = (uint64_t)(b >> 64);
    struct wide product = multiply(a, b);
    __CPROVER_assert(products_given == 4, "multiply: every product of halves");
    u256 sum = (u256)products[0][0] + ((u256)products[0][1] << 64) + ((u256)products[1][0] << 64) +
               ((u256)products[1][1] << 128);
    __CPROVER_assert(value(product) == sum, "multiply: the exact product");
    reached_end();
}

/* Callers rely on nothing about the product. */
struct wide model_multiply(mo_u128 a, mo_u128 b) {
    (void)a;
    (void)b;
    return any_wide();
}

/* ---- One step of the division -------------------------------------------------------------- */

/* divide_step, for any remainder below any denominator below 2^127, and either next bit. */
void harness_divide_step(void) {
    mo_u128 denominator = nondet_u128(), remainder = nondet_u128();
    bool bit = nondet_bool();
    __CPROVER_assume(denominator < LIMIT && remainder < denominator);
    u260 before = (u260)remainder * 2 + (bit ? 1U : 0U);
    bool subtracted = divide_step(&remainder, bit, denominator);
    __CPROVER_assert(remainder < denominator, "divide_step: the remainder stays below");
    __CPROVER_assert(before == (u260)remainder + (subtracted ? (u260)denominator : 0U),
                     "divide_step: twice the remainder plus the bit is the quotient bit times "
                     "the denominator plus the new remainder");
    reached_end();
}

/* ---- Rounding -------------------------------------------------------------------------------- */

/* round_quotient, for any quotient and any remainder below any denominator below 2^127. */
void harness_round_quotient(void) {
    struct wide quotient = any_wide();
    mo_u128 denominator = nondet_u128(), remainder = nondet_u128();
    bool negative = nondet_bool();
    __CPROVER_assume(denominator < LIMIT && remainder < denominator);
    u260 twice = (u260)remainder * 2, q = value(quotient);
    bool up = twice > denominator || (twice == denominator && (q & 1) != 0);
    u260 rounded_value = q + (up ? 1U : 0U);
    mo_i128 out = nondet_i128(), before = out;
    if (round_quotient(quotient, remainder, denominator, negative, &out)) {
        __CPROVER_assert(rounded_value < LIMIT, "round_quotient: a result inside the range");
        mo_i128 magnitude = (mo_i128)(mo_u128)rounded_value;
        __CPROVER_assert(out == (negative ? -magnitude : magnitude),
                         "round_quotient: to nearest, ties to even, with the sign");
    } else {
        __CPROVER_assert(rounded_value >= LIMIT, "round_quotient: refuses only outside the range");
        __CPROVER_assert(out == before, "round_quotient: a refusal leaves out alone");
    }
    reached_end();
}

bool model_round_quotient(struct wide quotient, mo_u128 remainder, mo_u128 denominator,
                          bool negative, mo_i128 *out) {
    __CPROVER_assert(out != NULL && remainder < denominator && denominator < LIMIT,
                     "round_quotient: a remainder below a denominator below 2^127");
    (void)negative;
    bool ok = nondet_bool();
    if (ok) {
        *out = nondet_i128();
        __CPROVER_assume(*out >= -DECIMAL_MAX);
    }
    return ok;
}

/* ---- The division ------------------------------------------------------------------------- */

/* rounded is not proved here: its 256 steps each require the remainder below the denominator,
 * and CBMC's solver takes time exponential in the number of steps to carry that through (eight
 * steps already take over a minute). Its loop is argued in cbmc/README.md from divide_step's
 * proof. Callers rely on what rounded requires, and on a refusal leaving out alone. */
bool model_rounded(struct wide numerator, mo_u128 denominator, bool negative, mo_i128 *out) {
    __CPROVER_assert(out != NULL && denominator != 0 && denominator < LIMIT,
                     "rounded: a denominator from 1 to 2^127 - 1");
    return model_round_quotient((struct wide){0}, 0, 1, negative, out);
}

/* ---- Decimal * and / ------------------------------------------------------------------------ */

static mo_i128 any_decimal(void) {
    mo_i128 d = nondet_i128();
    __CPROVER_assume(d >= -DECIMAL_MAX);
    return d;
}

/* mo_decimal_mul, for any two Decimals. */
void harness_decimal_mul(void) {
    mo_i128 a = any_decimal(), b = any_decimal(), out = nondet_i128(), before = out;
    if (!mo_decimal_mul(a, b, &out)) {
        __CPROVER_assert(out == before, "decimal_mul: a refusal leaves out alone");
    }
    reached_end();
}

/* mo_decimal_div, for any Decimal over any nonzero Decimal. */
void harness_decimal_div(void) {
    mo_i128 a = any_decimal(), b = any_decimal(), out = nondet_i128(), before = out;
    __CPROVER_assume(b != 0);
    if (!mo_decimal_div(a, b, &out)) {
        __CPROVER_assert(out == before, "decimal_div: a refusal leaves out alone");
    }
    reached_end();
}

#pragma CPROVER check pop
