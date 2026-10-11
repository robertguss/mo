#include "cli.h"

#include <stddef.h>
#include <string.h>

#ifndef MO_VERSION
#define MO_VERSION "0.0.0"
#endif

mo_command mo_cli_parse(int argc, const char *const argv[]) {
    MO_ASSERT(argc >= 0);
    if (argc < 2) {
        return MO_COMMAND_STATUS;
    }
    MO_ASSERT(argv != NULL);
    const char *first = argv[1];
    MO_ASSERT(first != NULL);
    if (strcmp(first, "--version") == 0 || strcmp(first, "-V") == 0) {
        return MO_COMMAND_VERSION;
    }
    return MO_COMMAND_STATUS;
}

const char *mo_version_line(void) {
    return "mo " MO_VERSION;
}
