#include "tools/inputs-internal.h"

#include "base/hash.h"

#include <string.h>

/* This key is public and fixed, not a process's private hash domain. */
static const uint64_t input_key[2] = {UINT64_C(0x6d6f2d70726f6d69), UINT64_C(0x73652d696e707574)};

uint64_t mo_input_random(uint64_t *state) {
    *state += UINT64_C(0x9e3779b97f4a7c15);
    uint64_t word = *state;
    word = (word ^ (word >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    word = (word ^ (word >> 27)) * UINT64_C(0x94d049bb133111eb);
    return word ^ (word >> 31);
}

static MO_MUST_USE uint64_t input_stream(uint64_t seed, uint64_t ordinal) {
    /* Advancing the starting state by SplitMix's step would overlap streams. */
    uint8_t bytes[16];
    for (unsigned i = 0; i < 8; i++) {
        bytes[i] = (uint8_t)(seed >> (8 * i));
        bytes[8 + i] = (uint8_t)(ordinal >> (8 * i));
    }
    return mo_hash(input_key, bytes, sizeof bytes);
}

struct mo_input_limits mo_input_limits_default(void) {
    return (struct mo_input_limits){.combinations = 200,
                                    .depth = 64,
                                    .nodes = 4096,
                                    .elements = 1024,
                                    .bytes = 64 << 10,
                                    .work = 1 << 20,
                                    .metadata = 64 << 20,
                                    .prepare_work = MO_LIMIT_INPUT_WORK};
}

enum mo_input_result mo_inputs_init(struct mo_inputs *inputs, const struct mo_types *types,
                                    const uint32_t *parameters, uint32_t count, const char *promise,
                                    size_t length, const struct mo_input_limits *limits,
                                    const struct mo_input_factory *factories,
                                    uint32_t factory_count) {
    MO_ASSERT(inputs != NULL && types != NULL && types->finished && limits != NULL);
    MO_ASSERT(count <= MO_LIMIT_BYTECODE_PARAMETERS && (parameters != NULL || count == 0));
    MO_ASSERT(promise != NULL || length == 0);
    MO_ASSERT(factories != NULL || factory_count == 0);
    *inputs = (struct mo_inputs){.types = types, .limits = *limits, .parameter_count = count};
    mo_arena_init(&inputs->arena, limits->metadata);
    if (limits->depth > MO_LIMIT_INPUT_DEPTH || limits->nodes > MO_LIMIT_INPUT_NODES ||
        limits->elements > MO_LIMIT_INPUT_ELEMENTS || limits->bytes > MO_LIMIT_INPUT_BYTES ||
        limits->work > MO_LIMIT_INPUT_WORK || limits->metadata > MO_LIMIT_INPUT_METADATA ||
        limits->prepare_work > MO_LIMIT_INPUT_WORK || length > limits->prepare_work) {
        return MO_INPUT_LIMIT;
    }
    inputs->seed = mo_hash(input_key, promise, length);
    for (uint32_t i = 0; i < count; i++) {
        MO_ASSERT(parameters[i] < types->count);
        inputs->parameters[i] = parameters[i];
    }
    enum mo_input_result result = mo_input_prepare(inputs, factories, factory_count, length);
    if (result != MO_INPUT_OK) {
        return result;
    }
    uint64_t product = 1;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t variants = inputs->info[parameters[i]].count;
        MO_ASSERT(variants != 0);
        inputs->counts[i] = variants;
        product =
            product > limits->combinations / variants ? limits->combinations : product * variants;
    }
    inputs->boundaries = (uint32_t)product;
    if (inputs->boundaries > limits->combinations) {
        inputs->boundaries = limits->combinations;
    }
    return MO_INPUT_OK;
}

void mo_inputs_free(struct mo_inputs *inputs) {
    mo_arena_free(&inputs->arena);
}

enum mo_input_result mo_inputs_next(struct mo_inputs *inputs, struct mo_heap *destination,
                                    struct mo_cell *out, enum mo_input_kind *kind) {
    MO_ASSERT(inputs != NULL && destination != NULL && kind != NULL);
    MO_ASSERT(out != NULL || inputs->parameter_count == 0);
    if (inputs->ordinal == UINT64_MAX || (inputs->parameter_count == 0 && inputs->ordinal != 0)) {
        return MO_INPUT_DONE;
    }
    uint32_t variants[MO_LIMIT_BYTECODE_PARAMETERS];
    uint64_t index = inputs->ordinal;
    bool boundary = inputs->ordinal < inputs->boundaries;
    for (uint32_t i = inputs->parameter_count; i != 0;) {
        i--;
        variants[i] = (uint32_t)(index % inputs->counts[i]);
        index /= inputs->counts[i];
    }
    uint64_t size = boundary ? 0 : 1 + (inputs->ordinal - inputs->boundaries) / 4;
    if (size > inputs->limits.elements) {
        size = inputs->limits.elements;
    }
    uint64_t work = inputs->limits.work;
    enum mo_input_result result =
        mo_input_build(inputs, destination, NULL, boundary ? variants : NULL, (uint32_t)size,
                       input_stream(inputs->seed, inputs->ordinal), NULL, &work, out);
    if (result != MO_INPUT_OK) {
        return result;
    }
    *kind = boundary ? MO_INPUT_BOUNDARY : MO_INPUT_RANDOM;
    inputs->ordinal++;
    return MO_INPUT_OK;
}

void mo_inputs_skip(struct mo_inputs *inputs) {
    MO_ASSERT(inputs != NULL && inputs->ordinal != UINT64_MAX);
    inputs->ordinal++;
}
