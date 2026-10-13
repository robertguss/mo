#include "syntax/format-doc.h"

#include "base/limits.h"

#include <string.h>

void mo_doc_init(struct mo_document *doc, mo_format_width_fn display_width) {
    *doc = (struct mo_document){.ok = true, .display_width = display_width};
    mo_arena_init(&doc->arena, MO_FORMAT_DOCUMENT_SIZE);
}

void mo_doc_free(struct mo_document *doc) {
    mo_arena_free(&doc->arena);
}

static uint32_t add_width(uint32_t a, uint32_t b) {
    return a > UINT32_MAX - b ? UINT32_MAX : a + b;
}

void mo_doc_add(struct mo_document *doc, enum mo_doc_kind kind, const uint8_t *bytes, size_t length,
                bool source_break) {
    if (!doc->ok)
        return;
    struct mo_doc *d = mo_arena_allocate(&doc->arena, sizeof *d, _Alignof(struct mo_doc));
    if (d == NULL) {
        doc->ok = false;
        return;
    }
    *d = (struct mo_doc){
        .kind = kind, .bytes = bytes, .length = length, .source_break = source_break};
    if (doc->last != NULL)
        doc->last->next = d;
    else
        doc->first = d;
    doc->last = d;
    switch (kind) {
    case MO_DOC_TEXT:
        d->flat_width =
            memchr(bytes, '\n', length) != NULL ? UINT32_MAX : doc->display_width(bytes, length);
        break;
    case MO_DOC_BREAK:
    case MO_DOC_SOURCE_BREAK:
    case MO_DOC_SPACE:
        d->flat_width = (uint32_t)length;
        break;
    case MO_DOC_COMMENT:
        d->flat_width = add_width(2, doc->display_width(bytes, length));
        break;
    case MO_DOC_SUFFIX_LINE:
        d->ends_line = true;
        break;
    case MO_DOC_LINE:
    case MO_DOC_LINE_IF:
    case MO_DOC_BLANK:
        d->flat_width = UINT32_MAX;
        break;
    case MO_DOC_GROUP:
        if (++doc->depth > MO_FORMAT_DOCUMENT_DEPTH)
            doc->ok = false;
        d->parent = doc->group;
        doc->group = d;
        return;
    case MO_DOC_END_GROUP:
        MO_ASSERT(doc->group != NULL && doc->depth != 0);
        d->parent = doc->group;
        d->flat_width = doc->group->flat_width;
        d->ends_line = doc->group->ends_line;
        doc->group = doc->group->parent;
        doc->depth--;
        break;
    case MO_DOC_NEST:
    case MO_DOC_UNNEST:
        break;
    case MO_DOC_ALIGN_LINE:
        if (++doc->align_depth > MO_FORMAT_DOCUMENT_DEPTH)
            doc->ok = false;
        d->parent = doc->align;
        doc->align = d;
        break;
    case MO_DOC_END_ALIGN:
        MO_ASSERT(doc->align != NULL && doc->align_depth != 0);
        d->parent = doc->align;
        doc->align = doc->align->parent;
        doc->align_depth--;
        break;
    }
    if (doc->group != NULL) {
        /* A line-ending comment at the end of a group doesn't break its
         * preceding code. More code after it makes a flat group impossible. */
        if (doc->group->ends_line && d->flat_width != 0)
            doc->group->flat_width = UINT32_MAX;
        doc->group->flat_width = add_width(doc->group->flat_width, d->flat_width);
        doc->group->ends_line = doc->group->ends_line || d->ends_line;
    }
}

struct writer {
    struct mo_buffer *out;
    struct mo_doc *group, *pending, *pending_last;
    mo_format_width_fn display_width;
    uint32_t indent, column, characters, line_indent;
};

static bool bytes(struct writer *w, const uint8_t *data, size_t size) {
    if (size == 0)
        return true;
    if (w->column == 0 && data[0] != '\n') {
        if (w->indent > MO_LIMIT_LINE_LENGTH)
            return false;
        for (uint32_t i = 0; i < w->indent; i++)
            if (!mo_buffer_append_byte(w->out, ' '))
                return false;
        w->column = w->indent;
        w->characters = w->indent;
        w->line_indent = w->indent;
    }
    if (!mo_buffer_append(w->out, data, size))
        return false;
    size_t start = 0;
    for (size_t i = 0; i < size; i++) {
        if (data[i] == '\n') {
            start = i + 1;
            w->characters = 0;
        } else if (data[i] != '\r' && (data[i] & 0xc0) != 0x80 &&
                   ++w->characters > MO_LIMIT_LINE_LENGTH)
            /* Valid input's indivisible token plus new indentation must not
             * produce source the lexer will refuse. Count scalars, not cells. */
            return false;
    }
    if (start != 0)
        w->column = 0;
    w->column = add_width(w->column, w->display_width(data + start, size - start));
    return true;
}

static bool newline(struct writer *w, bool always) {
    while (w->pending != NULL) {
        struct mo_doc *d = w->pending;
        if (w->column != 0 && !bytes(w, (const uint8_t *)"  ", 2))
            return false;
        if (!bytes(w, d->bytes, d->length))
            return false;
        w->pending = d->pending;
        if (w->pending != NULL) {
            if (!mo_buffer_append_byte(w->out, '\n'))
                return false;
            w->column = 0;
            w->characters = 0;
        }
    }
    w->pending_last = NULL;
    if (always || w->column != 0) {
        if (!mo_buffer_append_byte(w->out, '\n'))
            return false;
        w->column = 0;
        w->characters = 0;
    }
    return true;
}

bool mo_doc_render(struct mo_document *doc, struct mo_format_options options,
                   struct mo_buffer *output) {
    if (!doc->ok)
        return false;
    MO_ASSERT(doc->group == NULL && doc->depth == 0 && doc->align == NULL && doc->align_depth == 0);
    struct writer w = {.out = output, .display_width = doc->display_width};
    for (struct mo_doc *d = doc->first; d != NULL; d = d->next) {
        bool ok = true;
        switch (d->kind) {
        case MO_DOC_TEXT:
            ok = bytes(&w, d->bytes, d->length);
            break;
        case MO_DOC_COMMENT:
            if (w.pending_last != NULL)
                w.pending_last->pending = d;
            else
                w.pending = d;
            w.pending_last = d;
            break;
        case MO_DOC_BREAK:
            if (w.pending != NULL ||
                (options.policy == MO_FORMAT_SOURCE_PROTOTYPE ? d->source_break
                                                              : w.group != NULL && !w.group->flat))
                ok = newline(&w, false);
            else if (d->length != 0 && w.column != 0)
                ok = bytes(&w, (const uint8_t *)" ", 1);
            break;
        case MO_DOC_SOURCE_BREAK:
            /* Before an operator, a queued comment must wait until the
             * operator has been written: a leading '-' changes the tree. */
            if (options.policy == MO_FORMAT_SOURCE_PROTOTYPE && d->source_break)
                ok = newline(&w, false);
            else if (d->length != 0 && w.column != 0)
                ok = bytes(&w, (const uint8_t *)" ", 1);
            break;
        case MO_DOC_SPACE:
            if (w.column != 0)
                ok = bytes(&w, (const uint8_t *)" ", 1);
            break;
        case MO_DOC_LINE:
            ok = newline(&w, true);
            break;
        case MO_DOC_LINE_IF:
        case MO_DOC_SUFFIX_LINE:
            ok = newline(&w, false);
            break;
        case MO_DOC_BLANK:
            ok = newline(&w, false) && newline(&w, true);
            break;
        case MO_DOC_GROUP: {
            /* Indentation is emitted lazily, but already consumes width. */
            uint32_t column = w.column != 0 ? w.column : w.indent;
            d->flat = options.policy == MO_FORMAT_SOURCE_PROTOTYPE ||
                      (w.group != NULL && w.group->flat) ||
                      (column <= options.width && d->flat_width <= options.width - column);
            w.group = d;
            break;
        }
        case MO_DOC_END_GROUP:
            w.group = d->parent->parent;
            break;
        case MO_DOC_NEST:
            w.indent += 2;
            break;
        case MO_DOC_UNNEST:
            MO_ASSERT(w.indent >= 2);
            w.indent -= 2;
            break;
        case MO_DOC_ALIGN_LINE:
            /* Hanging operator/dot nesting counts only after it breaks.
             * A bracket or block starts at its header's physical indentation. */
            d->saved_indent = w.indent;
            if (w.column != 0)
                w.indent = w.line_indent;
            break;
        case MO_DOC_END_ALIGN:
            w.indent = d->parent->saved_indent;
            break;
        }
        if (!ok)
            return false;
    }
    return newline(&w, false);
}
