#define _POSIX_C_SOURCE 200809L
#include "host/files.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static void release(intptr_t handle) {
    struct mo_folder *folder = (struct mo_folder *)handle;
    mo_host_close(folder->descriptor);
    MO_ASSERT(folder->owner->open_folders != 0);
    folder->owner->open_folders--;
}

/* Takes the descriptor on success only, including deferred narrowed roots. */
static MO_MUST_USE enum mo_host_outcome capability(struct mo_host *host, struct mo_heap *heap,
                                                   uint32_t type, int fd, struct mo_state *state,
                                                   const struct mo_cell *prefix, bool forbidden,
                                                   struct mo_cell *out,
                                                   struct mo_host_error *error) {
    if (host->open_folders == MO_LIMIT_OPEN_FOLDERS)
        return mo_host_bug(error, "too many folders open at once");
    enum mo_capability cap = host->types->types.items[type].capability;
    struct mo_capability_info info = {.operations = &mo_host_operations[false][cap],
                                      .state = state,
                                      .release = release,
                                      .has_prefix = prefix != NULL,
                                      .read_only = cap == MO_CAP_READ_FILES};
    if (prefix != NULL)
        info.prefix = *prefix;
    struct mo_folder folder = {
        .owner = host, .descriptor = fd, .pending = prefix != NULL, .forbidden = forbidden};
    if (!mo_value_capability_native(heap, type, &info, &folder, sizeof folder, out))
        return MO_HOST_OUT_OF_MEMORY;
    host->open_folders++;
    return MO_HOST_RETURNED;
}

enum mo_host_outcome mo_host_folder_cap(struct mo_host *host, struct mo_heap *heap, uint32_t type,
                                        int descriptor, const struct mo_cell *prefix,
                                        struct mo_cell *out, struct mo_host_error *error) {
    MO_ASSERT(prefix == NULL && descriptor >= 0);
    return capability(host, heap, type, descriptor, NULL, NULL, false, out, error);
}

/* Missing roots bind when first found. Retaining the parent capability, rather
 * than concatenating paths, preserves every narrowed root's authority. */
static MO_MUST_USE struct mo_file_error
root(struct mo_host *host, const struct mo_capability_info *info, int *descriptor) {
    const struct mo_capability_info *pending[MO_LIMIT_OPEN_FOLDERS];
    uint32_t count = 0;
    struct mo_folder *folder = (struct mo_folder *)info->handle;
    while (folder->pending) {
        if (folder->forbidden)
            return (struct mo_file_error){.status = FILE_NOT_ALLOWED};
        MO_ASSERT(count < MO_LIMIT_OPEN_FOLDERS && info->state != NULL);
        pending[count++] = info;
        info = mo_capability_get(mo_state_get(info->state, 0));
        folder = (struct mo_folder *)info->handle;
    }
    int fd = folder->descriptor;
    while (count != 0) {
        info = pending[--count];
        size_t length;
        const uint8_t *prefix = mo_value_data(info->prefix, &length);
        if (length >= MO_LIMIT_HOST_PATH_SIZE)
            return (struct mo_file_error){.status = FILE_FAILED, .reason = "path too long"};
        char path[MO_LIMIT_HOST_PATH_SIZE];
        memcpy(path, prefix, length);
        path[length] = '\0';
        struct mo_path_result resolved;
        struct mo_file_error problem = mo_host_resolve(host, fd, path, PATH_FOLDER, &resolved);
        if (problem.status != FILE_OK)
            return problem;
        folder = (struct mo_folder *)info->handle;
        mo_host_close(folder->descriptor);
        folder->descriptor = resolved.descriptor;
        folder->pending = false;
        fd = folder->descriptor;
    }
    *descriptor = fd;
    return (struct mo_file_error){.status = FILE_OK};
}

static MO_MUST_USE enum mo_host_outcome narrow(struct mo_host *host, struct mo_heap *heap,
                                               uint32_t operation, uint32_t type,
                                               const struct mo_cell *args, const char *path,
                                               struct mo_file_error problem, struct mo_cell *out,
                                               struct mo_host_error *error) {
    if (host->open_folders == MO_LIMIT_OPEN_FOLDERS)
        return mo_host_bug(error, "too many folders open at once");
    const struct mo_capability_info *info = mo_capability_get(args[0]);
    struct mo_folder *original = (struct mo_folder *)info->handle;
    bool readonly = operation == MO_HOST_FILES_READ_ONLY;
    int fd = -1, parent;
    bool pending = original->pending || !readonly;
    if (problem.status == FILE_OK && !original->forbidden) {
        problem = root(host, info, &parent);
        if (problem.status == FILE_OK && !readonly) {
            struct mo_path_result resolved;
            problem = mo_host_resolve(host, parent, path, PATH_FOLDER, &resolved);
            if (problem.status == FILE_OK) {
                fd = resolved.descriptor;
                pending = false;
            }
        } else if (problem.status == FILE_OK)
            pending = false;
    }
    if (fd < 0) {
        fd = fcntl(original->descriptor, F_DUPFD_CLOEXEC, 0);
        if (fd < 0)
            return mo_host_bug(error, "too many folders open at once");
    }
    struct mo_state *state = NULL;
    struct mo_cell prefix;
    if (pending) {
        enum mo_capability cap = ((const struct mo_host_operations *)info->operations)->capability;
        uint32_t parent_type = host->types->capabilities[cap];
        MO_ASSERT(parent_type != MO_NO_INDEX);
        if (!mo_state_new(heap, &parent_type, args, 1, &state) ||
            !mo_value_text(heap, readonly ? "" : path, readonly ? 0 : strlen(path), &prefix)) {
            if (state != NULL)
                mo_state_drop(heap, state);
            mo_host_close(fd);
            return MO_HOST_OUT_OF_MEMORY;
        }
    }
    bool forbidden = original->forbidden || problem.status == FILE_NOT_ALLOWED;
    enum mo_host_outcome result =
        capability(host, heap, type, fd, state, pending ? &prefix : NULL, forbidden, out, error);
    if (pending) {
        mo_value_drop(heap, host->types->text, prefix);
        mo_state_drop(heap, state);
    }
    if (result != MO_HOST_RETURNED)
        mo_host_close(fd);
    return result;
}

static MO_MUST_USE enum mo_host_outcome read_file(struct mo_host *host, struct mo_heap *heap,
                                                  uint32_t operation, uint32_t type, int fd,
                                                  struct mo_cell path, struct mo_cell *out) {
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, heap);
    size_t used = 0, capacity = 0;
    uint8_t *bytes = NULL;
    struct mo_file_error problem = {.status = FILE_OK};
    bool ok = true;
    for (;;) {
        if (used == capacity) {
            if (capacity == MO_LIMIT_STRING_SIZE) {
                uint8_t extra;
                ssize_t n = read(fd, &extra, 1);
                if (n < 0 && errno == EINTR)
                    continue;
                if (n == 0)
                    break;
                if (n < 0)
                    problem = mo_host_errno(errno);
                else
                    ok = false;
                break;
            }
            size_t next = capacity == 0 ? 256 : capacity * 2;
            if (next > MO_LIMIT_STRING_SIZE)
                next = MO_LIMIT_STRING_SIZE;
            void *grown = mo_scratch_resize(&scratch, bytes, capacity, next);
            if (grown == NULL) {
                ok = false;
                break;
            }
            bytes = grown;
            capacity = next;
        }
        ssize_t n = read(fd, bytes + used, capacity - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            problem = mo_host_errno(errno);
            break;
        }
        if (n == 0)
            break;
        used += (size_t)n;
    }
    bool text = operation == MO_HOST_FILES_READ_TEXT || operation == MO_HOST_READFILES_READ_TEXT;
    struct mo_cell value;
    if (ok && problem.status == FILE_OK) {
        ok = text ? mo_value_text(heap, (const char *)bytes, used, &value)
                  : mo_value_bytes(heap, bytes, used, &value);
        if (!ok && text && errno == EINVAL) {
            problem.status = FILE_NOT_TEXT;
            ok = true;
        }
    }
    mo_scratch_free(&scratch);
    if (!ok)
        return MO_HOST_OUT_OF_MEMORY;
    if (problem.status != FILE_OK)
        return mo_host_file_error(host, heap, type, path, problem, out);
    enum mo_host_outcome outcome = mo_host_file_ok(host, heap, type, value, out);
    uint32_t payload =
        host->types->types.items[type].cases[mo_host_tag(host, type, "Ok")].fields[0].type;
    mo_value_drop(heap, payload, value);
    return outcome;
}

static MO_MUST_USE struct mo_file_error write_file(int fd, struct mo_cell text, bool append) {
    if (!append && ftruncate(fd, 0) != 0)
        return mo_host_errno(errno);
    size_t length;
    const uint8_t *bytes = mo_value_data(text, &length);
    for (size_t at = 0; at < length;) {
        ssize_t n = write(fd, bytes + at, length - at);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return mo_host_errno(n == 0 ? EIO : errno);
        at += (size_t)n;
    }
    return (struct mo_file_error){.status = FILE_OK};
}

static MO_MUST_USE enum mo_host_outcome listing(struct mo_host *host, struct mo_heap *heap,
                                                uint32_t type, int fd, struct mo_cell path,
                                                struct mo_cell *out) {
    /* A new open description gives each list its own directory offset. */
    int copy = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (copy < 0)
        return mo_host_file_error(host, heap, type, path, mo_host_errno(errno), out);
    DIR *directory = fdopendir(copy);
    if (directory == NULL) {
        struct mo_file_error problem = mo_host_errno(errno);
        mo_host_close(copy);
        return mo_host_file_error(host, heap, type, path, problem, out);
    }
    struct mo_cell names;
    bool ok = mo_value_list(heap, host->types->texts, NULL, 0, &names);
    bool built = ok;
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, heap);
    uint8_t *kinds = NULL;
    size_t count = 0, capacity = 0;
    struct mo_file_error problem = {.status = FILE_OK};
    while (ok) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0)
                problem = mo_host_errno(errno);
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        if (count == capacity) {
            size_t next = capacity == 0 ? 16 : capacity * 2;
            void *grown = mo_scratch_resize(&scratch, kinds, capacity, next);
            if (grown == NULL) {
                ok = false;
                break;
            }
            kinds = grown;
            capacity = next;
        }
        struct stat st;
        if (fstatat(copy, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT)
                continue; /* vanished during enumeration */
            problem = mo_host_errno(errno);
            break;
        }
        struct mo_cell name;
        ok = mo_value_text(heap, entry->d_name, strlen(entry->d_name), &name);
        if (!ok)
            break;
        ok = mo_list_append(heap, host->types->texts, &names, name);
        mo_value_drop(heap, host->types->text, name);
        if (ok)
            kinds[count++] = S_ISREG(st.st_mode)   ? 0
                             : S_ISDIR(st.st_mode) ? 1
                             : S_ISLNK(st.st_mode) ? 2
                                                   : 3;
    }
    int closed = closedir(directory);
    if (closed != 0 && problem.status == FILE_OK)
        problem = mo_host_errno(errno);
    enum mo_host_outcome outcome = MO_HOST_OUT_OF_MEMORY;
    if (ok && problem.status != FILE_OK)
        outcome = mo_host_file_error(host, heap, type, path, problem, out);
    else if (ok) {
        uint32_t payload =
            host->types->types.items[type].cases[mo_host_tag(host, type, "Ok")].fields[0].type;
        struct mo_cell sorted;
        if (mo_host_names(host, heap, payload, names, kinds, &sorted)) {
            outcome = mo_host_file_ok(host, heap, type, sorted, out);
            mo_value_drop(heap, payload, sorted);
        }
    }
    if (built)
        mo_value_drop(heap, host->types->texts, names);
    mo_scratch_free(&scratch);
    return outcome;
}

static MO_MUST_USE enum mo_host_outcome call(struct mo_host *host, struct mo_heap *heap,
                                             uint32_t operation, uint32_t type,
                                             const struct mo_cell *args, struct mo_cell *out,
                                             struct mo_host_error *error, const char *path,
                                             struct mo_file_error problem) {
    bool readonly = operation == MO_HOST_FILES_READ_ONLY;
    bool within = operation == MO_HOST_FILES_WITHIN || operation == MO_HOST_READFILES_WITHIN;
    if (readonly || within)
        return narrow(host, heap, operation, type, args, path, problem, out, error);
    bool exists = operation == MO_HOST_FILES_EXISTS || operation == MO_HOST_READFILES_EXISTS;
    const struct mo_capability_info *info = mo_capability_get(args[0]);
    int fd;
    if (problem.status == FILE_OK)
        problem = root(host, info, &fd);
    enum mo_path_mode mode = PATH_READ;
    if (exists)
        mode = PATH_EXISTS;
    else if (operation == MO_HOST_FILES_WRITE_TEXT)
        mode = PATH_WRITE;
    else if (operation == MO_HOST_FILES_APPEND_TEXT)
        mode = PATH_APPEND;
    else if (operation == MO_HOST_FILES_REMOVE)
        mode = PATH_REMOVE;
    else if (operation == MO_HOST_FILES_MAKE_FOLDER)
        mode = PATH_MAKE_FOLDER;
    else if (operation == MO_HOST_FILES_LIST || operation == MO_HOST_READFILES_LIST ||
             operation == MO_HOST_FILES_ENTRIES || operation == MO_HOST_READFILES_ENTRIES)
        mode = PATH_FOLDER;
    struct mo_path_result resolved;
    if (problem.status == FILE_OK)
        problem = mo_host_resolve(host, fd, path, mode, &resolved);
    if (exists) {
        if (problem.status == FILE_OK)
            mo_host_close(resolved.descriptor);
        *out = mo_cell_bool(problem.status == FILE_OK);
        return MO_HOST_RETURNED;
    }
    if (problem.status != FILE_OK)
        return mo_host_file_error(host, heap, type, args[1], problem, out);
    enum mo_host_outcome outcome;
    fd = resolved.descriptor;
    if (mode == PATH_READ)
        outcome = read_file(host, heap, operation, type, fd, args[1], out);
    else if (mode == PATH_FOLDER)
        outcome = listing(host, heap, type, fd, args[1], out);
    else {
        if (mode == PATH_WRITE || mode == PATH_APPEND)
            problem = write_file(fd, args[2], mode == PATH_APPEND);
        else if (mode == PATH_REMOVE) {
            if (S_ISDIR(resolved.stat.st_mode))
                problem = (struct mo_file_error){.status = FILE_FAILED, .reason = "is a folder"};
            else if (unlinkat(fd, resolved.name, 0) != 0)
                problem = mo_host_errno(errno);
        }
        outcome = problem.status == FILE_OK
                      ? mo_host_file_ok(host, heap, type, mo_cell_unit(), out)
                      : mo_host_file_error(host, heap, type, args[1], problem, out);
    }
    mo_host_close(fd);
    return outcome;
}

enum mo_host_outcome mo_host_real_files_call(struct mo_host *host, struct mo_heap *heap,
                                             uint32_t operation, uint32_t type,
                                             const struct mo_cell *args, struct mo_cell *out,
                                             struct mo_host_error *error) {
    char local[MO_LIMIT_HOST_PATH_SIZE] = "";
    char *path = local;
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, heap);
    struct mo_file_error problem = {.status = FILE_OK};
    if (operation != MO_HOST_FILES_READ_ONLY) {
        size_t length;
        const uint8_t *bytes = mo_value_data(args[1], &length);
        (void)bytes;
        if (length >= sizeof local) {
            path = mo_scratch_resize(&scratch, NULL, 0, length + 1);
            if (path == NULL) {
                mo_scratch_free(&scratch);
                return MO_HOST_OUT_OF_MEMORY;
            }
        }
        problem = mo_host_normalize(args[1], path);
        if (problem.status != FILE_OK)
            path[0] = '\0';
    }
    enum mo_host_outcome outcome =
        call(host, heap, operation, type, args, out, error, path, problem);
    mo_scratch_free(&scratch);
    return outcome;
}
