#include "check/check.h"

#include "base/codes.h"
#include "base/limits.h"

#include <string.h>

struct loaded {
    const char *name;
    struct mo_header header;
    uint32_t file;
    uint32_t left;
    uint32_t right;
    uint32_t height;
    uint8_t state;
};

struct loader {
    struct mo_arena *arena;
    struct mo_sources *sources;
    struct mo_diags *diags;
    struct loaded *items;
    struct mo_module_set *out;
    uint32_t index; /* AVL root, one-based; zero is absent */
};

static MO_MUST_USE bool report(struct loader *l, uint16_t code, struct mo_span span,
                               const char *message, const char *reason, const char *label) {
    return mo_diags_add(l->diags, &(struct mo_diag){.code = code,
                                                    .primary = {span, label},
                                                    .message = message,
                                                    .reason = reason});
}

static MO_MUST_USE void *allocate(struct loader *l, size_t count, size_t size, size_t alignment) {
    void *p = mo_arena_allocate_array(l->arena, count, size, alignment);
    if (p == NULL) {
        bool kept = report(l, MO_CODE_CHECK_NO_MEMORY, (struct mo_span){UINT32_MAX, 0, 0},
                           "the checking arena has no room", NULL, NULL);
        (void)kept;
    }
    return p;
}

static MO_MUST_USE bool source_error(struct loader *l, struct mo_source_error error,
                                     const char *path, struct mo_span span) {
    uint16_t code;
    const char *message;
    const char *reason = NULL;
    switch (error.problem) {
    case MO_SOURCE_UNREADABLE:
        code = MO_CODE_MODULE_MISSING;
        message = "a module's file can't be read";
        reason = "Every module is found from its name under the root folder (reference 2.1).";
        break;
    case MO_SOURCE_TOO_BIG:
        code = MO_CODE_FILE_TOO_BIG;
        message = "a source file is too big";
        break;
    case MO_SOURCE_PROGRAM_TOO_BIG:
        code = MO_CODE_PROGRAM_TOO_BIG;
        message = "the program's source is too big";
        break;
    case MO_SOURCE_TOO_MANY:
        code = MO_CODE_TOO_MANY_FILES;
        message = "the program has too many source files";
        break;
    case MO_SOURCE_PATH_TOO_LONG:
        code = MO_CODE_PATH_TOO_LONG;
        message = "a source file's path is too long";
        break;
    case MO_SOURCE_NO_MEMORY:
        code = MO_CODE_CHECK_NO_MEMORY;
        message = "source storage has no room";
        break;
    default:
        MO_ASSERT(false);
        return false;
    }
    return report(l, code, span, message, reason, path);
}

/* CapCase words, with an acronym kept together: XMLReader -> xml-reader. */
static MO_MUST_USE char *path_text(struct loader *l, const struct mo_syntax *path, bool filename) {
    char bytes[MO_LIMIT_PATH_SIZE + 1];
    size_t length = 0;
    for (uint32_t i = 0; i < path->count; i++) {
        const struct mo_syntax *part = path->children[i];
        uint32_t n = part->span.end - part->span.start;
        for (uint32_t j = 0; j < n; j++) {
            uint8_t c = part->text[j];
            bool capital = c >= 'A' && c <= 'Z';
            bool word = j != 0 && capital &&
                        ((part->text[j - 1] >= 'a' && part->text[j - 1] <= 'z') ||
                         (part->text[j - 1] >= '0' && part->text[j - 1] <= '9') ||
                         (j + 1 < n && part->text[j + 1] >= 'a' && part->text[j + 1] <= 'z'));
            if (length + (word && filename ? 2u : 1u) > MO_LIMIT_PATH_SIZE)
                goto too_long;
            if (word && filename)
                bytes[length++] = '-';
            bytes[length++] = (char)(capital && filename ? c + ('a' - 'A') : c);
        }
        if (i + 1 < path->count) {
            if (length == MO_LIMIT_PATH_SIZE)
                goto too_long;
            bytes[length++] = filename ? '/' : '.';
        }
    }
    if (filename) {
        if (length + 3 > MO_LIMIT_PATH_SIZE)
            goto too_long;
        memcpy(bytes + length, ".mo", 3);
        length += 3;
    }
    bytes[length] = '\0';
    char *result = allocate(l, length + 1, 1, 1);
    if (result != NULL)
        memcpy(result, bytes, length + 1);
    return result;
too_long : {
    bool kept =
        report(l, MO_CODE_PATH_TOO_LONG, path->span, "a module's path is too long", NULL, NULL);
    (void)kept;
    return NULL;
}
}

static MO_MUST_USE uint32_t height(const struct loader *l, uint32_t id) {
    return id == 0 ? 0 : l->items[id - 1].height;
}

static void refresh(struct loader *l, uint32_t id) {
    struct loaded *n = &l->items[id - 1];
    uint32_t a = height(l, n->left), b = height(l, n->right);
    n->height = 1 + (a > b ? a : b);
}

static MO_MUST_USE uint32_t rotate(struct loader *l, uint32_t id, bool right) {
    struct loaded *n = &l->items[id - 1];
    uint32_t next = right ? n->left : n->right;
    struct loaded *m = &l->items[next - 1];
    if (right) {
        n->left = m->right;
        m->right = id;
    } else {
        n->right = m->left;
        m->left = id;
    }
    refresh(l, id);
    refresh(l, next);
    return next;
}

static MO_MUST_USE uint32_t insert(struct loader *l, uint32_t root, uint32_t id) {
    if (root == 0)
        return id;
    struct loaded *n = &l->items[root - 1];
    int order = strcmp(l->items[id - 1].name, n->name);
    MO_ASSERT(order != 0);
    if (order < 0)
        n->left = insert(l, n->left, id);
    else
        n->right = insert(l, n->right, id);
    refresh(l, root);
    if (height(l, n->left) > height(l, n->right) + 1) {
        struct loaded *child = &l->items[n->left - 1];
        if (height(l, child->right) > height(l, child->left))
            n->left = rotate(l, n->left, false);
        return rotate(l, root, true);
    }
    if (height(l, n->right) > height(l, n->left) + 1) {
        struct loaded *child = &l->items[n->right - 1];
        if (height(l, child->left) > height(l, child->right))
            n->right = rotate(l, n->right, true);
        return rotate(l, root, false);
    }
    return root;
}

static MO_MUST_USE uint32_t find(const struct loader *l, const char *name) {
    uint32_t id = l->index;
    while (id != 0) {
        const struct loaded *n = &l->items[id - 1];
        int order = strcmp(name, n->name);
        if (order == 0)
            return id;
        id = order < 0 ? n->left : n->right;
    }
    return 0;
}

static MO_MUST_USE uint32_t read_module(struct loader *l, const char *file_path,
                                        const char *expected, struct mo_span origin) {
    uint32_t file;
    struct mo_source_error error;
    if (!mo_sources_add(l->sources, file_path, &file, &error)) {
        bool kept = source_error(l, error, file_path, origin);
        (void)kept;
        return 0;
    }
    struct mo_header header;
    if (!mo_read_header(l->arena, l->sources, file, &header, l->diags))
        return 0;
    char *name = path_text(l, header.path, false);
    if (name == NULL)
        return 0;
    if (expected != NULL && strcmp(name, expected) != 0) {
        bool kept = report(l, MO_CODE_MODULE_NAME, header.path->span,
                           "the module's name doesn't match its file",
                           "Each part of a module's name becomes a lowercase folder or file name "
                           "with hyphens between words (reference 2.1).",
                           expected);
        (void)kept;
        return 0;
    }
    MO_ASSERT(l->out->count < MO_LIMIT_FILES && find(l, name) == 0);
    uint32_t id = ++l->out->count;
    l->items[id - 1] = (struct loaded){.name = name, .header = header, .file = file, .height = 1};
    l->index = insert(l, l->index, id);
    return id;
}

static MO_MUST_USE char *join(struct loader *l, const char *root, const char *relative,
                              struct mo_span span) {
    size_t a = strlen(root), b = strlen(relative);
    if (a + b > MO_LIMIT_PATH_SIZE) {
        bool kept =
            report(l, MO_CODE_PATH_TOO_LONG, span, "a source file's path is too long", NULL, NULL);
        (void)kept;
        return NULL;
    }
    char *result = allocate(l, a + b + 1, 1, 1);
    if (result != NULL) {
        memcpy(result, root, a);
        memcpy(result + a, relative, b + 1);
    }
    return result;
}

struct frame {
    uint32_t id;
    uint32_t use;
};

static MO_MUST_USE bool graph(struct loader *l) {
    struct frame *stack = allocate(l, MO_LIMIT_FILES, sizeof(*stack), _Alignof(struct frame));
    if (stack == NULL)
        return false;
    uint32_t depth = 1, ordered = 0;
    stack[0] = (struct frame){1, 0};
    l->items[0].state = 1;
    while (depth != 0) {
        struct frame *frame = &stack[depth - 1];
        struct loaded *module = &l->items[frame->id - 1];
        if (frame->use == module->header.uses->count) {
            module->state = 2;
            l->out->order[ordered++] = frame->id - 1;
            depth--;
            continue;
        }
        const struct mo_syntax *use = module->header.uses->children[frame->use++];
        const struct mo_syntax *path = use->children[0];
        char *name = path_text(l, path, false);
        if (name == NULL)
            return false;
        uint32_t target = find(l, name);
        if (target == 0) {
            char *relative = path_text(l, path, true);
            char *full = relative == NULL ? NULL : join(l, l->out->root, relative, path->span);
            if (full == NULL)
                return false;
            target = read_module(l, full, name, path->span);
            if (target == 0)
                return false;
        }
        if (l->items[target - 1].state == 1) {
            bool kept =
                report(l, MO_CODE_MODULE_CYCLE, path->span, "modules use each other in a cycle",
                       "A cycle of modules that use each other is an error (reference "
                       "10.3).",
                       name);
            (void)kept;
            return false;
        }
        if (l->items[target - 1].state == 0) {
            MO_ASSERT(depth < MO_LIMIT_FILES);
            l->items[target - 1].state = 1;
            stack[depth++] = (struct frame){target, 0};
        }
    }
    MO_ASSERT(ordered == l->out->count);
    return true;
}

struct mo_module_set *mo_load(struct mo_arena *arena, struct mo_sources *sources, const char *entry,
                              const char *library, struct mo_diags *diags) {
    MO_ASSERT(arena != NULL && sources != NULL && entry != NULL && diags != NULL);
    MO_ASSERT(sources->count == 0);
    MO_ASSERT(library == NULL); /* library discovery is the next pass */
    struct loader l = {.arena = arena, .sources = sources, .diags = diags};
    l.out = allocate(&l, 1, sizeof(*l.out), _Alignof(struct mo_module_set));
    if (l.out == NULL)
        return NULL;
    *l.out = (struct mo_module_set){0};
    l.items = allocate(&l, MO_LIMIT_FILES, sizeof(*l.items), _Alignof(struct loaded));
    l.out->modules =
        allocate(&l, MO_LIMIT_FILES, sizeof(*l.out->modules), _Alignof(struct mo_module_syntax *));
    l.out->order = allocate(&l, MO_LIMIT_FILES, sizeof(*l.out->order), _Alignof(uint32_t));
    if (l.items == NULL || l.out->modules == NULL || l.out->order == NULL)
        return NULL;
    if (read_module(&l, entry, NULL, (struct mo_span){UINT32_MAX, 0, 0}) == 0)
        return NULL;
    char *relative = path_text(&l, l.items[0].header.path, true);
    if (relative == NULL)
        return NULL;
    size_t a = strlen(entry), b = strlen(relative);
    if (a < b || strcmp(entry + a - b, relative) != 0 || (a > b && entry[a - b - 1] != '/')) {
        bool kept = report(&l, MO_CODE_MODULE_NAME, l.items[0].header.path->span,
                           "the module's name doesn't match its file",
                           "Each part of a module's name becomes a lowercase folder or file name "
                           "with hyphens between words (reference 2.1).",
                           relative);
        (void)kept;
        return NULL;
    }
    char *root = allocate(&l, a - b + 1, 1, 1);
    if (root == NULL)
        return NULL;
    memcpy(root, entry, a - b);
    root[a - b] = '\0';
    l.out->root = root;
    if (!graph(&l))
        return NULL;
    uint32_t before = diags->count;
    for (uint32_t i = 0; i < l.out->count; i++) {
        l.out->modules[i] = mo_parse(arena, sources, l.items[i].file, diags);
        if (l.out->modules[i] == NULL)
            return NULL;
    }
    mo_diags_sort(diags);
    return diags->count == before && diags->dropped == 0 ? l.out : NULL;
}
