/* The independently transcribed instruction list of bytecode.md 4.3 to 4.7. It checks the
 * public descriptors the compiler, assembler, verifier and VM build against. */
#include "bytecode/bytecode.h"
#include "heap/types.h"
#include "test.h"

#include <string.h>

/* Lowercase letters are register accesses: w writes, r reads, c consumes, m conditionally
 * moves, a/p/h use call/primitive/host parameter ownership, t writes a token, q consumes one.
 * Uppercase letters are indices: K constant, G global, F function, T type, L layout, P primitive,
 * H host operation, J jump word offset, I field, V case. '-' is no variable operand. */
struct expected_opcode {
    const char *name;
    const char *fixed;
    char variable;
    uint16_t limit;
};

static const struct expected_opcode specification[] = {
    {"move", "wm", '-', 0},
    {"dup", "wr", '-', 0},
    {"drop", "c", '-', 0},
    {"const", "wK", '-', 0},
    {"global", "wG", '-', 0},
    {"set_global", "Gc", '-', 0},
    {"add_int", "wrr", '-', 0},
    {"add_decimal", "wrr", '-', 0},
    {"add_float", "wrr", '-', 0},
    {"sub_int", "wrr", '-', 0},
    {"sub_decimal", "wrr", '-', 0},
    {"sub_float", "wrr", '-', 0},
    {"mul_int", "wrr", '-', 0},
    {"mul_decimal", "wrr", '-', 0},
    {"mul_float", "wrr", '-', 0},
    {"div_int", "wrr", '-', 0},
    {"div_decimal", "wrr", '-', 0},
    {"div_float", "wrr", '-', 0},
    {"rem_int", "wrr", '-', 0},
    {"rem_decimal", "wrr", '-', 0},
    {"rem_float", "wrr", '-', 0},
    {"neg_int", "wr", '-', 0},
    {"neg_decimal", "wr", '-', 0},
    {"neg_float", "wr", '-', 0},
    {"neg_duration", "wr", '-', 0},
    {"add_duration", "wrr", '-', 0},
    {"sub_duration", "wrr", '-', 0},
    {"scale_duration", "wrr", '-', 0},
    {"add_time", "wrr", '-', 0},
    {"sub_time", "wrr", '-', 0},
    {"diff_time", "wrr", '-', 0},
    {"eq_int", "wrr", '-', 0},
    {"eq_decimal", "wrr", '-', 0},
    {"eq_float", "wrr", '-', 0},
    {"eq_bool", "wrr", '-', 0},
    {"eq_byte", "wrr", '-', 0},
    {"eq_char", "wrr", '-', 0},
    {"eq_time", "wrr", '-', 0},
    {"eq_duration", "wrr", '-', 0},
    {"eq_choice", "wrr", '-', 0},
    {"lt_int", "wrr", '-', 0},
    {"lt_decimal", "wrr", '-', 0},
    {"lt_float", "wrr", '-', 0},
    {"lt_bool", "wrr", '-', 0},
    {"lt_byte", "wrr", '-', 0},
    {"lt_char", "wrr", '-', 0},
    {"lt_time", "wrr", '-', 0},
    {"lt_duration", "wrr", '-', 0},
    {"lt_choice", "wrr", '-', 0},
    {"le_int", "wrr", '-', 0},
    {"le_decimal", "wrr", '-', 0},
    {"le_float", "wrr", '-', 0},
    {"le_bool", "wrr", '-', 0},
    {"le_byte", "wrr", '-', 0},
    {"le_char", "wrr", '-', 0},
    {"le_time", "wrr", '-', 0},
    {"le_duration", "wrr", '-', 0},
    {"le_choice", "wrr", '-', 0},
    {"cmp_float", "wrr", '-', 0},
    {"not", "wr", '-', 0},
    {"make_range", "wrr", '-', 0},
    {"range_start", "wr", '-', 0},
    {"range_end", "wr", '-', 0},
    {"jump", "J", '-', 0},
    {"branch", "rJJ", '-', 0},
    {"switch", "r", 'J', MO_LIMIT_CHOICE_CASES},
    {"call", "wF", 'a', 256},
    {"call_closure", "wc", 'c', 255},
    {"tail_call", "F", 'a', 256},
    {"tail_call_closure", "c", 'c', 255},
    {"ret", "c", '-', 0},
    {"stop", "r", '-', 0},
    {"fail", "r", '-', 0},
    {"construct", "wL", 'c', MO_LIMIT_FIELDS},
    {"make_case", "wTV", 'c', MO_LIMIT_FIELDS},
    {"field", "wrI", '-', 0},
    {"case_field", "wrVI", '-', 0},
    {"set_field", "wcIc", '-', 0},
    {"make_closure", "wF", 'c', MO_LIMIT_FIELDS},
    {"equal", "wTrr", '-', 0},
    {"prim", "wPT", 'p', 256},
    {"host", "wH", 'h', 256},
    {"reset", "tc", '-', 0},
    {"reuse", "wqL", 'c', MO_LIMIT_FIELDS},
    {"free_token", "q", '-', 0},
    {"clear", "w", '-', 0},
    {"sim", "wXXX", 'r', 256},
};

/* A descriptor has an access exactly when it names a register. This includes every consuming
 * access, whether unconditional, signature-dependent, a move, or a token. */
MO_MUST_USE static bool well_formed(struct mo_operand_info operand) {
    return operand.kind >= MO_OPERAND_NONE && operand.kind <= MO_OPERAND_INDEX &&
           operand.access >= MO_ACCESS_NONE && operand.access <= MO_ACCESS_TOKEN_CONSUME &&
           ((operand.kind == MO_OPERAND_REGISTER) == (operand.access != MO_ACCESS_NONE));
}

MO_MUST_USE static char descriptor_letter(struct mo_operand_info operand) {
    switch (operand.kind) {
    case MO_OPERAND_NONE:
        return '-';
    case MO_OPERAND_TYPE:
        return 'T';
    case MO_OPERAND_LAYOUT:
        return 'L';
    case MO_OPERAND_CONSTANT:
        return 'K';
    case MO_OPERAND_GLOBAL:
        return 'G';
    case MO_OPERAND_FUNCTION:
        return 'F';
    case MO_OPERAND_PRIMITIVE:
        return 'P';
    case MO_OPERAND_HOST:
        return 'H';
    case MO_OPERAND_TARGET:
        return 'J';
    case MO_OPERAND_FIELD:
        return 'I';
    case MO_OPERAND_CASE:
        return 'V';
    case MO_OPERAND_INDEX:
        return 'X';
    case MO_OPERAND_REGISTER:
        break;
    }
    switch (operand.access) {
    case MO_ACCESS_NONE:
        return '?';
    case MO_ACCESS_WRITE:
        return 'w';
    case MO_ACCESS_READ:
        return 'r';
    case MO_ACCESS_CONSUME:
        return 'c';
    case MO_ACCESS_MOVE:
        return 'm';
    case MO_ACCESS_CALL_PARAMETER:
        return 'a';
    case MO_ACCESS_PRIM_PARAMETER:
        return 'p';
    case MO_ACCESS_HOST_PARAMETER:
        return 'h';
    case MO_ACCESS_TOKEN_WRITE:
        return 't';
    case MO_ACCESS_TOKEN_CONSUME:
        return 'q';
    }
    return '?';
}

static void test_numbers_names_and_descriptors(void) {
    for (size_t i = 0; i < MO_OPCODE_COUNT; i++) {
        const struct mo_opcode_info *op = &mo_opcodes[i];
        CHECK(op->number == i + 1); /* assigned numbers and lookup order are part of the contract */
        CHECK(mo_opcode_find(op->number) == op);
        CHECK(op->name != NULL && op->name[0] != '\0');
        CHECK(op->fixed_count <= MO_OPCODE_FIXED_OPERANDS);
        CHECK(op->variable_limit <= MO_LIMIT_OPERANDS);
        CHECK(well_formed(op->variable));
        CHECK((op->variable_limit == 0) == (op->variable.kind == MO_OPERAND_NONE));
        for (size_t j = 0; j < MO_OPCODE_FIXED_OPERANDS; j++) {
            CHECK(well_formed(op->fixed[j]));
            CHECK((j >= op->fixed_count) == (op->fixed[j].kind == MO_OPERAND_NONE));
        }
        for (size_t j = i + 1; j < MO_OPCODE_COUNT; j++) {
            CHECK(op->number != mo_opcodes[j].number);
            CHECK(strcmp(op->name, mo_opcodes[j].name) != 0);
        }
    }
    CHECK(mo_opcode_find(0) == NULL);
    CHECK(mo_opcode_find(MO_OPCODE_COUNT + 1) == NULL);
    CHECK(mo_opcode_find(UINT16_MAX) == NULL);
    CHECK(mo_opcode_find(UINT32_MAX) == NULL);
}

static void test_every_specified_opcode(void) {
    CHECK(MO_OPCODE_COUNT == sizeof specification / sizeof specification[0]);
    for (size_t i = 0; i < sizeof specification / sizeof specification[0]; i++) {
        const struct expected_opcode *expected = &specification[i];
        size_t found = 0;
        for (size_t j = 0; j < MO_OPCODE_COUNT; j++) {
            const struct mo_opcode_info *op = &mo_opcodes[j];
            if (strcmp(op->name, expected->name) != 0) {
                continue;
            }
            found++;
            CHECK(op->number == i + 1);
            CHECK(op->fixed_count == strlen(expected->fixed));
            for (size_t k = 0; k < strlen(expected->fixed); k++) {
                MO_ASSERT(k < MO_OPCODE_FIXED_OPERANDS);
                CHECK(descriptor_letter(op->fixed[k]) == expected->fixed[k]);
            }
            CHECK(descriptor_letter(op->variable) == expected->variable);
            CHECK(op->variable_limit == expected->limit);
        }
        if (found != 1) {
            (void)fprintf(stderr, "%s: found %zu opcodes, expected one\n", expected->name, found);
            mo_test_failures++;
        }
    }
}

/* The arithmetic families use family_type, including scale_duration and field-less choice.
 * add_time/sub_time/diff_time are distinct mixed-type operations; equal is structural equality,
 * not an eq family member. The independent full list above catches missing/extra variants. */
static void test_family_names(void) {
    static const char *const families[] = {"add", "sub",   "mul", "div", "rem",
                                           "neg", "scale", "eq",  "lt",  "le"};
    static const char *const types[] = {"int",  "decimal", "float",    "bool",  "byte",
                                        "char", "time",    "duration", "choice"};
    for (size_t i = 0; i < MO_OPCODE_COUNT; i++) {
        const char *name = mo_opcodes[i].name;
        if (strcmp(name, "add_time") == 0 || strcmp(name, "sub_time") == 0 ||
            strcmp(name, "equal") == 0) {
            continue;
        }
        for (size_t j = 0; j < sizeof families / sizeof families[0]; j++) {
            size_t length = strlen(families[j]);
            if (strncmp(name, families[j], length) != 0) {
                continue;
            }
            CHECK(name[length] == '_');
            if (name[length] != '_') {
                continue;
            }
            size_t matches = 0;
            for (size_t k = 0; k < sizeof types / sizeof types[0]; k++) {
                matches += strcmp(name + length + 1, types[k]) == 0;
            }
            CHECK(matches == 1);
        }
    }
}

/* Enum numbers are wire data, not the compiler's choice of C representation. */
static void test_wire_numbers(void) {
    const enum mo_type_kind kinds[] = {
        MO_TYPE_UNIT, MO_TYPE_BOOL,       MO_TYPE_BYTE,     MO_TYPE_INT,     MO_TYPE_FLOAT,
        MO_TYPE_CHAR, MO_TYPE_TIME,       MO_TYPE_DURATION, MO_TYPE_DECIMAL, MO_TYPE_RANGE,
        MO_TYPE_TEXT, MO_TYPE_BYTES,      MO_TYPE_BIGINT,   MO_TYPE_TUPLE,   MO_TYPE_LIST,
        MO_TYPE_SET,  MO_TYPE_MAP,        MO_TYPE_RECORD,   MO_TYPE_CHOICE,  MO_TYPE_FUNCTION,
        MO_TYPE_ENV,  MO_TYPE_CAPABILITY,
    };
    const enum mo_layout_kind layouts[] = {MO_LAYOUT_RECORD, MO_LAYOUT_TUPLE, MO_LAYOUT_CASE,
                                           MO_LAYOUT_CLOSURE};
    const enum mo_capability capabilities[] = {MO_CAP_ARGS,  MO_CAP_ENV,        MO_CAP_CONSOLE,
                                               MO_CAP_FILES, MO_CAP_READ_FILES, MO_CAP_CLOCK,
                                               MO_CAP_RANDOM};
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        CHECK((uint32_t)kinds[i] == i);
    }
    for (size_t i = 0; i < sizeof layouts / sizeof layouts[0]; i++) {
        CHECK((uint32_t)layouts[i] == i);
    }
    for (size_t i = 0; i < sizeof capabilities / sizeof capabilities[0]; i++) {
        CHECK((uint32_t)capabilities[i] == i);
    }
    CHECK(MO_BIGINT_ZERO == 0 && MO_BIGINT_POSITIVE == 1 && MO_BIGINT_NEGATIVE == 2);
    CHECK(MO_NO_INDEX == UINT32_MAX && MO_BYTECODE_NO_INDEX == MO_NO_INDEX);
}

/* The worked type/layout sections of bytecode.md 3, as data. This is not a
 * replacement writer: stream 3 will test encoding/decoding through its APIs.
 * Keep the expected hex independent of enum numbers and native endianness. */
static void test_wire_example(void) {
    const uint32_t words[] = {
        8,
        MO_TYPE_UNIT,
        MO_TYPE_INT,
        MO_TYPE_TEXT,
        MO_TYPE_CHOICE,
        0,
        3,
        1,
        0, /* End */
        2,
        2,
        3,
        1,
        4,
        3, /* Link(value: Int, next: Chain) */
        5,
        1,
        MO_BYTECODE_NO_INDEX,
        3, /* Alias(Chain) */
        MO_TYPE_LIST,
        1,
        3,
        MO_TYPE_TUPLE,
        2,
        1,
        4,
        MO_TYPE_FUNCTION,
        1,
        3,
        1,
        MO_TYPE_ENV,
        0,
        4,
        MO_LAYOUT_CASE,
        3,
        1,
        2,
        1,
        3,
        MO_LAYOUT_CASE,
        3,
        2,
        1,
        3,
        MO_LAYOUT_TUPLE,
        5,
        2,
        1,
        4,
        MO_LAYOUT_CLOSURE,
        6,
        0,
        2,
        3,
        2,
    };
    const char expected[] = "08000000"
                            "00000000"
                            "03000000"
                            "0a000000"
                            "120000000000000003000000"
                            "0100000000000000"
                            "020000000200000003000000010000000400000003000000"
                            "0500000001000000ffffffff03000000"
                            "0e0000000100000003000000"
                            "0d000000020000000100000004000000"
                            "13000000010000000300000001000000"
                            "1400000000000000"
                            "04000000"
                            "020000000300000001000000020000000100000003000000"
                            "0200000003000000020000000100000003000000"
                            "0100000005000000020000000100000004000000"
                            "030000000600000000000000020000000300000002000000";
    struct mo_buffer bytes;
    struct mo_buffer hex;
    mo_buffer_init(&bytes, 1024);
    mo_buffer_init(&hex, 2048);
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) {
        CHECK(mo_buffer_append_le32(&bytes, words[i]));
    }
    for (size_t i = 0; i < bytes.length; i++) {
        CHECK(mo_buffer_append_hex(&hex, bytes.bytes[i], 2));
    }
    CHECK(hex.length == sizeof expected - 1);
    CHECK(hex.length == sizeof expected - 1 &&
          memcmp(hex.bytes, expected, sizeof expected - 1) == 0);
    mo_buffer_free(&hex);
    mo_buffer_free(&bytes);
}

int main(void) {
    test_numbers_names_and_descriptors();
    test_every_specified_opcode();
    test_family_names();
    test_wire_numbers();
    test_wire_example();
    return mo_test_report("test_opcodes");
}
