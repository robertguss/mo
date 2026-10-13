#ifndef MO_LOWER_SCALAR_H
#define MO_LOWER_SCALAR_H

#include "lower/internal.h"

mo_core_expr lower_literal(struct lowering *p, const struct mo_syntax *syntax, mo_core_ty type,
                           bool negative);
struct mo_core_prim lower_operator(struct lowering *p, enum mo_syntax_token operation,
                                   mo_core_ty left, mo_core_ty right);
mo_core_expr lower_error_text(struct lowering *p, mo_core_ty type, mo_core_expr value);

#endif
