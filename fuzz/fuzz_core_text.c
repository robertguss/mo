#include "base/limits.h"
#include "core/core.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* Try each caller-supplied category, including ambiguous tags such as unit, stopped and failed.
 * The real sample runs are seeds too, supplied by make fuzz-smoke without duplicate fixtures. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    const enum mo_core_text_kind kinds[] = {
        MO_CORE_TEXT_TYPE,      MO_CORE_TEXT_CAP,     MO_CORE_TEXT_LITERAL, MO_CORE_TEXT_PRIMITIVE,
        MO_CORE_TEXT_EXPR,      MO_CORE_TEXT_PROGRAM, MO_CORE_TEXT_VALUE,   MO_CORE_TEXT_WORLD,
        MO_CORE_TEXT_BUG,       MO_CORE_TEXT_OUTCOME, MO_CORE_TEXT_EFFECTS, MO_CORE_TEXT_REPORT,
        MO_CORE_TEXT_SELECTION, MO_CORE_TEXT_RUN};
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        struct mo_arena arena;
        struct mo_diags diags;
        struct mo_buffer first, second;
        mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
        if (!mo_diags_init(&diags)) {
            mo_arena_free(&arena);
            return 0;
        }
        mo_buffer_init(&first, MO_LIMIT_CORE_TEXT_SIZE * 2);
        mo_buffer_init(&second, MO_LIMIT_CORE_TEXT_SIZE * 2);
        void *value = mo_core_text_parse(&arena, kinds[i], (const char *)data, size, &diags);
        if (value == NULL) {
            MO_ASSERT(mo_diags_any(&diags));
        } else {
            MO_ASSERT(!mo_diags_any(&diags));
            MO_ASSERT(mo_core_text_print(kinds[i], value, &first));
            mo_arena_free(&arena);
            void *again = mo_core_text_parse(&arena, kinds[i], (const char *)first.bytes,
                                             first.length, &diags);
            MO_ASSERT(again != NULL && !mo_diags_any(&diags));
            MO_ASSERT(mo_core_text_print(kinds[i], again, &second));
            MO_ASSERT(first.length == second.length &&
                      memcmp(first.bytes, second.bytes, first.length) == 0);
        }
        mo_buffer_free(&second);
        mo_buffer_free(&first);
        mo_diags_free(&diags);
        mo_arena_free(&arena);
    }
    return 0;
}
