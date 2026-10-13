#ifndef MO_HOST_FILES_H
#define MO_HOST_FILES_H

#include "host/internal.h"

#include <sys/stat.h>

enum mo_file_status { FILE_OK, FILE_NOT_FOUND, FILE_NOT_ALLOWED, FILE_NOT_TEXT, FILE_FAILED };
struct mo_file_error {
    enum mo_file_status status;
    const char *reason;
    int number;
    bool containment; /* Only lexical/link escapes permanently forbid within. */
};
struct mo_folder {
    struct mo_host *owner;
    int descriptor;
    bool pending, forbidden;
};
enum mo_path_mode {
    PATH_READ,
    PATH_WRITE,
    PATH_APPEND,
    PATH_FOLDER,
    PATH_MAKE_FOLDER,
    PATH_REMOVE,
    PATH_EXISTS
};
struct mo_path_result {
    int descriptor;
    char name[MO_LIMIT_HOST_PATH_SIZE]; /* PATH_REMOVE / portable PATH_EXISTS: final component */
    struct stat stat;
};
MO_MUST_USE struct mo_file_error mo_host_errno(int number);
/* out holds value's byte length plus NUL, or prefix length + path length + 2.
 * Fake paths have no OS pathname bound. Callers charge this dynamic storage. */
MO_MUST_USE struct mo_file_error mo_host_normalize(struct mo_cell value, char *out, size_t *length);
MO_MUST_USE size_t mo_host_join(struct mo_cell prefix, const char *path, size_t length, char *out);
MO_MUST_USE struct mo_file_error mo_host_resolve(struct mo_host *host, int root, const char *path,
                                                 enum mo_path_mode mode,
                                                 struct mo_path_result *out);
void mo_host_close(int descriptor);
MO_MUST_USE enum mo_host_outcome
mo_host_folder_cap(struct mo_host *host, struct mo_heap *heap, uint32_t type, int descriptor,
                   const struct mo_cell *prefix, struct mo_cell *out, struct mo_host_error *error);
MO_MUST_USE enum mo_host_outcome mo_host_file_error(struct mo_host *host, struct mo_heap *heap,
                                                    uint32_t type, struct mo_cell path,
                                                    struct mo_file_error error,
                                                    struct mo_cell *out);
MO_MUST_USE enum mo_host_outcome mo_host_file_ok(struct mo_host *host, struct mo_heap *heap,
                                                 uint32_t type, struct mo_cell value,
                                                 struct mo_cell *out);
MO_MUST_USE enum mo_host_outcome mo_host_files_fake(struct mo_host *host, struct mo_heap *heap,
                                                    uint32_t type, struct mo_cell files,
                                                    struct mo_cell *out,
                                                    struct mo_host_error *error);
MO_MUST_USE enum mo_host_outcome mo_host_fake_files_call(struct mo_host *host, struct mo_heap *heap,
                                                         uint32_t operation, uint32_t type,
                                                         const struct mo_cell *args,
                                                         struct mo_cell *out,
                                                         struct mo_host_error *error);
MO_MUST_USE enum mo_host_outcome mo_host_real_files_call(struct mo_host *host, struct mo_heap *heap,
                                                         uint32_t operation, uint32_t type,
                                                         const struct mo_cell *args,
                                                         struct mo_cell *out,
                                                         struct mo_host_error *error);
MO_MUST_USE bool mo_host_names(struct mo_host *host, struct mo_heap *heap, uint32_t type,
                               struct mo_cell names, const uint8_t *kinds, struct mo_cell *out);

#endif
