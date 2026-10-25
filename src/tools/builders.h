/* Promise inputs of types exposed without their insides (architecture 10.1, "Hidden types"). Such
 * a value is built only through its module: an exposed constant, or a call to an exposed function
 * that returns it, then up to three more calls that take it and return it. A call's arguments may
 * hold other hidden types of lower levels (MO_CLI_BUILDER_LEVELS), built by their own factories.
 * The calls run in a scratch process, so one that stops costs nothing but that step, and the
 * value is then copied into the trial's heap. These plug into mo_inputs_init as exact-type
 * factories. A value for the trial keeps the hidden value its last call returned beside it, which
 * the driver may use as another input (D284). */
#ifndef MO_BUILDERS_H
#define MO_BUILDERS_H

#include "check/check.h"
#include "tools/deadline.h"
#include "tools/inputs.h"
#include "vm/vm.h"

struct mo_hidden;
struct mo_build_recipe;
struct mo_build_call;
struct mo_builder_replay;

/* Private replay metadata owned by one trial, never by its VM values. */
struct mo_build_trace {
    struct mo_arena arena;
    struct mo_build_recipe *roots;
    struct mo_process *snapshots;
    struct mo_build_call *frozen;
    struct mo_input_limits snapshot_limits;
    uint64_t ordinal;
    uint32_t next_id;
    bool complete;
    struct {
        uint32_t origin, parameter, type;
    } bindings[MO_CLI_BUILDER_COMPANIONS];
    uint32_t binding_count;
};

void mo_build_trace_free(struct mo_build_trace *trace);

/* A hidden value that the call building an input returned beside it, in the same tuple, as lease
 * returns the Queue it issued a Lease from (D284). It is owned by the trial's heap. */
struct mo_companion {
    uint32_t type;
    uint32_t owner; /* the type of the value it came with */
    uint32_t origin;
    struct mo_cell value;
};

struct mo_builders {
    struct mo_arena arena;
    const struct mo_image *image;
    const struct mo_limits *limits;
    struct mo_deadline *deadline;
    struct mo_hidden *hidden;
    struct mo_input_factory *factories;
    uint32_t count;
    /* Between mo_builders_begin and mo_builders_relate, values built into trial keep their
     * companions here. */
    struct mo_heap *trial;
    struct mo_companion companions[MO_CLI_BUILDER_COMPANIONS];
    uint32_t companion_count;
    struct mo_build_trace *recording;
    struct mo_build_recipe **scope;
    struct mo_builder_replay *replay;
};

/* One factory per hidden type in the image, over an initialized image. program names which
 * module constants are exposed. A hidden type with no way to start one within the levels gets a
 * factory that refuses, so it is never built from its insides. Calls poll deadline. Free after
 * every init. */
MO_MUST_USE enum mo_input_result mo_builders_init(struct mo_builders *builders,
                                                  const struct mo_program *program,
                                                  const struct mo_image *image,
                                                  const struct mo_limits *limits,
                                                  struct mo_deadline *deadline);
void mo_builders_free(struct mo_builders *builders);
/* Generating one input tuple into trial keeps the companions of the values built for it. */
void mo_builders_begin(struct mo_builders *builders, struct mo_heap *trial,
                       struct mo_build_trace *trace, uint64_t ordinal);
/* With swap, each input of a type some companion has (count owned cells of types, in the trial's
 * heap) is replaced by the first such companion, so it is the value that came from the same call.
 * An input of the type that companion came with then keeps its own value, so a pair of types that
 * each come with the other isn't swapped crosswise. Every other companion is dropped. Ends what
 * mo_builders_begin started. */
void mo_builders_relate(struct mo_builders *builders, struct mo_cell *inputs, const uint32_t *types,
                        uint32_t count, bool swap);

struct mo_builder_shrinker {
    struct mo_builders *builders;
    uint32_t node, mask, attempts;
    uint32_t call;
    const struct mo_inputs *arguments;
    struct mo_process *current, *proposed;
    struct mo_cell current_args[MO_LIMIT_BYTECODE_PARAMETERS];
    struct mo_cell proposed_args[MO_LIMIT_BYTECODE_PARAMETERS];
    struct mo_input_shrinker argument_shrinker;
    bool arguments_phase, changed;
    bool awaiting;
};

void mo_builder_shrink_init(struct mo_builder_shrinker *shrinker, struct mo_builders *builders);
void mo_builder_shrink_free(struct mo_builder_shrinker *shrinker);
MO_MUST_USE enum mo_input_result
mo_builder_shrink_next(struct mo_builder_shrinker *shrinker, struct mo_inputs *inputs,
                       const struct mo_build_trace *best, struct mo_heap *destination,
                       struct mo_build_trace *pending, struct mo_cell *out);
void mo_builder_shrink_feedback(struct mo_builder_shrinker *shrinker, bool still_fails);

static inline MO_MUST_USE const struct mo_input_factory *
mo_builders_factories(const struct mo_builders *builders) {
    return builders->factories;
}
static inline MO_MUST_USE uint32_t mo_builders_count(const struct mo_builders *builders) {
    return builders->count;
}

#endif
