/* Source integration and lowered-entry runner. No inference is reconstructed
 * here; entry capabilities are built by the host's public ABI. */
#define _POSIX_C_SOURCE 200809L
#include "base/codes.h"
#include "base/limits.h"
#include "base/stack.h"
#include "codegen/codegen.h"
#include "lower/lower.h"
#include "vm/vm.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct input {
    const char *path;
    const char *library;
    int result;
    bool core;
};

static int run_core(const struct mo_core *core, struct mo_diags *diags) {
    struct mo_buffer wire;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    if (!mo_compile(core, &wire, diags)) {
        MO_ASSERT(diags->count == 1 && diags->items[0].code == MO_CODE_CODEGEN_UNSUPPORTED);
        mo_buffer_free(&wire);
        return 77; /* explicit staged codegen refusal, never a matched outcome */
    }
    struct mo_load_error error;
    struct mo_image *image = mo_load(wire.bytes, wire.length, &error);
    MO_ASSERT(image != NULL);
    uint32_t entry = UINT32_MAX;
    const struct mo_bytecode *file = mo_image_bytecode(image);
    for (size_t i = 0; i < core->funs.count; i++)
        if (core->entry.length == core->funs.items[i].name.length &&
            memcmp(core->entry.bytes, core->funs.items[i].name.bytes, core->entry.length) == 0) {
            mo_core_string name = core->metadata.funs.items[i].name;
            for (uint32_t j = 0; j < file->function_count; j++) {
                const struct mo_bytecode_string *s = &file->strings[file->functions[j].name];
                if (name.length == s->length && memcmp(name.bytes, s->bytes, name.length) == 0)
                    entry = j;
            }
        }
    MO_ASSERT(entry != UINT32_MAX);
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    enum mo_outcome outcome = mo_image_init_bounded(image, &limits, 100000, &defect);
    struct mo_process *process = NULL;
    struct mo_cell result;
    FILE *console = tmpfile();
    MO_ASSERT(console != NULL);
    int folder = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    MO_ASSERT(folder >= 0);
    if (outcome == MO_RETURNED) {
        process = mo_process_new(image, &limits);
        MO_ASSERT(process != NULL);
        mo_process_host(process)->output = fileno(console);
        mo_process_host(process)->errors = STDERR_FILENO;
        mo_process_instruction_limit(process, 100000);
        struct mo_host *host = mo_process_host(process);
        struct mo_heap *heap = mo_process_heap(process);
        struct mo_cell args, env;
        MO_ASSERT(mo_value_list(heap, host->types->texts, NULL, 0, &args));
        MO_ASSERT(mo_value_map(heap, host->types->files, NULL, NULL, 0, &env));
        outcome = mo_call_main(process, entry, &args, &env, folder, NULL, &result, &defect);
        mo_value_drop(heap, host->types->texts, args);
        mo_value_drop(heap, host->types->files, env);
    }
    if (outcome == MO_RETURNED)
        MO_ASSERT(printf("returned\n%lld\n", (long long)mo_cell_get_int(result)) >= 0);
    else {
        MO_ASSERT(puts(outcome == MO_STOPPED ? "stopped" : "failed") >= 0);
        if (defect.has_detail) {
            size_t count;
            const uint8_t *bytes = mo_value_data(defect.detail, &count);
            MO_ASSERT(fwrite(bytes, 1, count, stdout) == count && putchar('\n') != EOF);
        } else
            MO_ASSERT(puts(defect.reason) >= 0);
    }
    mo_process_free(process);
    MO_ASSERT(close(folder) == 0);
    MO_ASSERT(fclose(console) == 0);
    mo_image_free(image);
    mo_buffer_free(&wire);
    return 0;
}

static void run_source(void *argument) {
    struct input *input = argument;
    struct mo_arena source, output;
    struct mo_sources sources;
    struct mo_diags diags;
    mo_arena_init(&source, MO_LIMIT_LOWER_WORK);
    mo_arena_init(&output, MO_LIMIT_LOWER_WORK);
    MO_ASSERT(mo_sources_init(&sources) && mo_diags_init(&diags));
    struct mo_core *core = NULL;
    if (input->core) {
        uint32_t file;
        struct mo_source_error error;
        MO_ASSERT(mo_sources_add(&sources, input->path, &file, &error));
        core = mo_core_parse(&output, (const char *)sources.files[file].bytes,
                             sources.files[file].length, &diags);
    } else {
        struct mo_module_set *set =
            mo_load_program(&source, &sources, input->path, input->library, &diags);
        struct mo_program *program = set == NULL ? NULL : mo_check(&source, set, &diags);
        core = program == NULL ? NULL : mo_lower(&output, program, &sources, &diags);
    }
    /* Printing/checking after releasing the entire front end proves the result
     * owns its types, names, spans and filenames, not merely its Expr tree. */
    mo_arena_free(&source);
    mo_sources_free(&sources);
    if (core != NULL) {
        MO_ASSERT(mo_core_check(core, &diags)); /* Any refusal here is a lowering bug. */
        if (input->core) {
            input->result = run_core(core, &diags);
            mo_diags_free(&diags);
            mo_arena_free(&output);
            return;
        }
        struct mo_buffer text;
        mo_buffer_init(&text, MO_LIMIT_CORE_TEXT_SIZE);
        if (mo_core_print(core, &text)) {
            MO_ASSERT(fwrite(text.bytes, 1, text.length, stdout) == text.length);
            input->result = 0;
        } else
            (void)fprintf(stderr, "E%04u: Core text exceeds the codec's resource bounds\n",
                          MO_CODE_CORE_TEXT_LIMIT);
        mo_buffer_free(&text);
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
    if (argc == 3 && strcmp(argv[1], "--core") == 0)
        input = (struct input){.path = argv[2], .core = true, .result = 1};
    MO_ASSERT(mo_with_stack(MO_SOURCE_STACK_SIZE, run_source, &input) == 0);
    return input.result;
}
