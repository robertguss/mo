#ifndef MO_INPUTS_INTERNAL_H
#define MO_INPUTS_INTERNAL_H

#include "tools/inputs.h"

struct mo_input_type_info {
    const struct mo_input_factory *factory;
    uint32_t *children;
    uint32_t *case_offsets;
    uint32_t *case_layouts;
    uint32_t child_count;
    uint32_t count;
    uint32_t depth;
    uint32_t base_case;
    uint32_t layout;
    uint32_t order;
    uint32_t visit;
    bool reachable;
    bool hashable;
};

struct mo_input_budget {
    struct mo_input_limits left;
};

static inline MO_MUST_USE bool mo_input_work(struct mo_input_budget *budget, uint64_t work) {
    if (work > budget->left.work) {
        return false;
    }
    budget->left.work -= work;
    return true;
}

static inline MO_MUST_USE bool mo_input_bytes(struct mo_input_budget *budget, size_t bytes) {
    if (bytes > budget->left.bytes || !mo_input_work(budget, bytes)) {
        return false;
    }
    budget->left.bytes -= bytes;
    return true;
}

struct mo_input_walk_frame {
    struct mo_cell value;
    struct mo_cell pending;
    uint32_t type;
    uint32_t count;
    uint32_t index;
    uint32_t position;
    bool emitted;
};

struct mo_input_walk {
    const struct mo_inputs *inputs;
    const struct mo_cell *roots;
    struct mo_input_budget budget;
    struct mo_input_walk_frame stack[MO_LIMIT_INPUT_DEPTH];
    uint32_t depth;
    uint32_t parameter;
    uint32_t end;
};

/* Reads source cells only; these helpers never retain foreign ownership. */
MO_MUST_USE enum mo_input_result mo_input_walk_next(struct mo_input_walk *walk, uint32_t *type,
                                                    struct mo_cell *value);
MO_MUST_USE enum mo_input_result mo_input_frame_init(const struct mo_inputs *inputs,
                                                     struct mo_input_walk_frame *frame,
                                                     uint32_t type, struct mo_cell value,
                                                     struct mo_input_budget *budget);
MO_MUST_USE struct mo_cell mo_input_child(const struct mo_inputs *inputs,
                                          struct mo_input_walk_frame *frame, uint32_t *type);
MO_MUST_USE uint32_t mo_input_skip_base(const struct mo_inputs *inputs, uint32_t type);
MO_MUST_USE uint32_t mo_input_variants(const struct mo_inputs *inputs, uint32_t parent,
                                       uint32_t child);
MO_MUST_USE uint32_t mo_input_scalar_count(enum mo_type_kind kind);
MO_MUST_USE uint64_t mo_input_random(uint64_t *state);
MO_MUST_USE enum mo_input_result mo_input_prepare(struct mo_inputs *inputs,
                                                  const struct mo_input_factory *factories,
                                                  uint32_t factory_count, size_t name_work);

enum mo_input_edit_kind {
    MO_INPUT_EDIT_SCALAR,
    MO_INPUT_EDIT_ERASE,
    MO_INPUT_EDIT_BASE,
    MO_INPUT_EDIT_FACTORY
};
struct mo_input_edit {
    uint64_t node;
    enum mo_input_edit_kind kind;
    uint32_t variant;
    uint32_t start;
    uint32_t count;
};

/* source=NULL generates. Otherwise copy its tree and apply one edit, or just
 * copy with edit=NULL. On refusal all partial owned cells are dropped. */
MO_MUST_USE enum mo_input_result mo_input_build(const struct mo_inputs *inputs,
                                                struct mo_heap *destination,
                                                const struct mo_cell *source,
                                                const uint32_t *boundaries, uint32_t size,
                                                uint64_t seed, const struct mo_input_edit *edit,
                                                uint64_t *work_left, struct mo_cell *out);
enum mo_input_mode { MO_INPUT_MINIMUM, MO_INPUT_SAMPLE, MO_INPUT_PRNG, MO_INPUT_COPY };
struct mo_input_value_request {
    uint32_t type;
    enum mo_input_mode mode;
    uint32_t boundary;
    uint32_t remaining_depth;
    struct mo_cell source;
    const struct mo_input_edit *edit;
};
struct mo_input_builder {
    const struct mo_inputs *inputs;
    struct mo_heap *heap;
    struct mo_scratch scratch;
    struct mo_input_budget budget;
    uint64_t random;
    uint64_t seen;
    uint32_t size;
    bool edited;
};
MO_MUST_USE enum mo_input_result mo_input_scalar_build(struct mo_input_builder *builder,
                                                       const struct mo_input_value_request *request,
                                                       struct mo_cell *out);
/* No allocation: shrinking can filter unchanged immediates before rebuilding
 * a containing record or tuple. */
MO_MUST_USE enum mo_input_result
mo_input_immediate_build(struct mo_input_builder *builder,
                         const struct mo_input_value_request *request, struct mo_cell *out);

#endif
