#include "test.h"
#include "tools/tools.h"

#include <string.h>

static void parse_commands(void) {
    struct mo_cli cli;
    const char *error;
    const char *args[] = {"mo", "run", "--max-stack", "17MiB", "main.mo", "--json", ""};
    CHECK(mo_cli_parse(7, args, &cli, &error));
    CHECK(cli.limits.stack_size == 17u * 1024u * 1024u);
    CHECK(cli.argument_count == 2 && strcmp(cli.arguments[0], "--json") == 0);
    const char *wrong[] = {"mo", "check", "--json", "--max-heap", "1GiB", "main.mo"};
    CHECK(!mo_cli_parse(6, wrong, &cli, &error));
    CHECK(strcmp(error, "flag is not accepted by this command") == 0);
    CHECK(!mo_cli_parse(0, NULL, &cli, &error));
}

int main(void) {
    parse_commands();
    return mo_test_report("test_cli");
}
