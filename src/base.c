#include "base.h"

#include <stdio.h>
#include <stdlib.h>

_Noreturn void mo_assert_failed(const char *expression, const char *file, int line) {
    /* The program is already failing; there is nothing useful to do if this
     * write fails too. */
    (void)fprintf(stderr, "mo: internal error: %s (%s:%d)\n", expression, file, line);
    abort();
}
