#define _POSIX_C_SOURCE 200809L

#include "tools/tools.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static char root[] = "/tmp/mo-fuzz-previous-XXXXXX";
static char now[64], folder[64], old[80];
static FILE *quiet;
static const char text[] = "module Main\nexpose clamp\n\n"
                           "fn clamp(n: Int) : Int\n"
                           "  promise \"never negative\"\n"
                           "    result >= 0\n"
                           "  end\n\n"
                           "  examples\n    clamp(-5) == 0\n    clamp(4) == 4\n  end\n\n"
                           "  n.max(0)\nend\n\n"
                           "fn twice(n: Int) : Int\n  n * 2\nend\n\n"
                           "test \"twice three is six\"\n  assert twice(3) == 6\nend\n";

static void remove_file(const char *path) {
    MO_ASSERT(unlink(path) == 0 || errno == ENOENT);
}

static void cleanup(void) {
    remove_file(now);
    remove_file(old);
    MO_ASSERT(rmdir(folder) == 0);
    MO_ASSERT(rmdir(root) == 0);
    MO_ASSERT(fclose(quiet) == 0);
}

static void write_file(const char *path, const uint8_t *bytes, size_t size) {
    FILE *file = fopen(path, "wb");
    MO_ASSERT(file != NULL);
    if (size != 0)
        MO_ASSERT(fwrite(bytes, 1, size, file) == size);
    MO_ASSERT(fclose(file) == 0);
}

static void name(char *out, size_t size, const char *in, const char *file) {
    int n = snprintf(out, size, "%s/%s", in, file);
    MO_ASSERT(n > 0 && (size_t)n < size);
}

static void setup(void) {
    if (quiet != NULL)
        return;
    MO_ASSERT(mkdtemp(root) != NULL);
    name(now, sizeof now, root, "main.mo");
    name(folder, sizeof folder, root, "old");
    name(old, sizeof old, folder, "main.mo");
    MO_ASSERT(mkdir(folder, 0700) == 0);
    quiet = fopen("/dev/null", "w");
    MO_ASSERT(quiet != NULL);
    MO_ASSERT(atexit(cleanup) == 0);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    setup();
    const uint8_t *split = size == 0 ? NULL : memchr(data, 0, size);
    if (split == NULL) {
        write_file(now, (const uint8_t *)text, sizeof text - 1);
        write_file(old, data, size);
    } else {
        size_t head = (size_t)(split - data);
        write_file(now, data, head);
        write_file(old, split + 1, size - head - 1);
    }
    const char *argv[] = {"mo", "test", now, "--against", old, NULL};
    struct mo_cli cli;
    const char *error;
    MO_ASSERT(mo_cli_parse(5, argv, &cli, &error));
    int status = mo_cli_execute(&cli, quiet, quiet);
    MO_ASSERT(status == 0 || status == 1);
    return 0;
}
