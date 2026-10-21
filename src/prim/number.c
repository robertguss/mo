#include "prim/internal.h"

#include <math.h>
#include <string.h>

static MO_MUST_USE unsigned bits(mo_u128 n) {
    unsigned count = 0;
    while (n != 0) {
        count++;
        n >>= 1;
    }
    return count;
}

/* Decimal and Int ratios need at most 114 bits, including the rounding bit. */
double mo_prim_ratio_float(bool negative, mo_u128 n, mo_u128 d) {
    MO_ASSERT(d != 0);
    uint64_t raw = negative ? UINT64_C(1) << 63 : 0;
    if (n != 0) {
        int e = (int)bits(n) - (int)bits(d);
        if ((e >= 0 && n < (d << (unsigned)e)) || (e < 0 && (n << (unsigned)-e) < d))
            e--;
        int shift = 52 - e;
        mo_u128 a = shift >= 0 ? n << (unsigned)shift : n;
        mo_u128 b = shift < 0 ? d << (unsigned)-shift : d;
        mo_u128 q = a / b, r = a % b;
        q += 2 * r > b || (2 * r == b && (q & 1) != 0) ? 1U : 0U;
        raw |= ((uint64_t)(e + 1022) << 52) + (uint64_t)q;
    }
    double value;
    memcpy(&value, &raw, sizeof value);
    return value;
}

static MO_MUST_USE enum mo_prim_status int_result(struct mo_prim_call *c, mo_i128 n,
                                                  struct mo_cell *out) {
    if (n < INT64_MIN || n > INT64_MAX) {
        c->reason = "Int";
        return MO_PRIM_OVERFLOW;
    }
    *out = mo_cell_int((int64_t)n);
    return MO_PRIM_RETURNED;
}

mo_i128 mo_prim_exact_product(int64_t a, int64_t b) {
    return (mo_i128)a * b;
}

/* C's division, which truncates toward zero, and its remainder, which has a's sign. */
static void truncated_division(int64_t a, int64_t b, int64_t *q, int64_t *r) {
    MO_ASSERT(b != 0 && b != -1);
    *q = a / b;
    *r = a % b;
}

static MO_MUST_USE enum mo_prim_status integers(struct mo_prim_call *c, struct mo_cell *out) {
    int64_t a = mo_cell_get_int(c->args[0]);
    int64_t b = 0;
    mo_i128 n;
    if (c->id == MO_PRIM_INT_MIN || c->id == MO_PRIM_INT_MAX ||
        (c->id >= MO_PRIM_INT_FLOOR_DIV && c->id <= MO_PRIM_INT_CHECKED_MUL))
        b = mo_cell_get_int(c->args[1]);
    switch ((unsigned)c->id) {
    case MO_PRIM_INT_ABS:
        return int_result(c, a < 0 ? -(mo_i128)a : a, out);
    case MO_PRIM_INT_MIN:
    case MO_PRIM_INT_MAX:
        *out = mo_cell_int(c->id == MO_PRIM_INT_MIN ? (a < b ? a : b) : (a > b ? a : b));
        return MO_PRIM_RETURNED;
    case MO_PRIM_INT_FLOOR_DIV:
    case MO_PRIM_INT_FLOOR_MOD: {
        if (b == 0) {
            c->reason = "dividing by zero";
            return MO_PRIM_DIV_BY_ZERO;
        }
        if (b == -1)
            return int_result(c, c->id == MO_PRIM_INT_FLOOR_DIV ? -(mo_i128)a : 0, out);
        /* Truncate, then step down when the remainder's sign is not the divisor's. */
        int64_t q, r;
        truncated_division(a, b, &q, &r);
        if (r != 0 && ((r < 0) != (b < 0))) {
            q--;
            r += b;
        }
        *out = mo_cell_int(c->id == MO_PRIM_INT_FLOOR_DIV ? q : r);
        return MO_PRIM_RETURNED;
    }
    case MO_PRIM_INT_WRAPPING_ADD:
    case MO_PRIM_INT_SATURATING_ADD:
    case MO_PRIM_INT_CHECKED_ADD:
        n = (mo_i128)a + b;
        break;
    case MO_PRIM_INT_WRAPPING_SUB:
    case MO_PRIM_INT_SATURATING_SUB:
    case MO_PRIM_INT_CHECKED_SUB:
        n = (mo_i128)a - b;
        break;
    case MO_PRIM_INT_WRAPPING_MUL:
    case MO_PRIM_INT_SATURATING_MUL:
    case MO_PRIM_INT_CHECKED_MUL:
        n = mo_prim_exact_product(a, b);
        break;
    case MO_PRIM_INT_TO_DECIMAL:
        *out = mo_cell_decimal(mo_prim_exact_product(a, MO_DECIMAL_SCALE));
        return MO_PRIM_RETURNED;
    case MO_PRIM_INT_TO_FLOAT:
        *out = mo_cell_float(mo_prim_ratio_float(a < 0, (mo_u128)(a < 0 ? -(mo_i128)a : a), 1));
        return MO_PRIM_RETURNED;
    case MO_PRIM_INT_TO_BIG: {
        uint64_t magnitude = (uint64_t)(a < 0 ? -(mo_i128)a : a);
        uint8_t bytes[8];
        size_t length = 0;
        while (magnitude != 0) {
            bytes[length++] = (uint8_t)(magnitude & 0xff);
            magnitude >>= 8;
        }
        enum mo_bigint_sign sign = a == 0  ? MO_BIGINT_ZERO
                                   : a < 0 ? MO_BIGINT_NEGATIVE
                                           : MO_BIGINT_POSITIVE;
        return mo_value_bigint(c->heap, sign, bytes, length, out) ? MO_PRIM_RETURNED
                                                                  : MO_PRIM_OUT_OF_MEMORY;
    }
    case MO_PRIM_INT_TO_BYTE: {
        bool valid = a >= 0 && a <= 255;
        return mo_prim_option(c, valid, mo_cell_byte(valid ? (uint8_t)a : 0), out);
    }
    case MO_PRIM_INT_TO_CHAR: {
        bool valid = a >= 0 && a <= 0x10ffff && (a < 0xd800 || a > 0xdfff);
        return mo_prim_option(c, valid, mo_cell_char(valid ? (uint32_t)a : 0), out);
    }
    case MO_PRIM_INT_TO_TEXT:
        return mo_prim_print(c, c->params[0], c->args[0], out);
    case MO_PRIM_INT_SECONDS:
    case MO_PRIM_INT_MILLISECONDS:
    case MO_PRIM_INT_MINUTES:
    case MO_PRIM_INT_HOURS:
    case MO_PRIM_INT_DAYS: {
        static const int64_t scales[] = {1000000, 1000, 60000000, 3600000000, 86400000000};
        n = mo_prim_exact_product(a, scales[c->id - MO_PRIM_INT_SECONDS]);
        if (n < INT64_MIN || n > INT64_MAX) {
            c->reason = "Duration";
            return MO_PRIM_OVERFLOW;
        }
        *out = mo_cell_duration((int64_t)n);
        return MO_PRIM_RETURNED;
    }
    default:
        MO_UNREACHABLE();
    }
    if (c->id <= MO_PRIM_INT_WRAPPING_MUL) {
        /* The exact result's low 64 bits, read as two's complement. */
        mo_u128 bits;
        memcpy(&bits, &n, sizeof bits);
        uint64_t low = (uint64_t)(bits & UINT64_MAX);
        int64_t signed_low;
        memcpy(&signed_low, &low, sizeof signed_low);
        *out = mo_cell_int(signed_low);
        return MO_PRIM_RETURNED;
    }
    if (c->id <= MO_PRIM_INT_SATURATING_MUL)
        return int_result(c, n > INT64_MAX ? INT64_MAX : n < INT64_MIN ? INT64_MIN : n, out);
    bool valid = n >= INT64_MIN && n <= INT64_MAX;
    return mo_prim_option(c, valid, mo_cell_int(valid ? (int64_t)n : 0), out);
}

static MO_MUST_USE mo_u128 ten(unsigned places) {
    MO_ASSERT(places <= 38);
    mo_u128 result = 1;
    for (unsigned i = 0; i < places; i++)
        result *= 10;
    return result;
}

static MO_MUST_USE enum mo_prim_status fixed(struct mo_prim_call *c, bool negative, mo_u128 n,
                                             unsigned places, struct mo_cell *out) {
    char bytes[64], backwards[40];
    size_t length = 0, count = 0;
    if (negative && n != 0)
        bytes[length++] = '-';
    mo_u128 whole = n / MO_DECIMAL_SCALE;
    do {
        backwards[count++] = (char)('0' + whole % 10);
        whole /= 10;
    } while (whole != 0);
    while (count != 0)
        bytes[length++] = backwards[--count];
    if (places != 0) {
        bytes[length++] = '.';
        mo_u128 fraction = n % MO_DECIMAL_SCALE, divisor = (mo_u128)MO_DECIMAL_SCALE / 10;
        for (unsigned i = 0; i < places; i++) {
            bytes[length++] = (char)('0' + fraction / divisor);
            fraction %= divisor;
            divisor /= 10;
        }
    }
    return mo_value_text(c->heap, bytes, length, out) ? MO_PRIM_RETURNED : MO_PRIM_OUT_OF_MEMORY;
}

static MO_MUST_USE enum mo_prim_status decimals(struct mo_prim_call *c, struct mo_cell *out) {
    mo_i128 a = mo_cell_get_decimal(c->args[0]);
    mo_u128 magnitude = (mo_u128)(a < 0 ? -a : a), rounded = magnitude;
    unsigned places = 0;
    switch ((unsigned)c->id) {
    case MO_PRIM_DECIMAL_ABS:
        *out = mo_cell_decimal((mo_i128)magnitude);
        return MO_PRIM_RETURNED;
    case MO_PRIM_DECIMAL_MIN:
    case MO_PRIM_DECIMAL_MAX: {
        mo_i128 b = mo_cell_get_decimal(c->args[1]);
        *out = mo_cell_decimal(c->id == MO_PRIM_DECIMAL_MIN ? (a < b ? a : b) : (a > b ? a : b));
        return MO_PRIM_RETURNED;
    }
    case MO_PRIM_DECIMAL_TO_INT: {
        mo_i128 n = a / MO_DECIMAL_SCALE;
        bool valid = a % MO_DECIMAL_SCALE == 0 && n >= INT64_MIN && n <= INT64_MAX;
        return mo_prim_option(c, valid, mo_cell_int(valid ? (int64_t)n : 0), out);
    }
    case MO_PRIM_DECIMAL_TO_FLOAT:
        *out = mo_cell_float(mo_prim_ratio_float(a < 0, magnitude, MO_DECIMAL_SCALE));
        return MO_PRIM_RETURNED;
    case MO_PRIM_DECIMAL_TO_TEXT:
        return mo_prim_print(c, c->params[0], c->args[0], out);
    case MO_PRIM_DECIMAL_ROUND:
    case MO_PRIM_DECIMAL_TO_FIXED: {
        int64_t p = mo_cell_get_int(c->args[1]);
        if (p < 0 || p > 18) {
            c->reason = c->id == MO_PRIM_DECIMAL_ROUND
                            ? "Decimal.round with places outside 0 to 18"
                            : "Decimal.to_fixed with places outside 0 to 18";
            return MO_PRIM_OUTSIDE;
        }
        places = (unsigned)p;
        mo_u128 divisor = ten(18 - places);
        rounded =
            (magnitude / divisor + (magnitude % divisor >= divisor / 2 + divisor % 2 ? 1U : 0U)) *
            divisor;
        break;
    }
    case MO_PRIM_DECIMAL_FLOOR:
    case MO_PRIM_DECIMAL_CEIL: {
        bool away = c->id == MO_PRIM_DECIMAL_FLOOR ? a < 0 : a >= 0;
        rounded =
            (magnitude / MO_DECIMAL_SCALE + (away && magnitude % MO_DECIMAL_SCALE != 0 ? 1U : 0U)) *
            MO_DECIMAL_SCALE;
        break;
    }
    default:
        MO_UNREACHABLE();
    }
    /* to_fixed can spell a rounded value outside Decimal's range; round cannot. */
    if (c->id == MO_PRIM_DECIMAL_TO_FIXED)
        return fixed(c, a < 0, rounded, places, out);
    if (rounded > (mo_u128)MO_DECIMAL_MAX) {
        c->reason = "Decimal";
        return MO_PRIM_OVERFLOW;
    }
    *out = mo_cell_decimal(a < 0 ? -(mo_i128)rounded : (mo_i128)rounded);
    return MO_PRIM_RETURNED;
}

static MO_MUST_USE enum mo_prim_status float_decimal(struct mo_prim_call *c, double a,
                                                     struct mo_cell *out) {
    if (!isfinite(a))
        return mo_prim_option(c, false, mo_cell_unit(), out);
    struct mo_buffer text;
    mo_buffer_init(&text, 64);
    if (!mo_float_text(a, &text)) {
        mo_buffer_free(&text);
        return MO_PRIM_OUT_OF_MEMORY;
    }
    mo_u128 coefficient = 0;
    int exponent = 18;
    bool fraction = false, negative = text.bytes[0] == '-';
    size_t i = negative ? 1U : 0U;
    for (; i < text.length && text.bytes[i] != 'e'; i++) {
        if (text.bytes[i] == '.')
            fraction = true;
        else {
            coefficient = coefficient * 10 + text.bytes[i] - '0';
            exponent -= fraction ? 1 : 0;
        }
    }
    if (i < text.length) {
        i++;
        bool minus = text.bytes[i] == '-';
        i += minus ? 1U : 0U;
        int e = 0;
        for (; i < text.length; i++)
            e = e * 10 + text.bytes[i] - '0';
        exponent += minus ? -e : e;
    }
    mo_buffer_free(&text);
    bool valid = true;
    for (int e = 0; e < exponent; e++) {
        if (coefficient > (mo_u128)MO_DECIMAL_MAX / 10) {
            valid = false;
            break;
        }
        coefficient *= 10;
    }
    if (exponent < -38)
        coefficient = 0;
    else if (exponent < 0) {
        mo_u128 divisor = ten((unsigned)-exponent);
        mo_u128 q = coefficient / divisor, r = coefficient % divisor;
        coefficient = q + (2 * r > divisor || (2 * r == divisor && (q & 1) != 0) ? 1U : 0U);
    }
    mo_i128 value = valid ? (mo_i128)coefficient : 0;
    return mo_prim_option(c, valid, mo_cell_decimal(negative ? -value : value), out);
}

enum mo_prim_status mo_prim_number(struct mo_prim_call *c, struct mo_cell *out) {
    if (c->id <= MO_PRIM_INT_DAYS)
        return integers(c, out);
    if (c->id == MO_PRIM_BYTE_TO_INT) {
        *out = mo_cell_int(mo_cell_get_byte(c->args[0]));
        return MO_PRIM_RETURNED;
    }
    if (c->id == MO_PRIM_BYTE_TO_TEXT)
        return mo_prim_print(c, c->params[0], c->args[0], out);
    if (c->id <= MO_PRIM_DECIMAL_TO_TEXT)
        return decimals(c, out);
    double a = mo_cell_get_float(c->args[0]);
    switch ((unsigned)c->id) {
    case MO_PRIM_FLOAT_ABS:
        a = fabs(a);
        break;
    case MO_PRIM_FLOAT_FLOOR:
        a = floor(a);
        break;
    case MO_PRIM_FLOAT_CEIL:
        a = ceil(a);
        break;
    case MO_PRIM_FLOAT_ROUND:
        a = round(a);
        break;
    case MO_PRIM_FLOAT_SQRT:
        a = sqrt(a);
        break;
    case MO_PRIM_FLOAT_IS_NAN:
    case MO_PRIM_FLOAT_IS_INFINITE:
        *out = mo_cell_bool(c->id == MO_PRIM_FLOAT_IS_NAN ? isnan(a) : isinf(a));
        return MO_PRIM_RETURNED;
    case MO_PRIM_FLOAT_TO_DECIMAL:
        return float_decimal(c, a, out);
    case MO_PRIM_FLOAT_TO_TEXT:
        return mo_prim_print(c, c->params[0], c->args[0], out);
    default:
        MO_UNREACHABLE();
    }
    if (isnan(a)) {
        uint64_t canonical = UINT64_C(0x7ff8000000000000);
        memcpy(&a, &canonical, sizeof a);
    }
    *out = mo_cell_float(a);
    return MO_PRIM_RETURNED;
}
