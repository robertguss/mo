/* The tests write files, so they use POSIX for a temporary folder. mkdtemp is
 * declared in <unistd.h> on macOS (and <stdlib.h> on Linux), and macOS hides it
 * under _POSIX_C_SOURCE alone, so Darwin also needs its own source macro. */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif

#include "base/diag.h"
#include "base/limits.h"
#include "test.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Adds text as a file named test.mo, and returns its index. */
static uint32_t add_text(struct mo_sources *sources, const char *text, size_t length) {
    uint32_t file = UINT32_MAX;
    struct mo_source_error error;
    CHECK(mo_sources_add_bytes(sources, "test.mo", (const uint8_t *)text, length, &file, &error));
    return file;
}

static bool at(const struct mo_sources *sources, uint32_t file, uint32_t offset, uint32_t line,
               uint32_t column) {
    struct mo_position position = mo_sources_position(sources, file, offset);
    if (position.line == line && position.column == column) {
        return true;
    }
    (void)fprintf(stderr, "offset %u: expected %u:%u, got %u:%u\n", (unsigned)offset,
                  (unsigned)line, (unsigned)column, (unsigned)position.line,
                  (unsigned)position.column);
    return false;
}

/* Checks the line and column of every offset of a text, from its first byte to its end. */
static void check_positions(const char *text, size_t length, const struct mo_position *expected) {
    struct mo_sources sources;
    CHECK(mo_sources_init(&sources));
    uint32_t file = add_text(&sources, text, length);
    for (uint32_t offset = 0; offset <= length; offset++) {
        CHECK(at(&sources, file, offset, expected[offset].line, expected[offset].column));
    }
    mo_sources_free(&sources);
}

#define POSITIONS(text, ...)                                                                       \
    do {                                                                                           \
        const struct mo_position expected[] = {__VA_ARGS__};                                       \
        CHECK(sizeof expected / sizeof expected[0] == sizeof text);                                \
        check_positions(text, sizeof text - 1, expected);                                          \
    } while (0)

static void test_lines_end_with_either_ending(void) {
    POSITIONS("ab\ncd", {1, 1}, {1, 2}, {1, 3}, {2, 1}, {2, 2}, {2, 3});
    /* The "\r" of a "\r\n" is part of the line ending: it has the column of the "\n". */
    POSITIONS("ab\r\ncd", {1, 1}, {1, 2}, {1, 3}, {1, 3}, {2, 1}, {2, 2}, {2, 3});
    /* Empty lines, of both endings, and a final line ending. */
    POSITIONS("a\r\n\r\n\nb\n", {1, 1}, {1, 2}, {1, 2}, {2, 1}, {2, 1}, {3, 1}, {4, 1}, {4, 2},
              {5, 1});
    /* A "\r" alone stays in its line, as a character. */
    POSITIONS("a\rb\r", {1, 1}, {1, 2}, {1, 3}, {1, 4}, {1, 5});
    POSITIONS("", {1, 1});
    POSITIONS("\n", {1, 1}, {2, 1});
    POSITIONS("\r\n", {1, 1}, {1, 1}, {2, 1});
}

static void test_a_tab_is_one_column(void) {
    POSITIONS("\tx\t\ty", {1, 1}, {1, 2}, {1, 3}, {1, 4}, {1, 5}, {1, 6});
    POSITIONS("a\n\t\tb", {1, 1}, {1, 2}, {2, 1}, {2, 2}, {2, 3}, {2, 4});
}

static void test_columns_count_characters(void) {
    /* é is 2 bytes, 中 3 and 😀 4; an offset inside one has its column. */
    POSITIONS("\xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80x", {1, 1}, {1, 1}, {1, 2}, {1, 2}, {1, 2},
              {1, 3}, {1, 3}, {1, 3}, {1, 3}, {1, 4}, {1, 5});
    POSITIONS("\xe2\x82\xac\r\n\xc3\xa9", {1, 1}, {1, 1}, {1, 1}, {1, 2}, {1, 2}, {2, 1}, {2, 1},
              {2, 2});
}

static void test_a_byte_outside_utf8_is_one_column(void) {
    /* Stray bytes. */
    POSITIONS("\xff\x80"
              "a",
              {1, 1}, {1, 2}, {1, 3}, {1, 4});
    /* The first two bytes of a three-byte character, then a. */
    POSITIONS("\xe4\xb8"
              "a",
              {1, 1}, {1, 2}, {1, 3}, {1, 4});
    /* A sequence cut off by the end of the file. */
    POSITIONS("a\xf0\x9f\x98", {1, 1}, {1, 2}, {1, 3}, {1, 4}, {1, 5});
    /* Too long a form of NUL, a surrogate, and a code point past U+10FFFF. */
    POSITIONS("\xc0\x80", {1, 1}, {1, 2}, {1, 3});
    POSITIONS("\xed\xa0\x80", {1, 1}, {1, 2}, {1, 3}, {1, 4});
    POSITIONS("\xf4\x90\x80\x80", {1, 1}, {1, 2}, {1, 3}, {1, 4}, {1, 5});
    /* The last character and the last code point are valid. */
    POSITIONS("\xef\xbf\xbf\xf4\x8f\xbf\xbf", {1, 1}, {1, 1}, {1, 1}, {1, 2}, {1, 2}, {1, 2},
              {1, 2}, {1, 3});
}

/* The line and column of an offset, computed the slow way: from the start of the file, counting
 * the lead bytes of characters, for text that is valid UTF-8. */
static struct mo_position slow_position(const uint8_t *text, size_t length, size_t offset) {
    struct mo_position position = {.line = 1, .column = 1};
    for (size_t i = 0; i < offset; i++) {
        uint8_t byte = text[i];
        if (byte == '\n') {
            position.line += 1;
            position.column = 1;
        } else if ((byte & 0xC0u) != 0x80u &&
                   !(byte == '\r' && i + 1 < length && text[i + 1] == '\n')) {
            /* A character starts here. It counts if it ends by the offset. */
            size_t size = byte < 0x80u ? 1 : byte >= 0xF0u ? 4 : byte >= 0xE0u ? 3 : 2;
            if (i + size <= offset) {
                position.column += 1;
            }
        }
    }
    return position;
}

static void test_every_offset_of_a_long_text(void) {
    /* Lines of every kind, repeated: the line table has many lines to search. */
    const char *pieces[] = {"fn main(args: Args, console: Console)\n",
                            "\tconsole.print(\"é\")\r\n",
                            "\n",
                            "  x = \"中文 😀\"\r\n",
                            "\r\n",
                            "# naïve café\n",
                            "end"};
    static struct {
        uint8_t bytes[20000];
        size_t length;
    } text;
    text.length = 0;
    for (size_t round = 0; text.length < sizeof text.bytes - 100; round++) {
        const char *piece = pieces[round % (sizeof pieces / sizeof pieces[0])];
        size_t size = strlen(piece);
        memcpy(text.bytes + text.length, piece, size);
        text.length += size;
    }
    struct mo_sources sources;
    CHECK(mo_sources_init(&sources));
    uint32_t file = add_text(&sources, (const char *)text.bytes, text.length);
    bool agree = true;
    for (size_t offset = 0; agree && offset <= text.length; offset++) {
        struct mo_position slow = slow_position(text.bytes, text.length, offset);
        agree = at(&sources, file, (uint32_t)offset, slow.line, slow.column);
    }
    CHECK(agree);
    struct mo_position end = slow_position(text.bytes, text.length, text.length);
    CHECK(sources.files[file].line_count == end.line);
    mo_sources_free(&sources);
}

static bool line_is(const struct mo_sources *sources, uint32_t file, uint32_t line,
                    const char *text) {
    struct mo_span span = mo_sources_line(sources, file, line);
    const struct mo_source *source = &sources->files[file];
    size_t length = strlen(text);
    return span.file == file && span.end >= span.start && span.end - span.start == length &&
           memcmp(source->bytes + span.start, text, length) == 0;
}

static void test_the_text_of_lines(void) {
    struct mo_sources sources;
    CHECK(mo_sources_init(&sources));
    const char text[] = "first\r\n\nthird\n\r\n\rsixth\r";
    uint32_t file = add_text(&sources, text, sizeof text - 1);
    CHECK(sources.files[file].line_count == 5);
    CHECK(line_is(&sources, file, 1, "first"));
    CHECK(line_is(&sources, file, 2, ""));
    CHECK(line_is(&sources, file, 3, "third"));
    CHECK(line_is(&sources, file, 4, ""));
    /* A "\r" alone stays in its line, at its start or its end. */
    CHECK(line_is(&sources, file, 5, "\rsixth\r"));
    uint32_t empty = add_text(&sources, "", 0);
    CHECK(sources.files[empty].line_count == 1 && line_is(&sources, empty, 1, ""));
    uint32_t ended = add_text(&sources, "a\n", 2);
    CHECK(sources.files[ended].line_count == 2);
    CHECK(line_is(&sources, ended, 1, "a") && line_is(&sources, ended, 2, ""));
    mo_sources_free(&sources);
}

static void test_files_keep_their_path_and_bytes(void) {
    struct mo_sources sources;
    CHECK(mo_sources_init(&sources));
    char path[] = "shop/orders.mo";
    char text[] = "module Shop.Orders\n";
    uint32_t file = UINT32_MAX;
    struct mo_source_error error;
    CHECK(mo_sources_add_bytes(&sources, path, (const uint8_t *)text, strlen(text), &file, &error));
    CHECK(file == 0 && sources.count == 1 && sources.size == strlen(text));
    memset(path, 'x', sizeof path - 1);
    memset(text, 'x', sizeof text - 1);
    const struct mo_source *source = &sources.files[0];
    CHECK(strcmp(source->path, "shop/orders.mo") == 0);
    CHECK(source->length == 19 && memcmp(source->bytes, "module Shop.Orders\n", 19) == 0);
    CHECK(source->bytes[source->length] == '\0');
    uint32_t second = UINT32_MAX;
    CHECK(mo_sources_add_bytes(&sources, "b.mo", NULL, 0, &second, &error));
    CHECK(second == 1 && sources.files[1].length == 0 && sources.files[1].bytes[0] == '\0');
    mo_sources_free(&sources);
    CHECK(sources.count == 0 && sources.files == NULL);
}

/* A temporary folder for the files the tests read. */
static char folder[256];

static bool make_folder(void) {
    const char *temporary = getenv("TMPDIR");
    if (temporary == NULL || temporary[0] == '\0') {
        temporary = "/tmp";
    }
    int written = snprintf(folder, sizeof folder, "%s/mo-test-sources-XXXXXX", temporary);
    return written > 0 && (size_t)written < sizeof folder && mkdtemp(folder) != NULL;
}

/* The path of a file in the folder. */
static const char *path_of(const char *name) {
    static char path[512];
    int written = snprintf(path, sizeof path, "%s/%s", folder, name);
    MO_ASSERT(written > 0 && (size_t)written < sizeof path);
    return path;
}

/* Writes size bytes to a file in the folder: the text, repeated. */
static bool write_file(const char *name, const char *text, size_t size) {
    FILE *file = fopen(path_of(name), "wb");
    if (file == NULL) {
        return false;
    }
    size_t length = strlen(text);
    bool written = true;
    for (size_t done = 0; written && done < size;) {
        size_t piece = size - done < length ? size - done : length;
        written = fwrite(text, 1, piece, file) == piece;
        done += piece;
    }
    return fclose(file) == 0 && written;
}

static void test_reading_files(void) {
    struct mo_sources sources;
    CHECK(mo_sources_init(&sources));
    struct mo_source_error error;
    uint32_t file = UINT32_MAX;

    const char text[] =
        "module Main\r\n\r\nfn main(console: Console)\r\n  console.print(\"hé\")\r\nend\r\n";
    CHECK(write_file("main.mo", text, sizeof text - 1));
    CHECK(mo_sources_add(&sources, path_of("main.mo"), &file, &error));
    CHECK(file == 0);
    const struct mo_source *source = &sources.files[0];
    CHECK(strcmp(source->path, path_of("main.mo")) == 0);
    CHECK(source->length == sizeof text - 1 && memcmp(source->bytes, text, sizeof text - 1) == 0);
    CHECK(source->bytes[source->length] == '\0');
    CHECK(source->line_count == 6);
    CHECK(at(&sources, 0, 60, 4, 19)); /* the é */
    CHECK(line_is(&sources, 0, 4, "  console.print(\"hé\")"));

    /* An empty file, and one larger than the first block it is read into. */
    CHECK(write_file("empty.mo", "", 0));
    CHECK(mo_sources_add(&sources, path_of("empty.mo"), &file, &error));
    CHECK(file == 1 && sources.files[1].length == 0 && sources.files[1].line_count == 1);
    CHECK(write_file("long.mo", "# a comment of some length\n", 1000000));
    CHECK(mo_sources_add(&sources, path_of("long.mo"), &file, &error));
    CHECK(file == 2 && sources.files[2].length == 1000000);
    CHECK(sources.files[2].line_count == 1000000 / 27 + 1);
    CHECK(sources.size == sizeof text - 1 + 1000000);

    /* A file that isn't there, and a folder. */
    CHECK(!mo_sources_add(&sources, path_of("missing.mo"), &file, &error));
    CHECK(error.problem == MO_SOURCE_UNREADABLE && error.system_error == ENOENT);
    CHECK(!mo_sources_add(&sources, folder, &file, &error));
    CHECK(error.problem == MO_SOURCE_UNREADABLE && error.system_error != 0);
    CHECK(sources.count == 3);
    mo_sources_free(&sources);
}

static void test_a_file_over_the_limit_is_refused(void) {
    struct mo_sources sources;
    CHECK(mo_sources_init(&sources));
    struct mo_source_error error;
    uint32_t file = UINT32_MAX;
    CHECK(write_file("exact.mo", "x = 1\n", MO_LIMIT_FILE_SIZE));
    CHECK(write_file("over.mo", "x = 1\n", (size_t)MO_LIMIT_FILE_SIZE + 1));
    CHECK(mo_sources_add(&sources, path_of("exact.mo"), &file, &error));
    CHECK(file == 0 && sources.files[0].length == MO_LIMIT_FILE_SIZE);
    /* A refused file takes no memory from the sources, even between files that are kept. */
    CHECK(write_file("small.mo", "x = 1\n", 6));
    for (int round = 0; round < 5; round++) {
        size_t used = sources.arena.used;
        CHECK(!mo_sources_add(&sources, path_of("over.mo"), &file, &error));
        CHECK(error.problem == MO_SOURCE_TOO_BIG && sources.arena.used == used);
        CHECK(mo_sources_add(&sources, path_of("small.mo"), &file, &error));
    }
    CHECK(sources.count == 6 && sources.size == MO_LIMIT_FILE_SIZE + 5 * 6);
    /* The same from memory. */
    static uint8_t bytes[MO_LIMIT_FILE_SIZE + 1];
    CHECK(!mo_sources_add_bytes(&sources, "over.mo", bytes, sizeof bytes, &file, &error));
    CHECK(error.problem == MO_SOURCE_TOO_BIG);
    CHECK(mo_sources_add_bytes(&sources, "exact.mo", bytes, sizeof bytes - 1, &file, &error));
    CHECK(file == 6);
    mo_sources_free(&sources);
}

static void test_a_program_over_the_limit_is_refused(void) {
    struct mo_sources sources;
    CHECK(mo_sources_init(&sources));
    struct mo_source_error error;
    uint32_t file = UINT32_MAX;
    static uint8_t bytes[MO_LIMIT_FILE_SIZE];
    memset(bytes, 'a', sizeof bytes);
    for (int i = 0; i < MO_LIMIT_PROGRAM_SIZE / MO_LIMIT_FILE_SIZE; i++) {
        CHECK(mo_sources_add_bytes(&sources, "part.mo", bytes, sizeof bytes, &file, &error));
    }
    CHECK(sources.size == MO_LIMIT_PROGRAM_SIZE);
    CHECK(!mo_sources_add_bytes(&sources, "more.mo", bytes, 1, &file, &error));
    CHECK(error.problem == MO_SOURCE_PROGRAM_TOO_BIG);
    CHECK(write_file("one.mo", "x", 1));
    CHECK(!mo_sources_add(&sources, path_of("one.mo"), &file, &error));
    CHECK(error.problem == MO_SOURCE_PROGRAM_TOO_BIG);
    /* A refused file takes no memory from the sources. */
    size_t used = sources.arena.used;
    CHECK(!mo_sources_add(&sources, path_of("one.mo"), &file, &error));
    CHECK(sources.arena.used == used);
    /* An empty file adds nothing to the program's size. */
    CHECK(mo_sources_add_bytes(&sources, "empty.mo", bytes, 0, &file, &error));
    CHECK(sources.size == MO_LIMIT_PROGRAM_SIZE && sources.count == 9);
    mo_sources_free(&sources);
}

static void test_the_number_of_files_is_limited(void) {
    struct mo_sources sources;
    CHECK(mo_sources_init(&sources));
    struct mo_source_error error;
    uint32_t file = UINT32_MAX;
    bool added = true;
    for (uint32_t i = 0; added && i < MO_LIMIT_FILES; i++) {
        added = mo_sources_add_bytes(&sources, "a.mo", (const uint8_t *)"a", 1, &file, &error);
        added = added && file == i;
    }
    CHECK(added && sources.count == MO_LIMIT_FILES);
    CHECK(!mo_sources_add_bytes(&sources, "b.mo", (const uint8_t *)"b", 1, &file, &error));
    CHECK(error.problem == MO_SOURCE_TOO_MANY);
    CHECK(!mo_sources_add(&sources, path_of("main.mo"), &file, &error));
    CHECK(error.problem == MO_SOURCE_TOO_MANY);
    CHECK(sources.count == MO_LIMIT_FILES);
    mo_sources_free(&sources);
}

static void test_a_path_over_the_limit_is_refused(void) {
    struct mo_sources sources;
    CHECK(mo_sources_init(&sources));
    struct mo_source_error error;
    uint32_t file = UINT32_MAX;
    static char path[MO_LIMIT_PATH_SIZE + 2];
    memset(path, 'p', MO_LIMIT_PATH_SIZE + 1);
    CHECK(!mo_sources_add(&sources, path, &file, &error));
    CHECK(error.problem == MO_SOURCE_PATH_TOO_LONG);
    CHECK(!mo_sources_add_bytes(&sources, path, (const uint8_t *)"a", 1, &file, &error));
    CHECK(error.problem == MO_SOURCE_PATH_TOO_LONG);
    path[MO_LIMIT_PATH_SIZE] = '\0';
    CHECK(mo_sources_add_bytes(&sources, path, (const uint8_t *)"a", 1, &file, &error));
    CHECK(strlen(sources.files[file].path) == MO_LIMIT_PATH_SIZE);
    mo_sources_free(&sources);
}

static void remove_files(void) {
    const char *names[] = {"main.mo", "empty.mo", "long.mo", "exact.mo",
                           "over.mo", "small.mo", "one.mo"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        (void)remove(path_of(names[i]));
    }
    CHECK(remove(folder) == 0);
}

int main(void) {
    test_lines_end_with_either_ending();
    test_a_tab_is_one_column();
    test_columns_count_characters();
    test_a_byte_outside_utf8_is_one_column();
    test_every_offset_of_a_long_text();
    test_the_text_of_lines();
    test_files_keep_their_path_and_bytes();
    test_a_path_over_the_limit_is_refused();
    if (make_folder()) {
        test_reading_files();
        test_a_file_over_the_limit_is_refused();
        test_a_program_over_the_limit_is_refused();
        test_the_number_of_files_is_limited();
        remove_files();
    } else {
        (void)fprintf(stderr, "test_sources: can't make a temporary folder\n");
        mo_test_failures++;
    }
    return mo_test_report("test_sources");
}
