/* Exact shortest binary64 digits, following Support/Num.lean. All arithmetic
 * fits in 1,536 bits; no locale, floating arithmetic or libc conversion. */
#include "heap/internal.h"

#include <string.h>

enum { FLOAT_WORDS = 48 };
struct float_nat {
    uint32_t words[FLOAT_WORDS];
    unsigned length;
};

static void nat_set(struct float_nat *out, uint64_t value, unsigned shift) {
    *out = (struct float_nat){0};
    unsigned offset = shift / 32, bits = shift % 32;
    mo_u128 wide = (mo_u128)value << bits;
    while (wide != 0) {
        MO_ASSERT(offset < FLOAT_WORDS);
        out->words[offset++] = (uint32_t)wide;
        wide >>= 32;
    }
    out->length = offset;
    if (value == 0) {
        out->length = 0;
    }
}

static MO_MUST_USE int nat_compare(const struct float_nat *a, const struct float_nat *b) {
    if (a->length != b->length) {
        return a->length < b->length ? -1 : 1;
    }
    for (unsigned i = a->length; i != 0; i--) {
        if (a->words[i - 1] != b->words[i - 1]) {
            return a->words[i - 1] < b->words[i - 1] ? -1 : 1;
        }
    }
    return 0;
}

static void nat_scale(struct float_nat *out, const struct float_nat *a, uint64_t scale) {
    struct float_nat result = {0};
    mo_u128 carry = 0;
    for (unsigned i = 0; i < a->length; i++) {
        carry += (mo_u128)a->words[i] * scale;
        result.words[i] = (uint32_t)carry;
        carry >>= 32;
    }
    result.length = a->length;
    while (carry != 0) {
        MO_ASSERT(result.length < FLOAT_WORDS);
        result.words[result.length++] = (uint32_t)carry;
        carry >>= 32;
    }
    while (result.length != 0 && result.words[result.length - 1] == 0) {
        result.length--;
    }
    *out = result;
}

static void nat_multiply(struct float_nat *out, const struct float_nat *a,
                         const struct float_nat *b) {
    *out = (struct float_nat){0};
    MO_ASSERT(a->length + b->length <= FLOAT_WORDS);
    if (a->length == 0 || b->length == 0) {
        return;
    }
    for (unsigned i = 0; i < a->length; i++) {
        uint64_t carry = 0;
        for (unsigned j = 0; j < b->length; j++) {
            carry += (uint64_t)a->words[i] * b->words[j] + out->words[i + j];
            out->words[i + j] = (uint32_t)carry;
            carry >>= 32;
        }
        out->words[i + b->length] = (uint32_t)carry;
    }
    out->length = a->length + b->length;
    while (out->length != 0 && out->words[out->length - 1] == 0) {
        out->length--;
    }
}

static void binary_parts(uint64_t bits, uint64_t *mantissa, int *exponent) {
    unsigned e = (unsigned)(bits >> 52);
    *mantissa = bits & UINT64_C(0x000fffffffffffff);
    *exponent = e == 0 ? -1074 : (int)e - 1075;
    if (e == 2047) {
        /* The virtual successor of the largest finite value is 2^1024. */
        *mantissa = 1;
        *exponent = 1024;
    } else if (e != 0) {
        *mantissa |= UINT64_C(1) << 52;
    }
}

static void binary_ratio(uint64_t mantissa, int exponent, struct float_nat *n,
                         struct float_nat *d) {
    nat_set(n, mantissa, exponent > 0 ? (unsigned)exponent : 0);
    nat_set(d, 1, exponent < 0 ? (unsigned)-exponent : 0);
}

static void midpoint(uint64_t a, uint64_t b, struct float_nat *n, struct float_nat *d) {
    uint64_t am, bm;
    int ae, be;
    binary_parts(a, &am, &ae);
    binary_parts(b, &bm, &be);
    int e = ae < be ? ae : be;
    MO_ASSERT(ae - e <= 53 && be - e <= 53);
    uint64_t m = (am << (unsigned)(ae - e)) + (bm << (unsigned)(be - e));
    binary_ratio(m, e - 1, n, d);
}

static MO_MUST_USE uint64_t nearest(const struct float_nat *n, const struct float_nat *d) {
    uint64_t low = 0, high = UINT64_C(1000000000000000000);
    struct float_nat product;
    while (low + 1 < high) {
        uint64_t middle = low + (high - low) / 2;
        nat_scale(&product, d, middle);
        if (nat_compare(&product, n) <= 0) {
            low = middle;
        } else {
            high = middle;
        }
    }
    struct float_nat twice;
    nat_scale(&twice, n, 2);
    nat_scale(&product, d, 2 * low + 1);
    int compare = nat_compare(&twice, &product);
    return low + (compare > 0 || (compare == 0 && low % 2 != 0) ? 1 : 0);
}

static MO_MUST_USE bool round_trips(uint64_t coefficient, int exponent, uint64_t bits,
                                    const struct float_nat *lower_n,
                                    const struct float_nat *lower_d,
                                    const struct float_nat *upper_n,
                                    const struct float_nat *upper_d) {
    struct float_nat n, d, left, right;
    nat_set(&n, coefficient, 0);
    nat_set(&d, 1, 0);
    for (int i = 0; i < exponent; i++) {
        nat_scale(&n, &n, 10);
    }
    for (int i = 0; i > exponent; i--) {
        nat_scale(&d, &d, 10);
    }
    nat_multiply(&left, &n, lower_d);
    nat_multiply(&right, lower_n, &d);
    int lower = nat_compare(&left, &right);
    nat_multiply(&left, &n, upper_d);
    nat_multiply(&right, upper_n, &d);
    int upper = nat_compare(&left, &right);
    bool even = bits % 2 == 0;
    return (lower > 0 || (lower == 0 && even)) && (upper < 0 || (upper == 0 && even));
}

bool mo_float_text(double value, struct mo_buffer *out) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    uint64_t magnitude = bits & UINT64_C(0x7fffffffffffffff);
    if (magnitude > UINT64_C(0x7ff0000000000000)) {
        return mo_buffer_append_text(out, "nan");
    }
    if (bits >> 63 != 0 && !mo_buffer_append_byte(out, '-')) {
        return false;
    }
    if (magnitude == UINT64_C(0x7ff0000000000000)) {
        return mo_buffer_append_text(out, "inf");
    }
    if (magnitude == 0) {
        return mo_buffer_append_byte(out, '0');
    }
    struct float_nat n, d, lower_n, lower_d, upper_n, upper_d;
    uint64_t mantissa;
    int binary_exponent;
    binary_parts(magnitude, &mantissa, &binary_exponent);
    binary_ratio(mantissa, binary_exponent, &n, &d);
    midpoint(magnitude - 1, magnitude, &lower_n, &lower_d);
    midpoint(magnitude, magnitude + 1, &upper_n, &upper_d);
    int k = 0;
    if (nat_compare(&n, &d) < 0) {
        do {
            nat_scale(&n, &n, 10);
            k--;
        } while (nat_compare(&n, &d) < 0);
    } else {
        struct float_nat next;
        for (;;) {
            nat_scale(&next, &d, 10);
            if (nat_compare(&n, &next) < 0) {
                break;
            }
            d = next;
            k++;
        }
    }
    uint64_t coefficient = 0;
    int exponent = k;
    bool found = false;
    for (unsigned digits = 1; digits <= 17 && !found; digits++) {
        uint64_t q = nearest(&n, &d);
        uint64_t candidates[] = {q, q == 0 ? 0 : q - 1, q + 1};
        exponent = k - (int)digits + 1;
        for (unsigned i = 0; i < 3; i++) {
            if (round_trips(candidates[i], exponent, magnitude, &lower_n, &lower_d, &upper_n,
                            &upper_d)) {
                coefficient = candidates[i];
                found = true;
                break;
            }
        }
        if (!found) {
            nat_scale(&n, &n, 10);
        }
    }
    MO_ASSERT(found && coefficient != 0);
    while (coefficient % 10 == 0) {
        coefficient /= 10;
        exponent++;
    }
    char digits[20];
    unsigned start = sizeof digits;
    do {
        digits[--start] = (char)('0' + coefficient % 10);
        coefficient /= 10;
    } while (coefficient != 0);
    unsigned length = (unsigned)sizeof digits - start;
    int point = (int)length + exponent;
    if (point < -5 || point > 21) {
        return mo_buffer_append(out, digits + start, 1) &&
               (length == 1 || (mo_buffer_append_byte(out, '.') &&
                                mo_buffer_append(out, digits + start + 1, length - 1))) &&
               mo_buffer_append_byte(out, 'e') && mo_buffer_append_int(out, point - 1);
    }
    if (point <= 0) {
        if (!mo_buffer_append_text(out, "0.")) {
            return false;
        }
        for (int i = 0; i > point; i--) {
            if (!mo_buffer_append_byte(out, '0')) {
                return false;
            }
        }
        return mo_buffer_append(out, digits + start, length);
    }
    for (unsigned i = 0; i < length || (int)i < point; i++) {
        if ((int)i == point && !mo_buffer_append_byte(out, '.')) {
            return false;
        }
        if (!mo_buffer_append_byte(out, i < length ? (uint8_t)digits[start + i] : '0')) {
            return false;
        }
    }
    return true;
}
