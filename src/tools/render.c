#define _POSIX_C_SOURCE 200809L

#include "tools/tools.h"

#include "prim/prim.h"
#include "tools/promises.h"
#include "tools/run.h"
#include "tools/tests.h"
#include "vm/vm.h"

#include <langinfo.h>
#include <string.h>

struct writer {
    struct mo_buffer *out;
    bool ok;
    bool utf8;
};

static void bytes(struct writer *w, const void *data, size_t length) {
    if (w->ok)
        w->ok = mo_buffer_append(w->out, data, length);
}
static void text(struct writer *w, const char *s) {
    if (w->ok)
        w->ok = mo_buffer_append_text(w->out, s);
}
static void unsigned_number(struct writer *w, uint64_t n) {
    if (w->ok)
        w->ok = mo_buffer_append_uint(w->out, n);
}
static void repeat(struct writer *w, char c, uint32_t count) {
    for (uint32_t i = 0; w->ok && i < count; i++)
        bytes(w, &c, 1);
}

/* A valid scalar takes 1..4 bytes; invalid UTF-8 consumes exactly one byte.
 * Unlike locale-dependent multibyte APIs this agrees with diag.h everywhere. */
static uint32_t scalar(const uint8_t *s, size_t length, uint32_t *value) {
    if (s[0] < 0x80) {
        *value = s[0];
        return 1;
    }
    uint32_t n = s[0] >= 0xC2 && s[0] <= 0xDF   ? 2u
                 : s[0] >= 0xE0 && s[0] <= 0xEF ? 3u
                 : s[0] >= 0xF0 && s[0] <= 0xF4 ? 4u
                                                : 0u;
    if (n != 0 && length >= n) {
        uint32_t v = s[0] & (0x7Fu >> n);
        for (uint32_t i = 1; i < n; i++) {
            if ((s[i] & 0xC0) != 0x80)
                goto invalid;
            v = (v << 6) | (s[i] & 0x3F);
        }
        if ((n == 2 && v >= 0x80) || (n == 3 && v >= 0x800) || (n == 4 && v >= 0x10000)) {
            if (v <= 0x10FFFF && !(v >= 0xD800 && v <= 0xDFFF)) {
                *value = v;
                return n;
            }
        }
    }
invalid:
    *value = UINT32_MAX;
    return 1;
}

static void hex(struct writer *w, uint32_t n, unsigned digits) {
    if (w->ok)
        w->ok = mo_buffer_append_hex(w->out, n, digits);
}

static bool escaped(uint32_t c);

static void json_bytes(struct writer *w, const uint8_t *s, size_t length) {
    text(w, "\"");
    for (size_t i = 0; w->ok && i < length;) {
        uint32_t value;
        uint32_t n = scalar(s + i, length - i, &value);
        if (s[i] == '"' || s[i] == '\\') {
            text(w, "\\");
            bytes(w, s + i, 1);
        } else if (escaped(value)) {
            if (value == UINT32_MAX)
                value = (uint8_t)s[i];
            text(w, "\\u");
            if (value <= 0xffff)
                hex(w, value, 4);
            else {
                value -= 0x10000;
                hex(w, 0xd800 + (value >> 10), 4);
                text(w, "\\u");
                hex(w, 0xdc00 + (value & 0x3ff), 4);
            }
        } else {
            bytes(w, s + i, n);
        }
        i += n;
    }
    text(w, "\"");
}

static void json_string(struct writer *w, const char *s) {
    if (s == NULL)
        text(w, "null");
    else
        json_bytes(w, (const uint8_t *)s, strlen(s));
}

static bool available(const struct mo_sources *sources, struct mo_span span) {
    return span.file < sources->count && span.start <= span.end &&
           span.end <= sources->files[span.file].length;
}

static void json_span(struct writer *w, const struct mo_sources *sources, struct mo_span span,
                      const char *label, bool labeled) {
    text(w, "{\"file\":");
    bool found = available(sources, span);
    json_string(w, found ? sources->files[span.file].path : NULL);
    text(w, ",\"start\":");
    unsigned_number(w, span.start);
    text(w, ",\"end\":");
    unsigned_number(w, span.end);
    struct mo_position start = {0}, end = {0};
    if (found) {
        start = mo_sources_position(sources, span.file, span.start);
        end = mo_sources_position(sources, span.file, span.end);
    }
    const char *keys[] = {",\"line\":", ",\"column\":", ",\"end_line\":", ",\"end_column\":"};
    uint32_t values[] = {start.line, start.column, end.line, end.column};
    for (unsigned i = 0; i < 4; i++) {
        text(w, keys[i]);
        if (found)
            unsigned_number(w, values[i]);
        else
            text(w, "null");
    }
    if (labeled) {
        text(w, ",\"label\":");
        json_string(w, label);
    }
    text(w, "}");
}

static void code(struct writer *w, uint16_t n) {
    text(w, "E");
    if (n < 1000)
        text(w, "0");
    if (n < 100)
        text(w, "0");
    if (n < 10)
        text(w, "0");
    unsigned_number(w, n);
}

static void json_diag(struct writer *w, const struct mo_sources *sources, const struct mo_diag *d) {
    text(w, "{\"record\":\"diagnostic\",\"code\":\"");
    code(w, d->code);
    text(w, "\",\"headline\":");
    json_string(w, d->message);
    text(w, ",\"reason\":");
    json_string(w, d->section == NULL ? NULL : d->reason);
    text(w, ",\"section\":");
    json_string(w, d->section);
    text(w, ",\"primary\":");
    json_span(w, sources, d->primary.span, d->primary.text, true);
    text(w, ",\"secondary\":[");
    for (uint32_t i = 0; w->ok && i < d->secondary_count; i++) {
        if (i != 0)
            text(w, ",");
        json_span(w, sources, d->secondary[i].span, d->secondary[i].text, true);
    }
    text(w, "],\"fixes\":[");
    for (uint32_t i = 0; w->ok && i < d->fix_count; i++) {
        if (i != 0)
            text(w, ",");
        const struct mo_fix *f = &d->fixes[i];
        text(w, "{\"title\":");
        json_string(w, f->title);
        text(w, ",\"edits\":[");
        for (uint32_t j = 0; w->ok && j < f->edit_count; j++) {
            if (j != 0)
                text(w, ",");
            text(w, "{\"span\":");
            json_span(w, sources, f->edits[j].span, NULL, false);
            text(w, ",\"text\":");
            json_string(w, f->edits[j].replacement);
            text(w, "}");
        }
        text(w, "]}");
    }
    text(w, "]}\n");
}

/* Reference 2.1's refused characters, plus line controls: never let a source
 * byte move the terminal cursor or change the surrounding text's direction. */
static bool escaped(uint32_t c) {
    return c < 32 || c == 127 || (c >= 128 && c <= 159) || c == UINT32_MAX || c == 0xad ||
           c == 0x61c || c == 0x180e || (c >= 0x200b && c <= 0x200f) ||
           (c >= 0x2028 && c <= 0x202e) || (c >= 0x2060 && c <= 0x2064) ||
           (c >= 0x2066 && c <= 0x206f) || c == 0xfeff || (c >= 0xfff9 && c <= 0xfffb) ||
           c == 0xe0001 || (c >= 0xe0020 && c <= 0xe007f);
}

static uint32_t plain_width(const uint8_t *s, size_t length) {
    uint64_t width;
    /* Called only on validated UTF-8 runs. Saturating is sufficient for
     * oversized diagnostic strings: the bounded writer will refuse them. */
    bool ok = mo_unicode16_display_width(s, length, &width);
    MO_ASSERT(ok);
    return width > UINT32_MAX ? UINT32_MAX : (uint32_t)width;
}

struct display_piece {
    size_t used;
    uint32_t width, value;
    bool plain;
};

struct display_cursor {
    const uint8_t *bytes;
    size_t length, at, safe_end;
};

/* Cache the validated run's end: finding it anew for each cluster would make
 * ASCII rows quadratic. A piece is one whole cluster or one escape/tab. */
static struct display_piece inspect(struct display_cursor *c, uint32_t cells, bool utf8) {
    const uint8_t *s = c->bytes + c->at;
    size_t length = c->length - c->at;
    struct display_piece p = {0};
    p.used = scalar(s, length, &p.value);
    if (!escaped(p.value) && (utf8 || p.value < 128)) {
        p.plain = true;
        if (c->safe_end <= c->at) {
            c->safe_end = c->at + p.used;
            while (c->safe_end < c->length) {
                uint32_t value;
                uint32_t n = scalar(c->bytes + c->safe_end, c->length - c->safe_end, &value);
                if (escaped(value) || (!utf8 && value >= 128))
                    break;
                c->safe_end += n;
            }
        }
        bool ok = mo_unicode16_next_grapheme(s, c->safe_end - c->at, &p.used);
        MO_ASSERT(ok);
        p.width = plain_width(s, p.used);
    } else {
        p.width = p.value == '\t'                           ? 4 - cells % 4
                  : p.value < 0x80 || p.value == UINT32_MAX ? 4u
                  : p.value <= 0xFFF                        ? 7u
                  : p.value <= 0xFFFF                       ? 8u
                  : p.value <= 0xFFFFF                      ? 9u
                                                            : 10u;
    }
    return p;
}

static void write_piece(struct writer *w, const uint8_t *s, struct display_piece p) {
    if (p.plain)
        bytes(w, s, p.used);
    else if (p.value == '\t')
        repeat(w, ' ', p.width);
    else if (p.value < 0x80 || p.value == UINT32_MAX) {
        text(w, "\\x");
        hex(w, s[0], 2);
    } else {
        text(w, "\\u{");
        hex(w, p.value, p.width - 4);
        text(w, "}");
    }
}

static void safe_bytes(struct writer *w, const uint8_t *s, size_t length) {
    struct display_cursor c = {.bytes = s, .length = length};
    uint32_t cells = 0;
    while (w->ok && c.at < c.length) {
        struct display_piece p = inspect(&c, cells, w->utf8);
        write_piece(w, c.bytes + c.at, p);
        cells += p.width;
        c.at += p.used;
    }
}

static void safe_text(struct writer *w, const char *s) {
    if (s != NULL)
        safe_bytes(w, (const uint8_t *)s, strlen(s));
}

static void location(struct writer *w, const struct mo_sources *sources, struct mo_span span) {
    if (!available(sources, span)) {
        text(w, "<unavailable>");
        return;
    }
    safe_text(w, sources->files[span.file].path);
    struct mo_position p = mo_sources_position(sources, span.file, span.start);
    text(w, ":");
    unsigned_number(w, p.line);
    text(w, ":");
    unsigned_number(w, p.column);
}

static void line_prefix(struct writer *w, uint32_t line, bool number) {
    text(w, "  ");
    if (number)
        unsigned_number(w, line);
    else {
        /* Caret and elision rows match the decimal line number's width. */
        do {
            text(w, " ");
            line /= 10;
        } while (line != 0);
    }
    text(w, " | ");
}

/* At most 160 cells and 4096 excerpt bytes, including elision markers. Retain
 * up to 12 complete clusters / 1024 bytes of context before a distant span.
 * Drawing marks every intersecting cluster; the original byte span is intact. */
static void source_line(struct writer *w, const uint8_t *s, uint32_t length, uint32_t line,
                        uint32_t start, uint32_t end, bool mark, const char *label) {
    struct display_cursor c = {.bytes = s, .length = length};
    size_t context[12];
    uint32_t count = 0, focus_cells = 0;
    while (c.at < start && c.at < length) {
        struct display_piece p = inspect(&c, focus_cells, w->utf8);
        if (p.used > start - c.at)
            break;
        if (count == 12) {
            memmove(context, context + 1, 11 * sizeof(context[0]));
            count--;
        }
        context[count++] = c.at;
        focus_cells += p.width;
        c.at += p.used;
    }
    size_t at = 0;
    if (focus_cells > 100 || c.at > 1024) {
        at = c.at;
        for (uint32_t i = 0; i < count; i++) {
            if (c.at - context[i] <= 1024) {
                at = context[i];
                break;
            }
        }
    }
    c.at = at;
    c.safe_end = at;
    line_prefix(w, line, true);
    uint32_t cells = 0, first = UINT32_MAX, last = 0;
    size_t row_bytes = 0;
    if (at != 0) {
        text(w, "...");
        cells = 3;
        row_bytes = 3;
    }
    while (w->ok && c.at < c.length && cells < 148) {
        struct display_piece p = inspect(&c, cells, w->utf8);
        size_t emitted = p.plain ? p.used : p.width;
        if (p.width > 148 - cells || emitted > 4093 - row_bytes)
            break;
        write_piece(w, s + c.at, p);
        if ((c.at < end && c.at + p.used > start) ||
            (start == end && c.at <= start && c.at + p.used > start)) {
            if (first == UINT32_MAX)
                first = cells;
            last = cells + p.width;
        }
        cells += p.width;
        row_bytes += emitted;
        c.at += p.used;
    }
    if (c.at < c.length)
        text(w, "...");
    if (first == UINT32_MAX) {
        first = cells;
        last = cells + 1;
    }
    if (last <= first)
        last = first + 1; /* A zero-width span still has a visible caret. */
    text(w, "\n");
    if (mark) {
        line_prefix(w, line, false);
        repeat(w, ' ', first);
        repeat(w, '^', last - first);
        if (label != NULL) {
            text(w, " ");
            safe_text(w, label);
        }
        text(w, "\n");
    }
}

static void text_label(struct writer *w, const struct mo_sources *sources,
                       const struct mo_label *label) {
    struct mo_span span = label->span;
    if (!available(sources, span)) {
        if (label->text != NULL) {
            /* The headline/secondary header already printed the location. */
            text(w, "  ");
            safe_text(w, label->text);
            text(w, "\n");
        }
        return;
    }
    struct mo_position first = mo_sources_position(sources, span.file, span.start);
    /* A nonempty span ending at the next line's start does not mark that line. */
    struct mo_position last =
        mo_sources_position(sources, span.file, span.end > span.start ? span.end - 1 : span.end);
    for (uint32_t line = first.line; w->ok && line <= last.line; line++) {
        if (line == first.line + 2 && last.line - first.line >= 4) {
            line_prefix(w, line, false);
            text(w, "...\n");
            line = last.line - 1;
        }
        struct mo_span row = mo_sources_line(sources, span.file, line);
        uint32_t start = span.start > row.start ? span.start - row.start : 0;
        uint32_t end = span.end < row.end ? span.end - row.start : row.end - row.start;
        if (start > row.end - row.start)
            start = row.end - row.start;
        source_line(w, sources->files[span.file].bytes + row.start, row.end - row.start, line,
                    start, end, true, line == last.line ? label->text : NULL);
    }
}

static void fixed_lines(struct writer *w, const uint8_t *s, uint32_t length, uint32_t line,
                        uint32_t focus) {
    if (s == NULL)
        s = (const uint8_t *)"";
    uint32_t at = 0;
    while (w->ok && at <= length) {
        uint32_t end = at;
        while (end < length && s[end] != '\n')
            end++;
        uint32_t trimmed = end;
        if (end < length && trimmed > at && s[trimmed - 1] == '\r')
            trimmed--;
        source_line(w, s + at, trimmed - at, line++, at == 0 ? focus : 0, 0, false, NULL);
        if (end == length)
            break;
        at = end + 1;
    }
}

static void text_fix(struct writer *w, const struct mo_sources *sources, const struct mo_fix *fix) {
    MO_ASSERT(fix->edit_count <= MO_LIMIT_FIX_EDITS);
    const struct mo_edit *edits[MO_LIMIT_FIX_EDITS];
    /* Stable insertion sort, bounded by 1024 edits. Insertions precede a
     * replacement at the same start, as simultaneous original-text edits do. */
    for (uint32_t i = 0; i < fix->edit_count; i++) {
        const struct mo_edit *e = &fix->edits[i];
        uint32_t j = i;
        while (j != 0 && (edits[j - 1]->span.file > e->span.file ||
                          (edits[j - 1]->span.file == e->span.file &&
                           (edits[j - 1]->span.start > e->span.start ||
                            (edits[j - 1]->span.start == e->span.start &&
                             edits[j - 1]->span.end > e->span.end))))) {
            edits[j] = edits[j - 1];
            j--;
        }
        edits[j] = e;
    }
    text(w, "\nFix:");
    if (fix->title != NULL) {
        text(w, " ");
        safe_text(w, fix->title);
    }
    text(w, "\n");
    for (uint32_t i = 0; w->ok && i < fix->edit_count;) {
        uint32_t stop = i + 1;
        while (stop < fix->edit_count && edits[stop]->span.file == edits[i]->span.file)
            stop++;
        if (!available(sources, edits[i]->span) || !available(sources, edits[stop - 1]->span)) {
            text(w, "  <unavailable>\n");
            i = stop;
            continue;
        }
        text(w, "  ");
        location(w, sources, edits[i]->span);
        text(w, "\n");
        struct mo_position p =
            mo_sources_position(sources, edits[i]->span.file, edits[i]->span.start);
        struct mo_position q =
            mo_sources_position(sources, edits[i]->span.file, edits[stop - 1]->span.end);
        uint32_t at = mo_sources_line(sources, edits[i]->span.file, p.line).start;
        uint32_t focus = edits[i]->span.start - at;
        uint32_t end = mo_sources_line(sources, edits[i]->span.file, q.line).end;
        /* Line text trims CRLF; an edit may end between its two bytes. */
        if (end < edits[stop - 1]->span.end)
            end = edits[stop - 1]->span.end;
        const uint8_t *original = sources->files[edits[i]->span.file].bytes;
        struct mo_buffer fixed;
        mo_buffer_init(&fixed, MO_CLI_DIAGNOSTIC_SIZE);
        bool ok = true;
        for (uint32_t j = i; ok && j < stop; j++) {
            struct mo_span span = edits[j]->span;
            if (!available(sources, span) || span.start < at) {
                ok = false;
                break;
            }
            ok = mo_buffer_append(&fixed, original + at, span.start - at) &&
                 mo_buffer_append_text(&fixed, edits[j]->replacement);
            at = span.end;
        }
        if (ok)
            ok = mo_buffer_append(&fixed, original + at, end - at);
        if (ok)
            fixed_lines(w, fixed.bytes, (uint32_t)fixed.length, p.line, focus);
        else
            w->ok = false;
        mo_buffer_free(&fixed);
        i = stop;
    }
}

bool mo_render_diagnostic(const struct mo_sources *sources, const struct mo_diag *d, bool json,
                          struct mo_buffer *out) {
    MO_ASSERT(sources != NULL && d != NULL && out != NULL);
    MO_ASSERT(out->limit <= MO_CLI_DIAGNOSTIC_SIZE);
    mo_buffer_clear(out);
    const char *codeset = nl_langinfo(CODESET);
    bool utf8 = strcmp(codeset, "UTF-8") == 0 || strcmp(codeset, "UTF8") == 0 ||
                strcmp(codeset, "utf-8") == 0 || strcmp(codeset, "utf8") == 0;
    struct writer w = {.out = out, .ok = true, .utf8 = utf8};
    if (json)
        json_diag(&w, sources, d);
    else {
        text(&w, "error ");
        code(&w, d->code);
        text(&w, ": ");
        safe_text(&w, d->message);
        text(&w, "  ");
        location(&w, sources, d->primary.span);
        text(&w, "\n\n");
        text_label(&w, sources, &d->primary);
        for (uint32_t i = 0; w.ok && i < d->secondary_count; i++) {
            text(&w, "\n  --> ");
            location(&w, sources, d->secondary[i].span);
            text(&w, "\n");
            text_label(&w, sources, &d->secondary[i]);
        }
        if (d->section != NULL) {
            text(&w, "\n");
            safe_text(&w, d->reason);
            text(&w, "\n");
        }
        for (uint32_t i = 0; w.ok && i < d->fix_count; i++)
            text_fix(&w, sources, &d->fixes[i]);
        text(&w, "\n");
    }
    if (!w.ok)
        mo_buffer_clear(out);
    return w.ok;
}

static bool write_buffer(FILE *stream, const struct mo_buffer *buffer) {
    return fwrite(buffer->bytes, 1, buffer->length, stream) == buffer->length &&
           fflush(stream) == 0;
}

bool mo_write_test(FILE *output, bool json, const struct mo_sources *sources,
                   const struct mo_observation *observation) {
    struct mo_buffer buffer;
    mo_buffer_init(&buffer, MO_CLI_DIAGNOSTIC_SIZE);
    struct writer w = {.out = &buffer, .ok = true};
    if (json) {
        text(&w, "{\"record\":\"test\",\"name\":");
        json_string(&w, observation->name);
        text(&w, ",\"kind\":");
        json_string(&w, mo_observation_kind(observation->source->kind));
        text(&w, ",\"span\":");
        json_span(&w, sources, observation->source->syntax->span, NULL, false);
        text(&w, "}\n");
    } else {
        safe_text(&w, observation->name);
        text(&w, "\n");
    }
    bool ok = w.ok && write_buffer(output, &buffer);
    mo_buffer_free(&buffer);
    return ok;
}

struct failure_part {
    const uint8_t *bytes;
    size_t length;
};

/* Lower's structural failure protocol has byte lengths, not delimiters inside
 * values. NUL, newlines and even another "right N:" remain ordinary value bytes. */
static bool failure_part(const uint8_t **at, size_t *left, const char *label,
                         struct failure_part *part) {
    size_t prefix = strlen(label);
    if (*left < prefix || memcmp(*at, label, prefix) != 0)
        return false;
    *at += prefix;
    *left -= prefix;
    size_t length = 0, digits = 0;
    while (*left != 0 && **at >= '0' && **at <= '9') {
        size_t digit = **at - (uint8_t)'0';
        if (length > (SIZE_MAX - digit) / 10 || (digits != 0 && length == 0))
            return false;
        length = length * 10 + digit;
        (*at)++;
        (*left)--;
        digits++;
    }
    if (digits == 0 || *left == 0 || **at != ':' || length >= *left)
        return false;
    (*at)++;
    (*left)--;
    *part = (struct failure_part){*at, length};
    *at += length;
    *left -= length;
    if (*left == 0 || **at != ',')
        return false;
    (*at)++;
    (*left)--;
    return true;
}

static void equality_failure(struct writer *w, const struct mo_defect *defect, bool json) {
    if (!defect->has_detail)
        return;
    size_t length;
    const uint8_t *at = mo_value_data(defect->detail, &length);
    const char prefix[] = "left ";
    if (length < sizeof(prefix) - 1 || memcmp(at, prefix, sizeof(prefix) - 1) != 0)
        return; /* Ordinary failed Bool assertion, not an equality. */
    struct failure_part left, right;
    if (!failure_part(&at, &length, "left ", &left) ||
        !failure_part(&at, &length, "right ", &right) || length != 0) {
        w->ok = false;
        return;
    }
    if (json) {
        text(w, ",\"left\":");
        json_bytes(w, left.bytes, left.length);
        text(w, ",\"right\":");
        json_bytes(w, right.bytes, right.length);
    } else {
        text(w, "  left: ");
        safe_bytes(w, left.bytes, left.length);
        text(w, "\n  right: ");
        safe_bytes(w, right.bytes, right.length);
        text(w, "\n");
    }
}

/* heap's root Text/Char spelling is unquoted. Defect variables use literals,
 * without asking a source to_text function to execute on a stopped process. */
static void structural_literal(struct writer *w, const struct mo_buffer *raw, uint8_t delimiter) {
    bytes(w, &delimiter, 1);
    for (size_t i = 0; w->ok && i < raw->length;) {
        uint32_t value;
        uint32_t n = scalar(raw->bytes + i, raw->length - i, &value);
        MO_ASSERT(value != UINT32_MAX);
        if (value == '\n' || value == '\r' || value == '\t') {
            text(w, value == '\n' ? "\\n" : value == '\r' ? "\\r" : "\\t");
        } else if (value == delimiter || value == '\\' ||
                   (value == '#' && i + 1 < raw->length && raw->bytes[i + 1] == '{')) {
            text(w, "\\");
            bytes(w, raw->bytes + i, n);
        } else if (escaped(value)) {
            text(w, "\\u{");
            hex(w, value, 1);
            text(w, "}");
        } else
            bytes(w, raw->bytes + i, n);
        i += n;
    }
    bytes(w, &delimiter, 1);
}

static void promise_value(struct writer *w, const struct mo_test_result *result, uint32_t type,
                          struct mo_cell cell, bool json) {
    struct mo_buffer raw, literal;
    mo_buffer_init(&raw, 4096);
    mo_buffer_init(&literal, 4096);
    bool printed = mo_value_print(mo_process_heap(result->process), type, cell, &raw);
    struct mo_buffer *value = &raw;
    enum mo_type_kind kind = result->file->types[type].kind;
    if (printed && (kind == MO_TYPE_TEXT || kind == MO_TYPE_CHAR)) {
        struct writer quoted = {.out = &literal, .ok = true};
        structural_literal(&quoted, &raw, kind == MO_TYPE_TEXT ? '"' : '\'');
        printed = quoted.ok;
        value = &literal;
    }
    w->ok = w->ok && printed;
    if (json)
        json_bytes(w, value->bytes, value->length);
    else
        safe_bytes(w, value->bytes, value->length);
    mo_buffer_free(&literal);
    mo_buffer_free(&raw);
}

static const char *extreme_description(const struct mo_input_extreme *extreme) {
    bool smallest = extreme->end == MO_INPUT_SMALLEST;
    if (extreme->type == MO_TYPE_INT)
        return smallest ? "Int's smallest value" : "Int's largest value";
    if (extreme->type == MO_TYPE_DECIMAL)
        return smallest ? "Decimal's smallest value" : "Decimal's largest value";
    if (extreme->type == MO_TYPE_TIME)
        return smallest ? "Time's smallest value" : "Time's largest value";
    if (extreme->type == MO_TYPE_DURATION)
        return smallest ? "Duration's smallest value" : "Duration's largest value";
    MO_UNREACHABLE();
}

static void promise_result(struct writer *w, const struct mo_test_result *result, bool json) {
    const struct mo_promise_result *promise = result->promise;
    if (result->outcome == MO_RETURNED)
        return;
    const struct mo_checked_spec *spec = result->observation->source;
    const struct mo_bytecode_function *subject = &result->file->functions[promise->subject];
    if (json)
        text(w, ",\"input\":[");
    for (uint32_t i = 0; w->ok && i < subject->parameter_count; i++) {
        const char *name = spec->parameters[i]->name;
        if (json) {
            if (i != 0)
                text(w, ",");
            text(w, "{\"name\":");
            json_string(w, name);
            text(w, ",\"value\":");
        } else {
            text(w, "  input ");
            safe_text(w, name);
            text(w, " = ");
        }
        promise_value(w, result, subject->parameters[i].type, promise->input[i], json);
        text(w, json ? "}" : "\n");
    }
    if (json)
        text(w, "]");
    if (result->outcome == MO_FAILED) {
        if (promise->has_value) {
            text(w, json ? ",\"result\":" : "  result: ");
            promise_value(w, result, subject->result_type, promise->value, json);
            if (!json)
                text(w, "\n");
        }
        text(w, json ? ",\"calls\":" : "  calls: ");
        unsigned_number(w, promise->calls);
        if (!json)
            text(w, "\n");
    }
    if (promise->extreme != NULL) {
        const char *name = spec->parameters[promise->extreme->parameter]->name;
        const char *description = extreme_description(promise->extreme);
        if (json) {
            text(w, ",\"extreme\":{\"name\":");
            json_string(w, name);
            text(w, ",\"description\":");
            json_string(w, description);
            text(w, "}");
        } else {
            text(w, "  ");
            safe_text(w, name);
            text(w, " is ");
            safe_text(w, description);
            text(w, "\n  fix: return a failure for such inputs, or take a type whose module "
                    "keeps its values in range (reference 12.2).\n");
        }
    }
}

struct stopped_variables {
    struct writer *writer;
    const struct mo_test_result *result;
    bool json, first;
};

static bool stopped_variable(const struct mo_defect_variable *variable, void *context) {
    struct stopped_variables *variables = context;
    const struct mo_test_result *result = variables->result;
    if (variable->frame != result->defect->frames)
        return true; /* The wire schema's variables belong to the stopping frame. */
    struct mo_buffer raw, literal;
    mo_buffer_init(&raw, 4096);
    mo_buffer_init(&literal, 4096);
    bool printed =
        mo_value_print(mo_process_heap(result->process), variable->type, variable->value, &raw);
    struct mo_buffer *value = &raw;
    enum mo_type_kind kind = result->file->types[variable->type].kind;
    if (printed && (kind == MO_TYPE_TEXT || kind == MO_TYPE_CHAR)) {
        struct writer w = {.out = &literal, .ok = true};
        structural_literal(&w, &raw, kind == MO_TYPE_TEXT ? '"' : '\'');
        printed = w.ok;
        value = &literal;
    }
    if (!printed) {
        mo_buffer_clear(&raw);
        printed = mo_buffer_append_text(&raw, "<value unavailable>");
        value = &raw;
    }
    struct writer *w = variables->writer;
    w->ok = w->ok && printed;
    if (variables->json) {
        if (!variables->first)
            text(w, ",");
        text(w, "{\"name\":");
        json_bytes(w, variable->name, variable->name_length);
        text(w, ",\"value\":");
        json_bytes(w, value->bytes, value->length);
        text(w, "}");
    } else {
        text(w, "  ");
        safe_bytes(w, variable->name, variable->name_length);
        text(w, " = ");
        safe_bytes(w, value->bytes, value->length);
        text(w, "\n");
    }
    variables->first = false;
    mo_buffer_free(&literal);
    mo_buffer_free(&raw);
    return w->ok;
}

static void stopped_variables(struct writer *w, const struct mo_test_result *result, bool json) {
    if (!w->ok || result->defect->frames == NULL)
        return; /* Process-allocation refusal really has no variables. */
    struct stopped_variables variables = {w, result, json, true};
    enum mo_defect_variables_result outcome =
        mo_visit_defect_variables(result->process, result->defect, MO_CLI_TEST_VARIABLES,
                                  MO_CLI_TEST_WORK, stopped_variable, &variables);
    /* Never manufacture an empty success when replay could not establish the
     * readable values, nor publish a partial list after a resource refusal. */
    w->ok = w->ok && outcome == MO_DEFECT_VARIABLES_COMPLETE;
}

static void stopped_result(struct writer *w, const struct mo_sources *sources,
                           const struct mo_test_result *result, bool json) {
    const struct mo_defect *defect = result->defect;
    struct mo_span span = mo_runtime_span(result->file, sources, defect->span.file,
                                          defect->span.start, defect->span.end);
    if (json) {
        text(w, ",\"defect\":{\"reason\":");
        json_string(w, defect->reason);
        if (defect->limit_name != NULL) {
            text(w, ",\"limit\":{\"name\":");
            json_string(w, defect->limit_name);
            text(w, ",\"value\":\"");
            unsigned_number(w, defect->limit);
            text(w, "\"}");
        }
        if (defect->has_detail) {
            size_t length;
            const uint8_t *detail = mo_value_data(defect->detail, &length);
            text(w, ",\"detail\":");
            json_bytes(w, detail, length);
        }
        text(w, ",\"span\":");
        json_span(w, sources, span, NULL, false);
        text(w, ",\"stack\":[");
    } else {
        text(w, "  bug: ");
        safe_text(w, defect->reason);
        if (defect->limit_name != NULL) {
            text(w, " (");
            safe_text(w, defect->limit_name);
            text(w, " = ");
            unsigned_number(w, defect->limit);
            text(w, ")");
        }
        text(w, "\n");
        if (defect->has_detail) {
            size_t length;
            const uint8_t *detail = mo_value_data(defect->detail, &length);
            text(w, "  detail: ");
            safe_bytes(w, detail, length);
            text(w, "\n");
        }
    }
    bool first = true;
    for (const struct mo_defect_frame *frame = defect->frames; w->ok && frame != NULL;
         frame = frame->previous) {
        const struct mo_bytecode_string *function =
            &result->file->strings[result->file->functions[frame->function].name];
        span = mo_runtime_span(result->file, sources, frame->span.file, frame->span.start,
                               frame->span.end);
        if (json) {
            if (!first)
                text(w, ",");
            text(w, "{\"function\":");
            json_bytes(w, function->bytes, function->length);
            text(w, ",\"span\":");
            json_span(w, sources, span, NULL, false);
            text(w, "}");
        } else {
            text(w, "  at ");
            safe_bytes(w, function->bytes, function->length);
            text(w, "  ");
            location(w, sources, span);
            text(w, "\n");
        }
        first = false;
    }
    if (json)
        text(w, "],\"variables\":[");
    stopped_variables(w, result, json);
    if (json)
        text(w, "]}");
}

bool mo_render_test_result(const struct mo_sources *sources, const struct mo_test_result *result,
                           bool json, struct mo_buffer *out) {
    MO_ASSERT(out->limit <= MO_CLI_DIAGNOSTIC_SIZE);
    mo_buffer_clear(out);
    const char *codeset = nl_langinfo(CODESET);
    struct writer w = {.out = out,
                       .ok = true,
                       .utf8 = strcmp(codeset, "UTF-8") == 0 || strcmp(codeset, "UTF8") == 0 ||
                               strcmp(codeset, "utf-8") == 0 || strcmp(codeset, "utf8") == 0};
    const struct mo_observation *observation = result->observation;
    const char *outcome = result->outcome == MO_RETURNED ? "passed"
                          : result->outcome == MO_FAILED ? "failed"
                                                         : "stopped";
    if (json) {
        text(&w, "{\"record\":\"result\",\"name\":");
        json_string(&w, observation->name);
        text(&w, ",\"kind\":");
        json_string(&w, mo_observation_kind(observation->source->kind));
        text(&w, ",\"span\":");
        json_span(&w, sources, observation->source->syntax->span, NULL, false);
        text(&w, ",\"outcome\":");
        json_string(&w, outcome);
        text(&w, ",\"microseconds\":");
        unsigned_number(&w, result->microseconds);
    } else {
        text(&w, outcome);
        text(&w, " ");
        safe_text(&w, observation->name);
        text(&w, "  ");
        location(&w, sources, observation->source->syntax->span);
        text(&w, "\n");
    }
    if (result->promise != NULL)
        promise_result(&w, result, json);
    if (result->outcome == MO_FAILED) {
        equality_failure(&w, result->defect, json);
        if (observation->source->kind == MO_SPEC_TEST ||
            observation->source->kind == MO_SPEC_PROMISE) {
            if (result->line.file == UINT32_MAX)
                w.ok = false; /* Never invent the failed assert's identity. */
            if (json) {
                text(&w, ",\"line\":");
                json_span(&w, sources, result->line, NULL, false);
            } else {
                struct mo_label label = {result->line, observation->source->kind == MO_SPEC_TEST
                                                           ? "this assertion was false"
                                                           : "this promise condition was false"};
                text_label(&w, sources, &label);
            }
        } else if (!json) {
            struct mo_label label = {observation->source->syntax->span, "this example was false"};
            text_label(&w, sources, &label);
        }
    } else if (result->outcome == MO_STOPPED) {
        stopped_result(&w, sources, result, json);
    }
    if (json)
        text(&w, "}\n");
    if (!w.ok)
        mo_buffer_clear(out);
    return w.ok;
}

bool mo_write_test_result(FILE *output, bool json, const struct mo_sources *sources,
                          const struct mo_test_result *result) {
    struct mo_buffer buffer;
    mo_buffer_init(&buffer, MO_CLI_DIAGNOSTIC_SIZE);
    bool ok =
        mo_render_test_result(sources, result, json, &buffer) && write_buffer(output, &buffer);
    mo_buffer_free(&buffer);
    return ok;
}

bool mo_write_defect(FILE *errors, const struct mo_process *process,
                     const struct mo_defect *defect) {
    struct mo_buffer raw, shown;
    mo_buffer_init(&raw, MO_LIMIT_DEFECT_TEXT_SIZE);
    mo_buffer_init(&shown, MO_CLI_DIAGNOSTIC_SIZE);
    bool complete = mo_render_defect(process, defect, &raw);
    const char *codeset = nl_langinfo(CODESET);
    struct writer w = {.out = &shown,
                       .ok = true,
                       .utf8 = strcmp(codeset, "UTF-8") == 0 || strcmp(codeset, "UTF8") == 0 ||
                               strcmp(codeset, "utf-8") == 0 || strcmp(codeset, "utf8") == 0};
    text(&w, "bug: ");
    if (raw.length == 0) {
        safe_text(&w, defect->reason);
        text(&w, "\n");
    } else {
        struct display_cursor c = {.bytes = raw.bytes, .length = raw.length};
        uint32_t cells = 0;
        while (w.ok && c.at < c.length) {
            if (c.bytes[c.at] == '\n') {
                text(&w, "\n");
                c.at++;
                cells = 0;
            } else {
                struct display_piece p = inspect(&c, cells, w.utf8);
                write_piece(&w, c.bytes + c.at, p);
                cells += p.width;
                c.at += p.used;
            }
        }
    }
    if (!complete)
        text(&w, "  <report incomplete>\n");
    bool ok = w.ok && write_buffer(errors, &shown);
    mo_buffer_free(&shown);
    mo_buffer_free(&raw);
    return ok;
}

bool mo_write_version(FILE *output, enum mo_command command) {
    MO_ASSERT(command == MO_COMMAND_CHECK || command == MO_COMMAND_TEST);
    struct mo_buffer buffer;
    mo_buffer_init(&buffer, MO_CLI_DIAGNOSTIC_SIZE);
    struct writer w = {.out = &buffer, .ok = true};
    text(&w, "{\"record\":\"version\",\"format\":\"1.0\",\"mo\":");
    json_string(&w, mo_version());
    text(&w, ",\"command\":");
    json_string(&w, command == MO_COMMAND_CHECK ? "check" : "test");
    text(&w, "}\n");
    bool ok = w.ok && write_buffer(output, &buffer);
    mo_buffer_free(&buffer);
    return ok;
}

int mo_write_diagnostics(FILE *output, FILE *errors, bool json, enum mo_command command,
                         const struct mo_sources *sources, struct mo_diags *diags,
                         const struct mo_test_totals *totals) {
    mo_diags_sort(diags);
    uint32_t written = 0;
    uint64_t dropped = diags->dropped;
    struct mo_buffer buffer;
    mo_buffer_init(&buffer, MO_CLI_DIAGNOSTIC_SIZE);
    bool ok = true;
    for (uint32_t i = 0; ok && i < diags->count; i++) {
        if (!mo_render_diagnostic(sources, &diags->items[i], json, &buffer)) {
            dropped++;
            continue;
        }
        ok = write_buffer(json ? output : errors, &buffer);
        if (ok)
            written++;
    }
    struct mo_test_totals counts = totals == NULL ? (struct mo_test_totals){0} : *totals;
    int status =
        mo_diags_any(diags) || dropped != 0 || !ok || counts.failed != 0 || counts.stopped != 0 ? 1
                                                                                                : 0;
    mo_buffer_clear(&buffer);
    struct writer w = {.out = &buffer, .ok = true};
    if (json) {
        text(&w, "{\"record\":\"totals\",\"errors\":");
        unsigned_number(&w, written);
        text(&w, ",\"dropped\":");
        unsigned_number(&w, dropped);
        if (command == MO_COMMAND_TEST) {
            text(&w, ",\"passed\":");
            unsigned_number(&w, counts.passed);
            text(&w, ",\"failed\":");
            unsigned_number(&w, counts.failed);
            text(&w, ",\"stopped\":");
            unsigned_number(&w, counts.stopped);
        }
        text(&w, ",\"status\":");
        unsigned_number(&w, (uint32_t)status);
        text(&w, "}\n");
    } else if (dropped != 0) {
        unsigned_number(&w, dropped);
        text(&w, " diagnostics omitted: diagnostic storage or output limit\n");
    }
    if (ok && buffer.length != 0)
        ok = w.ok && write_buffer(json ? output : errors, &buffer);
    if (ok && !json && (counts.passed != 0 || counts.failed != 0 || counts.stopped != 0)) {
        mo_buffer_clear(&buffer);
        unsigned_number(&w, counts.passed);
        text(&w, " passed, ");
        unsigned_number(&w, counts.failed);
        text(&w, " failed, ");
        unsigned_number(&w, counts.stopped);
        text(&w, " stopped\n");
        ok = w.ok && write_buffer(output, &buffer);
    }
    mo_buffer_free(&buffer);
    if (!ok) {
        (void)fputs("mo: could not write diagnostics\n", errors);
        return 1;
    }
    return status;
}
