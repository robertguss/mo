#define _POSIX_C_SOURCE 200809L

#include "tools/tools.h"

#include <locale.h>
#include <signal.h>
#include <stdio.h>

int main(int argc, char *argv[]) {
    /* Only character encoding follows the environment; numeric parsing does
     * not. An unavailable locale leaves POSIX's ASCII-safe C locale active. */
    (void)setlocale(LC_CTYPE, "");
    struct mo_cli cli;
    const char *error;
    if (!mo_cli_parse(argc, (const char *const *)argv, &cli, &error)) {
        (void)fprintf(stderr, "%s\n", error);
        return 2;
    }
    /* Broken output is an ordinary tool failure, not exit 141 from SIGPIPE. */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        (void)fputs("mo: could not configure output handling\n", stderr);
        return 1;
    }
    return mo_cli_execute(&cli, stdout, stderr);
}
