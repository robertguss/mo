#ifndef MO_LOWER_CPS_H
#define MO_LOWER_CPS_H

#include "lower/internal.h"

/* CPS is a work list, not C calls. A continuation may be shared by branches;
 * mutable values cross that boundary as explicit join parameters. */
struct variable {
    const struct mo_checked_binding *binding;
    struct variable *next;
};
struct continuation;
struct loop {
    struct continuation *break_, *continue_;
};
struct state {
    struct variable *variables;
    struct loop *loop;
    struct continuation *return_;
};
enum continuation_kind {
    K_END,
    K_SEQUENCE,
    K_BIND,
    K_ARGUMENTS,
    K_JUMP,
    K_SELECT,
    K_PATTERN,
    K_MATCH,
    K_FOR,
    K_OPTION,
    K_TRY,
    K_TEXT,
    K_PROMISE,
    K_ANCHOR
};
struct anchor_value {
    uint32_t expression, slot;
};
struct continuation {
    enum continuation_kind kind;
    struct continuation *next;
    const struct mo_checked_expression *row;
    uint32_t at, skip;
    mo_core_string name;
    const struct mo_checked_binding *binding;
    mo_core_expr_list arguments;
    struct variable *variables;
    bool result, diagnostic;
    mo_core_expr extra, then_, else_;
    mo_core_string position;
    mo_core_ty position_type;
    const struct mo_checked_pattern *pattern;
    const struct anchor_value *values;
};
enum task_kind { TASK_ROW, TASK_FINISH, TASK_CONDITION, TASK_PATTERN, TASK_ARM, TASK_COPY };
struct task {
    enum task_kind kind;
    struct task *next;
    uint32_t row;
    struct state state;
    struct continuation *continuation;
    mo_core_expr *out, *success, value, then_, else_;
    const struct mo_checked_pattern *pattern;
    const struct mo_checked_arm *arm;
    bool diagnostic;
};
struct body {
    struct lowering *p;
    struct instance *instance;
    const struct mo_checked_body *source;
    mo_core_string *names;
    struct task *tasks;
};

static inline const struct mo_checked_expression *lower_cps_row(struct body *b, uint32_t index) {
    MO_ASSERT(index < b->source->count);
    return &b->source->items[index];
}
static inline mo_core_ty lower_cps_type(struct body *b, const struct mo_source_type *source) {
    return lower_type(b->p, source, &b->instance->substitution);
}

struct continuation *lower_cps_continuation(struct body *b, enum continuation_kind kind);
mo_core_expr_list lower_cps_arguments(struct body *b, size_t count);
mo_core_expr lower_cps_projection(struct body *b, mo_core_expr value, size_t index);
mo_core_expr lower_cps_jump(struct body *b, struct continuation *k, mo_core_expr value);
mo_core_expr lower_cps_join(struct body *b, struct continuation *k, mo_core_ty result);
mo_core_expr lower_cps_field_read(struct body *b, mo_core_ty base, mo_core_expr value,
                                  uint32_t field);
mo_core_expr lower_cps_unary(struct body *b, struct mo_core_prim prim, mo_core_expr value);
enum mo_syntax_kind lower_cps_kind(const struct mo_checked_expression *r);
const struct mo_ability_implementation *lower_cps_ability(struct body *b,
                                                          const struct mo_checked_expression *r,
                                                          enum mo_source_ability wanted,
                                                          uint32_t operand);
bool lower_cps_ability_native(const struct mo_checked_expression *r);
bool lower_cps_direct_function(struct body *b, const struct mo_checked_expression *r);
mo_core_expr lower_cps_application(struct body *b, const struct mo_checked_expression *r,
                                   mo_core_expr_list args);

#endif
