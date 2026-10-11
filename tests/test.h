/* A minimal test harness. CHECK records a failure and carries on, so one run
 * reports every broken expectation; each test file is its own program. */
#ifndef MO_TEST_H
#define MO_TEST_H

#include <stdio.h>

static int mo_test_failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);    \
            mo_test_failures++;                                                                    \
        }                                                                                          \
    } while (0)

/* Prints the outcome and returns the program's exit status. */
static inline int mo_test_report(const char *name) {
    if (mo_test_failures == 0) {
        return printf("%s: all checks passed\n", name) < 0;
    }
    (void)fprintf(stderr, "%s: %d check(s) failed\n", name, mo_test_failures);
    return 1;
}

#endif
