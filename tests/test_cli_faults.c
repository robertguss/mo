#include "test.h"
#include "tools/tools.h"

#include <errno.h>
#include <string.h>

/* Only this test's execute.c renames the stack edge. There is no production
 * injection flag, environment variable or platform-specific interposition. */
MO_MUST_USE int mo_cli_fault_stack(size_t size, void (*function)(void *), void *argument);

int mo_cli_fault_stack(size_t size, void (*function)(void *), void *argument) {
    (void)size;
    (void)function;
    (void)argument;
    return EAGAIN;
}

static int execute(bool json, FILE *output, FILE *errors) {
    const char *args[] = {"mo", "check", "tests/cli/good/main.mo"};
    struct mo_cli cli;
    const char *error;
    MO_ASSERT(mo_cli_parse(3, args, &cli, &error));
    cli.json = json;
    return mo_cli_execute(&cli, output, errors);
}

int main(int argc, char *argv[]) {
    if (argc == 2)
        return execute(strcmp(argv[1], "--json") == 0, stdout, stderr);
    MO_ASSERT(argc == 1);
    for (unsigned mode = 0; mode < 2; mode++) {
        FILE *output = tmpfile(), *errors = tmpfile();
        MO_ASSERT(output != NULL && errors != NULL);
        CHECK(execute(mode != 0, output, errors) == 1);
        FILE *used = mode != 0 ? output : errors;
        rewind(used);
        char text[2048] = {0};
        size_t n = fread(text, 1, sizeof(text) - 1, used);
        CHECK(n != 0 && !ferror(used));
        CHECK(strstr(text, "E1401") != NULL);
        CHECK(strstr(text, "could not prepare the source pipeline stack") != NULL);
        CHECK(strstr(text, strerror(EAGAIN)) != NULL);
        if (mode != 0) {
            CHECK(strstr(text, "\"file\":null") != NULL);
            CHECK(strstr(text, "\"errors\":1,\"dropped\":0,\"status\":1") != NULL);
        } else
            CHECK(strstr(text, "<unavailable>") != NULL);
        rewind(mode != 0 ? errors : output);
        CHECK(fgetc(mode != 0 ? errors : output) == EOF);
        CHECK(fclose(output) == 0);
        CHECK(fclose(errors) == 0);
    }
    return mo_test_report("test_cli_faults");
}
