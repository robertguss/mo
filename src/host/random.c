#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "host/internal.h"

#include "base/hash.h"

#include <string.h>
#include <sys/random.h>
#include <unistd.h>

struct random_state {
    uint64_t words[4];
};

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
        state.words[i] = mo_splitmix64(&seed);
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
