#include "host/files.h"

#include <errno.h>
#include <string.h>

static void release_fake_file(intptr_t handle) {
    struct mo_fake_open_file *file = (struct mo_fake_open_file *)handle;
    MO_ASSERT(file->owner->open_files != 0);
    file->owner->open_files--;
}

bool mo_host_texts_to_bytes_map(struct mo_host *host, struct mo_heap *heap, struct mo_cell text_map,
                                struct mo_cell *bytes_map) {
    struct mo_cell out;
    if (!mo_value_map(heap, host->types->file_bytes, NULL, NULL, 0, &out))
        return false;
    uint32_t position = 0;
    struct mo_cell key, value;
    while (mo_table_next(text_map, &position, &key, &value)) {
        size_t len;
        const uint8_t *data = mo_value_data(value, &len);
        struct mo_cell raw;
        if (!mo_value_bytes(heap, len == 0 ? (const uint8_t *)"" : data, len, &raw)) {
            mo_value_drop(heap, host->types->file_bytes, out);
            return false;
        }
        bool ok = mo_table_set(heap, host->types->file_bytes, &out, key, raw);
        mo_value_drop(heap, host->types->bytes, raw);
        if (!ok) {
            mo_value_drop(heap, host->types->file_bytes, out);
            return false;
        }
    }
    *bytes_map = out;
    return true;
}

bool mo_host_bytes_to_texts_map(struct mo_host *host, struct mo_heap *heap,
                                struct mo_cell bytes_map, struct mo_cell *text_map) {
    struct mo_cell out;
    if (!mo_value_map(heap, host->types->files, NULL, NULL, 0, &out))
        return false;
    uint32_t position = 0;
    struct mo_cell key, value;
    while (mo_table_next(bytes_map, &position, &key, &value)) {
        size_t len;
        const uint8_t *data = mo_value_data(value, &len);
        struct mo_cell text;
        if (!mo_value_text(heap, len == 0 ? "" : (const char *)data, len, &text)) {
            mo_value_drop(heap, host->types->files, out);
            return false;
        }
        bool ok = mo_table_set(heap, host->types->files, &out, key, text);
        mo_value_drop(heap, host->types->text, text);
        if (!ok) {
            mo_value_drop(heap, host->types->files, out);
            return false;
        }
    }
    *text_map = out;
    return true;
}

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

static MO_MUST_USE bool parent_folder_exists(struct mo_host *host, struct mo_heap *heap,
                                             struct mo_cell folders, const char *path,
                                             size_t length, bool *exists) {
    size_t parent_length = length;
    while (parent_length != 0 && path[parent_length - 1] != '/')
        parent_length--;
    if (parent_length != 0)
        parent_length--;
    return folder(host, heap, folders, path, parent_length, exists);
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
    struct mo_cell raw_files;
    if (!mo_host_texts_to_bytes_map(host, heap, files, &raw_files)) {
        mo_value_drop(heap, host->types->folders, folders);
        return MO_HOST_OUT_OF_MEMORY;
    }
    struct mo_cell root;
    if (!mo_value_text(heap, "", 0, &root)) {
        mo_value_drop(heap, host->types->file_bytes, raw_files);
        mo_value_drop(heap, host->types->folders, folders);
        return MO_HOST_OUT_OF_MEMORY;
    }
    uint32_t types[] = {host->types->file_bytes, host->types->folders};
    struct mo_cell values[] = {raw_files, folders};
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
    mo_value_drop(heap, host->types->file_bytes, raw_files);
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

static MO_MUST_USE enum mo_host_outcome
fake_file_op(struct mo_host *host, struct mo_heap *heap, uint32_t operation, uint32_t type,
             const struct mo_cell *args, struct mo_cell *out, struct mo_host_error *error) {
    const struct mo_capability_info *info = mo_capability_get(args[0]);
    MO_ASSERT(info->state != NULL && info->has_prefix);
    struct mo_cell key = info->prefix;
    if (operation == MO_HOST_FILES_SYNC)
        return mo_host_file_ok(host, heap, type, mo_cell_unit(), out);
    if (operation == MO_HOST_FILES_READ_AT) {
        int64_t offset = mo_cell_get_int(args[1]);
        int64_t count = mo_cell_get_int(args[2]);
        if (offset < 0)
            return mo_host_bug(error, "negative file offset");
        if (count < 0 || (uint64_t)count > MO_LIMIT_FILE_IO_BYTES)
            return mo_host_bug(error, "read count outside 0 to the read limit");
        bool found;
        struct mo_cell value;
        if (!mo_table_get(heap, host->types->file_bytes, mo_state_get(info->state, 0), key, &found,
                          &value))
            return MO_HOST_OUT_OF_MEMORY;
        if (!found)
            return mo_host_file_error(host, heap, type, key,
                                      (struct mo_file_error){.status = FILE_NOT_FOUND}, out);
        size_t len;
        const uint8_t *bytes = mo_value_data(value, &len);
        size_t take = 0;
        const uint8_t *src = (const uint8_t *)"";
        if (count > 0 && (uint64_t)offset < len) {
            size_t avail = len - (size_t)offset;
            take = (size_t)count < avail ? (size_t)count : avail;
            src = bytes + (size_t)offset;
        }
        struct mo_cell slice;
        if (!mo_value_bytes(heap, src, take, &slice))
            return MO_HOST_OUT_OF_MEMORY;
        enum mo_host_outcome outcome = mo_host_file_ok(host, heap, type, slice, out);
        uint32_t payload =
            host->types->types.items[type].cases[mo_host_tag(host, type, "Ok")].fields[0].type;
        mo_value_drop(heap, payload, slice);
        return outcome;
    }
    if (operation == MO_HOST_FILES_SIZE) {
        bool found;
        struct mo_cell value;
        if (!mo_table_get(heap, host->types->file_bytes, mo_state_get(info->state, 0), key, &found,
                          &value))
            return MO_HOST_OUT_OF_MEMORY;
        if (!found)
            return mo_host_file_error(host, heap, type, key,
                                      (struct mo_file_error){.status = FILE_NOT_FOUND}, out);
        size_t len;
        const uint8_t *bytes = mo_value_data(value, &len);
        (void)bytes;
        return mo_host_file_ok(host, heap, type, mo_cell_int((int64_t)len), out);
    }
    if (operation == MO_HOST_FILES_WRITE_AT) {
        int64_t offset = mo_cell_get_int(args[1]);
        if (offset < 0)
            return mo_host_bug(error, "negative file offset");
        bool found;
        struct mo_cell value;
        if (!mo_table_get(heap, host->types->file_bytes, mo_state_get(info->state, 0), key, &found,
                          &value))
            return MO_HOST_OUT_OF_MEMORY;
        if (!found)
            return mo_host_file_error(host, heap, type, key,
                                      (struct mo_file_error){.status = FILE_NOT_FOUND}, out);
        size_t cur_len, write_len;
        const uint8_t *cur = mo_value_data(value, &cur_len);
        const uint8_t *src = mo_value_data(args[2], &write_len);
        if (write_len == 0)
            return mo_host_file_ok(host, heap, type, mo_cell_unit(), out);
        if ((uint64_t)offset > MO_LIMIT_VALUE_BYTES ||
            write_len > MO_LIMIT_VALUE_BYTES - (uint64_t)offset) {
            errno = ENOSPC;
            return MO_HOST_OUT_OF_MEMORY;
        }
        size_t end = (size_t)offset + write_len;
        size_t new_len = end > cur_len ? end : cur_len;
        struct mo_scratch scratch;
        mo_scratch_init(&scratch, heap);
        uint8_t *buf = new_len == 0 ? NULL : mo_scratch_resize(&scratch, NULL, 0, new_len);
        struct mo_cell updated;
        bool ok = new_len == 0 || buf != NULL;
        if (ok) {
            if (cur_len != 0)
                memcpy(buf, cur, cur_len);
            if ((size_t)offset > cur_len)
                memset(buf + cur_len, 0, (size_t)offset - cur_len);
            if (write_len != 0)
                memcpy(buf + (size_t)offset, src, write_len);
            ok = mo_value_bytes(heap, new_len == 0 ? (const uint8_t *)"" : buf, new_len, &updated);
        }
        int number = errno;
        mo_scratch_free(&scratch);
        if (!ok) {
            errno = number;
            return MO_HOST_OUT_OF_MEMORY;
        }
        struct mo_cell result;
        enum mo_host_outcome outcome = mo_host_file_ok(host, heap, type, mo_cell_unit(), &result);
        if (outcome == MO_HOST_RETURNED) {
            if (!mo_table_set(heap, host->types->file_bytes, mo_state_slot(info->state, 0), key,
                              updated)) {
                mo_value_drop(heap, type, result);
                outcome = MO_HOST_OUT_OF_MEMORY;
            } else
                *out = result;
        }
        mo_value_drop(heap, host->types->bytes, updated);
        return outcome;
    }
    MO_ASSERT(operation == MO_HOST_FILES_TRUNCATE);
    int64_t size = mo_cell_get_int(args[1]);
    if (size < 0)
        return mo_host_bug(error, "negative file size");
    bool found;
    struct mo_cell value;
    if (!mo_table_get(heap, host->types->file_bytes, mo_state_get(info->state, 0), key, &found,
                      &value))
        return MO_HOST_OUT_OF_MEMORY;
    if (!found)
        return mo_host_file_error(host, heap, type, key,
                                  (struct mo_file_error){.status = FILE_NOT_FOUND}, out);
    if ((uint64_t)size > MO_LIMIT_VALUE_BYTES) {
        errno = ENOSPC;
        return MO_HOST_OUT_OF_MEMORY;
    }
    size_t target = (size_t)size;
    size_t cur_len;
    const uint8_t *cur = mo_value_data(value, &cur_len);
    struct mo_cell updated;
    if (target <= cur_len) {
        if (!mo_value_bytes(heap, target == 0 ? (const uint8_t *)"" : cur, target, &updated))
            return MO_HOST_OUT_OF_MEMORY;
    } else {
        struct mo_scratch scratch;
        mo_scratch_init(&scratch, heap);
        uint8_t *buf = mo_scratch_resize(&scratch, NULL, 0, target);
        bool ok = buf != NULL;
        if (ok) {
            if (cur_len != 0)
                memcpy(buf, cur, cur_len);
            memset(buf + cur_len, 0, target - cur_len);
            ok = mo_value_bytes(heap, buf, target, &updated);
        }
        int number = errno;
        mo_scratch_free(&scratch);
        if (!ok) {
            errno = number;
            return MO_HOST_OUT_OF_MEMORY;
        }
    }
    struct mo_cell result;
    enum mo_host_outcome outcome = mo_host_file_ok(host, heap, type, mo_cell_unit(), &result);
    if (outcome == MO_HOST_RETURNED) {
        if (!mo_table_set(heap, host->types->file_bytes, mo_state_slot(info->state, 0), key,
                          updated)) {
            mo_value_drop(heap, type, result);
            outcome = MO_HOST_OUT_OF_MEMORY;
        } else
            *out = result;
    }
    mo_value_drop(heap, host->types->bytes, updated);
    return outcome;
}

static MO_MUST_USE enum mo_host_outcome fake_rename(struct mo_host *host, struct mo_heap *heap,
                                                    uint32_t type, const struct mo_cell *args,
                                                    struct mo_cell *out) {
    const struct mo_capability_info *info = mo_capability_get(args[0]);
    size_t from_len = 0, to_len = 0, root_length = 0;
    const uint8_t *from_raw = mo_value_data(args[1], &from_len);
    const uint8_t *to_raw = mo_value_data(args[2], &to_len);
    (void)from_raw;
    (void)to_raw;
    if (info->has_prefix) {
        const uint8_t *root_raw = mo_value_data(info->prefix, &root_length);
        (void)root_raw;
    }
    MO_ASSERT(from_len <= UINT32_MAX && to_len <= UINT32_MAX && root_length <= UINT32_MAX);
    size_t from_cap = 2 * from_len + root_length + 3;
    size_t to_cap = 2 * to_len + root_length + 3;
    if (from_len > (SIZE_MAX - 3 - root_length) / 2 || to_len > (SIZE_MAX - 3 - root_length) / 2 ||
        from_cap > SIZE_MAX - to_cap) {
        errno = ENOSPC;
        return MO_HOST_OUT_OF_MEMORY;
    }
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, heap);
    char *storage = mo_scratch_resize(&scratch, NULL, 0, from_cap + to_cap);
    if (storage == NULL) {
        mo_scratch_free(&scratch);
        return MO_HOST_OUT_OF_MEMORY;
    }
    char *from_norm = storage;
    char *from_path = storage + from_len + 1;
    char *to_norm = storage + from_cap;
    char *to_path = to_norm + to_len + 1;
    size_t flen = 0, tlen = 0;
    struct mo_file_error from_problem = mo_host_normalize(args[1], from_norm, &flen);
    struct mo_file_error to_problem = mo_host_normalize(args[2], to_norm, &tlen);
    if (!info->has_prefix) {
        from_problem = (struct mo_file_error){.status = FILE_NOT_ALLOWED};
        to_problem = (struct mo_file_error){.status = FILE_NOT_ALLOWED};
    }
    enum mo_host_outcome outcome = MO_HOST_OUT_OF_MEMORY;
    if (from_problem.status != FILE_OK) {
        outcome = mo_host_file_error(host, heap, type, args[1], from_problem, out);
        goto done;
    }
    if (to_problem.status != FILE_OK) {
        outcome = mo_host_file_error(host, heap, type, args[2], to_problem, out);
        goto done;
    }
    flen = mo_host_join(info->prefix, from_norm, flen, from_path);
    tlen = mo_host_join(info->prefix, to_norm, tlen, to_path);
    if (flen > UINT32_MAX || tlen > UINT32_MAX) {
        errno = ENOSPC;
        goto done;
    }
    struct mo_cell folders = mo_state_get(info->state, 1), files = mo_state_get(info->state, 0);
    bool root_folder = root_length == 0;
    struct mo_cell unused;
    if (!root_folder &&
        !mo_table_get(heap, host->types->folders, folders, info->prefix, &root_folder, &unused))
        goto done;
    bool from_block = blocked(info, from_path, flen);
    bool from_is_folder = false, from_is_file = false;
    struct mo_cell from_value;
    if (!folder(host, heap, folders, from_path, flen, &from_is_folder) ||
        !contains(host, heap, host->types->file_bytes, files, from_path, flen, &from_is_file,
                  &from_value))
        goto done;
    if (from_block) {
        outcome = mo_host_file_error(
            host, heap, type, args[1],
            (struct mo_file_error){.status = FILE_FAILED, .reason = "not a folder"}, out);
        goto done;
    }
    if (!root_folder) {
        outcome = mo_host_file_error(host, heap, type, args[1],
                                     (struct mo_file_error){.status = FILE_NOT_FOUND}, out);
        goto done;
    }
    if (from_is_folder) {
        outcome = mo_host_file_error(
            host, heap, type, args[1],
            (struct mo_file_error){.status = FILE_FAILED, .reason = "is a folder"}, out);
        goto done;
    }
    if (!from_is_file) {
        outcome = mo_host_file_error(host, heap, type, args[1],
                                     (struct mo_file_error){.status = FILE_NOT_FOUND}, out);
        goto done;
    }
    bool to_block = blocked(info, to_path, tlen);
    bool to_is_folder = false, to_parent_exists = false;
    if (!folder(host, heap, folders, to_path, tlen, &to_is_folder) ||
        !parent_folder_exists(host, heap, folders, to_path, tlen, &to_parent_exists))
        goto done;
    if (to_block) {
        outcome = mo_host_file_error(
            host, heap, type, args[2],
            (struct mo_file_error){.status = FILE_FAILED, .reason = "not a folder"}, out);
        goto done;
    }
    if (to_is_folder) {
        outcome = mo_host_file_error(
            host, heap, type, args[2],
            (struct mo_file_error){.status = FILE_FAILED, .reason = "is a folder"}, out);
        goto done;
    }
    if (!to_parent_exists) {
        outcome = mo_host_file_error(host, heap, type, args[2],
                                     (struct mo_file_error){.status = FILE_NOT_FOUND}, out);
        goto done;
    }
    if (flen == tlen && memcmp(from_path, to_path, flen) == 0) {
        outcome = mo_host_file_ok(host, heap, type, mo_cell_unit(), out);
        goto done;
    }
    struct mo_cell from_key, to_key;
    if (!mo_value_text(heap, from_path, flen, &from_key))
        goto done;
    if (!mo_value_text(heap, to_path, tlen, &to_key)) {
        mo_value_drop(heap, host->types->text, from_key);
        goto done;
    }
    struct mo_cell result;
    outcome = mo_host_file_ok(host, heap, type, mo_cell_unit(), &result);
    if (outcome == MO_HOST_RETURNED) {
        struct mo_cell staged = mo_value_dup(heap, host->types->file_bytes, files);
        struct mo_cell kept = mo_value_dup(heap, host->types->bytes, from_value);
        bool ok = mo_table_remove(heap, host->types->file_bytes, &staged, from_key) &&
                  mo_table_set(heap, host->types->file_bytes, &staged, to_key, kept);
        if (ok) {
            mo_state_set(heap, info->state, 0, staged);
            *out = result;
        } else {
            mo_value_drop(heap, type, result);
            outcome = MO_HOST_OUT_OF_MEMORY;
        }
        mo_value_drop(heap, host->types->bytes, kept);
        mo_value_drop(heap, host->types->file_bytes, staged);
    }
    mo_value_drop(heap, host->types->text, to_key);
    mo_value_drop(heap, host->types->text, from_key);
done:
    mo_scratch_free(&scratch);
    return outcome;
}

static MO_MUST_USE enum mo_host_outcome call(struct mo_host *host, struct mo_heap *heap,
                                             uint32_t operation, uint32_t type,
                                             const struct mo_cell *args, struct mo_cell *out,
                                             struct mo_host_error *error, char *normalized,
                                             char *path) {
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
        !contains(host, heap, host->types->file_bytes, files, path, length, &is_file, &value))
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
    if (operation == MO_HOST_FILES_OPEN) {
        bool parent_exists;
        if (!parent_folder_exists(host, heap, folders, path, length, &parent_exists))
            return MO_HOST_OUT_OF_MEMORY;
        if (!parent_exists)
            return mo_host_file_error(host, heap, type, args[1],
                                      (struct mo_file_error){.status = FILE_NOT_FOUND}, out);
        if (host->open_files == MO_LIMIT_OPEN_FILES)
            return mo_host_bug(error, "too many files open at once");
        struct mo_cell key;
        if (!mo_value_text(heap, path, length, &key))
            return MO_HOST_OUT_OF_MEMORY;
        struct mo_cell staged = mo_cell_unit();
        bool have_staged = false;
        if (!is_file) {
            struct mo_cell empty;
            if (!mo_value_bytes(heap, (const uint8_t *)"", 0, &empty)) {
                mo_value_drop(heap, host->types->text, key);
                return MO_HOST_OUT_OF_MEMORY;
            }
            staged = mo_value_dup(heap, host->types->file_bytes, files);
            bool ok = mo_table_set(heap, host->types->file_bytes, &staged, key, empty);
            mo_value_drop(heap, host->types->bytes, empty);
            if (!ok) {
                mo_value_drop(heap, host->types->file_bytes, staged);
                mo_value_drop(heap, host->types->text, key);
                return MO_HOST_OUT_OF_MEMORY;
            }
            have_staged = true;
        }
        uint32_t file_type =
            host->types->types.items[type].cases[mo_host_tag(host, type, "Ok")].fields[0].type;
        struct mo_capability_info cap_info = {.operations = &mo_host_operations[true][MO_CAP_FILE],
                                              .state = info->state,
                                              .release = release_fake_file,
                                              .has_prefix = true,
                                              .prefix = key};
        struct mo_fake_open_file payload = {.owner = host};
        struct mo_cell file_cap;
        if (!mo_value_capability_native(heap, file_type, &cap_info, &payload, sizeof payload,
                                        &file_cap)) {
            if (have_staged)
                mo_value_drop(heap, host->types->file_bytes, staged);
            mo_value_drop(heap, host->types->text, key);
            return MO_HOST_OUT_OF_MEMORY;
        }
        host->open_files++;
        enum mo_host_outcome outcome = mo_host_file_ok(host, heap, type, file_cap, out);
        mo_value_drop(heap, file_type, file_cap);
        if (have_staged) {
            if (outcome == MO_HOST_RETURNED)
                mo_state_set(heap, info->state, 0, staged);
            mo_value_drop(heap, host->types->file_bytes, staged);
        }
        mo_value_drop(heap, host->types->text, key);
        return outcome;
    }
    if (operation == MO_HOST_FILES_WRITE_TEXT || operation == MO_HOST_FILES_APPEND_TEXT ||
        operation == MO_HOST_FILES_WRITE_BYTES) {
        bool parent_exists;
        if (!parent_folder_exists(host, heap, folders, path, length, &parent_exists))
            return MO_HOST_OUT_OF_MEMORY;
        if (!parent_exists)
            return mo_host_file_error(host, heap, type, args[1],
                                      (struct mo_file_error){.status = FILE_NOT_FOUND}, out);
        struct mo_cell key;
        if (!mo_value_text(heap, path, length, &key))
            return MO_HOST_OUT_OF_MEMORY;
        struct mo_cell stored = args[2];
        bool owns_stored = false;
        if (operation == MO_HOST_FILES_WRITE_TEXT) {
            size_t len;
            const uint8_t *src = mo_value_data(args[2], &len);
            if (!mo_value_bytes(heap, len == 0 ? (const uint8_t *)"" : src, len, &stored)) {
                mo_value_drop(heap, host->types->text, key);
                return MO_HOST_OUT_OF_MEMORY;
            }
            owns_stored = true;
        } else if (operation == MO_HOST_FILES_APPEND_TEXT) {
            size_t a = 0, b;
            const uint8_t *pa = is_file ? mo_value_data(value, &a) : NULL;
            const uint8_t *pb = mo_value_data(args[2], &b);
            if (b > UINT32_MAX - a) {
                mo_value_drop(heap, host->types->text, key);
                errno = ENOSPC;
                return MO_HOST_OUT_OF_MEMORY;
            }
            struct mo_scratch scratch;
            mo_scratch_init(&scratch, heap);
            uint8_t *bytes = a + b == 0 ? NULL : mo_scratch_resize(&scratch, NULL, 0, a + b);
            bool ok = a + b == 0 || bytes != NULL;
            if (ok) {
                if (a != 0)
                    memcpy(bytes, pa, a);
                if (b != 0)
                    memcpy(bytes + a, pb, b);
                ok = mo_value_bytes(heap, a + b == 0 ? (const uint8_t *)"" : bytes, a + b, &stored);
            }
            mo_scratch_free(&scratch);
            if (!ok) {
                mo_value_drop(heap, host->types->text, key);
                return MO_HOST_OUT_OF_MEMORY;
            }
            owns_stored = true;
        }
        struct mo_cell result;
        enum mo_host_outcome outcome = mo_host_file_ok(host, heap, type, mo_cell_unit(), &result);
        if (outcome == MO_HOST_RETURNED) {
            if (!mo_table_set(heap, host->types->file_bytes, mo_state_slot(info->state, 0), key,
                              stored)) {
                mo_value_drop(heap, type, result);
                outcome = MO_HOST_OUT_OF_MEMORY;
            } else
                *out = result;
        }
        if (owns_stored)
            mo_value_drop(heap, host->types->bytes, stored);
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
        bool ok =
            mo_table_remove(heap, host->types->file_bytes, mo_state_slot(info->state, 0), key);
        mo_value_drop(heap, host->types->text, key);
        return ok ? mo_host_file_ok(host, heap, type, mo_cell_unit(), out) : MO_HOST_OUT_OF_MEMORY;
    }
    if (operation == MO_HOST_FILES_READ_BYTES || operation == MO_HOST_READFILES_READ_BYTES)
        return mo_host_file_ok(host, heap, type, value, out);
    size_t data_length;
    const uint8_t *bytes = mo_value_data(value, &data_length);
    struct mo_cell text;
    if (!mo_value_text(heap, data_length == 0 ? "" : (const char *)bytes, data_length, &text)) {
        if (errno == EINVAL)
            return mo_host_file_error(host, heap, type, args[1],
                                      (struct mo_file_error){.status = FILE_NOT_TEXT}, out);
        return MO_HOST_OUT_OF_MEMORY;
    }
    enum mo_host_outcome outcome = mo_host_file_ok(host, heap, type, text, out);
    mo_value_drop(heap, host->types->text, text);
    return outcome;
}

enum mo_host_outcome mo_host_fake_files_call(struct mo_host *host, struct mo_heap *heap,
                                             uint32_t operation, uint32_t type,
                                             const struct mo_cell *args, struct mo_cell *out,
                                             struct mo_host_error *error) {
    if (operation == MO_HOST_FILES_READ_AT || operation == MO_HOST_FILES_WRITE_AT ||
        operation == MO_HOST_FILES_SIZE || operation == MO_HOST_FILES_TRUNCATE ||
        operation == MO_HOST_FILES_SYNC)
        return fake_file_op(host, heap, operation, type, args, out, error);
    if (operation == MO_HOST_FILES_RENAME)
        return fake_rename(host, heap, type, args, out);
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
