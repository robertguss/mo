#ifndef MO_LOWER_SCALAR_H
#define MO_LOWER_SCALAR_H

#include "lower/internal.h"

mo_core_expr lower_literal(struct lowering *p, const struct mo_syntax *syntax, mo_core_ty type,
                           bool negative);
mo_core_expr lower_text_part(struct lowering *p, const struct mo_syntax *owner, uint32_t part);
struct mo_core_prim lower_operator(struct lowering *p, enum mo_syntax_token operation,
                                   mo_core_ty left, mo_core_ty right);
mo_core_expr lower_error_text(struct lowering *p, mo_core_ty type, mo_core_expr value);
mo_core_expr lower_to_text(struct lowering *p, const struct mo_ability_implementation *proof,
                           const struct substitution *substitution, uint32_t owner, uint32_t depth,
                           mo_core_expr value);
mo_core_expr lower_compare(struct lowering *p, const struct mo_ability_implementation *proof,
                           const struct substitution *substitution, uint32_t owner, uint32_t depth,
                           mo_core_expr left, mo_core_expr right);
mo_core_expr lower_to_json(struct lowering *p, const struct mo_ability_implementation *proof,
                           const struct substitution *substitution, uint32_t owner, uint32_t depth,
                           mo_core_expr value);
mo_core_expr lower_from_json(struct lowering *p, const struct mo_ability_implementation *proof,
                             const struct substitution *substitution, uint32_t owner,
                             uint32_t depth, mo_core_expr value, mo_core_expr path,
                             mo_core_expr strict);
mo_core_expr lower_json_decode_call(struct lowering *p,
                                    const struct mo_ability_implementation *proof,
                                    const struct substitution *substitution, uint32_t owner,
                                    uint32_t depth, mo_core_ty input_ty, mo_core_expr input,
                                    bool strict);
mo_core_expr lower_json_schema(struct lowering *p, const struct mo_ability_implementation *proof,
                               const struct substitution *substitution, uint32_t owner,
                               uint32_t depth, mo_core_expr seen);

#endif
