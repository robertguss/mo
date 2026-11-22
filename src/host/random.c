#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "host/internal.h"

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

struct random_state {
    uint64_t words[4];
};

struct secure_random_state {
    uint64_t seed;
    uint64_t counter;
};

static MO_MUST_USE uint64_t mix(uint64_t *seed) {
    *seed += UINT64_C(0x9e3779b97f4a7c15);
    uint64_t z = *seed;
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

static MO_MUST_USE uint64_t rotate(uint64_t x, unsigned shift) {
    return (x << shift) | (x >> (64 - shift));
}

static MO_MUST_USE uint64_t next(struct random_state *s) {
    uint64_t result = rotate(s->words[1] * 5, 7) * 9;
    uint64_t t = s->words[1] << 17;
    s->words[2] ^= s->words[0];
    s->words[3] ^= s->words[1];
    s->words[1] ^= s->words[2];
    s->words[0] ^= s->words[3];
    s->words[2] ^= t;
    s->words[3] = rotate(s->words[3], 45);
    return result;
}

bool mo_host_random_restore(struct mo_heap *heap, uint32_t type, bool fake, const uint64_t words[4],
                            struct mo_cell *out) {
    MO_ASSERT((words[0] | words[1] | words[2] | words[3]) != 0);
    struct random_state state;
    memcpy(state.words, words, sizeof state.words);
    struct mo_capability_info info = {.operations = &mo_host_operations[fake][MO_CAP_RANDOM]};
    return mo_value_capability_native(heap, type, &info, &state, sizeof state, out);
}

void mo_host_random_snapshot(struct mo_cell capability, uint64_t words[4]) {
    const struct random_state *state =
        (const struct random_state *)mo_capability_get(capability)->handle;
    memcpy(words, state->words, sizeof state->words);
}

enum mo_host_outcome mo_host_random_new(struct mo_host *host, struct mo_heap *heap, uint32_t type,
                                        bool fake, uint64_t seed, struct mo_cell *out,
                                        struct mo_host_error *error) {
    (void)host;
    if (!fake && getentropy(&seed, sizeof seed) != 0) {
        *error = (struct mo_host_error){.reason = "internal error: system randomness unavailable"};
        return MO_HOST_INTERNAL;
    }
    struct random_state state;
    for (unsigned i = 0; i < 4; i++)
        state.words[i] = mix(&seed);
    /* SplitMix64 is bijective: four distinct inputs cannot all produce zero. */
    MO_ASSERT((state.words[0] | state.words[1] | state.words[2] | state.words[3]) != 0);
    return mo_host_random_restore(heap, type, fake, state.words, out) ? MO_HOST_RETURNED
                                                                      : MO_HOST_OUT_OF_MEMORY;
}

enum mo_host_outcome mo_host_random(struct mo_host *host, struct mo_heap *heap, uint32_t type,
                                    const struct mo_cell *args, struct mo_cell *out,
                                    struct mo_host_error *error) {
    (void)host;
    (void)heap;
    (void)type;
    int64_t low = mo_cell_get_int(args[1]), high = mo_cell_get_int(args[2]);
    if (high <= low)
        return mo_host_bug(error, "high not greater than low");
    struct random_state *state = (struct random_state *)mo_capability_get(args[0])->handle;
    struct random_state initial = *state;
    uint64_t width = (uint64_t)high - (uint64_t)low;
    uint64_t threshold = (UINT64_C(0) - width) % width;
    uint64_t offset;
    for (;;) {
        mo_u128 product = (mo_u128)next(state) * width;
        if ((uint64_t)product >= threshold) {
            offset = (uint64_t)(product >> 64);
            break;
        }
        /* A nonzero xoshiro state has period 2^256 - 1; a period contains an
         * accepted draw. Detect a broken generator instead of looping forever. */
        MO_ASSERT(memcmp(state, &initial, sizeof initial) != 0);
    }
    mo_i128 result = (mo_i128)low + offset;
    MO_ASSERT(result >= low && result < high);
    *out = mo_cell_int((int64_t)result);
    return MO_HOST_RETURNED;
}

static void write_le64(uint8_t out[8], uint64_t value) {
    for (unsigned i = 0; i < 8; i++)
        out[i] = (uint8_t)((value >> (i * 8)) & 0xffU);
}

bool mo_host_secure_random_restore(struct mo_heap *heap, uint32_t type, const uint64_t words[2],
                                   struct mo_cell *out) {
    struct secure_random_state state = {.seed = words[0], .counter = words[1]};
    struct mo_capability_info info = {.operations =
                                          &mo_host_operations[true][MO_CAP_SECURE_RANDOM]};
    return mo_value_capability_native(heap, type, &info, &state, sizeof state, out);
}

void mo_host_secure_random_snapshot(struct mo_cell capability, uint64_t words[2]) {
    const struct secure_random_state *state =
        (const struct secure_random_state *)mo_capability_get(capability)->handle;
    words[0] = state->seed;
    words[1] = state->counter;
}

enum mo_host_outcome mo_host_secure_random_new(struct mo_host *host, struct mo_heap *heap,
                                               uint32_t type, uint64_t seed, struct mo_cell *out,
                                               struct mo_host_error *error) {
    (void)host;
    (void)error;
    uint64_t words[2] = {seed, 0};
    return mo_host_secure_random_restore(heap, type, words, out) ? MO_HOST_RETURNED
                                                                : MO_HOST_OUT_OF_MEMORY;
}

enum mo_host_outcome mo_host_secure_random_bytes(struct mo_host *host, struct mo_heap *heap,
                                                 uint32_t type, const struct mo_cell *args,
                                                 struct mo_cell *out, struct mo_host_error *error) {
    (void)host;
    (void)type;
    int64_t count = mo_cell_get_int(args[1]);
    if (count < 0 || count > MO_LIMIT_SECURE_RANDOM_BYTES)
        return mo_host_bug(error, "count outside 0 to 65536");
    size_t length = (size_t)count;
    if (length == 0)
        return mo_value_bytes(heap, (const uint8_t *)"", 0, out) ? MO_HOST_RETURNED
                                                                 : MO_HOST_OUT_OF_MEMORY;
    const struct mo_capability_info *info = mo_capability_get(args[0]);
    const struct mo_host_operations *operations = info->operations;
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, heap);
    uint8_t *buffer = mo_scratch_resize(&scratch, NULL, 0, length);
    if (buffer == NULL) {
        mo_scratch_free(&scratch);
        return MO_HOST_OUT_OF_MEMORY;
    }
    if (!operations->fake) {
        ERR_clear_error();
        int rc = RAND_bytes(buffer, (int)length);
        ERR_clear_error();
        if (rc != 1) {
            OPENSSL_cleanse(buffer, length);
            mo_scratch_free(&scratch);
            *error = (struct mo_host_error){
                .reason = "internal error: cryptographic randomness unavailable"};
            return MO_HOST_INTERNAL;
        }
    } else {
        struct secure_random_state *state = (struct secure_random_state *)info->handle;
        MO_ASSERT(state != NULL);
        size_t offset = 0;
        uint8_t input[24] = {'M', 'O', '_', 'S', 'R', 'N', 'G', 0};
        uint8_t block[32];
        write_le64(input + 8, state->seed);
        while (offset < length) {
            write_le64(input + 16, state->counter);
            unsigned block_len = 0;
            ERR_clear_error();
            int rc = EVP_Digest(input, sizeof input, block, &block_len, EVP_sha256(), NULL);
            ERR_clear_error();
            if (rc != 1 || block_len != sizeof block) {
                OPENSSL_cleanse(block, sizeof block);
                OPENSSL_cleanse(buffer, length);
                mo_scratch_free(&scratch);
                *error = (struct mo_host_error){
                    .reason = "internal error: cryptographic digest unavailable"};
                return MO_HOST_INTERNAL;
            }
            state->counter++;
            size_t take = length - offset < sizeof block ? length - offset : sizeof block;
            memcpy(buffer + offset, block, take);
            offset += take;
        }
        OPENSSL_cleanse(block, sizeof block);
        OPENSSL_cleanse(input, sizeof input);
    }
    bool ok = mo_value_bytes(heap, buffer, length, out);
    OPENSSL_cleanse(buffer, length);
    mo_scratch_free(&scratch);
    return ok ? MO_HOST_RETURNED : MO_HOST_OUT_OF_MEMORY;
}
