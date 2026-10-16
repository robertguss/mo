/* Tools' monotonic promise budget. The VM only polls a callback (D242). */
#ifndef MO_TOOLS_DEADLINE_H
#define MO_TOOLS_DEADLINE_H

#include "base/base.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

enum mo_deadline_state { MO_DEADLINE_ACTIVE, MO_DEADLINE_EXPIRED, MO_DEADLINE_ERROR };

struct mo_deadline {
    struct timespec start, last;
    uint64_t seconds;
    long nanoseconds;
    enum mo_deadline_state state;
    int error;
};

/* One absolute deadline per promise, never restarted for a call/candidate.
 * False records a clock failure. running is also the controlled-call callback;
 * false distinguishes expiry from clock failure through state/error. */
MO_MUST_USE bool mo_deadline_start(struct mo_deadline *deadline, uint32_t seconds);
MO_MUST_USE bool mo_deadline_running(void *context);
MO_MUST_USE uint64_t mo_deadline_microseconds(const struct mo_deadline *deadline);

#endif
