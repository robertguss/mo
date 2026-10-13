/* Private Wadler-style documents: concatenation is an arena-owned instruction
 * list; groups cache their flat widths. Rendering never rescans a suffix. */
#ifndef MO_FORMAT_DOC_H
#define MO_FORMAT_DOC_H

#include "syntax/format.h"

enum mo_doc_kind {
    MO_DOC_TEXT,
    MO_DOC_COMMENT,
    MO_DOC_BREAK,
    MO_DOC_SOURCE_BREAK,
    MO_DOC_SPACE,
    MO_DOC_LINE,
    MO_DOC_LINE_IF,
    MO_DOC_SUFFIX_LINE,
    MO_DOC_BLANK,
    MO_DOC_GROUP,
    MO_DOC_END_GROUP,
    MO_DOC_NEST,
    MO_DOC_UNNEST,
    MO_DOC_ALIGN_LINE,
    MO_DOC_END_ALIGN
};
struct mo_doc {
    enum mo_doc_kind kind;
    const uint8_t *bytes;
    size_t length;
    uint32_t flat_width, saved_indent;
    bool source_break, flat, ends_line;
    struct mo_doc *next, *parent, *pending;
};
struct mo_document {
    struct mo_arena arena;
    struct mo_doc *first, *last, *group, *align;
    mo_format_width_fn display_width;
    uint32_t depth, align_depth;
    bool ok;
};

void mo_doc_init(struct mo_document *doc, mo_format_width_fn display_width);
void mo_doc_free(struct mo_document *doc);
void mo_doc_add(struct mo_document *doc, enum mo_doc_kind kind, const uint8_t *bytes, size_t length,
                bool source_break);
MO_MUST_USE bool mo_doc_render(struct mo_document *doc, struct mo_format_options options,
                               struct mo_buffer *output);

#endif
