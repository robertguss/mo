#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#include "host/files.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void release(intptr_t handle) {
    struct mo_folder *folder = (struct mo_folder *)handle;
    mo_host_close(folder->descriptor);
    MO_ASSERT(folder->owner->open_folders != 0);
    folder->owner->open_folders--;
}

static void release_open_file(intptr_t handle) {
    struct mo_open_file *file = (struct mo_open_file *)handle;
    if (file->descriptor >= 0) {
        mo_host_close(file->descriptor);
        file->descriptor = -1;
    }
    MO_ASSERT(file->owner->open_files != 0);
    file->owner->open_files--;
}

static MO_MUST_USE struct mo_file_error sync_fd(int fd) {
#if defined(__APPLE__) && defined(F_FULLFSYNC)
    if (fcntl(fd, F_FULLFSYNC) == 0)
        return (struct mo_file_error){.status = FILE_OK};
    if (errno != ENOTSUP && errno != EINVAL && errno != ENOTTY)
        return mo_host_errno(errno);
#endif
    while (fsync(fd) != 0) {
        if (errno == EINTR)
            continue;
        return mo_host_errno(errno);
    }
    return (struct mo_file_error){.status = FILE_OK};
}

static MO_MUST_USE struct mo_file_error sync_dir(int dir_fd) {
    int syncable = openat(dir_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (syncable < 0)
        return mo_host_errno(errno);
    struct mo_file_error err = sync_fd(syncable);
    mo_host_close(syncable);
    return err;
}

static MO_MUST_USE struct mo_file_error
open_strict_leaf(int parent_fd, const char *leaf, int access_flags, int *fd_out, bool *created) {
    *fd_out = -1;
    *created = false;
    int common = O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | O_NOCTTY;
    for (unsigned attempt = 0; attempt < 4; attempt++) {
        struct stat st;
        int inspected = fstatat(parent_fd, leaf, &st, AT_SYMLINK_NOFOLLOW);
        if (inspected == 0) {
            if (S_ISLNK(st.st_mode))
                return (struct mo_file_error){.status = FILE_NOT_ALLOWED, .containment = true};
            if (S_ISDIR(st.st_mode))
                return (struct mo_file_error){.status = FILE_FAILED, .reason = "is a folder"};
            if (!S_ISREG(st.st_mode))
                return (struct mo_file_error){.status = FILE_FAILED,
                                              .reason = "not a regular file"};
            int fd = openat(parent_fd, leaf, access_flags | common);
            if (fd < 0) {
                if (errno == ELOOP)
                    return (struct mo_file_error){.status = FILE_NOT_ALLOWED, .containment = true};
                if (errno == ENOENT)
                    continue;
                return mo_host_errno(errno);
            }
            if (fstat(fd, &st) != 0) {
                struct mo_file_error err = mo_host_errno(errno);
                mo_host_close(fd);
                return err;
            }
            if (!S_ISREG(st.st_mode)) {
                mo_host_close(fd);
                return (struct mo_file_error){.status = FILE_FAILED,
                                              .reason = S_ISDIR(st.st_mode) ? "is a folder"
                                                                            : "not a regular file"};
            }
            *fd_out = fd;
            *created = false;
            return (struct mo_file_error){.status = FILE_OK};
        }
        if (errno != ENOENT)
            return mo_host_errno(errno);
        int fd = openat(parent_fd, leaf, access_flags | O_CREAT | O_EXCL | common, 0644);
        if (fd < 0) {
            if (errno == EEXIST)
                continue;
            if (errno == ELOOP)
                return (struct mo_file_error){.status = FILE_NOT_ALLOWED, .containment = true};
            return mo_host_errno(errno);
        }
        *fd_out = fd;
        *created = true;
        return (struct mo_file_error){.status = FILE_OK};
    }
    return (struct mo_file_error){.status = FILE_FAILED, .reason = "changed while being used"};
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
    bool forbidden = original->forbidden || problem.containment;
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
            if (capacity == MO_LIMIT_VALUE_BYTES) {
                uint8_t extra;
                ssize_t n = read(fd, &extra, 1);
                if (n < 0 && errno == EINTR)
                    continue;
                if (n == 0)
                    break;
                if (n < 0)
                    problem = mo_host_errno(errno);
                else {
                    errno = EOVERFLOW; /* Runtime representation, not the heap budget. */
                    ok = false;
                }
                break;
            }
            size_t next = capacity == 0 ? (MO_LIMIT_VALUE_BYTES < 256 ? MO_LIMIT_VALUE_BYTES : 256)
                          : capacity > MO_LIMIT_VALUE_BYTES / 2 ? MO_LIMIT_VALUE_BYTES
                                                                : capacity * 2;
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
    int number = errno;
    mo_scratch_free(&scratch);
    if (!ok)
        errno = number;
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
        if (!ok) {
            if (errno == EINVAL) {
                problem = (struct mo_file_error){
                    .status = FILE_FAILED, .reason = "folder contains a name that is not text"};
                ok = true;
            }
            break;
        }
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

static MO_MUST_USE enum mo_host_outcome
real_file_op(struct mo_host *host, struct mo_heap *heap, uint32_t operation, uint32_t type,
             const struct mo_cell *args, struct mo_cell *out, struct mo_host_error *error) {
    const struct mo_capability_info *info = mo_capability_get(args[0]);
    const struct mo_open_file *file = (const struct mo_open_file *)info->handle;
    MO_ASSERT(file != NULL && file->descriptor >= 0 && info->has_prefix);
    struct mo_cell path = info->prefix;
    if (operation == MO_HOST_FILES_SYNC) {
        struct mo_file_error problem = sync_fd(file->descriptor);
        return problem.status == FILE_OK ? mo_host_file_ok(host, heap, type, mo_cell_unit(), out)
                                         : mo_host_file_error(host, heap, type, path, problem, out);
    }
    if (operation == MO_HOST_FILES_SIZE) {
        struct stat st;
        if (fstat(file->descriptor, &st) != 0)
            return mo_host_file_error(host, heap, type, path, mo_host_errno(errno), out);
        if (st.st_size < 0)
            return mo_host_file_error(
                host, heap, type, path,
                (struct mo_file_error){.status = FILE_FAILED, .reason = "negative file size"}, out);
        return mo_host_file_ok(host, heap, type, mo_cell_int((int64_t)st.st_size), out);
    }
    if (operation == MO_HOST_FILES_TRUNCATE) {
        _Static_assert(sizeof(off_t) >= sizeof(int64_t), "64-bit off_t required");
        int64_t size = mo_cell_get_int(args[1]);
        if (size < 0)
            return mo_host_bug(error, "negative file size");
        while (ftruncate(file->descriptor, (off_t)size) != 0) {
            if (errno == EINTR)
                continue;
            return mo_host_file_error(host, heap, type, path, mo_host_errno(errno), out);
        }
        return mo_host_file_ok(host, heap, type, mo_cell_unit(), out);
    }
    if (operation == MO_HOST_FILES_WRITE_AT) {
        int64_t offset = mo_cell_get_int(args[1]);
        if (offset < 0)
            return mo_host_bug(error, "negative file offset");
        size_t length;
        const uint8_t *bytes = mo_value_data(args[2], &length);
        if (length > (uint64_t)INT64_MAX - (uint64_t)offset)
            return mo_host_file_error(host, heap, type, path, mo_host_errno(EFBIG), out);
        for (size_t at = 0; at < length;) {
            ssize_t n =
                pwrite(file->descriptor, bytes + at, length - at, (off_t)offset + (off_t)at);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                return mo_host_file_error(host, heap, type, path,
                                          mo_host_errno(n == 0 ? EIO : errno), out);
            at += (size_t)n;
        }
        return mo_host_file_ok(host, heap, type, mo_cell_unit(), out);
    }
    MO_ASSERT(operation == MO_HOST_FILES_READ_AT);
    int64_t offset = mo_cell_get_int(args[1]);
    int64_t count = mo_cell_get_int(args[2]);
    if (offset < 0)
        return mo_host_bug(error, "negative file offset");
    if (count < 0 || (uint64_t)count > MO_LIMIT_FILE_IO_BYTES)
        return mo_host_bug(error, "read count outside 0 to the read limit");
    if (count == 0) {
        struct mo_cell empty;
        if (!mo_value_bytes(heap, (const uint8_t *)"", 0, &empty))
            return MO_HOST_OUT_OF_MEMORY;
        enum mo_host_outcome outcome = mo_host_file_ok(host, heap, type, empty, out);
        uint32_t payload =
            host->types->types.items[type].cases[mo_host_tag(host, type, "Ok")].fields[0].type;
        mo_value_drop(heap, payload, empty);
        return outcome;
    }
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, heap);
    size_t want = (size_t)count;
    uint8_t *buf = mo_scratch_resize(&scratch, NULL, 0, want);
    if (buf == NULL) {
        int number = errno;
        mo_scratch_free(&scratch);
        errno = number;
        return MO_HOST_OUT_OF_MEMORY;
    }
    size_t got = 0;
    struct mo_file_error problem = {.status = FILE_OK};
    while (got < want) {
        uint64_t avail = (uint64_t)INT64_MAX - (uint64_t)offset - got;
        if (avail == 0)
            break;
        size_t remaining = want - got;
        size_t chunk = (uint64_t)remaining < avail ? remaining : (size_t)avail;
        ssize_t n = pread(file->descriptor, buf + got, chunk, (off_t)offset + (off_t)got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            problem = mo_host_errno(errno);
            break;
        }
        if (n == 0)
            break;
        got += (size_t)n;
    }
    struct mo_cell value;
    bool ok = true;
    if (problem.status == FILE_OK)
        ok = mo_value_bytes(heap, got == 0 ? (const uint8_t *)"" : buf, got, &value);
    int number = errno;
    mo_scratch_free(&scratch);
    if (!ok) {
        errno = number;
        return MO_HOST_OUT_OF_MEMORY;
    }
    if (problem.status != FILE_OK)
        return mo_host_file_error(host, heap, type, path, problem, out);
    enum mo_host_outcome outcome = mo_host_file_ok(host, heap, type, value, out);
    uint32_t payload =
        host->types->types.items[type].cases[mo_host_tag(host, type, "Ok")].fields[0].type;
    mo_value_drop(heap, payload, value);
    return outcome;
}

static MO_MUST_USE enum mo_host_outcome
real_write_bytes_or_open(struct mo_host *host, struct mo_heap *heap, uint32_t operation,
                         uint32_t type, const struct mo_cell *args, struct mo_cell *out,
                         struct mo_host_error *error, const char *path,
                         struct mo_file_error problem) {
    const struct mo_capability_info *info = mo_capability_get(args[0]);
    int root_fd = -1;
    if (problem.status == FILE_OK)
        problem = root(host, info, &root_fd);
    if (problem.status == FILE_OK && path[0] == '\0')
        problem = (struct mo_file_error){.status = FILE_FAILED, .reason = "is a folder"};
    struct mo_path_result resolved;
    if (problem.status == FILE_OK)
        problem = mo_host_resolve(host, root_fd, path, PATH_STRICT_PARENT, &resolved);
    if (problem.status != FILE_OK)
        return mo_host_file_error(host, heap, type, args[1], problem, out);
    int parent_fd = resolved.descriptor;
    if (operation == MO_HOST_FILES_OPEN) {
        struct stat st;
        int inspected = fstatat(parent_fd, resolved.name, &st, AT_SYMLINK_NOFOLLOW);
        if (inspected == 0) {
            if (S_ISLNK(st.st_mode))
                problem = (struct mo_file_error){.status = FILE_NOT_ALLOWED, .containment = true};
            else if (S_ISDIR(st.st_mode))
                problem = (struct mo_file_error){.status = FILE_FAILED, .reason = "is a folder"};
            else if (!S_ISREG(st.st_mode))
                problem =
                    (struct mo_file_error){.status = FILE_FAILED, .reason = "not a regular file"};
        } else if (errno != ENOENT)
            problem = mo_host_errno(errno);
        if (problem.status != FILE_OK) {
            mo_host_close(parent_fd);
            return mo_host_file_error(host, heap, type, args[1], problem, out);
        }
        if (host->open_files == MO_LIMIT_OPEN_FILES) {
            mo_host_close(parent_fd);
            return mo_host_bug(error, "too many files open at once");
        }
    }
    int file_fd = -1;
    bool created = false;
    problem =
        open_strict_leaf(parent_fd, resolved.name,
                         operation == MO_HOST_FILES_OPEN ? O_RDWR : O_WRONLY, &file_fd, &created);
    if (problem.status != FILE_OK) {
        mo_host_close(parent_fd);
        return mo_host_file_error(host, heap, type, args[1], problem, out);
    }
    if (operation == MO_HOST_FILES_WRITE_BYTES) {
        problem = write_file(file_fd, args[2], false);
        mo_host_close(file_fd);
        if (problem.status == FILE_OK && created)
            problem = sync_dir(parent_fd);
        mo_host_close(parent_fd);
        return problem.status == FILE_OK
                   ? mo_host_file_ok(host, heap, type, mo_cell_unit(), out)
                   : mo_host_file_error(host, heap, type, args[1], problem, out);
    }
    if (created) {
        problem = sync_dir(parent_fd);
        if (problem.status != FILE_OK) {
            mo_host_close(file_fd);
            mo_host_close(parent_fd);
            return mo_host_file_error(host, heap, type, args[1], problem, out);
        }
    }
    mo_host_close(parent_fd);
    uint32_t file_type =
        host->types->types.items[type].cases[mo_host_tag(host, type, "Ok")].fields[0].type;
    struct mo_capability_info cap_info = {.operations = &mo_host_operations[false][MO_CAP_FILE],
                                          .release = release_open_file,
                                          .has_prefix = true,
                                          .prefix = args[1]};
    struct mo_open_file payload = {.owner = host, .descriptor = file_fd};
    struct mo_cell file_cap;
    if (!mo_value_capability_native(heap, file_type, &cap_info, &payload, sizeof payload,
                                    &file_cap)) {
        int number = errno;
        mo_host_close(file_fd);
        errno = number;
        return MO_HOST_OUT_OF_MEMORY;
    }
    host->open_files++;
    enum mo_host_outcome outcome = mo_host_file_ok(host, heap, type, file_cap, out);
    mo_value_drop(heap, file_type, file_cap);
    return outcome;
}

static MO_MUST_USE enum mo_host_outcome
real_rename(struct mo_host *host, struct mo_heap *heap, uint32_t type, const struct mo_cell *args,
            struct mo_cell *out, const char *from_path, struct mo_file_error from_problem,
            const char *to_path, struct mo_file_error to_problem) {
    if (from_problem.status != FILE_OK)
        return mo_host_file_error(host, heap, type, args[1], from_problem, out);
    if (to_problem.status != FILE_OK)
        return mo_host_file_error(host, heap, type, args[2], to_problem, out);
    const struct mo_capability_info *info = mo_capability_get(args[0]);
    int root_fd = -1;
    struct mo_file_error problem = root(host, info, &root_fd);
    if (problem.status != FILE_OK)
        return mo_host_file_error(host, heap, type, args[1], problem, out);
    if (from_path[0] == '\0')
        return mo_host_file_error(
            host, heap, type, args[1],
            (struct mo_file_error){.status = FILE_FAILED, .reason = "is a folder"}, out);
    if (to_path[0] == '\0')
        return mo_host_file_error(
            host, heap, type, args[2],
            (struct mo_file_error){.status = FILE_FAILED, .reason = "is a folder"}, out);
    struct mo_path_result from_res;
    problem = mo_host_resolve(host, root_fd, from_path, PATH_STRICT_PARENT, &from_res);
    if (problem.status != FILE_OK)
        return mo_host_file_error(host, heap, type, args[1], problem, out);
    struct stat from_st;
    if (fstatat(from_res.descriptor, from_res.name, &from_st, AT_SYMLINK_NOFOLLOW) != 0) {
        problem = mo_host_errno(errno);
        mo_host_close(from_res.descriptor);
        return mo_host_file_error(host, heap, type, args[1], problem, out);
    }
    if (S_ISLNK(from_st.st_mode))
        problem = (struct mo_file_error){.status = FILE_NOT_ALLOWED, .containment = true};
    else if (S_ISDIR(from_st.st_mode))
        problem = (struct mo_file_error){.status = FILE_FAILED, .reason = "is a folder"};
    else if (!S_ISREG(from_st.st_mode))
        problem = (struct mo_file_error){.status = FILE_FAILED, .reason = "not a regular file"};
    if (problem.status != FILE_OK) {
        mo_host_close(from_res.descriptor);
        return mo_host_file_error(host, heap, type, args[1], problem, out);
    }
    struct mo_path_result to_res;
    problem = mo_host_resolve(host, root_fd, to_path, PATH_STRICT_PARENT, &to_res);
    if (problem.status != FILE_OK) {
        mo_host_close(from_res.descriptor);
        return mo_host_file_error(host, heap, type, args[2], problem, out);
    }
    struct stat to_st;
    if (fstatat(to_res.descriptor, to_res.name, &to_st, AT_SYMLINK_NOFOLLOW) == 0) {
        if (S_ISLNK(to_st.st_mode))
            problem = (struct mo_file_error){.status = FILE_NOT_ALLOWED, .containment = true};
        else if (S_ISDIR(to_st.st_mode))
            problem = (struct mo_file_error){.status = FILE_FAILED, .reason = "is a folder"};
        else if (!S_ISREG(to_st.st_mode))
            problem = (struct mo_file_error){.status = FILE_FAILED, .reason = "not a regular file"};
    } else if (errno != ENOENT)
        problem = mo_host_errno(errno);
    if (problem.status == FILE_OK &&
        renameat(from_res.descriptor, from_res.name, to_res.descriptor, to_res.name) != 0)
        problem = mo_host_errno(errno);
    if (problem.status == FILE_OK) {
        problem = sync_dir(to_res.descriptor);
        if (problem.status == FILE_OK) {
            struct stat from_dir_st, to_dir_st;
            if (fstat(from_res.descriptor, &from_dir_st) != 0 ||
                fstat(to_res.descriptor, &to_dir_st) != 0)
                problem = mo_host_errno(errno);
            else if (from_dir_st.st_dev != to_dir_st.st_dev ||
                     from_dir_st.st_ino != to_dir_st.st_ino)
                problem = sync_dir(from_res.descriptor);
        }
    }
    mo_host_close(to_res.descriptor);
    mo_host_close(from_res.descriptor);
    return problem.status == FILE_OK ? mo_host_file_ok(host, heap, type, mo_cell_unit(), out)
                                     : mo_host_file_error(host, heap, type, args[2], problem, out);
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
    if (operation == MO_HOST_FILES_WRITE_BYTES || operation == MO_HOST_FILES_OPEN)
        return real_write_bytes_or_open(host, heap, operation, type, args, out, error, path,
                                        problem);
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
    int number = errno;
    mo_host_close(fd);
    if (outcome == MO_HOST_OUT_OF_MEMORY)
        errno = number;
    return outcome;
}

static MO_MUST_USE bool prepare_real_path(struct mo_cell value, char *local, size_t local_size,
                                          struct mo_scratch *scratch, char **path_out,
                                          struct mo_file_error *problem_out) {
    size_t length;
    const uint8_t *bytes = mo_value_data(value, &length);
    char *path = local;
    if (length >= local_size) {
        path = mo_scratch_resize(scratch, NULL, 0, length + 1);
        if (path == NULL)
            return false;
    }
    struct mo_file_error problem =
        memchr(bytes, '\0', length) != NULL
            ? (struct mo_file_error){.status = FILE_NOT_ALLOWED, .containment = true}
            : mo_host_normalize(value, path, &length);
    if (problem.status != FILE_OK)
        path[0] = '\0';
    *path_out = path;
    *problem_out = problem;
    return true;
}

enum mo_host_outcome mo_host_real_files_call(struct mo_host *host, struct mo_heap *heap,
                                             uint32_t operation, uint32_t type,
                                             const struct mo_cell *args, struct mo_cell *out,
                                             struct mo_host_error *error) {
    if (operation == MO_HOST_FILES_READ_AT || operation == MO_HOST_FILES_WRITE_AT ||
        operation == MO_HOST_FILES_SIZE || operation == MO_HOST_FILES_TRUNCATE ||
        operation == MO_HOST_FILES_SYNC)
        return real_file_op(host, heap, operation, type, args, out, error);
    char local[MO_LIMIT_HOST_PATH_SIZE] = "";
    char *path = local;
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, heap);
    struct mo_file_error problem = {.status = FILE_OK};
    if (operation == MO_HOST_FILES_RENAME) {
        char to_local[MO_LIMIT_HOST_PATH_SIZE] = "";
        char *to_path = to_local;
        struct mo_scratch to_scratch;
        mo_scratch_init(&to_scratch, heap);
        struct mo_file_error to_problem = {.status = FILE_OK};
        enum mo_host_outcome outcome = MO_HOST_OUT_OF_MEMORY;
        if (prepare_real_path(args[1], local, sizeof local, &scratch, &path, &problem) &&
            prepare_real_path(args[2], to_local, sizeof to_local, &to_scratch, &to_path,
                              &to_problem))
            outcome = real_rename(host, heap, type, args, out, path, problem, to_path, to_problem);
        int number = errno;
        mo_scratch_free(&to_scratch);
        mo_scratch_free(&scratch);
        if (outcome == MO_HOST_OUT_OF_MEMORY)
            errno = number;
        return outcome;
    }
    if (operation != MO_HOST_FILES_READ_ONLY &&
        !prepare_real_path(args[1], local, sizeof local, &scratch, &path, &problem)) {
        int number = errno;
        mo_scratch_free(&scratch);
        errno = number;
        return MO_HOST_OUT_OF_MEMORY;
    }
    enum mo_host_outcome outcome =
        call(host, heap, operation, type, args, out, error, path, problem);
    int number = errno;
    mo_scratch_free(&scratch);
    if (outcome == MO_HOST_OUT_OF_MEMORY)
        errno = number;
    return outcome;
}

bool mo_host_open_confined_file(int root_fd, const char *mount_dir, const char *rel_path,
                                bool force_fallback, int *fd_out, uint64_t *size_out,
                                int64_t *mtime_sec_out, int64_t *mtime_nsec_out) {
    MO_ASSERT(root_fd >= 0 && mount_dir != NULL && rel_path != NULL && fd_out != NULL &&
              size_out != NULL && mtime_sec_out != NULL && mtime_nsec_out != NULL);
    *fd_out = -1;
    if (mount_dir[0] == '\0' || rel_path[0] == '\0')
        return false;
    struct mo_host dummy = {.force_fallback = force_fallback};
    struct mo_path_result mount_res;
    struct mo_file_error err = mo_host_resolve(&dummy, root_fd, mount_dir, PATH_FOLDER, &mount_res);
    if (err.status != FILE_OK)
        return false;
    struct mo_path_result file_res;
    err = mo_host_resolve(&dummy, mount_res.descriptor, rel_path, PATH_READ, &file_res);
    mo_host_close(mount_res.descriptor);
    if (err.status != FILE_OK)
        return false;
    if (!S_ISREG(file_res.stat.st_mode) || file_res.stat.st_size < 0) {
        mo_host_close(file_res.descriptor);
        return false;
    }
    *fd_out = file_res.descriptor;
    *size_out = (uint64_t)file_res.stat.st_size;
#if defined(__APPLE__)
    *mtime_sec_out = (int64_t)file_res.stat.st_mtimespec.tv_sec;
    *mtime_nsec_out = (int64_t)file_res.stat.st_mtimespec.tv_nsec;
#else
    *mtime_sec_out = (int64_t)file_res.stat.st_mtim.tv_sec;
    *mtime_nsec_out = (int64_t)file_res.stat.st_mtim.tv_nsec;
#endif
    return true;
}
