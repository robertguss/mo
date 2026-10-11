/* The mo command. A placeholder until Phase 2 builds the real toolchain. */
#include "cli.h"

#include <stdio.h>

static int print_version(void) {
    return printf("%s\n", mo_version_line()) >= 0;
}

static int print_status(void) {
    return printf("%s (pre-release)\n", mo_version_line()) >= 0 &&
           puts("Mo is in its design phase; there is nothing to run yet.") >= 0;
}

int main(int argc, char *argv[]) {
    int written = 0;
    switch (mo_cli_parse(argc, (const char *const *)argv)) {
    case MO_COMMAND_VERSION:
        written = print_version();
        break;
    case MO_COMMAND_STATUS:
        written = print_status();
        break;
    }
    if (!written || fflush(stdout) != 0) {
        (void)fputs("mo: could not write to standard output\n", stderr);
        return 1;
    }
    return 0;
}
