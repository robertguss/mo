#define _POSIX_C_SOURCE 200809L
#include "host/host.h"
#include "test.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if MO_UPGRADE_COMPILE_ASAN
#define MEMORY_CHECK ""
#else
#define MEMORY_CHECK "test \"$(ulimit -v)\" = 1048576 && "
#endif

static enum mo_compile_status complete(struct mo_compile_child *child) {
    enum mo_compile_status status;
    for (unsigned i = 0; i < 2000; i++) {
        status = mo_compile_poll(child, 100 + (uint64_t)i * 10000);
        if (status != MO_COMPILE_RUNNING)
            return status;
        struct timespec delay = {0, 10000000};
        (void)nanosleep(&delay, NULL);
    }
    mo_compile_cancel(child);
    return MO_COMPILE_IO;
}
int main(void) {
    char dir[] = "/tmp/mo-compile-child-XXXXXX";
    MO_ASSERT(mkdtemp(dir) != NULL);
    char exe[128], output[128], errors[128];
    MO_ASSERT(snprintf(exe, sizeof exe, "%s/compiler", dir) > 0);
    MO_ASSERT(snprintf(output, sizeof output, "%s/image", dir) > 0);
    MO_ASSERT(snprintf(errors, sizeof errors, "%s/errors", dir) > 0);
    FILE *script = fopen(exe, "w");
    MO_ASSERT(script != NULL);
    MO_ASSERT(fputs("#!/bin/sh\n[ -z \"$MO_CHILD_CANARY\" ] || exit 1\nif [ \"$1\" = test ]; then\n"
                    "  [ ! -e \"/proc/$$/fd/$2\" ] || exit 1\n"
                    "  printf list > \"$6\"; exit 0\nfi\ncase \"$2\" in\n"
                    "ok) printf image > \"$4\";;\n"
                    "diagnostic) echo refused >&2; exit 1;;\n"
                    "crash) kill -SEGV $$;;\n"
                    "initializer) exit 70;;\n"
                    "service) exit 69;;\n"
                    "hang) while :; do :; done;;\n"
                    "limits) " MEMORY_CHECK
                    "test \"$(ulimit -t)\" = 10 && test \"$(ulimit -c)\" = 0;;\n"
                    "*) exit 125;;\nesac\n",
                    script) >= 0);
    MO_ASSERT(fclose(script) == 0 && chmod(exe, 0700) == 0 &&
              setenv("MO_CHILD_CANARY", "1", 1) == 0);
    const char *modes[] = {"ok", "diagnostic", "crash", "limits", "initializer", "service"};
    const enum mo_compile_status expected[] = {MO_COMPILE_OK,          MO_COMPILE_DIAGNOSTIC,
                                               MO_COMPILE_CRASH,       MO_COMPILE_OK,
                                               MO_COMPILE_INITIALIZER, MO_COMPILE_SERVICE};
    for (size_t i = 0; i < sizeof modes / sizeof *modes; i++) {
        /* This deadline status is rendered as E1420 by upgrade admission. */
        struct mo_compile_child child = {0};
        CHECK(mo_compile_start(exe, modes[i], output, errors, NULL, NULL, NULL, 100, &child) ==
              MO_COMPILE_RUNNING);
        MO_ASSERT(child.pid > 0);
        CHECK(complete(&child) == expected[i]);
        CHECK(child.pid == 0);
        struct stat info;
        CHECK(stat(errors, &info) == 0 && (info.st_mode & 0777) == 0600);
        FILE *log = fopen(errors, "r");
        MO_ASSERT(log != NULL);
        char message[32] = {0};
        size_t read = fread(message, 1, sizeof message - 1, log);
        CHECK(i == 1 ? read == 8 && strcmp(message, "refused\n") == 0 : read == 0);
        CHECK(fclose(log) == 0);
        CHECK(unlink(errors) == 0);
    }
    struct mo_compile_child child = {0};
    CHECK(mo_compile_start(exe, "hang", output, errors, NULL, NULL, NULL, 100, &child) ==
          MO_COMPILE_RUNNING);
    CHECK(mo_compile_poll(&child, 100 + MO_LIMIT_UPGRADE_COMPILE_WALL_US) == MO_COMPILE_BUDGET);
    CHECK(child.pid == 0);
    CHECK(unlink(errors) == 0);
    CHECK(mo_compile_start(exe, "hang", output, errors, NULL, NULL, NULL, 100, &child) ==
          MO_COMPILE_RUNNING);
    mo_compile_cancel(&child);
    CHECK(child.pid == 0);
    mo_compile_cancel(&child);
    /* Existing diagnostics cannot be overwritten. */
    CHECK(mo_compile_start(exe, "ok", output, errors, NULL, NULL, NULL, 100, &child) ==
          MO_COMPILE_IO);
    CHECK(unlink(errors) == 0);
    int held = open(dir, O_RDONLY | O_DIRECTORY);
    MO_ASSERT(held > 2);
    char number[16];
    MO_ASSERT(snprintf(number, sizeof number, "%d", held) > 0);
    CHECK(mo_list_start(exe, number, output, errors, NULL, NULL, 100, &child) ==
          MO_COMPILE_RUNNING);
    CHECK(complete(&child) == MO_COMPILE_OK);
    CHECK(close(held) == 0 && unlink(errors) == 0);
    CHECK(unlink(output) == 0 && unlink(exe) == 0 && rmdir(dir) == 0);
    return mo_test_report("compiler child");
}
