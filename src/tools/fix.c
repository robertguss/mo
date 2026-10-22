#define _POSIX_C_SOURCE 200809L

#include "tools/fix.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Two edits overlap when they change a byte in common or insert at the same place, as struct
 * mo_fix says; an insertion may sit at either end of a replaced span. */
static bool overlap(const struct mo_edit *a, const struct mo_edit *b) {
    bool insertions = a->span.start == a->span.end && b->span.start == b->span.end;
    return (a->span.start < b->span.end && b->span.start < a->span.end) ||
           (insertions && a->span.start == b->span.start);
}

static int by_position(const void *left, const void *right) {
    const struct mo_edit *a = *(const struct mo_edit *const *)left;
    const struct mo_edit *b = *(const struct mo_edit *const *)right;
    if (a->span.start != b->span.start)
        return a->span.start < b->span.start ? -1 : 1;
    return a->span.end < b->span.end ? -1 : a->span.end > b->span.end ? 1 : 0;
}

/* Whether the fix can join the taken edits: every edit in the file, none overlapping a taken one.
 * The work is bounded by MO_LIMIT_FIX_ROUND_EDITS taken edits times the fix's edits. */
static bool fits(const struct mo_fix *fix, uint32_t file, uint32_t length,
                 const struct mo_edit *const *taken, uint32_t count) {
    if (fix->edit_count > MO_LIMIT_FIX_ROUND_EDITS - count)
        return false;
    for (uint32_t i = 0; i < fix->edit_count; i++) {
        const struct mo_edit *e = &fix->edits[i];
        if (e->span.file != file)
            return false;
        MO_ASSERT(e->span.start <= e->span.end && e->span.end <= length);
        for (uint32_t j = 0; j < count; j++)
            if (overlap(e, taken[j]))
                return false;
    }
    return true;
}

bool mo_fix_source(const struct mo_sources *sources, uint32_t file, struct mo_diags *diags,
                   struct mo_buffer *out, uint32_t *applied) {
    MO_ASSERT(file < sources->count);
    const struct mo_source *source = &sources->files[file];
    *applied = 0;
    struct mo_arena arena;
    mo_arena_init(&arena, 2 * MO_LIMIT_FIX_ROUND_EDITS * sizeof(const struct mo_edit *));
    const struct mo_edit **taken = mo_arena_allocate_array(
        &arena, MO_LIMIT_FIX_ROUND_EDITS, sizeof *taken, _Alignof(const struct mo_edit *));
    if (taken == NULL) {
        mo_arena_free(&arena);
        return false;
    }
    uint32_t count = 0;
    mo_diags_sort(diags);
    for (uint32_t i = 0; i < diags->count; i++) {
        const struct mo_diag *d = &diags->items[i];
        if (d->fix_count != 1 || !fits(&d->fixes[0], file, source->length, taken, count))
            continue;
        for (uint32_t j = 0; j < d->fixes[0].edit_count; j++)
            taken[count++] = &d->fixes[0].edits[j];
        (*applied)++;
    }
    qsort(taken, count, sizeof *taken, by_position);
    bool ok = true;
    uint32_t at = 0;
    for (uint32_t i = 0; ok && i < count; i++) {
        ok = mo_buffer_append(out, source->bytes + at, taken[i]->span.start - at) &&
             mo_buffer_append_text(out, taken[i]->replacement);
        at = taken[i]->span.end;
    }
    ok = ok && mo_buffer_append(out, source->bytes + at, source->length - at);
    mo_arena_free(&arena);
    return ok;
}

/* The temporary basename does not extend the source basename: a source name
 * already at NAME_MAX still has room for a sibling temporary. */
bool mo_replace_file(const char *path, const struct mo_buffer *output, const struct stat *original,
                     int *error) {
    const char *slash = strrchr(path, '/');
    size_t directory = slash == NULL ? 0 : (size_t)(slash - path) + 1;
    char temporary[MO_LIMIT_PATH_SIZE + 32];
    static const char suffix[] = ".mo-write-XXXXXX";
    MO_ASSERT(directory <= MO_LIMIT_PATH_SIZE);
    memcpy(temporary, path, directory);
    memcpy(temporary + directory, suffix, sizeof suffix);
    int fd = mkstemp(temporary);
    if (fd < 0) {
        *error = errno;
        return false;
    }
    FILE *stream = fdopen(fd, "wb");
    bool ok = stream != NULL;
    if (!ok) {
        *error = errno;
        if (close(fd) != 0)
            *error = errno;
    } else {
        ok = fwrite(output->bytes, 1, output->length, stream) == output->length &&
             fflush(stream) == 0 && fchmod(fd, original->st_mode & (mode_t)0777) == 0 &&
             fsync(fd) == 0;
        if (!ok)
            *error = errno;
        if (fclose(stream) != 0) {
            ok = false;
            *error = errno;
        }
    }
    if (ok) {
        struct stat current;
        ok = lstat(path, &current) == 0;
        if (!ok)
            *error = errno;
        else if (current.st_dev != original->st_dev || current.st_ino != original->st_ino) {
            ok = false;
            *error = EBUSY;
        }
    }
    if (ok && rename(temporary, path) != 0) {
        ok = false;
        *error = errno;
    }
    if (!ok && unlink(temporary) != 0)
        *error = errno;
    return ok;
}
