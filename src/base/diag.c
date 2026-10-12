#include "base/diag.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "base/codes.h"
#include "base/limits.h"

/* Diagnostics */

bool mo_diags_init(struct mo_diags *diags) {
    MO_ASSERT(diags != NULL);
    mo_arena_init(&diags->arena, MO_LIMIT_DIAGNOSTICS_SIZE);
    diags->count = 0;
    diags->dropped = 0;
    diags->items = mo_arena_allocate_array(&diags->arena, MO_LIMIT_DIAGNOSTICS,
                                           sizeof(struct mo_diag), _Alignof(struct mo_diag));
    return diags->items != NULL;
}

void mo_diags_free(struct mo_diags *diags) {
    MO_ASSERT(diags != NULL);
    mo_arena_free(&diags->arena);
    diags->items = NULL;
    diags->count = 0;
    diags->dropped = 0;
}

static void check_span(struct mo_span span) {
    MO_ASSERT(span.start <= span.end);
}

/* Copies text into the arena; NULL stays NULL. False when it doesn't fit. */
static bool copy_text(struct mo_arena *arena, const char *text, const char **copy) {
    if (text == NULL) {
        *copy = NULL;
        return true;
    }
    *copy = mo_arena_copy_text(arena, text);
    return *copy != NULL;
}

static bool copy_label(struct mo_arena *arena, const struct mo_label *label,
                       struct mo_label *copy) {
    check_span(label->span);
    copy->span = label->span;
    return copy_text(arena, label->text, &copy->text);
}

/* Fills *copy with diag, its strings, labels and fixes copied into the arena. False when they
 * don't fit, leaving *copy unfinished. */
static bool copy_diag(struct mo_arena *arena, const struct mo_diag *diag, const char *section,
                      struct mo_diag *copy) {
    *copy = (struct mo_diag){.code = diag->code, .section = section};
    if (!copy_label(arena, &diag->primary, &copy->primary) ||
        !copy_text(arena, diag->message, &copy->message)) {
        return false;
    }
    if (diag->secondary_count > 0) {
        struct mo_label *labels = mo_arena_allocate_array(
            arena, diag->secondary_count, sizeof(struct mo_label), _Alignof(struct mo_label));
        if (labels == NULL) {
            return false;
        }
        for (uint32_t i = 0; i < diag->secondary_count; i++) {
            if (!copy_label(arena, &diag->secondary[i], &labels[i])) {
                return false;
            }
        }
        copy->secondary = labels;
        copy->secondary_count = diag->secondary_count;
    }
    if (diag->fix_count > 0) {
        struct mo_fix *fixes = mo_arena_allocate_array(
            arena, diag->fix_count, sizeof(struct mo_fix), _Alignof(struct mo_fix));
        if (fixes == NULL) {
            return false;
        }
        for (uint32_t i = 0; i < diag->fix_count; i++) {
            const struct mo_fix *fix = &diag->fixes[i];
            check_span(fix->span);
            MO_ASSERT(fix->replacement != NULL);
            fixes[i].span = fix->span;
            if (!copy_text(arena, fix->replacement, &fixes[i].replacement)) {
                return false;
            }
        }
        copy->fixes = fixes;
        copy->fix_count = diag->fix_count;
    }
    return true;
}

bool mo_diags_add(struct mo_diags *diags, const struct mo_diag *diag) {
    MO_ASSERT(diags != NULL && diags->items != NULL && diag != NULL);
    const struct mo_code_info *code = mo_code_find(diag->code);
    MO_ASSERT(code != NULL);
    MO_ASSERT(diag->section == NULL ||
              (code->section != NULL && strcmp(diag->section, code->section) == 0));
    MO_ASSERT(diag->message != NULL);
    MO_ASSERT(diag->secondary_count == 0 || diag->secondary != NULL);
    MO_ASSERT(diag->fix_count == 0 || diag->fixes != NULL);
    MO_ASSERT(diags->count <= MO_LIMIT_DIAGNOSTICS);
    /* A diagnostic that doesn't fit leaves what it copied in the arena, unused until
     * mo_diags_free: the arena never frees one piece alone. */
    if (diags->count == MO_LIMIT_DIAGNOSTICS ||
        !copy_diag(&diags->arena, diag, code->section, &diags->items[diags->count])) {
        if (diags->dropped < UINT32_MAX) {
            diags->dropped += 1;
        }
        return false;
    }
    diags->count += 1;
    return true;
}

bool mo_diags_any(const struct mo_diags *diags) {
    MO_ASSERT(diags != NULL);
    return diags->count > 0 || diags->dropped > 0;
}

static bool comes_before(const struct mo_diag *a, const struct mo_diag *b) {
    if (a->primary.span.file != b->primary.span.file) {
        return a->primary.span.file < b->primary.span.file;
    }
    return a->primary.span.start < b->primary.span.start;
}

void mo_diags_sort(struct mo_diags *diags) {
    MO_ASSERT(diags != NULL);
    /* An insertion sort: stable, and quick on diagnostics that arrive nearly in order, as a
     * stage's do. There are at most MO_LIMIT_DIAGNOSTICS of them. */
    for (uint32_t i = 1; i < diags->count; i++) {
        struct mo_diag moving = diags->items[i];
        uint32_t j = i;
        while (j > 0 && comes_before(&moving, &diags->items[j - 1])) {
            diags->items[j] = diags->items[j - 1];
            j -= 1;
        }
        diags->items[j] = moving;
    }
}

/* Source files */

/* The memory the source files may take. It holds MO_LIMIT_PROGRAM_SIZE bytes of source with line
 * tables for as many lines as they could have, at four bytes a line, the paths, and a file read
 * past MO_LIMIT_FILE_SIZE before it is refused, twice over, since an arena may leave as much
 * unused as it hands out (src/base/arena.c). So the limits on source files, not this one, decide
 * what a program may hold. */
#define SOURCES_ARENA_LIMIT ((size_t)1 << 30)
_Static_assert(2 * (5 * ((size_t)MO_LIMIT_PROGRAM_SIZE + MO_LIMIT_FILES) +
                    (size_t)MO_LIMIT_FILES * (MO_LIMIT_PATH_SIZE + 1 + sizeof(struct mo_source)) +
                    2 * ((size_t)MO_LIMIT_FILE_SIZE + 1)) <=
                   SOURCES_ARENA_LIMIT,
               "the sources' arena holds the largest program the limits allow");

/* The first block a file is read into. It doubles from there, up to one byte more than
 * MO_LIMIT_FILE_SIZE, which tells that a file is too big without reading the rest of it. */
enum { FIRST_READ_SIZE = 16 * 1024 };

bool mo_sources_init(struct mo_sources *sources) {
    MO_ASSERT(sources != NULL);
    mo_arena_init(&sources->arena, SOURCES_ARENA_LIMIT);
    sources->count = 0;
    sources->size = 0;
    sources->files = mo_arena_allocate_array(&sources->arena, MO_LIMIT_FILES,
                                             sizeof(struct mo_source), _Alignof(struct mo_source));
    return sources->files != NULL;
}

void mo_sources_free(struct mo_sources *sources) {
    MO_ASSERT(sources != NULL);
    mo_arena_free(&sources->arena);
    sources->files = NULL;
    sources->count = 0;
    sources->size = 0;
}

static bool refuse(struct mo_source_error *error_out, enum mo_source_problem problem,
                   int system_error) {
    *error_out = (struct mo_source_error){.problem = problem, .system_error = system_error};
    return false;
}

/* Checks what can be checked before reading: the number of files, and the path. */
static bool check_room(const struct mo_sources *sources, const char *path,
                       struct mo_source_error *error_out) {
    MO_ASSERT(sources->files != NULL && sources->count <= MO_LIMIT_FILES);
    if (sources->count == MO_LIMIT_FILES) {
        return refuse(error_out, MO_SOURCE_TOO_MANY, 0);
    }
    if (strlen(path) > MO_LIMIT_PATH_SIZE) {
        return refuse(error_out, MO_SOURCE_PATH_TOO_LONG, 0);
    }
    return true;
}

/* Hands back a block that wasn't kept, so its memory can be used again if it is the arena's
 * newest. Shrinking never fails. */
static void give_back(struct mo_arena *arena, uint8_t *bytes, size_t size) {
    if (bytes != NULL) {
        void *kept = mo_arena_resize(arena, bytes, size, 0, 1);
        MO_ASSERT(kept == bytes);
    }
}

/* Finds where each line starts: returns how many lines there are, and writes their starts to
 * starts unless it is NULL. */
static uint32_t find_lines(const uint8_t *bytes, uint32_t length, uint32_t *starts) {
    uint32_t count = 1;
    if (starts != NULL) {
        starts[0] = 0;
    }
    uint32_t at = 0;
    while (at < length) {
        const uint8_t *newline = memchr(bytes + at, '\n', length - at);
        if (newline == NULL) {
            break;
        }
        at = (uint32_t)(newline - bytes) + 1;
        if (starts != NULL) {
            starts[count] = at;
        }
        count += 1;
    }
    return count;
}

/* Adds a file whose bytes, length of them and then a NUL, are already in the arena. */
static bool add_file(struct mo_sources *sources, const char *path, const uint8_t *bytes,
                     size_t length, uint32_t *file_out, struct mo_source_error *error_out) {
    MO_ASSERT(length <= MO_LIMIT_FILE_SIZE && bytes[length] == 0);
    MO_ASSERT(sources->size + length <= MO_LIMIT_PROGRAM_SIZE);
    uint32_t size = (uint32_t)length;
    uint32_t line_count = find_lines(bytes, size, NULL);
    uint32_t *line_starts =
        mo_arena_allocate_array(&sources->arena, line_count, sizeof(uint32_t), _Alignof(uint32_t));
    const char *path_copy = line_starts == NULL ? NULL : mo_arena_copy_text(&sources->arena, path);
    if (path_copy == NULL) {
        return refuse(error_out, MO_SOURCE_NO_MEMORY, 0);
    }
    uint32_t found = find_lines(bytes, size, line_starts);
    MO_ASSERT(found == line_count);
    uint32_t file = sources->count;
    sources->files[file] = (struct mo_source){.path = path_copy,
                                              .bytes = bytes,
                                              .length = size,
                                              .line_count = line_count,
                                              .line_starts = line_starts};
    sources->count += 1;
    sources->size += length;
    *file_out = file;
    return true;
}

bool mo_sources_add_bytes(struct mo_sources *sources, const char *path, const uint8_t *bytes,
                          size_t length, uint32_t *file_out, struct mo_source_error *error_out) {
    MO_ASSERT(sources != NULL && path != NULL && (bytes != NULL || length == 0));
    MO_ASSERT(file_out != NULL && error_out != NULL);
    if (!check_room(sources, path, error_out)) {
        return false;
    }
    if (length > MO_LIMIT_FILE_SIZE) {
        return refuse(error_out, MO_SOURCE_TOO_BIG, 0);
    }
    if (length > MO_LIMIT_PROGRAM_SIZE - sources->size) {
        return refuse(error_out, MO_SOURCE_PROGRAM_TOO_BIG, 0);
    }
    uint8_t *copy = mo_arena_allocate(&sources->arena, length + 1, 1);
    if (copy == NULL) {
        return refuse(error_out, MO_SOURCE_NO_MEMORY, 0);
    }
    if (length > 0) {
        memcpy(copy, bytes, length);
    }
    copy[length] = 0;
    if (!add_file(sources, path, copy, length, file_out, error_out)) {
        give_back(&sources->arena, copy, length + 1);
        return false;
    }
    return true;
}

/* Reads a whole file into a block of the arena, with a NUL after it. Sets *bytes_out and
 * *length_out, or says in *error_out why it can't. */
static bool read_file(struct mo_arena *arena, const char *path, uint8_t **bytes_out,
                      size_t *length_out, struct mo_source_error *error_out) {
    errno = 0;
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return refuse(error_out, MO_SOURCE_UNREADABLE, errno);
    }
    size_t most = (size_t)MO_LIMIT_FILE_SIZE + 1;
    uint8_t *bytes = NULL;
    size_t capacity = 0;
    size_t length = 0;
    struct mo_source_error error = {.problem = MO_SOURCE_UNREADABLE, .system_error = 0};
    bool complete = false;
    for (;;) {
        if (length == capacity) {
            if (capacity == most) {
                error.problem = MO_SOURCE_TOO_BIG;
                break;
            }
            size_t grown = capacity == 0 ? FIRST_READ_SIZE : 2 * capacity;
            if (grown > most) {
                grown = most;
            }
            uint8_t *moved = mo_arena_resize(arena, bytes, capacity, grown, 1);
            if (moved == NULL) {
                error.problem = MO_SOURCE_NO_MEMORY;
                break;
            }
            bytes = moved;
            capacity = grown;
        }
        size_t wanted = capacity - length;
        errno = 0;
        size_t got = fread(bytes + length, 1, wanted, file);
        length += got;
        if (got < wanted) {
            if (ferror(file) != 0) {
                error.system_error = errno;
            } else {
                complete = true;
            }
            break;
        }
    }
    errno = 0;
    if (fclose(file) != 0 && complete) {
        complete = false;
        error.system_error = errno;
    }
    if (!complete) {
        give_back(arena, bytes, capacity);
        *error_out = error;
        return false;
    }
    /* The last read stopped short of the block's end, so the NUL fits. */
    MO_ASSERT(length < capacity);
    uint8_t *kept = mo_arena_resize(arena, bytes, capacity, length + 1, 1);
    MO_ASSERT(kept == bytes);
    bytes[length] = 0;
    *bytes_out = bytes;
    *length_out = length;
    return true;
}

bool mo_sources_add(struct mo_sources *sources, const char *path, uint32_t *file_out,
                    struct mo_source_error *error_out) {
    MO_ASSERT(sources != NULL && path != NULL && file_out != NULL && error_out != NULL);
    if (!check_room(sources, path, error_out)) {
        return false;
    }
    uint8_t *bytes = NULL;
    size_t length = 0;
    if (!read_file(&sources->arena, path, &bytes, &length, error_out)) {
        return false;
    }
    if (length > MO_LIMIT_PROGRAM_SIZE - sources->size) {
        give_back(&sources->arena, bytes, length + 1);
        return refuse(error_out, MO_SOURCE_PROGRAM_TOO_BIG, 0);
    }
    if (!add_file(sources, path, bytes, length, file_out, error_out)) {
        give_back(&sources->arena, bytes, length + 1);
        return false;
    }
    return true;
}

static const struct mo_source *source_at(const struct mo_sources *sources, uint32_t file) {
    MO_ASSERT(sources != NULL && file < sources->count);
    return &sources->files[file];
}

/* The bytes of the character at bytes[at]: its UTF-8's, if a valid character is there, decoded as
 * grammar/lexer.c decodes, or else 1, for a byte that isn't part of one. */
static uint32_t character_size(const uint8_t *bytes, uint32_t length, uint32_t at) {
    uint8_t first = bytes[at];
    uint32_t size;
    uint32_t value;
    uint32_t least;
    if (first < 0x80u) {
        return 1;
    } else if ((first & 0xE0u) == 0xC0u) {
        size = 2;
        value = first & 0x1Fu;
        least = 0x80u;
    } else if ((first & 0xF0u) == 0xE0u) {
        size = 3;
        value = first & 0x0Fu;
        least = 0x800u;
    } else if ((first & 0xF8u) == 0xF0u) {
        size = 4;
        value = first & 0x07u;
        least = 0x10000u;
    } else {
        return 1;
    }
    if (length - at < size) {
        return 1;
    }
    for (uint32_t i = 1; i < size; i++) {
        uint8_t next = bytes[at + i];
        if ((next & 0xC0u) != 0x80u) {
            return 1;
        }
        value = (value << 6) | (next & 0x3Fu);
    }
    if (value < least || value > 0x10FFFFu || (value >= 0xD800u && value <= 0xDFFFu)) {
        return 1;
    }
    return size;
}

/* The index of the line that holds offset: the last that starts at or before it. */
static uint32_t line_index(const struct mo_source *source, uint32_t offset) {
    uint32_t low = 0;
    uint32_t high = source->line_count;
    while (high - low > 1) {
        uint32_t middle = low + (high - low) / 2;
        if (source->line_starts[middle] <= offset) {
            low = middle;
        } else {
            high = middle;
        }
    }
    return low;
}

struct mo_position mo_sources_position(const struct mo_sources *sources, uint32_t file,
                                       uint32_t offset) {
    const struct mo_source *source = source_at(sources, file);
    MO_ASSERT(offset <= source->length);
    uint32_t index = line_index(source, offset);
    uint32_t column = 1;
    uint32_t at = source->line_starts[index];
    while (at < offset) {
        uint32_t size = character_size(source->bytes, source->length, at);
        if (size > offset - at) {
            break; /* the offset is inside this character */
        }
        bool line_ending =
            source->bytes[at] == '\r' && at + 1 < source->length && source->bytes[at + 1] == '\n';
        if (!line_ending) {
            column += 1;
        }
        at += size;
    }
    return (struct mo_position){.line = index + 1, .column = column};
}

struct mo_span mo_sources_line(const struct mo_sources *sources, uint32_t file, uint32_t line) {
    const struct mo_source *source = source_at(sources, file);
    MO_ASSERT(line >= 1 && line <= source->line_count);
    uint32_t start = source->line_starts[line - 1];
    if (line == source->line_count) {
        /* The last line has no line ending, so a "\r" at its end stays in it. */
        return (struct mo_span){.file = file, .start = start, .end = source->length};
    }
    uint32_t end = source->line_starts[line] - 1; /* its "\n" */
    if (end > start && source->bytes[end - 1] == '\r') {
        end -= 1;
    }
    return (struct mo_span){.file = file, .start = start, .end = end};
}
