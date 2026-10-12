// The lexer for Mo's executable grammar: turns source text into the tokens the
// grammar in mo.y reads, including the end-of-line tokens that end statements.
#ifndef MO_GRAMMAR_LEXER_H
#define MO_GRAMMAR_LEXER_H

#include <stdbool.h>
#include <stddef.h>

#include "../src/base.h"

struct node;
struct arena;

enum { MO_LEXER_MAX_INTERPOLATION = 16 };

// One open `#{` inside a text literal. While one is open, braces are counted so
// that the `}` closing it can be told from a map literal's.
struct mo_interpolation {
    int braces;
    bool triple;   // inside `"""` text
    size_t indent; // the closing `"""` line's indentation, for `"""` text
};

struct lexer {
    const char *path; // the file named in messages
    const unsigned char *src;
    size_t len;
    size_t pos;
    int line;          // 1-based, counted from the first line given to mo_lexer_init
    int col;           // 1-based, counted in characters
    int start_token;   // emitted before anything else, then 0
    int last;          // the last token returned, 0 before the first
    size_t last_start; // where the last token's text starts, and its length
    size_t last_length;
    size_t previous_start; // the same for the token before it
    size_t previous_length;
    size_t token_start; // where the token being read starts
    bool has_pending;   // a token read ahead while deciding on an end of line
    int pending;
    struct node *pending_value;
    int pending_line, pending_col;
    size_t pending_start, pending_length;
    struct mo_interpolation interpolations[MO_LEXER_MAX_INTERPOLATION];
    int depth; // open interpolations
    struct arena *arena;
    int errors;            // errors reported for this source
    bool quiet;            // report nothing (used by --expect-error)
    char first_error[512]; // the first message, kept for --expect-error
};

// Prepares a lexer over src, which need not end with a NUL. Lines are reported
// counting from first_line. fragment selects the grammar's fragment mode, which
// accepts declarations and statements in any order, as examples show them.
void mo_lexer_init(struct lexer *lx, const char *path, const unsigned char *src, size_t len,
                   int first_line, bool fragment, struct arena *arena);

// Advice for a word other languages use where Mo has another spelling, such as
// def or elif, or NULL. These words stay usable as names; the advice is added
// only to a syntax error at one of them.
const char *mo_other_spelling(const unsigned char *text, size_t length);

// Reports an error at a position, once per source: the first error stops the parse.
void mo_lexer_error(struct lexer *lx, int line, int col, const char *format, ...)
    __attribute__((format(printf, 4, 5)));

#endif
