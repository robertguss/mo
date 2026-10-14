#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "tools/deadline.h"

#include <errno.h>

/* Substitute only the POSIX clock edge, never VM outcomes or call ordering. */
int mo_cli_deadline_clock(clockid_t clock, struct timespec *now);
static struct timespec instant;
static bool unavailable;

int mo_cli_deadline_clock(clockid_t clock, struct timespec *now) {
    MO_ASSERT(clock == CLOCK_MONOTONIC);
    if (unavailable) {
        errno = EIO;
        return -1;
    }
    *now = instant;
    return 0;
}

int main(void) {
    instant = (struct timespec){7, 900000000};
    struct mo_deadline deadline;
    CHECK(mo_deadline_start(&deadline, 2));
    instant = (struct timespec){8, 300000000};
    CHECK(mo_deadline_running(&deadline));
    CHECK(mo_deadline_microseconds(&deadline) == 400000);
    instant = (struct timespec){9, 899999999};
    CHECK(mo_deadline_running(&deadline));
    instant.tv_nsec++;
    CHECK(!mo_deadline_running(&deadline));
    CHECK(deadline.state == MO_DEADLINE_EXPIRED);
    CHECK(mo_deadline_microseconds(&deadline) == 2000000);

    CHECK(mo_deadline_start(&deadline, UINT32_MAX));
    instant.tv_sec += 3;
    unavailable = true;
    CHECK(!mo_deadline_running(&deadline));
    CHECK(deadline.state == MO_DEADLINE_ERROR && deadline.error == EIO);
    unavailable = false;
    CHECK(!mo_deadline_running(&deadline));
    CHECK(deadline.state == MO_DEADLINE_ERROR);
    CHECK(mo_deadline_start(&deadline, UINT32_MAX));
    instant.tv_sec++;
    CHECK(mo_deadline_running(&deadline));
    unavailable = true;
    CHECK(!mo_deadline_start(&deadline, 1));
    CHECK(deadline.state == MO_DEADLINE_ERROR && deadline.error == EIO);
    return mo_test_report("promise deadline");
}
