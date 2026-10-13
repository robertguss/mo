/* Fuzzes the argument parser: the input's zero bytes split it into arguments.
 * Whatever arrives, the parser must return a known command without tripping
 * an assertion or a sanitizer. */
#include "tools/tools.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    enum { MAX_ARGUMENTS = 16 };
    char *copy = malloc(size + 1);
    if (copy == NULL) {
        return 0;
    }
    if (size > 0) {
        memcpy(copy, data, size);
    }
    copy[size] = '\0';

    const char *argv[MAX_ARGUMENTS + 1];
    int argc = 0;
    argv[argc++] = "mo";
    size_t start = 0;
    for (size_t i = 0; i <= size && argc < MAX_ARGUMENTS; i++) {
        if (i == size || copy[i] == '\0') {
            argv[argc++] = copy + start;
            start = i + 1;
        }
    }
    argv[argc] = NULL;

    struct mo_cli cli;
    const char *error;
    bool parsed = mo_cli_parse(argc, argv, &cli, &error);
    MO_ASSERT(parsed || error != NULL);
    free(copy);
    return 0;
}
