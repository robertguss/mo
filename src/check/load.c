#define _XOPEN_SOURCE 700

#include "check/check.h"

#include "base/buffer.h"
#include "base/codes.h"
#include "base/limits.h"
#include "base/path.h"
#include "base/utf8.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct dep_package {
    const char *key;
    const char *real_dir;
    const char *src_override;
    uint32_t root_count;
    const char *root_slash[3];
    const char *real_roots[3];
    uint32_t direct_count;
    uint32_t direct[MO_LIMIT_DEPENDENCIES];
};

struct loaded {
    const char *name;
    struct mo_header header;
    uint32_t file;
    uint32_t left;
    uint32_t right;
    uint32_t height;
    uint32_t pkg;
    uint8_t state;
};

struct loader {
    struct mo_arena *arena;
    struct mo_sources *sources;
    struct mo_diags *diags;
    struct loaded *items;
    struct mo_module_set *out;
    const char *real_root;
    const char *real_library;
    const char *real_entry; /* the entry's real path when the entry is a library file */
    const char *entry_library_rel;
    const struct mo_embedded_library *embedded;
    uint32_t entry_direct_count;
    uint32_t entry_direct[MO_LIMIT_DEPENDENCIES];
    uint32_t dep_count;
    struct dep_package *deps;
    uint32_t index; /* AVL root, one-based; zero is absent */
    bool memory_only, upgrade;
    bool failed;
};

static MO_MUST_USE bool report(struct loader *l, uint16_t code, struct mo_span span,
                               const char *message, const char *reason, const char *label) {
    return mo_diags_add(l->diags, &(struct mo_diag){.code = code,
                                                    .primary = {span, label},
                                                    .message = message,
                                                    .reason = reason});
}

static MO_MUST_USE void *allocate(struct loader *l, size_t count, size_t size, size_t alignment) {
    if (l->failed)
        return NULL;
    void *p = mo_arena_allocate_array(l->arena, count, size, alignment);
    if (p == NULL) {
        l->failed = true;
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

static MO_MUST_USE char *path_text(struct loader *l, const struct mo_syntax *path, bool filename) {
    char bytes[MO_LIMIT_PATH_SIZE + 1];
    char converted[MO_LIMIT_PATH_SIZE + 1];
    size_t length = 0;
    for (uint32_t i = 0; i < path->count; i++) {
        const struct mo_syntax *part = path->children[i];
        uint32_t n = part->span.end - part->span.start;
        if (n > MO_LIMIT_PATH_SIZE - length)
            goto too_long;
        memcpy(bytes + length, part->text, n);
        length += n;
        if (i + 1 < path->count) {
            if (length == MO_LIMIT_PATH_SIZE)
                goto too_long;
            bytes[length++] = '.';
        }
    }
    bytes[length] = '\0';
    const char *text = bytes;
    if (filename) {
        if (!mo_module_path((const uint8_t *)bytes, length, converted))
            goto too_long;
        text = converted;
        length = strlen(converted);
    }
    char *result = allocate(l, length + 1, 1, 1);
    if (result != NULL)
        memcpy(result, text, length + 1);
    return result;
too_long : {
    bool kept =
        report(l, MO_CODE_PATH_TOO_LONG, path->span, "a module's path is too long", NULL, NULL);
    (void)kept;
    return NULL;
}
}

static MO_MUST_USE char *normal_path(struct loader *l, const char *path) {
    char text[MO_LIMIT_PATH_SIZE + 1];
    size_t length = 0;
    if (*path == '/')
        text[length++] = '/';
    while (*path != '\0') {
        if (*path == '/') {
            path++;
            continue;
        }
        const char *part = path;
        while (*path != '\0' && *path != '/')
            path++;
        size_t n = (size_t)(path - part);
        if (n == 1 && *part == '.')
            continue;
        if (length != 0 && text[length - 1] != '/') {
            if (length == MO_LIMIT_PATH_SIZE)
                goto too_long;
            text[length++] = '/';
        }
        if (n > MO_LIMIT_PATH_SIZE - length)
            goto too_long;
        memcpy(text + length, part, n);
        length += n;
    }
    text[length] = '\0';
    char *result = allocate(l, length + 1, 1, 1);
    if (result != NULL)
        memcpy(result, text, length + 1);
    return result;
too_long : {
    bool kept = report(l, MO_CODE_PATH_TOO_LONG, (struct mo_span){UINT32_MAX, 0, 0},
                       "a source file's path is too long", NULL, NULL);
    (void)kept;
    return NULL;
}
}

static MO_MUST_USE char *real_path(struct loader *l, const char *path, struct mo_span origin,
                                   bool folder) {
    char resolved[MO_LIMIT_PATH_SIZE + 1];
    _Static_assert(sizeof(resolved) >= PATH_MAX, "realpath's buffer holds a system path");
    if (realpath(*path == '\0' ? "." : path, resolved) == NULL) {
        bool kept = folder ? report(l, MO_CODE_LIBRARY_UNREADABLE, origin,
                                    "the library folder can't be inspected", NULL, path)
                           : source_error(l, (struct mo_source_error){MO_SOURCE_UNREADABLE, errno},
                                          path, origin);
        (void)kept;
        return NULL;
    }
    size_t n = strlen(resolved);
    char *result = allocate(l, n + 1, 1, 1);
    if (result != NULL)
        memcpy(result, resolved, n + 1);
    return result;
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

static const char *const on_use[] = {
    "Api",          "Arvo.Audit", "Arvo.Auth",       "Arvo.Changeset",
    "Arvo.Cookie",  "Arvo.Csrf",  "Arvo.Flash",      "Arvo.Form",
    "Arvo.Inertia", "Arvo.Jobs",  "Arvo.Migrations", "Arvo.Session",
    "Arvo.WebTest", "Command",    "Crypto",          "Date",
    "Http",         "Json",       "Sqlite",          "Unicode16"};

static MO_MUST_USE bool loaded_on_use(const char *name) {
    for (size_t i = 0; i < sizeof on_use / sizeof *on_use; i++)
        if (strcmp(name, on_use[i]) == 0)
            return true;
    return false;
}

static MO_MUST_USE bool is_reserved_on_use(const char *name) {
    return loaded_on_use(name) && strncmp(name, "Arvo.", 5) != 0;
}

static MO_MUST_USE bool file_loaded_on_use(const char *relative) {
    for (size_t i = 0; i < sizeof on_use / sizeof *on_use; i++) {
        char file[MO_LIMIT_PATH_SIZE + 1];
        bool valid = mo_module_path((const uint8_t *)on_use[i], strlen(on_use[i]), file);
        MO_ASSERT(valid);
        if (strcmp(relative, file) == 0)
            return true;
    }
    return false;
}

static MO_MUST_USE uint32_t index_module_pkg(struct loader *l, uint32_t file, const char *expected,
                                             struct mo_span origin, bool library, uint32_t pkg) {
    struct mo_header header;
    uint32_t before = l->diags->count;
    if (!mo_read_header(l->arena, l->sources, file, &header, l->diags) ||
        l->diags->count != before || l->diags->dropped != 0)
        return 0;
    char *name = path_text(l, header.path, false);
    if (name == NULL)
        return 0;
    if (expected != NULL && strcmp(name, expected) != 0) {
        struct mo_label secondary = {header.path->span, "the module declares this name"};
        bool kept = mo_diags_add(
            l->diags,
            &(struct mo_diag){
                .code = MO_CODE_MODULE_NAME,
                .primary = {origin, expected},
                .message = "the module's name doesn't match its file",
                .reason = "Each part of a module's name becomes a lowercase folder or file name "
                          "with hyphens between words (reference 2.1).",
                .secondary = &secondary,
                .secondary_count = 1});
        (void)kept;
        return 0;
    }
    uint32_t previous = find(l, name);
    if (previous != 0 || (!library && is_reserved_on_use(name))) {
        struct mo_span conflict = library && previous != 0 && !l->out->library[previous - 1]
                                      ? l->items[previous - 1].header.path->span
                                      : header.path->span;
        bool kept = report(l, MO_CODE_MODULE_RESERVED, conflict,
                           "a program can't declare a library module's name",
                           "A program can't declare a module with a library module's name "
                           "(reference 10.3).",
                           name);
        (void)kept;
        return 0;
    }
    MO_ASSERT(l->out->count < MO_LIMIT_FILES);
    uint32_t id = ++l->out->count;
    l->items[id - 1] =
        (struct loaded){.name = name, .header = header, .file = file, .height = 1, .pkg = pkg};
    l->out->names[id - 1] = name;
    l->out->library[id - 1] = library;
    l->out->dependency[id - 1] = !library && pkg != 0 && pkg != UINT32_MAX;
    l->out->uses[id - 1] = allocate(l, header.uses->count, sizeof(uint32_t), _Alignof(uint32_t));
    if (l->out->uses[id - 1] == NULL)
        return 0;
    l->index = insert(l, l->index, id);
    return id;
}

static MO_MUST_USE uint32_t index_module(struct loader *l, uint32_t file, const char *expected,
                                         struct mo_span origin, bool library) {
    return index_module_pkg(l, file, expected, origin, library, library ? UINT32_MAX : 0);
}

static MO_MUST_USE uint32_t read_module_in_root(struct loader *l, const char *file_path,
                                                const char *confine_root, const char *expected,
                                                struct mo_span origin, bool library, uint32_t pkg) {
    MO_ASSERT(!l->memory_only || library);
    MO_ASSERT(!library || l->real_library != NULL || l->entry_library_rel != NULL);
    if (expected != NULL || (library && l->real_library != NULL)) {
        const char *root = library ? l->real_library : confine_root;
        MO_ASSERT(root != NULL);
        char *real = real_path(l, file_path, origin, false);
        if (real == NULL)
            return 0;
        size_t n = strlen(root);
        if (strncmp(root, real, n) != 0 || (n != 1 && real[n] != '/')) {
            bool kept = report(l, MO_CODE_MODULE_OUTSIDE_ROOT, origin,
                               "a module's file is outside its root folder", NULL, file_path);
            (void)kept;
            return 0;
        }
    }
    uint32_t file;
    struct mo_source_error error;
    if (!mo_sources_add(l->sources, file_path, &file, &error)) {
        bool kept = source_error(l, error, file_path, origin);
        (void)kept;
        return 0;
    }
    return index_module_pkg(l, file, expected, origin, library, pkg);
}

static MO_MUST_USE uint32_t read_module(struct loader *l, const char *file_path,
                                        const char *expected, struct mo_span origin, bool library) {
    return read_module_in_root(l, file_path, l->real_root, expected, origin, library,
                               library ? UINT32_MAX : 0);
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

struct folder {
    const char *path;
    struct folder *next;
};

static MO_MUST_USE int compare_paths(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static MO_MUST_USE bool library_files(struct loader *l, const char *root) {
    struct folder first = {root, NULL};
    struct folder *pending = &first;
    const char **files = allocate(l, MO_LIMIT_FILES, sizeof(*files), _Alignof(const char *));
    if (files == NULL)
        return false;
    uint32_t count = 0, entries = 0;
    struct mo_span unavailable = {UINT32_MAX, 0, 0};
    while (pending != NULL) {
        const char *path = pending->path;
        pending = pending->next;
        size_t n = strlen(path);
        char *prefix = join(l, path, n != 0 && path[n - 1] == '/' ? "" : "/", unavailable);
        if (prefix == NULL)
            return false;
        DIR *directory = opendir(path);
        if (directory == NULL) {
            bool kept = report(l, MO_CODE_LIBRARY_UNREADABLE, unavailable,
                               "the library folder can't be inspected", NULL, path);
            (void)kept;
            return false;
        }
        bool ok = true;
        for (;;) {
            errno = 0;
            struct dirent *entry = readdir(directory);
            if (entry == NULL) {
                if (errno != 0) {
                    bool kept = report(l, MO_CODE_LIBRARY_UNREADABLE, unavailable,
                                       "the library folder can't be inspected", NULL, path);
                    (void)kept;
                    ok = false;
                }
                break;
            }
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;
            if (++entries > MO_LIMIT_LIBRARY_ENTRIES) {
                bool kept = report(l, MO_CODE_LIBRARY_TOO_BIG, unavailable,
                                   "the library has too many directory entries", NULL, root);
                (void)kept;
                ok = false;
                break;
            }
            char *full = join(l, prefix, entry->d_name, unavailable);
            if (full == NULL) {
                ok = false;
                break;
            }
            struct stat status;
            if (lstat(full, &status) != 0) {
                bool kept = report(l, MO_CODE_LIBRARY_UNREADABLE, unavailable,
                                   "the library folder can't be inspected", NULL, full);
                (void)kept;
                ok = false;
                break;
            }
            if (S_ISDIR(status.st_mode)) {
                struct folder *next = allocate(l, 1, sizeof(*next), _Alignof(struct folder));
                if (next == NULL) {
                    ok = false;
                    break;
                }
                *next = (struct folder){full, pending};
                pending = next;
            } else {
                size_t length = strlen(full);
                if ((!S_ISREG(status.st_mode) && !S_ISLNK(status.st_mode)) || length < 3 ||
                    strcmp(full + length - 3, ".mo") != 0)
                    continue;
                if (count == MO_LIMIT_FILES - 1) {
                    bool kept = report(l, MO_CODE_TOO_MANY_FILES, unavailable,
                                       "the program has too many source files", NULL, root);
                    (void)kept;
                    ok = false;
                    break;
                }
                files[count++] = full;
            }
        }
        if (closedir(directory) != 0) {
            if (ok) {
                bool kept = report(l, MO_CODE_LIBRARY_UNREADABLE, unavailable,
                                   "the library folder can't be inspected", NULL, path);
                (void)kept;
            }
            ok = false;
        }
        if (!ok)
            return false;
    }
    qsort(files, count, sizeof(*files), compare_paths);
    size_t root_length = strlen(root);
    for (uint32_t i = 0; i < count; i++) {
        if (l->real_entry != NULL) {
            char resolved[MO_LIMIT_PATH_SIZE + 1];
            if (realpath(files[i], resolved) != NULL && strcmp(resolved, l->real_entry) == 0)
                continue; /* the entry is this file, already loaded as module 0 */
        }
        const char *actual = files[i] + root_length;
        if (*actual == '/')
            actual++;
        if (file_loaded_on_use(actual))
            continue;
        uint32_t id = read_module(l, files[i], NULL, unavailable, true);
        if (id == 0)
            return false;
        char *relative = path_text(l, l->items[id - 1].header.path, true);
        if (relative == NULL)
            return false;
        if (strcmp(actual, relative) != 0) {
            bool kept = report(l, MO_CODE_MODULE_NAME, l->items[id - 1].header.path->span,
                               "the module's name doesn't match its file",
                               "Each part of a module's name becomes a lowercase folder or file "
                               "name with hyphens between words (reference 2.1).",
                               relative);
            (void)kept;
            return false;
        }
    }
    return true;
}

static MO_MUST_USE char *library_file(struct loader *l, const struct mo_syntax *path) {
    char *relative = path_text(l, path, true);
    if (l->real_library == NULL)
        return relative == NULL ? NULL : join(l, "lib/", relative, path->span);
    char *prefix = join(l, l->real_library, "/", path->span);
    return relative == NULL || prefix == NULL ? NULL : join(l, prefix, relative, path->span);
}

static MO_MUST_USE uint32_t read_embedded_module(struct loader *l,
                                                 const struct mo_embedded_source *src,
                                                 const char *expected, struct mo_span origin) {
    char *virtual_path = join(l, "<lib>/", src->path, origin);
    if (virtual_path == NULL)
        return 0;
    uint32_t file;
    struct mo_source_error error;
    if (!mo_sources_add_bytes(l->sources, virtual_path, src->bytes, src->length, &file, &error)) {
        bool kept = source_error(l, error, virtual_path, origin);
        (void)kept;
        return 0;
    }
    return index_module(l, file, expected, origin, true);
}

static MO_MUST_USE bool embedded_library_files(struct loader *l,
                                               const struct mo_embedded_library *embedded) {
    struct mo_span unavailable = {UINT32_MAX, 0, 0};
    for (uint32_t i = 0; i < embedded->count; i++) {
        const struct mo_embedded_source *src = &embedded->files[i];
        if (l->entry_library_rel != NULL && strcmp(src->path, l->entry_library_rel) == 0)
            continue;
        if (file_loaded_on_use(src->path))
            continue;
        uint32_t id = read_embedded_module(l, src, NULL, unavailable);
        if (id == 0)
            return false;
        char *relative = path_text(l, l->items[id - 1].header.path, true);
        if (relative == NULL)
            return false;
        if (strcmp(src->path, relative) != 0) {
            bool kept = report(l, MO_CODE_MODULE_NAME, l->items[id - 1].header.path->span,
                               "the module's name doesn't match its file",
                               "Each part of a module's name becomes a lowercase folder or file "
                               "name with hyphens between words (reference 2.1).",
                               relative);
            (void)kept;
            return false;
        }
    }
    return true;
}

static const struct mo_embedded_source *find_embedded_on_use(struct loader *l,
                                                             const struct mo_syntax *path) {
    if (l->embedded == NULL)
        return NULL;
    char *relative = path_text(l, path, true);
    if (relative == NULL)
        return NULL;
    for (uint32_t i = 0; i < l->embedded->count; i++)
        if (strcmp(l->embedded->files[i].path, relative) == 0)
            return &l->embedded->files[i];
    return NULL;
}

static MO_MUST_USE bool manifest_error(struct loader *l, const char *manifest_path,
                                       const char *message, const char *reason) {
    struct mo_span unavailable = {UINT32_MAX, 0, 0};
    const char *shown = manifest_path;
    if (manifest_path != NULL && l->real_root != NULL && l->out != NULL && l->out->root != NULL) {
        size_t rlen = strlen(l->real_root);
        if (strncmp(manifest_path, l->real_root, rlen) == 0 && manifest_path[rlen] == '/') {
            char *rel = join(l, l->out->root, manifest_path + rlen + 1, unavailable);
            if (rel != NULL)
                shown = rel;
        }
    }
    bool kept = report(l, MO_CODE_PACKAGE_MANIFEST, unavailable, message, reason, shown);
    (void)kept;
    return false;
}

static void toml_skip_ws(const char *s, size_t len, size_t *pos) {
    while (*pos < len && (s[*pos] == ' ' || s[*pos] == '\t'))
        (*pos)++;
}

static MO_MUST_USE bool toml_is_bare_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-';
}

static MO_MUST_USE char *toml_parse_bare_key(struct loader *l, const char *s, size_t len,
                                             size_t *pos) {
    size_t start = *pos;
    while (*pos < len && toml_is_bare_char(s[*pos]))
        (*pos)++;
    if (*pos == start)
        return NULL;
    size_t klen = *pos - start;
    char *out = allocate(l, klen + 1, 1, 1);
    if (out != NULL) {
        memcpy(out, s + start, klen);
        out[klen] = '\0';
    }
    return out;
}

static MO_MUST_USE bool toml_parse_string(struct loader *l, const char *s, size_t len, size_t *pos,
                                          char **out) {
    *out = NULL;
    if (*pos >= len || s[*pos] != '"')
        return false;
    (*pos)++;
    char *buf = allocate(l, len - *pos + 1, 1, 1);
    if (buf == NULL)
        return false;
    size_t n = 0;
    while (*pos < len) {
        char c = s[(*pos)++];
        if (c == '"') {
            buf[n] = '\0';
            *out = buf;
            return true;
        }
        if ((unsigned char)c < 0x20 || (unsigned char)c == 0x7f)
            return false;
        if (c == '\\') {
            if (*pos >= len)
                return false;
            char esc = s[(*pos)++];
            switch (esc) {
            case '"':
                buf[n++] = '"';
                break;
            case '\\':
                buf[n++] = '\\';
                break;
            case 'n':
                buf[n++] = '\n';
                break;
            case 'r':
                buf[n++] = '\r';
                break;
            case 't':
                buf[n++] = '\t';
                break;
            default:
                return false;
            }
        } else {
            buf[n++] = c;
        }
    }
    return false;
}

static MO_MUST_USE char *ensure_trailing_slash(struct loader *l, const char *dir) {
    size_t n = strlen(dir);
    if (n > 0 && dir[n - 1] == '/')
        return join(l, dir, "", (struct mo_span){UINT32_MAX, 0, 0});
    return join(l, dir, "/", (struct mo_span){UINT32_MAX, 0, 0});
}

static MO_MUST_USE bool load_manifest_recursive(struct loader *l, const char *manifest_path,
                                                const char *real_manifest_dir,
                                                const char *stack_dirs[MO_LIMIT_DEPENDENCIES + 1],
                                                uint32_t stack_depth, uint32_t *direct_out,
                                                uint32_t *direct_count_out);

static MO_MUST_USE bool resolve_dep_entry(struct loader *l, const char *manifest_path,
                                          const char *real_manifest_dir, const char *dep_key,
                                          const char *dep_path, const char *dep_src,
                                          const char *stack_dirs[MO_LIMIT_DEPENDENCIES + 1],
                                          uint32_t stack_depth, uint32_t *dep_id_out) {
    struct mo_span unavailable = {UINT32_MAX, 0, 0};
    const char *raw_dir = dep_path;
    if (dep_path[0] != '/') {
        char *base_slash = ensure_trailing_slash(l, real_manifest_dir);
        if (base_slash == NULL)
            return false;
        raw_dir = join(l, base_slash, dep_path, unavailable);
        if (raw_dir == NULL)
            return false;
    }

    char resolved_dir[MO_LIMIT_PATH_SIZE + 1];
    struct stat st;
    if (realpath(raw_dir, resolved_dir) == NULL || stat(resolved_dir, &st) != 0 ||
        !S_ISDIR(st.st_mode)) {
        return manifest_error(l, manifest_path, "a dependency folder in mo.toml does not exist",
                              "Check that each path = \"...\" under [dependencies] points to an "
                              "existing folder.");
    }

    for (uint32_t s = 0; s < stack_depth; s++) {
        if (strcmp(stack_dirs[s], resolved_dir) == 0)
            return manifest_error(l, manifest_path, "mo.toml dependencies form a cycle",
                                  "Remove the circular path dependency between mo.toml manifests.");
    }

    const char *norm_src = dep_src != NULL ? dep_src : "";
    for (uint32_t d = 0; d < l->dep_count; d++) {
        if (strcmp(l->deps[d].real_dir, resolved_dir) == 0 &&
            strcmp(l->deps[d].src_override, norm_src) == 0) {
            *dep_id_out = d + 1;
            return true;
        }
    }

    if (l->dep_count >= MO_LIMIT_DEPENDENCIES)
        return manifest_error(l, manifest_path, "the program declares too many dependencies",
                              "A program may load at most 64 path dependencies via mo.toml.");

    uint32_t idx = l->dep_count++;
    struct dep_package *pkg = &l->deps[idx];
    *pkg = (struct dep_package){0};

    size_t dlen = strlen(resolved_dir);
    char *copied_real = allocate(l, dlen + 1, 1, 1);
    char *copied_src = allocate(l, strlen(norm_src) + 1, 1, 1);
    if (copied_real == NULL || copied_src == NULL)
        return false;
    memcpy(copied_real, resolved_dir, dlen + 1);
    memcpy(copied_src, norm_src, strlen(norm_src) + 1);
    pkg->key = dep_key;
    pkg->real_dir = copied_real;
    pkg->src_override = copied_src;

    if (dep_src != NULL) {
        if (dep_src[0] == '/' || strchr(dep_src, '\\') != NULL)
            return manifest_error(
                l, manifest_path, "a dependency src override in mo.toml must be a relative folder",
                "Use a relative subfolder inside the dependency, such as \"src\".");
        char *dep_slash = ensure_trailing_slash(l, copied_real);
        char *sub_cand = dep_slash == NULL ? NULL : join(l, dep_slash, dep_src, unavailable);
        if (sub_cand == NULL)
            return false;
        char resolved_sub[MO_LIMIT_PATH_SIZE + 1];
        struct stat sub_st;
        if (realpath(sub_cand, resolved_sub) == NULL || stat(resolved_sub, &sub_st) != 0 ||
            !S_ISDIR(sub_st.st_mode))
            return manifest_error(l, manifest_path,
                                  "a dependency src folder in mo.toml does not exist", NULL);
        if (strncmp(copied_real, resolved_sub, dlen) != 0 ||
            (dlen != 1 && resolved_sub[dlen] != '\0' && resolved_sub[dlen] != '/'))
            return manifest_error(l, manifest_path,
                                  "a dependency src folder escapes its package folder", NULL);
        size_t slen = strlen(resolved_sub);
        char *sub_copy = allocate(l, slen + 1, 1, 1);
        if (sub_copy == NULL)
            return false;
        memcpy(sub_copy, resolved_sub, slen + 1);
        char *sub_slash = ensure_trailing_slash(l, sub_copy);
        if (sub_slash == NULL)
            return false;
        pkg->real_roots[0] = sub_copy;
        pkg->root_slash[0] = sub_slash;
        pkg->root_count = 1;
    } else {
        static const char *const defaults[3] = {"src", "lib", ""};
        char *dep_slash = ensure_trailing_slash(l, copied_real);
        if (dep_slash == NULL)
            return false;
        for (size_t k = 0; k < 3; k++) {
            char *cand =
                defaults[k][0] == '\0' ? copied_real : join(l, dep_slash, defaults[k], unavailable);
            if (cand == NULL)
                return false;
            char resolved_sub[MO_LIMIT_PATH_SIZE + 1];
            struct stat sub_st;
            if (realpath(cand, resolved_sub) != NULL && stat(resolved_sub, &sub_st) == 0 &&
                S_ISDIR(sub_st.st_mode) && strncmp(copied_real, resolved_sub, dlen) == 0 &&
                (dlen == 1 || resolved_sub[dlen] == '\0' || resolved_sub[dlen] == '/')) {
                bool dup_root = false;
                for (uint32_t r = 0; r < pkg->root_count; r++)
                    if (strcmp(pkg->real_roots[r], resolved_sub) == 0)
                        dup_root = true;
                if (dup_root)
                    continue;
                size_t slen = strlen(resolved_sub);
                char *sub_copy = allocate(l, slen + 1, 1, 1);
                if (sub_copy == NULL)
                    return false;
                memcpy(sub_copy, resolved_sub, slen + 1);
                char *sub_slash = ensure_trailing_slash(l, sub_copy);
                if (sub_slash == NULL)
                    return false;
                pkg->real_roots[pkg->root_count] = sub_copy;
                pkg->root_slash[pkg->root_count] = sub_slash;
                pkg->root_count++;
            }
        }
    }

    char *dep_slash = ensure_trailing_slash(l, copied_real);
    char *sub_manifest = dep_slash == NULL ? NULL : join(l, dep_slash, "mo.toml", unavailable);
    if (sub_manifest == NULL)
        return false;
    struct stat mst;
    if (stat(sub_manifest, &mst) == 0) {
        if (!S_ISREG(mst.st_mode))
            return manifest_error(l, sub_manifest, "mo.toml is not a regular file", NULL);
        stack_dirs[stack_depth] = copied_real;
        if (!load_manifest_recursive(l, sub_manifest, copied_real, stack_dirs, stack_depth + 1,
                                     pkg->direct, &pkg->direct_count))
            return false;
    }

    *dep_id_out = idx + 1;
    return true;
}

static MO_MUST_USE bool load_manifest_recursive(struct loader *l, const char *manifest_path,
                                                const char *real_manifest_dir,
                                                const char *stack_dirs[MO_LIMIT_DEPENDENCIES + 1],
                                                uint32_t stack_depth, uint32_t *direct_out,
                                                uint32_t *direct_count_out) {
    *direct_count_out = 0;
    FILE *fp = fopen(manifest_path, "rb");
    if (fp == NULL)
        return manifest_error(l, manifest_path, "mo.toml can't be read", NULL);
    struct stat st;
    if (fstat(fileno(fp), &st) != 0 || !S_ISREG(st.st_mode)) {
        (void)fclose(fp);
        return manifest_error(l, manifest_path, "mo.toml is not a regular file", NULL);
    }
    if (st.st_size < 0 || (uint64_t)st.st_size > MO_LIMIT_MANIFEST_SIZE) {
        (void)fclose(fp);
        return manifest_error(l, manifest_path, "mo.toml exceeds 64 KiB", NULL);
    }
    size_t fsize = (size_t)st.st_size;
    char *raw = allocate(l, fsize + 1, 1, 1);
    if (raw == NULL) {
        (void)fclose(fp);
        return false;
    }
    size_t got = fsize > 0 ? fread(raw, 1, fsize, fp) : 0;
    bool read_err = ferror(fp) != 0;
    (void)fclose(fp);
    if (read_err || got != fsize)
        return manifest_error(l, manifest_path, "mo.toml can't be read", NULL);
    raw[fsize] = '\0';

    if (l->out->manifest_count < MO_LIMIT_DEPENDENCIES + 1)
        l->out->manifests[l->out->manifest_count++] = manifest_path;

    for (size_t i = 0; i < fsize; i++) {
        if (raw[i] == '\0')
            return manifest_error(l, manifest_path, "mo.toml contains a NUL byte", NULL);
    }
    if (!mo_utf8_valid((const uint8_t *)raw, fsize))
        return manifest_error(l, manifest_path, "mo.toml is not valid UTF-8", NULL);

    enum { SEC_NONE, SEC_PACKAGE, SEC_DEPS } section = SEC_NONE;
    bool seen_pkg = false, seen_pkg_name = false, seen_pkg_version = false, seen_deps = false;
    const char *dep_keys[MO_LIMIT_DEPENDENCIES];
    uint32_t local_dep_count = 0;

    size_t pos = 0;
    while (pos < fsize) {
        size_t line_start = pos;
        while (pos < fsize && raw[pos] != '\n')
            pos++;
        size_t line_end = pos;
        if (pos < fsize && raw[pos] == '\n')
            pos++;
        if (line_end > line_start && raw[line_end - 1] == '\r')
            line_end--;

        /* Strip comments outside quoted strings and reject stray control bytes. */
        bool in_quotes = false;
        size_t clean_end = line_end;
        for (size_t i = line_start; i < line_end; i++) {
            unsigned char c = (unsigned char)raw[i];
            if (!in_quotes && c == '#') {
                clean_end = i;
                break;
            }
            if ((c < 0x20 && c != '\t') || c == 0x7f)
                return manifest_error(l, manifest_path, "mo.toml contains an invalid control byte",
                                      NULL);
            if (c == '"')
                in_quotes = !in_quotes;
            else if (in_quotes && c == '\\' && i + 1 < line_end)
                i++;
        }
        if (in_quotes)
            return manifest_error(l, manifest_path, "unterminated string in mo.toml", NULL);

        const char *line = raw + line_start;
        size_t llen = clean_end - line_start;
        size_t p = 0;
        toml_skip_ws(line, llen, &p);
        while (llen > p && (line[llen - 1] == ' ' || line[llen - 1] == '\t'))
            llen--;
        if (p == llen)
            continue;

        if (line[p] == '[') {
            if (line[llen - 1] != ']')
                return manifest_error(l, manifest_path, "malformed section header in mo.toml",
                                      "Write [package] or [dependencies] on its own line.");
            p++;
            size_t sec_end = llen - 1;
            toml_skip_ws(line, sec_end, &p);
            while (sec_end > p && (line[sec_end - 1] == ' ' || line[sec_end - 1] == '\t'))
                sec_end--;
            size_t slen = sec_end - p;
            if (slen == 7 && memcmp(line + p, "package", 7) == 0) {
                if (seen_pkg)
                    return manifest_error(l, manifest_path,
                                          "duplicate [package] section in mo.toml", NULL);
                seen_pkg = true;
                section = SEC_PACKAGE;
            } else if (slen == 12 && memcmp(line + p, "dependencies", 12) == 0) {
                if (seen_deps)
                    return manifest_error(l, manifest_path,
                                          "duplicate [dependencies] section in mo.toml", NULL);
                seen_deps = true;
                section = SEC_DEPS;
            } else {
                return manifest_error(
                    l, manifest_path, "unknown section in mo.toml",
                    "Only [package] and [dependencies] are supported in mo.toml.");
            }
            continue;
        }

        if (section == SEC_NONE)
            return manifest_error(l, manifest_path,
                                  "keys in mo.toml must appear under [package] or [dependencies]",
                                  NULL);

        char *key = toml_parse_bare_key(l, line, llen, &p);
        if (key == NULL)
            return l->failed ? false
                             : manifest_error(l, manifest_path, "invalid key in mo.toml", NULL);
        toml_skip_ws(line, llen, &p);
        if (p >= llen || line[p] != '=')
            return manifest_error(l, manifest_path, "expected '=' after key in mo.toml", NULL);
        p++;
        toml_skip_ws(line, llen, &p);

        if (section == SEC_PACKAGE) {
            char *val = NULL;
            if (!toml_parse_string(l, line, llen, &p, &val))
                return l->failed
                           ? false
                           : manifest_error(l, manifest_path,
                                            "package fields in mo.toml must be strings", NULL);
            toml_skip_ws(line, llen, &p);
            if (p != llen)
                return manifest_error(l, manifest_path, "unexpected trailing tokens in mo.toml",
                                      NULL);
            if (strcmp(key, "name") == 0) {
                if (seen_pkg_name)
                    return manifest_error(l, manifest_path, "duplicate package name in mo.toml",
                                          NULL);
                if (val[0] == '\0')
                    return manifest_error(l, manifest_path,
                                          "package name in mo.toml cannot be empty", NULL);
                seen_pkg_name = true;
            } else if (strcmp(key, "version") == 0) {
                if (seen_pkg_version)
                    return manifest_error(l, manifest_path, "duplicate package version in mo.toml",
                                          NULL);
                if (val[0] == '\0')
                    return manifest_error(l, manifest_path,
                                          "package version in mo.toml cannot be empty", NULL);
                seen_pkg_version = true;
            } else {
                return manifest_error(l, manifest_path, "unknown key under [package] in mo.toml",
                                      "Supported [package] keys are name and version.");
            }
        } else {
            for (uint32_t prev = 0; prev < local_dep_count; prev++) {
                if (strcmp(dep_keys[prev], key) == 0)
                    return manifest_error(l, manifest_path, "duplicate dependency key in mo.toml",
                                          NULL);
            }
            if (local_dep_count >= MO_LIMIT_DEPENDENCIES)
                return manifest_error(l, manifest_path, "too many dependencies in mo.toml", NULL);
            if (p >= llen || line[p] != '{' || line[llen - 1] != '}')
                return manifest_error(
                    l, manifest_path,
                    "a dependency in mo.toml must be an inline table { path = \"...\" }", NULL);
            p++;
            size_t tbl_end = llen - 1;
            toml_skip_ws(line, tbl_end, &p);
            char *dep_path = NULL;
            char *dep_src = NULL;
            if (p == tbl_end)
                return manifest_error(l, manifest_path,
                                      "a dependency table in mo.toml requires path = \"...\"",
                                      NULL);
            for (;;) {
                char *tkey = toml_parse_bare_key(l, line, tbl_end, &p);
                if (tkey == NULL)
                    return l->failed ? false
                                     : manifest_error(l, manifest_path,
                                                      "invalid inline table key in mo.toml", NULL);
                toml_skip_ws(line, tbl_end, &p);
                if (p >= tbl_end || line[p] != '=')
                    return manifest_error(l, manifest_path,
                                          "expected '=' in dependency inline table in mo.toml",
                                          NULL);
                p++;
                toml_skip_ws(line, tbl_end, &p);
                char *tval = NULL;
                if (!toml_parse_string(l, line, tbl_end, &p, &tval))
                    return l->failed
                               ? false
                               : manifest_error(l, manifest_path,
                                                "dependency fields in mo.toml must be strings",
                                                NULL);
                if (strcmp(tkey, "path") == 0) {
                    if (dep_path != NULL)
                        return manifest_error(l, manifest_path,
                                              "duplicate path key in dependency table", NULL);
                    if (tval[0] == '\0')
                        return manifest_error(l, manifest_path,
                                              "dependency path in mo.toml cannot be empty", NULL);
                    dep_path = tval;
                } else if (strcmp(tkey, "src") == 0) {
                    if (dep_src != NULL)
                        return manifest_error(l, manifest_path,
                                              "duplicate src key in dependency table", NULL);
                    if (tval[0] == '\0')
                        return manifest_error(l, manifest_path,
                                              "dependency src in mo.toml cannot be empty", NULL);
                    dep_src = tval;
                } else {
                    return manifest_error(l, manifest_path,
                                          "unknown key in dependency inline table in mo.toml",
                                          "Supported dependency keys are path and src.");
                }
                toml_skip_ws(line, tbl_end, &p);
                if (p == tbl_end)
                    break;
                if (line[p] != ',')
                    return manifest_error(l, manifest_path,
                                          "expected ',' or '}' in dependency inline table", NULL);
                p++;
                toml_skip_ws(line, tbl_end, &p);
                if (p == tbl_end)
                    return manifest_error(l, manifest_path,
                                          "trailing comma in dependency inline table in mo.toml",
                                          NULL);
            }
            if (dep_path == NULL)
                return manifest_error(l, manifest_path,
                                      "a dependency table in mo.toml requires path = \"...\"",
                                      NULL);

            dep_keys[local_dep_count++] = key;
            uint32_t dep_id = 0;
            if (!resolve_dep_entry(l, manifest_path, real_manifest_dir, key, dep_path, dep_src,
                                   stack_dirs, stack_depth, &dep_id))
                return false;
            direct_out[(*direct_count_out)++] = dep_id;
        }
    }

    if (seen_pkg && !seen_pkg_name)
        return manifest_error(l, manifest_path, "[package] in mo.toml requires name = \"...\"",
                              NULL);
    return true;
}

static MO_MUST_USE bool discover_and_load_manifest(struct loader *l) {
    if (l->memory_only || l->upgrade || l->real_entry != NULL || l->entry_library_rel != NULL ||
        l->real_root == NULL)
        return true;

    char cur[MO_LIMIT_PATH_SIZE + 1];
    size_t rlen = strlen(l->real_root);
    if (rlen >= sizeof cur)
        return true;
    memcpy(cur, l->real_root, rlen + 1);

    for (;;) {
        char cand[MO_LIMIT_PATH_SIZE + 16];
        int n = strcmp(cur, "/") == 0 ? snprintf(cand, sizeof cand, "/mo.toml")
                                      : snprintf(cand, sizeof cand, "%s/mo.toml", cur);
        if (n > 0 && (size_t)n < sizeof cand) {
            struct stat st;
            if (stat(cand, &st) == 0) {
                char *manifest_copy = allocate(l, (size_t)n + 1, 1, 1);
                size_t clen = strlen(cur);
                char *dir_copy = allocate(l, clen + 1, 1, 1);
                if (manifest_copy == NULL || dir_copy == NULL)
                    return false;
                memcpy(manifest_copy, cand, (size_t)n + 1);
                memcpy(dir_copy, cur, clen + 1);
                if (!S_ISREG(st.st_mode))
                    return manifest_error(l, manifest_copy, "mo.toml is not a regular file", NULL);
                l->deps = allocate(l, MO_LIMIT_DEPENDENCIES, sizeof *l->deps,
                                   _Alignof(struct dep_package));
                if (l->deps == NULL)
                    return false;
                const char *stack_dirs[MO_LIMIT_DEPENDENCIES + 1] = {dir_copy};
                return load_manifest_recursive(l, manifest_copy, dir_copy, stack_dirs, 1,
                                               l->entry_direct, &l->entry_direct_count);
            }
        }
        if (strcmp(cur, "/") == 0)
            break;
        char *slash = strrchr(cur, '/');
        if (slash == NULL)
            break;
        if (slash == cur)
            cur[1] = '\0';
        else
            *slash = '\0';
    }
    return true;
}

static MO_MUST_USE bool graph(struct loader *l) {
    struct frame *stack = allocate(l, MO_LIMIT_FILES, sizeof(*stack), _Alignof(struct frame));
    if (stack == NULL)
        return false;
    uint32_t ordered = 0;
    for (uint32_t root = 0; root < l->out->count; root++) {
        if (l->items[root].state != 0)
            continue;
        uint32_t depth = 1;
        stack[0] = (struct frame){root + 1, 0};
        l->items[root].state = 1;
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
            if (target == 0 && l->upgrade && strncmp(name, "UpgradeOld.", 11) == 0) {
                struct mo_buffer text;
                mo_buffer_init(&text, MO_LIMIT_FILE_SIZE);
                bool ok = mo_buffer_append_text(&text, "module ") &&
                          mo_buffer_append_text(&text, name) && mo_buffer_append_byte(&text, '\n');
                uint32_t file;
                struct mo_source_error error;
                ok = ok &&
                     mo_sources_add_bytes(l->sources, name, text.bytes, text.length, &file, &error);
                if (ok)
                    target = index_module(l, file, name, path->span, false);
                mo_buffer_free(&text);
                if (!ok || target == 0)
                    return false;
            }
            if (target == 0 && l->real_library != NULL && is_reserved_on_use(name)) {
                char *full = library_file(l, path);
                if (full == NULL)
                    return false;
                target = read_module(l, full, name, path->span, true);
                if (target == 0)
                    return false;
            } else if (target == 0 && l->embedded != NULL && is_reserved_on_use(name)) {
                const struct mo_embedded_source *src = find_embedded_on_use(l, path);
                if (src != NULL) {
                    target = read_embedded_module(l, src, name, path->span);
                    if (target == 0)
                        return false;
                }
            }
            if (l->memory_only &&
                (target == 0 || (l->out->library[frame->id - 1] && !l->out->library[target - 1]))) {
                bool kept = source_error(
                    l, (struct mo_source_error){.problem = MO_SOURCE_UNREADABLE}, name, path->span);
                (void)kept;
                return false;
            }
            if (l->out->library[frame->id - 1] && target == 0 && loaded_on_use(name)) {
                if (l->real_library != NULL) {
                    char *full = library_file(l, path);
                    if (full == NULL)
                        return false;
                    target = read_module(l, full, name, path->span, true);
                    if (target == 0)
                        return false;
                } else if (l->embedded != NULL) {
                    const struct mo_embedded_source *src = find_embedded_on_use(l, path);
                    if (src != NULL) {
                        target = read_embedded_module(l, src, name, path->span);
                        if (target == 0)
                            return false;
                    }
                }
            }
            if (l->out->library[frame->id - 1] && (target == 0 || !l->out->library[target - 1])) {
                char *full = library_file(l, path);
                if (full == NULL)
                    return false;
                bool kept = report(l, MO_CODE_MODULE_MISSING, path->span,
                                   "a library module's file can't be found",
                                   "Every module is found under its root folder (reference 2.1); "
                                   "the toolchain library has its own root.",
                                   full);
                (void)kept;
                return false;
            }
            if (target == 0) {
                char *relative = path_text(l, path, true);
                if (relative == NULL)
                    return false;
                /* 1. Local module root of the importing package (entry project or current dep). */
                if (module->pkg == 0) {
                    char *local_cand = join(l, l->out->root, relative, path->span);
                    if (local_cand == NULL)
                        return false;
                    struct stat lst;
                    if (lstat(local_cand, &lst) == 0) {
                        target = read_module_in_root(l, local_cand, l->real_root, name, path->span,
                                                     false, 0);
                        if (target == 0)
                            return false;
                    }
                } else if (module->pkg <= l->dep_count) {
                    const struct dep_package *own_dep = &l->deps[module->pkg - 1];
                    for (uint32_t r = 0; r < own_dep->root_count; r++) {
                        char *cand = join(l, own_dep->root_slash[r], relative, path->span);
                        if (cand == NULL)
                            return false;
                        struct stat dst;
                        if (lstat(cand, &dst) == 0) {
                            target = read_module_in_root(l, cand, own_dep->real_roots[r], name,
                                                         path->span, false, module->pkg);
                            if (target == 0)
                                return false;
                            break;
                        }
                    }
                }

                /* 2. Declared [dependencies] from mo.toml (rejecting ambiguous hits). */
                if (target == 0) {
                    const uint32_t *direct =
                        module->pkg == 0 ? l->entry_direct : l->deps[module->pkg - 1].direct;
                    uint32_t direct_count = module->pkg == 0
                                                ? l->entry_direct_count
                                                : l->deps[module->pkg - 1].direct_count;
                    uint32_t matched_dep = 0;
                    const char *matched_cand = NULL;
                    const char *matched_real_root = NULL;
                    for (uint32_t d = 0; d < direct_count; d++) {
                        uint32_t dep_id = direct[d];
                        const struct dep_package *dep = &l->deps[dep_id - 1];
                        for (uint32_t r = 0; r < dep->root_count; r++) {
                            char *cand = join(l, dep->root_slash[r], relative, path->span);
                            if (cand == NULL)
                                return false;
                            struct stat dst;
                            if (lstat(cand, &dst) == 0) {
                                if (matched_dep != 0 && matched_dep != dep_id) {
                                    bool kept = report(
                                        l, MO_CODE_PACKAGE_MANIFEST, path->span,
                                        "a module is provided by multiple dependencies in mo.toml",
                                        "Keep module namespaces distinct across [dependencies] in "
                                        "mo.toml.",
                                        name);
                                    (void)kept;
                                    return false;
                                }
                                matched_dep = dep_id;
                                matched_cand = cand;
                                matched_real_root = dep->real_roots[r];
                                break;
                            }
                        }
                    }
                    if (matched_dep != 0) {
                        target = read_module_in_root(l, matched_cand, matched_real_root, name,
                                                     path->span, false, matched_dep);
                        if (target == 0)
                            return false;
                    }
                }

                /* 3. Mo's built-in lib/ (disk or embedded LIBRARY_SOURCES). */
                if (target == 0 && loaded_on_use(name)) {
                    if (l->real_library != NULL) {
                        char *full = library_file(l, path);
                        if (full == NULL)
                            return false;
                        target = read_module(l, full, name, path->span, true);
                        if (target == 0)
                            return false;
                    } else if (l->embedded != NULL) {
                        const struct mo_embedded_source *src = find_embedded_on_use(l, path);
                        if (src != NULL) {
                            target = read_embedded_module(l, src, name, path->span);
                            if (target == 0)
                                return false;
                        }
                    }
                }

                /* 4. Standard missing-module diagnostic under the importing module's root. */
                if (target == 0) {
                    const char *miss_slash =
                        module->pkg == 0 ? l->out->root : l->deps[module->pkg - 1].root_slash[0];
                    const char *miss_real =
                        module->pkg == 0 ? l->real_root : l->deps[module->pkg - 1].real_roots[0];
                    char *full = join(l, miss_slash, relative, path->span);
                    if (full == NULL)
                        return false;
                    target = read_module_in_root(l, full, miss_real, name, path->span, false,
                                                 module->pkg);
                    if (target == 0)
                        return false;
                }
            }
            l->out->uses[frame->id - 1][frame->use - 1] = target - 1;
            if (l->items[target - 1].state == 1) {
                uint32_t start = 0;
                while (stack[start].id != target) {
                    start++;
                    MO_ASSERT(start < depth);
                }
                uint32_t count = depth - start - 1;
                struct mo_label *edges =
                    allocate(l, count, sizeof(*edges), _Alignof(struct mo_label));
                if (edges == NULL)
                    return false;
                for (uint32_t i = 0; i < count; i++) {
                    const struct frame *from = &stack[start + i];
                    const struct mo_syntax *edge =
                        l->items[from->id - 1].header.uses->children[from->use - 1];
                    edges[i] = (struct mo_label){edge->children[0]->span,
                                                 l->items[stack[start + i + 1].id - 1].name};
                }
                bool kept = mo_diags_add(
                    l->diags,
                    &(struct mo_diag){
                        .code = MO_CODE_MODULE_CYCLE,
                        .primary = {path->span, name},
                        .message = "modules use each other in a cycle",
                        .reason =
                            "A cycle of modules that use each other is an error (reference 10.3).",
                        .secondary = edges,
                        .secondary_count = count});
                (void)kept;
                return false;
            }
            if (l->items[target - 1].state == 0) {
                MO_ASSERT(depth < MO_LIMIT_FILES);
                l->items[target - 1].state = 1;
                stack[depth++] = (struct frame){target, 0};
            }
        }
    }
    MO_ASSERT(ordered == l->out->count);
    return true;
}

static MO_MUST_USE bool prepare(struct loader *l) {
    l->out = allocate(l, 1, sizeof(*l->out), _Alignof(struct mo_module_set));
    if (l->out == NULL)
        return false;
    *l->out = (struct mo_module_set){.sources = l->sources};
    l->items = allocate(l, MO_LIMIT_FILES, sizeof(*l->items), _Alignof(struct loaded));
    l->out->modules =
        allocate(l, MO_LIMIT_FILES, sizeof(*l->out->modules), _Alignof(struct mo_module_syntax *));
    l->out->order = allocate(l, MO_LIMIT_FILES, sizeof(*l->out->order), _Alignof(uint32_t));
    l->out->names = allocate(l, MO_LIMIT_FILES, sizeof(*l->out->names), _Alignof(const char *));
    l->out->library = allocate(l, MO_LIMIT_FILES, sizeof(*l->out->library), _Alignof(bool));
    l->out->dependency = allocate(l, MO_LIMIT_FILES, sizeof(*l->out->dependency), _Alignof(bool));
    l->out->uses = allocate(l, MO_LIMIT_FILES, sizeof(*l->out->uses), _Alignof(uint32_t *));
    l->out->manifests =
        allocate(l, MO_LIMIT_DEPENDENCIES + 1, sizeof(*l->out->manifests), _Alignof(const char *));
    return !l->failed;
}

static MO_MUST_USE bool load_named_library(struct loader *l, const char *name, const char *file) {
    if (find(l, name) != 0)
        return true;
    struct mo_span unavailable = {UINT32_MAX, 0, 0};
    if (l->real_library != NULL) {
        char *prefix = join(l, l->real_library, "/", unavailable);
        char *full = prefix == NULL ? NULL : join(l, prefix, file, unavailable);
        return full != NULL && read_module(l, full, name, unavailable, true) != 0;
    }
    if (l->embedded != NULL) {
        for (uint32_t i = 0; i < l->embedded->count; i++)
            if (strcmp(l->embedded->files[i].path, file) == 0)
                return read_embedded_module(l, &l->embedded->files[i], name, unavailable) != 0;
    }
    if (!l->memory_only) {
        char *full = join(l, "lib/", file, unavailable);
        return full != NULL && read_module(l, full, name, unavailable, true) != 0;
    }
    return true;
}

static bool syntax_mentions_sqlite(const struct mo_syntax *node) {
    if (node == NULL)
        return false;
    if (node->kind == MO_S_CAPNAME && node->text != NULL) {
        size_t len = node->span.end - node->span.start;
        if ((len == 6 && memcmp(node->text, "Sqlite", 6) == 0) ||
            (len == 17 && memcmp(node->text, "SqliteDecodeError", 17) == 0) ||
            (len == 13 && memcmp(node->text, "MissingColumn", 13) == 0) ||
            (len == 12 && memcmp(node->text, "TypeMismatch", 12) == 0) ||
            (len == 12 && memcmp(node->text, "InvalidValue", 12) == 0) ||
            (len == 14 && memcmp(node->text, "ExpectedOneRow", 14) == 0))
            return true;
    }
    for (uint32_t i = 0; i < node->count; i++)
        if (syntax_mentions_sqlite(node->children[i]))
            return true;
    return false;
}

static MO_MUST_USE struct mo_module_set *parse_modules(struct loader *l) {
    if (!graph(l))
        return NULL;
    uint32_t before = l->diags->count;
    bool has_api = false;
    bool has_sqlite = false;
    for (uint32_t i = 0; i < l->out->count; i++) {
        l->out->modules[i] = mo_parse(l->arena, l->sources, l->items[i].file, l->diags);
        if (l->out->modules[i] == NULL)
            return NULL;
        const struct mo_syntax *items = l->out->modules[i]->tree->children[1];
        for (uint32_t j = 0; j < items->count; j++)
            if (items->children[j]->kind == MO_S_API)
                has_api = true;
        if (strcmp(l->items[i].name, "Sqlite") != 0 &&
            syntax_mentions_sqlite(l->out->modules[i]->tree))
            has_sqlite = true;
    }
    bool need_api = has_api && (find(l, "Api") == 0 || find(l, "Json") == 0);
    bool need_sqlite = (has_sqlite || has_api) &&
                       (find(l, "Sqlite") == 0 || find(l, "Json") == 0 || find(l, "Date") == 0);
    if (need_api || need_sqlite) {
        uint32_t prev_count = l->out->count;
        if (need_api &&
            (!load_named_library(l, "Json", "json.mo") || !load_named_library(l, "Api", "api.mo")))
            return NULL;
        if (need_sqlite && (!load_named_library(l, "Json", "json.mo") ||
                            !load_named_library(l, "Date", "date.mo") ||
                            !load_named_library(l, "Sqlite", "sqlite.mo")))
            return NULL;
        if (l->out->count > prev_count) {
            for (uint32_t i = prev_count; i < l->out->count; i++) {
                l->out->modules[i] = mo_parse(l->arena, l->sources, l->items[i].file, l->diags);
                if (l->out->modules[i] == NULL)
                    return NULL;
            }
            for (uint32_t i = 0; i < l->out->count; i++)
                l->items[i].state = 0;
            if (!graph(l))
                return NULL;
        }
    }
    mo_diags_sort(l->diags);
    return l->diags->count == before && l->diags->dropped == 0 ? l->out : NULL;
}

static MO_MUST_USE struct mo_module_set *load(struct mo_arena *arena, struct mo_sources *sources,
                                              const char *entry, const char *library,
                                              const struct mo_embedded_library *embedded,
                                              const char *library_entry_root,
                                              struct mo_diags *diags, bool upgrade) {
    MO_ASSERT(arena != NULL && sources != NULL && entry != NULL && diags != NULL);
    MO_ASSERT(sources->count == 0);
    struct loader l = {.arena = arena,
                       .sources = sources,
                       .diags = diags,
                       .embedded = library == NULL ? embedded : NULL};
    if (!prepare(&l))
        return NULL;
    entry = normal_path(&l, entry);
    if (entry == NULL)
        return NULL;
    if (library != NULL) {
        library = normal_path(&l, library);
        if (library == NULL)
            return NULL;
        l.real_library = real_path(&l, library, (struct mo_span){UINT32_MAX, 0, 0}, true);
        if (l.real_library == NULL)
            return NULL;
        /* An entry inside the library folder is a library module itself, so that the toolchain
         * can check and test its own library, and it isn't read a second time below. */
        char resolved[MO_LIMIT_PATH_SIZE + 1];
        size_t n = strlen(l.real_library);
        if (realpath(entry, resolved) != NULL && strncmp(l.real_library, resolved, n) == 0 &&
            (n == 1 || resolved[n] == '/')) {
            l.real_entry = real_path(&l, entry, (struct mo_span){UINT32_MAX, 0, 0}, false);
            if (l.real_entry == NULL)
                return NULL;
        }
    } else if (embedded != NULL && library_entry_root != NULL) {
        char lib_resolved[MO_LIMIT_PATH_SIZE + 1], entry_resolved[MO_LIMIT_PATH_SIZE + 1];
        if (realpath(library_entry_root, lib_resolved) != NULL &&
            realpath(entry, entry_resolved) != NULL) {
            size_t n = strlen(lib_resolved);
            if (strncmp(lib_resolved, entry_resolved, n) == 0 &&
                (n == 1 || entry_resolved[n] == '/')) {
                l.real_entry = real_path(&l, entry, (struct mo_span){UINT32_MAX, 0, 0}, false);
                if (l.real_entry == NULL)
                    return NULL;
                const char *rel = l.real_entry + n;
                if (*rel == '/')
                    rel++;
                l.entry_library_rel = rel;
            }
        }
    }
    if (read_module(&l, entry, NULL, (struct mo_span){UINT32_MAX, 0, 0}, l.real_entry != NULL) == 0)
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
    l.real_root = real_path(&l, root, l.items[0].header.path->span, false);
    if (l.real_root == NULL)
        return NULL;
    l.upgrade = upgrade;
    if (!discover_and_load_manifest(&l))
        return NULL;
    if (upgrade) {
        static const uint8_t placeholder[] = "module UpgradeOld\n";
        uint32_t file;
        struct mo_source_error error;
        if (!mo_sources_add_bytes(sources, "<UpgradeOld>", placeholder, sizeof placeholder - 1,
                                  &file, &error) ||
            index_module(&l, file, "UpgradeOld", (struct mo_span){UINT32_MAX, 0, 0}, false) == 0)
            return NULL;
    }
    if (library != NULL && !library_files(&l, library))
        return NULL;
    if (library == NULL && embedded != NULL && !embedded_library_files(&l, embedded))
        return NULL;
    return parse_modules(&l);
}

struct mo_module_set *mo_load_program(struct mo_arena *arena, struct mo_sources *sources,
                                      const char *entry, const char *library,
                                      struct mo_diags *diags) {
    return mo_load_program_with_library(arena, sources, entry, library, NULL, NULL, diags);
}

struct mo_module_set *
mo_load_program_with_library(struct mo_arena *arena, struct mo_sources *sources, const char *entry,
                             const char *library, const struct mo_embedded_library *embedded,
                             const char *library_entry_root, struct mo_diags *diags) {
    struct mo_module_set *set =
        load(arena, sources, entry, library, embedded, library_entry_root, diags, false);
    mo_diags_sort(diags);
    return set;
}

struct mo_module_set *mo_load_program_sources(struct mo_arena *arena, struct mo_sources *sources,
                                              const bool *library, struct mo_diags *diags) {
    MO_ASSERT(arena != NULL && sources != NULL && diags != NULL);
    MO_ASSERT(sources->count != 0 && sources->count <= MO_LIMIT_FILES);
    struct loader l = {.arena = arena, .sources = sources, .diags = diags, .memory_only = true};
    struct mo_module_set *set = NULL;
    if (!prepare(&l))
        goto done;
    l.out->root = "";
    for (uint32_t i = 0; i < sources->count; i++) {
        if (index_module(&l, i, NULL, (struct mo_span){UINT32_MAX, 0, 0},
                         library != NULL && library[i]) == 0)
            goto done;
    }
    set = parse_modules(&l);
done:
    mo_diags_sort(diags);
    return set;
}

struct mo_module_set *mo_load_upgrade_sources(struct mo_arena *arena, struct mo_sources *sources,
                                              const char *entry, const char *library,
                                              struct mo_diags *diags) {
    return mo_load_upgrade_with_library(arena, sources, entry, library, NULL, NULL, diags);
}

struct mo_module_set *
mo_load_upgrade_with_library(struct mo_arena *arena, struct mo_sources *sources, const char *entry,
                             const char *library, const struct mo_embedded_library *embedded,
                             const char *library_entry_root, struct mo_diags *diags) {
    struct mo_module_set *set =
        load(arena, sources, entry, library, embedded, library_entry_root, diags, true);
    mo_diags_sort(diags);
    return set;
}

struct mo_module_set *mo_load_bundle_sources(struct mo_arena *arena, struct mo_sources *sources,
                                             const char *library, struct mo_diags *diags) {
    return mo_load_bundle_with_library(arena, sources, library, NULL, diags);
}

struct mo_module_set *mo_load_bundle_with_library(struct mo_arena *arena,
                                                  struct mo_sources *sources, const char *library,
                                                  const struct mo_embedded_library *embedded,
                                                  struct mo_diags *diags) {
    MO_ASSERT(sources->count != 0);
    struct loader l = {.arena = arena,
                       .sources = sources,
                       .diags = diags,
                       .embedded = library == NULL ? embedded : NULL};
    if (!prepare(&l))
        return NULL;
    l.out->root = "";
    uint32_t count = sources->count;
    for (uint32_t i = 0; i < count; i++)
        if (index_module(&l, i, NULL, (struct mo_span){UINT32_MAX, 0, 0}, false) == 0)
            return NULL;
    if (library != NULL) {
        l.real_library = real_path(&l, library, (struct mo_span){UINT32_MAX, 0, 0}, true);
        if (l.real_library == NULL || !library_files(&l, library))
            return NULL;
    } else if (embedded != NULL && !embedded_library_files(&l, embedded)) {
        return NULL;
    }
    l.memory_only = true;
    struct mo_module_set *set = parse_modules(&l);
    mo_diags_sort(diags);
    return set;
}
