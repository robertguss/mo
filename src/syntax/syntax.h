/* Source syntax. The caller owns the arena; the tree copies token and comment text.
 * Spans are half-open byte ranges. Children follow source order, including NULL
 * for an absent optional child. Architecture 3.1 records the normalized shapes.
 *
 * Reserve at least 2 MiB of C stack: 255 nested lambdas need roughly 256-512 KiB
 * optimized, or 1 MiB under ASan, depending on compiler flags. Dense source can
 * need about 100 arena bytes per source byte (800 MiB for 8 MiB), or more for
 * flat chains. These are estimates, not bounds. The caller's arena limit bounds
 * tree storage plus temporary comment indexes; scratch uses only its unused
 * budget and is freed before return. Source and diagnostics have separate budgets. */
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
    MO_S_SERVICE,       /* clauses sequence, in source order */
    MO_S_SERVICE_STATE, /* type, initializer */
    MO_S_SERVICE_TOOLS, /* local names sequence */
    MO_S_SERVICE_EVERY, /* interval, local handler name */
    MO_S_SERVICE_TOKEN, /* plain text */
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
    MO_S_BINARY_CHAIN, /* operand, operator token, operand, ... */
    MO_S_POSTFIX,      /* base, then steps with no repeated base */
    MO_S_PREFIX,       /* operator tokens, then operand */
    MO_S_IF_PART,      /* condition, body */
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
    MO_T_SERVICE,
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

enum mo_syntax_comment_anchor { MO_COMMENT_OWN_LINE, MO_COMMENT_TOKEN, MO_COMMENT_ITEM };

struct mo_syntax_comment {
    struct mo_span span;
    const uint8_t *text; /* span.end - span.start bytes, including # */
    uint32_t indentation;
    bool documentation;
    enum mo_syntax_comment_anchor anchor;
    enum mo_syntax_token token; /* TOKEN: kind of the immediately preceding token */
    uint32_t token_index;       /* TOKEN: occurrence of that kind owned by this node;
                                 * ITEM: item index; both zero-based, ignoring children */
    struct mo_syntax_comment *next;
};

struct mo_syntax {
    enum mo_syntax_kind kind;
    struct mo_span span;
    const uint8_t *text;        /* leaves: span.end - span.start bytes, no terminating NUL */
    enum mo_syntax_token token; /* operator or literal spelling */
    uint32_t count;
    struct mo_syntax **children;
    struct mo_syntax_comment *leading;
    struct mo_syntax_comment *trailing;
    struct mo_syntax_comment *ending; /* comments at the end of a block or bracket scope */
    bool incomplete;   /* an erroneous child was omitted; do not check this as a whole */
    bool blank_before; /* one optional blank line before a statement/declaration's leading comments;
                        * false for the first statement in a block */
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

extern const char *const mo_duration_units[];
extern const size_t mo_duration_unit_count;
MO_MUST_USE const char *mo_duration_unit(const uint8_t *text, size_t length);

#endif
