/* Source integration driver. Link with the toolchain library objects in an
 * integration checkout containing mo_check; main intentionally does not yet
 * provide that implementation. No inference is reconstructed by this driver. */
#include "base/limits.h"
#include "base/stack.h"
#include "lower/lower.h"

#include <stdio.h>

struct input {
    const char *path;
    const char *library;
    int result;
};

static void run_source(void *argument) {
    struct input *input = argument;
    struct mo_arena source, output;
    struct mo_sources sources;
    struct mo_diags diags;
    mo_arena_init(&source, MO_LIMIT_LOWER_WORK);
    mo_arena_init(&output, MO_LIMIT_LOWER_WORK);
    MO_ASSERT(mo_sources_init(&sources) && mo_diags_init(&diags));
    struct mo_module_set *set =
        mo_load_program(&source, &sources, input->path, input->library, &diags);
    struct mo_program *program = set == NULL ? NULL : mo_check(&source, set, &diags);
    struct mo_core *core = program == NULL ? NULL : mo_lower(&output, program, &sources, &diags);
    /* Printing/checking after releasing the entire front end proves the result
     * owns its types, names, spans and filenames, not merely its Expr tree. */
    mo_arena_free(&source);
    mo_sources_free(&sources);
    if (core != NULL) {
        MO_ASSERT(mo_core_check(core, &diags)); /* Any refusal here is a lowering bug. */
        struct mo_buffer text;
        mo_buffer_init(&text, MO_LIMIT_CORE_TEXT_SIZE);
        MO_ASSERT(mo_core_print(core, &text));
        MO_ASSERT(fwrite(text.bytes, 1, text.length, stdout) == text.length);
        mo_buffer_free(&text);
        input->result = 0;
    } else {
        MO_ASSERT(diags.count != 0);
        for (uint32_t i = 0; i < diags.count; i++)
            (void)fprintf(stderr, "E%04u: %s (%u:%u)\n", diags.items[i].code,
                          diags.items[i].message, diags.items[i].primary.span.file,
                          diags.items[i].primary.span.start);
    }
    mo_diags_free(&diags);
    mo_arena_free(&output);
}

int main(int argc, char **argv) {
    MO_ASSERT(argc == 2 || argc == 3);
    struct input input = {.path = argv[1], .library = argc == 3 ? argv[2] : NULL, .result = 1};
    MO_ASSERT(mo_with_stack(MO_SOURCE_STACK_SIZE, run_source, &input) == 0);
    return input.result;
}
