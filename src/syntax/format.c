#include "syntax/format.h"

#include "base/limits.h"
#include "syntax/format-doc.h"

#include <string.h>

struct comment_cursor {
    const struct mo_syntax *node;
    const struct mo_syntax_comment *next;
    uint32_t counts[MO_T_PERCENT + 1];
};

struct printer {
    struct mo_document doc;
    uint32_t depth;
    struct comment_cursor *cursor;
};

static void node(struct printer *f, const struct mo_syntax *n);
static const char *token(enum mo_syntax_token t);

static void op(struct printer *f, enum mo_doc_kind kind) {
    mo_doc_add(&f->doc, kind, NULL, 0);
}

static void anchor(struct printer *f, struct comment_cursor *cursor,
                   enum mo_syntax_comment_anchor kind, enum mo_syntax_token t, uint32_t index) {
    while (cursor->next != NULL && cursor->next->anchor == kind && cursor->next->token == t &&
           cursor->next->token_index == index) {
        const struct mo_syntax_comment *c = cursor->next;
        mo_doc_add(&f->doc, MO_DOC_COMMENT, c->text, c->span.end - c->span.start);
        op(f, MO_DOC_SUFFIX_LINE);
        cursor->next = c->next;
    }
}

static void token_anchor(struct printer *f, enum mo_syntax_token t) {
    anchor(f, f->cursor, MO_COMMENT_TOKEN, t, f->cursor->counts[t]++);
}

static const char *const spellings[MO_T_PERCENT + 1] = {[MO_T_MODULE] = "module",
                                                        [MO_T_EXPOSE] = "expose",
                                                        [MO_T_USE] = "use",
                                                        [MO_T_FN] = "fn",
                                                        [MO_T_STRUCT] = "struct",
                                                        [MO_T_ENUM] = "enum",
                                                        [MO_T_VAR] = "var",
                                                        [MO_T_IF] = "if",
                                                        [MO_T_ELSE] = "else",
                                                        [MO_T_CASE] = "case",
                                                        [MO_T_FOR] = "for",
                                                        [MO_T_IN] = "in",
                                                        [MO_T_WHILE] = "while",
                                                        [MO_T_BREAK] = "break",
                                                        [MO_T_CONTINUE] = "continue",
                                                        [MO_T_RETURN] = "return",
                                                        [MO_T_TRY] = "try",
                                                        [MO_T_AND] = "and",
                                                        [MO_T_OR] = "or",
                                                        [MO_T_NOT] = "not",
                                                        [MO_T_IS] = "is",
                                                        [MO_T_IMPLIES] = "implies",
                                                        [MO_T_TRUE] = "true",
                                                        [MO_T_FALSE] = "false",
                                                        [MO_T_STOP] = "stop",
                                                        [MO_T_PROMISE] = "promise",
                                                        [MO_T_ALLOWS] = "allows",
                                                        [MO_T_FORBIDS] = "forbids",
                                                        [MO_T_EXAMPLES] = "examples",
                                                        [MO_T_TEST] = "test",
                                                        [MO_T_ASSERT] = "assert",
                                                        [MO_T_WHERE] = "where",
                                                        [MO_T_END] = "end",
                                                        [MO_T_LPAREN] = "(",
                                                        [MO_T_RPAREN] = ")",
                                                        [MO_T_LBRACK] = "[",
                                                        [MO_T_RBRACK] = "]",
                                                        [MO_T_LBRACE] = "{",
                                                        [MO_T_RBRACE] = "}",
                                                        [MO_T_COMMA] = ",",
                                                        [MO_T_COLON] = ":",
                                                        [MO_T_DOT] = ".",
                                                        [MO_T_DOTDOT] = "..",
                                                        [MO_T_BAR] = "|",
                                                        [MO_T_ASSIGN] = "=",
                                                        [MO_T_PLUSEQ] = "+=",
                                                        [MO_T_MINUSEQ] = "-=",
                                                        [MO_T_PLUS] = "+",
                                                        [MO_T_MINUS] = "-",
                                                        [MO_T_STAR] = "*",
                                                        [MO_T_SLASH] = "/",
                                                        [MO_T_PERCENT] = "%",
                                                        [MO_T_EQ] = "==",
                                                        [MO_T_NE] = "!=",
                                                        [MO_T_LT] = "<",
                                                        [MO_T_LE] = "<=",
                                                        [MO_T_GT] = ">",
                                                        [MO_T_GE] = ">="};

static void text(struct printer *f, const char *s) {
    while (*s != '\0') {
        if (*s == ' ') {
            mo_doc_add(&f->doc, MO_DOC_SPACE, NULL, 1);
            s++;
            continue;
        }
        size_t length = 0;
        enum mo_syntax_token t = MO_T_EOF;
        for (unsigned i = MO_T_MODULE; i <= MO_T_PERCENT; i++) {
            if (spellings[i] != NULL) {
                size_t n = strlen(spellings[i]);
                if (n > length && strncmp(s, spellings[i], n) == 0) {
                    length = n;
                    t = (enum mo_syntax_token)i;
                }
            }
        }
        MO_ASSERT(length != 0);
        mo_doc_add(&f->doc, MO_DOC_TEXT, (const uint8_t *)s, length);
        token_anchor(f, t);
        s += length;
    }
}

static void soft(struct printer *f, unsigned space) {
    mo_doc_add(&f->doc, MO_DOC_BREAK, NULL, space);
}

static void comments(struct printer *f, const struct mo_syntax_comment *c, bool trailing) {
    for (; c != NULL; c = c->next) {
        if (!trailing)
            op(f, MO_DOC_LINE_IF);
        mo_doc_add(&f->doc, trailing ? MO_DOC_COMMENT : MO_DOC_TEXT, c->text,
                   c->span.end - c->span.start);
        if (!trailing)
            op(f, MO_DOC_LINE);
    }
}

static void statements(struct printer *f, const struct mo_syntax *seq, bool top) {
    (void)top;
    for (uint32_t i = 0; i < seq->count && f->doc.ok; i++) {
        const struct mo_syntax *n = seq->children[i];
        if (n->blank_before)
            op(f, MO_DOC_BLANK);
        node(f, n);
        op(f, MO_DOC_LINE_IF);
    }
}

static void separated(struct printer *f, const struct mo_syntax *seq, const char *separator) {
    bool dotted = strcmp(separator, ".") == 0;
    if (dotted) {
        /* A module path needs its own fit decision: expose/use lines make
         * its enclosing header group multiline even when this path fits. */
        op(f, MO_DOC_GROUP);
        op(f, MO_DOC_NEST);
    }
    for (uint32_t i = 0; i < seq->count && f->doc.ok; i++) {
        if (i != 0) {
            if (dotted)
                soft(f, 0);
            text(f, separator);
        }
        node(f, seq->children[i]);
    }
    if (dotted) {
        op(f, MO_DOC_UNNEST);
        op(f, MO_DOC_END_GROUP);
    }
}

static bool bracketed(const struct mo_syntax *n) {
    return n->kind == MO_S_GROUP || n->kind == MO_S_TUPLE || n->kind == MO_S_LIST ||
           n->kind == MO_S_MAP;
}

static void brackets(struct printer *f, const struct mo_syntax *owner,
                     const struct mo_syntax *boundary_owner, struct mo_syntax *const *items,
                     uint32_t count, const char *open, const char *close) {
    struct comment_cursor separate = {.node = boundary_owner, .next = boundary_owner->trailing};
    struct comment_cursor *cursor = boundary_owner == f->cursor->node ? f->cursor : &separate;
    op(f, MO_DOC_ALIGN_LINE);
    op(f, MO_DOC_GROUP);
    text(f, open);
    op(f, MO_DOC_NEST);
    /* A sole bracketed value or block callback provides the call's layout;
     * do not add another pair of enclosing lines around that inner scope. */
    bool envelope = owner != NULL && owner->kind == MO_S_CALL && count == 1 &&
                    (bracketed(items[0]) || (items[0]->kind == MO_S_LAMBDA &&
                                             items[0]->children[1]->kind == MO_S_SEQUENCE));
    if (!envelope && count != 0)
        soft(f, 0);
    for (uint32_t i = 0; i < count && f->doc.ok; i++) {
        node(f, items[i]);
        if (i + 1 != count)
            text(f, ",");
        anchor(f, cursor, MO_COMMENT_ITEM, MO_T_EOF, i);
        if (i + 1 != count)
            soft(f, 1);
    }
    if (owner != NULL)
        comments(f, owner->ending, false);
    op(f, MO_DOC_UNNEST);
    if (!envelope)
        soft(f, 0);
    text(f, close);
    op(f, MO_DOC_END_GROUP);
    op(f, MO_DOC_END_ALIGN);
    if (cursor != f->cursor && cursor->next != NULL)
        f->doc.ok = false;
}

static void sequence_brackets(struct printer *f, const struct mo_syntax *owner,
                              const struct mo_syntax *seq, const char *open, const char *close) {
    brackets(f, owner, seq, seq->children, seq->count, open, close);
}

static const char *token(enum mo_syntax_token t) {
    MO_ASSERT(t <= MO_T_PERCENT && spellings[t] != NULL);
    return spellings[t];
}

static void binary_step(struct printer *f, const struct mo_syntax *operator_node,
                        enum mo_syntax_token t, const struct mo_syntax *right) {
    const char *word = token(t);
    unsigned space = t == MO_T_DOTDOT ? 0 : 1;
    /* A comment queued before an operator must wait until the operator has
     * been written: a leading '-' on the next line changes the tree. */
    if (space != 0)
        text(f, " ");
    if (operator_node != NULL)
        node(f, operator_node);
    else
        text(f, word);
    /* A bracketed operand has its own breaks. Keep the operator and opener
     * together, as in the reference's multiline-list equality examples. */
    if (bracketed(right)) {
        if (space != 0)
            text(f, " ");
    } else
        soft(f, space);
    node(f, right);
}

static bool same_target(const struct mo_syntax *a, const struct mo_syntax *b) {
    if (a->kind != b->kind || a->count != b->count || a->leading != NULL || a->trailing != NULL ||
        a->ending != NULL || b->leading != NULL || b->trailing != NULL || b->ending != NULL)
        return false;
    if (a->kind == MO_S_NAME) {
        uint32_t size = a->span.end - a->span.start;
        return size == b->span.end - b->span.start && memcmp(a->text, b->text, size) == 0;
    }
    if (a->kind != MO_S_FIELD_ACCESS && a->kind != MO_S_POSTFIX)
        return false;
    for (uint32_t i = 0; i < a->count; i++)
        if (!same_target(a->children[i], b->children[i]))
            return false;
    return true;
}

static const struct mo_syntax *addition(const struct mo_syntax *n) {
    const struct mo_syntax *rhs = n->children[1];
    if (n->kind == MO_S_BIND && n->trailing == NULL && rhs->kind == MO_S_BINARY &&
        rhs->token == MO_T_PLUS && rhs->leading == NULL && rhs->trailing == NULL &&
        rhs->ending == NULL && same_target(n->children[0], rhs->children[0]))
        return rhs->children[1];
    return NULL;
}

static void block_body(struct printer *f, const struct mo_syntax *body,
                       const struct mo_syntax *owner) {
    op(f, MO_DOC_LINE_IF);
    op(f, MO_DOC_NEST);
    statements(f, body, false);
    comments(f, owner->ending, false);
    op(f, MO_DOC_UNNEST);
}

static void postfix_step(struct printer *f, const struct mo_syntax *n) {
    if (n->kind == MO_S_CALL) {
        sequence_brackets(f, n, n->children[n->count - 1], "(", ")");
    } else {
        const struct mo_syntax *field = n->children[n->count - 1];
        soft(f, 0);
        text(f, ".");
        node(f, field);
    }
}

static void print_node(struct printer *f, const struct mo_syntax *n) {
    switch (n->kind) {
    case MO_S_FILE: {
        const struct mo_syntax *header = n->children[0], *items = n->children[1];
        if (header != NULL) {
            node(f, header);
            op(f, MO_DOC_LINE_IF);
        }
        statements(f, items, true);
        comments(f, n->ending, false);
        break;
    }
    case MO_S_MODULE:
        text(f, "module ");
        separated(f, n->children[0], ".");
        if (n->children[1] != NULL) {
            op(f, MO_DOC_LINE_IF);
            node(f, n->children[1]);
        }
        for (uint32_t i = 0; i < n->children[2]->count; i++) {
            op(f, MO_DOC_LINE_IF);
            node(f, n->children[2]->children[i]);
        }
        comments(f, n->ending, false);
        break;
    case MO_S_EXPOSE:
        text(f, "expose ");
        for (uint32_t i = 0; i < n->count; i++) {
            if (i != 0) {
                text(f, ",");
                soft(f, 1);
            }
            node(f, n->children[i]);
        }
        break;
    case MO_S_EXPOSE_ALL:
        node(f, n->children[0]);
        text(f, "(..)");
        break;
    case MO_S_USE:
        text(f, "use ");
        separated(f, n->children[0], ".");
        if (n->children[1] != NULL)
            sequence_brackets(f, NULL, n->children[1], "{", "}");
        break;
    case MO_S_FN: {
        op(f, MO_DOC_GROUP);
        text(f, "fn ");
        node(f, n->children[0]);
        sequence_brackets(f, NULL, n->children[1], "(", ")");
        if (n->children[2] != NULL) {
            text(f, " : ");
            node(f, n->children[2]);
        }
        if (n->children[3] != NULL) {
            op(f, MO_DOC_NEST);
            soft(f, 1);
            node(f, n->children[3]);
            op(f, MO_DOC_UNNEST);
        }
        op(f, MO_DOC_END_GROUP);
        op(f, MO_DOC_LINE_IF);
        op(f, MO_DOC_NEST);
        const struct mo_syntax *spec = n->children[4], *body = n->children[5];
        for (uint32_t i = 0; i < spec->count; i++) {
            if (spec->children[i]->blank_before)
                op(f, MO_DOC_BLANK);
            node(f, spec->children[i]);
            op(f, MO_DOC_LINE_IF);
        }
        /* A blank line separates the specification from the code (reference 12). */
        if (spec->count != 0 && body->count != 0 && !body->children[0]->blank_before)
            op(f, MO_DOC_BLANK);
        statements(f, body, false);
        comments(f, n->ending, false);
        op(f, MO_DOC_UNNEST);
        text(f, "end");
        break;
    }
    case MO_S_PARAM:
    case MO_S_FIELD:
    case MO_S_NAMED_ARG:
    case MO_S_MAP_ENTRY:
        node(f, n->children[0]);
        text(f, ": ");
        node(f, n->children[1]);
        break;
    case MO_S_WHERE:
        text(f, "where ");
        for (uint32_t i = 0; i < n->count; i++) {
            if (i != 0) {
                text(f, ",");
                soft(f, 1);
            }
            node(f, n->children[i]->children[0]);
            text(f, ": ");
            node(f, n->children[i]->children[1]);
        }
        break;
    case MO_S_STRUCT:
    case MO_S_ENUM:
        text(f, n->kind == MO_S_STRUCT ? "struct " : "enum ");
        node(f, n->children[0]);
        if (n->children[1] != NULL)
            sequence_brackets(f, NULL, n->children[1], "(", ")");
        block_body(f, n->children[2], n);
        text(f, "end");
        break;
    case MO_S_CASE_DECL:
        node(f, n->children[0]);
        if (n->children[1] != NULL) {
            const struct mo_syntax *fields = n->children[1];
            if (fields->kind == MO_S_SEQUENCE)
                sequence_brackets(f, NULL, fields, "(", ")");
            else
                brackets(f, NULL, n, n->children + 1, 1, "(", ")");
        }
        break;
    case MO_S_TEST:
        text(f, "test ");
        node(f, n->children[0]);
        block_body(f, n->children[1], n);
        text(f, "end");
        break;
    case MO_S_PROMISE:
    case MO_S_EXAMPLES: {
        bool promise = n->kind == MO_S_PROMISE;
        text(f, promise ? "promise " : "examples");
        if (promise)
            node(f, n->children[0]);
        op(f, MO_DOC_LINE_IF);
        op(f, MO_DOC_NEST);
        statements(f, n->children[promise ? 1 : 0], false);
        if (promise)
            statements(f, n->children[2], false);
        comments(f, n->ending, false);
        op(f, MO_DOC_UNNEST);
        text(f, "end");
        break;
    }
    case MO_S_ANCHOR:
        text(f, n->token == MO_T_ALLOWS ? "allows " : "forbids ");
        separated(f, n->children[0], ", ");
        break;
    case MO_S_TYPE:
        if (n->token == MO_T_FN || n->token == MO_T_LPAREN) {
            if (n->token == MO_T_FN)
                text(f, "fn");
            sequence_brackets(f, NULL, n->children[0], "(", ")");
            if (n->token == MO_T_FN && n->children[1] != NULL) {
                text(f, " : ");
                node(f, n->children[1]);
            }
        } else {
            node(f, n->children[0]);
            if (n->children[1] != NULL)
                sequence_brackets(f, NULL, n->children[1], "(", ")");
        }
        break;
    case MO_S_SEQUENCE:
        separated(f, n, ", ");
        break;
    case MO_S_EXPR_STMT:
    case MO_S_BINDER:
        node(f, n->children[0]);
        break;
    case MO_S_CONST:
    case MO_S_BIND:
    case MO_S_VAR:
    case MO_S_ADD_ASSIGN:
    case MO_S_SUB_ASSIGN: {
        const struct mo_syntax *rhs = addition(n);
        /* The assignment prefix is a separate group: a block in the value
         * must not force a break between '=' and its opening keyword. */
        op(f, MO_DOC_GROUP);
        if (n->kind == MO_S_VAR)
            text(f, "var ");
        node(f, n->children[0]);
        text(f, rhs != NULL || n->kind == MO_S_ADD_ASSIGN ? " +="
                : n->kind == MO_S_SUB_ASSIGN              ? " -="
                                                          : " =");
        if (rhs == NULL)
            rhs = n->children[1];
        soft(f, 1);
        op(f, MO_DOC_END_GROUP);
        node(f, rhs);
        break;
    }
    case MO_S_RETURN:
    case MO_S_BREAK:
    case MO_S_CONTINUE: {
        bool ret = n->kind == MO_S_RETURN;
        text(f, ret ? "return" : n->kind == MO_S_BREAK ? "break" : "continue");
        if (ret && n->children[0] != NULL) {
            text(f, " ");
            node(f, n->children[0]);
        }
        const struct mo_syntax *guard = n->children[ret ? 1 : 0];
        if (guard != NULL) {
            text(f, " if ");
            node(f, guard);
        }
        break;
    }
    case MO_S_ASSERT:
        text(f, "assert ");
        node(f, n->children[0]);
        break;
    case MO_S_FOR:
    case MO_S_WHILE: {
        bool loop = n->kind == MO_S_FOR;
        text(f, loop ? "for " : "while ");
        node(f, n->children[0]);
        if (loop) {
            text(f, " in ");
            node(f, n->children[1]);
        }
        block_body(f, n->children[loop ? 2 : 1], n);
        text(f, "end");
        break;
    }
    case MO_S_IF:
    case MO_S_LINE_IF: {
        bool block = n->kind == MO_S_IF;
        text(f, "if ");
        node(f, n->children[0]);
        if (block)
            block_body(f, n->children[1], n);
        else {
            text(f, ": ");
            node(f, n->children[1]);
        }
        for (uint32_t i = 0; i < n->children[2]->count; i++) {
            const struct mo_syntax *part = n->children[2]->children[i];
            if (block)
                op(f, MO_DOC_LINE_IF);
            else
                text(f, " ");
            node(f, part);
        }
        if (block) {
            op(f, MO_DOC_LINE_IF);
            text(f, "end");
        }
        break;
    }
    case MO_S_ELSE:
    case MO_S_IF_PART: {
        bool branch = n->kind == MO_S_IF_PART;
        text(f, branch ? "else if " : "else");
        if (branch)
            node(f, n->children[0]);
        const struct mo_syntax *body = n->children[branch ? 1 : 0];
        if (body->kind == MO_S_SEQUENCE)
            block_body(f, body, n);
        else {
            text(f, ": ");
            node(f, body);
        }
        break;
    }
    case MO_S_CASE:
        text(f, "case ");
        node(f, n->children[0]);
        block_body(f, n->children[1], n);
        text(f, "end");
        break;
    case MO_S_ARM: {
        separated(f, n->children[0], " | ");
        if (n->children[1] != NULL) {
            text(f, " if ");
            node(f, n->children[1]);
        }
        text(f, ":");
        const struct mo_syntax *body = n->children[2];
        const struct mo_syntax *only = body->count == 1 ? body->children[0] : NULL;
        if (only != NULL && only->kind == MO_S_EXPR_STMT)
            only = only->children[0];
        bool inline_body = body->count == 1 && body->children[0]->leading == NULL &&
                           only->kind != MO_S_FOR && only->kind != MO_S_WHILE &&
                           only->kind != MO_S_CASE && only->kind != MO_S_IF;
        if (inline_body) {
            text(f, " ");
            node(f, body->children[0]);
            op(f, MO_DOC_LINE_IF);
            op(f, MO_DOC_NEST);
            comments(f, n->ending, false);
            op(f, MO_DOC_UNNEST);
        } else
            block_body(f, body, n);
        break;
    }
    case MO_S_LAMBDA:
        text(f, "fn");
        sequence_brackets(f, NULL, n->children[0], "(", ")");
        if (n->children[1]->kind == MO_S_SEQUENCE) {
            block_body(f, n->children[1], n);
            text(f, "end");
        } else {
            text(f, " ");
            node(f, n->children[1]);
            text(f, " end");
        }
        break;
    case MO_S_BINARY:
    case MO_S_IS:
    case MO_S_RANGE:
        node(f, n->children[0]);
        op(f, MO_DOC_NEST);
        binary_step(f, NULL, n->token, n->children[1]);
        op(f, MO_DOC_UNNEST);
        break;
    case MO_S_BINARY_CHAIN:
        node(f, n->children[0]);
        op(f, MO_DOC_NEST);
        for (uint32_t i = 1; i < n->count; i += 2)
            binary_step(f, n->children[i], n->children[i]->token, n->children[i + 1]);
        op(f, MO_DOC_UNNEST);
        break;
    case MO_S_NEGATE:
    case MO_S_NOT:
    case MO_S_TRY:
        text(f, n->kind == MO_S_NEGATE ? "-" : n->kind == MO_S_NOT ? "not " : "try ");
        if (n->kind == MO_S_NEGATE && n->children[0]->kind == MO_S_NEGATE)
            text(f, " ");
        node(f, n->children[0]);
        break;
    case MO_S_PREFIX:
        for (uint32_t i = 0; i + 1 < n->count; i++) {
            node(f, n->children[i]);
            if (n->children[i]->token != MO_T_MINUS || n->children[i + 1]->token == MO_T_MINUS)
                text(f, " ");
        }
        node(f, n->children[n->count - 1]);
        break;
    case MO_S_CALL:
    case MO_S_FIELD_ACCESS:
    case MO_S_QUALIFIED:
    case MO_S_POSITION:
        if (n->count == 2)
            node(f, n->children[0]);
        if (n->kind != MO_S_CALL)
            op(f, MO_DOC_NEST);
        postfix_step(f, n);
        if (n->kind != MO_S_CALL)
            op(f, MO_DOC_UNNEST);
        break;
    case MO_S_POSTFIX: {
        node(f, n->children[0]);
        op(f, MO_DOC_NEST);
        for (uint32_t i = 1; i < n->count; i++) {
            const struct mo_syntax *step = n->children[i];
            struct comment_cursor cursor = {.node = step, .next = step->trailing};
            struct comment_cursor *previous = f->cursor;
            f->cursor = &cursor;
            comments(f, step->leading, false);
            op(f, MO_DOC_GROUP);
            postfix_step(f, step);
            op(f, MO_DOC_END_GROUP);
            if (cursor.next != NULL)
                f->doc.ok = false;
            f->cursor = previous;
        }
        op(f, MO_DOC_UNNEST);
        break;
    }
    case MO_S_INT:
    case MO_S_DECIMAL:
    case MO_S_CHAR:
    case MO_S_TEXT:
    case MO_S_TRUE:
    case MO_S_FALSE:
    case MO_S_NAME:
    case MO_S_CAPNAME:
    case MO_S_UNDERSCORE:
    case MO_S_TOKEN:
        mo_doc_add(&f->doc, MO_DOC_TEXT, n->text, n->span.end - n->span.start);
        token_anchor(f, n->token);
        break;
    case MO_S_TEXT_INTERPOLATED:
        for (uint32_t i = 0; i < n->count; i++)
            node(f, n->children[i]);
        break;
    case MO_S_GROUP:
    case MO_S_TUPLE:
    case MO_S_LIST:
    case MO_S_MAP:
        brackets(f, n, n, n->children, n->count,
                 n->kind == MO_S_LIST  ? "["
                 : n->kind == MO_S_MAP ? "{"
                                       : "(",
                 n->kind == MO_S_LIST  ? "]"
                 : n->kind == MO_S_MAP ? "}"
                                       : ")");
        break;
    case MO_S_REST:
        text(f, "..");
        node(f, n->children[0]);
        break;
    case MO_S_STOP:
        text(f, "stop ");
        node(f, n->children[0]);
        break;
    }
}

static void node(struct printer *f, const struct mo_syntax *n) {
    if (!f->doc.ok)
        return;
    if (n == NULL || n->incomplete || ++f->depth > MO_LIMIT_SOURCE_INFERENCE_DEPTH) {
        f->doc.ok = false;
        return;
    }
    struct comment_cursor cursor = {.node = n, .next = n->trailing};
    struct comment_cursor *previous = f->cursor;
    f->cursor = &cursor;
    comments(f, n->leading, false);
    bool block = n->kind == MO_S_FN || n->kind == MO_S_STRUCT || n->kind == MO_S_ENUM ||
                 n->kind == MO_S_TEST || n->kind == MO_S_PROMISE || n->kind == MO_S_EXAMPLES ||
                 n->kind == MO_S_FOR || n->kind == MO_S_WHILE || n->kind == MO_S_IF ||
                 n->kind == MO_S_CASE || n->kind == MO_S_LAMBDA;
    if (block)
        op(f, MO_DOC_ALIGN_LINE);
    op(f, MO_DOC_GROUP);
    print_node(f, n);
    op(f, MO_DOC_END_GROUP);
    if (block)
        op(f, MO_DOC_END_ALIGN);
    if (cursor.next != NULL)
        f->doc.ok = false;
    f->cursor = previous;
    f->depth--;
}

bool mo_format(const struct mo_module_syntax *module, struct mo_format_options options,
               struct mo_buffer *output) {
    mo_buffer_clear(output);
    if (module == NULL || module->tree == NULL || options.width == 0 ||
        options.width > MO_LIMIT_LINE_LENGTH || options.display_width == NULL)
        return false;
    struct printer f = {0};
    mo_doc_init(&f.doc, options.display_width);
    node(&f, module->tree);
    bool ok = mo_doc_render(&f.doc, options, output);
    mo_doc_free(&f.doc);
    if (!ok)
        mo_buffer_clear(output);
    return ok;
}
