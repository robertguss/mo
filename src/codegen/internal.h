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

MO_MUST_USE bool codegen_problem(struct compiler *c, uint16_t code, const char *reason);
MO_MUST_USE void *codegen_allocate(struct compiler *c, size_t count, size_t width);
MO_MUST_USE void *codegen_append(struct compiler *c, struct vector *v, uint32_t n, uint32_t limit,
                                 uint16_t code);
MO_MUST_USE bool codegen_service_shape(struct compiler *c, uint32_t type,
                                       const struct mo_service_shape *source);
MO_MUST_USE mo_core_string codegen_text(const char *s);
MO_MUST_USE struct mo_codegen_index *codegen_index_add(struct compiler *c,
                                                       struct mo_codegen_index **root,
                                                       mo_core_string key, uint32_t value,
                                                       bool copy);
MO_MUST_USE uint32_t codegen_string(struct compiler *c, mo_core_string s);
MO_MUST_USE bool codegen_name_valid(struct compiler *c, mo_core_string s);
MO_MUST_USE const char *codegen_name(struct compiler *c, mo_core_string s);
MO_MUST_USE uint32_t codegen_suffixed_name(struct compiler *c, mo_core_string base,
                                           const char *suffix);
MO_MUST_USE uint32_t codegen_type_result(struct compiler *c, enum mo_type_problem status,
                                         uint32_t id);
MO_MUST_USE uint32_t codegen_scalar(struct compiler *c, enum mo_type_kind kind);
MO_MUST_USE uint32_t codegen_type(struct compiler *c, mo_core_ty t, unsigned depth);
MO_MUST_USE uint32_t codegen_constant(struct compiler *c, struct mo_bytecode_constant value);
MO_MUST_USE uint32_t codegen_register_add(struct compiler *c, struct function *f, uint32_t ty,
                                          uint32_t original);
void codegen_function_init(struct function *f);
MO_MUST_USE struct node *codegen_prepare(struct compiler *c, struct function *f, mo_core_expr expr,
                                         uint32_t expected, unsigned depth);
MO_MUST_USE uint32_t codegen_emit(struct compiler *c, struct function *f,
                                  struct mo_core_span source, enum mo_opcode op,
                                  const uint32_t *fixed, uint32_t n, const uint32_t *variable,
                                  uint32_t count);
MO_MUST_USE bool codegen_entry_valid(struct compiler *c);
MO_MUST_USE bool codegen_declarations(struct compiler *c);
MO_MUST_USE bool codegen_globals(struct compiler *c);
MO_MUST_USE bool codegen_bodies(struct compiler *c);
MO_MUST_USE bool codegen_initializer(struct compiler *c, struct function *f, uint32_t index);
MO_MUST_USE bool codegen_service_descriptors(struct compiler *c, struct mo_bytecode *file);

#endif
