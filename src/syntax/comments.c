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

/* Find the innermost owner of an actual token, not the node with the last
 * source endpoint. Else parts include their parent's end only for own-line
 * scope comments; that end token still belongs to the enclosing if. */
static struct mo_syntax *token_owner(struct mo_syntax *n, struct mo_token token,
                                     struct mo_syntax *found) {
    if (n == NULL || token.span.start < n->span.start || token.span.end > n->span.end)
        return found;
    if (n->kind != MO_S_EXPR_STMT && n->kind != MO_S_BINDER && n->kind != MO_S_SEQUENCE &&
        !((n->kind == MO_S_ELSE || n->kind == MO_S_IF_PART) && token.kind == MO_T_END))
        found = n;
    if (n->count <= 6) {
        for (uint32_t i = 0; i < n->count; i++)
            found = token_owner(n->children[i], token, found);
    } else {
        uint32_t low = 0, high = n->count;
        while (low < high) {
            uint32_t mid = low + (high - low) / 2;
            if (n->children[mid]->span.start <= token.span.start)
                low = mid + 1;
            else
                high = mid;
        }
        if (low != 0)
            found = token_owner(n->children[low - 1], token, found);
    }
    return found;
}

struct item_list {
    struct mo_syntax *owner;
    struct mo_syntax **items;
    uint32_t count;
    enum mo_syntax_token closer;
};

static struct item_list bracket_items(struct mo_syntax *n) {
    struct item_list list = {.closer = MO_T_RPAREN};
    struct mo_syntax *seq = NULL;
    if (n->kind == MO_S_LIST || n->kind == MO_S_MAP || n->kind == MO_S_TUPLE) {
        seq = n;
        if (n->kind == MO_S_LIST)
            list.closer = MO_T_RBRACK;
        if (n->kind == MO_S_MAP)
            list.closer = MO_T_RBRACE;
    } else if (n->kind == MO_S_FN)
        seq = n->children[1];
    else if (n->kind == MO_S_CALL)
        seq = n->children[n->count - 1];
    else if (n->kind == MO_S_CASE_DECL && n->children[1] != NULL) {
        seq = n->children[1];
        if (seq->kind != MO_S_SEQUENCE)
            return list;
    }
    if (seq != NULL) {
        list.owner = seq;
        list.items = seq->children;
        list.count = seq->count;
    }
    return list;
}

/* A final-item comment and an optional trailing-comma comment have the same
 * semantic boundary. Dropping an optional trailing comma cannot
 * change its anchor. Descend first to prefer the innermost bracket list. */
static struct mo_syntax *item_owner(struct mo_syntax *n, struct mo_token preceding,
                                    enum mo_syntax_token next, uint32_t *index) {
    if (n == NULL || preceding.span.start < n->span.start || preceding.span.end > n->span.end)
        return NULL;
    if (n->count <= 6) {
        for (uint32_t i = 0; i < n->count; i++) {
            struct mo_syntax *found = item_owner(n->children[i], preceding, next, index);
            if (found != NULL)
                return found;
        }
    } else {
        uint32_t low = 0, high = n->count;
        while (low < high) {
            uint32_t mid = low + (high - low) / 2;
            if (n->children[mid]->span.start <= preceding.span.start)
                low = mid + 1;
            else
                high = mid;
        }
        if (low != 0) {
            struct mo_syntax *found = item_owner(n->children[low - 1], preceding, next, index);
            if (found != NULL)
                return found;
        }
    }
    struct item_list list = bracket_items(n);
    if (list.count == 0 || next != list.closer)
        return NULL;
    uint32_t target = preceding.kind == MO_T_COMMA ? preceding.span.start : preceding.span.end;
    uint32_t low = 0, high = list.count;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        if (list.items[mid]->span.end <= target)
            low = mid + 1;
        else
            high = mid;
    }
    if (low != list.count ||
        (preceding.kind != MO_T_COMMA && list.items[low - 1]->span.end != target))
        return NULL;
    *index = low - 1;
    return list.owner;
}

struct token_counter {
    struct mo_syntax *owner;
    enum mo_syntax_token kind;
    uint32_t count;
};

static int counter_order(const void *left, const void *right) {
    const struct token_counter *a = left, *b = right;
    uintptr_t x = (uintptr_t)a->owner, y = (uintptr_t)b->owner;
    if (x != y)
        return x < y ? -1 : 1;
    return a->kind < b->kind ? -1 : a->kind > b->kind ? 1 : 0;
}

static bool inline_anchors(struct mo_arena *scratch, struct mo_arena *arena,
                           const struct mo_sources *sources, struct mo_syntax *tree,
                           struct mo_syntax_comment *comments) {
    size_t count = 0;
    for (struct mo_syntax_comment *c = comments; c != NULL; c = c->next)
        if (c->anchor == MO_COMMENT_TOKEN)
            count++;
    if (count == 0)
        return true;
    struct token_counter *counters =
        mo_arena_allocate_array(scratch, count, sizeof *counters, _Alignof(struct token_counter));
    if (counters == NULL)
        return false;
    size_t at = 0;
    for (struct mo_syntax_comment *c = comments; c != NULL; c = c->next) {
        if (c->anchor != MO_COMMENT_TOKEN)
            continue;
        struct mo_comment_storage *storage = (struct mo_comment_storage *)c;
        storage->owner = token_owner(tree, storage->preceding, tree);
        counters[at++] = (struct token_counter){storage->owner, c->token, 0};
    }
    qsort(counters, count, sizeof *counters, counter_order);
    struct mo_diags quiet;
    if (!mo_diags_init(&quiet)) {
        mo_diags_free(&quiet);
        return false;
    }
    struct mo_lex lx;
    mo_syntax_lex_init(&lx, arena, sources, tree->span.file, &quiet);
    lx.ignore_comments = true;
    lx.recovering = true;
    struct mo_syntax_comment *c = comments;
    struct mo_token previous = {0};
    uint32_t previous_index = 0;
    for (;;) {
        struct mo_token token = mo_syntax_next(&lx);
        if (token.kind == MO_T_NL)
            continue;
        if (token.kind == MO_T_ERROR) {
            mo_syntax_resume(&lx);
            if (lx.pos == lx.source->length)
                break;
            continue;
        }
        while (c != NULL && c->span.start < token.span.start) {
            if (c->anchor == MO_COMMENT_TOKEN) {
                struct mo_comment_storage *storage = (struct mo_comment_storage *)c;
                if (storage->preceding.span.start == previous.span.start &&
                    storage->preceding.kind == previous.kind) {
                    c->token_index = previous_index;
                    struct mo_syntax *boundary =
                        item_owner(tree, previous, token.kind, &c->token_index);
                    if (boundary != NULL) {
                        c->anchor = MO_COMMENT_ITEM;
                        c->token = MO_T_EOF;
                        storage->owner = boundary;
                    }
                }
            }
            c = c->next;
        }
        if (token.kind == MO_T_EOF)
            break;
        struct token_counter key = {token_owner(tree, token, tree), token.kind, 0};
        size_t low = 0, high = count;
        while (low < high) {
            size_t mid = low + (high - low) / 2;
            if (counter_order(&counters[mid], &key) < 0)
                low = mid + 1;
            else
                high = mid;
        }
        previous_index =
            low < count && counter_order(&counters[low], &key) == 0 ? counters[low].count++ : 0;
        previous = token;
    }
    mo_diags_free(&quiet);
    return true;
}

static void attach(struct mo_syntax_comment **list, struct mo_syntax_comment *comment) {
    /* Attachment walks comments backward, so prepend keeps source order. */
    comment->next = *list;
    *list = comment;
}

static bool blank_between(const struct mo_source *source, uint32_t start, uint32_t end) {
    bool whole_line = false, empty = true;
    MO_ASSERT(start <= end && end <= source->length);
    for (uint32_t i = start; i < end; i++) {
        uint8_t byte = source->bytes[i];
        if (byte == '\n') {
            if (whole_line && empty)
                return true;
            whole_line = true;
            empty = true;
        } else if (byte != ' ' && byte != '\t' && byte != '\r')
            empty = false;
    }
    return false;
}

static uint32_t content_end(const struct mo_syntax *n) {
    /* Arm and else-part spans deliberately extend past their own content. */
    if (n->kind == MO_S_ARM || n->kind == MO_S_IF_PART || n->kind == MO_S_ELSE) {
        const struct mo_syntax *body = n->children[n->kind == MO_S_ARM       ? 2
                                                   : n->kind == MO_S_IF_PART ? 1
                                                                             : 0];
        if (body != NULL && body->kind == MO_S_SEQUENCE) {
            if (body->count != 0)
                return body->children[body->count - 1]->span.end;
            if (n->kind != MO_S_ELSE && n->children[0] != NULL)
                return n->children[0]->span.end;
            return n->span.start;
        }
    }
    return n->span.end;
}

static void statement_blanks(const struct mo_source *source, struct mo_syntax *seq,
                             const struct mo_syntax *previous) {
    for (uint32_t i = 0; i < seq->count; i++) {
        struct mo_syntax *n = seq->children[i];
        uint32_t start = n->span.start;
        uint32_t end = previous == NULL ? start : content_end(previous);
        n->blank_before = end < start && blank_between(source, end, start);
        previous = n;
    }
}

static void blank_trivia(const struct mo_source *source, struct mo_syntax *n) {
    if (n == NULL)
        return;
    for (uint32_t i = 0; i < n->count; i++)
        blank_trivia(source, n->children[i]);
    struct mo_syntax *body = NULL;
    if (n->kind == MO_S_FILE)
        statement_blanks(source, n->children[1], n->children[0]);
    else if (n->kind == MO_S_FN) {
        struct mo_syntax *spec = n->children[4];
        statement_blanks(source, spec, NULL);
        statement_blanks(source, n->children[5],
                         spec->count == 0 ? NULL : spec->children[spec->count - 1]);
    } else if (n->kind == MO_S_STRUCT || n->kind == MO_S_ENUM || n->kind == MO_S_FOR ||
               n->kind == MO_S_ARM)
        body = n->children[2];
    else if (n->kind == MO_S_TEST || n->kind == MO_S_WHILE || n->kind == MO_S_IF ||
             n->kind == MO_S_IF_PART || n->kind == MO_S_CASE || n->kind == MO_S_LAMBDA)
        body = n->children[1];
    else if (n->kind == MO_S_ELSE || n->kind == MO_S_EXAMPLES)
        body = n->children[0];
    else if (n->kind == MO_S_PROMISE) {
        statement_blanks(source, n->children[1], NULL);
        const struct mo_syntax *allows = n->children[1];
        statement_blanks(source, n->children[2],
                         allows->count == 0 ? NULL : allows->children[allows->count - 1]);
    }
    if (body != NULL && body->kind == MO_S_SEQUENCE)
        statement_blanks(source, body, NULL);
}

bool mo_syntax_attach(struct mo_arena *arena, const struct mo_sources *sources,
                      struct mo_syntax *tree, struct mo_syntax_comment *comments) {
    if (comments == NULL) {
        blank_trivia(&sources->files[tree->span.file], tree);
        return true;
    }
    struct mo_arena scratch;
    size_t nodes = count_nodes(tree);
    MO_ASSERT(arena->used <= arena->limit);
    mo_arena_init(&scratch, arena->limit - arena->used);
    struct candidate *starts =
        mo_arena_allocate_array(&scratch, nodes, sizeof *starts, _Alignof(struct candidate));
    if (starts == NULL || !inline_anchors(&scratch, arena, sources, tree, comments)) {
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
    qsort(starts, count, sizeof *starts, order_start);
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
        if (comment->anchor != MO_COMMENT_OWN_LINE) {
            struct mo_comment_storage *storage = (struct mo_comment_storage *)comment;
            attach(&storage->owner->trailing, comment);
            continue;
        }
        uint32_t end = tree->span.end;
        struct mo_syntax *enclosing = owner(tree, comment->span.start, tree, &end);
        size_t low = 0, high = count;
        {
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
        }
    }
    mo_arena_free(&scratch);
    blank_trivia(&sources->files[tree->span.file], tree);
    return true;
}
