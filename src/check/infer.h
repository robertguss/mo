#ifndef MO_CHECK_INFER_H
#define MO_CHECK_INFER_H

#include "check/abilities.h"
#include "check/data.h"
#include "check/patterns.h"

enum numeric { INT = 1, BIG = 2, BYTE = 4, DECIMAL = 8, FLOAT = 16 };

struct term {
    struct mo_source_type shape;
    struct term **arguments;
    struct term *parent;
    const struct mo_source_type *resolved;
    uint32_t rank;
    uint32_t seen;
    uint32_t height; /* longest path to a leaf in a resolved type DAG */
    unsigned numeric;
    bool variable;
    bool partial_default;
    bool deferred;
};

struct local;
struct name_slot {
    const struct mo_syntax *name;
    const struct mo_syntax *module_use;
    const struct mo_syntax *local_name;
    struct name_slot *left, *right;
    struct local *active;
    struct local *expired;
    uint32_t height;
};

struct local {
    struct name_slot *slot;
    const struct mo_syntax *name;
    struct term *type;
    struct local *previous;
    struct local *next;
    struct note *binding;
    struct capture_write *writes;
    uint32_t function, version;
    uint32_t pattern_group, pattern_alternative;
    bool variable;
    bool used;
    bool statement; /* bound by `name = value`, where `var` would make it assignable */
};

struct emitted_lambda {
    struct mo_checked_lambda output;
    struct note **parameters;
    struct emitted_lambda *parent;
    struct emitted_lambda *next;
    struct capture *captures;
    const struct mo_syntax *syntax;
    struct capture_loop *loop;
    struct capture_path *path;
    uint32_t depth;
};

struct capture {
    struct local *local;
    struct mo_span use;
    struct capture *next;
};

struct capture_loop {
    const struct mo_syntax *syntax;
    struct capture_loop *parent;
};

struct capture_path {
    const struct mo_syntax *group;
    uint32_t arm;
    struct capture_path *parent;
};

struct capture_write {
    const struct mo_syntax *syntax, *target;
    struct capture_path *path;
    struct capture_write *next;
};

struct emitted_match {
    struct mo_checked_match output;
    struct mo_checked_arm *arms;
};

struct emitted {
    struct mo_checked_expression output;
    struct note **operands;
    struct emitted_lambda *lambda;
    struct emitted_match *match;
    const struct mo_source_type *generic;
    struct term *instantiated;
    struct term *pattern_type;
    struct term *try_result;
    struct failure_check *failure;
    const struct mo_source_type *function_type;
    const uint8_t *requirements;
    struct term **parameters; /* compact A-Z substitution terms */
};

/* M.to_text or M.compare: its graph takes only types the module M declares. */
struct ability_module {
    const struct emitted *graph;
    const struct mo_module_scope *module;
    struct ability_module *next;
};

struct note {
    struct mo_inferred_expression output;
    struct term *type;
    struct note *next;
    struct emitted *graph;
    struct note *binding;
    struct note *callee;
    uint32_t index;
    uint32_t binding_index;
    bool variable;
    bool discarded;
    bool negative;
};

struct note_range {
    struct note *first, *last;
};

struct typed_pattern {
    struct mo_match_pattern match;
    struct term *type;
    const struct mo_syntax *syntax;
    struct typed_pattern *next;
};

struct match_check {
    struct mo_match_pattern **patterns;
    uint32_t count;
    uint32_t end;
    struct mo_span span;
    const struct mo_syntax *scrutinee;
    struct term *type;
    const struct mo_syntax *catch_all;
    struct match_check *next;
};

struct pattern_defaults {
    const struct mo_data_signature *data;
    const struct mo_source_type *type;
    const struct mo_checked_pattern **fields;
    struct pattern_defaults *next;
};

struct failure_check {
    struct failure_check *next;
    struct term *input, *value, *context, *fallback;
    struct note *input_note;
    struct emitted *graph;
    struct mo_span span;
    struct mo_constructor_ref wrapper;
    const struct mo_source_type *generic;
    enum mo_syntax_token op;
    uint16_t code;
    bool solved, never;
};

struct type_pair {
    const struct mo_source_type *a, *b;
    struct type_pair *next;
};

struct failure_guess {
    struct term *term;
    const struct mo_source_type *type;
    struct failure_guess *next;
};

struct inference {
    struct mo_arena *arena;
    struct mo_diags *diags;
    const struct mo_module_interfaces *interfaces;
    const struct mo_signatures *signatures;
    const struct mo_function_signature *function;
    struct mo_constant_context *constants;
    struct mo_ability_context abilities;
    struct name_slot *names;
    struct local *locals, *all_locals;
    struct note *notes, *last;
    struct typed_pattern *patterns;
    struct match_check *matches;
    struct pattern_defaults *defaults;
    struct dispatch *dispatches, *last_dispatch;
    struct ability_module *ability_modules;
    struct failure_check *failures;
    struct type_pair *equal_types;
    struct emitted_lambda *lambda_frame, *lambdas;
    struct capture_loop *capture_loop;
    uint32_t capture_free; /* type parameters that lambdas capture, so can't hold capabilities */
    struct capture_path *capture_path;
    struct term *result;
    struct mo_span span;
    struct mo_diag error;
    struct mo_label secondary;
    struct mo_edit edit;
    struct mo_fix fix;
    struct local *unused, *error_unused;
    const char *type_label;
    uint16_t expectation, node_expectation;
    uint32_t count, binding_count, depth, loops, lambda, work, epoch;
    uint32_t pattern_group, pattern_depth, lambda_count;
    bool checked, spec;
    bool keep_patterns;
    bool failed, fatal, partial, unavailable;
};

struct value {
    struct term *type;
    const struct mo_syntax *binding;
    const struct mo_symbol *symbol;
    const struct mo_field_signature *field;
    const char *builtin_case;
    struct emitted *graph;
    struct note *note;
    struct note *bound;
    struct note *callee;
    struct dispatch *dispatch;
    bool never;
};

struct dispatch {
    struct dispatch *next;
    const struct mo_syntax *name;
    const struct mo_symbol *group;
    uint32_t count;
    struct term *type;
    struct emitted *graph;
    struct note *note;
    const struct mo_syntax *syntax, *call, *arguments;
    struct value receiver;
    struct value *inputs;
    bool position; /* `.0` on a tuple: `name` is the index */
    bool solved;
};

struct type_preview {
    char text[512];
    size_t length;
};

extern const char infer_unknown_member[];

MO_MUST_USE bool infer_problem(struct inference *p, uint16_t code, struct mo_span span,
                               const char *message, const char *reason,
                               const struct mo_syntax *previous);
void infer_defer(struct inference *p, uint16_t code, struct mo_span span, const char *message,
                 const char *reason);
void infer_emit_problem(struct inference *p);
MO_MUST_USE bool infer_unfinished(struct inference *p);
MO_MUST_USE bool infer_work(struct inference *p);
MO_MUST_USE void *infer_allocate(struct inference *p, size_t count, size_t size, size_t alignment);
MO_MUST_USE bool infer_map_keys(struct inference *p, const struct mo_syntax *node,
                                const uint32_t *versions);
MO_MUST_USE struct term *infer_make_type(struct inference *p, struct mo_source_type shape);
MO_MUST_USE struct term *infer_fresh(struct inference *p, unsigned numeric);
MO_MUST_USE struct term *infer_native(struct inference *p, const char *name, uint32_t count);
MO_MUST_USE struct term *infer_root(struct term *t);
MO_MUST_USE bool infer_named(struct term *t, const char *name);
MO_MUST_USE bool infer_type_depth(struct inference *p, uint32_t depth);
MO_MUST_USE struct term *infer_argument(struct inference *p, struct term *t, uint32_t index);
MO_MUST_USE bool infer_preview_type(struct inference *p, struct type_preview *out, struct term *t,
                                    uint32_t depth);
MO_MUST_USE bool infer_unify(struct inference *p, struct term *a, struct term *b, uint32_t depth);
MO_MUST_USE bool infer_unify_as(struct inference *p, struct term *found, struct term *expected,
                                uint16_t code);
MO_MUST_USE bool infer_same_type(struct inference *p, struct term *a, struct term *b,
                                 uint32_t depth);
MO_MUST_USE struct term *infer_annotation(struct inference *p, const struct mo_source_type *type,
                                          struct term **parameters, uint32_t depth);
MO_MUST_USE int infer_compare_name(const struct mo_syntax *a, const struct mo_syntax *b);
MO_MUST_USE struct value infer_graph(struct inference *p, struct value v, uint32_t count);
MO_MUST_USE bool infer_record(struct inference *p, const struct mo_syntax *node, struct value v);
MO_MUST_USE int infer_source_note_order(const void *a, const void *b);
MO_MUST_USE int infer_syntax_note_order(const void *a, const void *b);
MO_MUST_USE int infer_compare_text(const struct mo_syntax *a, const char *b);
MO_MUST_USE const struct mo_source_type *infer_parameter_type(const struct mo_source_type *type,
                                                              uint32_t bit);

MO_MUST_USE const struct mo_source_type *infer_finish_type(struct inference *p, struct term *t,
                                                           uint32_t depth);
MO_MUST_USE bool infer_finish_notes(struct inference *p, struct mo_inferred_expression *items);
MO_MUST_USE bool infer_finish_captures(struct inference *p);
MO_MUST_USE bool infer_finish_patterns(struct inference *p);
MO_MUST_USE bool infer_finish_operator(struct inference *p, struct note *note);
MO_MUST_USE bool infer_finish_interpolation(struct inference *p, struct note *note);
MO_MUST_USE bool infer_finish_function_use(struct inference *p, struct emitted *graph);
MO_MUST_USE bool infer_type_arguments(struct inference *p, const struct mo_source_type *generic,
                                      const struct mo_source_type *actual,
                                      struct mo_type_arguments *out);
MO_MUST_USE bool infer_data_requirements(struct inference *p,
                                         const struct mo_type_arguments *arguments, uint32_t *count,
                                         const struct mo_checked_ability_use **out);
MO_MUST_USE bool infer_finish_wrapping(struct inference *p);
MO_MUST_USE struct mo_checked_pattern *
infer_checked_pattern(struct inference *p, const struct mo_syntax *syntax,
                      const struct mo_source_type *type, struct note *const *notes,
                      uint32_t note_count, const struct mo_checked_binding *bindings,
                      uint32_t binding_count, uint32_t depth);

MO_MUST_USE bool infer_solve_failure(struct inference *p, struct failure_check *at, bool defaults);
MO_MUST_USE bool infer_finish_dispatches(struct inference *p);
MO_MUST_USE bool infer_anchor_inputs(struct inference *p, const struct mo_syntax *anchor,
                                     const struct mo_syntax *parameters, uint32_t count,
                                     const struct mo_syntax **inputs);

MO_MUST_USE const struct mo_data_signature *infer_data_type(struct inference *p,
                                                            const struct mo_symbol *symbol);
MO_MUST_USE bool infer_visible_data(struct inference *p, const struct mo_symbol *owner);
MO_MUST_USE struct value infer_declared_function(struct inference *p, const struct mo_symbol *s);
MO_MUST_USE const struct mo_symbol *infer_select_overload(struct inference *p,
                                                          const struct mo_symbol *group,
                                                          uint32_t count, struct term *receiver);
MO_MUST_USE uint32_t infer_builtin_field(const struct mo_builtin_case *data,
                                         const struct mo_syntax *arg);
MO_MUST_USE struct term *infer_builtin_part(struct inference *p, const struct mo_builtin_case *data,
                                            struct term *type, uint32_t index);
MO_MUST_USE struct value infer_member(struct inference *p, struct value v,
                                      const struct mo_syntax *name, bool *called);
bool infer_receiver_only(struct term *type);
MO_MUST_USE bool infer_empty_dot_call(struct inference *p, const struct mo_syntax *site);
MO_MUST_USE struct term *infer_tuple_part(struct inference *p, struct term *t,
                                          const struct mo_syntax *index, uint32_t *part);

#endif
