/* Synchronous work on an explicit POSIX thread stack. */
#ifndef MO_STACK_H
#define MO_STACK_H

#include "base/base.h"

#include <stddef.h>

/* Runs function(argument) on a new thread with at least stack_size bytes,
 * rounding up to the POSIX page size, then joins it.
 * Returns zero after completion, or a POSIX error number without running it.
 * The argument and all storage it borrows must remain valid until this returns.
 * This does not make the work concurrent with the caller, or make shared state
 * thread-safe. A callback must return normally, not exit/cancel its thread. */
MO_MUST_USE int mo_with_stack(size_t stack_size, void (*function)(void *), void *argument);

#endif
