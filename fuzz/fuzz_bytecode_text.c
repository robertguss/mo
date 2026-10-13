#include "bytecode/bytecode.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
size_t LLVMFuzzerCustomMutator(uint8_t *data, size_t size, size_t maximum, unsigned seed);
size_t LLVMFuzzerMutate(uint8_t *data, size_t size, size_t maximum);

static void roundtrip(const uint8_t *data, size_t size, bool required) {
    struct mo_buffer text, wire, again;
    struct mo_load_error le;
    struct mo_assemble_error ae;
    mo_buffer_init(&text, MO_LIMIT_BYTECODE_TEXT_SIZE);
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    mo_buffer_init(&again, MO_LIMIT_BYTECODE_TEXT_SIZE);
    bool printed = mo_bytecode_disassemble(data, size, &text, &le);
    MO_ASSERT(printed || !required);
    if (printed) {
        MO_ASSERT(mo_bytecode_assemble((const char *)text.bytes, text.length, &wire, &ae));
        MO_ASSERT(wire.length == size && memcmp(wire.bytes, data, size) == 0);
        MO_ASSERT(mo_bytecode_disassemble(wire.bytes, wire.length, &again, &le));
        MO_ASSERT(text.length == again.length && memcmp(text.bytes, again.bytes, text.length) == 0);
    } else {
        MO_ASSERT(text.length == 0 && le.reason != NULL && le.offset <= size);
    }
    mo_buffer_free(&again);
    mo_buffer_free(&wire);
    mo_buffer_free(&text);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size >= 4 && memcmp(data, "MOBC", 4) == 0) {
        roundtrip(data, size, false);
    } else {
        struct mo_buffer wire;
        struct mo_assemble_error error;
        mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
        if (mo_bytecode_assemble((const char *)data, size, &wire, &error)) {
            /* The disassembler independently loads/verifies the assembler's output. */
            roundtrip(wire.bytes, wire.length, true);
        } else {
            MO_ASSERT(wire.length == 0 && error.reason != NULL && error.offset <= size);
        }
        mo_buffer_free(&wire);
    }
    return 0;
}

size_t LLVMFuzzerCustomMutator(uint8_t *data, size_t size, size_t maximum, unsigned seed) {
    /* Start mutations of binary fixtures from their canonical text, rather than wasting
     * mutations on a checksum. Native text seeds exercise shorthand as well. */
    if (size >= 4 && memcmp(data, "MOBC", 4) == 0) {
        struct mo_buffer text;
        struct mo_load_error error;
        mo_buffer_init(&text, MO_LIMIT_BYTECODE_TEXT_SIZE);
        if (mo_bytecode_disassemble(data, size, &text, &error) && text.length <= maximum) {
            memcpy(data, text.bytes, text.length);
            size = text.length;
        }
        mo_buffer_free(&text);
    }
    if (seed % 8 == 0 && size != 0) {
        /* Keep most of the file intact: changing one index digit or quote reaches past
         * the header and exercises precise refusals and accepted noncanonical spellings. */
        size_t at = (size_t)seed % size;
        static const uint8_t bytes[] = "0123456789,\"-;\n";
        data[at] = bytes[(seed / 8) % (sizeof bytes - 1)];
        return size;
    }
    return LLVMFuzzerMutate(data, size, maximum);
}
