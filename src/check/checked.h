/* Body graph production, private to check. Shares the inference walk. */
#ifndef MO_CHECK_CHECKED_H
#define MO_CHECK_CHECKED_H

#include "check/check.h"

/* Initializers decide their own types before any use can constrain them.
 * pending pauses inference at an unresolved constant for the explicit driver. */
struct mo_constant_context {
    uint32_t count;
    struct mo_checked_constant *items;
    struct mo_checked_constant **by_symbol;
    bool *done;
    struct mo_checked_constant *pending;
    struct mo_span pending_span;
};

MO_MUST_USE struct mo_checked_constant *
mo_constant_by_symbol(const struct mo_constant_context *constants, const struct mo_symbol *symbol);
MO_MUST_USE bool mo_check_constants(struct mo_arena *arena,
                                    const struct mo_module_interfaces *interfaces,
                                    const struct mo_signatures *signatures,
                                    struct mo_constant_context *constants, struct mo_diags *diags);
MO_MUST_USE bool
mo_check_constant(struct mo_arena *arena, const struct mo_module_interfaces *interfaces,
                  const struct mo_signatures *signatures, struct mo_constant_context *constants,
                  struct mo_checked_constant *constant, struct mo_diags *diags, bool *halt);
MO_MUST_USE bool mo_order_constants(struct mo_arena *arena, struct mo_program *program,
                                    const struct mo_constant_context *constants,
                                    struct mo_diags *diags);

/* halt distinguishes resource/storage or staging failure from an independent body error,
 * including errors dropped solely because the diagnostic count is full. */
MO_MUST_USE bool mo_check_function(struct mo_arena *arena,
                                   const struct mo_module_interfaces *interfaces,
                                   const struct mo_signatures *signatures,
                                   struct mo_constant_context *constants, uint32_t function,
                                   struct mo_diags *diags, struct mo_checked_function *out,
                                   bool *halt);

/* Fills a spec's body, parameters, condition rows or anchor values. Original
 * text/identity belongs to the program-level producer. signature is the subject
 * for examples/promises/anchors, or a zero-parameter Unit context for a test. */
MO_MUST_USE bool
mo_check_spec_body(struct mo_arena *arena, const struct mo_module_interfaces *interfaces,
                   const struct mo_signatures *signatures, struct mo_constant_context *constants,
                   const struct mo_function_signature *signature, struct mo_checked_spec *spec,
                   struct mo_checked_condition *conditions, struct mo_diags *diags, bool *halt);

MO_MUST_USE bool mo_check_recursion(struct mo_arena *arena, const struct mo_program *program,
                                    struct mo_diags *diags);

/* Rejects a use of a generic function whose anonymous functions capture a type parameter when
 * the argument holds a capability (reference 11.4), through callers that pass their own
 * parameters on. */
MO_MUST_USE bool mo_check_capture_arguments(struct mo_arena *arena,
                                            const struct mo_program *program,
                                            struct mo_diags *diags);

MO_MUST_USE bool mo_check_data_recursion(struct mo_arena *arena,
                                         const struct mo_signatures *signatures,
                                         struct mo_diags *diags);

#endif
