#define _POSIX_C_SOURCE 200809L

#include "tools/tools.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static char root[] = "/tmp/mo-fuzz-strength-XXXXXX";
static char program[64], versions[64], version[80], counted[80];
static FILE *quiet;
static const char text[] = "module Main\n\nlimit = 10\n\n"
                           "fn within(n: Int) : Bool\n  n >= 0 and n <= limit\nend\n\n"
                           "fn ceiling(n: Int) : Int\n  return limit if n > limit\n  n\nend\n\n"
                           "fn clamp(n: Int) : Int\n"
                           "  promise \"the result stays within the limit\"\n"
                           "    within(result)\n"
                           "    allows n: 3, result: 3\n"
                           "    forbids n: 20, result: 20\n"
                           "  end\n\n"
                           "  examples\n    clamp(3) == 3\n    clamp(20) == 10\n  end\n\n"
                           "  return 0 if n < 0\n  ceiling(n)\nend\n";

static void remove_file(const char *path) {
    MO_ASSERT(unlink(path) == 0 || errno == ENOENT);
}

static void cleanup(void) {
    remove_file(version);
    remove_file(counted);
    remove_file(program);
    MO_ASSERT(rmdir(versions) == 0);
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

static void name(char *out, size_t size, const char *folder, const char *file) {
    int n = snprintf(out, size, "%s/%s", folder, file);
    MO_ASSERT(n > 0 && (size_t)n < size);
}

static void setup(void) {
    if (quiet != NULL)
        return;
    MO_ASSERT(mkdtemp(root) != NULL);
    name(program, sizeof program, root, "main.mo");
    name(versions, sizeof versions, root, "versions");
    name(version, sizeof version, versions, "wrong.mo");
    name(counted, sizeof counted, versions, "counted");
    MO_ASSERT(mkdir(versions, 0700) == 0);
    write_file(program, (const uint8_t *)text, sizeof text - 1);
    quiet = fopen("/dev/null", "w");
    MO_ASSERT(quiet != NULL);
    MO_ASSERT(atexit(cleanup) == 0);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    setup();
    const uint8_t *split = size == 0 ? NULL : memchr(data, 0, size);
    if (split == NULL) {
        remove_file(counted);
        write_file(version, data, size);
    } else {
        size_t head = (size_t)(split - data);
        write_file(counted, data, head);
        write_file(version, split + 1, size - head - 1);
    }
    const char *argv[] = {"mo", "test", program, "--strength", versions, "--seconds", "1", NULL};
    struct mo_cli cli;
    const char *error;
    MO_ASSERT(mo_cli_parse(7, argv, &cli, &error));
    int status = mo_cli_execute(&cli, quiet, quiet);
    (void)status;
    return 0;
}
