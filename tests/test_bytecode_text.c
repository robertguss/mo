#include "bytecode/bytecode.h"
#include "test.h"

#include <string.h>

static void empty(void) {
    static const char expected[] = "bytecode 0.1 indexed\nstrings\nend\ntypes\nend\nlayouts\nend\n"
                                   "constants\nend\nglobals\nend\ninit\nend\nfunctions\nend\n"
                                   "modules\nend\ndebug\nend\n";
    struct mo_bytecode file = {0};
    struct mo_buffer wire, text, small;
    struct mo_write_error we;
    struct mo_load_error le;
    struct mo_assemble_error ae;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    mo_buffer_init(&text, MO_LIMIT_BYTECODE_TEXT_SIZE);
    mo_buffer_init(&small, sizeof expected - 2);
    CHECK(mo_bytecode_write(&file, &wire, &we));
    CHECK(mo_bytecode_disassemble(wire.bytes, wire.length, &text, &le));
    CHECK(text.length == sizeof expected - 1);
    CHECK(memcmp(text.bytes, expected, text.length) == 0);
    struct mo_buffer assembled;
    mo_buffer_init(&assembled, MO_LIMIT_BYTECODE_SIZE);
    CHECK(mo_bytecode_assemble(expected, sizeof expected - 1, &assembled, &ae));
    CHECK(assembled.length == wire.length && memcmp(assembled.bytes, wire.bytes, wire.length) == 0);
    mo_buffer_free(&assembled);
    CHECK(!mo_bytecode_disassemble(wire.bytes, wire.length, &small, &le));
    CHECK(small.length == 0 && le.offset == 0 && le.kind == MO_BYTECODE_LIMIT);
    wire.bytes[0] = 'X';
    mo_buffer_clear(&text);
    CHECK(!mo_bytecode_disassemble(wire.bytes, wire.length, &text, &le));
    CHECK(text.length == 0 && le.kind == MO_BYTECODE_INVALID);
    mo_buffer_free(&small);
    mo_buffer_free(&text);
    mo_buffer_free(&wire);
}

int main(void) {
    empty();
    return mo_test_report("bytecode text");
}
