#include "base/codes.h"
#include "bytecode/bytecode.h"
#include "codegen/codegen.h"
#include "test.h"
#include "tools/tools.h"

#include <string.h>

static bool no_core_memory, corrupt_bytecode;

MO_MUST_USE void *mo_compiler_fault_allocate_array(struct mo_arena *arena, size_t count,
                                                   size_t size, size_t alignment);
MO_MUST_USE bool mo_compiler_fault_bytecode_write(const struct mo_bytecode *file,
                                                  struct mo_buffer *out,
                                                  struct mo_write_error *error);

void *mo_compiler_fault_allocate_array(struct mo_arena *arena, size_t count, size_t size,
                                       size_t alignment) {
    return no_core_memory ? NULL : mo_arena_allocate_array(arena, count, size, alignment);
}

bool mo_compiler_fault_bytecode_write(const struct mo_bytecode *file, struct mo_buffer *out,
                                      struct mo_write_error *error) {
    if (!corrupt_bytecode)
        return mo_bytecode_write(file, out, error);
    MO_ASSERT(file->constant_count == 1);
    struct mo_bytecode_constant constant = file->constants[0];
    constant.type = file->type_count;
    struct mo_bytecode invalid = *file;
    invalid.constants = &constant;
    return mo_bytecode_write(&invalid, out, error);
}

static void exact_message(const struct mo_diags *diags, uint16_t code, const char *reason,
                          const char *expected) {
    CHECK(diags->count == 1 && diags->dropped == 0);
    if (diags->count != 1)
        return;
    CHECK(diags->items[0].code == code);
    CHECK(strcmp(diags->items[0].reason, reason) == 0);
    struct mo_sources sources;
    struct mo_buffer text;
    MO_ASSERT(mo_sources_init(&sources));
    mo_buffer_init(&text, MO_CLI_DIAGNOSTIC_SIZE);
    CHECK(mo_render_diagnostic(&sources, &diags->items[0], false, &text));
    CHECK(text.length == strlen(expected));
    if (text.length == strlen(expected))
        CHECK(memcmp(text.bytes, expected, text.length) == 0);
    mo_buffer_free(&text);
    mo_sources_free(&sources);
}

int main(void) {
    const char *source = "(core " MO_CORE_TEXT_VERSION
                         " (Program () ((FunDecl \"main\" () (unit) (lit (unit)) \"Main\" "
                         "(function) 0)) () \"main\" (noSource)))";
    struct mo_arena arena;
    struct mo_diags diags;
    struct mo_buffer out;
    mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
    MO_ASSERT(mo_diags_init(&diags));
    mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
    struct mo_core *core = mo_core_parse(&arena, source, strlen(source), &diags);
    MO_ASSERT(core != NULL && !mo_diags_any(&diags));
    CHECK(mo_core_check(core, &diags) && !mo_diags_any(&diags));
    no_core_memory = true;
    CHECK(!mo_core_check(core, &diags));
    no_core_memory = false;
    exact_message(&diags, MO_CODE_CORE_CHECK_MEMORY,
                  "Core checking exhausts its bounded scratch arena or system memory.",
                  "error E1053: Core checker resource limit exceeded  <unavailable>\n\n"
                  "  in declaration \"main\"\n\n");
    mo_diags_free(&diags);
    MO_ASSERT(mo_diags_init(&diags));
    CHECK(mo_compile(core, &out, &diags) && out.length != 0 && !mo_diags_any(&diags));
    mo_buffer_free(&out);
    mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
    corrupt_bytecode = true;
    CHECK(!mo_compile(core, &out, &diags) && out.length == 0);
    corrupt_bytecode = false;
    exact_message(&diags, MO_CODE_CODEGEN_INVALID, "constant type out of range",
                  "error E1057: code generation produced invalid bytecode: bug in Mo  "
                  "<unavailable>\n\n  main: constant type out of range\n\n");
    mo_diags_free(&diags);
    MO_ASSERT(mo_diags_init(&diags));
    CHECK(mo_compile(core, &out, &diags) && out.length != 0 && !mo_diags_any(&diags));
    mo_buffer_free(&out);
    mo_diags_free(&diags);
    mo_arena_free(&arena);
    return mo_test_report("test_compiler_faults");
}
