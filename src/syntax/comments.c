/* Index source positions once, so comment attachment is O(n log n), even on
 * generated files with many declarations and comments. Scratch is freed here. */
#include "syntax/internal.h"

#include <stdlib.h>

struct candidate {
    struct mo_syntax *node;
    uint32_t indentation;
};

static MO_MUST_USE size_t count_nodes(const struct mo_syntax *n) {
    if (n == NULL)
        return 0;
    size_t count = 1;
    for (uint32_t i = 0; i < n->count; i++)
        count += count_nodes(n->children[i]);
    return count;
}

static void collect(struct mo_syntax *n, struct candidate *items, size_t *count) {
    if (n == NULL)
        return;
    if (n->kind != MO_S_SEQUENCE && n->kind != MO_S_FILE)
        items[(*count)++] = (struct candidate){n, 0};
    for (uint32_t i = 0; i < n->count; i++)
        collect(n->children[i], items, count);
}

static MO_MUST_USE int order_start(const void *left, const void *right) {
    const struct candidate *a = left, *b = right;
    if (a->indentation != b->indentation)
        return a->indentation < b->indentation ? -1 : 1;
    if (a->node->span.start != b->node->span.start)
        return a->node->span.start < b->node->span.start ? -1 : 1;
    /* At one start prefer the outer statement or declaration. */
    if (a->node->span.end != b->node->span.end)
        return a->node->span.end > b->node->span.end ? -1 : 1;
    return a->node->kind < b->node->kind ? -1 : a->node->kind > b->node->kind ? 1 : 0;
}

static MO_MUST_USE int order_end(const void *left, const void *right) {
    const struct candidate *a = left, *b = right;
    if (a->node->span.end != b->node->span.end)
        return a->node->span.end < b->node->span.end ? -1 : 1;
    /* At one end prefer the smallest, last node on the line. */
    if (a->node->span.start != b->node->span.start)
        return a->node->span.start < b->node->span.start ? -1 : 1;
    return a->node->kind > b->node->kind ? -1 : a->node->kind < b->node->kind ? 1 : 0;
}

static MO_MUST_USE bool block(const struct mo_syntax *n) {
    return n->kind == MO_S_FILE || n->kind == MO_S_MODULE || n->kind == MO_S_FN ||
           n->kind == MO_S_STRUCT || n->kind == MO_S_ENUM || n->kind == MO_S_TEST ||
           n->kind == MO_S_IF || n->kind == MO_S_ELSE || n->kind == MO_S_CASE ||
           n->kind == MO_S_FOR || n->kind == MO_S_WHILE || n->kind == MO_S_LAMBDA ||
           n->kind == MO_S_PROMISE || n->kind == MO_S_EXAMPLES || n->kind == MO_S_ARM ||
           n->kind == MO_S_IF_PART || n->kind == MO_S_LIST || n->kind == MO_S_MAP ||
           n->kind == MO_S_TUPLE || n->kind == MO_S_GROUP || n->kind == MO_S_CALL;
}

static MO_MUST_USE struct mo_syntax *owner(struct mo_syntax *n, uint32_t offset,
                                           struct mo_syntax *enclosing, uint32_t *end) {
    if (n == NULL || offset < n->span.start || offset > n->span.end)
        return enclosing;
    if (block(n)) {
        enclosing = n;
        *end = n->span.end;
    }
    if (n->kind == MO_S_IF && n->children[2] != NULL && n->children[2]->count != 0 &&
        offset < n->children[2]->span.start)
        *end = n->children[2]->span.start;
    if (n->count <= 6) {
        for (uint32_t i = 0; i < n->count; i++) {
            struct mo_syntax *child = n->children[i];
            if (child != NULL && child->span.start <= offset && child->span.end >= offset)
                enclosing = owner(child, offset, enclosing, end);
        }
    } else {
        size_t low = 0, high = n->count;
        while (low < high) {
            size_t mid = low + (high - low) / 2;
            if (n->children[mid]->span.start <= offset)
                low = mid + 1;
            else
                high = mid;
        }
        if (low > 0)
            enclosing = owner(n->children[low - 1], offset, enclosing, end);
    }
    return enclosing;
}

static void attach(struct mo_syntax_comment **list, struct mo_syntax_comment *comment) {
    /* Attachment walks comments backward, so prepend keeps source order. */
    comment->next = *list;
    *list = comment;
}

bool mo_syntax_attach(struct mo_arena *arena, const struct mo_sources *sources,
                      struct mo_syntax *tree, struct mo_syntax_comment *comments) {
    if (comments == NULL)
        return true;
    struct mo_arena scratch;
    size_t nodes = count_nodes(tree);
    MO_ASSERT(arena->used <= arena->limit);
    mo_arena_init(&scratch, arena->limit - arena->used);
    struct candidate *starts =
        mo_arena_allocate_array(&scratch, nodes, sizeof *starts, _Alignof(struct candidate));
    struct candidate *ends =
        mo_arena_allocate_array(&scratch, nodes, sizeof *ends, _Alignof(struct candidate));
    if (starts == NULL || ends == NULL) {
        mo_arena_free(&scratch);
        return false;
    }
    size_t count = 0;
    collect(tree, starts, &count);
    /* With indentation zero, order_start sorts by offset. Count columns once,
     * rather than rescanning a long source line for each of its nodes. */
    qsort(starts, count, sizeof *starts, order_start);
    const struct mo_source *file = &sources->files[tree->span.file];
    uint32_t offset = 0, column = 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t target = starts[i].node->span.start;
        while (offset < target) {
            uint32_t cp = file->bytes[offset];
            uint32_t width = mo_syntax_decode(file->bytes + offset, file->length - offset, &cp);
            if (width == 0)
                width = 1;
            MO_ASSERT(width <= target - offset);
            if (cp == '\n')
                column = 0;
            else if (cp != '\r' || offset + 1 == file->length || file->bytes[offset + 1] != '\n')
                column++;
            offset += width;
        }
        starts[i].indentation = column;
    }
    for (size_t i = 0; i < count; i++)
        ends[i] = starts[i];
    qsort(starts, count, sizeof *starts, order_start);
    qsort(ends, count, sizeof *ends, order_end);
    struct mo_syntax_comment *reversed = NULL;
    while (comments != NULL) {
        struct mo_syntax_comment *next = comments->next;
        comments->next = reversed;
        reversed = comments;
        comments = next;
    }
    while (reversed != NULL) {
        struct mo_syntax_comment *comment = reversed;
        reversed = comment->next;
        const struct mo_source *source = &sources->files[comment->span.file];
        uint32_t at = comment->span.start;
        while (at > 0 && (source->bytes[at - 1] == ' ' || source->bytes[at - 1] == '\t'))
            at--;
        bool own_line = at == 0 || source->bytes[at - 1] == '\n';
        uint32_t end = tree->span.end;
        struct mo_syntax *enclosing = owner(tree, comment->span.start, tree, &end);
        size_t low = 0, high = count;
        if (own_line) {
            while (low < high) {
                size_t mid = low + (high - low) / 2;
                bool before = starts[mid].indentation < comment->indentation ||
                              (starts[mid].indentation == comment->indentation &&
                               starts[mid].node->span.start < comment->span.end);
                if (before)
                    low = mid + 1;
                else
                    high = mid;
            }
            if (low < count && starts[low].indentation == comment->indentation &&
                starts[low].node->span.start <= end)
                attach(&starts[low].node->leading, comment);
            else
                attach(&enclosing->ending, comment);
        } else {
            while (low < high) {
                size_t mid = low + (high - low) / 2;
                if (ends[mid].node->span.end <= comment->span.start)
                    low = mid + 1;
                else
                    high = mid;
            }
            struct mo_position position =
                mo_sources_position(sources, comment->span.file, comment->span.start);
            if (low > 0 &&
                mo_sources_position(sources, comment->span.file, ends[low - 1].node->span.end)
                        .line == position.line)
                attach(&ends[low - 1].node->trailing, comment);
            else if (mo_sources_position(sources, comment->span.file, enclosing->span.start).line ==
                     position.line)
                attach(&enclosing->trailing, comment);
            else
                attach(&enclosing->ending, comment);
        }
    }
    mo_arena_free(&scratch);
    return true;
}
