#include "base/path.h"
#include "test.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    FILE *table = fopen("tests/module_paths.tsv", "rb");
    MO_ASSERT(table != NULL);
    char name[64], expected[128], path[MO_LIMIT_PATH_SIZE + 1];
    int got;
    while ((got = fscanf(table, "%63s %127s", name, expected)) == 2) {
        CHECK(mo_module_path((const uint8_t *)name, strlen(name), path));
        CHECK(strcmp(path, expected) == 0);
    }
    CHECK(got == EOF && ferror(table) == 0);
    CHECK(fclose(table) == 0);
    const char *bad[] = {"", ".Foo", "Foo.", "Foo..Bar", "foo", "Foo/Bar", "Foo._Bar"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(!mo_module_path((const uint8_t *)bad[i], strlen(bad[i]), path));
    uint8_t long_name[MO_LIMIT_PATH_SIZE];
    memset(long_name, 'A', sizeof(long_name));
    CHECK(mo_module_path(long_name, MO_LIMIT_PATH_SIZE - 3, path));
    CHECK(strlen(path) == MO_LIMIT_PATH_SIZE);
    CHECK(!mo_module_path(long_name, MO_LIMIT_PATH_SIZE - 2, path));
    return mo_test_report("module paths");
}
