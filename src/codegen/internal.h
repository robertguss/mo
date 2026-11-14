#ifndef MO_CODEGEN_INTERNAL_H
#define MO_CODEGEN_INTERNAL_H

#include "bytecode/bytecode.h"
#include "codegen/codegen.h"
#include "codegen/index.h"
#include "codegen/operations.h"
#include "heap/types.h"

/* Arrays grow geometrically. Retained arena blocks cost less than twice the final capacity. */
struct vector {
    void *items;
    uint32_t count, capacity;
    size_t width;
};
enum ref_state { REF_UNDEFINED, REF_BORROWED, REF_OWNED, REF_MOVED };
struct reg {
    uint32_t type, name;
    uint32_t flow, previous, next;
    uint32_t mark;
    enum ref_state state, goal;
    bool owned, protected;
};
struct change {
    uint32_t reg;
    enum ref_state previous;
};
struct ownership {
    uint32_t type, edges;
    bool owned;
};
struct edge {
    uint32_t to, next;
};
struct capture {
    uint32_t source, reg;
};
struct constraint {
    uint32_t type;
    uint8_t ability;
    const struct mo_core_fun *function;
    struct mo_core_span span, declaration;
};
struct live_set {
    uint32_t *items;
    uint32_t count, capacity;
};
struct node {
    mo_core_expr core;
    struct mo_core_span span;
    uint32_t type, reg, index;
    uint32_t count, bind_count;
    struct node **children;
    uint32_t *binds, *cases;
    struct mo_codegen_operation operation;
    uint32_t signature;
    bool *last;
    struct live_set after, work;
    bool temporary;
};
struct join {
    struct node *node;
    uint32_t offset, checkpoint;
    struct live_set live;
    bool *used;
    bool reachable; /* generate will emit at least one jump to this continuation. */
};
struct function {
    struct vector regs, code, spans, names, captures, joins, changes;
    struct node *body;
    struct mo_codegen_index *locals, *bindings, *zeros, *closure_names, *capture_index, *join_names;
    const struct mo_core_fun_meta *meta;
    const struct mo_core_fun *source;
    struct function *parent, *root;
    struct mo_core_span declaration;
    uint32_t lambda_number, closure_type;
    uint32_t preorder, instructions, owned, mark;
    uint32_t live_count, live_cells, reference_count;
    uint32_t *references;
    uint64_t live_work;
};
struct compiler {
    const struct mo_core *core;
    const struct mo_services *services;
    struct mo_arena arena;
    struct mo_types types;
    struct vector strings, constants, modules, constructors, ownership, edges, constraints, shapes;
    struct mo_codegen_index *string_index, *constant_index, *data_index, *fun_index, *global_index,
        *module_index, *builtin_index, *annotation_index, *operation_index;
    struct mo_codegen_index *bigint_index[2];
    uint32_t *data, *layouts;
    struct mo_bytecode_bytes *files;
    uint32_t range_int, function_count, function_capacity, unit_constant;
    struct mo_bytecode_global *globals;
    uint32_t *init;
    struct mo_bytecode_function *funs;
    struct mo_parameter **parameters;
    struct mo_bytecode_debug *debug;
    struct function *bodies;
    uint32_t debug_entries;
    const struct mo_core_fun *current;
    struct mo_core_span span, declaration;
    uint16_t error;
    const char *reason;
    size_t bigint_work;
};

static inline MO_MUST_USE bool codegen_reference(struct compiler *c, uint32_t type_id) {
    return type_id != MO_NO_INDEX && mo_type_is_reference(&c->types, type_id);
}
static inline MO_MUST_USE uint32_t codegen_flow_id(struct function *f, uint32_t r) {
    return r == MO_NO_INDEX ? MO_NO_INDEX : ((struct reg *)f->regs.items)[r].flow;
}
static inline MO_MUST_USE uint32_t codegen_live_position(const struct live_set *live, uint32_t r) {
    uint32_t lo = 0, hi = live->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (live->items[mid] < r)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
static inline MO_MUST_USE bool codegen_live_has(const struct live_set *live, uint32_t r) {
    uint32_t at = codegen_live_position(live, r);
    return at < live->count && live->items[at] == r;
}

#endif
