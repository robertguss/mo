#include "tree_sitter/parser.h"

#include <stdbool.h>
#include <stdint.h>

enum TokenType {
    NEWLINE,
    STRING_CONTENT,
    MULTILINE_STRING_CONTENT,
    MULTILINE_STRING_END,
};

static bool is_ident_cont(int32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '?';
}

static bool match_keyword(TSLexer *lexer, const char *word) {
    for (const char *p = word; *p != '\0'; p++) {
        if (lexer->lookahead != (int32_t)(unsigned char)*p)
            return false;
        lexer->advance(lexer, false);
    }
    return !is_ident_cont(lexer->lookahead);
}

/* Mirrors continues_before() in grammar/lexer.c: tokens at the start of a line
 * that continue the previous logical line rather than terminating it. */
static bool starts_continuation(TSLexer *lexer) {
    int32_t c = lexer->lookahead;
    switch (c) {
    case ')':
    case ']':
    case '}':
    case '.':
    case '+':
    case '*':
    case '/':
    case '%':
    case '|':
    case '<':
    case '>':
        return true;
    case '=':
        lexer->advance(lexer, false);
        return lexer->lookahead == '=';
    case '!':
        lexer->advance(lexer, false);
        return lexer->lookahead == '=';
    case 'a':
        return match_keyword(lexer, "and");
    case 'o':
        return match_keyword(lexer, "or");
    case 'i':
        lexer->advance(lexer, false);
        if (lexer->lookahead == 's') {
            lexer->advance(lexer, false);
            return !is_ident_cont(lexer->lookahead);
        }
        if (lexer->lookahead == 'm')
            return match_keyword(lexer, "mplies");
        return false;
    case 'w':
        return match_keyword(lexer, "where");
    default:
        return false;
    }
}

static bool scan_string_content(TSLexer *lexer) {
    bool any = false;
    while (!lexer->eof(lexer)) {
        int32_t c = lexer->lookahead;
        if (c == '"' || c == '\\' || c == '\n' || c == '\r')
            break;
        if (c == '#') {
            lexer->mark_end(lexer);
            lexer->advance(lexer, false);
            if (lexer->lookahead == '{') {
                lexer->result_symbol = STRING_CONTENT;
                return any;
            }
            any = true;
            lexer->mark_end(lexer);
            continue;
        }
        lexer->advance(lexer, false);
        any = true;
        lexer->mark_end(lexer);
    }
    if (any)
        lexer->result_symbol = STRING_CONTENT;
    return any;
}

static bool scan_multiline_string(TSLexer *lexer, bool allow_content, bool allow_end) {
    bool any = false;
    while (!lexer->eof(lexer)) {
        int32_t c = lexer->lookahead;
        if (c == '\\')
            break;
        if (c == '#') {
            lexer->mark_end(lexer);
            lexer->advance(lexer, false);
            if (lexer->lookahead == '{') {
                if (any && allow_content) {
                    lexer->result_symbol = MULTILINE_STRING_CONTENT;
                    return true;
                }
                return false;
            }
            if (!allow_content)
                return false;
            any = true;
            lexer->mark_end(lexer);
            continue;
        }
        if (c == '"') {
            lexer->mark_end(lexer);
            lexer->advance(lexer, false);
            if (lexer->lookahead == '"') {
                lexer->advance(lexer, false);
                if (lexer->lookahead == '"') {
                    if (any && allow_content) {
                        lexer->result_symbol = MULTILINE_STRING_CONTENT;
                        return true;
                    }
                    if (allow_end) {
                        lexer->advance(lexer, false);
                        lexer->mark_end(lexer);
                        lexer->result_symbol = MULTILINE_STRING_END;
                        return true;
                    }
                    return false;
                }
            }
            if (!allow_content)
                return false;
            any = true;
            lexer->mark_end(lexer);
            continue;
        }
        if (!allow_content)
            return false;
        lexer->advance(lexer, false);
        any = true;
        lexer->mark_end(lexer);
    }
    if (any && allow_content) {
        lexer->result_symbol = MULTILINE_STRING_CONTENT;
        return true;
    }
    return false;
}

void *tree_sitter_mo_external_scanner_create(void) {
    return NULL;
}

void tree_sitter_mo_external_scanner_destroy(void *payload) {
    (void)payload;
}

unsigned tree_sitter_mo_external_scanner_serialize(void *payload, char *buffer) {
    (void)payload;
    (void)buffer;
    return 0;
}

void tree_sitter_mo_external_scanner_deserialize(void *payload, const char *buffer,
                                                 unsigned length) {
    (void)payload;
    (void)buffer;
    (void)length;
}

bool tree_sitter_mo_external_scanner_scan(void *payload, TSLexer *lexer,
                                          const bool *valid_symbols) {
    (void)payload;

    /* Guard against error-recovery mode where every external symbol is marked valid. */
    bool error_recovery = valid_symbols[NEWLINE] && valid_symbols[STRING_CONTENT] &&
                          valid_symbols[MULTILINE_STRING_CONTENT] &&
                          valid_symbols[MULTILINE_STRING_END];
    if (!error_recovery) {
        if (valid_symbols[STRING_CONTENT])
            return scan_string_content(lexer);
        if (valid_symbols[MULTILINE_STRING_CONTENT] || valid_symbols[MULTILINE_STRING_END])
            return scan_multiline_string(lexer, valid_symbols[MULTILINE_STRING_CONTENT],
                                         valid_symbols[MULTILINE_STRING_END]);
    }

    if (!valid_symbols[NEWLINE])
        return false;

    while (lexer->lookahead == ' ' || lexer->lookahead == '\t')
        lexer->advance(lexer, true);

    /* Let Tree-sitter's extras parse # and ## comments as real AST nodes first;
     * the scanner will be invoked again at the '\n' immediately after the comment. */
    if (lexer->lookahead == '#')
        return false;

    if (lexer->lookahead != '\n' && lexer->lookahead != '\r')
        return false;

    if (lexer->lookahead == '\r')
        lexer->advance(lexer, false);
    if (lexer->lookahead != '\n')
        return false;
    lexer->advance(lexer, false);
    lexer->mark_end(lexer);

    for (;;) {
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t' || lexer->lookahead == '\r' ||
               lexer->lookahead == '\n')
            lexer->advance(lexer, false);
        if (lexer->lookahead == '#') {
            while (!lexer->eof(lexer) && lexer->lookahead != '\n')
                lexer->advance(lexer, false);
            continue;
        }
        break;
    }

    if (starts_continuation(lexer))
        return false;

    lexer->result_symbol = NEWLINE;
    return true;
}
