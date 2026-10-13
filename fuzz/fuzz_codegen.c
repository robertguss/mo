#include "base/codes.h"
#include "base/limits.h"
#include "base/stack.h"
#include "codegen/codegen.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

struct input {
    const uint8_t *data;
    size_t size;
};
static void run(void *argument) {
    const struct input *input = argument;
    const uint8_t *data = input->data;
    size_t size = input->size;
    const enum mo_core_text_kind kinds[] = {MO_CORE_TEXT_PROGRAM, MO_CORE_TEXT_RUN};
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        struct mo_arena arena;
        struct mo_diags diags;
        mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
        if (!mo_diags_init(&diags)) {
            mo_arena_free(&arena);
            return;
        }
        void *parsed = mo_core_text_parse(&arena, kinds[i], (const char *)data, size, &diags);
        const struct mo_core *p = parsed == NULL ? NULL
                                  : kinds[i] == MO_CORE_TEXT_PROGRAM
                                      ? parsed
                                      : &((struct mo_core_run *)parsed)->program;
        if (p != NULL && mo_core_check(p, &diags)) {
            struct mo_buffer first, second;
            mo_buffer_init(&first, MO_LIMIT_BYTECODE_SIZE);
            mo_buffer_init(&second, MO_LIMIT_BYTECODE_SIZE);
            bool ok = mo_compile(p, &first, &diags);
            uint32_t previous = diags.count + diags.dropped;
            bool again = mo_compile(p, &second, &diags);
            MO_ASSERT(ok == again);
            MO_ASSERT(ok ? !mo_diags_any(&diags) : mo_diags_any(&diags));
            MO_ASSERT(first.length == second.length);
            if (ok) {
                MO_ASSERT(first.length != 0 &&
                          memcmp(first.bytes, second.bytes, first.length) == 0);
            } else {
                MO_ASSERT(first.length == 0 && diags.count + diags.dropped == previous + 1);
                MO_ASSERT(diags.count == 0 || diags.items[0].code != MO_CODE_CODEGEN_INVALID);
            }
            mo_buffer_free(&first);
            mo_buffer_free(&second);
        }
        mo_diags_free(&diags);
        mo_arena_free(&arena);
    }
}
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    struct input input = {data, size};
    MO_ASSERT(mo_with_stack(MO_SOURCE_STACK_SIZE, run, &input) == 0);
    return 0;
}
