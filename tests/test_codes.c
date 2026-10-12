#include "base/codes.h"
#include "test.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Whether a section is written as the reference numbers them: "8" or "8.3". */
static bool is_section(const char *section) {
    size_t i = 0;
    for (int part = 0; part < 2; part++) {
        size_t digits = 0;
        while (section[i] >= '0' && section[i] <= '9') {
            i += 1;
            digits += 1;
        }
        if (digits == 0 || digits > 2) {
            return false;
        }
        if (section[i] == '\0') {
            return true;
        }
        if (section[i] != '.') {
            return false;
        }
        i += 1;
    }
    return false;
}

static void check_row(unsigned number, enum mo_code code, const char *identifier,
                      const char *section) {
    CHECK((unsigned)code == number);
    const struct mo_code_info *info = mo_code_find(number);
    CHECK(info != NULL);
    if (info == NULL) {
        return;
    }
    CHECK(info->number == number);
    CHECK(strcmp(info->identifier, identifier) == 0);
    /* The language's errors cite a section; the limits, from E1000, cite none. */
    if (number < 1000) {
        CHECK(section != NULL && info->section != NULL && is_section(info->section));
        CHECK(section != NULL && info->section != NULL && strcmp(info->section, section) == 0);
    } else {
        CHECK(section == NULL && info->section == NULL);
    }
}

static void test_every_row(void) {
    unsigned rows = 0;
#define CHECK_ROW(number, identifier, section)                                                     \
    check_row(number, MO_CODE_##identifier, #identifier, section);                                 \
    rows += 1;
    MO_CODES(CHECK_ROW)
#undef CHECK_ROW
    CHECK(rows >= 16);
}

static void test_the_codes_the_reference_shows(void) {
    const struct mo_code_info *info = mo_code_find(412);
    CHECK(info != NULL && info->section != NULL && strcmp(info->section, "8.3") == 0);
    CHECK(MO_CODE_FAILURE_IGNORED == 412);
}

static void test_the_codes_of_limits(void) {
    CHECK(MO_CODE_FILE_TOO_BIG == 1001);
    CHECK(MO_CODE_PROGRAM_TOO_BIG == 1002);
    CHECK(MO_CODE_TOO_MANY_FILES == 1003);
    CHECK(MO_CODE_PATH_TOO_LONG == 1004);
    CHECK(MO_CODE_LINE_TOO_LONG == 1005);
    CHECK(MO_CODE_NESTED_TOO_DEEPLY == 1006);
    CHECK(MO_CODE_INTERPOLATIONS_TOO_DEEP == 1007);
    CHECK(MO_CODE_TOO_MANY_NAMES == 1008);
    CHECK(MO_CODE_TOO_MANY_PARAMETERS == 1009);
    CHECK(MO_CODE_TOO_MANY_FIELDS == 1010);
    CHECK(MO_CODE_TOO_MANY_CASES == 1011);
    CHECK(MO_CODE_INSTANTIATIONS_TOO_DEEP == 1012);
    CHECK(MO_CODE_TOO_MANY_REGISTERS == 1013);
    CHECK(MO_CODE_FUNCTION_TOO_LONG == 1014);
    CHECK(MO_CODE_BYTECODE_TOO_BIG == 1015);
}

static void test_unknown_codes(void) {
    CHECK(mo_code_find(0) == NULL);
    CHECK(mo_code_find(411) == NULL);
    CHECK(mo_code_find(413) == NULL);
    CHECK(mo_code_find(1000) == NULL);
    CHECK(mo_code_find(9999) == NULL);
    CHECK(mo_code_find(65536 + 412) == NULL);
}

int main(void) {
    test_every_row();
    test_the_codes_the_reference_shows();
    test_the_codes_of_limits();
    test_unknown_codes();
    return mo_test_report("test_codes");
}
