/* The independently transcribed instruction list of bytecode.md 4.3 to 4.7. It checks the
 * public descriptors the compiler, assembler, verifier and VM build against. */
#include "bytecode/bytecode.h"
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
    {"call", "wF", 'a', MO_LIMIT_PARAMETERS},
    {"call_closure", "wc", 'c', MO_LIMIT_PARAMETERS},
    {"tail_call", "F", 'a', MO_LIMIT_PARAMETERS},
    {"tail_call_closure", "c", 'c', MO_LIMIT_PARAMETERS},
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
    {"prim", "wPT", 'p', MO_LIMIT_PARAMETERS},
    {"host", "wH", 'h', MO_LIMIT_PARAMETERS},
    {"reset", "tc", '-', 0},
    {"reuse", "wqL", 'c', MO_LIMIT_FIELDS},
    {"free_token", "q", '-', 0},
};

/* A descriptor has an access exactly when it names a register. This includes every consuming
 * access, whether unconditional, signature-dependent, a move, or a token. */
MO_MUST_USE static bool well_formed(struct mo_operand_info operand) {
    return operand.kind >= MO_OPERAND_NONE && operand.kind <= MO_OPERAND_CASE &&
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

int main(void) {
    test_numbers_names_and_descriptors();
    test_every_specified_opcode();
    test_family_names();
    return mo_test_report("test_opcodes");
}
