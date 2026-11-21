#define _XOPEN_SOURCE 700
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#include "host/command.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "host/files.h"

struct mo_command_root {
    struct mo_host *owner;
    int descriptor;
    bool pending;
    bool forbidden;
};

static void release_command_root(intptr_t handle) {
    struct mo_command_root *root = (struct mo_command_root *)handle;
    mo_host_close(root->descriptor);
    MO_ASSERT(root->owner->open_folders != 0);
    root->owner->open_folders--;
}

static MO_MUST_USE bool valid_program_name(const uint8_t *bytes, size_t length) {
    if (length == 0 || length > 255)
        return false;
    if ((length == 1 && bytes[0] == '.') || (length == 2 && bytes[0] == '.' && bytes[1] == '.'))
        return false;
    for (size_t i = 0; i < length; i++) {
        uint8_t ch = bytes[i];
        if (ch <= 0x20 || ch >= 0x7f || ch == '/' || ch == '\\')
            return false;
    }
    return true;
}

static MO_MUST_USE bool list_contains_text(struct mo_cell list, const uint8_t *bytes,
                                           size_t length) {
    uint32_t count = mo_list_size(list);
    for (uint32_t i = 0; i < count; i++) {
        struct mo_cell item;
        MO_ASSERT(mo_list_get(list, i, &item));
        size_t item_len;
        const uint8_t *item_bytes = mo_value_data(item, &item_len);
        if (item_len == length && memcmp(item_bytes, bytes, length) == 0)
            return true;
    }
    return false;
}

static MO_MUST_USE bool append_unique_program(struct mo_heap *heap, uint32_t texts_type,
                                              uint32_t text_type, struct mo_cell *allowed,
                                              const uint8_t *bytes, size_t length) {
    if (list_contains_text(*allowed, bytes, length))
        return true;
    struct mo_cell entry;
    if (!mo_value_text(heap, (const char *)bytes, length, &entry))
        return false;
    bool ok = mo_list_append(heap, texts_type, allowed, entry);
    mo_value_drop(heap, text_type, entry);
    return ok;
}

enum mo_host_outcome mo_host_command_real(struct mo_host *host, struct mo_heap *heap, uint32_t type,
                                          int folder, struct mo_cell *out,
                                          struct mo_host_error *error) {
    MO_ASSERT(folder >= 0);
    if (host->open_folders == MO_LIMIT_OPEN_FOLDERS)
        return mo_host_bug(error, "too many folders open at once");
    int dup_fd = fcntl(folder, F_DUPFD_CLOEXEC, 0);
    if (dup_fd < 0)
        return mo_host_bug(error, "too many folders open at once");
    struct mo_capability_info info = {
        .operations = &mo_host_operations[false][MO_CAP_COMMAND],
        .release = release_command_root,
    };
    struct mo_command_root payload = {.owner = host, .descriptor = dup_fd};
    if (!mo_value_capability_native(heap, type, &info, &payload, sizeof payload, out)) {
        mo_host_close(dup_fd);
        return MO_HOST_OUT_OF_MEMORY;
    }
    host->open_folders++;
    return MO_HOST_RETURNED;
}

static MO_MUST_USE enum mo_host_outcome
make_real_command_cap(struct mo_host *host, struct mo_heap *heap, uint32_t type, int fd,
                      struct mo_state *state, const struct mo_cell *prefix, bool pending,
                      bool forbidden, struct mo_cell *out, struct mo_host_error *error) {
    if (host->open_folders == MO_LIMIT_OPEN_FOLDERS)
        return mo_host_bug(error, "too many folders open at once");
    struct mo_capability_info info = {
        .operations = &mo_host_operations[false][MO_CAP_COMMAND],
        .state = state,
        .release = release_command_root,
        .has_prefix = prefix != NULL,
    };
    if (prefix != NULL)
        info.prefix = *prefix;
    struct mo_command_root payload = {
        .owner = host, .descriptor = fd, .pending = pending, .forbidden = forbidden};
    if (!mo_value_capability_native(heap, type, &info, &payload, sizeof payload, out))
        return MO_HOST_OUT_OF_MEMORY;
    host->open_folders++;
    return MO_HOST_RETURNED;
}

static MO_MUST_USE enum mo_host_outcome command_error_tag(struct mo_host *host,
                                                          struct mo_heap *heap,
                                                          uint32_t result_type, const char *prefix,
                                                          const uint8_t *detail, size_t detail_len,
                                                          struct mo_cell *out) {
    size_t pre_len = strlen(prefix);
    if (pre_len > MO_LIMIT_VALUE_BYTES || detail_len > (size_t)MO_LIMIT_VALUE_BYTES - pre_len)
        return MO_HOST_OUT_OF_MEMORY;
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, heap);
    size_t total = pre_len + detail_len;
    char *buf = total == 0 ? NULL : mo_scratch_resize(&scratch, NULL, 0, total);
    if (total != 0 && buf == NULL) {
        mo_scratch_free(&scratch);
        return MO_HOST_OUT_OF_MEMORY;
    }
    if (pre_len != 0)
        memcpy(buf, prefix, pre_len);
    if (detail_len != 0)
        memcpy(buf + pre_len, detail, detail_len);
    struct mo_cell msg;
    bool ok = mo_value_text(heap, total == 0 ? "" : buf, total, &msg);
    mo_scratch_free(&scratch);
    if (!ok)
        return MO_HOST_OUT_OF_MEMORY;
    bool wrapped = mo_host_case(host, heap, result_type, "Error", &msg, 1, out);
    mo_value_drop(heap, host->types->text, msg);
    return wrapped ? MO_HOST_RETURNED : MO_HOST_OUT_OF_MEMORY;
}

static MO_MUST_USE enum mo_host_outcome
command_ok_tuple(struct mo_host *host, struct mo_heap *heap, uint32_t result_type, int64_t status,
                 const uint8_t *stdout_bytes, size_t stdout_len, const uint8_t *stderr_bytes,
                 size_t stderr_len, struct mo_cell *out) {
    uint32_t ok_tag = mo_host_tag(host, result_type, "Ok");
    uint32_t tuple_type = host->types->types.items[result_type].cases[ok_tag].fields[0].type;
    struct mo_cell out_b, err_b;
    if (!mo_value_bytes(heap, stdout_bytes, stdout_len, &out_b))
        return MO_HOST_OUT_OF_MEMORY;
    if (!mo_value_bytes(heap, stderr_bytes, stderr_len, &err_b)) {
        mo_value_drop(heap, host->types->bytes, out_b);
        return MO_HOST_OUT_OF_MEMORY;
    }
    struct mo_cell fields[3] = {mo_cell_int(status), out_b, err_b};
    struct mo_cell tup;
    bool built = mo_value_product(heap, tuple_type, fields, 3, &tup);
    mo_value_drop(heap, host->types->bytes, out_b);
    mo_value_drop(heap, host->types->bytes, err_b);
    if (!built)
        return MO_HOST_OUT_OF_MEMORY;
    bool wrapped = mo_host_case(host, heap, result_type, "Ok", &tup, 1, out);
    mo_value_drop(heap, tuple_type, tup);
    return wrapped ? MO_HOST_RETURNED : MO_HOST_OUT_OF_MEMORY;
}

static MO_MUST_USE enum mo_host_outcome command_allow_programs(struct mo_host *host,
                                                               struct mo_heap *heap, uint32_t type,
                                                               const struct mo_cell *args,
                                                               struct mo_cell *out,
                                                               struct mo_host_error *error) {
    const struct mo_capability_info *info = mo_capability_get(args[0]);
    bool fake = ((const struct mo_host_operations *)info->operations)->fake;
    bool has_existing = mo_state_length(info->state) > (fake ? 1u : 0u);
    struct mo_cell existing =
        has_existing ? mo_state_get(info->state, fake ? 1u : 0u) : mo_cell_unit();
    struct mo_cell allowed;
    if (!mo_value_list(heap, host->types->texts, NULL, 0, &allowed))
        return MO_HOST_OUT_OF_MEMORY;
    uint32_t requested = mo_list_size(args[1]);
    for (uint32_t i = 0; i < requested; i++) {
        struct mo_cell item;
        MO_ASSERT(mo_list_get(args[1], i, &item));
        size_t len;
        const uint8_t *bytes = mo_value_data(item, &len);
        if (!valid_program_name(bytes, len))
            continue;
        if (has_existing && !list_contains_text(existing, bytes, len))
            continue;
        if (!append_unique_program(heap, host->types->texts, host->types->text, &allowed, bytes,
                                   len)) {
            mo_value_drop(heap, host->types->texts, allowed);
            return MO_HOST_OUT_OF_MEMORY;
        }
    }
    if (fake) {
        uint32_t slot_types[] = {host->types->replies, host->types->texts};
        struct mo_cell slot_values[] = {mo_state_get(info->state, 0), allowed};
        struct mo_state *state;
        bool ok = mo_state_new(heap, slot_types, slot_values, 2, &state);
        mo_value_drop(heap, host->types->texts, allowed);
        if (!ok)
            return MO_HOST_OUT_OF_MEMORY;
        struct mo_capability_info copy = *info;
        copy.state = state;
        ok = mo_value_capability(heap, type, &copy, out);
        mo_state_drop(heap, state);
        return ok ? MO_HOST_RETURNED : MO_HOST_OUT_OF_MEMORY;
    }
    const struct mo_command_root *orig = (const struct mo_command_root *)info->handle;
    if (host->open_folders == MO_LIMIT_OPEN_FOLDERS) {
        mo_value_drop(heap, host->types->texts, allowed);
        return mo_host_bug(error, "too many folders open at once");
    }
    int dup_fd = fcntl(orig->descriptor, F_DUPFD_CLOEXEC, 0);
    if (dup_fd < 0) {
        mo_value_drop(heap, host->types->texts, allowed);
        return mo_host_bug(error, "too many folders open at once");
    }
    struct mo_state *state;
    bool ok = mo_state_new(heap, &host->types->texts, &allowed, 1, &state);
    mo_value_drop(heap, host->types->texts, allowed);
    if (!ok) {
        mo_host_close(dup_fd);
        return MO_HOST_OUT_OF_MEMORY;
    }
    enum mo_host_outcome res = make_real_command_cap(host, heap, type, dup_fd, state,
                                                     info->has_prefix ? &info->prefix : NULL,
                                                     orig->pending, orig->forbidden, out, error);
    mo_state_drop(heap, state);
    if (res != MO_HOST_RETURNED)
        mo_host_close(dup_fd);
    return res;
}

static MO_MUST_USE enum mo_host_outcome command_within_fake(struct mo_host *host,
                                                            struct mo_heap *heap, uint32_t type,
                                                            const struct mo_cell *args,
                                                            struct mo_cell *out) {
    const struct mo_capability_info *info = mo_capability_get(args[0]);
    if (info->read_only) {
        /* Already forbidden: keep the first escaped path. */
        struct mo_capability_info copy = *info;
        return mo_value_capability(heap, type, &copy, out) ? MO_HOST_RETURNED
                                                           : MO_HOST_OUT_OF_MEMORY;
    }
    size_t raw_len;
    const uint8_t *raw_ptr = mo_value_data(args[1], &raw_len);
    (void)raw_ptr;
    size_t root_len = 0;
    if (info->has_prefix) {
        const uint8_t *root_ptr = mo_value_data(info->prefix, &root_len);
        (void)root_ptr;
    }
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, heap);
    char *norm = mo_scratch_resize(&scratch, NULL, 0, raw_len + 1);
    char *joined = mo_scratch_resize(&scratch, NULL, 0, root_len + raw_len + 2);
    if (norm == NULL || joined == NULL) {
        mo_scratch_free(&scratch);
        return MO_HOST_OUT_OF_MEMORY;
    }
    size_t norm_len = 0;
    struct mo_file_error problem = mo_host_normalize(args[1], norm, &norm_len);
    if (problem.status != FILE_OK) {
        mo_scratch_free(&scratch);
        struct mo_capability_info copy = *info;
        copy.has_prefix = true;
        copy.prefix = args[1];
        copy.read_only = true;
        return mo_value_capability(heap, type, &copy, out) ? MO_HOST_RETURNED
                                                           : MO_HOST_OUT_OF_MEMORY;
    }
    size_t joined_len = norm_len;
    if (info->has_prefix && root_len != 0) {
        joined_len = mo_host_join(info->prefix, norm, norm_len, joined);
    } else {
        memcpy(joined, norm, norm_len + 1);
    }
    struct mo_cell prefix_cell = mo_cell_unit();
    bool has_prefix = joined_len != 0;
    if (has_prefix && !mo_value_text(heap, joined, joined_len, &prefix_cell)) {
        mo_scratch_free(&scratch);
        return MO_HOST_OUT_OF_MEMORY;
    }
    mo_scratch_free(&scratch);
    struct mo_capability_info copy = *info;
    copy.has_prefix = has_prefix;
    copy.prefix = prefix_cell;
    copy.read_only = false;
    bool ok = mo_value_capability(heap, type, &copy, out);
    if (has_prefix)
        mo_value_drop(heap, host->types->text, prefix_cell);
    return ok ? MO_HOST_RETURNED : MO_HOST_OUT_OF_MEMORY;
}

static MO_MUST_USE enum mo_host_outcome
command_within_real(struct mo_host *host, struct mo_heap *heap, uint32_t type,
                    const struct mo_cell *args, struct mo_cell *out, struct mo_host_error *error) {
    if (host->open_folders == MO_LIMIT_OPEN_FOLDERS)
        return mo_host_bug(error, "too many folders open at once");
    const struct mo_capability_info *info = mo_capability_get(args[0]);
    const struct mo_command_root *orig = (const struct mo_command_root *)info->handle;
    if (orig->forbidden) {
        int dup_fd = fcntl(orig->descriptor, F_DUPFD_CLOEXEC, 0);
        if (dup_fd < 0)
            return mo_host_bug(error, "too many folders open at once");
        enum mo_host_outcome res =
            make_real_command_cap(host, heap, type, dup_fd, info->state,
                                  info->has_prefix ? &info->prefix : NULL, false, true, out, error);
        if (res != MO_HOST_RETURNED)
            mo_host_close(dup_fd);
        return res;
    }
    size_t raw_len;
    const uint8_t *raw_ptr = mo_value_data(args[1], &raw_len);
    (void)raw_ptr;
    size_t prev_len = 0;
    if (orig->pending && info->has_prefix) {
        const uint8_t *prev_ptr = mo_value_data(info->prefix, &prev_len);
        (void)prev_ptr;
    }
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, heap);
    char *norm = mo_scratch_resize(&scratch, NULL, 0, raw_len + 1);
    char *joined = mo_scratch_resize(&scratch, NULL, 0, prev_len + raw_len + 2);
    if (norm == NULL || joined == NULL) {
        mo_scratch_free(&scratch);
        return MO_HOST_OUT_OF_MEMORY;
    }
    size_t norm_len = 0;
    struct mo_file_error problem = mo_host_normalize(args[1], norm, &norm_len);
    if (problem.status != FILE_OK) {
        mo_scratch_free(&scratch);
        int dup_fd = fcntl(orig->descriptor, F_DUPFD_CLOEXEC, 0);
        if (dup_fd < 0)
            return mo_host_bug(error, "too many folders open at once");
        enum mo_host_outcome res = make_real_command_cap(host, heap, type, dup_fd, info->state,
                                                         &args[1], false, true, out, error);
        if (res != MO_HOST_RETURNED)
            mo_host_close(dup_fd);
        return res;
    }
    size_t joined_len = norm_len;
    if (orig->pending && info->has_prefix && prev_len != 0)
        joined_len = mo_host_join(info->prefix, norm, norm_len, joined);
    else
        memcpy(joined, norm, norm_len + 1);

    if (joined_len == 0) {
        mo_scratch_free(&scratch);
        int dup_fd = fcntl(orig->descriptor, F_DUPFD_CLOEXEC, 0);
        if (dup_fd < 0)
            return mo_host_bug(error, "too many folders open at once");
        enum mo_host_outcome res = make_real_command_cap(host, heap, type, dup_fd, info->state,
                                                         NULL, false, false, out, error);
        if (res != MO_HOST_RETURNED)
            mo_host_close(dup_fd);
        return res;
    }
    if (joined_len < MO_LIMIT_HOST_PATH_SIZE) {
        struct mo_path_result resolved;
        struct mo_file_error rerr =
            mo_host_resolve(host, orig->descriptor, joined, PATH_FOLDER, &resolved);
        if (rerr.status == FILE_OK) {
            mo_scratch_free(&scratch);
            enum mo_host_outcome res = make_real_command_cap(
                host, heap, type, resolved.descriptor, info->state, NULL, false, false, out, error);
            if (res != MO_HOST_RETURNED)
                mo_host_close(resolved.descriptor);
            return res;
        }
        if (rerr.containment) {
            mo_scratch_free(&scratch);
            int dup_fd = fcntl(orig->descriptor, F_DUPFD_CLOEXEC, 0);
            if (dup_fd < 0)
                return mo_host_bug(error, "too many folders open at once");
            enum mo_host_outcome res = make_real_command_cap(host, heap, type, dup_fd, info->state,
                                                             &args[1], false, true, out, error);
            if (res != MO_HOST_RETURNED)
                mo_host_close(dup_fd);
            return res;
        }
    }
    struct mo_cell pending_prefix;
    if (!mo_value_text(heap, joined, joined_len, &pending_prefix)) {
        mo_scratch_free(&scratch);
        return MO_HOST_OUT_OF_MEMORY;
    }
    mo_scratch_free(&scratch);
    int dup_fd = fcntl(orig->descriptor, F_DUPFD_CLOEXEC, 0);
    if (dup_fd < 0) {
        mo_value_drop(heap, host->types->text, pending_prefix);
        return mo_host_bug(error, "too many folders open at once");
    }
    enum mo_host_outcome res = make_real_command_cap(host, heap, type, dup_fd, info->state,
                                                     &pending_prefix, true, false, out, error);
    mo_value_drop(heap, host->types->text, pending_prefix);
    if (res != MO_HOST_RETURNED)
        mo_host_close(dup_fd);
    return res;
}

static MO_MUST_USE size_t append_decimal(char *dst, uint64_t val) {
    char rev[24];
    size_t n = 0;
    do {
        rev[n++] = (char)('0' + (val % 10u));
        val /= 10u;
    } while (val != 0);
    for (size_t i = 0; i < n; i++)
        dst[i] = rev[n - 1 - i];
    return n;
}

static MO_MUST_USE bool parse_u64_until(const uint8_t *bytes, size_t len, size_t *pos, uint8_t sep,
                                        uint64_t *out) {
    size_t start = *pos;
    uint64_t acc = 0;
    while (*pos < len && bytes[*pos] != sep) {
        uint8_t c = bytes[*pos];
        if (c < '0' || c > '9')
            return false;
        uint64_t d = (uint64_t)(c - '0');
        if (acc > (UINT64_MAX - d) / 10u)
            return false;
        acc = acc * 10u + d;
        (*pos)++;
    }
    if (*pos == start || *pos >= len || bytes[*pos] != sep)
        return false;
    (*pos)++;
    *out = acc;
    return true;
}

static MO_MUST_USE bool parse_i64_until(const uint8_t *bytes, size_t len, size_t *pos, uint8_t sep,
                                        int64_t *out) {
    if (*pos >= len)
        return false;
    bool neg = bytes[*pos] == '-';
    if (neg)
        (*pos)++;
    uint64_t mag = 0;
    if (!parse_u64_until(bytes, len, pos, sep, &mag))
        return false;
    if (neg) {
        if (mag > (uint64_t)INT64_MAX + 1u)
            return false;
        *out = mag == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(int64_t)mag;
    } else {
        if (mag > (uint64_t)INT64_MAX)
            return false;
        *out = (int64_t)mag;
    }
    return true;
}

static MO_MUST_USE bool program_allowed(const struct mo_capability_info *info, bool fake,
                                        const uint8_t *prog, size_t prog_len) {
    if (!valid_program_name(prog, prog_len))
        return false;
    uint32_t allow_slot = fake ? 1u : 0u;
    if (mo_state_length(info->state) <= allow_slot)
        return true;
    return list_contains_text(mo_state_get(info->state, allow_slot), prog, prog_len);
}

static MO_MUST_USE enum mo_host_outcome command_run_fake(struct mo_host *host, struct mo_heap *heap,
                                                         uint32_t result_type,
                                                         const struct mo_cell *args,
                                                         struct mo_cell *out) {
    const struct mo_capability_info *info = mo_capability_get(args[0]);
    if (info->read_only) {
        size_t esc_len = 0;
        const uint8_t *esc = info->has_prefix ? mo_value_data(info->prefix, &esc_len) : NULL;
        return command_error_tag(host, heap, result_type, "outside_folder:", esc, esc_len, out);
    }
    size_t prog_len = 0;
    const uint8_t *prog = mo_value_data(args[1], &prog_len);
    if (!program_allowed(info, true, prog, prog_len))
        return command_error_tag(host, heap, result_type, "not_permitted:", prog, prog_len, out);

    uint32_t argc = mo_list_size(args[2]);
    size_t cwd_len = 0;
    const uint8_t *cwd = info->has_prefix ? mo_value_data(info->prefix, &cwd_len) : NULL;
    size_t max_key_len = 32 + cwd_len + 32 + prog_len + 32;
    for (uint32_t i = 0; i < argc; i++) {
        struct mo_cell a;
        MO_ASSERT(mo_list_get(args[2], i, &a));
        size_t alen;
        const uint8_t *abytes = mo_value_data(a, &alen);
        if (alen > MO_LIMIT_COMMAND_ARG_BYTES || memchr(abytes, 0, alen) != NULL)
            return command_error_tag(host, heap, result_type,
                                     "spawn_failed:invalid command argument", NULL, 0, out);
        max_key_len += 32 + alen;
    }
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, heap);
    char *key_buf = mo_scratch_resize(&scratch, NULL, 0, max_key_len);
    if (key_buf == NULL) {
        mo_scratch_free(&scratch);
        return MO_HOST_OUT_OF_MEMORY;
    }
    size_t at = 0;
    at += append_decimal(key_buf + at, cwd_len);
    key_buf[at++] = ':';
    if (cwd_len != 0) {
        memcpy(key_buf + at, cwd, cwd_len);
        at += cwd_len;
    }
    key_buf[at++] = '\n';
    at += append_decimal(key_buf + at, prog_len);
    key_buf[at++] = ':';
    memcpy(key_buf + at, prog, prog_len);
    at += prog_len;
    key_buf[at++] = '\n';
    at += append_decimal(key_buf + at, argc);
    key_buf[at++] = '\n';
    for (uint32_t i = 0; i < argc; i++) {
        struct mo_cell a;
        MO_ASSERT(mo_list_get(args[2], i, &a));
        size_t alen;
        const uint8_t *abytes = mo_value_data(a, &alen);
        at += append_decimal(key_buf + at, alen);
        key_buf[at++] = ':';
        if (alen != 0) {
            memcpy(key_buf + at, abytes, alen);
            at += alen;
        }
        key_buf[at++] = '\n';
    }
    MO_ASSERT(at <= max_key_len);
    struct mo_cell key_cell;
    bool key_ok = mo_value_bytes(heap, (const uint8_t *)key_buf, at, &key_cell);
    mo_scratch_free(&scratch);
    if (!key_ok)
        return MO_HOST_OUT_OF_MEMORY;

    struct mo_cell val_cell;
    bool found = false;
    bool tbl_ok = mo_table_get(heap, host->types->replies, mo_state_get(info->state, 0), key_cell,
                               &found, &val_cell);
    mo_value_drop(heap, host->types->bytes, key_cell);
    if (!tbl_ok)
        return MO_HOST_OUT_OF_MEMORY;
    if (!found)
        return command_error_tag(host, heap, result_type,
                                 "spawn_failed:unexpected command in fake: ", prog, prog_len, out);

    size_t val_len = 0, pos = 0;
    const uint8_t *val = mo_value_data(val_cell, &val_len);
    int64_t status = 0;
    uint64_t out_len = 0, err_len = 0;
    if (!parse_i64_until(val, val_len, &pos, ':', &status) ||
        !parse_u64_until(val, val_len, &pos, ':', &out_len) ||
        !parse_u64_until(val, val_len, &pos, '\n', &err_len) ||
        out_len > MO_LIMIT_COMMAND_OUTPUT_BYTES || err_len > MO_LIMIT_COMMAND_OUTPUT_BYTES ||
        pos + (size_t)out_len + (size_t)err_len != val_len)
        return command_error_tag(host, heap, result_type,
                                 "spawn_failed:malformed fake command reply", NULL, 0, out);
    return command_ok_tuple(host, heap, result_type, status, val + pos, (size_t)out_len,
                            val + pos + (size_t)out_len, (size_t)err_len, out);
}

static bool set_rlimit_bound(int resource, rlim_t value) {
    struct rlimit bounds = {value, value};
    return setrlimit(resource, &bounds) == 0;
}

static pid_t reap_child(pid_t pid, int *status, int options) {
    pid_t res;
    do {
        res = waitpid(pid, status, options);
    } while (res < 0 && errno == EINTR);
    return res;
}

static void kill_and_reap(pid_t pid) {
    if (pid <= 0)
        return;
    (void)kill(-pid, SIGKILL);
    (void)kill(pid, SIGKILL);
    int status = 0;
    (void)reap_child(pid, &status, 0);
}

static void remove_temp_tree(const char *dir_path) {
    if (dir_path == NULL || dir_path[0] == '\0')
        return;
    DIR *dir = opendir(dir_path);
    if (dir != NULL) {
        for (struct dirent *e; (e = readdir(dir)) != NULL;) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            char child[MO_LIMIT_HOST_PATH_SIZE + 256];
            int n = snprintf(child, sizeof child, "%s/%s", dir_path, e->d_name);
            if (n <= 0 || (size_t)n >= sizeof child)
                continue;
            struct stat st;
            if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
                remove_temp_tree(child);
            else
                (void)unlink(child);
        }
        (void)closedir(dir);
    }
    (void)rmdir(dir_path);
}

static MO_MUST_USE bool resolve_executable(const uint8_t *prog, size_t prog_len,
                                           char out[MO_LIMIT_HOST_PATH_SIZE]) {
    char name[256];
    memcpy(name, prog, prog_len);
    name[prog_len] = '\0';
    if (strcmp(name, "mo") == 0) {
#if defined(__linux__)
        ssize_t n = readlink("/proc/self/exe", out, MO_LIMIT_HOST_PATH_SIZE - 1);
        if (n > 0 && (size_t)n < MO_LIMIT_HOST_PATH_SIZE - 1) {
            out[n] = '\0';
            /* When invoked inside a unit test binary (e.g. test_command.test), look for sibling
             * ../../mo if the binary isn't mo itself. */
            const char *slash = strrchr(out, '/');
            if (slash != NULL && strcmp(slash + 1, "mo") == 0 && access(out, X_OK) == 0)
                return true;
            if (slash != NULL) {
                char candidate[MO_LIMIT_HOST_PATH_SIZE];
                size_t dir_len = (size_t)(slash - out);
                int cn = snprintf(candidate, sizeof candidate, "%.*s/../mo", (int)dir_len, out);
                if (cn > 0 && (size_t)cn < sizeof candidate && access(candidate, X_OK) == 0) {
                    char real_mo[PATH_MAX];
                    if (realpath(candidate, real_mo) != NULL &&
                        strlen(real_mo) < MO_LIMIT_HOST_PATH_SIZE) {
                        memcpy(out, real_mo, strlen(real_mo) + 1);
                        return true;
                    }
                }
            }
        }
#endif
    }
    static const char *const dirs[] = {"/usr/local/bin", "/usr/bin", "/bin"};
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        int n = snprintf(out, MO_LIMIT_HOST_PATH_SIZE, "%s/%s", dirs[i], name);
        if (n > 0 && (size_t)n < MO_LIMIT_HOST_PATH_SIZE) {
            struct stat st;
            if (stat(out, &st) == 0 && S_ISREG(st.st_mode) && access(out, X_OK) == 0)
                return true;
        }
    }
    return false;
}

static MO_MUST_USE bool fd_realpath(int fd, char out[MO_LIMIT_HOST_PATH_SIZE]) {
#if defined(__linux__)
    char proc_link[64];
    int n = snprintf(proc_link, sizeof proc_link, "/proc/self/fd/%d", fd);
    MO_ASSERT(n > 0 && (size_t)n < sizeof proc_link);
    ssize_t rl = readlink(proc_link, out, MO_LIMIT_HOST_PATH_SIZE - 1);
    if (rl > 0 && (size_t)rl < MO_LIMIT_HOST_PATH_SIZE - 1) {
        out[rl] = '\0';
        return true;
    }
#endif
    int saved = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (saved < 0)
        return false;
    bool ok = fchdir(fd) == 0 && getcwd(out, MO_LIMIT_HOST_PATH_SIZE) != NULL;
    if (fchdir(saved) != 0)
        ok = false;
    mo_host_close(saved);
    return ok;
}

static MO_MUST_USE bool now_monotonic_ns(uint64_t *out) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return false;
    *out = (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
    return true;
}

static MO_MUST_USE enum mo_host_outcome command_run_real(struct mo_host *host, struct mo_heap *heap,
                                                         uint32_t result_type,
                                                         const struct mo_cell *args,
                                                         int64_t timeout_us, struct mo_cell *out,
                                                         struct mo_host_error *error) {
    const struct mo_capability_info *info = mo_capability_get(args[0]);
    const struct mo_command_root *root = (const struct mo_command_root *)info->handle;
    if (root->forbidden) {
        size_t esc_len = 0;
        const uint8_t *esc = info->has_prefix ? mo_value_data(info->prefix, &esc_len) : NULL;
        return command_error_tag(host, heap, result_type, "outside_folder:", esc, esc_len, out);
    }
    size_t prog_len = 0;
    const uint8_t *prog = mo_value_data(args[1], &prog_len);
    if (!program_allowed(info, false, prog, prog_len))
        return command_error_tag(host, heap, result_type, "not_permitted:", prog, prog_len, out);

    int workdir_fd = root->descriptor;
    bool close_workdir = false;
    if (root->pending) {
        size_t plen = 0;
        const uint8_t *pbytes = mo_value_data(info->prefix, &plen);
        if (plen >= MO_LIMIT_HOST_PATH_SIZE)
            return command_error_tag(host, heap, result_type, "spawn_failed:path too long", NULL, 0,
                                     out);
        char rel[MO_LIMIT_HOST_PATH_SIZE];
        memcpy(rel, pbytes, plen);
        rel[plen] = '\0';
        struct mo_path_result resolved;
        struct mo_file_error rerr =
            mo_host_resolve(host, root->descriptor, rel, PATH_FOLDER, &resolved);
        if (rerr.containment || rerr.status == FILE_NOT_ALLOWED)
            return command_error_tag(host, heap, result_type, "outside_folder:", pbytes, plen, out);
        if (rerr.status != FILE_OK)
            return command_error_tag(host, heap, result_type,
                                     "spawn_failed:working folder unavailable", NULL, 0, out);
        workdir_fd = resolved.descriptor;
        close_workdir = true;
    }

    uint32_t argc = mo_list_size(args[2]);
    for (uint32_t i = 0; i < argc; i++) {
        struct mo_cell a;
        MO_ASSERT(mo_list_get(args[2], i, &a));
        size_t alen;
        const uint8_t *abytes = mo_value_data(a, &alen);
        if (alen > MO_LIMIT_COMMAND_ARG_BYTES || memchr(abytes, 0, alen) != NULL) {
            if (close_workdir)
                mo_host_close(workdir_fd);
            return command_error_tag(host, heap, result_type,
                                     "spawn_failed:invalid command argument", NULL, 0, out);
        }
    }

    char exe_path[MO_LIMIT_HOST_PATH_SIZE];
    if (!resolve_executable(prog, prog_len, exe_path)) {
        if (close_workdir)
            mo_host_close(workdir_fd);
        return command_error_tag(host, heap, result_type, "spawn_failed:program not found: ", prog,
                                 prog_len, out);
    }

    char workdir_path[MO_LIMIT_HOST_PATH_SIZE];
    if (!fd_realpath(workdir_fd, workdir_path)) {
        if (close_workdir)
            mo_host_close(workdir_fd);
        return command_error_tag(host, heap, result_type,
                                 "spawn_failed:could not resolve working folder", NULL, 0, out);
    }

    const char *base_tmp = getenv("TMPDIR");
    char tmp_dir[MO_LIMIT_HOST_PATH_SIZE];
    int tn = snprintf(tmp_dir, sizeof tmp_dir, "%s/mo-cmd-XXXXXX",
                      (base_tmp != NULL && base_tmp[0] != '\0' && strlen(base_tmp) < 512) ? base_tmp
                                                                                          : "/tmp");
    if (tn <= 0 || (size_t)tn >= sizeof tmp_dir || mkdtemp(tmp_dir) == NULL) {
        if (close_workdir)
            mo_host_close(workdir_fd);
        return command_error_tag(host, heap, result_type,
                                 "spawn_failed:could not create temporary folder", NULL, 0, out);
    }

    /* Copy argv strings into scratch so each is NUL-terminated. */
    struct mo_scratch arg_scratch;
    mo_scratch_init(&arg_scratch, heap);
    size_t arg_bytes_total = prog_len + 1;
    for (uint32_t i = 0; i < argc; i++) {
        struct mo_cell a;
        MO_ASSERT(mo_list_get(args[2], i, &a));
        size_t alen;
        const uint8_t *aptr = mo_value_data(a, &alen);
        (void)aptr;
        arg_bytes_total += alen + 1;
    }
    char *arg_pool = mo_scratch_resize(&arg_scratch, NULL, 0, arg_bytes_total);
    if (arg_pool == NULL) {
        mo_scratch_free(&arg_scratch);
        remove_temp_tree(tmp_dir);
        if (close_workdir)
            mo_host_close(workdir_fd);
        return MO_HOST_OUT_OF_MEMORY;
    }
    char *child_argv[MO_LIMIT_COMMAND_ARGS + 2];
    size_t pool_at = 0;
    memcpy(arg_pool, prog, prog_len);
    arg_pool[prog_len] = '\0';
    child_argv[0] = exe_path;
    pool_at += prog_len + 1;
    for (uint32_t i = 0; i < argc; i++) {
        struct mo_cell a;
        MO_ASSERT(mo_list_get(args[2], i, &a));
        size_t alen;
        const uint8_t *abytes = mo_value_data(a, &alen);
        child_argv[i + 1] = arg_pool + pool_at;
        if (alen != 0)
            memcpy(arg_pool + pool_at, abytes, alen);
        arg_pool[pool_at + alen] = '\0';
        pool_at += alen + 1;
    }
    child_argv[argc + 1] = NULL;

    int out_pipe[2] = {-1, -1}, err_pipe[2] = {-1, -1};
    long max_fd = sysconf(_SC_OPEN_MAX);
    if (max_fd < 0 || max_fd > 65536)
        max_fd = 1024;
    uint64_t start_ns = 0;
    if (!now_monotonic_ns(&start_ns)) {
        mo_scratch_free(&arg_scratch);
        remove_temp_tree(tmp_dir);
        if (close_workdir)
            mo_host_close(workdir_fd);
        *error = (struct mo_host_error){.reason = "internal error: monotonic clock unavailable"};
        return MO_HOST_INTERNAL;
    }
    uint64_t deadline_ns = start_ns + (uint64_t)timeout_us * UINT64_C(1000);

    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        if (out_pipe[0] >= 0) {
            mo_host_close(out_pipe[0]);
            mo_host_close(out_pipe[1]);
        }
        mo_scratch_free(&arg_scratch);
        remove_temp_tree(tmp_dir);
        if (close_workdir)
            mo_host_close(workdir_fd);
        return command_error_tag(host, heap, result_type, "spawn_failed:pipe failed", NULL, 0, out);
    }
    (void)fcntl(out_pipe[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(out_pipe[1], F_SETFD, FD_CLOEXEC);
    (void)fcntl(err_pipe[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(err_pipe[1], F_SETFD, FD_CLOEXEC);
    (void)fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
    (void)fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);

    char home_env[MO_LIMIT_HOST_PATH_SIZE + 8];
    char tmp_env[MO_LIMIT_HOST_PATH_SIZE + 10];
    (void)snprintf(home_env, sizeof home_env, "HOME=%s", workdir_path);
    (void)snprintf(tmp_env, sizeof tmp_env, "TMPDIR=%s", tmp_dir);
    bool can_confine = mo_compile_can_confine();
    rlim_t cpu_seconds = (rlim_t)((timeout_us + 999999) / 1000000 + 1);

    pid_t pid = fork();
    if (pid == 0) {
        (void)setpgid(0, 0);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull < 0 || dup2(devnull, STDIN_FILENO) < 0 ||
            dup2(out_pipe[1], STDOUT_FILENO) < 0 || dup2(err_pipe[1], STDERR_FILENO) < 0 ||
            fchdir(workdir_fd) != 0 ||
            (!MO_UPGRADE_COMPILE_ASAN && !set_rlimit_bound(RLIMIT_AS, MO_LIMIT_COMMAND_MEMORY)) ||
            !set_rlimit_bound(RLIMIT_CPU, cpu_seconds) || !set_rlimit_bound(RLIMIT_CORE, 0))
            _exit(125);
        for (int fd = 3; fd < (int)max_fd; fd++)
            (void)close(fd);
        if (can_confine && !mo_command_confine(exe_path, workdir_path, tmp_dir))
            _exit(125);
        char path_env[] = "PATH=/usr/local/bin:/usr/bin:/bin";
        char lang_env[] = "LANG=C.UTF-8";
        char git_nosys[] = "GIT_CONFIG_NOSYSTEM=1";
        char *envp[] = {path_env, home_env, tmp_env, lang_env, git_nosys, NULL};
        execve(exe_path, child_argv, envp);
        _exit(126);
    }
    mo_scratch_free(&arg_scratch);
    mo_host_close(out_pipe[1]);
    mo_host_close(err_pipe[1]);
    if (close_workdir)
        mo_host_close(workdir_fd);

    if (pid < 0) {
        mo_host_close(out_pipe[0]);
        mo_host_close(err_pipe[0]);
        remove_temp_tree(tmp_dir);
        return command_error_tag(host, heap, result_type, "spawn_failed:fork failed", NULL, 0, out);
    }
    (void)setpgid(pid, pid);

    struct mo_scratch io_scratch;
    mo_scratch_init(&io_scratch, heap);
    uint8_t *out_buf = NULL, *err_buf = NULL;
    size_t out_len = 0, out_cap = 0, err_len = 0, err_cap = 0;
    bool out_open = true, err_open = true, reaped = false;
    bool timed_out = false, too_large = false, oom = false;
    int child_status = 0;

    while (out_open || err_open || !reaped) {
        uint64_t now_ns = 0;
        if (!now_monotonic_ns(&now_ns)) {
            kill_and_reap(pid);
            if (out_open)
                mo_host_close(out_pipe[0]);
            if (err_open)
                mo_host_close(err_pipe[0]);
            mo_scratch_free(&io_scratch);
            remove_temp_tree(tmp_dir);
            *error =
                (struct mo_host_error){.reason = "internal error: monotonic clock unavailable"};
            return MO_HOST_INTERNAL;
        }
        if (now_ns >= deadline_ns) {
            if (!reaped) {
                pid_t r = reap_child(pid, &child_status, WNOHANG);
                if (r == pid)
                    reaped = true;
            }
            if (!reaped) {
                timed_out = true;
                break;
            }
        }
        if (out_open || err_open) {
            uint64_t rem_ms = now_ns < deadline_ns ? (deadline_ns - now_ns) / UINT64_C(1000000) : 0;
            int wait_ms = rem_ms > 20 ? 20 : (int)rem_ms;
            struct pollfd pfds[2] = {
                {.fd = out_open ? out_pipe[0] : -1, .events = POLLIN},
                {.fd = err_open ? err_pipe[0] : -1, .events = POLLIN},
            };
            int pr = poll(pfds, 2, reaped ? 0 : wait_ms);
            if (pr < 0 && errno != EINTR)
                break;
            for (int s = 0; s < 2; s++) {
                bool *is_open = s == 0 ? &out_open : &err_open;
                int pfd = s == 0 ? out_pipe[0] : err_pipe[0];
                uint8_t **buf = s == 0 ? &out_buf : &err_buf;
                size_t *len = s == 0 ? &out_len : &err_len;
                size_t *cap = s == 0 ? &out_cap : &err_cap;
                if (!*is_open || (pfds[s].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
                    continue;
                for (;;) {
                    uint8_t chunk[8192];
                    ssize_t n = read(pfd, chunk, sizeof chunk);
                    if (n > 0) {
                        if ((size_t)n > MO_LIMIT_COMMAND_OUTPUT_BYTES - *len) {
                            too_large = true;
                            break;
                        }
                        size_t needed = *len + (size_t)n;
                        if (needed > *cap) {
                            size_t next_cap = *cap == 0 ? 4096 : *cap * 2;
                            while (next_cap < needed)
                                next_cap *= 2;
                            if (next_cap > MO_LIMIT_COMMAND_OUTPUT_BYTES)
                                next_cap = MO_LIMIT_COMMAND_OUTPUT_BYTES;
                            uint8_t *grown = mo_scratch_resize(&io_scratch, *buf, *cap, next_cap);
                            if (grown == NULL) {
                                oom = true;
                                break;
                            }
                            *buf = grown;
                            *cap = next_cap;
                        }
                        memcpy(*buf + *len, chunk, (size_t)n);
                        *len += (size_t)n;
                    } else if (n == 0) {
                        mo_host_close(pfd);
                        *is_open = false;
                        break;
                    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    } else if (errno == EINTR) {
                        continue;
                    } else {
                        mo_host_close(pfd);
                        *is_open = false;
                        break;
                    }
                }
                if (too_large || oom)
                    break;
            }
            if (too_large || oom)
                break;
        }
        if (!reaped) {
            pid_t r = reap_child(pid, &child_status, (!out_open && !err_open) ? WNOHANG : WNOHANG);
            if (r == pid) {
                reaped = true;
            } else if (!out_open && !err_open) {
                struct timespec nap = {.tv_sec = 0, .tv_nsec = 2000000};
                (void)nanosleep(&nap, NULL);
            }
        }
    }

    if (!reaped)
        kill_and_reap(pid);
    if (out_open)
        mo_host_close(out_pipe[0]);
    if (err_open)
        mo_host_close(err_pipe[0]);
    remove_temp_tree(tmp_dir);

    if (oom) {
        mo_scratch_free(&io_scratch);
        return MO_HOST_OUT_OF_MEMORY;
    }
    if (too_large) {
        mo_scratch_free(&io_scratch);
        return command_error_tag(host, heap, result_type, "output_too_large", NULL, 0, out);
    }
    if (timed_out || (WIFSIGNALED(child_status) && WTERMSIG(child_status) == SIGXCPU)) {
        mo_scratch_free(&io_scratch);
        return command_error_tag(host, heap, result_type, "timed_out", NULL, 0, out);
    }
    if (WIFEXITED(child_status) && WEXITSTATUS(child_status) == 125) {
        mo_scratch_free(&io_scratch);
        return command_error_tag(host, heap, result_type,
                                 "spawn_failed:child setup or confinement failed", NULL, 0, out);
    }
    if (WIFEXITED(child_status) && WEXITSTATUS(child_status) == 126) {
        mo_scratch_free(&io_scratch);
        return command_error_tag(host, heap, result_type, "spawn_failed:execve failed", NULL, 0,
                                 out);
    }
    int64_t exit_code =
        WIFEXITED(child_status)
            ? (int64_t)WEXITSTATUS(child_status)
            : (int64_t)(128 + (WIFSIGNALED(child_status) ? WTERMSIG(child_status) : 0));
    enum mo_host_outcome built = command_ok_tuple(host, heap, result_type, exit_code, out_buf,
                                                  out_len, err_buf, err_len, out);
    mo_scratch_free(&io_scratch);
    return built;
}

enum mo_host_outcome mo_host_command_call(struct mo_host *host, struct mo_heap *heap,
                                          uint32_t operation, uint32_t type,
                                          const struct mo_cell *args, struct mo_cell *out,
                                          struct mo_host_error *error) {
    const struct mo_capability_info *info = mo_capability_get(args[0]);
    bool fake = ((const struct mo_host_operations *)info->operations)->fake;
    if (operation == MO_HOST_COMMAND_ALLOW_PROGRAMS)
        return command_allow_programs(host, heap, type, args, out, error);
    if (operation == MO_HOST_COMMAND_WITHIN)
        return fake ? command_within_fake(host, heap, type, args, out)
                    : command_within_real(host, heap, type, args, out, error);
    MO_ASSERT(operation == MO_HOST_COMMAND_RUN_RAW);
    int64_t timeout_us = mo_cell_get_duration(args[3]);
    if (timeout_us < 1000 || timeout_us > MO_LIMIT_COMMAND_DURATION_US)
        return mo_host_bug(error, "timeout below 1ms or above 1 hour");
    if (mo_list_size(args[2]) > MO_LIMIT_COMMAND_ARGS)
        return mo_host_bug(error, "too many command arguments");
    if (fake)
        return command_run_fake(host, heap, type, args, out);
    return command_run_real(host, heap, type, args, timeout_us, out, error);
}
