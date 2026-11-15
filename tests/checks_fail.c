#define _POSIX_C_SOURCE 200809L
#include "tools/tests.h"
#include <stdio.h>
#include <unistd.h>

void mo_checks_fail_run(const struct mo_cli *cli, struct mo_arena *arena,
                        const struct mo_program *program, const struct mo_sources *sources,
                        const struct mo_test_selection *selection, FILE *output,
                        struct mo_diags *diags, struct mo_test_totals *totals);

void mo_checks_fail_run(const struct mo_cli *cli, struct mo_arena *arena,
                        const struct mo_program *program, const struct mo_sources *sources,
                        const struct mo_test_selection *selection, FILE *output,
                        struct mo_diags *diags, struct mo_test_totals *totals) {
    if (access("checks-fail", F_OK) != 0) {
        mo_run_tests(cli, arena, program, sources, selection, output, diags, totals);
        return;
    }
    totals->failed = 1;
    (void)fputs("failed this build's checks\n", output);
}
