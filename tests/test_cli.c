#include "test.h"
#include "tools/cli.h"

#include <stddef.h>
#include <string.h>

static void test_no_arguments_shows_the_status(void) {
    const char *argv[] = {"mo", NULL};
    CHECK(mo_cli_parse(1, argv) == MO_COMMAND_STATUS);
    CHECK(mo_cli_parse(0, NULL) == MO_COMMAND_STATUS);
}

static void test_version_flags(void) {
    const char *long_flag[] = {"mo", "--version", NULL};
    const char *short_flag[] = {"mo", "-V", NULL};
    CHECK(mo_cli_parse(2, long_flag) == MO_COMMAND_VERSION);
    CHECK(mo_cli_parse(2, short_flag) == MO_COMMAND_VERSION);
}

static void test_other_arguments_show_the_status(void) {
    const char *run[] = {"mo", "run", "main.mo", NULL};
    const char *near_miss[] = {"mo", "--versions", NULL};
    const char *empty[] = {"mo", "", NULL};
    CHECK(mo_cli_parse(3, run) == MO_COMMAND_STATUS);
    CHECK(mo_cli_parse(2, near_miss) == MO_COMMAND_STATUS);
    CHECK(mo_cli_parse(2, empty) == MO_COMMAND_STATUS);
}

static void test_the_version_line_names_the_command(void) {
    CHECK(strncmp(mo_version_line(), "mo ", 3) == 0);
}

int main(void) {
    test_no_arguments_shows_the_status();
    test_version_flags();
    test_other_arguments_show_the_status();
    test_the_version_line_names_the_command();
    return mo_test_report("test_cli");
}
