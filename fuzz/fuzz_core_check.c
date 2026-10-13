#include "base/limits.h"
#include "base/stack.h"
#include "core/core.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

struct input {
    const uint8_t *data;
    size_t size;
};
/* Type checking sees only successfully parsed Core, including ill-typed numeric literals.
 * Rechecking must agree and preserve any diagnostic from the earlier refusal. */
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
        void *value = mo_core_text_parse(&arena, kinds[i], (const char *)data, size, &diags);
        if (value != NULL) {
            const struct mo_core *p =
                kinds[i] == MO_CORE_TEXT_PROGRAM ? value : &((struct mo_core_run *)value)->program;
            bool valid = mo_core_check(p, &diags);
            MO_ASSERT(valid ? !mo_diags_any(&diags) : mo_diags_any(&diags));
            uint32_t previous = diags.count + diags.dropped;
            bool again = mo_core_check(p, &diags);
            MO_ASSERT(again == valid);
            MO_ASSERT(diags.count + diags.dropped == previous + (valid ? 0u : 1u));
        } else {
            MO_ASSERT(mo_diags_any(&diags));
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
