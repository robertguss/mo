/* Source syntax. The caller owns the arena; source bytes must outlive the tree.
 * Spans are half-open byte ranges. Children have the order of grammar/mo.y's
 * productions, including NULL for an absent optional child. */
#ifndef MO_SYNTAX_H
#define MO_SYNTAX_H

#include "base/diag.h"

enum mo_syntax_kind {
    MO_S_FILE,
    MO_S_MODULE,
    MO_S_EXPOSE,
    MO_S_EXPOSE_ALL,
    MO_S_USE,
    MO_S_FN,
    MO_S_PARAM,
    MO_S_WHERE,
    MO_S_STRUCT,
    MO_S_FIELD,
    MO_S_ENUM,
    MO_S_CASE_DECL,
    MO_S_TEST,
    MO_S_CONST,
    MO_S_PROMISE,
    MO_S_ANCHOR,
    MO_S_EXAMPLES,
    MO_S_TYPE,
    MO_S_SEQUENCE,
    MO_S_EXPR_STMT,
    MO_S_BIND,
    MO_S_VAR,
    MO_S_ADD_ASSIGN,
    MO_S_SUB_ASSIGN,
    MO_S_RETURN,
    MO_S_BREAK,
    MO_S_CONTINUE,
    MO_S_ASSERT,
    MO_S_FOR,
    MO_S_WHILE,
    MO_S_BINDER,
    MO_S_IF,
    MO_S_ELSE,
    MO_S_LINE_IF,
    MO_S_CASE,
    MO_S_ARM,
    MO_S_LAMBDA,
    MO_S_BINARY,
    MO_S_NEGATE,
    MO_S_NOT,
    MO_S_TRY,
    MO_S_IS,
    MO_S_RANGE,
    MO_S_CALL,
    MO_S_NAMED_ARG,
    MO_S_FIELD_ACCESS,
    MO_S_QUALIFIED,
    MO_S_POSITION,
    MO_S_INT,
    MO_S_DECIMAL,
    MO_S_CHAR,
    MO_S_TEXT,
    MO_S_TEXT_INTERPOLATED,
    MO_S_TRUE,
    MO_S_FALSE,
    MO_S_NAME,
    MO_S_CAPNAME,
    MO_S_UNDERSCORE,
    MO_S_GROUP,
    MO_S_TUPLE,
    MO_S_LIST,
    MO_S_REST,
    MO_S_MAP,
    MO_S_MAP_ENTRY,
    MO_S_STOP,
    MO_S_TOKEN,
};

/* Tokens are independent of Bison's generated numeric values. */
enum mo_syntax_token {
    MO_T_EOF,
    MO_T_ERROR,
    MO_T_NL,
    MO_T_NAME,
    MO_T_CAPNAME,
    MO_T_UNDERSCORE,
    MO_T_INT,
    MO_T_DECIMAL,
    MO_T_CHAR,
    MO_T_TEXT,
    MO_T_TEXT_HEAD,
    MO_T_TEXT_MID,
    MO_T_TEXT_TAIL,
    MO_T_MODULE,
    MO_T_EXPOSE,
    MO_T_USE,
    MO_T_FN,
    MO_T_STRUCT,
    MO_T_ENUM,
    MO_T_VAR,
    MO_T_IF,
    MO_T_ELSE,
    MO_T_CASE,
    MO_T_FOR,
    MO_T_IN,
    MO_T_WHILE,
    MO_T_BREAK,
    MO_T_CONTINUE,
    MO_T_RETURN,
    MO_T_TRY,
    MO_T_AND,
    MO_T_OR,
    MO_T_NOT,
    MO_T_IS,
    MO_T_IMPLIES,
    MO_T_TRUE,
    MO_T_FALSE,
    MO_T_STOP,
    MO_T_PROMISE,
    MO_T_ALLOWS,
    MO_T_FORBIDS,
    MO_T_EXAMPLES,
    MO_T_TEST,
    MO_T_ASSERT,
    MO_T_WHERE,
    MO_T_END,
    MO_T_LPAREN,
    MO_T_RPAREN,
    MO_T_LBRACK,
    MO_T_RBRACK,
    MO_T_LBRACE,
    MO_T_RBRACE,
    MO_T_COMMA,
    MO_T_COLON,
    MO_T_DOT,
    MO_T_DOTDOT,
    MO_T_BAR,
    MO_T_ASSIGN,
    MO_T_PLUSEQ,
    MO_T_MINUSEQ,
    MO_T_EQ,
    MO_T_NE,
    MO_T_LT,
    MO_T_LE,
    MO_T_GT,
    MO_T_GE,
    MO_T_PLUS,
    MO_T_MINUS,
    MO_T_STAR,
    MO_T_SLASH,
    MO_T_PERCENT,
};

struct mo_syntax_comment {
    struct mo_span span;
    uint32_t indentation;
    bool documentation;
    struct mo_syntax_comment *next;
};

struct mo_syntax {
    enum mo_syntax_kind kind;
    struct mo_span span;
    enum mo_syntax_token token; /* operator or literal spelling */
    uint32_t count;
    struct mo_syntax **children;
    struct mo_syntax_comment *leading;
    struct mo_syntax_comment *trailing;
    struct mo_syntax_comment *ending; /* comments at the end of a block */
};

struct mo_header {
    struct mo_span span;
    struct mo_syntax *path;   /* sequence of CapCase path parts */
    struct mo_syntax *expose; /* NULL or exposed names */
    struct mo_syntax *uses;   /* sequence of use nodes */
};

struct mo_module_syntax {
    struct mo_span span;
    struct mo_header header;
    struct mo_syntax *tree; /* file: module header or NULL, then items */
};

/* Syntax errors return a partial tree and diagnostics. NULL means allocation
 * failed or diagnostics could no longer be kept. */
MO_MUST_USE struct mo_module_syntax *mo_parse(struct mo_arena *arena,
                                              const struct mo_sources *sources, uint32_t file,
                                              struct mo_diags *diags);
/* Reference snippets and grammar fixtures: statements and declarations in any
 * order, with an optional header. The same syntax rules as mo_parse. */
MO_MUST_USE struct mo_module_syntax *mo_parse_fragment(struct mo_arena *arena,
                                                       const struct mo_sources *sources,
                                                       uint32_t file, struct mo_diags *diags);
/* Reads only the header, not the body. False leaves header_out unchanged. */
MO_MUST_USE bool mo_read_header(struct mo_arena *arena, const struct mo_sources *sources,
                                uint32_t file, struct mo_header *header_out,
                                struct mo_diags *diags);

#endif
