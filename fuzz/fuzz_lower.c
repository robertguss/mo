#include "../tests/lower_fixture.h"
#include "base/codes.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* Generate well-typed checked graphs, rather than mostly rejecting source.
 * Every accepted Core document must check, including its complete metadata. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    struct lower_fixture f;
    lower_fixture_init(&f, "module Main\nfn main() : Int\n0 + 1 + 7\nend\n");
    uint32_t values[3];
    const char *spellings[] = {"0", "1", "7"};
    for (size_t i = 0; i < 3; i++)
        values[i] = lower_fixture_row(&f, MO_S_INT, &f.int_, spellings[i], 0, NULL);
    uint32_t last = values[0];
    size_t count = size < 24 ? size : 24;
    for (size_t i = 0; i < count; i++) {
        uint32_t right = values[data[i] % 3];
        if ((data[i] & 8) != 0) {
            uint32_t condition =
                lower_fixture_op(&f, data[i] & 1 ? MO_T_LT : MO_T_EQ, &f.bool_, last, right);
            uint32_t arms[] = {condition, last, right};
            last = lower_fixture_row(&f, MO_S_IF, &f.int_, NULL, 3, arms);
        } else {
            const enum mo_syntax_token ops[] = {MO_T_PLUS, MO_T_MINUS, MO_T_STAR, MO_T_SLASH};
            last = lower_fixture_op(&f, ops[(data[i] >> 4) & 3], &f.int_, last, right);
        }
    }
    struct mo_core *core = mo_lower(&f.arena, &f.program, &f.sources, &f.diags);
    if (core != NULL)
        MO_ASSERT(mo_core_check(core, &f.diags));
    else {
        MO_ASSERT(f.diags.count != 0);
        uint16_t code = f.diags.items[0].code;
        MO_ASSERT(code == MO_CODE_LOWER_DEPTH || code == MO_CODE_LOWER_WORK ||
                  code == MO_CODE_LOWER_NO_MEMORY);
    }
    lower_fixture_free(&f);
    return 0;
}
