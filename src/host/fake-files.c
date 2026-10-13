#include "host/files.h"

#include <errno.h>
#include <string.h>

static MO_MUST_USE bool contains(struct mo_host *host, struct mo_heap *heap, uint32_t type,
                                 struct mo_cell table, const char *path, size_t length, bool *found,
                                 struct mo_cell *value) {
    struct mo_cell key;
    if (!mo_value_text(heap, path, length, &key))
        return false;
    bool ok = mo_table_get(heap, type, table, key, found, value);
    mo_value_drop(heap, host->types->text, key);
    return ok;
}

static MO_MUST_USE bool folder(struct mo_host *host, struct mo_heap *heap, struct mo_cell folders,
                               const char *path, size_t length, bool *found) {
    if (length == 0) {
        *found = true;
        return true;
    }
    struct mo_cell unused;
    return contains(host, heap, host->types->folders, folders, path, length, found, &unused);
}

static MO_MUST_USE bool add_prefixes(struct mo_host *host, struct mo_heap *heap,
                                     struct mo_cell *folders, const uint8_t *bytes, size_t length,
                                     bool last) {
    for (size_t i = 1; i <= length; i++) {
        if ((i == length && !last) || (i < length && bytes[i] != '/'))
            continue;
        struct mo_cell prefix;
        if (!mo_value_text(heap, (const char *)bytes, i, &prefix))
            return false;
        bool ok = mo_table_set(heap, host->types->folders, folders, prefix, mo_cell_unit());
        mo_value_drop(heap, host->types->text, prefix);
        if (!ok)
            return false;
    }
    return true;
}

/* Constructor keys are stored verbatim, not normalized like operation paths. */
static MO_MUST_USE bool plain_relative(const uint8_t *bytes, size_t length) {
    size_t start = 0;
    for (size_t end = 0; end <= length; end++) {
        if (end < length && bytes[end] != '/')
            continue;
        size_t n = end - start;
        if (n == 0 || (bytes[start] == '.' && (n == 1 || (n == 2 && bytes[start + 1] == '.'))))
            return false;
        start = end + 1;
    }
    return true;
}

enum mo_host_outcome mo_host_files_fake(struct mo_host *host, struct mo_heap *heap, uint32_t type,
                                        struct mo_cell files, struct mo_cell *out,
                                        struct mo_host_error *error) {
    uint32_t position = 0;
    struct mo_cell key, value;
    while (mo_table_next(files, &position, &key, &value)) {
        size_t length;
        const uint8_t *bytes = mo_value_data(key, &length);
        if (!plain_relative(bytes, length)) {
            *error =
                (struct mo_host_error){.reason = "fake has a key that is not a plain relative path",
                                       .has_detail = true,
                                       .detail = key};
            return MO_HOST_BUG;
        }
    }
    struct mo_cell folders;
    if (!mo_value_set(heap, host->types->folders, NULL, 0, &folders))
        return MO_HOST_OUT_OF_MEMORY;
    position = 0;
    while (mo_table_next(files, &position, &key, &value)) {
        size_t length;
        const uint8_t *bytes = mo_value_data(key, &length);
        if (!add_prefixes(host, heap, &folders, bytes, length, false)) {
            mo_value_drop(heap, host->types->folders, folders);
            return MO_HOST_OUT_OF_MEMORY;
        }
    }
    position = 0;
    while (mo_table_next(files, &position, &key, &value)) {
        bool found;
        if (!mo_table_get(heap, host->types->folders, folders, key, &found, &value)) {
            mo_value_drop(heap, host->types->folders, folders);
            return MO_HOST_OUT_OF_MEMORY;
        }
        if (found) {
            mo_value_drop(heap, host->types->folders, folders);
            *error = (struct mo_host_error){.reason = "fake has a file that is also a folder",
                                            .has_detail = true,
                                            .detail = key};
            return MO_HOST_BUG;
        }
    }
    struct mo_cell root;
    if (!mo_value_text(heap, "", 0, &root)) {
        mo_value_drop(heap, host->types->folders, folders);
        return MO_HOST_OUT_OF_MEMORY;
    }
    uint32_t types[] = {host->types->files, host->types->folders};
    struct mo_cell values[] = {files, folders};
    struct mo_state *state;
    bool ok = mo_state_new(heap, types, values, 2, &state);
    if (ok) {
        struct mo_capability_info info = {.operations = &mo_host_operations[true][MO_CAP_FILES],
                                          .state = state,
                                          .has_prefix = true,
                                          .prefix = root};
        ok = mo_value_capability(heap, type, &info, out);
        mo_state_drop(heap, state);
    }
    mo_value_drop(heap, host->types->text, root);
    mo_value_drop(heap, host->types->folders, folders);
    return ok ? MO_HOST_RETURNED : MO_HOST_OUT_OF_MEMORY;
}

/* Scan stored key bytes once instead of allocating and hashing every path
 * prefix. Deep paths cannot turn one lookup into quadratic prefix work. */
static MO_MUST_USE bool blocked(const struct mo_capability_info *info, const char *path,
                                size_t length) {
    size_t root_length;
    const uint8_t *root = mo_value_data(info->prefix, &root_length);
    uint32_t position = 0;
    struct mo_cell key, unused;
    while (mo_table_next(mo_state_get(info->state, 0), &position, &key, &unused)) {
        size_t n;
        const uint8_t *bytes = mo_value_data(key, &n);
        if ((n == root_length && memcmp(bytes, root, n) == 0) ||
            (n < length && path[n] == '/' && memcmp(bytes, path, n) == 0))
            return true;
    }
    return false;
}

static MO_MUST_USE enum mo_host_outcome derived(struct mo_host *host, struct mo_heap *heap,
                                                uint32_t type,
                                                const struct mo_capability_info *info,
                                                const char *path, size_t length, bool valid,
                                                struct mo_cell *out) {
    struct mo_capability_info copy = *info;
    enum mo_capability cap = host->types->types.items[type].capability;
    copy.operations = &mo_host_operations[true][cap];
    copy.read_only = cap == MO_CAP_READ_FILES;
    copy.has_prefix = valid;
    if (valid && !mo_value_text(heap, path, length, &copy.prefix))
        return MO_HOST_OUT_OF_MEMORY;
    bool ok = mo_value_capability(heap, type, &copy, out);
    if (valid)
        mo_value_drop(heap, host->types->text, copy.prefix);
    return ok ? MO_HOST_RETURNED : MO_HOST_OUT_OF_MEMORY;
}

static MO_MUST_USE enum mo_host_outcome listing(struct mo_host *host, struct mo_heap *heap,
                                                uint32_t type,
                                                const struct mo_capability_info *info,
                                                const char *path, size_t n, struct mo_cell *out) {
    struct mo_cell unique;
    if (!mo_value_set(heap, host->types->folders, NULL, 0, &unique))
        return MO_HOST_OUT_OF_MEMORY;
    bool ok = true;
    for (uint32_t slot = 0; slot < 2 && ok; slot++) {
        uint32_t position = 0;
        struct mo_cell key, unused;
        while (mo_table_next(mo_state_get(info->state, slot), &position, &key, &unused)) {
            size_t length;
            const uint8_t *bytes = mo_value_data(key, &length);
            if (length <= n || (n != 0 && (memcmp(bytes, path, n) != 0 || bytes[n] != '/')))
                continue;
            size_t start = n == 0 ? 0 : n + 1, end = start;
            while (end < length && bytes[end] != '/')
                end++;
            struct mo_cell name;
            if (!mo_value_text(heap, (const char *)bytes + start, end - start, &name)) {
                ok = false;
                break;
            }
            ok = mo_table_set(heap, host->types->folders, &unique, name, mo_cell_unit());
            mo_value_drop(heap, host->types->text, name);
            if (!ok)
                break;
        }
    }
    struct mo_cell names;
    bool have_names = ok && mo_value_list(heap, host->types->texts, NULL, 0, &names);
    ok = have_names;
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, heap);
    uint32_t count = mo_table_size(unique);
    uint8_t *kinds = count == 0 ? NULL : mo_scratch_resize(&scratch, NULL, 0, count);
    if (count != 0 && kinds == NULL)
        ok = false;
    uint32_t position = 0, index = 0;
    struct mo_cell key, unused;
    while (ok && mo_table_next(unique, &position, &key, &unused)) {
        ok = mo_list_append(heap, host->types->texts, &names, key);
        if (!ok)
            break;
        size_t length;
        const uint8_t *bytes = mo_value_data(key, &length);
        struct mo_cell full;
        struct mo_scratch path_scratch;
        mo_scratch_init(&path_scratch, heap);
        size_t pre = n == 0 ? 0 : n + 1;
        char *full_bytes = mo_scratch_resize(&path_scratch, NULL, 0, pre + length);
        if (pre + length != 0 && full_bytes == NULL)
            ok = false;
        if (ok) {
            if (n != 0) {
                memcpy(full_bytes, path, n);
                full_bytes[n] = '/';
            }
            if (length != 0)
                memcpy(full_bytes + pre, bytes, length);
            ok = mo_value_text(heap, full_bytes, pre + length, &full);
            if (ok) {
                bool is_folder;
                ok = mo_table_get(heap, host->types->folders, mo_state_get(info->state, 1), full,
                                  &is_folder, &unused);
                if (ok)
                    kinds[index++] = is_folder ? 1 : 0;
                mo_value_drop(heap, host->types->text, full);
            }
        }
        mo_scratch_free(&path_scratch);
    }
    uint32_t list_type =
        host->types->types.items[type].cases[mo_host_tag(host, type, "Ok")].fields[0].type;
    struct mo_cell sorted;
    if (ok)
        ok = mo_host_names(host, heap, list_type, names, kinds, &sorted);
    enum mo_host_outcome result = MO_HOST_OUT_OF_MEMORY;
    if (ok) {
        result = mo_host_file_ok(host, heap, type, sorted, out);
        mo_value_drop(heap, list_type, sorted);
    }
    if (have_names)
        mo_value_drop(heap, host->types->texts, names);
    mo_value_drop(heap, host->types->folders, unique);
    mo_scratch_free(&scratch);
    return result;
}

static MO_MUST_USE enum mo_host_outcome call(struct mo_host *host, struct mo_heap *heap,
                                             uint32_t operation, uint32_t type,
                                             const struct mo_cell *args, struct mo_cell *out,
                                             struct mo_host_error *error, char *normalized,
                                             char *path) {
    (void)error;
    const struct mo_capability_info *info = mo_capability_get(args[0]);
    if (operation == MO_HOST_FILES_READ_ONLY) {
        struct mo_capability_info copy = *info;
        copy.operations = &mo_host_operations[true][MO_CAP_READ_FILES];
        copy.read_only = true;
        return mo_value_capability(heap, type, &copy, out) ? MO_HOST_RETURNED
                                                           : MO_HOST_OUT_OF_MEMORY;
    }
    size_t length = 0;
    struct mo_file_error problem = mo_host_normalize(args[1], normalized, &length);
    if (!info->has_prefix)
        problem = (struct mo_file_error){.status = FILE_NOT_ALLOWED};
    if (problem.status == FILE_OK) {
        length = mo_host_join(info->prefix, normalized, length, path);
        if (length > UINT32_MAX) {
            errno = ENOSPC;
            return MO_HOST_OUT_OF_MEMORY;
        }
    }
    bool within = operation == MO_HOST_FILES_WITHIN || operation == MO_HOST_READFILES_WITHIN;
    if (within)
        return derived(host, heap, type, info, path, length, problem.status == FILE_OK, out);
    bool exists = operation == MO_HOST_FILES_EXISTS || operation == MO_HOST_READFILES_EXISTS;
    if (problem.status != FILE_OK) {
        if (exists) {
            *out = mo_cell_bool(false);
            return MO_HOST_RETURNED;
        }
        return mo_host_file_error(host, heap, type, args[1], problem, out);
    }
    bool block = blocked(info, path, length), root_folder, is_folder, is_file;
    struct mo_cell folders = mo_state_get(info->state, 1), files = mo_state_get(info->state, 0),
                   value;
    size_t root_length;
    const uint8_t *root = mo_value_data(info->prefix, &root_length);
    (void)root;
    root_folder = root_length == 0;
    struct mo_cell unused;
    if ((!root_folder &&
         !mo_table_get(heap, host->types->folders, folders, info->prefix, &root_folder, &unused)) ||
        !folder(host, heap, folders, path, length, &is_folder) ||
        !contains(host, heap, host->types->files, files, path, length, &is_file, &value))
        return MO_HOST_OUT_OF_MEMORY;
    if (exists) {
        *out = mo_cell_bool(!block && root_folder && (is_folder || is_file));
        return MO_HOST_RETURNED;
    }
    bool directory = operation == MO_HOST_FILES_MAKE_FOLDER || operation == MO_HOST_FILES_LIST ||
                     operation == MO_HOST_READFILES_LIST || operation == MO_HOST_FILES_ENTRIES ||
                     operation == MO_HOST_READFILES_ENTRIES;
    if (block || (directory && is_file))
        problem = (struct mo_file_error){.status = FILE_FAILED, .reason = "not a folder"};
    else if (!root_folder)
        problem.status = FILE_NOT_FOUND;
    else if (!directory && is_folder)
        problem = (struct mo_file_error){.status = FILE_FAILED, .reason = "is a folder"};
    else if (directory && operation != MO_HOST_FILES_MAKE_FOLDER && !is_folder)
        problem.status = FILE_NOT_FOUND;
    if (problem.status != FILE_OK)
        return mo_host_file_error(host, heap, type, args[1], problem, out);
    if (operation == MO_HOST_FILES_LIST || operation == MO_HOST_READFILES_LIST ||
        operation == MO_HOST_FILES_ENTRIES || operation == MO_HOST_READFILES_ENTRIES)
        return listing(host, heap, type, info, path, length, out);
    if (operation == MO_HOST_FILES_MAKE_FOLDER) {
        struct mo_cell result;
        enum mo_host_outcome outcome = mo_host_file_ok(host, heap, type, mo_cell_unit(), &result);
        if (outcome != MO_HOST_RETURNED)
            return outcome;
        struct mo_cell staged = mo_value_dup(heap, host->types->folders, folders);
        bool ok = add_prefixes(host, heap, &staged, (const uint8_t *)path, length, true);
        if (ok)
            mo_state_set(heap, info->state, 1, staged);
        mo_value_drop(heap, host->types->folders, staged);
        if (!ok) {
            mo_value_drop(heap, type, result);
            return MO_HOST_OUT_OF_MEMORY;
        }
        *out = result;
        return MO_HOST_RETURNED;
    }
    if (operation == MO_HOST_FILES_WRITE_TEXT || operation == MO_HOST_FILES_APPEND_TEXT) {
        size_t parent_length = length;
        while (parent_length != 0 && path[parent_length - 1] != '/')
            parent_length--;
        if (parent_length != 0)
            parent_length--;
        bool parent_exists;
        bool parent_ok = folder(host, heap, folders, path, parent_length, &parent_exists);
        if (!parent_ok)
            return MO_HOST_OUT_OF_MEMORY;
        if (!parent_exists)
            return mo_host_file_error(host, heap, type, args[1],
                                      (struct mo_file_error){.status = FILE_NOT_FOUND}, out);
        struct mo_cell key;
        if (!mo_value_text(heap, path, length, &key))
            return MO_HOST_OUT_OF_MEMORY;
        struct mo_cell text = args[2];
        bool appended = operation == MO_HOST_FILES_APPEND_TEXT && is_file;
        if (appended) {
            size_t a, b;
            const uint8_t *pa = mo_value_data(value, &a), *pb = mo_value_data(text, &b);
            struct mo_scratch scratch;
            mo_scratch_init(&scratch, heap);
            if (b > UINT32_MAX - a) {
                mo_value_drop(heap, host->types->text, key);
                errno = ENOSPC;
                return MO_HOST_OUT_OF_MEMORY;
            }
            uint8_t *bytes = a + b == 0 ? NULL : mo_scratch_resize(&scratch, NULL, 0, a + b);
            bool ok = a + b == 0 || bytes != NULL;
            if (ok) {
                if (a != 0)
                    memcpy(bytes, pa, a);
                if (b != 0)
                    memcpy(bytes + a, pb, b);
                ok = mo_value_text(heap, (const char *)bytes, a + b, &text);
            }
            mo_scratch_free(&scratch);
            if (!ok) {
                mo_value_drop(heap, host->types->text, key);
                return MO_HOST_OUT_OF_MEMORY;
            }
        }
        struct mo_cell result;
        enum mo_host_outcome outcome = mo_host_file_ok(host, heap, type, mo_cell_unit(), &result);
        if (outcome == MO_HOST_RETURNED) {
            if (!mo_table_set(heap, host->types->files, mo_state_slot(info->state, 0), key, text)) {
                mo_value_drop(heap, type, result);
                outcome = MO_HOST_OUT_OF_MEMORY;
            } else
                *out = result;
        }
        if (appended)
            mo_value_drop(heap, host->types->text, text);
        mo_value_drop(heap, host->types->text, key);
        return outcome;
    }
    if (!is_file)
        return mo_host_file_error(host, heap, type, args[1],
                                  (struct mo_file_error){.status = FILE_NOT_FOUND}, out);
    if (operation == MO_HOST_FILES_REMOVE) {
        struct mo_cell key;
        if (!mo_value_text(heap, path, length, &key))
            return MO_HOST_OUT_OF_MEMORY;
        bool ok = mo_table_remove(heap, host->types->files, mo_state_slot(info->state, 0), key);
        mo_value_drop(heap, host->types->text, key);
        return ok ? mo_host_file_ok(host, heap, type, mo_cell_unit(), out) : MO_HOST_OUT_OF_MEMORY;
    }
    if (operation == MO_HOST_FILES_READ_BYTES || operation == MO_HOST_READFILES_READ_BYTES) {
        size_t data_length;
        const uint8_t *bytes = mo_value_data(value, &data_length);
        struct mo_cell data;
        if (!mo_value_bytes(heap, bytes, data_length, &data))
            return MO_HOST_OUT_OF_MEMORY;
        enum mo_host_outcome outcome = mo_host_file_ok(host, heap, type, data, out);
        uint32_t payload =
            host->types->types.items[type].cases[mo_host_tag(host, type, "Ok")].fields[0].type;
        mo_value_drop(heap, payload, data);
        return outcome;
    }
    return mo_host_file_ok(host, heap, type, value, out);
}

enum mo_host_outcome mo_host_fake_files_call(struct mo_host *host, struct mo_heap *heap,
                                             uint32_t operation, uint32_t type,
                                             const struct mo_cell *args, struct mo_cell *out,
                                             struct mo_host_error *error) {
    if (operation == MO_HOST_FILES_READ_ONLY)
        return call(host, heap, operation, type, args, out, error, NULL, NULL);
    const struct mo_capability_info *info = mo_capability_get(args[0]);
    size_t length, root_length = 0;
    const uint8_t *bytes = mo_value_data(args[1], &length);
    (void)bytes;
    if (info->has_prefix) {
        const uint8_t *root = mo_value_data(info->prefix, &root_length);
        (void)root;
    }
    MO_ASSERT(length <= UINT32_MAX && root_length <= UINT32_MAX);
    if (length > (SIZE_MAX - 3 - root_length) / 2) {
        errno = ENOSPC;
        return MO_HOST_OUT_OF_MEMORY;
    }
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, heap);
    char *storage = mo_scratch_resize(&scratch, NULL, 0, 2 * length + root_length + 3);
    if (storage == NULL) {
        mo_scratch_free(&scratch);
        return MO_HOST_OUT_OF_MEMORY;
    }
    enum mo_host_outcome result =
        call(host, heap, operation, type, args, out, error, storage, storage + length + 1);
    mo_scratch_free(&scratch);
    return result;
}
