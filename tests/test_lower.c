#include "base/codes.h"
#include "base/limits.h"
#include "codegen/codegen.h"
#include "lower/lower.h"
#include "test.h"
#include "vm/vm.h"

#include <string.h>

static void run(struct mo_core *core, int64_t expected) {
    struct mo_diags diags;
    struct mo_buffer bytes;
    CHECK(mo_diags_init(&diags));
    MO_ASSERT(mo_core_check(core, &diags)); /* A refusal is a lowering bug. */
    mo_buffer_init(&bytes, MO_LIMIT_BYTECODE_SIZE);
    CHECK(mo_compile(core, &bytes, &diags));
    if (mo_diags_any(&diags))
        (void)fprintf(stderr, "E%04u: %s (%s)\n", diags.items[0].code, diags.items[0].message,
                      diags.items[0].primary.text == NULL ? "" : diags.items[0].primary.text);
    struct mo_load_error error;
    struct mo_image *image = mo_load(bytes.bytes, bytes.length, &error);
    CHECK(image != NULL);
    if (image != NULL) {
        struct mo_limits limits = mo_limits_default();
        struct mo_defect defect;
        CHECK(mo_image_init(image, &limits, &defect) == MO_RETURNED);
        struct mo_process *process = mo_process_new(image, &limits);
        CHECK(process != NULL);
        if (process != NULL) {
            struct mo_cell result;
            const struct mo_bytecode *file = mo_image_bytecode(image);
            uint32_t entry = UINT32_MAX;
            for (size_t i = 0; i < core->funs.count; i++)
                if (core->entry.length == core->funs.items[i].name.length &&
                    memcmp(core->entry.bytes, core->funs.items[i].name.bytes, core->entry.length) ==
                        0) {
                    mo_core_string name = core->metadata.funs.items[i].name;
                    for (uint32_t j = 0; j < file->function_count; j++) {
                        const struct mo_bytecode_string *s =
                            &file->strings[file->functions[j].name];
                        if (s->length == name.length &&
                            memcmp(s->bytes, name.bytes, name.length) == 0)
                            entry = j;
                    }
                }
            CHECK(entry != UINT32_MAX);
            CHECK(mo_call(process, entry, NULL, 0, &result, &defect) == MO_RETURNED);
            CHECK(mo_cell_get_int(result) == expected);
            mo_process_free(process);
        }
        mo_image_free(image);
    }
    mo_buffer_free(&bytes);
    mo_diags_free(&diags);
}

static void test_literal(void) {
    const char *text = "module Main\nfn main() : Int\n17\nend\n";
    struct mo_arena syntax, arena;
    struct mo_sources sources;
    struct mo_diags diags;
    mo_arena_init(&syntax, 1 << 20);
    mo_arena_init(&arena, 1 << 20);
    CHECK(mo_sources_init(&sources));
    CHECK(mo_diags_init(&diags));
    uint32_t file;
    struct mo_source_error error;
    CHECK(mo_sources_add_bytes(&sources, "main.mo", (const uint8_t *)text, strlen(text), &file,
                               &error));
    struct mo_module_syntax *module = mo_parse(&syntax, &sources, file, &diags);
    CHECK(module != NULL && !mo_diags_any(&diags));
    const char *names[] = {"Main"};
    struct mo_module_set set = {.count = 1, .modules = &module, .names = names};
    struct mo_syntax *fn = module->tree->children[1]->children[0];
    struct mo_symbol symbol = {
        .name = "main", .declaration = fn, .owner = UINT32_MAX, .receiver = UINT32_MAX};
    struct mo_source_type int_type = {.kind = MO_SOURCE_NAMED, .name = "Int", .module = UINT32_MAX};
    const struct mo_source_type *parts[] = {&int_type};
    struct mo_source_type function_type = {
        .kind = MO_SOURCE_FUNCTION, .count = 1, .arguments = parts};
    struct mo_function_signature signature = {&symbol, &function_type};
    struct mo_syntax literal = {
        .kind = MO_S_INT, .span = {0, 28, 30}, .text = (const uint8_t *)"17"};
    struct mo_checked_expression expression = {
        .syntax = &literal, .type = &int_type, .field = UINT32_MAX, .receiver = UINT32_MAX};
    struct mo_checked_function function = {.signature = &signature,
                                           .body = {.count = 1, .items = &expression}};
    struct mo_program program = {
        .set = &set, .function_count = 1, .functions = &function, .entry.result = MO_ENTRY_STATUS};
    struct mo_core *core = mo_lower(&arena, &program, &sources, &diags);
    CHECK(core != NULL && !mo_diags_any(&diags));
    if (core != NULL) {
        struct mo_buffer output;
        mo_buffer_init(&output, MO_LIMIT_CORE_TEXT_SIZE);
        CHECK(mo_core_print(core, &output));
        CHECK(core->metadata.source && core->metadata.files.count == 1);
        CHECK(core->metadata.funs.items[0].span.some &&
              core->metadata.funs.items[0].span.value.file == 0);
        mo_buffer_free(&output);
        mo_arena_free(&syntax);
        mo_sources_free(&sources); /* The lower result must own its provenance. */
        run(core, 17);
    } else {
        mo_arena_free(&syntax);
        mo_sources_free(&sources);
    }
    mo_arena_free(&arena);
    mo_diags_free(&diags);
}

static void test_refusal(void) {
    struct mo_arena arena;
    struct mo_sources sources;
    struct mo_diags diags;
    mo_arena_init(&arena, 1 << 20);
    CHECK(mo_sources_init(&sources));
    CHECK(mo_diags_init(&diags));
    struct mo_program program = {0};
    CHECK(mo_lower(&arena, &program, &sources, &diags) == NULL);
    CHECK(diags.count == 1 && diags.items[0].code == MO_CODE_LOWER_UNFINISHED);
    mo_arena_free(&arena);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
}

int main(void) {
    test_refusal();
    test_literal();
    return mo_test_report("lower");
}
