#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "tools/deadline.h"
#include "tools/tools.h"

#include <errno.h>
#include <locale.h>
#include <string.h>

/* Substitute only the POSIX clock edge, never VM outcomes or call ordering. */
int mo_cli_deadline_clock(clockid_t clock, struct timespec *now);
static struct timespec instant;
static bool unavailable;
static long step;
static time_t fail_at;

int mo_cli_deadline_clock(clockid_t clock, struct timespec *now) {
    MO_ASSERT(clock == CLOCK_MONOTONIC);
    if (unavailable || (fail_at != 0 && instant.tv_sec >= fail_at)) {
        errno = EIO;
        return -1;
    }
    *now = instant;
    instant.tv_nsec += step;
    if (instant.tv_nsec >= 1000000000) {
        instant.tv_sec++;
        instant.tv_nsec -= 1000000000;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc > 1) {
        /* Black-box CLI fixtures replace only the monotonic system clock.
         * Time advances, not a scripted series of VM outcomes or call counts. */
        instant = (struct timespec){17, 0};
        unavailable = strcmp(argv[1], "start-failure") == 0;
        step = strcmp(argv[1], "fixed") == 0 ? 0 : 1000000;
        fail_at = strcmp(argv[1], "failure") == 0 ? 18 : 0;
        (void)setlocale(LC_CTYPE, "");
        struct mo_cli cli;
        const char *error;
        if (!mo_cli_parse(argc - 1, (const char *const *)(argv + 1), &cli, &error)) {
            (void)fprintf(stderr, "%s\n", error);
            return 2;
        }
        return mo_cli_execute(&cli, stdout, stderr);
    }
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
