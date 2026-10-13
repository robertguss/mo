#define _POSIX_C_SOURCE 200809L
#include "prim/internal.h"

#include <fenv.h>
#include <locale.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static MO_MUST_USE bool digit(uint8_t c) {
    return c >= '0' && c <= '9';
}

static MO_MUST_USE bool decimal(const uint8_t *s, size_t length, mo_u128 maximum, bool fractional,
                                mo_i128 *out) {
    size_t at = length != 0 && s[0] == '-' ? 1U : 0U, start = at;
    mo_u128 n = 0;
    while (at < length && digit(s[at])) {
        unsigned value = s[at++] - '0';
        if (n > (maximum - value) / 10)
            return false;
        n = n * 10 + value;
    }
    if (at == start)
        return false;
    if (fractional) {
        unsigned places = 0;
        if (at < length && s[at] == '.') {
            start = ++at;
            while (at < length && digit(s[at]) && places < 18) {
                unsigned value = s[at++] - '0';
                if (n > (maximum - value) / 10)
                    return false;
                n = n * 10 + value;
                places++;
            }
            if (at == start)
                return false;
        }
        for (; places < 18; places++) {
            if (n > maximum / 10)
                return false;
            n *= 10;
        }
    }
    if (at != length)
        return false;
    /* The only 2^63 magnitude is a negative Int, not a positive Int. */
    if (!fractional && n == (mo_u128)INT64_MAX + 1 && s[0] != '-')
        return false;
    *out = s[0] == '-' ? -(mo_i128)n : (mo_i128)n;
    return true;
}

static MO_MUST_USE enum mo_prim_status floating(struct mo_prim_call *c, const uint8_t *s,
                                                size_t length, struct mo_cell *out) {
    double value;
    if (length == 3 && memcmp(s, "nan", 3) == 0) {
        uint64_t bits = UINT64_C(0x7ff8000000000000);
        memcpy(&value, &bits, sizeof value);
        return mo_prim_option(c, true, mo_cell_float(value), out);
    }
    if ((length == 3 && memcmp(s, "inf", 3) == 0) || (length == 4 && memcmp(s, "-inf", 4) == 0))
        return mo_prim_option(c, true, mo_cell_float(length == 3 ? INFINITY : -INFINITY), out);
    size_t at = length != 0 && s[0] == '-' ? 1U : 0U, start = at, fraction = 0;
    bool negative = at != 0, nonzero = false;
    size_t significant = 0;
    while (at < length && digit(s[at])) {
        nonzero = nonzero || s[at] != '0';
        significant += nonzero ? 1U : 0U;
        at++;
    }
    if (at == start)
        return mo_prim_option(c, false, mo_cell_unit(), out);
    if (at < length && s[at] == '.') {
        start = ++at;
        while (at < length && digit(s[at])) {
            nonzero = nonzero || s[at] != '0';
            significant += nonzero ? 1U : 0U;
            at++;
        }
        fraction = at - start;
        if (fraction == 0)
            return mo_prim_option(c, false, mo_cell_unit(), out);
    }
    int64_t exponent = 0;
    if (at < length && (s[at] == 'e' || s[at] == 'E')) {
        at++;
        bool minus = at < length && s[at] == '-';
        if (at < length && (s[at] == '-' || s[at] == '+'))
            at++;
        start = at;
        while (at < length && digit(s[at])) {
            /* This saturation exceeds every possible mantissa length and preserves its order. */
            if (exponent <= MO_LIMIT_STRING_SIZE)
                exponent = exponent * 10 + s[at] - '0';
            at++;
        }
        if (at == start)
            return mo_prim_option(c, false, mo_cell_unit(), out);
        if (minus)
            exponent = -exponent;
    }
    if (at != length)
        return mo_prim_option(c, false, mo_cell_unit(), out);
    int64_t order = (int64_t)significant + exponent - (int64_t)fraction;
    if (!nonzero || order < -323)
        return mo_prim_option(c, true, mo_cell_float(negative ? -0.0 : 0.0), out);
    if (order > 309)
        return mo_prim_option(c, false, mo_cell_unit(), out);
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, c->heap);
    char *copy = mo_scratch_resize(&scratch, NULL, 0, length + 1);
    if (copy == NULL) {
        mo_scratch_free(&scratch);
        return MO_PRIM_OUT_OF_MEMORY;
    }
    memcpy(copy, s, length);
    copy[length] = '\0';
    /* POSIX's per-thread C locale leaves the host's locale untouched. strtod
     * rounds once, including long mantissas, subnormals and overflow ties. */
    locale_t locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    if (locale == (locale_t)0) {
        mo_scratch_free(&scratch);
        return MO_PRIM_OUT_OF_MEMORY;
    }
    locale_t previous = uselocale(locale);
    MO_ASSERT(previous != (locale_t)0);
    int rounding = fegetround();
    MO_ASSERT(rounding >= 0 && fesetround(FE_TONEAREST) == 0);
    char *end;
    value = strtod(copy, &end);
    MO_ASSERT(end == copy + length);
    MO_ASSERT(fesetround(rounding) == 0 && uselocale(previous) != (locale_t)0);
    freelocale(locale);
    mo_scratch_free(&scratch);
    return mo_prim_option(c, isfinite(value), mo_cell_float(value), out);
}

enum mo_prim_status mo_prim_parse(struct mo_prim_call *c, struct mo_cell *out) {
    size_t length;
    const uint8_t *s = mo_value_data(c->args[0], &length);
    if (c->id == MO_PRIM_TEXT_TO_FLOAT)
        return floating(c, s, length, out);
    mo_i128 n;
    bool fractional = c->id == MO_PRIM_TEXT_TO_DECIMAL;
    bool valid = decimal(s, length, fractional ? (mo_u128)MO_DECIMAL_MAX : (mo_u128)INT64_MAX + 1,
                         fractional, &n);
    struct mo_cell value = !valid       ? mo_cell_unit()
                           : fractional ? mo_cell_decimal(n)
                                        : mo_cell_int((int64_t)n);
    return mo_prim_option(c, valid, value, out);
}
