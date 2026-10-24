/* Independent observable formatter contracts, shared by corpus and fuzz runs.
 * Compare public trees, raw leaves, and comment attachments, not source spans. */
#ifndef MO_TEST_FORMAT_CONTRACT_H
#define MO_TEST_FORMAT_CONTRACT_H

#include "base/limits.h"
#include "prim/prim.h"
#include "syntax/format.h"

#include <string.h>

static inline uint32_t format_display_width(const uint8_t *bytes, size_t length) {
    uint64_t width;
    MO_ASSERT(mo_unicode16_display_width(bytes, length, &width));
    MO_ASSERT(width <= UINT32_MAX);
    return (uint32_t)width;
}

static inline void format_word(struct mo_buffer *b, uint32_t value) {
    MO_ASSERT(mo_buffer_append_le32(b, value));
}

static inline void format_comments(struct mo_buffer *b, const struct mo_syntax_comment *c) {
    for (; c != NULL; c = c->next) {
        format_word(b, c->span.end - c->span.start);
        format_word(b, c->documentation);
        format_word(b, (uint32_t)c->anchor);
        format_word(b, (uint32_t)c->token);
        format_word(b, c->token_index);
        MO_ASSERT(mo_buffer_append(b, c->text, c->span.end - c->span.start));
    }
    format_word(b, UINT32_MAX);
}

struct format_visit {
    const struct mo_syntax *node;
    bool sum, omit_comments;
    bool spec_body; /* a function's code after a non-empty specification */
    bool blank;     /* the first line of that code, which always follows a blank line */
};

/* Expand += into assignment/addition, independently of the printer's matching
 * logic. Only the synthesized repeated target has no comments. Iterative
 * traversal also covers the parser's arbitrarily long flat chains. */
static inline void format_tree(struct mo_buffer *b, const struct mo_syntax *root) {
    struct mo_buffer stack;
    mo_buffer_init(&stack, 1 << 27);
    struct format_visit visit = {.node = root};
    MO_ASSERT(mo_buffer_append(&stack, &visit, sizeof visit));
    while (stack.length != 0) {
        stack.length -= sizeof visit;
        memcpy(&visit, stack.bytes + stack.length, sizeof visit);
        const struct mo_syntax *n = visit.node;
        if (n == NULL) {
            format_word(b, UINT32_MAX);
            continue;
        }
        bool increment = n->kind == MO_S_ADD_ASSIGN;
        format_word(b, (uint32_t)(visit.sum ? MO_S_BINARY : increment ? MO_S_BIND : n->kind));
        format_word(b, (uint32_t)(visit.sum ? MO_T_PLUS : n->token));
        format_word(b, n->count);
        format_word(b, visit.sum ? 0 : visit.blank ? 1u : n->blank_before);
        format_word(b, n->text == NULL ? 0 : n->span.end - n->span.start);
        if (n->text != NULL)
            MO_ASSERT(mo_buffer_append(b, n->text, n->span.end - n->span.start));
        format_comments(b, visit.sum || visit.omit_comments ? NULL : n->leading);
        format_comments(b, visit.sum || visit.omit_comments ? NULL : n->trailing);
        format_comments(b, visit.sum || visit.omit_comments ? NULL : n->ending);
        for (uint32_t i = n->count; i != 0; i--) {
            struct format_visit child = {
                .node = increment && !visit.sum && i == 2 ? n : n->children[i - 1],
                .sum = increment && !visit.sum && i == 2,
                .omit_comments = visit.omit_comments || (visit.sum && i == 1),
                .spec_body = n->kind == MO_S_FN && i == 6 && n->children[4] != NULL &&
                             n->children[4]->count != 0,
                .blank = visit.spec_body && i == 1,
            };
            MO_ASSERT(mo_buffer_append(&stack, &child, sizeof child));
        }
    }
    mo_buffer_free(&stack);
}

static inline bool format_buffer_equal(const struct mo_buffer *a, const struct mo_buffer *b) {
    return a->length == b->length && (a->length == 0 || memcmp(a->bytes, b->bytes, a->length) == 0);
}

#endif
