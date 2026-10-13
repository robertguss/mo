#include "tools/inputs.h"

#include "base/hash.h"

#include <string.h>

/* This key is public and fixed, not a process's private hash domain. */
static const uint64_t input_key[2] = {UINT64_C(0x6d6f2d70726f6d69), UINT64_C(0x73652d696e707574)};

static MO_MUST_USE uint64_t random_word(uint64_t *state) {
    *state += UINT64_C(0x9e3779b97f4a7c15);
    uint64_t word = *state;
    word = (word ^ (word >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    word = (word ^ (word >> 27)) * UINT64_C(0x94d049bb133111eb);
    return word ^ (word >> 31);
}

struct mo_input_limits mo_input_limits_default(void) {
    return (struct mo_input_limits){.combinations = 200,
                                    .depth = 64,
                                    .nodes = 4096,
                                    .elements = 1024,
                                    .bytes = 64 << 10,
                                    .work = 1 << 20};
}

enum mo_input_result mo_inputs_init(struct mo_inputs *inputs, const struct mo_types *types,
                                    const uint32_t *parameters, uint32_t count, const char *promise,
                                    size_t length, const struct mo_input_limits *limits) {
    MO_ASSERT(inputs != NULL && types != NULL && types->finished && limits != NULL);
    MO_ASSERT(count <= MO_LIMIT_BYTECODE_PARAMETERS && (parameters != NULL || count == 0));
    MO_ASSERT(promise != NULL || length == 0);
    *inputs = (struct mo_inputs){.types = types,
                                 .limits = *limits,
                                 .parameter_count = count,
                                 .seed = mo_hash(input_key, promise, length)};
    mo_arena_init(&inputs->arena, 64 << 20);
    uint64_t product = 1;
    for (uint32_t i = 0; i < count; i++) {
        MO_ASSERT(parameters[i] < types->count);
        inputs->parameters[i] = parameters[i];
        if (types->items[parameters[i]].kind != MO_TYPE_INT) {
            return MO_INPUT_UNSUPPORTED;
        }
        inputs->counts[i] = 5;
        product = product > limits->combinations / 5 ? limits->combinations : product * 5;
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
    if (inputs->parameter_count > inputs->limits.nodes ||
        inputs->parameter_count > inputs->limits.work) {
        return MO_INPUT_LIMIT;
    }
    struct mo_cell values[MO_LIMIT_BYTECODE_PARAMETERS];
    uint64_t index = inputs->ordinal;
    uint64_t state = inputs->seed + inputs->ordinal * UINT64_C(0x9e3779b97f4a7c15);
    bool boundary = inputs->ordinal < inputs->boundaries;
    static const int64_t integers[] = {0, 1, -1, INT64_MIN, INT64_MAX};
    for (uint32_t i = inputs->parameter_count; i != 0;) {
        i--;
        int64_t number;
        if (boundary) {
            number = integers[index % 5];
            index /= 5;
        } else {
            uint64_t word = random_word(&state);
            number = word <= INT64_MAX ? (int64_t)word : -1 - (int64_t)(UINT64_MAX - word);
        }
        values[i] = mo_cell_int(number);
    }
    if (inputs->parameter_count != 0) {
        memcpy(out, values, inputs->parameter_count * sizeof *out);
    }
    *kind = boundary ? MO_INPUT_BOUNDARY : MO_INPUT_RANDOM;
    inputs->ordinal++;
    return MO_INPUT_OK;
}

void mo_inputs_skip(struct mo_inputs *inputs) {
    MO_ASSERT(inputs != NULL && inputs->ordinal != UINT64_MAX);
    inputs->ordinal++;
}
