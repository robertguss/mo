/* Shared fixed-point intermediates for the VM and primitives. */
#ifndef MO_DECIMAL_H
#define MO_DECIMAL_H

#include <stdbool.h>

#include "base/base.h"

/* Inputs are valid Decimal coefficients (units of 10^-18), excluding -2^127.
 * Division requires a nonzero right operand. Round the exact rational once,
 * to nearest with ties to even. false means overflow and preserves out.
 * No allocation; multiplication is four limbs, division exactly 256 steps. */
MO_MUST_USE bool mo_decimal_mul(mo_i128 left, mo_i128 right, mo_i128 *out);
MO_MUST_USE bool mo_decimal_div(mo_i128 left, mo_i128 right, mo_i128 *out);

#endif
