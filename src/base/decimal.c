#include "base/decimal.h"

#include <stddef.h>
#include <stdint.h>

struct wide {
    uint64_t words[4];
};

static MO_MUST_USE mo_u128 magnitude(mo_i128 value) {
    const mo_i128 maximum = (mo_i128)(((mo_u128)1 << 127) - 1);
    MO_ASSERT(value >= -maximum);
    return value < 0 ? (mo_u128)(-value) : (mo_u128)value;
}

static MO_MUST_USE struct wide multiply(mo_u128 a, mo_u128 b) {
    uint64_t x[] = {(uint64_t)a, (uint64_t)(a >> 64)};
    uint64_t y[] = {(uint64_t)b, (uint64_t)(b >> 64)};
    struct wide out = {0};
    for (unsigned i = 0; i < 2; i++) {
        mo_u128 carry = 0;
        for (unsigned j = 0; j < 2; j++) {
            mo_u128 product = (mo_u128)x[i] * y[j] + out.words[i + j] + carry;
            out.words[i + j] = (uint64_t)product;
            carry = product >> 64;
        }
        out.words[i + 2] = (uint64_t)carry;
    }
    return out;
}

/* The denominator fits the positive Decimal range, so doubling the remainder
 * fits in unsigned 128 bits. Do not round partial products. */
static MO_MUST_USE bool rounded(struct wide numerator, mo_u128 denominator, bool negative,
                                mo_i128 *out) {
    MO_ASSERT(out != NULL && denominator != 0 && denominator < (mo_u128)1 << 127);
    struct wide quotient = {0};
    mo_u128 remainder = 0;
    for (unsigned bit = 256; bit != 0;) {
        bit--;
        remainder = (remainder << 1) | ((numerator.words[bit / 64] >> (bit % 64)) & 1);
        if (remainder >= denominator) {
            remainder -= denominator;
            quotient.words[bit / 64] |= UINT64_C(1) << (bit % 64);
        }
    }
    if (remainder * 2 > denominator ||
        (remainder * 2 == denominator && (quotient.words[0] & 1) != 0)) {
        for (unsigned i = 0; i < 4; i++) {
            quotient.words[i]++;
            if (quotient.words[i] != 0)
                break;
            MO_ASSERT(i != 3);
        }
    }
    if (quotient.words[2] != 0 || quotient.words[3] != 0 || quotient.words[1] >> 63 != 0)
        return false;
    mo_i128 value = (mo_i128)(((mo_u128)quotient.words[1] << 64) | quotient.words[0]);
    *out = negative ? -value : value;
    return true;
}

bool mo_decimal_mul(mo_i128 left, mo_i128 right, mo_i128 *out) {
    return rounded(multiply(magnitude(left), magnitude(right)), UINT64_C(1000000000000000000),
                   (left < 0) != (right < 0), out);
}

bool mo_decimal_div(mo_i128 left, mo_i128 right, mo_i128 *out) {
    return rounded(multiply(magnitude(left), UINT64_C(1000000000000000000)), magnitude(right),
                   (left < 0) != (right < 0), out);
}
