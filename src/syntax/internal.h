/* Private lexer and parser state. */
#ifndef MO_SYNTAX_INTERNAL_H
#define MO_SYNTAX_INTERNAL_H

#include "base/limits.h"
#include "syntax/syntax.h"

struct mo_token {
    enum mo_syntax_token kind;
    struct mo_span span;
    uint32_t line;
    uint32_t column;
};

struct mo_comment_storage {
    struct mo_syntax_comment value;
    struct mo_comment_storage *previous;
    struct mo_token preceding;
    struct mo_syntax *owner;
};

struct mo_lex {
    const struct mo_source *source;
    uint32_t file;
    uint32_t pos;
    uint32_t line;
    uint32_t column;
    enum mo_syntax_token last;
    bool lambda_parameters; /* after fn(, followed only by names, _ and commas */
    bool lambda_close;      /* the last scanned token closes lambda parameters */
    struct mo_token preceding;
    uint32_t preceding_line; /* physical line at the end of that token */
    bool ignore_comments;    /* bounded token replay for source-independent anchors */
    struct mo_token pending;
    bool has_pending;
    uint32_t depth;
    struct {
        uint32_t braces;
        bool triple;
        uint32_t indent;
    } interpolations[MO_LIMIT_INTERPOLATION_DEPTH];
    struct mo_arena *arena;
    struct mo_diags *diags;
    struct mo_syntax_comment *comments;
    struct mo_syntax_comment **comment_tail;
    struct mo_comment_storage *last_comment;
    uint32_t triple_close;
    uint32_t triple_indent;
    uint32_t recovery_end;
    uint8_t limits_reported;
    bool triple_cached;
    bool failed;
    bool fatal;
    bool header_only;
    bool recovering;
};

void mo_syntax_lex_init(struct mo_lex *lx, struct mo_arena *arena, const struct mo_sources *sources,
                        uint32_t file, struct mo_diags *diags);
MO_MUST_USE uint32_t mo_syntax_decode(const uint8_t *bytes, uint32_t length, uint32_t *cp);
MO_MUST_USE struct mo_token mo_syntax_next(struct mo_lex *lx);
void mo_syntax_error(struct mo_lex *lx, uint16_t code, struct mo_span span, const char *message,
                     const char *reason);
void mo_syntax_resume(struct mo_lex *lx);
MO_MUST_USE bool mo_syntax_attach(struct mo_arena *arena, const struct mo_sources *sources,
                                  struct mo_syntax *tree, struct mo_syntax_comment *comments);

#endif
