#define _POSIX_C_SOURCE 200809L
#include "host/files.h"

#include <errno.h>
#include <string.h>

#ifndef MO_CBMC_NORMALIZE /* cbmc/path.c compiles only mo_host_normalize from this file. */
struct mo_file_error mo_host_errno(int number) {
    if (number == ENOENT)
        return (struct mo_file_error){.status = FILE_NOT_FOUND};
    if (number == EACCES || number == EPERM || number == EXDEV)
        return (struct mo_file_error){.status = FILE_NOT_ALLOWED};
    if (number == ENOTDIR)
        return (struct mo_file_error){.status = FILE_FAILED, .reason = "not a folder"};
    if (number == EISDIR)
        return (struct mo_file_error){.status = FILE_FAILED, .reason = "is a folder"};
    if (number == ELOOP)
        return (struct mo_file_error){.status = FILE_FAILED, .reason = "too many links"};
    return (struct mo_file_error){.status = FILE_FAILED, .number = number};
}
#endif

struct mo_file_error mo_host_normalize(struct mo_cell value, char *out, size_t *size) {
    size_t length;
    const uint8_t *bytes = mo_value_data(value, &length);
    if (length == 0)
        return (struct mo_file_error){.status = FILE_NOT_ALLOWED, .containment = true};
    size_t written = 0, at = 0;
    while (at < length) {
        size_t end = at;
        while (end < length && bytes[end] != '/')
            end++;
        size_t n = end - at;
        if (n == 0 || (end == length - 1 && bytes[end] == '/'))
            return (struct mo_file_error){.status = FILE_NOT_ALLOWED, .containment = true};
        if (n == 2 && bytes[at] == '.' && bytes[at + 1] == '.') {
            if (written == 0)
                return (struct mo_file_error){.status = FILE_NOT_ALLOWED, .containment = true};
            while (written != 0 && out[written - 1] != '/')
                written--;
            if (written != 0)
                written--;
        } else if (!(n == 1 && bytes[at] == '.')) {
            if (written != 0)
                out[written++] = '/';
            memcpy(out + written, bytes + at, n);
            written += n;
        }
        at = end + 1;
    }
    out[written] = '\0';
    *size = written;
    return (struct mo_file_error){.status = FILE_OK};
}

#ifndef MO_CBMC_NORMALIZE
size_t mo_host_join(struct mo_cell prefix, const char *path, size_t length, char *out) {
    size_t root_length;
    const uint8_t *root = mo_value_data(prefix, &root_length);
    size_t separator = root_length != 0 && length != 0 ? 1 : 0;
    memcpy(out, root, root_length);
    if (separator != 0)
        out[root_length] = '/';
    memcpy(out + root_length + separator, path, length + 1);
    return root_length + separator + length;
}

enum mo_host_outcome mo_host_file_error(struct mo_host *host, struct mo_heap *heap, uint32_t type,
                                        struct mo_cell path, struct mo_file_error error,
                                        struct mo_cell *out) {
    uint32_t error_tag = mo_host_tag(host, type, "Error");
    uint32_t error_type = host->types->types.items[type].cases[error_tag].fields[0].type;
    const char *name;
    struct mo_cell fields[] = {path, mo_cell_unit()};
    uint32_t count = 1;
    switch (error.status) {
    case FILE_NOT_FOUND:
        name = "NotFound";
        break;
    case FILE_NOT_ALLOWED:
        name = "NotAllowed";
        break;
    case FILE_NOT_TEXT:
        name = "NotText";
        break;
    case FILE_FAILED: {
        char system[256];
        const char *reason = error.reason;
        if (reason == NULL) {
            reason = strerror_r(error.number, system, sizeof system) == 0 ? system : "system error";
        }
        if (!mo_value_text(heap, reason, strlen(reason), &fields[1]))
            return MO_HOST_OUT_OF_MEMORY;
        name = "Failed";
        count = 2;
        break;
    }
    case FILE_OK:
    default:
        MO_UNREACHABLE();
    }
    struct mo_cell value;
    bool built = mo_host_case(host, heap, error_type, name, fields, count, &value);
    if (count == 2)
        mo_value_drop(heap, host->types->text, fields[1]);
    if (!built)
        return MO_HOST_OUT_OF_MEMORY;
    bool wrapped = mo_host_case(host, heap, type, "Error", &value, 1, out);
    mo_value_drop(heap, error_type, value);
    return wrapped ? MO_HOST_RETURNED : MO_HOST_OUT_OF_MEMORY;
}

enum mo_host_outcome mo_host_file_ok(struct mo_host *host, struct mo_heap *heap, uint32_t type,
                                     struct mo_cell value, struct mo_cell *out) {
    return mo_host_case(host, heap, type, "Ok", &value, 1, out) ? MO_HOST_RETURNED
                                                                : MO_HOST_OUT_OF_MEMORY;
}

struct name_entry {
    struct mo_cell name;
    uint8_t kind;
};

static MO_MUST_USE int compare(struct mo_cell a, struct mo_cell b) {
    size_t na, nb;
    const uint8_t *pa = mo_value_data(a, &na), *pb = mo_value_data(b, &nb);
    int order = memcmp(pa, pb, na < nb ? na : nb);
    return order != 0 ? order : (na > nb) - (na < nb);
}

static void sift(struct name_entry *entries, uint32_t root, uint32_t count) {
    while (root < count / 2) {
        uint32_t child = 2 * root + 1;
        if (child + 1 < count && compare(entries[child].name, entries[child + 1].name) < 0)
            child++;
        if (compare(entries[root].name, entries[child].name) >= 0)
            break;
        struct name_entry temporary = entries[root];
        entries[root] = entries[child];
        entries[child] = temporary;
        root = child;
    }
}

bool mo_host_names(struct mo_host *host, struct mo_heap *heap, uint32_t type, struct mo_cell names,
                   const uint8_t *kinds, struct mo_cell *out) {
    uint32_t count = mo_list_size(names);
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, heap);
    struct name_entry *entries =
        count == 0 ? NULL : mo_scratch_resize(&scratch, NULL, 0, (size_t)count * sizeof *entries);
    if (count != 0 && entries == NULL) {
        mo_scratch_free(&scratch);
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        MO_ASSERT(mo_list_get(names, i, &entries[i].name));
        entries[i].kind = kinds == NULL ? 0 : kinds[i];
    }
    for (uint32_t i = count / 2; i != 0; i--)
        sift(entries, i - 1, count);
    for (uint32_t i = count; i > 1; i--) {
        struct name_entry temporary = entries[0];
        entries[0] = entries[i - 1];
        entries[i - 1] = temporary;
        sift(entries, 0, i - 1);
    }
    uint32_t element_type = host->types->types.items[type].parameters[0];
    bool records = host->types->types.items[element_type].kind == MO_TYPE_RECORD;
    uint32_t layout = MO_NO_INDEX;
    if (records) {
        for (uint32_t i = 0; i < host->types->types.layout_count; i++) {
            const struct mo_layout *candidate = &host->types->types.layouts[i];
            if (candidate->kind == MO_LAYOUT_RECORD && candidate->type == element_type) {
                layout = i;
                break;
            }
        }
        MO_ASSERT(layout != MO_NO_INDEX);
    }
    struct mo_cell list;
    bool ok = mo_value_list(heap, type, NULL, 0, &list);
    if (ok) {
        for (uint32_t i = 0; i < count; i++) {
            struct mo_cell value = entries[i].name;
            if (records) {
                static const char *const names_[] = {"File", "Folder", "Link", "Other"};
                uint32_t kind_type = host->types->types.items[element_type].fields[1].type;
                struct mo_cell kind;
                ok = mo_host_case(host, heap, kind_type, names_[entries[i].kind], NULL, 0, &kind);
                if (ok) {
                    struct mo_cell fields[] = {value, kind};
                    ok = mo_value_construct(heap, layout, fields, 2, &value);
                    mo_value_drop(heap, kind_type, kind);
                }
                if (!ok)
                    break;
            }
            ok = mo_list_append(heap, type, &list, value);
            if (records)
                mo_value_drop(heap, element_type, value);
            if (!ok)
                break;
        }
        if (ok)
            *out = list;
        else
            mo_value_drop(heap, type, list);
    }
    mo_scratch_free(&scratch);
    return ok;
}
#endif
