#include "syntax/format.h"

#include "base/limits.h"
#include "syntax/format-doc.h"

#include <string.h>

struct printer {
    struct mo_document doc;
    const struct mo_source *source; /* provisional blank-line trivia */
    enum mo_format_policy policy;
    uint32_t depth;
};

static void node(struct printer *f, const struct mo_syntax *n);

static void op(struct printer *f, enum mo_doc_kind kind) {
    mo_doc_add(&f->doc, kind, NULL, 0, false);
}

static void text(struct printer *f, const char *s) {
    mo_doc_text(&f->doc, s);
}

static uint32_t line_at(const struct mo_source *source, uint32_t offset) {
    size_t low = 0, high = source->line_count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (source->line_starts[mid] <= offset)
            low = mid + 1;
        else
            high = mid;
    }
    return (uint32_t)low;
}

static uint32_t lines(struct printer *f, uint32_t start, uint32_t end) {
    if (f->source == NULL || start > end)
        return 0;
    MO_ASSERT(end <= f->source->length);
    return line_at(f->source, end) - line_at(f->source, start);
}

static void soft(struct printer *f, unsigned space, uint32_t start, uint32_t end) {
    mo_doc_add(&f->doc, MO_DOC_BREAK, NULL, space, lines(f, start, end) != 0);
}

static void comments(struct printer *f, const struct mo_syntax_comment *c, bool trailing) {
    uint32_t previous = 0;
    bool first = true;
    for (; c != NULL; c = c->next) {
        if (!trailing) {
            op(f, MO_DOC_LINE_IF);
            if (!first && lines(f, previous, c->span.start) > 1)
                op(f, MO_DOC_BLANK);
        }
        mo_doc_add(&f->doc, trailing ? MO_DOC_COMMENT : MO_DOC_TEXT, c->text,
                   c->span.end - c->span.start, false);
        if (!trailing)
            op(f, MO_DOC_LINE);
        previous = c->span.end;
        first = false;
    }
}

/* A scope can own a comment on its opening line as well as after its
 * closing token. Those have the same attachment, but distinct spans. */
static void trailing(struct printer *f, const struct mo_syntax *n, bool header) {
    for (const struct mo_syntax_comment *c = n->trailing; c != NULL; c = c->next)
        if ((c->span.start < n->span.end) == header)
            mo_doc_add(&f->doc, MO_DOC_COMMENT, c->text, c->span.end - c->span.start, false);
}

static uint32_t first(const struct mo_syntax *n) {
    return n->leading != NULL ? n->leading->span.start : n->span.start;
}

static bool declaration(const struct mo_syntax *n) {
    return n->kind == MO_S_FN || n->kind == MO_S_STRUCT || n->kind == MO_S_ENUM ||
           n->kind == MO_S_TEST;
}

static void statements(struct printer *f, const struct mo_syntax *seq, bool top) {
    for (uint32_t i = 0; i < seq->count && f->doc.ok; i++) {
        const struct mo_syntax *n = seq->children[i];
        if (i != 0) {
            const struct mo_syntax *prev = seq->children[i - 1];
            if ((top && (declaration(prev) || declaration(n))) ||
                lines(f, prev->span.end, first(n)) > 1)
                op(f, MO_DOC_BLANK);
        }
        node(f, n);
        op(f, MO_DOC_LINE_IF);
    }
}

static void separated(struct printer *f, const struct mo_syntax *seq, const char *separator) {
    for (uint32_t i = 0; i < seq->count && f->doc.ok; i++) {
        if (i != 0)
            text(f, separator);
        node(f, seq->children[i]);
    }
}

static uint32_t closing(struct printer *f, uint32_t start, uint32_t end, char close) {
    if (f->source == NULL)
        return end;
    for (uint32_t i = start; i < end; i++) {
        if (f->source->bytes[i] == '#')
            while (i < end && f->source->bytes[i] != '\n')
                i++;
        else if (f->source->bytes[i] == (uint8_t)close)
            return i;
    }
    return end;
}

static void brackets(struct printer *f, const struct mo_syntax *owner,
                     struct mo_syntax *const *items, uint32_t count, const char *open,
                     const char *close, uint32_t start, uint32_t end) {
    op(f, MO_DOC_GROUP);
    text(f, open);
    op(f, MO_DOC_NEST);
    uint32_t before = start;
    if (count != 0)
        soft(f, 0, before, first(items[0]));
    for (uint32_t i = 0; i < count && f->doc.ok; i++) {
        if (i != 0) {
            text(f, ",");
            soft(f, 1, items[i - 1]->span.end, first(items[i]));
        }
        node(f, items[i]);
        before = items[i]->span.end;
    }
    if (owner != NULL)
        comments(f, owner->ending, false);
    op(f, MO_DOC_UNNEST);
    soft(f, 0, before, closing(f, before, end, close[0]));
    text(f, close);
    op(f, MO_DOC_END_GROUP);
}

static void sequence_brackets(struct printer *f, const struct mo_syntax *owner,
                              const struct mo_syntax *seq, const char *open, const char *close,
                              uint32_t start, uint32_t end) {
    brackets(f, owner, seq->children, seq->count, open, close, start, end);
}

static const char *token(enum mo_syntax_token t) {
    static const char *const spellings[MO_T_PERCENT + 1] = {
        [MO_T_PLUS] = "+",    [MO_T_MINUS] = "-",
        [MO_T_STAR] = "*",    [MO_T_SLASH] = "/",
        [MO_T_PERCENT] = "%", [MO_T_EQ] = "==",
        [MO_T_NE] = "!=",     [MO_T_LT] = "<",
        [MO_T_LE] = "<=",     [MO_T_GT] = ">",
        [MO_T_GE] = ">=",     [MO_T_AND] = "and",
        [MO_T_OR] = "or",     [MO_T_IMPLIES] = "implies",
        [MO_T_IS] = "is",     [MO_T_DOTDOT] = "..",
        [MO_T_NOT] = "not",   [MO_T_TRY] = "try"};
    MO_ASSERT(t <= MO_T_PERCENT && spellings[t] != NULL);
    return spellings[t];
}

static uint32_t operator_at(struct printer *f, uint32_t start, uint32_t end, const char *word) {
    if (f->source == NULL)
        return start;
    size_t length = strlen(word);
    for (uint32_t i = start; i < end; i++) {
        if (f->source->bytes[i] == '#')
            while (i < end && f->source->bytes[i] != '\n')
                i++;
        else if (length <= end - i && memcmp(f->source->bytes + i, word, length) == 0)
            return i;
    }
    return start;
}

static void binary_step(struct printer *f, const struct mo_syntax *left,
                        const struct mo_syntax *operator_node, enum mo_syntax_token t,
                        const struct mo_syntax *right) {
    const char *word = token(t);
    unsigned space = t == MO_T_DOTDOT ? 0 : 1;
    uint32_t at = operator_node != NULL ? operator_node->span.start
                                        : operator_at(f, left->span.end, right->span.start, word);
    mo_doc_add(&f->doc, MO_DOC_SOURCE_BREAK, NULL, space, lines(f, left->span.end, at) != 0);
    if (operator_node != NULL)
        node(f, operator_node);
    else
        text(f, word);
    soft(f, space, at, right->span.start);
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
    if (a->kind != MO_S_FIELD_ACCESS)
        return false;
    for (uint32_t i = 0; i < a->count; i++)
        if (!same_target(a->children[i], b->children[i]))
            return false;
    return true;
}

static const struct mo_syntax *addition(const struct mo_syntax *n) {
    const struct mo_syntax *rhs = n->children[1];
    if (n->kind == MO_S_BIND && rhs->kind == MO_S_BINARY && rhs->token == MO_T_PLUS &&
        rhs->leading == NULL && rhs->trailing == NULL && rhs->ending == NULL &&
        same_target(n->children[0], rhs->children[0]))
        return rhs->children[1];
    return NULL;
}

static void block_body(struct printer *f, const struct mo_syntax *body,
                       const struct mo_syntax *owner) {
    trailing(f, owner, true);
    op(f, MO_DOC_LINE_IF);
    op(f, MO_DOC_NEST);
    statements(f, body, false);
    comments(f, owner->ending, false);
    op(f, MO_DOC_UNNEST);
}

static void postfix_step(struct printer *f, const struct mo_syntax *n, uint32_t before) {
    if (n->kind == MO_S_CALL) {
        sequence_brackets(f, n, n->children[n->count - 1], "(", ")", before, n->span.end);
    } else {
        const struct mo_syntax *field = n->children[n->count - 1];
        soft(f, 0, before, field->span.start);
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
            if (items->count != 0)
                op(f, MO_DOC_BLANK);
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
                soft(f, 1, n->children[i - 1]->span.end, first(n->children[i]));
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
            sequence_brackets(f, NULL, n->children[1], "{", "}", n->children[0]->span.end,
                              n->span.end);
        break;
    case MO_S_FN: {
        op(f, MO_DOC_GROUP);
        text(f, "fn ");
        node(f, n->children[0]);
        sequence_brackets(f, NULL, n->children[1], "(", ")", n->children[0]->span.end, n->span.end);
        uint32_t before = n->children[1]->span.end;
        if (n->children[2] != NULL) {
            text(f, " : ");
            node(f, n->children[2]);
            before = n->children[2]->span.end;
        }
        if (n->children[3] != NULL) {
            op(f, MO_DOC_NEST);
            soft(f, 1, before, n->children[3]->span.start);
            node(f, n->children[3]);
            op(f, MO_DOC_UNNEST);
            before = n->children[3]->span.end;
        }
        op(f, MO_DOC_END_GROUP);
        op(f, MO_DOC_LINE_IF);
        op(f, MO_DOC_NEST);
        const struct mo_syntax *spec = n->children[4], *body = n->children[5];
        for (uint32_t i = 0; i < spec->count; i++) {
            if (i != 0)
                op(f, MO_DOC_BLANK);
            node(f, spec->children[i]);
            op(f, MO_DOC_LINE_IF);
        }
        if (body->count != 0 && spec->count != 0)
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
                soft(f, 1, n->children[i - 1]->span.end, n->children[i]->span.start);
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
            sequence_brackets(f, NULL, n->children[1], "(", ")", n->children[0]->span.end,
                              n->span.end);
        block_body(f, n->children[2], n);
        text(f, "end");
        break;
    case MO_S_CASE_DECL:
        node(f, n->children[0]);
        if (n->children[1] != NULL) {
            const struct mo_syntax *fields = n->children[1];
            if (fields->kind == MO_S_SEQUENCE)
                sequence_brackets(f, NULL, fields, "(", ")", n->children[0]->span.end, n->span.end);
            else
                brackets(f, NULL, n->children + 1, 1, "(", ")", n->children[0]->span.end,
                         n->span.end);
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
        trailing(f, n, true);
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
            sequence_brackets(f, NULL, n->children[0], "(", ")", n->span.start, n->span.end);
            if (n->token == MO_T_FN && n->children[1] != NULL) {
                text(f, " : ");
                node(f, n->children[1]);
            }
        } else {
            node(f, n->children[0]);
            if (n->children[1] != NULL)
                sequence_brackets(f, NULL, n->children[1], "(", ")", n->children[0]->span.end,
                                  n->span.end);
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
        soft(f, 1, n->children[0]->span.end, rhs->span.start);
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
        bool inline_body = body->count == 1 && body->children[0]->leading == NULL &&
                           body->children[0]->kind != MO_S_FOR &&
                           body->children[0]->kind != MO_S_WHILE &&
                           (f->policy == MO_FORMAT_WIDTH ||
                            lines(f, n->children[0]->span.end, body->children[0]->span.start) == 0);
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
        sequence_brackets(f, NULL, n->children[0], "(", ")", n->span.start, n->span.end);
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
        binary_step(f, n->children[0], NULL, n->token, n->children[1]);
        op(f, MO_DOC_UNNEST);
        break;
    case MO_S_BINARY_CHAIN:
        node(f, n->children[0]);
        op(f, MO_DOC_NEST);
        for (uint32_t i = 1; i < n->count; i += 2)
            binary_step(f, n->children[i - 1], n->children[i], n->children[i]->token,
                        n->children[i + 1]);
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
        postfix_step(f, n, n->count == 2 ? n->children[0]->span.end : n->span.start);
        break;
    case MO_S_POSTFIX: {
        node(f, n->children[0]);
        op(f, MO_DOC_NEST);
        uint32_t before = n->children[0]->span.end;
        for (uint32_t i = 1; i < n->count; i++) {
            const struct mo_syntax *step = n->children[i];
            comments(f, step->leading, false);
            op(f, MO_DOC_GROUP);
            postfix_step(f, step, before);
            trailing(f, step, false);
            op(f, MO_DOC_END_GROUP);
            before = step->span.end;
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
        mo_doc_add(&f->doc, MO_DOC_TEXT, n->text, n->span.end - n->span.start, false);
        break;
    case MO_S_TEXT_INTERPOLATED:
        for (uint32_t i = 0; i < n->count; i++)
            node(f, n->children[i]);
        break;
    case MO_S_GROUP:
    case MO_S_TUPLE:
    case MO_S_LIST:
    case MO_S_MAP:
        brackets(f, n, n->children, n->count,
                 n->kind == MO_S_LIST  ? "["
                 : n->kind == MO_S_MAP ? "{"
                                       : "(",
                 n->kind == MO_S_LIST  ? "]"
                 : n->kind == MO_S_MAP ? "}"
                                       : ")",
                 n->span.start, n->span.end);
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
    comments(f, n->leading, false);
    op(f, MO_DOC_GROUP);
    print_node(f, n);
    trailing(f, n, false);
    op(f, MO_DOC_END_GROUP);
    f->depth--;
}

bool mo_format(const struct mo_module_syntax *module, const struct mo_sources *sources,
               struct mo_format_options options, struct mo_buffer *output) {
    mo_buffer_clear(output);
    if (module == NULL || module->tree == NULL || options.width == 0 ||
        options.width > MO_LIMIT_LINE_LENGTH || options.display_width == NULL)
        return false;
    struct printer f = {.policy = options.policy};
    mo_doc_init(&f.doc, options.display_width);
    if (sources != NULL && module->span.file < sources->count)
        f.source = &sources->files[module->span.file];
    if (options.policy == MO_FORMAT_SOURCE_PROTOTYPE && f.source == NULL) {
        mo_doc_free(&f.doc);
        return false;
    }
    node(&f, module->tree);
    bool ok = mo_doc_render(&f.doc, options, output);
    mo_doc_free(&f.doc);
    if (!ok)
        mo_buffer_clear(output);
    return ok;
}
