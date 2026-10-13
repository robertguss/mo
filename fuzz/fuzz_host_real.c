#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#include "../tests/host_fixture.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* The directory skeleton never changes. Only regular leaves can be created;
 * unlink links themselves while cleaning, never follow fuzz-generated paths. */
static void clean(int fd) {
    int copy = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    MO_ASSERT(copy >= 0);
    DIR *directory = fdopendir(copy);
    MO_ASSERT(directory != NULL);
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (entry == NULL) {
            MO_ASSERT(errno == 0);
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        struct stat st;
        MO_ASSERT(fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0);
        if (!S_ISDIR(st.st_mode))
            MO_ASSERT(unlinkat(fd, entry->d_name, 0) == 0);
    }
    MO_ASSERT(closedir(directory) == 0);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 4)
        return 0;
    char temporary[] = "/tmp/mo-host-fuzz-XXXXXX";
    MO_ASSERT(mkdtemp(temporary) != NULL);
    int base = open(temporary, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    MO_ASSERT(base >= 0 && mkdirat(base, "root", 0700) == 0 && mkdirat(base, "outside", 0700) == 0);
    int root = openat(base, "root", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int outside = openat(base, "outside", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    MO_ASSERT(root >= 0 && outside >= 0);
    int sentinel = openat(outside, "sentinel", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    MO_ASSERT(
        sentinel >= 0 && write(sentinel, "outside", 7) == 7 && mkdirat(root, "d", 0700) == 0 &&
        mkdirat(root, "d/deep", 0700) == 0 && symlinkat("../outside", root, "escape") == 0 &&
        symlinkat("/etc/passwd", root, "absolute") == 0 &&
        symlinkat("deep/../../file", root, "d/up") == 0 &&
        symlinkat("../up", root, "d/deep/up") == 0 && symlinkat("missing/", root, "slash") == 0 &&
        symlinkat("loop", root, "loop") == 0 && mkfifoat(root, "fifo", 0600) == 0);
    struct stat original;
    MO_ASSERT(fstat(sentinel, &original) == 0);
    struct host_fixture f;
    host_fixture_init(&f, 16384 + (size_t)data[0] * 1024);
    f.host.force_fallback = (data[1] & 1) != 0;
    struct mo_cell caps[4];
    uint32_t cap_type = f.capabilities[MO_CAP_FILES];
    struct mo_host_error error;
    MO_ASSERT(mo_host_real(&f.host, f.heap, cap_type, NULL, root, &caps[0], &error) ==
              MO_HOST_RETURNED);
    for (unsigned i = 1; i < 4; i++)
        caps[i] = mo_value_dup(f.heap, cap_type, caps[0]);
    static const uint32_t ops[] = {MO_HOST_FILES_READ_TEXT,  MO_HOST_FILES_READ_BYTES,
                                   MO_HOST_FILES_WRITE_TEXT, MO_HOST_FILES_APPEND_TEXT,
                                   MO_HOST_FILES_REMOVE,     MO_HOST_FILES_LIST,
                                   MO_HOST_FILES_ENTRIES,    MO_HOST_FILES_EXISTS,
                                   MO_HOST_FILES_WITHIN,     MO_HOST_FILES_READ_ONLY};
    static const char *const paths[] = {".",
                                        "file",
                                        "d/file",
                                        "d/up",
                                        "d/deep/up",
                                        "escape/sentinel",
                                        "absolute",
                                        "slash",
                                        "loop",
                                        "fifo",
                                        "../outside/sentinel",
                                        "d/deep/../../../outside/sentinel",
                                        "d/../file",
                                        "d/deep",
                                        "d",
                                        "missing/file"};
    for (size_t at = 2; at + 3 < size && at < 128; at += 4) {
        uint32_t op = ops[data[at] % (sizeof ops / sizeof ops[0])];
        unsigned source = data[at + 1] % 4u, destination = data[at + 2] % 4u;
        char path[256];
        size_t length;
        if ((data[at + 3] & 1) != 0) {
            const char *selected = paths[(data[at + 3] >> 1) % (sizeof paths / sizeof paths[0])];
            length = strlen(selected);
            memcpy(path, selected, length);
        } else {
            static const char letters[] = "ad./\0abdefilpsotwxy";
            length = size - at < sizeof path ? size - at : sizeof path;
            for (size_t i = 0; i < length; i++)
                path[i] = letters[data[at + i] % (sizeof letters - 1)];
        }
        struct mo_cell args[] = {caps[source], mo_cell_unit(), mo_cell_unit()};
        if (!mo_value_text(f.heap, path, length, &args[1]))
            continue;
        if (!mo_value_text(f.heap, "changed", 7, &args[2])) {
            mo_value_drop(f.heap, f.types.text, args[1]);
            continue;
        }
        struct mo_scratch pressure;
        mo_scratch_init(&pressure, f.heap);
        if ((data[at + 1] & 8) != 0) {
            size_t used = mo_heap_used(f.heap), limit = 16384 + (size_t)data[0] * 1024;
            if (limit > used + 128) {
                void *held = mo_scratch_resize(&pressure, NULL, 0, limit - used - 128);
                if (held != NULL)
                    MO_ASSERT(mo_heap_used(f.heap) > used);
            }
        }
        struct mo_cell out = mo_cell_int(19);
        uint32_t type = host_result_type(&f, op);
        enum mo_host_outcome outcome = mo_host_call(&f.host, f.heap, op, type, args, &out, &error);
        mo_scratch_free(&pressure);
        if (outcome == MO_HOST_RETURNED) {
            if (op == MO_HOST_FILES_WITHIN) {
                mo_value_drop(f.heap, cap_type, caps[destination]);
                caps[destination] = out;
            } else
                mo_value_drop(f.heap, type, out);
        } else
            MO_ASSERT(mo_cell_get_int(out) == 19);
        mo_value_drop(f.heap, f.types.text, args[1]);
        mo_value_drop(f.heap, f.types.text, args[2]);
        char kept[8];
        struct stat st;
        MO_ASSERT(pread(sentinel, kept, sizeof kept, 0) == 7 && memcmp(kept, "outside", 7) == 0 &&
                  fstatat(outside, "sentinel", &st, AT_SYMLINK_NOFOLLOW) == 0 &&
                  S_ISREG(st.st_mode) && st.st_dev == original.st_dev &&
                  st.st_ino == original.st_ino);
    }
    for (unsigned i = 0; i < 4; i++)
        mo_value_drop(f.heap, cap_type, caps[i]);
    MO_ASSERT(mo_heap_objects(f.heap) == 0 && f.host.open_folders == 0);
    host_fixture_free(&f);
    int d = openat(root, "d", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int deep = openat(d, "deep", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    MO_ASSERT(d >= 0 && deep >= 0);
    clean(deep);
    clean(d);
    clean(root);
    MO_ASSERT(close(deep) == 0 && unlinkat(d, "deep", AT_REMOVEDIR) == 0 && close(d) == 0 &&
              unlinkat(root, "d", AT_REMOVEDIR) == 0 && close(root) == 0 &&
              unlinkat(base, "root", AT_REMOVEDIR) == 0 && close(sentinel) == 0 &&
              unlinkat(outside, "sentinel", 0) == 0 && close(outside) == 0 &&
              unlinkat(base, "outside", AT_REMOVEDIR) == 0 && close(base) == 0 &&
              rmdir(temporary) == 0);
    return 0;
}
