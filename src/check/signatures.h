#ifndef MO_CHECK_SIGNATURES_H
#define MO_CHECK_SIGNATURES_H

#include "check/check.h"

struct mo_nominal_signature {
    const struct mo_source_type *type;
    uint32_t field_count;
    uint32_t hash_parameters, key_parameters;
    uint32_t case_count;
    bool invalid;
};

/* Private diagnostic recovery. Invalid function slots have NULL types; nominal
 * layouts propagate invalid dependencies. Neither can certify a program. */
MO_MUST_USE struct mo_signatures *
mo_prepare_available_signatures(struct mo_arena *arena, const struct mo_module_set *set,
                                const struct mo_module_interfaces *interfaces,
                                struct mo_diags *diags);
MO_MUST_USE bool mo_signature_type_available(const struct mo_signatures *signatures,
                                             const struct mo_source_type *type);

MO_MUST_USE const struct mo_data_signature *
mo_data_signature_by_symbol(const struct mo_signatures *signatures, const struct mo_symbol *symbol);
MO_MUST_USE const struct mo_function_signature *
mo_function_signature_by_symbol(const struct mo_signatures *signatures,
                                const struct mo_symbol *symbol);
MO_MUST_USE const struct mo_nominal_signature *
mo_nominal_signature_by_symbol(const struct mo_signatures *signatures,
                               const struct mo_symbol *symbol);
MO_MUST_USE const struct mo_function_signature *
mo_customizer_signature(const struct mo_signatures *signatures, const struct mo_source_type *type,
                        enum mo_source_ability ability);

#endif
