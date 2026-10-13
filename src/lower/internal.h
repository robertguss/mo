#ifndef MO_LOWER_INTERNAL_H
#define MO_LOWER_INTERNAL_H

#include "lower/lower.h"

#include "base/codes.h"
#include "base/limits.h"

#include <setjmp.h>

struct substitution {
    uint32_t count;
    const struct mo_source_type **parameters;
    mo_core_ty *types;
};

struct instance {
    struct instance *next, *hash_next, *source_next;
    const struct mo_checked_function *source;
    struct substitution substitution;
    uint32_t owner, depth, index;
    struct mo_core_fun fun;
    struct mo_core_fun_meta meta;
};

struct data_instance {
    struct data_instance *next;
    const struct mo_source_type *source;
    struct substitution substitution;
    uint32_t owner;
    mo_core_ty type;
    struct mo_core_data data;
};

struct type_cache;
struct canonical_type;
struct type_frame;
struct derived;
struct lower_node {
    struct mo_core_expr expr; /* first, so every emitted Expr carries private provenance */
    struct mo_core_span span;
    mo_core_names binders;
    mo_core_string closure;
};

struct lowering {
    struct mo_arena *arena;
    const struct mo_program *program;
    const struct mo_sources *sources;
    struct mo_diags *diags;
    struct mo_span span;
    jmp_buf escape;
    uint64_t work;
    uint32_t nodes, type_count, serial, function_count, data_count;
    struct instance *functions, **function_tail;
    struct data_instance *data, **data_tail;
    struct type_cache **cache;
    struct canonical_type **types;
    struct type_frame *type_stack;
    uint32_t cache_count, cache_slots, type_slots, instance_slots;
    struct instance **instance_index, **by_source, ***source_tails;
    const struct mo_checked_function **source_index;
    struct derived *derived, **derived_tail, *derived_pending;
    struct derived **derived_index;
    uint32_t derived_count, derived_slots;
};

_Noreturn void lower_refuse(struct lowering *p, uint16_t code, const char *message);
void lower_work(struct lowering *p, size_t amount);
void *lower_allocate(struct lowering *p, size_t count, size_t size, size_t alignment);
#define LOWER_NEW(p, type, count) lower_allocate((p), (count), sizeof(type), _Alignof(type))
mo_core_string lower_copy(struct lowering *p, const char *bytes, size_t length);
mo_core_string lower_text(struct lowering *p, const char *text);
mo_core_string lower_number(struct lowering *p, uint64_t number);
mo_core_string lower_concat(struct lowering *p, mo_core_string a, const char *separator,
                            mo_core_string b);
mo_core_string lower_fresh(struct lowering *p);
bool lower_same(mo_core_string a, mo_core_string b);
struct mo_core_span lower_span(struct lowering *p, struct mo_span span);

mo_core_ty lower_type(struct lowering *p, const struct mo_source_type *type,
                      const struct substitution *substitution);
void lower_indices(struct lowering *p);
mo_core_string lower_type_name(struct lowering *p, mo_core_ty type);
struct instance *lower_instance(struct lowering *p, const struct mo_symbol *symbol,
                                struct mo_type_arguments arguments,
                                const struct substitution *substitution, uint32_t owner,
                                uint32_t depth);
void lower_data(struct lowering *p, struct data_instance *instance);
mo_core_ctors lower_ctors(struct lowering *p, mo_core_ty type);
mo_core_expr lower_derived_text(struct lowering *p, const struct mo_ability_implementation *proof,
                                const struct substitution *substitution, uint32_t owner,
                                uint32_t depth, mo_core_expr value);
void lower_derived_functions(struct lowering *p);

mo_core_expr lower_expr(struct lowering *p, enum mo_core_expr_tag tag);
mo_core_expr lower_var(struct lowering *p, mo_core_string name);
mo_core_expr lower_unit(struct lowering *p);
mo_core_expr lower_bool(struct lowering *p, bool value);
mo_core_expr lower_int(struct lowering *p, uint64_t value);
mo_core_expr lower_prim(struct lowering *p, struct mo_core_prim prim, mo_core_expr_list args);
mo_core_expr lower_child(mo_core_expr expr, size_t at);
void lower_function(struct lowering *p, struct instance *instance);
void lower_metadata(struct lowering *p, struct mo_core *core);

#endif
