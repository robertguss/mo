/* Promise inputs, without VM calls or timing (architecture 10.1). */
#ifndef MO_INPUTS_H
#define MO_INPUTS_H

#include "heap/heap.h"

enum mo_input_result {
    MO_INPUT_OK,
    MO_INPUT_DONE,
    MO_INPUT_UNSUPPORTED,
    MO_INPUT_UNINHABITED,
    MO_INPUT_LIMIT,
    MO_INPUT_NO_MEMORY,
};

enum mo_input_kind { MO_INPUT_BOUNDARY, MO_INPUT_RANDOM };

struct mo_input_limits {
    uint32_t combinations;
    uint32_t depth;
    uint32_t nodes;
    uint32_t elements;
    size_t bytes;
    uint64_t work;
};

MO_MUST_USE struct mo_input_limits mo_input_limits_default(void);

struct mo_input_type_info;
/* Private fields. The sealed types outlive the generator. */
struct mo_inputs {
    struct mo_arena arena;
    const struct mo_types *types;
    struct mo_input_limits limits;
    struct mo_input_type_info *info;
    uint32_t parameters[MO_LIMIT_BYTECODE_PARAMETERS];
    uint32_t counts[MO_LIMIT_BYTECODE_PARAMETERS];
    uint32_t parameter_count;
    uint32_t boundaries;
    uint64_t seed;
    uint64_t ordinal;
};

/* parameters are valid indices in mo_image_types(image), without an implicit
 * env or the promise's result. The seed hashes the full promise name, not its
 * subject's name, with the generator's fixed key. Free after every init,
 * including a refusal. No function, env or capability inputs yet. */
MO_MUST_USE enum mo_input_result mo_inputs_init(struct mo_inputs *inputs,
                                                const struct mo_types *types,
                                                const uint32_t *parameters, uint32_t count,
                                                const char *promise, size_t length,
                                                const struct mo_input_limits *limits);
void mo_inputs_free(struct mo_inputs *inputs);

/* Success writes count owned cells belonging to destination and advances one
 * input. On refusal, out, kind and the sequence are unchanged, with no retained
 * values. NULL out is allowed only with no parameters. The caller may retain
 * these inputs and pass owned duplicates to mo_call. Empty parameter lists
 * have one input. Otherwise random inputs continue until the caller's budget.
 * The destination belongs to the same type table; it may change each step. */
MO_MUST_USE enum mo_input_result mo_inputs_next(struct mo_inputs *inputs,
                                                struct mo_heap *destination, struct mo_cell *out,
                                                enum mo_input_kind *kind);
/* Skip an input which exceeds generation limits; failed allocation can instead
 * be retried with more heap. Each input has its own random stream. */
void mo_inputs_skip(struct mo_inputs *inputs);

#endif
