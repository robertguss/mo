#include "tools/tools.h"

#include <string.h>

struct writer {
    struct mo_buffer *out;
    bool ok;
};

static void bytes(struct writer *w, const void *data, size_t length) {
    if (w->ok)
        w->ok = mo_buffer_append(w->out, data, length);
}
static void text(struct writer *w, const char *s) {
    if (w->ok)
        w->ok = mo_buffer_append_text(w->out, s);
}
static void uint(struct writer *w, uint64_t n) {
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

static void json_string(struct writer *w, const char *s) {
    if (s == NULL) {
        text(w, "null");
        return;
    }
    text(w, "\"");
    size_t length = strlen(s);
    for (size_t i = 0; w->ok && i < length;) {
        uint32_t value;
        uint32_t n = scalar((const uint8_t *)s + i, length - i, &value);
        if (s[i] == '"' || s[i] == '\\') {
            text(w, "\\");
            bytes(w, s + i, 1);
        } else if (value < 0x20 || value == UINT32_MAX) {
            text(w, "\\u");
            hex(w, (uint8_t)s[i], 4);
        } else {
            bytes(w, s + i, n);
        }
        i += n;
    }
    text(w, "\"");
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
    uint(w, span.start);
    text(w, ",\"end\":");
    uint(w, span.end);
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
            uint(w, values[i]);
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
    uint(w, n);
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

/* Render non-ASCII and controls as escapes. In particular, wide and combining
 * characters cannot displace a caret on terminals with different width tables.
 * Locations still count the original scalar as ONE character, never its escape. */
static uint32_t visible(struct writer *w, const uint8_t *s, size_t length, uint32_t *used,
                        uint32_t cells) {
    uint32_t value;
    *used = scalar(s, length, &value);
    if (value == '\t') {
        uint32_t width = 4 - cells % 4;
        repeat(w, ' ', width);
        return width;
    }
    if (value >= 0x20 && value <= 0x7E) {
        bytes(w, s, 1);
        return 1;
    }
    if (value < 0x80 || value == UINT32_MAX) {
        text(w, "\\x");
        hex(w, s[0], 2);
        return 4;
    }
    text(w, "\\u{");
    unsigned digits = value <= 0xFFF ? 3u : value <= 0xFFFF ? 4u : value <= 0xFFFFF ? 5u : 6u;
    hex(w, value, digits);
    text(w, "}");
    return digits + 4;
}

static void safe_text(struct writer *w, const char *s) {
    if (s == NULL)
        return;
    size_t length = strlen(s);
    uint32_t cells = 0;
    for (size_t i = 0; w->ok && i < length;) {
        uint32_t used;
        cells += visible(w, (const uint8_t *)s + i, length - i, &used, cells);
        i += used;
    }
}

static void location(struct writer *w, const struct mo_sources *sources, struct mo_span span) {
    if (!available(sources, span)) {
        text(w, "<unavailable>");
        return;
    }
    safe_text(w, sources->files[span.file].path);
    struct mo_position p = mo_sources_position(sources, span.file, span.start);
    text(w, ":");
    uint(w, p.line);
    text(w, ":");
    uint(w, p.column);
}

static void line_prefix(struct writer *w, uint32_t line) {
    text(w, "  ");
    uint(w, line);
    text(w, " | ");
}

/* At most 160 displayed cells, with 12 source characters of leading context.
 * Marks intersecting scalars even when a byte span starts inside one. */
static void source_line(struct writer *w, const uint8_t *s, uint32_t length, uint32_t line,
                        uint32_t start, uint32_t end, bool mark, const char *label) {
    uint32_t at = 0, column = 0, focus = 0, focus_cells = 0;
    while (at < start && at < length) {
        uint32_t v;
        uint32_t n = scalar(s + at, length - at, &v);
        if (n > start - at)
            break;
        focus_cells += v == '\t'                     ? 4 - focus_cells % 4
                       : v >= 0x20 && v <= 0x7E      ? 1u
                       : v < 0x80 || v == UINT32_MAX ? 4u
                       : v <= 0xFFF                  ? 7u
                       : v <= 0xFFFF                 ? 8u
                       : v <= 0xFFFFF                ? 9u
                                                     : 10u;
        at += n;
        focus++;
    }
    at = 0;
    while (focus_cells > 100 && column + 12 < focus && at < length) {
        uint32_t v;
        at += scalar(s + at, length - at, &v);
        column++;
    }
    line_prefix(w, line);
    uint32_t cells = 0, first = UINT32_MAX, last = 0;
    if (at != 0) {
        text(w, "...");
        cells = 3;
    }
    while (w->ok && at < length && cells < 148) {
        uint32_t used;
        uint32_t width = visible(w, s + at, length - at, &used, cells);
        if ((at < end && at + used > start) || (start == end && at <= start && at + used > start)) {
            if (first == UINT32_MAX)
                first = cells;
            last = cells + width;
        }
        cells += width;
        at += used;
    }
    if (at < length)
        text(w, "...");
    if (first == UINT32_MAX) {
        first = cells;
        last = cells + 1;
    }
    text(w, "\n");
    if (mark) {
        /* Match the decimal line number's width, independently of its size. */
        text(w, "  ");
        uint32_t n = line;
        do {
            text(w, " ");
            n /= 10;
        } while (n != 0);
        text(w, " | ");
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
        text(w, "  <unavailable>");
        if (label->text != NULL) {
            text(w, " ");
            safe_text(w, label->text);
        }
        text(w, "\n");
        return;
    }
    struct mo_position first = mo_sources_position(sources, span.file, span.start);
    /* A nonempty span ending at the next line's start does not mark that line. */
    struct mo_position last =
        mo_sources_position(sources, span.file, span.end > span.start ? span.end - 1 : span.end);
    for (uint32_t line = first.line; w->ok && line <= last.line; line++) {
        if (line == first.line + 2 && last.line - first.line >= 4) {
            text(w, "    | ...\n");
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
    struct writer w = {out, true};
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

bool mo_write_version(FILE *output, enum mo_command command) {
    MO_ASSERT(command == MO_COMMAND_CHECK || command == MO_COMMAND_TEST);
    struct mo_buffer buffer;
    mo_buffer_init(&buffer, MO_CLI_DIAGNOSTIC_SIZE);
    struct writer w = {&buffer, true};
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
                         const struct mo_sources *sources, struct mo_diags *diags) {
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
    int status = mo_diags_any(diags) || dropped != 0 || !ok ? 1 : 0;
    mo_buffer_clear(&buffer);
    struct writer w = {&buffer, true};
    if (json) {
        text(&w, "{\"record\":\"totals\",\"errors\":");
        uint(&w, written);
        text(&w, ",\"dropped\":");
        uint(&w, dropped);
        if (command == MO_COMMAND_TEST)
            text(&w, ",\"passed\":0,\"failed\":0,\"stopped\":0");
        text(&w, ",\"status\":");
        uint(&w, (uint32_t)status);
        text(&w, "}\n");
    } else if (dropped != 0) {
        uint(&w, dropped);
        text(&w, " diagnostics omitted: diagnostic storage or output limit\n");
    }
    if (ok && buffer.length != 0)
        ok = w.ok && write_buffer(json ? output : errors, &buffer);
    mo_buffer_free(&buffer);
    if (!ok) {
        (void)fputs("mo: could not write diagnostics\n", errors);
        return 1;
    }
    return status;
}
