#include "bytecode/io.h"
#include "codegen/codegen.h"
#include "heap/types.h"
#include "test.h"

#include <string.h>

int main(void) {
    const char text[] =
        "(core " MO_CORE_TEXT_VERSION " (Program () ((FunDecl \"finish\" () (unit) (lit (unit)) "
                                      "\"Other\" (function) 0)) () \"finish\" (noSource)))";
    struct mo_arena arena;
    struct mo_diags diags;
    struct mo_buffer out;
    mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
    mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
    CHECK(mo_diags_init(&diags));
    struct mo_core *p = mo_core_parse(&arena, text, strlen(text), &diags);
    CHECK(p != NULL);
    if (p != NULL) {
        CHECK(mo_compile(p, &out, &diags));
        CHECK(!mo_diags_any(&diags));
        struct mo_load_error error;
        struct mo_bytecode *b = mo_bytecode_decode(&arena, out.bytes, out.length, &error);
        CHECK(b != NULL);
        if (b != NULL) {
            CHECK(b->function_count == 1 && b->functions[0].code_count == 5);
            CHECK(b->functions[0].code[0] == MO_OP_CONST && b->functions[0].code[3] == MO_OP_RET);
            CHECK(b->strings[b->functions[0].name].length == 12);
            CHECK(memcmp(b->strings[b->functions[0].name].bytes, "Other.finish", 12) == 0);
        }
    }
    mo_buffer_free(&out);
    mo_diags_free(&diags);
    mo_arena_free(&arena);
    return mo_test_report("test_codegen");
}
