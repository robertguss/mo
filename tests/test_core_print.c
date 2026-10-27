#include "base/limits.h"
#include "base/stack.h"
#include "core/core.h"
#include "test.h"

#include <string.h>

/* The differential harness (ci/check_core_generated.py) calls this program with Run files and
 * compares the canonical text it prints with the text Lean prints for the same file. Each
 * argument yields "R LENGTH\n" and then LENGTH bytes of canonical text, or "E CODE\n" when the
 * file is refused. Without arguments it checks a small program round trips. */
static MO_MUST_USE bool canonical_text(struct mo_arena *arena, struct mo_diags *diags,
                                       const char *text, size_t length, struct mo_buffer *out) {
    struct mo_core_run *run = mo_core_text_parse(arena, MO_CORE_TEXT_RUN, text, length, diags);
    return run != NULL && mo_core_text_print(MO_CORE_TEXT_RUN, run, out);
}

static MO_MUST_USE int print_file(const char *path) {
    struct mo_sources sources;
    struct mo_source_error error;
    struct mo_arena arena;
    struct mo_diags diags;
    struct mo_buffer out;
    uint32_t file;
    if (!mo_sources_init(&sources))
        return 8;
    if (!mo_diags_init(&diags)) {
        mo_sources_free(&sources);
        return 8;
    }
    mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
    mo_buffer_init(&out, MO_LIMIT_CORE_TEXT_SIZE * 2);
    int status = 8;
    if (mo_sources_add(&sources, path, &file, &error)) {
        const struct mo_source *s = &sources.files[file];
        if (canonical_text(&arena, &diags, (const char *)s->bytes, s->length, &out)) {
            status = printf("R %zu\n", out.length) < 0 ||
                             fwrite(out.bytes, 1, out.length, stdout) != out.length
                         ? 8
                         : 0;
        } else {
            status =
                printf("E %u\n", diags.count == 0 ? 0u : (unsigned)diags.items[0].code) < 0 ? 8 : 0;
        }
    }
    mo_buffer_free(&out);
    mo_arena_free(&arena);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
    return status;
}

static void test_round_trip(void) {
    static const char text[] =
        "(core 0.9 (Run (Program () ((FunDecl \"entry\" () (int) (lit (int 7)) \"M\" (function) "
        "0)) () \"entry\" (noSource)) (World () () () () () () 0 (RandomState 0x0000000000000001 "
        "0x0000000000000002 0x0000000000000003 0x0000000000000004) () ()) (entry ()) 0))\n";
    struct mo_arena arena;
    struct mo_diags diags;
    struct mo_buffer out;
    mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
    mo_buffer_init(&out, MO_LIMIT_CORE_TEXT_SIZE);
    CHECK(mo_diags_init(&diags));
    CHECK(canonical_text(&arena, &diags, text, sizeof text - 1, &out));
    CHECK(out.length == sizeof text - 1 && memcmp(out.bytes, text, out.length) == 0);
    mo_buffer_free(&out);
    mo_diags_free(&diags);
    mo_arena_free(&arena);
}

struct arguments {
    int argc, status;
    char **argv;
};
static void run(void *argument) {
    struct arguments *a = argument;
    if (a->argc <= 1) {
        test_round_trip();
        a->status = mo_test_report("test_core_print");
        return;
    }
    for (int i = 1; i < a->argc; i++) {
        a->status = print_file(a->argv[i]);
        if (a->status != 0)
            return;
    }
}
int main(int argc, char **argv) {
    struct arguments a = {.argc = argc, .argv = argv};
    int error = mo_with_stack(MO_SOURCE_STACK_SIZE, run, &a);
    CHECK(error == 0);
    return error == 0 ? a.status : mo_test_report("test_core_print stack");
}
