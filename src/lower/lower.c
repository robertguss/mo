#include "lower/lower.h"

#include "base/codes.h"
#include "base/limits.h"

#include <string.h>

struct lowering {
    struct mo_arena *arena;
    const struct mo_program *program;
    const struct mo_sources *sources;
    struct mo_diags *diags;
    struct mo_span span;
    bool failed;
};

static void refuse(struct lowering *p, uint16_t code, const char *message) {
    if (!p->failed) {
        struct mo_diag diag = {.code = code, .primary.span = p->span, .message = message};
        bool kept = mo_diags_add(p->diags, &diag);
        (void)kept;
        p->failed = true;
    }
}

static void *allocate(struct lowering *p, size_t count, size_t size, size_t alignment) {
    if (p->failed)
        return NULL;
    void *result = mo_arena_allocate_array(p->arena, count, size, alignment);
    if (result == NULL)
        refuse(p, MO_CODE_LOWER_NO_MEMORY, "the lowering arena has no room");
    else
        memset(result, 0, count * size);
    return result;
}
#define NEW(p, type, count) allocate((p), (count), sizeof(type), _Alignof(type))

static mo_core_string copy(struct lowering *p, const char *text) {
    size_t length = strlen(text);
    char *bytes = NEW(p, char, length + 1);
    if (bytes != NULL)
        memcpy(bytes, text, length);
    return (mo_core_string){bytes, length};
}

struct mo_core *mo_lower(struct mo_arena *arena, const struct mo_program *program,
                         const struct mo_sources *sources, struct mo_diags *diags) {
    MO_ASSERT(arena != NULL && program != NULL && sources != NULL && diags != NULL);
    struct lowering p = {.arena = arena,
                         .program = program,
                         .sources = sources,
                         .diags = diags,
                         .span = {.file = UINT32_MAX}};
    /* The first walking skeleton deliberately refuses instead of returning
     * an uncertified partial tree. Subsequent passes replace this boundary. */
    struct mo_core *result = NEW(&p, struct mo_core, 1);
    if (result == NULL)
        return NULL;
    result->entry = copy(&p, "%entry");
    if (!p.failed)
        refuse(&p, MO_CODE_LOWER_UNFINISHED, "this checked form is not lowered yet");
    return NULL;
}
