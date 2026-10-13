/* Balanced decimal-to-binary conversion. Repeated multiplication by ten is quadratic. */
#include "codegen/bigint.h"

#include <string.h>

enum { DECIMAL_BASE = 1000000000, MULTIPLY_LEAF = 16, MULTIPLY_DEPTH = 24 };

/* Padding ceil(digits / 9) to a power of two must stay within the coefficient bound below. */
_Static_assert(MO_LIMIT_CORE_TEXT_SIZE <= 9u * (1u << 20),
               "decimal conversion has at most 2^20 limbs");
_Static_assert(MULTIPLY_DEPTH >= 21 && MULTIPLY_DEPTH <= MO_LIMIT_CORE_DEPTH,
               "the explicit multiplication stack covers the limb bound");

static MO_MUST_USE bool charge(size_t *work, size_t amount) {
    MO_ASSERT(*work <= MO_LIMIT_CODEGEN_BIGINT_WORK);
    if (amount > MO_LIMIT_CODEGEN_BIGINT_WORK - *work)
        return false;
    *work += amount;
    return true;
}
/* Predict coefficient work before allocating scratch. For power-of-two n, multiply costs
 * 2n+n² at a leaf and 5n+3M(n/2) otherwise. Output-byte work depends on the magnitude and
 * stays charged at emission; no acceptable input is refused by a pessimistic limb count. */
static MO_MUST_USE bool fits_work(size_t chunks, size_t work) {
    if (!charge(&work, 2 * chunks)) /* initial zeroing and final high-limb scan */
        return false;
    size_t multiplication = 3; /* M(1) */
    for (size_t stride = 1; stride < chunks; stride *= 2) {
        size_t groups = chunks / (2 * stride), cost = multiplication + 3 * stride;
        if (cost > (MO_LIMIT_CODEGEN_BIGINT_WORK - work) / groups || !charge(&work, groups * cost))
            return false;
        if (2 * stride < chunks && !charge(&work, multiplication + 4 * stride))
            return false;
        /* multiplication cannot have exceeded the work ceiling on a passing iteration. */
        multiplication = stride < MULTIPLY_LEAF ? 4 * stride + 4 * stride * stride
                                                : 10 * stride + 3 * multiplication;
    }
    return true;
}
struct frame {
    const mo_i128 *a, *b;
    mo_i128 *out, *scratch;
    size_t count;
    unsigned stage;
};
/* Inputs have count coefficients; output has twice count, scratch four times count.
 * At most 20 levels sum base-2^32 limbs: inputs stay below 2^52, a leaf's sum
 * below 2^108, and interpolation below 2^110, all within signed i128. */
static MO_MUST_USE bool multiply(const mo_i128 *a, const mo_i128 *b, mo_i128 *out, size_t count,
                                 mo_i128 *scratch, size_t *work) {
    struct frame stack[MULTIPLY_DEPTH];
    unsigned depth = 1;
    stack[0] = (struct frame){a, b, out, scratch, count, 0};
    while (depth != 0) {
        struct frame *f = &stack[depth - 1];
        size_t n = f->count, half = n / 2;
        if (f->stage == 0) {
            if (!charge(work, 2 * n))
                return false;
            memset(f->out, 0, 2 * n * sizeof *out);
            if (n <= MULTIPLY_LEAF) {
                if (!charge(work, n * n))
                    return false;
                for (size_t i = 0; i < n; i++)
                    for (size_t j = 0; j < n; j++)
                        f->out[i + j] += f->a[i] * f->b[j];
                depth--;
                continue;
            }
            if (!charge(work, 2 * half))
                return false;
            for (size_t i = 0; i < half; i++) {
                f->scratch[i] = f->a[i] + f->a[half + i];
                f->scratch[half + i] = f->b[i] + f->b[half + i];
            }
            f->stage = 1;
            MO_ASSERT(depth < MULTIPLY_DEPTH);
            stack[depth++] = (struct frame){f->a, f->b, f->out, f->scratch + 2 * n, half, 0};
        } else if (f->stage == 1) {
            f->stage = 2;
            MO_ASSERT(depth < MULTIPLY_DEPTH);
            stack[depth++] =
                (struct frame){f->a + half, f->b + half, f->out + n, f->scratch + 2 * n, half, 0};
        } else if (f->stage == 2) {
            f->stage = 3;
            MO_ASSERT(depth < MULTIPLY_DEPTH);
            stack[depth++] = (struct frame){
                f->scratch, f->scratch + half, f->scratch + n, f->scratch + 2 * n, half, 0};
        } else {
            if (!charge(work, 2 * n))
                return false;
            for (size_t i = 0; i < n; i++)
                f->scratch[n + i] -= f->out[i] + f->out[n + i];
            for (size_t i = 0; i < n; i++)
                f->out[half + i] += f->scratch[n + i];
            depth--;
        }
    }
    return true;
}
static MO_MUST_USE bool normalize(mo_i128 *digits, size_t count, size_t *work) {
    if (!charge(work, count))
        return false;
    mo_i128 carry = 0;
    for (size_t i = 0; i < count; i++) {
        carry += digits[i];
        MO_ASSERT(carry >= 0);
        digits[i] = carry & (mo_i128)UINT32_MAX;
        carry >>= 32;
    }
    MO_ASSERT(carry == 0);
    return true;
}
enum mo_codegen_bigint_problem mo_codegen_bigint(struct mo_arena *arena, mo_core_integer integer,
                                                 size_t *work, struct mo_bytecode_bigint *out) {
    size_t length = integer.digits.length;
    MO_ASSERT(length != 0 && length <= MO_LIMIT_CORE_TEXT_SIZE);
    if (!charge(work, length))
        return MO_CODEGEN_BIGINT_LIMIT;
    if (length == 1 && integer.digits.bytes[0] == '0') {
        *out = (struct mo_bytecode_bigint){.sign = MO_BIGINT_ZERO};
        return MO_CODEGEN_BIGINT_OK;
    }
    size_t chunks = 1;
    while (chunks < (length + 8) / 9)
        chunks *= 2;
    MO_ASSERT(chunks <= (1u << 20));
    if (!fits_work(chunks, *work))
        return MO_CODEGEN_BIGINT_LIMIT;
    struct mo_arena temporary;
    mo_arena_init(&temporary, 9 * chunks * sizeof(mo_i128) + MO_ARENA_CHUNK_HEADER);
    mo_i128 *storage =
        mo_arena_allocate_array(&temporary, 9 * chunks, sizeof *storage, _Alignof(mo_i128));
    if (storage == NULL) {
        mo_arena_free(&temporary);
        return MO_CODEGEN_BIGINT_MEMORY;
    }
    mo_i128 *current = storage, *next = current + chunks, *power = next + chunks,
            *product = power + chunks, *scratch = product + 2 * chunks;
    if (!charge(work, chunks)) {
        mo_arena_free(&temporary);
        return MO_CODEGEN_BIGINT_LIMIT;
    }
    memset(current, 0, chunks * sizeof *current);
    for (size_t at = length, i = 0; at != 0; i++) {
        size_t start = at > 9 ? at - 9 : 0;
        uint32_t value = 0;
        for (size_t j = start; j < at; j++) {
            MO_ASSERT(integer.digits.bytes[j] >= '0' && integer.digits.bytes[j] <= '9');
            value = value * 10u + (unsigned)(integer.digits.bytes[j] - '0');
        }
        current[i] = value;
        at = start;
    }
    power[0] = DECIMAL_BASE;
    bool ok = true;
    for (size_t stride = 1; ok && stride < chunks; stride *= 2) {
        for (size_t offset = 0; ok && offset < chunks; offset += 2 * stride) {
            ok = multiply(current + offset + stride, power, next + offset, stride, scratch, work) &&
                 charge(work, stride);
            if (ok) {
                for (size_t i = 0; i < stride; i++)
                    next[offset + i] += current[offset + i];
                ok = normalize(next + offset, 2 * stride, work);
            }
        }
        mo_i128 *swap = current;
        current = next;
        next = swap;
        if (ok && 2 * stride < chunks) {
            ok = multiply(power, power, product, stride, scratch, work) &&
                 normalize(product, 2 * stride, work) && charge(work, 2 * stride);
            if (ok)
                memcpy(power, product, 2 * stride * sizeof *power);
        }
    }
    ok = ok && charge(work, chunks);
    size_t limbs = ok ? chunks : 0;
    while (limbs != 0 && current[limbs - 1] == 0)
        limbs--;
    uint8_t *bytes = NULL;
    if (ok) {
        MO_ASSERT(limbs != 0);
        ok = charge(work, 4 * limbs);
        if (ok)
            bytes = mo_arena_allocate(arena, 4 * limbs, 1);
        if (bytes != NULL) {
            for (size_t i = 0; i < 4 * limbs; i++)
                bytes[i] = (uint8_t)((uint32_t)current[i / 4] >> (8 * (i % 4)));
            size_t count = 4 * limbs;
            while (bytes[count - 1] == 0)
                count--;
            *out = (struct mo_bytecode_bigint){.sign = integer.negative ? MO_BIGINT_NEGATIVE
                                                                        : MO_BIGINT_POSITIVE,
                                               .magnitude = {(uint32_t)count, bytes}};
        }
    }
    mo_arena_free(&temporary);
    return !ok             ? MO_CODEGEN_BIGINT_LIMIT
           : bytes == NULL ? MO_CODEGEN_BIGINT_MEMORY
                           : MO_CODEGEN_BIGINT_OK;
}
