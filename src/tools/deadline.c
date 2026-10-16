#define _POSIX_C_SOURCE 200809L

#include "tools/deadline.h"

#include <errno.h>

bool mo_deadline_start(struct mo_deadline *deadline, uint32_t seconds) {
    *deadline = (struct mo_deadline){0};
    if (clock_gettime(CLOCK_MONOTONIC, &deadline->start) != 0) {
        deadline->state = MO_DEADLINE_ERROR;
        deadline->error = errno;
        return false;
    }
    deadline->last = deadline->start;
    deadline->seconds = (uint64_t)deadline->start.tv_sec + seconds;
    deadline->nanoseconds = deadline->start.tv_nsec;
    return true;
}

bool mo_deadline_running(void *context) {
    struct mo_deadline *deadline = context;
    if (deadline->state != MO_DEADLINE_ACTIVE)
        return false;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        deadline->state = MO_DEADLINE_ERROR;
        deadline->error = errno;
        return false;
    }
    deadline->last = now;
    uint64_t seconds = (uint64_t)now.tv_sec;
    if (seconds > deadline->seconds ||
        (seconds == deadline->seconds && now.tv_nsec >= deadline->nanoseconds)) {
        deadline->state = MO_DEADLINE_EXPIRED;
        return false;
    }
    return true;
}

uint64_t mo_deadline_microseconds(const struct mo_deadline *deadline) {
    uint64_t seconds = (uint64_t)deadline->last.tv_sec - (uint64_t)deadline->start.tv_sec;
    long nanos = deadline->last.tv_nsec - deadline->start.tv_nsec;
    if (nanos < 0) {
        MO_ASSERT(seconds != 0);
        seconds--;
        nanos += 1000000000;
    }
    MO_ASSERT(seconds <= (UINT64_MAX - 999999) / 1000000);
    return seconds * 1000000 + (uint64_t)nanos / 1000;
}
