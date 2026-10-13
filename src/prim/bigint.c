#include "prim/internal.h"

#include <string.h>

/* Native arithmetic uses base 2^32. The heap owns canonical base-256 bytes;
 * all temporary limbs are charged to the calling process. */
struct big {
    uint32_t *limbs;
    size_t count;
    bool negative;
};

static void trim(struct big *n) {
    while (n->count != 0 && n->limbs[n->count - 1] == 0)
        n->count--;
    if (n->count == 0)
        n->negative = false;
}

static MO_MUST_USE bool allocate(struct mo_scratch *s, size_t count, struct big *n) {
    if (count > MO_LIMIT_STRING_SIZE / 4 + 1)
        return false;
    n->limbs = mo_scratch_resize(s, NULL, 0, (count == 0 ? 1 : count) * sizeof *n->limbs);
    n->count = count;
    n->negative = false;
    if (n->limbs == NULL)
        return false;
    memset(n->limbs, 0, count * sizeof *n->limbs);
    return true;
}

static MO_MUST_USE bool read_big(struct mo_scratch *s, struct mo_cell cell, struct big *n) {
    size_t length;
    const uint8_t *bytes = mo_value_data(cell, &length);
    if (!allocate(s, (length + 3) / 4, n))
        return false;
    for (size_t i = 0; i < length; i++)
        n->limbs[i / 4] |= (uint32_t)bytes[i] << (8 * (i % 4));
    n->negative = mo_value_bigint_sign(cell) == MO_BIGINT_NEGATIVE;
    return true;
}

static MO_MUST_USE int compare(struct big a, struct big b) {
    if (a.count != b.count)
        return a.count < b.count ? -1 : 1;
    for (size_t i = a.count; i != 0; i--)
        if (a.limbs[i - 1] != b.limbs[i - 1])
            return a.limbs[i - 1] < b.limbs[i - 1] ? -1 : 1;
    return 0;
}

static MO_MUST_USE bool add(struct mo_scratch *s, struct big a, struct big b, struct big *out) {
    if (a.negative == b.negative) {
        size_t count = a.count > b.count ? a.count : b.count;
        if (!allocate(s, count + 1, out))
            return false;
        uint64_t carry = 0;
        for (size_t i = 0; i < count; i++) {
            carry += (i < a.count ? a.limbs[i] : 0) + (uint64_t)(i < b.count ? b.limbs[i] : 0);
            out->limbs[i] = (uint32_t)carry;
            carry >>= 32;
        }
        out->limbs[count] = (uint32_t)carry;
        out->negative = a.negative;
    } else {
        if (compare(a, b) < 0) {
            struct big swap = a;
            a = b;
            b = swap;
        }
        if (!allocate(s, a.count, out))
            return false;
        uint64_t borrow = 0;
        for (size_t i = 0; i < a.count; i++) {
            uint64_t sub = (i < b.count ? b.limbs[i] : 0) + borrow;
            out->limbs[i] = (uint32_t)((uint64_t)a.limbs[i] - sub);
            borrow = a.limbs[i] < sub ? 1U : 0U;
        }
        MO_ASSERT(borrow == 0);
        out->negative = a.negative;
    }
    trim(out);
    return true;
}

struct product {
    const mo_i128 *a, *b;
    mo_i128 *out, *scratch;
    size_t count;
    unsigned stage;
};

/* Karatsuba keeps large multiplication subquadratic. Coefficients stay below
 * 2^110 for the byte limit, before carrying into base 2^32. No C recursion. */
static void product(const mo_i128 *a, const mo_i128 *b, mo_i128 *out, mo_i128 *scratch,
                    size_t count) {
    struct product stack[24];
    unsigned depth = 1;
    stack[0] = (struct product){a, b, out, scratch, count, 0};
    while (depth != 0) {
        struct product *f = &stack[depth - 1];
        size_t n = f->count, half = n / 2;
        if (f->stage == 0) {
            memset(f->out, 0, 2 * n * sizeof *out);
            if (n <= 16) {
                for (size_t i = 0; i < n; i++)
                    for (size_t j = 0; j < n; j++)
                        f->out[i + j] += f->a[i] * f->b[j];
                depth--;
                continue;
            }
            for (size_t i = 0; i < half; i++) {
                f->scratch[i] = f->a[i] + f->a[half + i];
                f->scratch[half + i] = f->b[i] + f->b[half + i];
            }
            f->stage = 1;
            MO_ASSERT(depth < 24);
            stack[depth++] = (struct product){f->a, f->b, f->out, f->scratch + 2 * n, half, 0};
        } else if (f->stage == 1) {
            f->stage = 2;
            MO_ASSERT(depth < 24);
            stack[depth++] =
                (struct product){f->a + half, f->b + half, f->out + n, f->scratch + 2 * n, half, 0};
        } else if (f->stage == 2) {
            f->stage = 3;
            MO_ASSERT(depth < 24);
            stack[depth++] = (struct product){
                f->scratch, f->scratch + half, f->scratch + n, f->scratch + 2 * n, half, 0};
        } else {
            for (size_t i = 0; i < n; i++)
                f->scratch[n + i] -= f->out[i] + f->out[n + i];
            for (size_t i = 0; i < n; i++)
                f->out[half + i] += f->scratch[n + i];
            depth--;
        }
    }
}

static MO_MUST_USE bool multiply(struct mo_scratch *s, struct big a, struct big b,
                                 struct big *out) {
    if (a.count == 0 || b.count == 0)
        return allocate(s, 0, out);
    if (a.count + b.count - 1 > MO_LIMIT_STRING_SIZE / 4)
        return false;
    size_t n = 1;
    while (n < a.count || n < b.count)
        n *= 2;
    mo_i128 *memory = mo_scratch_resize(s, NULL, 0, 8 * n * sizeof *memory);
    if (memory == NULL || !allocate(s, a.count + b.count, out))
        return false;
    memset(memory, 0, 2 * n * sizeof *memory);
    for (size_t i = 0; i < a.count; i++)
        memory[i] = a.limbs[i];
    for (size_t i = 0; i < b.count; i++)
        memory[n + i] = b.limbs[i];
    product(memory, memory + n, memory + 2 * n, memory + 4 * n, n);
    mo_i128 carry = 0;
    for (size_t i = 0; i < out->count; i++) {
        carry += memory[2 * n + i];
        MO_ASSERT(carry >= 0);
        out->limbs[i] = (uint32_t)((mo_u128)carry & UINT32_MAX);
        carry >>= 32;
    }
    MO_ASSERT(carry == 0);
    out->negative = a.negative != b.negative;
    trim(out);
    return true;
}

/* Normalized long division estimates and corrects one quotient limb at a
 * time. A one-limb divisor is a single pass. Inputs are never modified. */
static MO_MUST_USE bool divide(struct mo_scratch *s, struct big a, struct big b, struct big *q,
                               struct big *r) {
    MO_ASSERT(b.count != 0);
    if (compare(a, b) < 0) {
        *r = a;
        r->negative = false;
        return allocate(s, 0, q);
    }
    if (!allocate(s, a.count - b.count + 1, q) || !allocate(s, b.count, r))
        return false;
    if (b.count == 1) {
        uint64_t carry = 0;
        for (size_t i = a.count; i != 0; i--) {
            uint64_t n = (carry << 32) | a.limbs[i - 1];
            q->limbs[i - 1] = (uint32_t)(n / b.limbs[0]);
            carry = n % b.limbs[0];
        }
        r->limbs[0] = (uint32_t)carry;
    } else {
        unsigned shift = 0;
        for (uint32_t high = b.limbs[b.count - 1]; high < UINT32_C(0x80000000); high <<= 1)
            shift++;
        struct big u, v;
        if (!allocate(s, a.count + 1, &u) || !allocate(s, b.count, &v))
            return false;
        uint64_t carry = 0;
        for (size_t i = 0; i < a.count; i++) {
            carry |= (uint64_t)a.limbs[i] << shift;
            u.limbs[i] = (uint32_t)carry;
            carry >>= 32;
        }
        u.limbs[a.count] = (uint32_t)carry;
        carry = 0;
        for (size_t i = 0; i < b.count; i++) {
            carry |= (uint64_t)b.limbs[i] << shift;
            v.limbs[i] = (uint32_t)carry;
            carry >>= 32;
        }
        MO_ASSERT(carry == 0);
        size_t n = b.count;
        const uint64_t base = UINT64_C(1) << 32;
        for (size_t k = q->count; k != 0; k--) {
            size_t j = k - 1;
            uint64_t numerator = ((uint64_t)u.limbs[j + n] << 32) | u.limbs[j + n - 1];
            uint64_t estimate = numerator / v.limbs[n - 1];
            uint64_t remainder = numerator % v.limbs[n - 1];
            while (estimate >= base ||
                   (remainder < base &&
                    estimate * v.limbs[n - 2] > (remainder << 32) + u.limbs[j + n - 2])) {
                estimate--;
                remainder += v.limbs[n - 1];
                if (remainder >= base)
                    break;
            }
            uint64_t borrow = 0;
            for (size_t i = 0; i < n; i++) {
                uint64_t p = estimate * v.limbs[i] + borrow;
                uint32_t low = (uint32_t)p;
                borrow = (p >> 32) + (u.limbs[j + i] < low ? 1U : 0U);
                u.limbs[j + i] -= low;
            }
            bool negative = u.limbs[j + n] < borrow;
            u.limbs[j + n] -= (uint32_t)borrow;
            if (negative) {
                estimate--;
                carry = 0;
                for (size_t i = 0; i < n; i++) {
                    carry += (uint64_t)u.limbs[j + i] + v.limbs[i];
                    u.limbs[j + i] = (uint32_t)carry;
                    carry >>= 32;
                }
                u.limbs[j + n] += (uint32_t)carry;
            }
            q->limbs[j] = (uint32_t)estimate;
        }
        for (size_t i = 0; i < n; i++) {
            uint64_t word = ((uint64_t)u.limbs[i + 1] << 32) | u.limbs[i];
            r->limbs[i] = (uint32_t)(word >> shift);
        }
    }
    trim(q);
    trim(r);
    return true;
}

static MO_MUST_USE bool write_big(struct mo_prim_call *c, struct mo_scratch *s, struct big n,
                                  struct mo_cell *out) {
    uint8_t *bytes = mo_scratch_resize(s, NULL, 0, n.count == 0 ? 1 : n.count * 4);
    if (bytes == NULL)
        return false;
    for (size_t i = 0; i < n.count * 4; i++)
        bytes[i] = (uint8_t)(n.limbs[i / 4] >> (8 * (i % 4)));
    size_t length = n.count * 4;
    while (length != 0 && bytes[length - 1] == 0)
        length--;
    enum mo_bigint_sign sign = length == 0  ? MO_BIGINT_ZERO
                               : n.negative ? MO_BIGINT_NEGATIVE
                                            : MO_BIGINT_POSITIVE;
    return mo_value_bigint(c->heap, sign, bytes, length, out);
}

enum mo_prim_status mo_prim_bigint(struct mo_prim_call *c, struct mo_cell *out) {
    if (c->id == MO_PRIM_BIGINT_TO_TEXT)
        return mo_prim_print(c, c->params[0], c->args[0], out);
    if (c->id == MO_PRIM_BIGINT_COMPARE || c->id == MO_PRIM_BIGINT_MIN ||
        c->id == MO_PRIM_BIGINT_MAX) {
        int comparison;
        if (!mo_value_compare(c->heap, c->params[0], c->args[0], c->args[1], &comparison))
            return MO_PRIM_OUT_OF_MEMORY;
        if (c->id == MO_PRIM_BIGINT_COMPARE)
            *out = mo_cell_case(comparison < 0 ? 0U : comparison == 0 ? 1U : 2U, mo_cell_unit());
        else
            *out = mo_value_dup(
                c->heap, c->result,
                c->args[(c->id == MO_PRIM_BIGINT_MIN ? comparison > 0 : comparison < 0) ? 1 : 0]);
        return MO_PRIM_RETURNED;
    }
    if (c->id == MO_PRIM_BIGINT_TO_INT) {
        size_t length;
        const uint8_t *bytes = mo_value_data(c->args[0], &length);
        uint64_t magnitude = 0;
        bool negative = mo_value_bigint_sign(c->args[0]) == MO_BIGINT_NEGATIVE;
        if (length <= 8)
            for (size_t i = length; i != 0; i--)
                magnitude = magnitude * 256 + bytes[i - 1];
        bool fits = length <= 8 && magnitude <= (negative ? UINT64_C(1) << 63 : INT64_MAX);
        int64_t value = fits ? (int64_t)(negative ? -(mo_i128)magnitude : magnitude) : 0;
        return mo_prim_option(c, fits, mo_cell_int(value), out);
    }
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, c->heap);
    enum mo_prim_status status = MO_PRIM_OUT_OF_MEMORY;
    struct big a, b, result;
    if (!read_big(&scratch, c->args[0], &a))
        goto done;
    if (c->id == MO_PRIM_BIGINT_NEG || c->id == MO_PRIM_BIGINT_ABS) {
        result = a;
        result.negative = a.count != 0 && c->id == MO_PRIM_BIGINT_NEG && !a.negative;
    } else if (c->id == MO_PRIM_BIGINT_POW) {
        int64_t exponent = mo_cell_get_int(c->args[1]);
        if (exponent < 0) {
            c->reason = "BigInt.pow with a negative exponent";
            status = MO_PRIM_OUTSIDE;
            goto done;
        }
        if (!allocate(&scratch, 1, &result))
            goto done;
        result.limbs[0] = 1;
        if (exponent != 0 && a.count <= 1 && (a.count == 0 || a.limbs[0] == 1)) {
            result = a;
            result.negative = a.negative && (exponent & 1) != 0;
        } else {
            while (exponent != 0) {
                if ((exponent & 1) != 0 && !multiply(&scratch, result, a, &result))
                    goto done;
                exponent >>= 1;
                if (exponent != 0 && !multiply(&scratch, a, a, &a))
                    goto done;
            }
        }
    } else {
        if (!read_big(&scratch, c->args[1], &b))
            goto done;
        if (c->id == MO_PRIM_BIGINT_ADD || c->id == MO_PRIM_BIGINT_SUB) {
            if (c->id == MO_PRIM_BIGINT_SUB)
                b.negative = !b.negative;
            if (!add(&scratch, a, b, &result))
                goto done;
        } else if (c->id == MO_PRIM_BIGINT_MUL) {
            if (!multiply(&scratch, a, b, &result))
                goto done;
        } else {
            if (b.count == 0) {
                c->reason = "dividing by zero";
                status = MO_PRIM_DIV_BY_ZERO;
                goto done;
            }
            struct big q, r;
            if (!divide(&scratch, a, b, &q, &r))
                goto done;
            bool floor = c->id == MO_PRIM_BIGINT_FLOOR_DIV || c->id == MO_PRIM_BIGINT_FLOOR_MOD;
            if (floor && a.negative != b.negative && r.count != 0) {
                uint32_t limb = 1;
                struct big one = {&limb, 1, false};
                if (!add(&scratch, q, one, &q))
                    goto done;
                r.negative = true;
                b.negative = false;
                if (!add(&scratch, b, r, &r))
                    goto done;
            }
            /* b's sign may have been cleared while forming the floor remainder. */
            bool divisor_negative = mo_value_bigint_sign(c->args[1]) == MO_BIGINT_NEGATIVE;
            q.negative = q.count != 0 && a.negative != divisor_negative;
            r.negative = r.count != 0 && (floor ? divisor_negative : a.negative);
            result = c->id == MO_PRIM_BIGINT_DIV || c->id == MO_PRIM_BIGINT_FLOOR_DIV ? q : r;
        }
    }
    if (write_big(c, &scratch, result, out))
        status = MO_PRIM_RETURNED;
done:
    mo_scratch_free(&scratch);
    return status;
}
