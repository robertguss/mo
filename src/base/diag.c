#include "base/diag.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "base/buffer.h"
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

/* Whether two edits of one fix can't both apply against the original text: they change a byte in
 * common, one inserts inside the other's span, or both insert at the same place, where their
 * order would be unclear. */
static bool overlap(const struct mo_edit *a, const struct mo_edit *b) {
    if (a->span.file != b->span.file) {
        return false;
    }
    bool a_inserts = a->span.start == a->span.end;
    bool b_inserts = b->span.start == b->span.end;
    if (a_inserts && b_inserts) {
        return a->span.start == b->span.start;
    }
    return a->span.start < b->span.end && b->span.start < a->span.end;
}

static void check_fix(const struct mo_fix *fix) {
    MO_ASSERT(fix->edit_count >= 1 && fix->edit_count <= MO_LIMIT_FIX_EDITS);
    MO_ASSERT(fix->edits != NULL);
    for (uint32_t i = 0; i < fix->edit_count; i++) {
        check_span(fix->edits[i].span);
        MO_ASSERT(fix->edits[i].replacement != NULL);
        /* Every pair, which MO_LIMIT_FIX_EDITS keeps to half a million. */
        for (uint32_t j = 0; j < i; j++) {
            MO_ASSERT(!overlap(&fix->edits[i], &fix->edits[j]));
        }
    }
}

/* Checks what every diagnostic must be, whether it is kept or not. */
static void check_diag(const struct mo_diag *diag, const struct mo_code_info *code) {
    MO_ASSERT(code != NULL);
    MO_ASSERT(diag->section == NULL ||
              (code->section != NULL && strcmp(diag->section, code->section) == 0));
    MO_ASSERT(diag->message != NULL);
    MO_ASSERT(diag->reason != NULL || code->section == NULL);
    check_span(diag->primary.span);
    MO_ASSERT(diag->secondary_count == 0 || diag->secondary != NULL);
    for (uint32_t i = 0; i < diag->secondary_count; i++) {
        check_span(diag->secondary[i].span);
    }
    MO_ASSERT(diag->fix_count == 0 || diag->fixes != NULL);
    for (uint32_t i = 0; i < diag->fix_count; i++) {
        check_fix(&diag->fixes[i]);
    }
}

/* Adds more to *size. False when the sum doesn't fit in a size_t. */
static bool count_size(size_t *size, size_t more) {
    if (more > SIZE_MAX - *size) {
        return false;
    }
    *size += more;
    return true;
}

/* Adds to *size the bytes a copy of text takes, with its NUL, and nothing for NULL. */
static bool count_text(size_t *size, const char *text) {
    return text == NULL || count_size(size, strlen(text) + 1);
}

/* Copies text to *place and moves *place past the copy. NULL stays NULL. */
static const char *place_text(char **place, const char *text) {
    if (text == NULL) {
        return NULL;
    }
    size_t size = strlen(text) + 1;
    memcpy(*place, text, size);
    const char *copy = *place;
    *place += size;
    return copy;
}

_Static_assert(_Alignof(struct mo_fix) == _Alignof(struct mo_label) &&
                   _Alignof(struct mo_edit) == _Alignof(struct mo_label),
               "a diagnostic's labels, fixes and edits can follow each other in one block");

/* The bytes a copy of diag takes in one block: its labels, its fixes, their edits, and then its
 * strings. False when they don't fit in a size_t. */
static bool count_diag(const struct mo_diag *diag, size_t *size_out) {
    /* No count passes UINT32_MAX, so on a 64-bit platform no product overflows. */
    size_t size = (size_t)diag->secondary_count * sizeof(struct mo_label);
    bool counted = count_size(&size, (size_t)diag->fix_count * sizeof(struct mo_fix)) &&
                   count_text(&size, diag->primary.text) && count_text(&size, diag->message) &&
                   count_text(&size, diag->reason);
    for (uint32_t i = 0; counted && i < diag->secondary_count; i++) {
        counted = count_text(&size, diag->secondary[i].text);
    }
    for (uint32_t i = 0; counted && i < diag->fix_count; i++) {
        const struct mo_fix *fix = &diag->fixes[i];
        counted = count_size(&size, (size_t)fix->edit_count * sizeof(struct mo_edit)) &&
                  count_text(&size, fix->title);
        for (uint32_t j = 0; counted && j < fix->edit_count; j++) {
            counted = count_text(&size, fix->edits[j].replacement);
        }
    }
    *size_out = size;
    return counted;
}

/* Fills *copy with diag, its labels, fixes, edits and strings copied into one block of the arena,
 * so that a diagnostic is kept whole or takes nothing. False when the block doesn't fit. */
static bool copy_diag(struct mo_arena *arena, const struct mo_diag *diag, const char *section,
                      struct mo_diag *copy) {
    size_t size = 0;
    void *block =
        count_diag(diag, &size) ? mo_arena_allocate(arena, size, _Alignof(struct mo_label)) : NULL;
    if (block == NULL) {
        return false;
    }
    /* The labels, then the fixes, then the edits of each fix in turn, then the strings. These are
     * parts of size, so none of them overflows. */
    size_t labels_size = (size_t)diag->secondary_count * sizeof(struct mo_label);
    size_t fixes_size = (size_t)diag->fix_count * sizeof(struct mo_fix);
    size_t edits_size = 0;
    for (uint32_t i = 0; i < diag->fix_count; i++) {
        edits_size += (size_t)diag->fixes[i].edit_count * sizeof(struct mo_edit);
    }
    struct mo_label *labels = block;
    void *fixes_place = (char *)block + labels_size;
    struct mo_fix *fixes = fixes_place;
    void *edits_place = (char *)fixes_place + fixes_size;
    struct mo_edit *edits = edits_place;
    char *strings = (char *)edits_place + edits_size;

    *copy = (struct mo_diag){.code = diag->code, .section = section};
    copy->primary.span = diag->primary.span;
    copy->primary.text = place_text(&strings, diag->primary.text);
    copy->message = place_text(&strings, diag->message);
    copy->reason = place_text(&strings, diag->reason);
    for (uint32_t i = 0; i < diag->secondary_count; i++) {
        labels[i].span = diag->secondary[i].span;
        labels[i].text = place_text(&strings, diag->secondary[i].text);
    }
    if (diag->secondary_count > 0) {
        copy->secondary = labels;
        copy->secondary_count = diag->secondary_count;
    }
    for (uint32_t i = 0; i < diag->fix_count; i++) {
        const struct mo_fix *fix = &diag->fixes[i];
        fixes[i].title = place_text(&strings, fix->title);
        fixes[i].edits = edits;
        fixes[i].edit_count = fix->edit_count;
        for (uint32_t j = 0; j < fix->edit_count; j++) {
            edits[j].span = fix->edits[j].span;
            edits[j].replacement = place_text(&strings, fix->edits[j].replacement);
        }
        edits += fix->edit_count;
    }
    if (diag->fix_count > 0) {
        copy->fixes = fixes;
        copy->fix_count = diag->fix_count;
    }
    MO_ASSERT((void *)edits == (char *)edits_place + edits_size);
    MO_ASSERT(strings == (char *)block + size);
    return true;
}

bool mo_diags_add(struct mo_diags *diags, const struct mo_diag *diag) {
    MO_ASSERT(diags != NULL && diags->items != NULL && diag != NULL);
    MO_ASSERT(diags->count <= MO_LIMIT_DIAGNOSTICS);
    const struct mo_code_info *code = mo_code_find(diag->code);
    check_diag(diag, code);
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

/* The memory the source files may take. Each file takes one block: its line table, at four bytes
 * a line, its bytes and a NUL, and its path. MO_LIMIT_PROGRAM_SIZE bytes of source, with as many
 * lines as they could have, MO_LIMIT_FILES paths and the table of files take less than this, twice
 * over, since an arena may leave as much unused as it hands out, and with room for the newest
 * chunk's unused end, a mebibyte at most (src/base/arena.c). So the limits on source files, not
 * this one, decide what a program may hold. */
#define SOURCES_ARENA_LIMIT ((size_t)1 << 30)
_Static_assert(2 * (5 * ((size_t)MO_LIMIT_PROGRAM_SIZE + MO_LIMIT_FILES) +
                    (size_t)MO_LIMIT_FILES * (MO_LIMIT_PATH_SIZE + 2 + sizeof(struct mo_source))) +
                       ((size_t)1 << 20) <=
                   SOURCES_ARENA_LIMIT,
               "the sources' arena holds the largest program the limits allow");

/* Files are read in pieces this big. */
enum { READ_SIZE = 16 * 1024 };

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

/* Adds a file of length bytes, once check_room has passed it. The file takes one block of the
 * arena, which holds its line table, its bytes with a NUL after them, and its path, so it is added
 * whole or takes nothing. */
static bool add_file(struct mo_sources *sources, const char *path, const uint8_t *bytes,
                     size_t length, uint32_t *file_out, struct mo_source_error *error_out) {
    MO_ASSERT(bytes != NULL || length == 0);
    if (length > MO_LIMIT_FILE_SIZE) {
        return refuse(error_out, MO_SOURCE_TOO_BIG, 0);
    }
    if (length > MO_LIMIT_PROGRAM_SIZE - sources->size) {
        return refuse(error_out, MO_SOURCE_PROGRAM_TOO_BIG, 0);
    }
    uint32_t size = (uint32_t)length;
    uint32_t line_count = find_lines(bytes, size, NULL);
    size_t table_size = (size_t)line_count * sizeof(uint32_t);
    size_t path_size = strlen(path) + 1;
    void *block =
        mo_arena_allocate(&sources->arena, table_size + length + 1 + path_size, _Alignof(uint32_t));
    if (block == NULL) {
        return refuse(error_out, MO_SOURCE_NO_MEMORY, 0);
    }
    uint32_t *line_starts = block;
    uint8_t *copy = (uint8_t *)block + table_size;
    char *path_copy = (char *)copy + length + 1;
    if (length > 0) {
        memcpy(copy, bytes, length);
    }
    copy[length] = 0;
    memcpy(path_copy, path, path_size);
    uint32_t found = find_lines(copy, size, line_starts);
    MO_ASSERT(found == line_count);
    uint32_t file = sources->count;
    sources->files[file] = (struct mo_source){.path = path_copy,
                                              .bytes = copy,
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
    return check_room(sources, path, error_out) &&
           add_file(sources, path, bytes, length, file_out, error_out);
}

/* Reads a whole file into scratch, up to MO_LIMIT_FILE_SIZE bytes. False, saying why in
 * *error_out, when it can't. */
static bool read_file(const char *path, struct mo_buffer *scratch,
                      struct mo_source_error *error_out) {
    errno = 0;
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return refuse(error_out, MO_SOURCE_UNREADABLE, errno);
    }
    uint8_t piece[READ_SIZE];
    struct mo_source_error error = {.problem = MO_SOURCE_UNREADABLE, .system_error = 0};
    bool complete = false;
    for (;;) {
        errno = 0;
        size_t got = fread(piece, 1, sizeof piece, file);
        int read_error = errno;
        if (got > MO_LIMIT_FILE_SIZE - scratch->length) {
            error.problem = MO_SOURCE_TOO_BIG;
            break;
        }
        if (!mo_buffer_append(scratch, piece, got)) {
            error.problem = MO_SOURCE_NO_MEMORY;
            break;
        }
        if (got < sizeof piece) {
            if (ferror(file) != 0) {
                error.system_error = read_error;
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
        *error_out = error;
    }
    return complete;
}

bool mo_sources_add(struct mo_sources *sources, const char *path, uint32_t *file_out,
                    struct mo_source_error *error_out) {
    MO_ASSERT(sources != NULL && path != NULL && file_out != NULL && error_out != NULL);
    if (!check_room(sources, path, error_out)) {
        return false;
    }
    /* The file is read outside the sources' arena, so a file that is refused takes nothing from
     * it. */
    struct mo_buffer scratch;
    mo_buffer_init(&scratch, MO_LIMIT_FILE_SIZE);
    bool added = read_file(path, &scratch, error_out) &&
                 add_file(sources, path, scratch.bytes, scratch.length, file_out, error_out);
    mo_buffer_free(&scratch);
    return added;
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
