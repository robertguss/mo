/* Both text round trips, on a file the caller has already verified. */
#ifndef MO_TEST_BYTECODE_TEXT_H
#define MO_TEST_BYTECODE_TEXT_H

#include "bytecode/bytecode.h"
#include "test.h"

#include <string.h>

static void text_roundtrip(const struct mo_buffer *wire) {
    struct mo_buffer text, assembled, again;
    struct mo_load_error le;
    struct mo_assemble_error ae;
    mo_buffer_init(&text, MO_LIMIT_BYTECODE_TEXT_SIZE);
    mo_buffer_init(&again, MO_LIMIT_BYTECODE_TEXT_SIZE);
    mo_buffer_init(&assembled, MO_LIMIT_BYTECODE_SIZE);
    bool printed = mo_bytecode_disassemble(wire->bytes, wire->length, &text, &le);
    CHECK(printed);
    if (printed) {
        bool parsed = mo_bytecode_assemble((const char *)text.bytes, text.length, &assembled, &ae);
        CHECK(parsed);
        if (!parsed)
            CHECK(fprintf(stderr, "text refusal at %u: %s\n", ae.offset, ae.reason) >= 0);
        if (parsed) {
            CHECK(assembled.length == wire->length &&
                  memcmp(assembled.bytes, wire->bytes, wire->length) == 0);
            CHECK(mo_bytecode_disassemble(assembled.bytes, assembled.length, &again, &le));
            CHECK(text.length == again.length && memcmp(text.bytes, again.bytes, text.length) == 0);
        }
    }
    mo_buffer_free(&assembled);
    mo_buffer_free(&again);
    mo_buffer_free(&text);
}

#endif
