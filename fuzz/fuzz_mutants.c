#include "base/limits.h"
#include "base/stack.h"
#include "check/check.h"
#include "tools/mutants.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

struct input {
    const uint8_t *data;
    size_t size;
};

static void mutant(const struct mo_program *program, const struct mo_sources *sources,
                   const struct mo_mutant *m) {
    const struct mo_syntax *fn = program->functions[m->function].signature->function->declaration;
    const struct mo_source *before = &sources->files[m->span.file];
    size_t original = strlen(m->original), replacement = strlen(m->replacement);
    MO_ASSERT(fn->kind == MO_S_FN && m->span.start >= fn->children[5]->span.start &&
              m->span.end <= fn->children[5]->span.end);
    MO_ASSERT(original == m->span.end - m->span.start &&
              memcmp(before->bytes + m->span.start, m->original, original) == 0);
    MO_ASSERT(original != replacement || memcmp(m->original, m->replacement, original) != 0);
    struct mo_sources out;
    struct mo_diags diags;
    struct mo_arena arena;
    MO_ASSERT(mo_sources_init(&out) && mo_diags_init(&diags));
    MO_ASSERT(mo_mutant_sources(program, sources, m, &out));
    const struct mo_source *after = &out.files[m->module];
    MO_ASSERT(after->length == before->length - original + replacement &&
              memcmp(after->bytes, before->bytes, m->span.start) == 0 &&
              memcmp(after->bytes + m->span.start, m->replacement, replacement) == 0 &&
              memcmp(after->bytes + m->span.start + replacement, before->bytes + m->span.end,
                     before->length - m->span.end) == 0);
    mo_arena_init(&arena, 64 << 20);
    struct mo_module_syntax *parsed = mo_parse(&arena, &out, m->module, &diags);
    MO_ASSERT(parsed != NULL && !mo_diags_any(&diags));
    mo_arena_free(&arena);
    mo_diags_free(&diags);
    mo_sources_free(&out);
}

static void fuzz_input(void *argument) {
    const struct input *input = argument;
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_arena arena;
    MO_ASSERT(mo_sources_init(&sources) && mo_diags_init(&diags));
    mo_arena_init(&arena, 64 << 20);
    uint32_t file;
    struct mo_source_error error;
    struct mo_program *program = NULL;
    if (mo_sources_add_bytes(&sources, "main.mo", input->data, input->size, &file, &error)) {
        struct mo_module_set *set = mo_load_bundle_sources(&arena, &sources, "lib", &diags);
        program = set != NULL && !mo_diags_any(&diags) ? mo_check(&arena, set, &diags) : NULL;
    }
    struct mo_mutants mutants;
    if (program != NULL && !mo_diags_any(&diags) &&
        mo_list_mutants(&arena, program, &sources, &mutants) == NULL)
        for (uint32_t i = 0; i < mutants.count; i++) {
            MO_ASSERT(mutants.items[i].module == 0);
            mutant(program, &sources, &mutants.items[i]);
        }
    mo_arena_free(&arena);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    struct input input = {data, size};
    MO_ASSERT(mo_with_stack(MO_SOURCE_STACK_SIZE, fuzz_input, &input) == 0);
    return 0;
}
