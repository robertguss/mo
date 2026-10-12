/* Definitions shared by the whole Mo implementation.
 *
 * Mo is written in C on five conditions (decision D207); see
 * docs/design/implementation.md. This header carries the parts of them that
 * live in code: assertions that are never compiled out, and results the
 * compiler insists are used. */
#ifndef MO_BASE_H
#define MO_BASE_H

#include <limits.h>

#if CHAR_BIT != 8
#error "Mo assumes 8-bit bytes."
#endif

_Static_assert(sizeof(void *) == 8, "Mo assumes a 64-bit platform.");

/* Decimal's count and base's checked arithmetic (architecture section 2). */
__extension__ typedef __int128 mo_i128;
__extension__ typedef unsigned __int128 mo_u128;
_Static_assert(sizeof(mo_i128) == 16, "Mo needs 128-bit integers.");

/* BigInt signs shared by the bytecode's literals and heap's C builder. The
 * magnitude is little-endian; zero has no bytes, nonzero has no high zero byte. */
enum mo_bigint_sign { MO_BIGINT_ZERO = 0, MO_BIGINT_POSITIVE = 1, MO_BIGINT_NEGATIVE = 2 };

/* A result the caller must use. Ignoring it is a compile error, because
 * warnings are errors. */
#define MO_MUST_USE __attribute__((warn_unused_result))

/* Reports a broken invariant and stops the program. Never returns. */
_Noreturn void mo_assert_failed(const char *expression, const char *file, int line);

/* Checks an invariant in every build, including release builds (Tiger Style):
 * a program that has proven itself wrong stops where it happened. */
#define MO_ASSERT(expression)                                                                      \
    ((expression) ? (void)0 : mo_assert_failed(#expression, __FILE__, __LINE__))

/* Marks code that cannot be reached; reaching it stops the program. */
#define MO_UNREACHABLE() mo_assert_failed("unreachable code was reached", __FILE__, __LINE__)

#endif
