/* The executable grammar of Mo, Language Reference v0.1.
 *
 * This file is the authority on what Mo's syntax accepts; the reference's
 * grammar summary is written from it. Bison builds an LR(1) parser from it and
 * fails the build on any conflict (%expect 0), and the grammar
 * declares no operator precedence: every precedence level is its own rule. A
 * grammar Bison accepts that way is unambiguous, which is what decision D33
 * asks of Mo.
 *
 * Three things are outside the rules below:
 * - lexer.c decides where lines end (NL), and reads names, numbers and text,
 *   including the parts of interpolated text;
 * - ast.c checks what a context-free grammar can't: that a pattern is a
 *   pattern, what stands on the left of `=`, and how cases and records are
 *   built;
 * - meaning (types, names in scope, exhaustiveness) is the compiler's, from
 *   Phase 2.
 */

%code top {
#include <stdio.h>
#include <string.h>
}

%code requires {
#include "ast.h"
#include "lexer.h"
}

%code provides {
int yylex(YYSTYPE *value, YYLTYPE *location, struct lexer *lx);
const char *mo_token_name(int token);
}

%code {
static void yyerror(YYLTYPE *location, struct lexer *lx, struct node **root, const char *message);

static struct node *OP(struct node *node, int token) {
    node->op = token;
    return node;
}

#define KIDS(...) (struct node *[]){__VA_ARGS__}
#define COUNT(...) ((int)(sizeof(KIDS(__VA_ARGS__)) / sizeof(struct node *)))
#define MK(kind, loc, ...) \
    mo_node(lx, (kind), (loc).first_line, (loc).first_column, COUNT(__VA_ARGS__), KIDS(__VA_ARGS__))
#define LEAF(kind, loc) mo_node(lx, (kind), (loc).first_line, (loc).first_column, 0, NULL)
#define ADD(list, kid) mo_node_append(lx, (list), (kid))
}

%define api.pure full
%define api.value.type {struct node *}
%define parse.error detailed
%define lr.type ielr
%locations
%param {struct lexer *lx}
%parse-param {struct node **root}
%expect 0

%token START_FILE START_FRAGMENT
%token NL "end of line"
%token NAME "name" CAPNAME "capitalized name" UNDERSCORE "_"
%token INT "whole number" DECIMAL "decimal number" CHAR "character" TEXT "text"
%token TEXT_HEAD "text before #{" TEXT_MID "text between interpolations" TEXT_TAIL "text after }"
%token MODULE "module" EXPOSE "expose" USE "use" FN "fn" STRUCT "struct" ENUM "enum"
%token VAR "var" IF "if" ELSE "else" CASE "case" FOR "for" IN "in" WHILE "while"
%token BREAK "break" CONTINUE "continue" RETURN "return" TRY "try"
%token AND "and" OR "or" NOT "not" IS "is" IMPLIES "implies" TRUE "true" FALSE "false"
%token STOP "stop" PROMISE "promise" ALLOWS "allows" FORBIDS "forbids"
%token EXAMPLES "examples" TEST "test" ASSERT "assert" WHERE "where" END "end"
%token LPAREN "(" RPAREN ")" LBRACK "[" RBRACK "]" LBRACE "{" RBRACE "}"
%token COMMA "," COLON ":" DOT "." DOTDOT ".." BAR "|"
%token ASSIGN "=" PLUSEQ "+=" MINUSEQ "-="
%token EQ "==" NE "!=" LT "<" LE "<=" GT ">" GE ">="
%token PLUS "+" MINUS "-" STAR "*" SLASH "/" PERCENT "%"

%start start

%%

start
    : START_FILE file                       { *root = $2; }
    | START_FRAGMENT fragment               { *root = $2; }
    ;

/* A file is one module. A fragment, as the reference's examples show code, is
 * declarations and statements in any order, with an optional module header. */
file
    : module_header top_items               { $$ = MK(K_FILE, @$, $1, $2); }
    ;

fragment
    : fragment_items                        { $$ = MK(K_FILE, @$, NULL, $1); }
    | module_header fragment_items          { $$ = MK(K_FILE, @$, $1, $2); }
    ;

/* Modules */

module_header
    : "module" module_path NL expose uses   { $$ = MK(K_MODULE, @$, $2, $4, $5); }
    ;

module_path
    : CAPNAME                               { $$ = MK(K_LIST_OF, @$, $1); }
    | module_path "." CAPNAME               { $$ = ADD($1, $3); }
    ;

expose
    : %empty                                { $$ = NULL; }
    | "expose" expose_items NL              { $$ = $2; }
    ;

expose_items
    : expose_item                           { $$ = MK(K_EXPOSE, @$, $1); }
    | expose_items "," expose_item          { $$ = ADD($1, $3); }
    ;

expose_item
    : NAME
    | CAPNAME
    | CAPNAME "(" ".." ")"                  { $$ = MK(K_EXPOSE_ALL, @$, $1); }
    ;

uses
    : %empty                                { $$ = LEAF(K_LIST_OF, @$); }
    | uses use                              { $$ = ADD($1, $2); }
    ;

use
    : "use" module_path NL                  { $$ = MK(K_USE, @$, $2, NULL); }
    | "use" module_path "{" use_items "}" NL
                                            { $$ = MK(K_USE, @$, $2, $4); }
    ;

use_items
    : use_item                              { $$ = MK(K_LIST_OF, @$, $1); }
    | use_items "," use_item                { $$ = ADD($1, $3); }
    ;

use_item
    : NAME
    | CAPNAME
    ;

top_items
    : %empty                                { $$ = LEAF(K_LIST_OF, @$); }
    | top_items top_item                    { $$ = ADD($1, $2); }
    ;

top_item
    : function
    | struct
    | enum
    | test
    | NAME "=" expr NL                      { $$ = MK(K_CONST, @$, $1, $3); }
    ;

fragment_items
    : %empty                                { $$ = LEAF(K_LIST_OF, @$); }
    | fragment_items fragment_item          { $$ = ADD($1, $2); }
    ;

fragment_item
    : function
    | struct
    | enum
    | test
    | statement
    ;

/* Types */

type
    : type_name                             { $$ = MK(K_TYPE, @$, $1); }
    | type_name "(" types ")"               { $$ = MK(K_TYPE, @$, $1, $3); }
    | "(" type "," types ")"                { $$ = MK(K_TYPE, @$, $2, $4); }
    | "fn" "(" ")" function_result          { $$ = MK(K_TYPE, @$, $4); }
    | "fn" "(" types ")" function_result    { $$ = MK(K_TYPE, @$, $3, $5); }
    ;

type_name
    : CAPNAME
    | CAPNAME "." CAPNAME                   { $$ = MK(K_QUALIFIED, @$, $1, $3); }
    ;

types
    : type                                  { $$ = MK(K_LIST_OF, @$, $1); }
    | types "," type                        { $$ = ADD($1, $3); }
    ;

function_result
    : %empty                                { $$ = NULL; }
    | ":" type                              { $$ = $2; }
    ;

type_parameters
    : %empty                                { $$ = NULL; }
    | "(" capnames ")"                      { $$ = $2; }
    ;

capnames
    : CAPNAME                               { $$ = MK(K_LIST_OF, @$, $1); }
    | capnames "," CAPNAME                  { $$ = ADD($1, $3); }
    ;

/* Records and choice types */

struct
    : "struct" CAPNAME type_parameters NL fields "end" NL
                                            { $$ = MK(K_STRUCT, @$, $2, $3, $5); }
    ;

fields
    : %empty                                { $$ = LEAF(K_LIST_OF, @$); }
    | fields NAME ":" type NL               { $$ = ADD($1, MK(K_FIELD, @2, $2, $4)); }
    ;

enum
    : "enum" CAPNAME type_parameters NL cases "end" NL
                                            { $$ = MK(K_ENUM, @$, $2, $3, $5); }
    ;

cases
    : %empty                                { $$ = LEAF(K_LIST_OF, @$); }
    | cases case NL                         { $$ = ADD($1, $2); }
    ;

case
    : CAPNAME                               { $$ = MK(K_CASE_DECL, @$, $1, NULL); }
    | CAPNAME "(" type ")"                  { $$ = MK(K_CASE_DECL, @$, $1, $3); }
    | CAPNAME "(" named_fields comma ")"    { $$ = MK(K_CASE_DECL, @$, $1, $3); }
    ;

named_fields
    : NAME ":" type                         { $$ = MK(K_LIST_OF, @$, MK(K_FIELD, @$, $1, $3)); }
    | named_fields "," NAME ":" type        { $$ = ADD($1, MK(K_FIELD, @3, $3, $5)); }
    ;

/* Functions */

function
    : "fn" NAME "(" parameters ")" function_result where NL specification statements "end" NL
                                            { $$ = MK(K_FN, @$, $2, $4, $6, $7, $9, $10); }
    ;

parameters
    : %empty                                { $$ = LEAF(K_LIST_OF, @$); }
    | parameter_list comma
    ;

parameter_list
    : parameter                             { $$ = MK(K_LIST_OF, @$, $1); }
    | parameter_list "," parameter          { $$ = ADD($1, $3); }
    ;

parameter
    : NAME ":" type                         { $$ = MK(K_PARAM, @$, $1, $3); }
    ;

where
    : %empty                                { $$ = NULL; }
    | "where" constraints                   { $$ = $2; }
    ;

constraints
    : constraint                            { $$ = MK(K_WHERE, @$, $1); }
    | constraints "," constraint            { $$ = ADD($1, $3); }
    ;

constraint
    : CAPNAME ":" CAPNAME                   { $$ = MK(K_LIST_OF, @$, $1, $3); }
    ;

comma
    : %empty                                { $$ = NULL; }
    | ","                                   { $$ = NULL; }
    ;

/* Promises and examples come first in a function's body: its specification. */

specification
    : %empty                                { $$ = LEAF(K_LIST_OF, @$); }
    | specification promise                 { $$ = ADD($1, $2); }
    | specification examples                { $$ = ADD($1, $2); }
    ;

promise
    : "promise" TEXT NL lines anchors "end" NL
                                            { $$ = MK(K_PROMISE, @$, $2, $4, $5); }
    ;

anchors
    : anchor                                { $$ = MK(K_LIST_OF, @$, $1); }
    | anchors anchor                        { $$ = ADD($1, $2); }
    ;

anchor
    : "allows" anchor_values NL             { $$ = OP(MK(K_ANCHOR, @$, $2), ALLOWS); }
    | "forbids" anchor_values NL            { $$ = OP(MK(K_ANCHOR, @$, $2), FORBIDS); }
    ;

anchor_values
    : NAME ":" expr                         { $$ = MK(K_LIST_OF, @$, MK(K_NAMED_ARG, @$, $1, $3)); }
    | anchor_values "," NAME ":" expr       { $$ = ADD($1, MK(K_NAMED_ARG, @3, $3, $5)); }
    ;

examples
    : "examples" NL lines "end" NL          { $$ = MK(K_EXAMPLES, @$, $3); }
    ;

lines
    : expr NL                               { $$ = MK(K_LIST_OF, @$, $1); }
    | lines expr NL                         { $$ = ADD($1, $2); }
    ;

test
    : "test" TEXT NL statements "end" NL    { $$ = MK(K_TEST, @$, $2, $4); }
    ;

/* Statements */

statements
    : %empty                                { $$ = LEAF(K_LIST_OF, @$); }
    | statements statement                  { $$ = ADD($1, $2); }
    ;

statement
    : simple_statement NL
    | "for" binder "in" expr NL statements "end" NL
                                            { $$ = MK(K_FOR, @$, $2, $4, $6); }
    | "while" condition NL statements "end" NL
                                            { $$ = MK(K_WHILE, @$, $2, $4); }
    ;

/* A statement that starts on one line, so it can also stand on a case arm's
 * line. A block `if` stands only here, as a statement or after `=`: after
 * `return`, `if` starts a guard (`return x if c`) or a one-line `if`. A guard,
 * a trailing `if`, is allowed only on the three early exits: return, break and
 * continue. */
simple_statement
    : expr                                  { $$ = MK(K_EXPR_STMT, @$, $1); }
    | block_if                              { $$ = MK(K_EXPR_STMT, @$, $1); }
    | expr "=" expr                         { $$ = MK(K_BIND, @$, $1, $3); }
    | expr "=" block_if                     { $$ = MK(K_BIND, @$, $1, $3); }
    | expr "+=" expr                        { $$ = MK(K_ADD_ASSIGN, @$, $1, $3); }
    | expr "-=" expr                        { $$ = MK(K_SUB_ASSIGN, @$, $1, $3); }
    | "var" NAME "=" expr                   { $$ = MK(K_VAR, @$, $2, $4); }
    | "var" NAME "=" block_if               { $$ = MK(K_VAR, @$, $2, $4); }
    | "return"                              { $$ = MK(K_RETURN, @$, NULL, NULL); }
    | "return" expr                         { $$ = MK(K_RETURN, @$, $2, NULL); }
    | "return" "if" condition               { $$ = MK(K_RETURN, @$, NULL, $3); }
    | "return" expr "if" condition          { $$ = MK(K_RETURN, @$, $2, $4); }
    | "break"                               { $$ = MK(K_BREAK, @$, NULL); }
    | "break" "if" condition                { $$ = MK(K_BREAK, @$, $3); }
    | "continue"                            { $$ = MK(K_CONTINUE, @$, NULL); }
    | "continue" "if" condition             { $$ = MK(K_CONTINUE, @$, $3); }
    | "assert" expr                         { $$ = MK(K_ASSERT, @$, $2); }
    ;

binder
    : NAME                                  { $$ = MK(K_BINDER, @$, $1); }
    | "_"                                   { $$ = MK(K_BINDER, @$, $1); }
    | "(" binders ")"                       { $$ = $2; }
    ;

binders
    : binder "," binder                     { $$ = MK(K_TUPLE, @$, $1, $3); }
    | binders "," binder                    { $$ = ADD($1, $3); }
    ;

/* Expressions, from the loosest operator to the tightest */

expr
    : line_if
    | implies
    ;

condition
    : implies
    ;

/* `if c: a else: b` is a value on one line; `else if` chains it. */
line_if
    : "if" implies ":" implies "else" line_else
                                            { $$ = MK(K_LINE_IF, @$, $2, $4, $6); }
    ;

line_else
    : ":" expr                              { $$ = $2; }
    | line_if
    ;

implies
    : or
    | or "implies" or                       { $$ = OP(MK(K_BINARY, @$, $1, $3), IMPLIES); }
    ;

or
    : and
    | or "or" and                           { $$ = OP(MK(K_BINARY, @$, $1, $3), OR); }
    ;

and
    : not
    | and "and" not                         { $$ = OP(MK(K_BINARY, @$, $1, $3), AND); }
    ;

not
    : comparison
    | "not" not                             { $$ = MK(K_NOT, @$, $2); }
    ;

comparison
    : range
    | range comparison_operator range       { $$ = OP(MK(K_BINARY, @$, $1, $3), $2->op); }
    | range "is" unary                      { $$ = MK(K_IS, @$, $1, $3); }
    ;

comparison_operator
    : "==" | "!=" | "<" | "<=" | ">" | ">="
    ;

range
    : sum
    | sum ".." sum                          { $$ = MK(K_RANGE, @$, $1, $3); }
    ;

sum
    : product
    | sum "+" product                       { $$ = OP(MK(K_BINARY, @$, $1, $3), PLUS); }
    | sum "-" product                       { $$ = OP(MK(K_BINARY, @$, $1, $3), MINUS); }
    ;

product
    : unary
    | product "*" unary                     { $$ = OP(MK(K_BINARY, @$, $1, $3), STAR); }
    | product "/" unary                     { $$ = OP(MK(K_BINARY, @$, $1, $3), SLASH); }
    | product "%" unary                     { $$ = OP(MK(K_BINARY, @$, $1, $3), PERCENT); }
    ;

unary
    : postfix
    | "-" unary                             { $$ = MK(K_NEGATE, @$, $2); }
    | "try" unary                           { $$ = MK(K_TRY, @$, $2); }
    ;

postfix
    : primary
    | postfix "." NAME                      { $$ = MK(K_FIELD_ACCESS, @$, $1, $3); }
    | postfix "." CAPNAME                   { $$ = MK(K_QUALIFIED, @$, $1, $3); }
    | postfix "." INT                       { $$ = MK(K_POSITION, @$, $1, $3); }
    | postfix "(" arguments ")"             { $$ = MK(K_CALL, @$, $1, $3); }
    ;

arguments
    : %empty                                { $$ = LEAF(K_LIST_OF, @$); }
    | argument_list comma
    ;

argument_list
    : argument                              { $$ = MK(K_LIST_OF, @$, $1); }
    | argument_list "," argument            { $$ = ADD($1, $3); }
    ;

argument
    : expr
    | NAME ":" expr                         { $$ = MK(K_NAMED_ARG, @$, $1, $3); }
    ;

primary
    : INT
    | DECIMAL
    | CHAR
    | text
    | NAME
    | CAPNAME
    | "_"
    | "true"
    | "false"
    | "(" expr ")"                          { $$ = MK(K_GROUP, @$, $2); }
    | "(" expr "," elements comma ")"       { $$ = MK(K_TUPLE, @$, $2);
                                              for (int i = 0; i < $4->count; i++) {
                                                  $$ = ADD($$, $4->kids[i]);
                                              } }
    | "[" "]"                               { $$ = LEAF(K_LIST, @$); }
    | "[" list_items comma "]"              { $$ = $2; }
    | "{" "}"                               { $$ = LEAF(K_MAP, @$); }
    | "{" map_entries comma "}"             { $$ = $2; }
    | case_expr
    | lambda
    | "stop" text                           { $$ = MK(K_STOP, @$, $2); }
    ;

/* The tuple's elements after its first. */
elements
    : expr                                  { $$ = MK(K_LIST_OF, @$, $1); }
    | elements "," expr                     { $$ = ADD($1, $3); }
    ;

list_items
    : list_item                             { $$ = MK(K_LIST, @$, $1); }
    | list_items "," list_item              { $$ = ADD($1, $3); }
    ;

list_item
    : expr
    | ".." NAME                             { $$ = MK(K_REST, @$, $2); }
    | ".." "_"                              { $$ = MK(K_REST, @$, $2); }
    ;

map_entries
    : map_entry                             { $$ = MK(K_MAP, @$, $1); }
    | map_entries "," map_entry             { $$ = ADD($1, $3); }
    ;

map_entry
    : implies ":" expr                      { $$ = MK(K_MAP_ENTRY, @$, $1, $3); }
    ;

text
    : TEXT
    | TEXT_HEAD expr text_rest              { $$ = ADD(ADD(MK(K_TEXT_INTERPOLATED, @$, $1), $2), $3); }
    ;

/* The rest of an interpolated text: a right-nested spine of mid/expr pairs ending in a tail. */
text_rest
    : TEXT_TAIL                             { $$ = $1; }
    | TEXT_MID expr text_rest               { $$ = MK(K_TEXT_INTERPOLATED, @$, $1, $2, $3); }
    ;

block_if
    : "if" condition NL statements else "end"
                                            { $$ = MK(K_IF, @$, $2, $4, $5); }
    ;

else
    : %empty                                { $$ = NULL; }
    | "else" NL statements                  { $$ = MK(K_ELSE, @$, $3); }
    | "else" "if" condition NL statements else
                                            { $$ = MK(K_IF, @$, $3, $5, $6); }
    ;

/* A case's lines are arms and statements: each statement belongs to the arm
 * above it. Keeping them flat lets an arm's block end wherever the next arm
 * begins, with no marker. ast.c checks that the first line is an arm. */
case_expr
    : "case" expr NL case_lines "end"       { $$ = MK(K_CASE, @$, $2, $4); }
    ;

case_lines
    : %empty                                { $$ = LEAF(K_LIST_OF, @$); }
    | case_lines case_line                  { $$ = ADD($1, $2); }
    ;

case_line
    : arm_head ":" NL                       { $$ = MK(K_ARM, @$, $1->kids[0], $1->kids[1], NULL); }
    | arm_head ":" simple_statement NL      { $$ = MK(K_ARM, @$, $1->kids[0], $1->kids[1], $3); }
    | statement
    ;

arm_head
    : alternatives                          { $$ = MK(K_LIST_OF, @$, $1, NULL); }
    | alternatives "if" condition           { $$ = MK(K_LIST_OF, @$, $1, $3); }
    ;

alternatives
    : unary                                 { $$ = MK(K_LIST_OF, @$, $1); }
    | alternatives "|" unary                { $$ = ADD($1, $3); }
    ;

lambda
    : "fn" "(" lambda_parameters ")" expr "end"
                                            { $$ = MK(K_LAMBDA, @$, $3, $5); }
    | "fn" "(" lambda_parameters ")" NL statements "end"
                                            { $$ = MK(K_LAMBDA, @$, $3, $6); }
    ;

lambda_parameters
    : %empty                                { $$ = LEAF(K_LIST_OF, @$); }
    | lambda_parameter_list
    ;

lambda_parameter_list
    : lambda_parameter                      { $$ = MK(K_LIST_OF, @$, $1); }
    | lambda_parameter_list "," lambda_parameter
                                            { $$ = ADD($1, $3); }
    ;

lambda_parameter
    : NAME
    | "_"
    ;

%%

static void yyerror(YYLTYPE *location, struct lexer *lx, struct node **root, const char *message) {
    (void)root;
    const char *prefix = "syntax error, ";
    size_t length = sizeof "syntax error, " - 1;
    if (strncmp(message, prefix, length) == 0) {
        message += length;
    }
    // A word from another language, at the error or just before it, gets advice.
    const char *advice = NULL;
    const unsigned char *word = NULL;
    size_t word_length = 0;
    if (lx->last == NAME) {
        word = lx->src + lx->last_start;
        word_length = lx->last_length;
        advice = mo_other_spelling(word, word_length);
    }
    if (advice == NULL && lx->previous_length > 0) {
        word = lx->src + lx->previous_start;
        word_length = lx->previous_length;
        advice = mo_other_spelling(word, word_length);
    }
    if (advice != NULL) {
        mo_lexer_error(lx, location->first_line, location->first_column, "%s (%.*s is not Mo: %s)",
                       message, (int)word_length, (const char *)word, advice);
    } else if (lx->last == EXAMPLES || lx->last == PROMISE) {
        mo_lexer_error(lx, location->first_line, location->first_column,
                       "%s: examples and promises come first in a function's body", message);
    } else {
        mo_lexer_error(lx, location->first_line, location->first_column, "%s", message);
    }
}

const char *mo_token_name(int token) {
    return yysymbol_name(YYTRANSLATE(token));
}
