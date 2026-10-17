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
    /* Base and one-parameter variations, then the remaining Cartesian product
     * (last parameter fastest), before random trials. A smaller cap truncates. */
    uint32_t combinations;
    /* Per input: tree height, nodes (including scalars), collection entries,
     * total Text/Bytes/BigInt payload bytes, and walk/build work. */
    uint32_t depth;
    uint32_t nodes;
    uint32_t elements;
    size_t bytes;
    uint64_t work;
    /* Preparation has its own arena and work budget. */
    size_t metadata;
    uint64_t prepare_work;
};

MO_MUST_USE struct mo_input_limits mo_input_limits_default(void);

struct mo_input_request {
    enum mo_input_kind kind;
    uint32_t boundary;
    uint32_t size;
    uint64_t seed;
};

/* The driver supplies fakes (tools/fakes.h) here; later stages add hidden-type builders and
 * closures. Entries
 * override an exact type index; boundary 0 is the minimum. Callbacks are
 * deterministic, honor the remaining limits, and return a valid owned cell
 * in destination, preserving out on refusal. The library counts the root as
 * one node; callbacks subtract child nodes, payload bytes and work from
 * remaining, on refusal too. Other limits are read-only; depth is the space
 * left on this path. copy rebuilds
 * a borrowed value in a possibly different heap, never retaining foreign
 * references. shrink returns DONE after its last candidate. Callback tables
 * and contexts outlive the generator. NULL copy/shrink disables shrinking
 * inputs containing that override. hashable must match the runtime ability. */
struct mo_input_factory {
    uint32_t type;
    uint32_t boundaries;
    bool hashable;
    void *context;
    enum mo_input_result (*build)(void *context, struct mo_heap *destination,
                                  const struct mo_input_request *request,
                                  struct mo_input_limits *remaining, struct mo_cell *out);
    enum mo_input_result (*copy)(void *context, struct mo_heap *destination, struct mo_cell source,
                                 struct mo_input_limits *remaining, struct mo_cell *out);
    enum mo_input_result (*shrink)(void *context, struct mo_heap *destination,
                                   struct mo_cell source, uint32_t candidate,
                                   struct mo_input_limits *remaining, struct mo_cell *out);
};

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
    uint32_t axes;
    uint64_t seed;
    uint64_t ordinal;
};

/* parameters are valid indices in mo_image_types(image), without an implicit
 * env or the promise's result. The seed hashes the full promise name, not its
 * subject's name, with the generator's fixed key. Free after every init,
 * including a refusal. No function or env inputs yet; capability
 * inputs need a factory. */
MO_MUST_USE enum mo_input_result
mo_inputs_init(struct mo_inputs *inputs, const struct mo_types *types, const uint32_t *parameters,
               uint32_t count, const char *promise, size_t length,
               const struct mo_input_limits *limits, const struct mo_input_factory *factories,
               uint32_t factory_count);
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

/* A shrink session owns no values. Before next, best is the caller's borrowed
 * current failing tuple. Success returns an owned, deeply copied candidate in
 * destination. Retain it while testing it; on a still-failing verdict replace
 * best with it before the next step, otherwise drop it. feedback resumes at
 * the accepted node, retaining numeric distance and restarting structural
 * edits there. A pass that accepts anything restarts at node zero; shrinking
 * stops after a pass accepts nothing or the call budget runs out. A refusal
 * preserves the cursor and output. The budget counts emitted candidates,
 * not skipped equal values.
 * Timing and classification of stopped calls remain with the driver. */
struct mo_input_shrinker {
    const struct mo_inputs *inputs;
    uint64_t node;
    uint32_t variant;
    uint32_t calls;
    uint32_t budget;
    bool awaiting;
    bool numeric;
    bool changed;
};
void mo_input_shrink_init(struct mo_input_shrinker *shrinker, const struct mo_inputs *inputs,
                          uint32_t budget);
MO_MUST_USE enum mo_input_result mo_input_shrink_next(struct mo_input_shrinker *shrinker,
                                                      const struct mo_cell *best,
                                                      struct mo_heap *destination,
                                                      struct mo_cell *out);
void mo_input_shrink_feedback(struct mo_input_shrinker *shrinker, bool still_fails);

enum mo_input_end { MO_INPUT_SMALLEST, MO_INPUT_LARGEST };
struct mo_input_extreme {
    uint32_t parameter;
    enum mo_type_kind type;
    enum mo_input_end end;
};
/* Find the first Int/Decimal/Time/Duration extreme inside parameter, walking
 * fields and collection entries in order, including Range's Int endpoints
 * (start before end). DONE means none; out is unchanged
 * unless OK. Overrides are opaque. No allocation and no ownership changes. */
MO_MUST_USE enum mo_input_result mo_inputs_extreme(const struct mo_inputs *inputs,
                                                   const struct mo_cell *values, uint32_t parameter,
                                                   struct mo_input_extreme *out);

#endif
