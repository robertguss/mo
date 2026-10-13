#include "syntax/format-doc.h"

#include "base/limits.h"

#include <string.h>

enum { DOC_MEMORY = 128 << 20, DOC_DEPTH = 32 * MO_LIMIT_NESTING_DEPTH + 32 };

void mo_doc_init(struct mo_document *doc, mo_format_width_fn display_width) {
    *doc = (struct mo_document){.ok = true, .display_width = display_width};
    mo_arena_init(&doc->arena, DOC_MEMORY);
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
        d->flat_width = (uint32_t)length;
        break;
    case MO_DOC_COMMENT:
    case MO_DOC_LINE:
    case MO_DOC_LINE_IF:
    case MO_DOC_BLANK:
        d->flat_width = UINT32_MAX;
        break;
    case MO_DOC_GROUP:
        if (++doc->depth > DOC_DEPTH)
            doc->ok = false;
        d->parent = doc->group;
        doc->group = d;
        return;
    case MO_DOC_END_GROUP:
        MO_ASSERT(doc->group != NULL && doc->depth != 0);
        d->parent = doc->group;
        d->flat_width = doc->group->flat_width;
        doc->group = doc->group->parent;
        doc->depth--;
        break;
    case MO_DOC_NEST:
    case MO_DOC_UNNEST:
        break;
    }
    if (doc->group != NULL)
        doc->group->flat_width = add_width(doc->group->flat_width, d->flat_width);
}

void mo_doc_text(struct mo_document *doc, const char *text) {
    mo_doc_add(doc, MO_DOC_TEXT, (const uint8_t *)text, strlen(text), false);
}

struct writer {
    struct mo_buffer *out;
    struct mo_doc *group, *pending, *pending_last;
    mo_format_width_fn display_width;
    uint32_t indent, column;
};

static bool bytes(struct writer *w, const uint8_t *data, size_t size) {
    if (size == 0)
        return true;
    if (w->column == 0 && data[0] != '\n') {
        for (uint32_t i = 0; i < w->indent; i++)
            if (!mo_buffer_append_byte(w->out, ' '))
                return false;
        w->column = w->indent;
    }
    if (!mo_buffer_append(w->out, data, size))
        return false;
    size_t start = 0;
    for (size_t i = 0; i < size; i++) {
        if (data[i] == '\n')
            start = i + 1;
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
        }
    }
    w->pending_last = NULL;
    if (always || w->column != 0) {
        if (!mo_buffer_append_byte(w->out, '\n'))
            return false;
        w->column = 0;
    }
    return true;
}

bool mo_doc_render(struct mo_document *doc, struct mo_format_options options,
                   struct mo_buffer *output) {
    if (!doc->ok)
        return false;
    MO_ASSERT(doc->group == NULL && doc->depth == 0);
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
        case MO_DOC_LINE:
            ok = newline(&w, true);
            break;
        case MO_DOC_LINE_IF:
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
        }
        if (!ok)
            return false;
    }
    return newline(&w, false);
}
