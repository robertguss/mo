// The syntax tree the grammar builds, and the checks on it that a context-free
// grammar can't express: what may stand in a pattern, on the left of `=`, and
// in a construction's arguments.
#ifndef MO_GRAMMAR_AST_H
#define MO_GRAMMAR_AST_H

#include <stdbool.h>
#include <stddef.h>

#include "base/base.h"

struct lexer;

enum node_kind {
    // Declarations
    K_FILE,
    K_MODULE,
    K_EXPOSE,
    K_EXPOSE_ALL, // `Name(..)` on the expose line
    K_USE,
    K_FN,
    K_PARAM,
    K_WHERE,
    K_STRUCT,
    K_FIELD,
    K_ENUM,
    K_CASE_DECL,
    K_TEST,
    K_CONST,
    K_PROMISE,
    K_ANCHOR,
    K_EXAMPLES,
    K_TYPE,
    K_LIST_OF, // a list of children with no meaning of its own
    // Statements
    K_EXPR_STMT,
    K_BIND, // `left = right`: a binding, or an assignment to a var
    K_VAR,
    K_ADD_ASSIGN,
    K_SUB_ASSIGN,
    K_RETURN,
    K_BREAK,
    K_CONTINUE,
    K_ASSERT,
    K_FOR,
    K_WHILE,
    K_BINDER,
    // Expressions
    K_IF,      // block form: condition, body, then an else part or nothing
    K_ELSE,    // block form's else: body
    K_LINE_IF, // one-line form: condition, value, else value
    K_CASE,
    K_ARM, // alternatives, guard or NULL, statement on the arm's line or NULL
    K_LAMBDA,
    K_BINARY, // op holds the operator token
    K_NEGATE,
    K_NOT,
    K_TRY,
    K_IS,
    K_RANGE,
    K_CALL, // callee, then arguments
    K_NAMED_ARG,
    K_FIELD_ACCESS, // `x.name`
    K_QUALIFIED,    // `Module.Name`
    K_POSITION,     // `pair.0`
    K_INT,
    K_DECIMAL,
    K_CHAR,
    K_TEXT,
    K_TEXT_INTERPOLATED,
    K_TRUE,
    K_FALSE,
    K_NAME,
    K_CAPNAME,
    K_UNDERSCORE,
    K_GROUP,
    K_TUPLE,
    K_LIST,
    K_REST, // `..rest` or `.._` in a list pattern
    K_MAP,
    K_MAP_ENTRY,
    K_STOP,
    K_TOKEN, // any other token, kept only for its position
};

struct node {
    enum node_kind kind;
    int line;
    int col;
    int op;
    int count;
    int capacity;
    struct node **kids;
};

struct arena;

MO_MUST_USE struct arena *mo_arena_new(void);
void mo_arena_free(struct arena *arena);

MO_MUST_USE struct node *mo_node(struct lexer *lx, enum node_kind kind, int line, int col,
                                 int count, struct node *const kids[]);
MO_MUST_USE struct node *mo_node_append(struct lexer *lx, struct node *list, struct node *kid);

// Checks the parsed tree; reports the first problem through the lexer and
// returns false.
MO_MUST_USE bool mo_validate(struct lexer *lx, const struct node *root);

#endif
