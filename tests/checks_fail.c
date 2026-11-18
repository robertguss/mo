#define _POSIX_C_SOURCE 200809L
#include "host/host.h"
#include "tools/tests.h"
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

void mo_checks_fail_run(const struct mo_cli *cli, struct mo_arena *arena,
                        const struct mo_program *program, const struct mo_sources *sources,
                        const struct mo_test_selection *selection, FILE *output,
                        struct mo_diags *diags, struct mo_test_totals *totals);
enum mo_compile_status mo_checks_fail_start(const struct mo_compiler *compiler, const char *folder,
                                            const char *bundle, const char *diagnostics,
                                            const char *old_bundle, const char *context,
                                            uint64_t now, struct mo_compile_child *child);

void mo_checks_fail_run(const struct mo_cli *cli, struct mo_arena *arena,
                        const struct mo_program *program, const struct mo_sources *sources,
                        const struct mo_test_selection *selection, FILE *output,
                        struct mo_diags *diags, struct mo_test_totals *totals) {
    if (unlink("checks-fail") != 0) {
        mo_run_tests(cli, arena, program, sources, selection, output, diags, totals);
        return;
    }
    totals->failed = 1;
    (void)fputs("failed this build's checks\n", output);
}

enum mo_compile_status mo_checks_fail_start(const struct mo_compiler *compiler, const char *folder,
                                            const char *bundle, const char *diagnostics,
                                            const char *old_bundle, const char *context,
                                            uint64_t now, struct mo_compile_child *child) {
    char marker[256];
    int n = snprintf(marker, sizeof marker, "%s/checks-fail", folder);
    MO_ASSERT(n > 0 && (size_t)n < sizeof marker);
    if (access("checks-fail", F_OK) == 0) {
        int fd = open(marker, O_WRONLY | O_CREAT | O_EXCL, 0600);
        MO_ASSERT(fd >= 0 && close(fd) == 0);
    }
    return mo_check_start(compiler, folder, bundle, diagnostics, old_bundle, context, now, child);
}
