/* Binary-to-decimal conversion by balanced radix conversion and Karatsuba.
 * Repeated division of a large magnitude would take quadratic work. */
#include "heap/internal.h"

#include <string.h>

enum { DECIMAL_BASE = 1000000000, MULTIPLY_LEAF = 16, MULTIPLY_DEPTH = 32 };

struct multiply_frame {
    const mo_i128 *a, *b;
    mo_i128 *out, *scratch;
    size_t count;
    unsigned stage;
};

/* scratch has four times count cells. The explicit stack grows logarithmically
 * and fits for every magnitude allowed by MO_LIMIT_VALUE_BYTES. */
static void multiply(const mo_i128 *a, const mo_i128 *b, mo_i128 *out, size_t count,
                     mo_i128 *scratch) {
    struct multiply_frame stack[MULTIPLY_DEPTH];
    unsigned depth = 1;
    stack[0] = (struct multiply_frame){a, b, out, scratch, count, 0};
    while (depth != 0) {
        struct multiply_frame *f = &stack[depth - 1];
        size_t n = f->count, half = n / 2;
        if (f->stage == 0) {
            memset(f->out, 0, 2 * n * sizeof *out);
            if (n <= MULTIPLY_LEAF) {
                for (size_t i = 0; i < n; i++) {
                    for (size_t j = 0; j < n; j++) {
                        f->out[i + j] += f->a[i] * f->b[j];
                    }
                }
                depth--;
                continue;
            }
            for (size_t i = 0; i < half; i++) {
                f->scratch[i] = f->a[i] + f->a[half + i];
                f->scratch[half + i] = f->b[i] + f->b[half + i];
            }
            f->stage = 1;
            MO_ASSERT(depth < MULTIPLY_DEPTH);
            stack[depth++] =
                (struct multiply_frame){f->a, f->b, f->out, f->scratch + 2 * n, half, 0};
        } else if (f->stage == 1) {
            f->stage = 2;
            MO_ASSERT(depth < MULTIPLY_DEPTH);
            stack[depth++] = (struct multiply_frame){f->a + half,        f->b + half, f->out + n,
                                                     f->scratch + 2 * n, half,        0};
        } else if (f->stage == 2) {
            f->stage = 3;
            MO_ASSERT(depth < MULTIPLY_DEPTH);
            stack[depth++] = (struct multiply_frame){
                f->scratch, f->scratch + half, f->scratch + n, f->scratch + 2 * n, half, 0};
        } else {
            for (size_t i = 0; i < n; i++) {
                f->scratch[n + i] -= f->out[i] + f->out[n + i];
            }
            for (size_t i = 0; i < n; i++) {
                f->out[half + i] += f->scratch[n + i];
            }
            depth--;
        }
    }
}

static void normalize(mo_i128 *digits, size_t count) {
    mo_i128 carry = 0;
    for (size_t i = 0; i < count; i++) {
        carry += digits[i];
        MO_ASSERT(carry >= 0);
        digits[i] = carry % DECIMAL_BASE;
        carry /= DECIMAL_BASE;
    }
    MO_ASSERT(carry == 0);
}

bool mo_bigint_text(struct mo_heap *process, struct mo_cell value, struct mo_buffer *out) {
    struct mo_allocation *allocation = mo_heap_allocation(mo_cell_get_ref(value));
    if (allocation->length == 0) {
        return mo_buffer_append_byte(out, '0');
    }
    /* Every full lower byte requires more than two decimal digits. Refuse a
     * definitely short caller buffer before doing the radix conversion. */
    if (2 * ((size_t)allocation->length - 1) > out->limit - out->length) {
        return false;
    }
    size_t chunks = 1;
    while (chunks < ((size_t)allocation->length + 3) / 4) {
        chunks *= 2;
    }
    struct mo_scratch temporary;
    mo_scratch_init(&temporary, process);
    mo_i128 *storage = mo_scratch_resize(&temporary, NULL, 0, 12 * chunks * sizeof(mo_i128));
    if (storage == NULL) {
        mo_scratch_free(&temporary);
        return false;
    }
    mo_i128 *current = storage, *next = current + 2 * chunks;
    mo_i128 *power = next + 2 * chunks, *product = power + 2 * chunks;
    mo_i128 *scratch = product + 2 * chunks;
    memset(current, 0, 2 * chunks * sizeof *current);
    const uint8_t *bytes = (const uint8_t *)allocation->cells;
    for (size_t i = 0; i < chunks; i++) {
        uint64_t word = 0;
        for (unsigned j = 0; j < 4 && 4 * i + j < allocation->length; j++) {
            word |= (uint64_t)bytes[4 * i + j] << (8 * j);
        }
        current[2 * i] = (mo_i128)(word % DECIMAL_BASE);
        current[2 * i + 1] = (mo_i128)(word / DECIMAL_BASE);
    }
    power[0] = 294967296;
    power[1] = 4;
    for (size_t stride = 2; stride < 2 * chunks; stride *= 2) {
        for (size_t offset = 0; offset < 2 * chunks; offset += 2 * stride) {
            multiply(current + offset + stride, power, next + offset, stride, scratch);
            for (size_t i = 0; i < stride; i++) {
                next[offset + i] += current[offset + i];
            }
            normalize(next + offset, 2 * stride);
        }
        mo_i128 *swap = current;
        current = next;
        next = swap;
        if (2 * stride < 2 * chunks) {
            multiply(power, power, product, stride, scratch);
            normalize(product, 2 * stride);
            memcpy(power, product, 2 * stride * sizeof *power);
        }
    }
    bool ok = true;
    size_t length = 2 * chunks;
    while (length != 0 && current[length - 1] == 0) {
        length--;
    }
    if (ok && mo_bigint_sign(allocation) == MO_BIGINT_NEGATIVE) {
        ok = mo_buffer_append_byte(out, '-');
    }
    if (ok) {
        MO_ASSERT(length != 0);
        ok = mo_buffer_append_uint(out, (uint64_t)current[--length]);
    }
    while (ok && length != 0) {
        uint32_t digit = (uint32_t)current[--length];
        char text[9];
        for (unsigned i = 9; i != 0; i--) {
            text[i - 1] = (char)('0' + digit % 10);
            digit /= 10;
        }
        ok = mo_buffer_append(out, text, sizeof text);
    }
    mo_scratch_free(&temporary);
    return ok;
}
