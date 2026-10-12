/* Recursive descent, with a recovery boundary around each source line. */
#include "base/codes.h"
#include "syntax/internal.h"

#include <setjmp.h>
#include <string.h>

struct parser {
    struct mo_lex lx;
    struct mo_token current;
    struct mo_span previous;
    jmp_buf *escape;
    uint32_t depth;
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
    p->current = mo_syntax_next(&p->lx);
    if (p->current.kind == MO_T_ERROR)
        longjmp(*p->escape, 1);
}

static MO_MUST_USE bool take(struct parser *p, enum mo_syntax_token token) {
    if (p->current.kind != token)
        return false;
    next(p);
    return true;
}

static void expect(struct parser *p, enum mo_syntax_token token) {
    if (!take(p, token))
        error(p, MO_CODE_SOURCE_SYNTAX, p->current.span,
              "a statement ends with a line break; blocks close with end",
              "Blocks open with a keyword and close with end (reference 2.4).");
}

static MO_MUST_USE struct mo_syntax *node(struct parser *p, enum mo_syntax_kind kind,
                                          struct mo_span start, uint32_t count,
                                          struct mo_syntax *const children[]) {
    struct mo_syntax *n = allocate(p, sizeof *n);
    *n = (struct mo_syntax){.kind = kind, .span = start, .count = count};
    if (p->previous.end > n->span.end)
        n->span.end = p->previous.end;
    if (count != 0) {
        n->children = allocate(p, count * sizeof *n->children);
        memcpy(n->children, children, count * sizeof *n->children);
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
    /* Geometric growth without a capacity field in the public tree. */
    if (n->count == 0 || (n->count & (n->count - 1)) == 0) {
        uint32_t capacity = n->count == 0 ? 1 : n->count * 2;
        struct mo_syntax **children = allocate(p, capacity * sizeof *children);
        if (n->count != 0)
            memcpy(children, n->children, n->count * sizeof *children);
        n->children = children;
    }
    n->children[n->count++] = child;
    if (child != NULL) {
        if (n->count == 1)
            n->span.start = child->span.start;
        n->span.end = child->span.end;
    }
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
    struct mo_syntax *n = N(MO_S_MODULE, start, module_path, expose, uses);
    *out = (struct mo_header){n->span, module_path, expose, uses};
    return n;
}

static MO_MUST_USE struct mo_module_syntax *parse(struct mo_arena *arena,
                                                  const struct mo_sources *sources, uint32_t file,
                                                  struct mo_diags *diags, bool fragment) {
    struct parser p = {0};
    mo_syntax_lex_init(&p.lx, arena, sources, file, diags);
    jmp_buf escape;
    p.escape = &escape;
    if (setjmp(escape) != 0)
        return NULL;
    next(&p);
    struct mo_module_syntax *module = allocate(&p, sizeof *module);
    *module = (struct mo_module_syntax){.span = {file, 0, sources->files[file].length}};
    struct mo_syntax *h = NULL;
    if (!fragment || p.current.kind == MO_T_MODULE)
        h = header(&p, &module->header);
    struct mo_syntax *items = sequence(&p);
    while (p.current.kind != MO_T_EOF) {
        struct mo_syntax *lhs = name(&p, MO_T_NAME);
        expect(&p, MO_T_ASSIGN);
        struct mo_syntax *rhs = leaf(&p);
        expect(&p, MO_T_NL);
        append(&p, items, node(&p, MO_S_CONST, lhs->span, 2, (struct mo_syntax *[]){lhs, rhs}));
    }
    module->tree = node(&p, MO_S_FILE, module->span, 2, (struct mo_syntax *[]){h, items});
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

bool mo_read_header(struct mo_arena *arena, const struct mo_sources *sources, uint32_t file,
                    struct mo_header *header_out, struct mo_diags *diags) {
    struct parser p = {0};
    mo_syntax_lex_init(&p.lx, arena, sources, file, diags);
    jmp_buf escape;
    p.escape = &escape;
    if (setjmp(escape) != 0)
        return false;
    next(&p);
    struct mo_header result;
    struct mo_syntax *h = header(&p, &result);
    MO_ASSERT(h != NULL);
    *header_out = result;
    return true;
}
