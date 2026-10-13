#include "tools/tools.h"

#include <stdio.h>

int main(int argc, char *argv[]) {
    struct mo_cli cli;
    const char *error;
    if (!mo_cli_parse(argc, (const char *const *)argv, &cli, &error)) {
        (void)fprintf(stderr, "%s\n", error);
        return 2;
    }
    if (cli.command == MO_COMMAND_VERSION)
        return printf("%s\n", mo_version_line()) >= 0 && fflush(stdout) == 0 ? 0 : 1;
    (void)fputs("this build cannot do that yet\n", stderr);
    return 1;
}
