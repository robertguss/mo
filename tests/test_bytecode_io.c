#include "bytecode/io.h"
#include "heap/types.h"
#include "test.h"

#include <string.h>

static void test_empty(void) {
    const struct mo_bytecode file = {0};
    struct mo_buffer out;
    struct mo_write_error error;
    mo_buffer_init(&out, 1024);
    CHECK(mo_bytecode_write(&file, &out, &error));
    CHECK(out.length == 56);
    CHECK(memcmp(out.bytes, "MOBC\0\0\1\0\0\0\0\0", 12) == 0);
    for (size_t i = 20; i < out.length; i++) {
        CHECK(out.bytes[i] == 0);
    }
    mo_buffer_free(&out);
    mo_buffer_init(&out, 55);
    CHECK(!mo_bytecode_write(&file, &out, &error));
    CHECK(out.length == 0 && error.reason != NULL);
    mo_buffer_free(&out);
}

int main(void) {
    test_empty();
    return mo_test_report("test_bytecode_io");
}
