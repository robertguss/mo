/* Fuzzes source files' lines and columns. Whatever bytes arrive, every offset's line and column
 * must agree with a second count, made in one pass from the start of the file, that decodes UTF-8
 * by the Unicode Standard's table of well-formed byte sequences (table 3-7). And the text of each
 * line must be the bytes between its line endings. */
#include "base/diag.h"
#include "base/limits.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* Each offset's position is checked, and each costs a walk along its line, so longer inputs only
 * slow the fuzzer down. */
enum { MOST_BYTES = 1024 };

/* The length of the well-formed UTF-8 sequence at data[at], or 0 if there is none: table 3-7 of
 * the Unicode Standard, row by row. */
static size_t well_formed(const uint8_t *data, size_t size, size_t at) {
    static const struct {
        uint8_t first_low, first_high, second_low, second_high;
        size_t length;
    } rows[] = {
        {0x00, 0x7F, 0x00, 0x00, 1}, {0xC2, 0xDF, 0x80, 0xBF, 2}, {0xE0, 0xE0, 0xA0, 0xBF, 3},
        {0xE1, 0xEC, 0x80, 0xBF, 3}, {0xED, 0xED, 0x80, 0x9F, 3}, {0xEE, 0xEF, 0x80, 0xBF, 3},
        {0xF0, 0xF0, 0x90, 0xBF, 4}, {0xF1, 0xF3, 0x80, 0xBF, 4}, {0xF4, 0xF4, 0x80, 0x8F, 4},
    };
    uint8_t first = data[at];
    for (size_t r = 0; r < sizeof rows / sizeof rows[0]; r++) {
        if (first < rows[r].first_low || first > rows[r].first_high) {
            continue;
        }
        size_t length = rows[r].length;
        if (length == 1) {
            return 1;
        }
        if (size - at < length || data[at + 1] < rows[r].second_low ||
            data[at + 1] > rows[r].second_high) {
            return 0;
        }
        for (size_t i = 2; i < length; i++) {
            if (data[at + i] < 0x80 || data[at + i] > 0xBF) {
                return 0;
            }
        }
        return length;
    }
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > MOST_BYTES) {
        size = MOST_BYTES;
    }
    struct mo_sources sources;
    bool ready = mo_sources_init(&sources);
    MO_ASSERT(ready);
    uint32_t file = 0;
    struct mo_source_error error;
    bool added = mo_sources_add_bytes(&sources, "fuzz.mo", data, size, &file, &error);
    MO_ASSERT(added);
    const struct mo_source *source = &sources.files[file];
    MO_ASSERT(source->length == size && source->bytes[size] == 0);

    /* One pass, a character at a time. Every offset inside a character has its position, and a
     * character counts one column unless it is the "\r" of a "\r\n". */
    uint32_t line = 1;
    uint32_t column = 1;
    size_t at = 0;
    while (at <= size) {
        size_t length = 1;
        if (at < size) {
            size_t formed = well_formed(data, size, at);
            length = formed == 0 ? 1 : formed;
        }
        for (size_t offset = at; offset < at + length; offset++) {
            struct mo_position position = mo_sources_position(&sources, file, (uint32_t)offset);
            MO_ASSERT(position.line == line && position.column == column);
        }
        if (at == size) {
            break;
        }
        if (data[at] == '\n') {
            MO_ASSERT(source->line_starts[line] == at + 1);
            line += 1;
            column = 1;
        } else if (!(data[at] == '\r' && at + 1 < size && data[at + 1] == '\n')) {
            column += 1;
        }
        at += length;
    }
    MO_ASSERT(source->line_count == line);

    /* Each line's text runs from its start to its line ending, which it doesn't include. */
    for (uint32_t number = 1; number <= source->line_count; number++) {
        struct mo_span text = mo_sources_line(&sources, file, number);
        MO_ASSERT(text.file == file && text.start == source->line_starts[number - 1]);
        MO_ASSERT(text.start <= text.end && text.end <= size);
        for (uint32_t i = text.start; i < text.end; i++) {
            MO_ASSERT(data[i] != '\n');
        }
        if (number < source->line_count) {
            uint32_t next = source->line_starts[number];
            bool crlf = next >= 2 && next - 2 >= text.start && data[next - 2] == '\r';
            MO_ASSERT(text.end == next - (crlf ? 2u : 1u));
        } else {
            MO_ASSERT(text.end == size);
        }
    }
    mo_sources_free(&sources);
    return 0;
}
