/* Recursive descent, with a recovery boundary around each source line. */
#include "base/codes.h"
#include "syntax/internal.h"

#include <setjmp.h>
#include <string.h>

struct parser {
    struct mo_lex lx;
    struct mo_token current;
    struct mo_token lookahead;
    bool has_lookahead;
    struct mo_span previous;
    jmp_buf *escape;
    uint32_t depth;
    uint32_t names;
};

struct syntax_node {
    struct mo_syntax value;
    uint32_t capacity;
    uint32_t height;
};

static _Noreturn void error(struct parser *p, uint16_t code, struct mo_span span,
                            const char *message, const char *reason) {
    bool kept = mo_syntax_error(&p->lx, code, span, message, reason);
    (void)kept;
    MO_ASSERT(p->escape != NULL);
    longjmp(*p->escape, 1);
}

static MO_MUST_USE void *allocate(struct parser *p, size_t size) {
    void *result = mo_arena_allocate(p->lx.arena, size, _Alignof(max_align_t));
    if (result == NULL) {
        bool kept = mo_syntax_error(&p->lx, MO_CODE_SYNTAX_NO_MEMORY, p->current.span,
                                    "the syntax arena has no room", NULL);
        (void)kept;
        p->lx.fatal = true;
        longjmp(*p->escape, 1);
    }
    return result;
}

static void next(struct parser *p) {
    p->previous = p->current.span;
    p->current = p->has_lookahead ? p->lookahead : mo_syntax_next(&p->lx);
    p->has_lookahead = false;
}

static MO_MUST_USE enum mo_syntax_token peek_next(struct parser *p) {
    if (!p->has_lookahead) {
        p->lookahead = mo_syntax_next(&p->lx);
        p->has_lookahead = true;
    }
    return p->lookahead.kind;
}

static MO_MUST_USE bool take(struct parser *p, enum mo_syntax_token token) {
    if (p->current.kind != token)
        return false;
    next(p);
    return true;
}

static void expect(struct parser *p, enum mo_syntax_token token) {
    struct mo_span previous = p->previous;
    if (!take(p, token))
        error(p, MO_CODE_SOURCE_SYNTAX, p->current.span,
              "a statement ends with a line break; blocks close with end",
              "Blocks open with a keyword and close with end (reference 2.4).");
    if (token == MO_T_NL)
        p->previous = previous;
}

static void enter(struct parser *p) {
    if (p->depth == MO_LIMIT_NESTING_DEPTH)
        error(p, MO_CODE_NESTED_TOO_DEEPLY, p->current.span, "code is nested too deeply", NULL);
    p->depth++;
}

static void height(struct parser *p, struct mo_syntax *n, const struct mo_syntax *child) {
    if (child == NULL)
        return;
    uint32_t h = ((const struct syntax_node *)child)->height;
    if (n->kind != MO_S_SEQUENCE && n->kind != MO_S_FILE && n->kind != MO_S_MODULE &&
        n->kind != MO_S_EXPR_STMT)
        h++;
    if (h > MO_LIMIT_NESTING_DEPTH)
        error(p, MO_CODE_NESTED_TOO_DEEPLY, n->span, "code is nested too deeply", NULL);
    struct syntax_node *storage = (struct syntax_node *)n;
    if (h > storage->height)
        storage->height = h;
}

static MO_MUST_USE struct mo_syntax *node(struct parser *p, enum mo_syntax_kind kind,
                                          struct mo_span start, uint32_t count,
                                          struct mo_syntax *const children[]) {
    struct syntax_node *storage = allocate(p, sizeof *storage);
    *storage = (struct syntax_node){.capacity = count};
    struct mo_syntax *n = &storage->value;
    *n = (struct mo_syntax){.kind = kind, .span = start, .count = count};
    if (p->previous.end > n->span.end)
        n->span.end = p->previous.end;
    if (count != 0) {
        n->children = allocate(p, count * sizeof *n->children);
        memcpy(n->children, children, count * sizeof *n->children);
    }
    for (uint32_t i = 0; i < count; i++) {
        height(p, n, children[i]);
        if (children[i] != NULL)
            n->incomplete = n->incomplete || children[i]->incomplete;
    }
    return n;
}

#define N(kind, start, ...)                                                                        \
    node(p, kind, start,                                                                           \
         (uint32_t)(sizeof((struct mo_syntax *[]){__VA_ARGS__}) / sizeof(struct mo_syntax *)),     \
         (struct mo_syntax *[]){__VA_ARGS__})

static MO_MUST_USE struct mo_syntax *sequence(struct parser *p) {
    return node(p, MO_S_SEQUENCE,
                (struct mo_span){p->lx.file, p->current.span.start, p->current.span.start}, 0,
                NULL);
}

static void append(struct parser *p, struct mo_syntax *n, struct mo_syntax *child) {
    struct syntax_node *storage = (struct syntax_node *)n;
    if (n->count == storage->capacity) {
        uint32_t capacity = storage->capacity == 0 ? 1 : storage->capacity * 2;
        struct mo_syntax **children = allocate(p, capacity * sizeof *children);
        if (n->count != 0)
            memcpy(children, n->children, n->count * sizeof *children);
        n->children = children;
        storage->capacity = capacity;
    }
    n->children[n->count++] = child;
    if (child != NULL) {
        if (n->count == 1)
            n->span.start = child->span.start;
        n->span.end = child->span.end;
    }
    height(p, n, child);
    n->incomplete = n->incomplete || (child != NULL && child->incomplete);
}

static MO_MUST_USE struct mo_syntax *leaf(struct parser *p) {
    struct mo_token token = p->current;
    enum mo_syntax_kind kind = token.kind == MO_T_NAME         ? MO_S_NAME
                               : token.kind == MO_T_CAPNAME    ? MO_S_CAPNAME
                               : token.kind == MO_T_INT        ? MO_S_INT
                               : token.kind == MO_T_DECIMAL    ? MO_S_DECIMAL
                               : token.kind == MO_T_CHAR       ? MO_S_CHAR
                               : token.kind == MO_T_TRUE       ? MO_S_TRUE
                               : token.kind == MO_T_FALSE      ? MO_S_FALSE
                               : token.kind == MO_T_UNDERSCORE ? MO_S_UNDERSCORE
                                                               : MO_S_TEXT;
    next(p);
    struct mo_syntax *n = node(p, kind, token.span, 0, NULL);
    n->span = token.span;
    n->token = token.kind;
    uint8_t *bytes = allocate(p, token.span.end - token.span.start);
    memcpy(bytes, p->lx.source->bytes + token.span.start, token.span.end - token.span.start);
    n->text = bytes;
    return n;
}

static MO_MUST_USE struct mo_syntax *name(struct parser *p, enum mo_syntax_token kind) {
    if (p->current.kind != kind)
        expect(p, kind);
    return leaf(p);
}

static MO_MUST_USE struct mo_syntax *path(struct parser *p) {
    struct mo_syntax *n = sequence(p);
    append(p, n, name(p, MO_T_CAPNAME));
    while (take(p, MO_T_DOT))
        append(p, n, name(p, MO_T_CAPNAME));
    return n;
}

static MO_MUST_USE struct mo_syntax *header(struct parser *p, struct mo_header *out) {
    struct mo_span start = p->current.span;
    if (p->current.kind != MO_T_MODULE)
        error(p, MO_CODE_SOURCE_HEADER, p->current.span,
              "a file is one module, named on its first line of code",
              "A module starts with its name, then expose and use lines (reference 10.1).");
    expect(p, MO_T_MODULE);
    struct mo_syntax *module_path = path(p);
    expect(p, MO_T_NL);
    struct mo_syntax *expose = NULL;
    if (take(p, MO_T_EXPOSE)) {
        expose = sequence(p);
        expose->kind = MO_S_EXPOSE;
        do {
            bool cap = p->current.kind == MO_T_CAPNAME;
            struct mo_syntax *item = name(p, cap ? MO_T_CAPNAME : MO_T_NAME);
            if (cap && take(p, MO_T_LPAREN)) {
                expect(p, MO_T_DOTDOT);
                expect(p, MO_T_RPAREN);
                item = N(MO_S_EXPOSE_ALL, item->span, item);
            }
            append(p, expose, item);
        } while (take(p, MO_T_COMMA));
        expect(p, MO_T_NL);
    }
    struct mo_syntax *uses = sequence(p);
    while (p->current.kind == MO_T_USE) {
        struct mo_span use_start = p->current.span;
        next(p);
        struct mo_syntax *use_path = path(p), *items = NULL;
        if (take(p, MO_T_LBRACE)) {
            items = sequence(p);
            do {
                append(p, items,
                       name(p, p->current.kind == MO_T_CAPNAME ? MO_T_CAPNAME : MO_T_NAME));
            } while (take(p, MO_T_COMMA));
            expect(p, MO_T_RBRACE);
        }
        expect(p, MO_T_NL);
        append(p, uses, N(MO_S_USE, use_start, use_path, items));
    }
    if (uses->count == 0)
        uses->span = (struct mo_span){p->lx.file, p->previous.end, p->previous.end};
    struct mo_syntax *n = N(MO_S_MODULE, start, module_path, expose, uses);
    *out = (struct mo_header){n->span, module_path, expose, uses};
    return n;
}

static MO_MUST_USE struct mo_syntax *expr(struct parser *p, unsigned minimum);
static MO_MUST_USE struct mo_syntax *statement(struct parser *p, bool newline);
static MO_MUST_USE struct mo_syntax *statements(struct parser *p);
static void validate(struct parser *p, const struct mo_syntax *n, bool pattern);
enum item_context {
    MODULE_ITEM,
    FRAGMENT_ITEM,
    STATEMENT_ITEM,
    CASE_ITEM,
    FIELD_ITEM,
    CASE_DECL_ITEM,
    SPEC_ITEM,
    EXPR_ITEM,
    ANCHOR_ITEM
};
static MO_MUST_USE struct mo_syntax *read_item(struct parser *p, enum item_context context);
static void recover(struct parser *p, uint32_t first_line);

static void limit(struct parser *p, const struct mo_syntax *n, uint32_t maximum, uint16_t code,
                  const char *message) {
    if (n->count > maximum)
        error(p, code, n->span, message, NULL);
}

static MO_MUST_USE struct mo_syntax *type(struct parser *p) {
    enter(p);
    struct mo_span start = p->current.span;
    struct mo_syntax *n = NULL;
    if (take(p, MO_T_FN)) {
        expect(p, MO_T_LPAREN);
        struct mo_syntax *args = sequence(p);
        if (p->current.kind != MO_T_RPAREN) {
            do {
                append(p, args, type(p));
            } while (take(p, MO_T_COMMA));
        }
        expect(p, MO_T_RPAREN);
        limit(p, args, MO_LIMIT_PARAMETERS, MO_CODE_TOO_MANY_PARAMETERS,
              "a function type has too many parameters");
        struct mo_syntax *result = take(p, MO_T_COLON) ? type(p) : NULL;
        n = N(MO_S_TYPE, start, args, result);
        n->token = MO_T_FN;
    } else if (take(p, MO_T_LPAREN)) {
        struct mo_syntax *args = sequence(p);
        append(p, args, type(p));
        expect(p, MO_T_COMMA);
        do {
            append(p, args, type(p));
        } while (take(p, MO_T_COMMA));
        expect(p, MO_T_RPAREN);
        limit(p, args, MO_LIMIT_FIELDS, MO_CODE_TOO_MANY_FIELDS,
              "a tuple type has too many elements");
        n = N(MO_S_TYPE, start, args);
        n->token = MO_T_LPAREN;
    } else {
        struct mo_syntax *base = name(p, MO_T_CAPNAME);
        if (take(p, MO_T_DOT))
            base = N(MO_S_QUALIFIED, start, base, name(p, MO_T_CAPNAME));
        struct mo_syntax *args = NULL;
        if (take(p, MO_T_LPAREN)) {
            args = sequence(p);
            do {
                append(p, args, type(p));
            } while (take(p, MO_T_COMMA));
            expect(p, MO_T_RPAREN);
        }
        n = N(MO_S_TYPE, start, base, args);
    }
    p->depth--;
    return n;
}

static MO_MUST_USE struct mo_syntax *type_parameters(struct parser *p) {
    if (!take(p, MO_T_LPAREN))
        return NULL;
    struct mo_syntax *args = sequence(p);
    do {
        append(p, args, name(p, MO_T_CAPNAME));
    } while (take(p, MO_T_COMMA));
    expect(p, MO_T_RPAREN);
    return args;
}

static MO_MUST_USE struct mo_syntax *text(struct parser *p) {
    if (p->current.kind == MO_T_TEXT)
        return leaf(p);
    if (p->current.kind != MO_T_TEXT_HEAD)
        expect(p, MO_T_TEXT);
    enter(p);
    struct mo_span start = p->current.span;
    struct mo_syntax *n = sequence(p);
    n->kind = MO_S_TEXT_INTERPOLATED;
    append(p, n, leaf(p));
    for (;;) {
        append(p, n, expr(p, 0));
        bool tail = p->current.kind == MO_T_TEXT_TAIL;
        if (!tail && p->current.kind != MO_T_TEXT_MID)
            expect(p, MO_T_TEXT_TAIL);
        append(p, n, leaf(p));
        if (tail)
            break;
    }
    n->span.start = start.start;
    p->depth--;
    return n;
}

static MO_MUST_USE struct mo_syntax *line_if_tail(struct parser *p, struct mo_span start,
                                                  struct mo_syntax *condition);

static MO_MUST_USE struct mo_syntax *if_expr(struct parser *p, bool block) {
    enter(p);
    struct mo_span start = p->current.span;
    expect(p, MO_T_IF);
    struct mo_syntax *condition = expr(p, 1), *body = NULL, *otherwise = NULL;
    enum mo_syntax_kind kind = MO_S_LINE_IF;
    if (p->current.kind == MO_T_COLON) {
        struct mo_syntax *n = line_if_tail(p, start, condition);
        p->depth--;
        return n;
    } else {
        if (!block)
            expect(p, MO_T_COLON);
        kind = MO_S_IF;
        expect(p, MO_T_NL);
        body = statements(p);
        struct mo_syntax *parts = sequence(p);
        while (take(p, MO_T_ELSE)) {
            struct mo_span else_start = p->previous;
            struct mo_syntax *part;
            if (take(p, MO_T_IF)) {
                struct mo_syntax *c = expr(p, 1);
                expect(p, MO_T_NL);
                struct mo_syntax *b = statements(p);
                part = N(MO_S_IF, else_start, c, b, NULL);
            } else {
                expect(p, MO_T_NL);
                part = N(MO_S_ELSE, else_start, statements(p));
            }
            append(p, parts, part);
            if (part->kind == MO_S_ELSE)
                break;
        }
        expect(p, MO_T_END);
        for (uint32_t i = parts->count; i > 0; i--) {
            struct mo_syntax *part = parts->children[i - 1];
            if (part->kind == MO_S_IF) {
                part->children[2] = otherwise;
                height(p, part, otherwise);
                part->incomplete = part->incomplete || (otherwise != NULL && otherwise->incomplete);
            }
            part->span.end = p->previous.end;
            otherwise = part;
        }
    }
    struct mo_syntax *n = N(kind, start, condition, body, otherwise);
    p->depth--;
    return n;
}

static MO_MUST_USE struct mo_syntax *line_if_tail(struct parser *p, struct mo_span start,
                                                  struct mo_syntax *condition) {
    expect(p, MO_T_COLON);
    struct mo_syntax *body = expr(p, 1);
    expect(p, MO_T_ELSE);
    struct mo_syntax *otherwise = take(p, MO_T_COLON) ? expr(p, 0) : if_expr(p, false);
    return N(MO_S_LINE_IF, start, condition, body, otherwise);
}

static MO_MUST_USE struct mo_syntax *case_expr(struct parser *p) {
    enter(p);
    struct mo_span start = p->current.span;
    expect(p, MO_T_CASE);
    struct mo_syntax *value = expr(p, 0);
    expect(p, MO_T_NL);
    struct mo_syntax *lines = sequence(p);
    struct mo_syntax *arm = NULL;
    while (p->current.kind != MO_T_END && p->current.kind != MO_T_EOF) {
        struct mo_syntax *line = read_item(p, CASE_ITEM);
        if (line == NULL) {
            lines->incomplete = true;
            if (arm != NULL) {
                arm->incomplete = true;
                arm->children[2]->incomplete = true;
            }
        } else if (line->kind == MO_S_ARM) {
            if (arm != NULL)
                arm->span.end = line->span.start;
            struct mo_syntax *body = sequence(p);
            if (line->children[2] != NULL)
                append(p, body, line->children[2]);
            line->children[2] = body;
            arm = line;
            append(p, lines, line);
        } else if (arm != NULL) {
            append(p, arm->children[2], line);
            height(p, arm, arm->children[2]);
        } else
            append(p, lines, line);
    }
    if (arm != NULL)
        arm->span.end = p->current.span.start;
    if (lines->count > 0) {
        lines->span.end = lines->children[lines->count - 1]->span.end;
        for (uint32_t i = 0; i < lines->count; i++)
            height(p, lines, lines->children[i]);
    }
    expect(p, MO_T_END);
    struct mo_syntax *n = N(MO_S_CASE, start, value, lines);
    p->depth--;
    return n;
}

static MO_MUST_USE struct mo_syntax *lambda(struct parser *p) {
    enter(p);
    struct mo_span start = p->current.span;
    expect(p, MO_T_FN);
    expect(p, MO_T_LPAREN);
    struct mo_syntax *params = sequence(p);
    if (p->current.kind != MO_T_RPAREN) {
        do {
            append(p, params,
                   name(p, p->current.kind == MO_T_UNDERSCORE ? MO_T_UNDERSCORE : MO_T_NAME));
        } while (take(p, MO_T_COMMA));
    }
    expect(p, MO_T_RPAREN);
    struct mo_syntax *body = take(p, MO_T_NL) ? statements(p) : expr(p, 0);
    expect(p, MO_T_END);
    limit(p, params, MO_LIMIT_PARAMETERS, MO_CODE_TOO_MANY_PARAMETERS,
          "a function has too many parameters");
    struct mo_syntax *n = N(MO_S_LAMBDA, start, params, body);
    p->depth--;
    return n;
}

static MO_MUST_USE struct mo_syntax *primary(struct parser *p) {
    enum mo_syntax_token token = p->current.kind;
    struct mo_span start = p->current.span;
    if (token == MO_T_INT || token == MO_T_DECIMAL || token == MO_T_CHAR || token == MO_T_NAME ||
        token == MO_T_CAPNAME || token == MO_T_UNDERSCORE || token == MO_T_TRUE ||
        token == MO_T_FALSE)
        return leaf(p);
    if (token == MO_T_TEXT || token == MO_T_TEXT_HEAD)
        return text(p);
    if (token == MO_T_CASE)
        return case_expr(p);
    if (token == MO_T_FN)
        return lambda(p);
    if (take(p, MO_T_STOP))
        return N(MO_S_STOP, start, text(p));
    if (token == MO_T_LPAREN || token == MO_T_LBRACK || token == MO_T_LBRACE) {
        enter(p);
        next(p);
        struct mo_syntax *n = sequence(p);
        n->kind = token == MO_T_LPAREN ? MO_S_GROUP : token == MO_T_LBRACK ? MO_S_LIST : MO_S_MAP;
        enum mo_syntax_token close = token == MO_T_LPAREN   ? MO_T_RPAREN
                                     : token == MO_T_LBRACK ? MO_T_RBRACK
                                                            : MO_T_RBRACE;
        if (p->current.kind != close || token == MO_T_LPAREN) {
            do {
                struct mo_syntax *item;
                struct mo_span item_start = p->current.span;
                if (token == MO_T_LBRACK && take(p, MO_T_DOTDOT)) {
                    item = N(
                        MO_S_REST, item_start,
                        name(p, p->current.kind == MO_T_UNDERSCORE ? MO_T_UNDERSCORE : MO_T_NAME));
                } else {
                    item = expr(p, token == MO_T_LBRACE ? 1 : 0);
                    if (token == MO_T_LBRACE) {
                        expect(p, MO_T_COLON);
                        item = N(MO_S_MAP_ENTRY, item_start, item, expr(p, 0));
                    }
                }
                append(p, n, item);
                if (!take(p, MO_T_COMMA))
                    break;
                if (token == MO_T_LPAREN)
                    n->kind = MO_S_TUPLE;
                if (p->current.kind == close) {
                    if (token == MO_T_LPAREN && n->count < 2)
                        expect(p, MO_T_NAME);
                    break;
                }
            } while (true);
        }
        expect(p, close);
        if (n->kind == MO_S_TUPLE)
            limit(p, n, MO_LIMIT_FIELDS, MO_CODE_TOO_MANY_FIELDS, "a tuple has too many elements");
        n->span.start = start.start;
        n->span.end = p->previous.end;
        p->depth--;
        return n;
    }
    error(p, MO_CODE_SOURCE_SYNTAX, p->current.span, "a value is an expression",
          "A body is a sequence of statements, and its value is the value of its last line "
          "(reference 2.4).");
}

static MO_MUST_USE struct mo_syntax *postfix(struct parser *p) {
    struct mo_syntax *n = primary(p);
    while (p->current.kind == MO_T_DOT || p->current.kind == MO_T_LPAREN) {
        struct mo_span start = n->span;
        if (take(p, MO_T_DOT)) {
            enum mo_syntax_token token = p->current.kind;
            struct mo_syntax *field = name(p, token == MO_T_CAPNAME ? MO_T_CAPNAME
                                              : token == MO_T_INT   ? MO_T_INT
                                                                    : MO_T_NAME);
            n = N(token == MO_T_CAPNAME ? MO_S_QUALIFIED
                  : token == MO_T_INT   ? MO_S_POSITION
                                        : MO_S_FIELD_ACCESS,
                  start, n, field);
        } else {
            enter(p);
            expect(p, MO_T_LPAREN);
            struct mo_syntax *args = sequence(p);
            if (p->current.kind != MO_T_RPAREN) {
                do {
                    struct mo_syntax *arg = expr(p, 0);
                    if (arg->kind == MO_S_NAME && take(p, MO_T_COLON))
                        arg = N(MO_S_NAMED_ARG, arg->span, arg, expr(p, 0));
                    append(p, args, arg);
                    if (!take(p, MO_T_COMMA) || p->current.kind == MO_T_RPAREN)
                        break;
                } while (true);
            }
            expect(p, MO_T_RPAREN);
            bool constructor = n->kind == MO_S_CAPNAME ||
                               (n->kind == MO_S_QUALIFIED && n->children[1]->kind == MO_S_CAPNAME);
            limit(p, args, constructor ? MO_LIMIT_FIELDS : MO_LIMIT_PARAMETERS,
                  constructor ? MO_CODE_TOO_MANY_FIELDS : MO_CODE_TOO_MANY_PARAMETERS,
                  constructor ? "a record or case has too many fields"
                              : "a call has too many arguments");
            n = N(MO_S_CALL, start, n, args);
            p->depth--;
        }
    }
    return n;
}

static MO_MUST_USE unsigned precedence(enum mo_syntax_token token) {
    if (token == MO_T_IMPLIES)
        return 2;
    if (token == MO_T_OR)
        return 3;
    if (token == MO_T_AND)
        return 4;
    if (token == MO_T_EQ || token == MO_T_NE || token == MO_T_LT || token == MO_T_LE ||
        token == MO_T_GT || token == MO_T_GE || token == MO_T_IS)
        return 6;
    if (token == MO_T_DOTDOT)
        return 7;
    if (token == MO_T_PLUS || token == MO_T_MINUS)
        return 8;
    if (token == MO_T_STAR || token == MO_T_SLASH || token == MO_T_PERCENT)
        return 9;
    return 0;
}

static MO_MUST_USE struct mo_syntax *expr(struct parser *p, unsigned minimum) {
    struct mo_span start = p->current.span;
    enum mo_syntax_token token = p->current.kind;
    if (token == MO_T_IF && minimum == 0)
        return if_expr(p, false);
    struct mo_syntax *n;
    if (token == MO_T_MINUS || token == MO_T_TRY || (token == MO_T_NOT && minimum <= 5)) {
        enter(p);
        next(p);
        n = N(token == MO_T_MINUS ? MO_S_NEGATE
              : token == MO_T_TRY ? MO_S_TRY
                                  : MO_S_NOT,
              start, expr(p, token == MO_T_NOT ? 5 : 10));
        p->depth--;
    } else
        n = postfix(p);
    unsigned nonchain = 0;
    for (;;) {
        token = p->current.kind;
        unsigned level = precedence(token);
        if (level == 0 || level < minimum || (nonchain != 0 && level >= nonchain))
            break;
        next(p);
        struct mo_syntax *rhs = expr(p, token == MO_T_IS ? 10 : level + 1);
        n = N(token == MO_T_IS       ? MO_S_IS
              : token == MO_T_DOTDOT ? MO_S_RANGE
                                     : MO_S_BINARY,
              start, n, rhs);
        n->token = token;
        if (level == 2 || level == 6 || level == 7)
            nonchain = level;
    }
    return n;
}

static MO_MUST_USE struct mo_syntax *binder(struct parser *p) {
    struct mo_span start = p->current.span;
    if (!take(p, MO_T_LPAREN))
        return N(MO_S_BINDER, start,
                 name(p, p->current.kind == MO_T_UNDERSCORE ? MO_T_UNDERSCORE : MO_T_NAME));
    enter(p);
    struct mo_syntax *n = sequence(p);
    n->kind = MO_S_TUPLE;
    append(p, n, binder(p));
    expect(p, MO_T_COMMA);
    do {
        append(p, n, binder(p));
    } while (take(p, MO_T_COMMA));
    expect(p, MO_T_RPAREN);
    n->span.start = start.start;
    n->span.end = p->previous.end;
    p->depth--;
    return n;
}

static MO_MUST_USE struct mo_syntax *statement(struct parser *p, bool newline) {
    struct mo_span start = p->current.span;
    enum mo_syntax_token token = p->current.kind;
    struct mo_syntax *n;
    if (token == MO_T_FOR || token == MO_T_WHILE) {
        enter(p);
        next(p);
        struct mo_syntax *b = token == MO_T_FOR ? binder(p) : NULL;
        if (b != NULL)
            expect(p, MO_T_IN);
        struct mo_syntax *condition = expr(p, token == MO_T_FOR ? 0 : 1);
        expect(p, MO_T_NL);
        struct mo_syntax *body = statements(p);
        expect(p, MO_T_END);
        n = b == NULL ? N(MO_S_WHILE, start, condition, body)
                      : N(MO_S_FOR, start, b, condition, body);
        p->depth--;
    } else if (token == MO_T_VAR) {
        next(p);
        struct mo_syntax *v = name(p, MO_T_NAME);
        expect(p, MO_T_ASSIGN);
        struct mo_syntax *value = p->current.kind == MO_T_IF ? if_expr(p, true) : expr(p, 0);
        n = N(MO_S_VAR, start, v, value);
    } else if (token == MO_T_RETURN || token == MO_T_BREAK || token == MO_T_CONTINUE) {
        next(p);
        struct mo_syntax *value = NULL;
        if (token == MO_T_RETURN && p->current.kind != MO_T_NL && p->current.kind != MO_T_IF)
            value = expr(p, 0);
        struct mo_syntax *guard = NULL;
        if (p->current.kind == MO_T_IF) {
            struct mo_span if_start = p->current.span;
            next(p);
            guard = expr(p, 1);
            if (token == MO_T_RETURN && value == NULL && p->current.kind == MO_T_COLON) {
                enter(p);
                value = line_if_tail(p, if_start, guard);
                p->depth--;
                guard = take(p, MO_T_IF) ? expr(p, 1) : NULL;
            }
        }
        n = token == MO_T_RETURN
                ? N(MO_S_RETURN, start, value, guard)
                : N(token == MO_T_BREAK ? MO_S_BREAK : MO_S_CONTINUE, start, guard);
    } else if (take(p, MO_T_ASSERT))
        n = N(MO_S_ASSERT, start, expr(p, 0));
    else {
        struct mo_syntax *lhs = p->current.kind == MO_T_IF ? if_expr(p, true) : expr(p, 0);
        token = p->current.kind;
        if (token == MO_T_ASSIGN || token == MO_T_PLUSEQ || token == MO_T_MINUSEQ) {
            next(p);
            struct mo_syntax *rhs =
                token == MO_T_ASSIGN && p->current.kind == MO_T_IF ? if_expr(p, true) : expr(p, 0);
            n = N(token == MO_T_ASSIGN   ? MO_S_BIND
                  : token == MO_T_PLUSEQ ? MO_S_ADD_ASSIGN
                                         : MO_S_SUB_ASSIGN,
                  start, lhs, rhs);
        } else
            n = N(MO_S_EXPR_STMT, start, lhs);
    }
    if (newline)
        expect(p, MO_T_NL);
    n->span.end = p->previous.end;
    return n;
}

static MO_MUST_USE struct mo_syntax *statements(struct parser *p) {
    struct mo_syntax *n = sequence(p);
    while (p->current.kind != MO_T_END && p->current.kind != MO_T_ELSE &&
           p->current.kind != MO_T_EOF) {
        if (p->current.kind == MO_T_STRUCT || p->current.kind == MO_T_ENUM ||
            p->current.kind == MO_T_TEST ||
            (p->current.kind == MO_T_FN && peek_next(p) == MO_T_NAME))
            break;
        struct mo_syntax *item = read_item(p, STATEMENT_ITEM);
        if (item != NULL)
            append(p, n, item);
        else
            n->incomplete = true;
    }
    return n;
}

static MO_MUST_USE bool declaration_header(struct parser *p, enum mo_syntax_token token,
                                           struct mo_syntax **parts) {
    jmp_buf escape;
    jmp_buf *saved = p->escape;
    uint32_t depth = p->depth, first_line = p->current.line;
    p->escape = &escape;
    if (setjmp(escape) != 0) {
        p->depth = depth;
        p->escape = saved;
        if (p->lx.fatal)
            longjmp(*saved, 1);
        recover(p, first_line);
        if (p->lx.fatal)
            longjmp(*saved, 1);
        return false;
    }
    next(p);
    if (token == MO_T_FN) {
        parts[0] = name(p, MO_T_NAME);
        expect(p, MO_T_LPAREN);
        parts[1] = sequence(p);
        if (p->current.kind != MO_T_RPAREN) {
            do {
                struct mo_syntax *v = name(p, MO_T_NAME);
                expect(p, MO_T_COLON);
                append(p, parts[1], N(MO_S_PARAM, v->span, v, type(p)));
                if (!take(p, MO_T_COMMA) || p->current.kind == MO_T_RPAREN)
                    break;
            } while (true);
        }
        expect(p, MO_T_RPAREN);
        parts[2] = take(p, MO_T_COLON) ? type(p) : NULL;
        if (take(p, MO_T_WHERE)) {
            parts[3] = sequence(p);
            parts[3]->kind = MO_S_WHERE;
            do {
                struct mo_syntax *v = name(p, MO_T_CAPNAME);
                expect(p, MO_T_COLON);
                append(p, parts[3], N(MO_S_SEQUENCE, v->span, v, name(p, MO_T_CAPNAME)));
            } while (take(p, MO_T_COMMA));
        }
    } else if (token == MO_T_STRUCT || token == MO_T_ENUM) {
        parts[0] = name(p, MO_T_CAPNAME);
        parts[1] = type_parameters(p);
    } else if (token == MO_T_TEST || token == MO_T_PROMISE)
        parts[0] = name(p, MO_T_TEXT);
    expect(p, MO_T_NL);
    p->escape = saved;
    return true;
}

static MO_MUST_USE struct mo_syntax *anchor(struct parser *p) {
    enum mo_syntax_token token = p->current.kind;
    struct mo_span start = p->current.span;
    next(p);
    struct mo_syntax *values = sequence(p);
    do {
        struct mo_syntax *v = name(p, MO_T_NAME);
        expect(p, MO_T_COLON);
        append(p, values, N(MO_S_NAMED_ARG, v->span, v, expr(p, 0)));
    } while (take(p, MO_T_COMMA));
    expect(p, MO_T_NL);
    struct mo_syntax *n = N(MO_S_ANCHOR, start, values);
    n->token = token;
    return n;
}

static MO_MUST_USE struct mo_syntax *specification_item(struct parser *p) {
    bool promise = p->current.kind == MO_T_PROMISE;
    struct mo_span start = p->current.span;
    struct mo_syntax *parts[4] = {NULL};
    bool valid = declaration_header(p, p->current.kind, parts);
    struct mo_syntax *lines = sequence(p);
    while (p->current.kind != MO_T_END && p->current.kind != MO_T_ALLOWS &&
           p->current.kind != MO_T_FORBIDS && p->current.kind != MO_T_EOF) {
        struct mo_syntax *line = read_item(p, EXPR_ITEM);
        if (line != NULL)
            append(p, lines, line);
        else
            lines->incomplete = true;
    }
    struct mo_syntax *anchors = sequence(p);
    if (promise) {
        while (p->current.kind == MO_T_ALLOWS || p->current.kind == MO_T_FORBIDS) {
            struct mo_syntax *a = read_item(p, ANCHOR_ITEM);
            if (a != NULL)
                append(p, anchors, a);
            else
                anchors->incomplete = true;
        }
    }
    if (valid || p->current.kind != MO_T_EOF) {
        expect(p, MO_T_END);
        expect(p, MO_T_NL);
    } else
        p->previous = p->current.span;
    struct mo_syntax *n =
        promise ? N(MO_S_PROMISE, start, parts[0], lines, anchors) : N(MO_S_EXAMPLES, start, lines);
    n->incomplete = n->incomplete || !valid;
    return n;
}

static MO_MUST_USE struct mo_syntax *specification(struct parser *p) {
    struct mo_syntax *spec = sequence(p);
    while (p->current.kind == MO_T_PROMISE || p->current.kind == MO_T_EXAMPLES) {
        struct mo_syntax *n = read_item(p, SPEC_ITEM);
        if (n != NULL)
            append(p, spec, n);
        else
            spec->incomplete = true;
    }
    return spec;
}

static MO_MUST_USE struct mo_syntax *member(struct parser *p, bool record) {
    if (record) {
        struct mo_syntax *field = name(p, MO_T_NAME);
        expect(p, MO_T_COLON);
        struct mo_syntax *n = N(MO_S_FIELD, field->span, field, type(p));
        expect(p, MO_T_NL);
        return n;
    }
    struct mo_syntax *id = name(p, MO_T_CAPNAME), *fields = NULL;
    if (take(p, MO_T_LPAREN)) {
        if (p->current.kind == MO_T_NAME) {
            fields = sequence(p);
            do {
                struct mo_syntax *field = name(p, MO_T_NAME);
                expect(p, MO_T_COLON);
                append(p, fields, N(MO_S_FIELD, field->span, field, type(p)));
                if (!take(p, MO_T_COMMA) || p->current.kind == MO_T_RPAREN)
                    break;
            } while (true);
            limit(p, fields, MO_LIMIT_FIELDS, MO_CODE_TOO_MANY_FIELDS,
                  "a case has too many fields");
        } else
            fields = type(p);
        expect(p, MO_T_RPAREN);
    }
    struct mo_syntax *n = N(MO_S_CASE_DECL, id->span, id, fields);
    expect(p, MO_T_NL);
    return n;
}

static MO_MUST_USE struct mo_syntax *declaration(struct parser *p, bool fragment) {
    struct mo_span start = p->current.span;
    enum mo_syntax_token token = p->current.kind;
    if (fragment && token == MO_T_FN && peek_next(p) == MO_T_LPAREN)
        return statement(p, true);
    struct mo_syntax *n;
    struct mo_syntax *parts[4] = {NULL};
    bool valid = true;
    if (token == MO_T_STRUCT || token == MO_T_ENUM) {
        valid = declaration_header(p, token, parts);
        struct mo_syntax *members = sequence(p);
        while (p->current.kind != MO_T_END && p->current.kind != MO_T_EOF) {
            struct mo_syntax *m = read_item(p, token == MO_T_STRUCT ? FIELD_ITEM : CASE_DECL_ITEM);
            if (m != NULL)
                append(p, members, m);
            else
                members->incomplete = true;
        }
        if (valid || p->current.kind != MO_T_EOF) {
            expect(p, MO_T_END);
            expect(p, MO_T_NL);
        } else
            p->previous = p->current.span;
        limit(p, members, token == MO_T_STRUCT ? MO_LIMIT_FIELDS : MO_LIMIT_CHOICE_CASES,
              token == MO_T_STRUCT ? MO_CODE_TOO_MANY_FIELDS : MO_CODE_TOO_MANY_CASES,
              token == MO_T_STRUCT ? "a record has too many fields"
                                   : "a choice type has too many cases");
        n = N(token == MO_T_STRUCT ? MO_S_STRUCT : MO_S_ENUM, start, parts[0], parts[1], members);
    } else if (token == MO_T_FN) {
        valid = declaration_header(p, token, parts);
        struct mo_syntax *spec = specification(p), *body = statements(p);
        if (valid || p->current.kind != MO_T_EOF) {
            expect(p, MO_T_END);
            expect(p, MO_T_NL);
        } else
            p->previous = p->current.span;
        if (parts[1] != NULL)
            limit(p, parts[1], MO_LIMIT_PARAMETERS, MO_CODE_TOO_MANY_PARAMETERS,
                  "a function has too many parameters");
        n = N(MO_S_FN, start, parts[0], parts[1], parts[2], parts[3], spec, body);
    } else if (token == MO_T_TEST) {
        valid = declaration_header(p, token, parts);
        struct mo_syntax *body = statements(p);
        if (valid || p->current.kind != MO_T_EOF) {
            expect(p, MO_T_END);
            expect(p, MO_T_NL);
        } else
            p->previous = p->current.span;
        n = N(MO_S_TEST, start, parts[0], body);
    } else if (!fragment) {
        struct mo_syntax *id = name(p, MO_T_NAME);
        expect(p, MO_T_ASSIGN);
        struct mo_syntax *value = expr(p, 0);
        expect(p, MO_T_NL);
        n = N(MO_S_CONST, start, id, value);
    } else
        n = statement(p, true);
    n->incomplete = n->incomplete || !valid;
    return n;
}

static MO_MUST_USE bool constructor(const struct mo_syntax *n) {
    return n->kind == MO_S_CAPNAME ||
           (n->kind == MO_S_QUALIFIED && n->children[1]->kind == MO_S_CAPNAME);
}

static MO_MUST_USE bool place(const struct mo_syntax *n) {
    return n->kind == MO_S_NAME || (n->kind == MO_S_FIELD_ACCESS && place(n->children[0]));
}

static MO_MUST_USE bool tuple_names(const struct mo_syntax *n) {
    if (n->kind != MO_S_TUPLE)
        return false;
    for (uint32_t i = 0; i < n->count; i++) {
        const struct mo_syntax *child = n->children[i];
        if (child->kind != MO_S_NAME && child->kind != MO_S_UNDERSCORE && !tuple_names(child))
            return false;
    }
    return true;
}

static void validate(struct parser *p, const struct mo_syntax *n, bool pattern) {
    if (n == NULL || n->incomplete)
        return;
    enum mo_syntax_kind kind = n->kind;
    if (pattern) {
        if (kind == MO_S_NAME || kind == MO_S_CAPNAME || kind == MO_S_UNDERSCORE ||
            kind == MO_S_INT || kind == MO_S_DECIMAL || kind == MO_S_TEXT || kind == MO_S_CHAR ||
            kind == MO_S_TRUE || kind == MO_S_FALSE || (kind == MO_S_QUALIFIED && constructor(n)) ||
            (kind == MO_S_NEGATE &&
             (n->children[0]->kind == MO_S_INT || n->children[0]->kind == MO_S_DECIMAL)))
            return;
        if (kind == MO_S_GROUP || kind == MO_S_TUPLE || kind == MO_S_LIST ||
            kind == MO_S_SEQUENCE) {
            for (uint32_t i = 0; i < n->count; i++) {
                const struct mo_syntax *child = n->children[i];
                if (kind == MO_S_LIST && child->kind == MO_S_REST && i + 1 == n->count)
                    continue;
                validate(p, child, true);
            }
            return;
        }
        if (kind != MO_S_CALL || !constructor(n->children[0]))
            error(p, MO_CODE_SOURCE_PATTERN, n->span,
                  "a pattern is a name, _, a literal, a case, a tuple or a list",
                  "A pattern matches the forms listed in reference 7.3.");
    }
    if (kind == MO_S_CALL) {
        const struct mo_syntax *args = n->children[1];
        uint32_t named = 0;
        for (uint32_t i = 0; i < args->count; i++)
            named += args->children[i]->kind == MO_S_NAMED_ARG ? 1u : 0u;
        if (constructor(n->children[0])) {
            if (args->count == 0 || (named != args->count && !(named == 0 && args->count == 1)))
                error(p, pattern ? MO_CODE_SOURCE_PATTERN : MO_CODE_SOURCE_CONSTRUCTION, n->span,
                      "a case carries no data, one unnamed value, or named fields",
                      pattern
                          ? "A case with named fields is matched by naming them (reference 7.3)."
                          : "A case is built like a record, by naming its fields, or from its one "
                            "unnamed value (reference 4.2).");
        } else if (named != 0)
            error(p, MO_CODE_SOURCE_ARGUMENTS, n->span, "arguments are passed by position",
                  "Only records and cases are built with named fields (reference 5.2).");
        if (!pattern)
            validate(p, n->children[0], false);
        for (uint32_t i = 0; i < args->count; i++) {
            const struct mo_syntax *arg = args->children[i];
            validate(p, arg->kind == MO_S_NAMED_ARG ? arg->children[1] : arg, pattern);
        }
        return;
    }
    if (kind == MO_S_UNDERSCORE || kind == MO_S_REST)
        error(
            p, MO_CODE_SOURCE_PATTERN, n->span, "_ means ignored; ..rest ends a list pattern",
            "Patterns use _ to ignore a value, and ..rest to end a list pattern (reference 7.3).");
    if (kind == MO_S_TYPE || kind == MO_S_PARAM || kind == MO_S_FIELD || kind == MO_S_WHERE ||
        kind == MO_S_EXPOSE || kind == MO_S_EXPOSE_ALL || kind == MO_S_USE || kind == MO_S_BINDER)
        return;
    if (kind == MO_S_STRUCT || kind == MO_S_ENUM) {
        if (n->children[2]->count == 0)
            error(p, kind == MO_S_STRUCT ? MO_CODE_SOURCE_STRUCT : MO_CODE_SOURCE_ENUM, n->span,
                  kind == MO_S_STRUCT ? "a record has named fields, each with a type"
                                      : "a choice type has cases",
                  kind == MO_S_STRUCT ? "A struct declares a record type: named fields, each with "
                                        "a type (reference 4.1)."
                                      : "An enum declares a choice type: each value is exactly one "
                                        "of its cases (reference 4.2).");
        return;
    }
    if (kind == MO_S_PROMISE) {
        if (n->children[1]->count == 0)
            error(p, MO_CODE_SOURCE_PROMISE, n->span, "a promise is one or more Boolean lines",
                  "A promise's body is one or more Boolean lines (reference 12.2).");
        const struct mo_syntax *anchors = n->children[2];
        bool allows = false, forbids = false;
        for (uint32_t i = 0; i < anchors->count; i++) {
            allows = allows || anchors->children[i]->token == MO_T_ALLOWS;
            forbids = forbids || anchors->children[i]->token == MO_T_FORBIDS;
        }
        if (!allows || !forbids)
            error(p, MO_CODE_SOURCE_PROMISE, n->span,
                  "a promise has at least one allows line and one forbids line",
                  "Anchors give every parameter and result a value which the body must accept or "
                  "reject (reference 12.2).");
    }
    if (kind == MO_S_EXAMPLES && n->children[0]->count == 0)
        error(p, MO_CODE_SOURCE_SYNTAX, n->span, "an example is a Boolean line",
              "An example is a Boolean line that must be true (reference 2.4).");
    if (kind == MO_S_BIND || kind == MO_S_ADD_ASSIGN || kind == MO_S_SUB_ASSIGN) {
        if (!place(n->children[0]) && !(kind == MO_S_BIND && tuple_names(n->children[0])))
            error(p, MO_CODE_SOURCE_TARGET, n->children[0]->span,
                  "assignment changes a var or its fields",
                  "A var declares a name that can be assigned again; assigning to a field builds a "
                  "new record (reference 6.2).");
        validate(p, n->children[1], false);
        return;
    }
    if (kind == MO_S_IS) {
        validate(p, n->children[0], false);
        validate(p, n->children[1], true);
        return;
    }
    if (kind == MO_S_CASE &&
        (n->children[1]->count == 0 || n->children[1]->children[0]->kind != MO_S_ARM))
        error(p, MO_CODE_SOURCE_CASE, n->span, "an arm is Pattern: statement",
              "Case compares a value with patterns, top to bottom (reference 7.2).");
    if (kind == MO_S_FOR)
        validate(p, n->children[0], false);
    if (kind == MO_S_TUPLE)
        limit(p, n, MO_LIMIT_FIELDS, MO_CODE_TOO_MANY_FIELDS, "a tuple has too many elements");
    uint32_t from = kind == MO_S_FN ? 4
                    : kind == MO_S_PROMISE || kind == MO_S_TEST || kind == MO_S_CONST ||
                            kind == MO_S_VAR || kind == MO_S_FOR || kind == MO_S_LAMBDA ||
                            kind == MO_S_NAMED_ARG
                        ? 1
                        : 0;
    if (kind == MO_S_FIELD_ACCESS || kind == MO_S_QUALIFIED || kind == MO_S_POSITION) {
        validate(p, n->children[0], false);
        return;
    }
    if (kind == MO_S_ARM) {
        validate(p, n->children[0], true);
        from = 1;
    }
    for (uint32_t i = from; i < n->count; i++)
        validate(p, n->children[i], false);
}

static MO_MUST_USE bool line_start(const struct parser *p) {
    uint32_t at = p->current.span.start;
    while (at > 0 && (p->lx.source->bytes[at - 1] == ' ' || p->lx.source->bytes[at - 1] == '\t'))
        at--;
    return at == 0 || p->lx.source->bytes[at - 1] == '\n';
}

static MO_MUST_USE bool can_start(enum mo_syntax_token token) {
    return token == MO_T_FN || token == MO_T_STRUCT || token == MO_T_ENUM || token == MO_T_TEST ||
           token == MO_T_VAR || token == MO_T_IF || token == MO_T_CASE || token == MO_T_FOR ||
           token == MO_T_WHILE || token == MO_T_RETURN || token == MO_T_BREAK ||
           token == MO_T_CONTINUE || token == MO_T_ASSERT || token == MO_T_NAME ||
           token == MO_T_CAPNAME || token == MO_T_LPAREN || token == MO_T_LBRACK ||
           token == MO_T_LBRACE || token == MO_T_INT || token == MO_T_DECIMAL ||
           token == MO_T_CHAR || token == MO_T_TEXT || token == MO_T_TEXT_HEAD ||
           token == MO_T_TRUE || token == MO_T_FALSE || token == MO_T_MINUS || token == MO_T_TRY ||
           token == MO_T_NOT || token == MO_T_STOP || token == MO_T_UNDERSCORE ||
           token == MO_T_ALLOWS || token == MO_T_FORBIDS || token == MO_T_PROMISE ||
           token == MO_T_EXAMPLES;
}

static void recover(struct parser *p, uint32_t first_line) {
    p->lx.recovering = true;
    if (p->current.kind == MO_T_NL) {
        p->lx.failed = false;
        next(p);
    } else if (p->current.kind != MO_T_ERROR && p->current.line > first_line && line_start(p)) {
        p->lx.failed = false;
    } else if (p->current.kind != MO_T_ERROR && p->current.kind != MO_T_EOF &&
               p->current.line > first_line) {
        /* A missing opening-line delimiter may already have consumed the next
         * line's first token. Read that physical line again, not just its tail. */
        p->lx.pos = p->lx.source->line_starts[p->current.line - 1];
        p->lx.line = p->current.line;
        p->lx.column = 1;
        p->lx.depth = 0;
        p->lx.last = MO_T_EOF;
        p->lx.has_pending = false;
        p->has_lookahead = false;
        p->lx.failed = false;
        struct mo_syntax_comment **tail = &p->lx.comments;
        while (*tail != NULL && (*tail)->span.start < p->lx.pos)
            tail = &(*tail)->next;
        *tail = NULL;
        p->lx.comment_tail = tail;
        p->current = mo_syntax_next(&p->lx);
    } else {
        p->has_lookahead = false;
        mo_syntax_resume(&p->lx);
        p->current = mo_syntax_next(&p->lx);
    }
    while (!p->lx.fatal && p->current.kind != MO_T_EOF && p->current.kind != MO_T_END &&
           p->current.kind != MO_T_ELSE && !can_start(p->current.kind)) {
        p->has_lookahead = false;
        mo_syntax_resume(&p->lx);
        p->current = mo_syntax_next(&p->lx);
    }
    p->lx.recovering = false;
    p->lx.failed = false;
}

static MO_MUST_USE struct mo_syntax *read_item(struct parser *p, enum item_context context) {
    jmp_buf escape;
    jmp_buf *saved = p->escape;
    uint32_t depth = p->depth, first_line = p->current.line;
    p->escape = &escape;
    if (setjmp(escape) != 0) {
        p->depth = depth;
        p->escape = saved;
        if (p->lx.fatal)
            longjmp(*saved, 1);
        recover(p, first_line);
        if (p->lx.fatal)
            longjmp(*saved, 1);
        return NULL;
    }
    struct mo_syntax *n;
    if (context == CASE_ITEM) {
        struct mo_span start = p->current.span;
        n = statement(p, false);
        if (n->kind == MO_S_EXPR_STMT &&
            (p->current.kind == MO_T_BAR || p->current.kind == MO_T_IF ||
             p->current.kind == MO_T_COLON)) {
            struct mo_syntax *alternatives = sequence(p);
            append(p, alternatives, n->children[0]);
            while (take(p, MO_T_BAR))
                append(p, alternatives, expr(p, 10));
            struct mo_syntax *guard = take(p, MO_T_IF) ? expr(p, 1) : NULL;
            expect(p, MO_T_COLON);
            if (p->current.kind == MO_T_FOR || p->current.kind == MO_T_WHILE)
                error(p, MO_CODE_SOURCE_CASE, p->current.span, "an arm is Pattern: statement",
                      "A loop starts on its own line in an arm (reference 7.2).");
            struct mo_syntax *body = p->current.kind == MO_T_NL ? NULL : statement(p, false);
            n = N(MO_S_ARM, start, alternatives, guard, body);
        }
        expect(p, MO_T_NL);
    } else if (context == FIELD_ITEM || context == CASE_DECL_ITEM)
        n = member(p, context == FIELD_ITEM);
    else if (context == SPEC_ITEM)
        n = specification_item(p);
    else if (context == ANCHOR_ITEM)
        n = anchor(p);
    else if (context == EXPR_ITEM) {
        n = expr(p, 0);
        expect(p, MO_T_NL);
    } else if (context == STATEMENT_ITEM)
        n = statement(p, true);
    else
        n = declaration(p, context == FRAGMENT_ITEM);
    validate(p, n, false);
    if (context == MODULE_ITEM || context == FRAGMENT_ITEM) {
        if (n->kind == MO_S_FN || n->kind == MO_S_STRUCT || n->kind == MO_S_ENUM ||
            n->kind == MO_S_CONST) {
            p->names++;
            if (p->names > MO_LIMIT_MODULE_NAMES)
                error(p, MO_CODE_TOO_MANY_NAMES, n->span, "a module declares too many names", NULL);
        }
    }
    p->escape = saved;
    return n;
}

static MO_MUST_USE struct mo_syntax *initial(struct parser *p, struct mo_header *out,
                                             bool fragment) {
    jmp_buf escape;
    jmp_buf *saved = p->escape;
    p->escape = &escape;
    if (setjmp(escape) != 0) {
        p->escape = saved;
        if (p->lx.fatal)
            longjmp(*saved, 1);
        recover(p, 1);
        return NULL;
    }
    next(p);
    struct mo_syntax *h = NULL;
    if (!fragment || p->current.kind == MO_T_MODULE)
        h = header(p, out);
    p->escape = saved;
    return h;
}

static MO_MUST_USE struct mo_module_syntax *parse_run(struct parser *p, bool fragment) {
    jmp_buf escape;
    p->escape = &escape;
    if (setjmp(escape) != 0)
        return NULL;
    struct mo_module_syntax *module = allocate(p, sizeof *module);
    *module = (struct mo_module_syntax){.span = {p->lx.file, 0, p->lx.source->length}};
    uint32_t errors = p->lx.diags->count;
    struct mo_syntax *h = initial(p, &module->header, fragment);
    bool incomplete = errors != p->lx.diags->count;
    struct mo_syntax *items = sequence(p);
    while (p->current.kind != MO_T_EOF) {
        struct mo_syntax *item = read_item(p, fragment ? FRAGMENT_ITEM : MODULE_ITEM);
        if (item != NULL)
            append(p, items, item);
        else
            items->incomplete = true;
    }
    module->tree = N(MO_S_FILE, module->span, h, items);
    module->tree->incomplete = module->tree->incomplete || incomplete;
    return module;
}

static MO_MUST_USE struct mo_module_syntax *parse(struct mo_arena *arena,
                                                  const struct mo_sources *sources, uint32_t file,
                                                  struct mo_diags *diags, bool fragment) {
    struct parser p = {0};
    mo_syntax_lex_init(&p.lx, arena, sources, file, diags);
    p.current.span.file = file;
    struct mo_module_syntax *module = parse_run(&p, fragment);
    if (module != NULL && !mo_syntax_attach(sources, module->tree, p.lx.comments)) {
        bool kept = mo_syntax_error(&p.lx, MO_CODE_SYNTAX_NO_MEMORY, module->span,
                                    "comment attachment has no room", NULL);
        (void)kept;
        return NULL;
    }
    return module;
}

struct mo_module_syntax *mo_parse(struct mo_arena *arena, const struct mo_sources *sources,
                                  uint32_t file, struct mo_diags *diags) {
    return parse(arena, sources, file, diags, false);
}

struct mo_module_syntax *mo_parse_fragment(struct mo_arena *arena, const struct mo_sources *sources,
                                           uint32_t file, struct mo_diags *diags) {
    return parse(arena, sources, file, diags, true);
}

static MO_MUST_USE bool read_header(struct parser *p, struct mo_header *header_out) {
    jmp_buf escape;
    p->escape = &escape;
    if (setjmp(escape) != 0)
        return false;
    next(p);
    struct mo_header result;
    struct mo_syntax *h = header(p, &result);
    MO_ASSERT(h != NULL);
    *header_out = result;
    return true;
}

bool mo_read_header(struct mo_arena *arena, const struct mo_sources *sources, uint32_t file,
                    struct mo_header *header_out, struct mo_diags *diags) {
    struct parser p = {0};
    mo_syntax_lex_init(&p.lx, arena, sources, file, diags);
    p.lx.header_only = true;
    return read_header(&p, header_out);
}
