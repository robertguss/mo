/* CBMC proofs of Int's and Decimal's primitives, src/prim/number.c and Int.pow in
 * src/prim/prim.c (V5 part 3, MO-55).
 *
 * Int is a 64-bit integer that never wraps silently: an operation whose result does not fit
 * stops the program (reference 13.2), except the wrapping_, saturating_ and checked_ families,
 * which say what they do instead. The proofs show, for every Int, every Decimal and every
 * primitive of each family, that the primitive gives the reference's result, computed here in
 * wider arithmetic, or refuses exactly when the reference says it does, and that it never
 * overflows, divides by zero, shifts out of range or fails an MO_ASSERT on the way.
 *
 * The heap is not proved here. Its builders are replaced by models that check what they are
 * given (an Option's case and value, a BigInt's sign and bytes) and may run out of memory.
 * Printing (Int.to_text, Decimal.to_text, Decimal.to_fixed's digits) is replaced by a model that
 * may give any text, and Float.to_decimal, which reads printed digits, is left to the tests.
 *
 * number.c and prim.c are included rather than linked, so that their static functions can be
 * proved. */
#include "prim/number.c"
#include "heap/cell.c"
#include "prim/prim.c"

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

int64_t nondet_i64(void);
mo_i128 nondet_i128(void);
unsigned nondet_unsigned(void);
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

/* ---- The heap ------------------------------------------------------------------------------ */

/* The last value a builder made, so a harness can check it. */
static struct {
    bool built;
    uint32_t tag;
    struct mo_cell field;
    enum mo_bigint_sign sign;
    uint8_t magnitude[8];
    size_t length;
} made;

/* Every builder may run out of memory, and then leaves out alone. */
static bool allocated(struct mo_cell *out) {
    if (nondet_bool()) {
        return false;
    }
    *out = mo_cell_ref((struct mo_object *)&made);
    made.built = true;
    return true;
}

/* An Option: case 0 is None, with no fields, and case 1 is Some, with one. */
bool mo_value_case(struct mo_heap *process, uint32_t type, uint32_t tag,
                   const struct mo_cell *fields, uint32_t count, struct mo_cell *out) {
    (void)process;
    (void)type;
    __CPROVER_assert(tag <= 1 && count == tag && (count == 0 || fields != NULL),
                     "an Option's case and fields");
    if (!allocated(out)) {
        return false;
    }
    made.tag = tag;
    if (count != 0) {
        made.field = fields[0];
    }
    return true;
}

bool mo_value_bigint(struct mo_heap *process, enum mo_bigint_sign sign, const uint8_t *magnitude,
                     size_t length, struct mo_cell *out) {
    (void)process;
    __CPROVER_assert(length <= 8 && (length == 0 || __CPROVER_r_ok(magnitude, length)),
                     "a BigInt's magnitude bytes");
    if (!allocated(out)) {
        return false;
    }
    made.sign = sign;
    made.length = length;
    for (size_t i = 0; i < length; i++) {
        made.magnitude[i] = magnitude[i];
    }
    return true;
}

bool mo_value_text(struct mo_heap *process, const char *utf8, size_t length, struct mo_cell *out) {
    (void)process;
    __CPROVER_assert(length == 0 || __CPROVER_r_ok(utf8, length), "a text's bytes");
    return allocated(out);
}

/* Printing is not proved here: it may succeed, with any text, or run out of memory. */
size_t nondet_size(void);
void mo_scratch_init(struct mo_scratch *scratch, struct mo_heap *process) {
    scratch->arena = (struct mo_arena){.limit = nondet_size()};
    scratch->process = process;
}
void mo_scratch_free(struct mo_scratch *scratch) {
    (void)scratch;
}
void mo_buffer_init(struct mo_buffer *buffer, size_t limit) {
    *buffer = (struct mo_buffer){.limit = limit};
}
void mo_buffer_free(struct mo_buffer *buffer) {
    (void)buffer;
}
bool mo_value_print(struct mo_heap *process, uint32_t type, struct mo_cell value,
                    struct mo_buffer *out) {
    (void)process;
    (void)type;
    (void)value;
    (void)out;
    return nondet_bool();
}

/* C's truncating division is not proved here (CBMC is very slow to reason about a divider):
 * its model gives any quotient and remainder with the properties C's have, and records them for
 * the harness. truncated_division's own proof shows it divides only where C allows. */
static struct { int64_t q, r; } truncated;

void model_truncated_division(int64_t a, int64_t b, int64_t *q, int64_t *r) {
    __CPROVER_assert(b != 0 && b != -1, "truncated_division: a divisor other than 0 and -1");
    truncated.q = nondet_i64();
    truncated.r = nondet_i64();
    mo_i128 magnitude_b = b < 0 ? -(mo_i128)b : b,
            magnitude_r = truncated.r < 0 ? -(mo_i128)truncated.r : truncated.r;
    /* The remainder is below the divisor and has a's sign; with |b| >= 2 the quotient is at
     * most 2^62 either way. */
    __CPROVER_assume(magnitude_r < magnitude_b &&
                     (truncated.r == 0 || (truncated.r < 0) == (a < 0)));
    __CPROVER_assume(b == 1
                         ? truncated.q == a && truncated.r == 0
                         : truncated.q >= -(INT64_C(1) << 62) && truncated.q <= INT64_C(1) << 62);
    *q = truncated.q;
    *r = truncated.r;
}

/* Nor is C's 128-bit multiplication: showing the harness's product equal to the primitive's
 * means showing two multipliers equal, which CBMC does not finish. mo_prim_exact_product's model
 * gives any value its proof allows, and records it, so the harness checks what each primitive does
 * with the product. */
static mo_i128 product_given;
static int64_t product_of[2];
static unsigned products_given;

mo_i128 model_mo_prim_exact_product(int64_t a, int64_t b) {
    product_of[0] = a;
    product_of[1] = b;
    products_given++;
    product_given = nondet_i128();
    __CPROVER_assume(product_given >= -((mo_i128)1 << 126) && product_given <= (mo_i128)1 << 126);
    return product_given;
}

/* ---- A call ------------------------------------------------------------------------------ */

static struct mo_type option_type = {.kind = MO_TYPE_CHOICE};
static struct mo_types types = {.items = &option_type, .count = 1, .finished = true};
static uint32_t params[2];

/* A call of the primitive id on a and b (b only when it takes two), returning an Option when it
 * returns one. */
static struct mo_prim_call call_of(enum mo_primitive_id id, struct mo_cell *args) {
    return (struct mo_prim_call){.types = &types, .id = id, .args = args, .params = params};
}

static enum mo_primitive_id any_id(enum mo_primitive_id first, enum mo_primitive_id last) {
    enum mo_primitive_id id = (enum mo_primitive_id)nondet_unsigned();
    __CPROVER_assume(id >= first && id <= last);
    return id;
}

static bool fits(mo_i128 n) {
    return n >= INT64_MIN && n <= INT64_MAX;
}

/* status is a success with the Int n, or, when n does not fit, an overflow. */
static bool int_or_overflow(enum mo_prim_status status, struct mo_cell out, mo_i128 n) {
    return fits(n) ? status == MO_PRIM_RETURNED && mo_cell_get_int(out) == n
                   : status == MO_PRIM_OVERFLOW;
}

/* status is an Option: Some with value when some, else None, or running out of memory. */
static bool option(enum mo_prim_status status, bool some, struct mo_cell value) {
    if (status == MO_PRIM_OUT_OF_MEMORY) {
        return true;
    }
    return status == MO_PRIM_RETURNED && made.built && made.tag == (some ? 1U : 0U) &&
           (!some || memcmp(made.field.bytes, value.bytes, sizeof value.bytes) == 0);
}

/* ---- Int ------------------------------------------------------------------------------------ */

/* The primitives a run proves; -DMO_CBMC_FIRST=id -DMO_CBMC_LAST=id proves one, for a quick
 * check of a change. */
#ifndef MO_CBMC_FIRST
#define MO_CBMC_FIRST 0
#endif
#ifndef MO_CBMC_LAST
#define MO_CBMC_LAST MO_PRIM_COUNT
#endif

static enum mo_primitive_id any_of(enum mo_primitive_id first, enum mo_primitive_id last) {
    return any_id(first > MO_CBMC_FIRST ? first : MO_CBMC_FIRST,
                  last < MO_CBMC_LAST ? last : MO_CBMC_LAST);
}

/* Every Int primitive but pow, on any two Ints. */
void harness_int_primitive(void) {
    enum mo_primitive_id id = any_of(MO_PRIM_INT_ABS, MO_PRIM_INT_DAYS);
    __CPROVER_assume(id != MO_PRIM_INT_POW);
    int64_t a = nondet_i64(), b = nondet_i64();
    struct mo_cell args[2] = {mo_cell_int(a), mo_cell_int(b)};
    struct mo_prim_call call = call_of(id, args);
    struct mo_cell out = {0};
    enum mo_prim_status status = mo_prim_number(&call, &out);
    mo_i128 x = a, y = b, sum = x + y, difference = x - y, product = product_given;
    /* The one product a primitive multiplies is of a and this. */
    int64_t factor = id == MO_PRIM_INT_TO_DECIMAL     ? INT64_C(1000000000000000000)
                     : id == MO_PRIM_INT_SECONDS      ? 1000000
                     : id == MO_PRIM_INT_MILLISECONDS ? 1000
                     : id == MO_PRIM_INT_MINUTES      ? 60000000
                     : id == MO_PRIM_INT_HOURS        ? INT64_C(3600000000)
                     : id == MO_PRIM_INT_DAYS         ? INT64_C(86400000000)
                                                      : b;
    bool multiplies = id == MO_PRIM_INT_WRAPPING_MUL || id == MO_PRIM_INT_SATURATING_MUL ||
                      id == MO_PRIM_INT_CHECKED_MUL || id == MO_PRIM_INT_TO_DECIMAL ||
                      (id >= MO_PRIM_INT_SECONDS && id <= MO_PRIM_INT_DAYS);
    __CPROVER_assert(multiplies
                         ? products_given == 1 && product_of[0] == a && product_of[1] == factor
                         : products_given == 0,
                     "Int primitive: multiplies a by the right factor, once");
    uint64_t ua = (uint64_t)a, ub = (uint64_t)b;
    bool ok = true;
    switch ((unsigned)id) {
    case MO_PRIM_INT_ABS:
        ok = int_or_overflow(status, out, a < 0 ? -x : x);
        break;
    case MO_PRIM_INT_MIN:
    case MO_PRIM_INT_MAX:
        ok = status == MO_PRIM_RETURNED &&
             mo_cell_get_int(out) == (id == MO_PRIM_INT_MIN ? (a < b ? a : b) : (a > b ? a : b));
        break;
    case MO_PRIM_INT_FLOOR_DIV:
    case MO_PRIM_INT_FLOOR_MOD:
        if (b == 0) {
            ok = status == MO_PRIM_DIV_BY_ZERO;
        } else if (b == -1) {
            ok = id == MO_PRIM_INT_FLOOR_DIV
                     ? int_or_overflow(status, out, -x)
                     : status == MO_PRIM_RETURNED && mo_cell_get_int(out) == 0;
        } else {
            /* From C's truncating quotient and remainder, the floor: one less, and the
             * remainder moved by b, when the remainder is not zero and its sign is not b's. */
            bool step = truncated.r != 0 && (truncated.r < 0) != (b < 0);
            mo_i128 q = (mo_i128)truncated.q - (step ? 1 : 0), m = truncated.r + (step ? y : 0);
            ok = status == MO_PRIM_RETURNED &&
                 mo_cell_get_int(out) == (id == MO_PRIM_INT_FLOOR_DIV ? q : m) &&
                 (b > 0 ? m >= 0 && m < y : m <= 0 && m > y);
        }
        break;
    case MO_PRIM_INT_WRAPPING_ADD:
        ok = status == MO_PRIM_RETURNED && mo_cell_get_int(out) == (int64_t)(ua + ub);
        break;
    case MO_PRIM_INT_WRAPPING_SUB:
        ok = status == MO_PRIM_RETURNED && mo_cell_get_int(out) == (int64_t)(ua - ub);
        break;
    case MO_PRIM_INT_WRAPPING_MUL:
        ok = status == MO_PRIM_RETURNED && mo_cell_get_int(out) == (int64_t)product;
        break;
    case MO_PRIM_INT_SATURATING_ADD:
    case MO_PRIM_INT_SATURATING_SUB:
    case MO_PRIM_INT_SATURATING_MUL: {
        mo_i128 n = id == MO_PRIM_INT_SATURATING_ADD   ? sum
                    : id == MO_PRIM_INT_SATURATING_SUB ? difference
                                                       : product;
        ok = status == MO_PRIM_RETURNED && mo_cell_get_int(out) == (n > INT64_MAX   ? INT64_MAX
                                                                    : n < INT64_MIN ? INT64_MIN
                                                                                    : n);
        break;
    }
    case MO_PRIM_INT_CHECKED_ADD:
    case MO_PRIM_INT_CHECKED_SUB:
    case MO_PRIM_INT_CHECKED_MUL: {
        mo_i128 n = id == MO_PRIM_INT_CHECKED_ADD   ? sum
                    : id == MO_PRIM_INT_CHECKED_SUB ? difference
                                                    : product;
        ok = option(status, fits(n), mo_cell_int((int64_t)n));
        break;
    }
    case MO_PRIM_INT_TO_DECIMAL:
        ok = status == MO_PRIM_RETURNED && mo_cell_get_decimal(out) == product;
        break;
    case MO_PRIM_INT_TO_FLOAT:
        /* C's conversion rounds to nearest, ties to even, as Float's does (reference 3.2). */
        ok = status == MO_PRIM_RETURNED && mo_cell_get_float(out) == (double)a;
        break;
    case MO_PRIM_INT_TO_BIG: {
        if (status == MO_PRIM_OUT_OF_MEMORY) {
            break;
        }
        /* The magnitude's bytes, least significant first, with no zero byte at the top. */
        mo_u128 magnitude = (mo_u128)(a < 0 ? -x : x);
        ok = status == MO_PRIM_RETURNED && made.built &&
             made.sign == (a == 0  ? MO_BIGINT_ZERO
                           : a < 0 ? MO_BIGINT_NEGATIVE
                                   : MO_BIGINT_POSITIVE) &&
             (made.length == 0 ? magnitude == 0 : made.magnitude[made.length - 1] != 0);
        for (size_t i = 0; i < made.length; i++) {
            ok = ok && made.magnitude[i] == (uint8_t)(magnitude >> (8 * i));
        }
        ok = ok && (made.length == 8 || magnitude >> (8 * made.length) == 0);
        break;
    }
    case MO_PRIM_INT_TO_TEXT:
        ok = status == MO_PRIM_RETURNED || status == MO_PRIM_OUT_OF_MEMORY;
        break;
    case MO_PRIM_INT_TO_BYTE:
        ok = option(status, a >= 0 && a <= 255, mo_cell_byte((uint8_t)a));
        break;
    case MO_PRIM_INT_TO_CHAR: {
        bool scalar = a >= 0 && a <= 0x10ffff && (a < 0xd800 || a > 0xdfff);
        ok = option(status, scalar, mo_cell_char(scalar ? (uint32_t)a : 0));
        break;
    }
    case MO_PRIM_INT_SECONDS:
    case MO_PRIM_INT_MILLISECONDS:
    case MO_PRIM_INT_MINUTES:
    case MO_PRIM_INT_HOURS:
    case MO_PRIM_INT_DAYS: {
        /* A Duration counts microseconds. */
        mo_i128 n = product;
        ok = fits(n) ? status == MO_PRIM_RETURNED && mo_cell_get_duration(out) == n
                     : status == MO_PRIM_OVERFLOW;
        break;
    }
    default:
        ok = false;
    }
    __CPROVER_assert(ok, "Int primitive: the reference's result");
    __CPROVER_assert(status == MO_PRIM_RETURNED || status == MO_PRIM_OUT_OF_MEMORY ||
                         call.reason != NULL,
                     "Int primitive: a refusal has its reason");
    reached_end();
}

/* mo_prim_exact_product, for any two Ints: at most 2^126 either way. */
void harness_exact_product(void) {
    int64_t a = nondet_i64(), b = nondet_i64();
    mo_i128 p = mo_prim_exact_product(a, b);
    __CPROVER_assert(p >= -((mo_i128)1 << 126) && p <= (mo_i128)1 << 126,
                     "mo_prim_exact_product: at most 2^126 either way");
    reached_end();
}

/* truncated_division, for any Int over any Int but 0 and -1: no overflow, and the properties
 * its model assumes. */
void harness_truncated_division(void) {
    int64_t a = nondet_i64(), b = nondet_i64(), q, r;
    __CPROVER_assume(b != 0 && b != -1);
    truncated_division(a, b, &q, &r);
    mo_i128 magnitude_b = b < 0 ? -(mo_i128)b : b, magnitude_r = r < 0 ? -(mo_i128)r : r;
    __CPROVER_assert(magnitude_r < magnitude_b && (r == 0 || (r < 0) == (a < 0)),
                     "truncated_division: a remainder below the divisor, with a's sign");
    __CPROVER_assert(b == 1 ? q == a && r == 0 : q >= -(INT64_C(1) << 62) && q <= INT64_C(1) << 62,
                     "truncated_division: a quotient of at most 2^62 either way");
    reached_end();
}

/* prim.c's checked multiplication, for any two Ints, with mo_prim_exact_product replaced by its
 * model: it multiplies a by b, refuses exactly when the product does not fit, leaving out alone,
 * and otherwise gives the product. */
void harness_int_multiply(void) {
    int64_t a = nondet_i64(), b = nondet_i64(), out = nondet_i64(), before = out;
    bool ok = multiply(a, b, &out);
    __CPROVER_assert(products_given == 1 && product_of[0] == a && product_of[1] == b,
                     "multiply: multiplies a by b, once");
    __CPROVER_assert(ok == fits(product_given), "multiply: refuses exactly when it does not fit");
    __CPROVER_assert(ok ? out == product_given : out == before,
                     "multiply: the product, or out left alone");
    reached_end();
}

/* multiply's model: any product, or a refusal, which Int.pow's proof counts. */
static unsigned multiplications, refusals;

bool model_multiply(int64_t a, int64_t b, int64_t *out) {
    (void)a;
    (void)b;
    multiplications++;
    if (nondet_bool()) {
        refusals++;
        return false;
    }
    *out = nondet_i64();
    return true;
}

/* Int.pow, for any Int raised to any Int, with multiply replaced by its model: a negative
 * exponent is refused, a zero one gives 1 without multiplying, at most two multiplications run
 * for each of the exponent's 63 bits, and the result overflows exactly when one of them does.
 * The value of a^n, and which multiplications run, are left to tests/test_prim.c. */
void harness_int_pow(void) {
    int64_t a = nondet_i64(), n = nondet_i64();
    struct mo_cell args[2] = {mo_cell_int(a), mo_cell_int(n)};
    struct mo_cell out = {0};
    const char *reason = NULL;
    enum mo_prim_status status = power(args, &out, &reason);
    if (n < 0) {
        __CPROVER_assert(status == MO_PRIM_OUTSIDE && reason != NULL,
                         "Int.pow: a negative exponent is refused");
    } else if (n == 0) {
        __CPROVER_assert(status == MO_PRIM_RETURNED && mo_cell_get_int(out) == 1 &&
                             multiplications == 0,
                         "Int.pow: a^0 is 1, without multiplying");
    } else {
        __CPROVER_assert(multiplications <= 2 * 63, "Int.pow: two multiplications a bit");
        __CPROVER_assert(refusals == 0
                             ? status == MO_PRIM_RETURNED
                             : refusals == 1 && status == MO_PRIM_OVERFLOW && reason != NULL,
                         "Int.pow: overflows exactly when a multiplication does");
    }
    reached_end();
}

#pragma CPROVER check pop
