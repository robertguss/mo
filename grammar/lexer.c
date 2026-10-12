// The lexer for Mo's executable grammar. See lexer.h, and the reference's
// section on source text for the rules it implements.
#include "lexer.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ast.h"
#include "mo.tab.h"

void mo_lexer_init(struct lexer *lx, const char *path, const unsigned char *src, size_t len,
                   int first_line, bool fragment, struct arena *arena) {
    MO_ASSERT(lx != NULL && path != NULL && (src != NULL || len == 0) && arena != NULL);
    memset(lx, 0, sizeof *lx);
    lx->path = path;
    lx->src = src;
    lx->len = len;
    lx->line = first_line;
    lx->col = 1;
    lx->start_token = fragment ? START_FRAGMENT : START_FILE;
    lx->arena = arena;
}

void mo_lexer_error(struct lexer *lx, int line, int col, const char *format, ...) {
    lx->errors += 1;
    if (lx->errors > 1) {
        return;
    }
    char message[400];
    va_list args;
    va_start(args, format);
    int written = vsnprintf(message, sizeof message, format, args);
    va_end(args);
    if (written < 0) {
        (void)snprintf(message, sizeof message, "(the error message could not be formatted)");
    }
    (void)snprintf(lx->first_error, sizeof lx->first_error, "%s:%d:%d: %s", lx->path, line, col,
                   message);
    if (!lx->quiet) {
        (void)fprintf(stderr, "%s\n", lx->first_error);
    }
}

// Decodes the UTF-8 sequence at src[pos] into *cp and returns its length, or
// returns 0 when the bytes there are not valid UTF-8.
static size_t decode(const unsigned char *src, size_t len, size_t pos, uint32_t *cp) {
    unsigned char first = src[pos];
    if (first < 0x80u) {
        *cp = first;
        return 1;
    }
    size_t length;
    uint32_t value;
    uint32_t least;
    if ((first & 0xE0u) == 0xC0u) {
        length = 2;
        value = first & 0x1Fu;
        least = 0x80u;
    } else if ((first & 0xF0u) == 0xE0u) {
        length = 3;
        value = first & 0x0Fu;
        least = 0x800u;
    } else if ((first & 0xF8u) == 0xF0u) {
        length = 4;
        value = first & 0x07u;
        least = 0x10000u;
    } else {
        return 0;
    }
    if (len - pos < length) {
        return 0;
    }
    for (size_t i = 1; i < length; i++) {
        unsigned char next = src[pos + i];
        if ((next & 0xC0u) != 0x80u) {
            return 0;
        }
        value = (value << 6) | (next & 0x3Fu);
    }
    if (value < least || value > 0x10FFFFu || (value >= 0xD800u && value <= 0xDFFFu)) {
        return 0;
    }
    *cp = value;
    return length;
}

// Characters that change how text looks without being seen: control
// characters, direction overrides and isolates, zero-width characters and other
// formatting characters. Mo rejects them everywhere, even in text and comments,
// so that code reads as it runs ("Trojan Source", CVE-2021-42574).
static bool invisible(uint32_t c) {
    if (c < 0x20u) {
        return c != '\t' && c != '\n' && c != '\r';
    }
    return c == 0x7Fu || (c >= 0x80u && c <= 0x9Fu) || c == 0xADu || c == 0x61Cu || c == 0x180Eu ||
           (c >= 0x200Bu && c <= 0x200Fu) || c == 0x2028u || c == 0x2029u ||
           (c >= 0x202Au && c <= 0x202Eu) || (c >= 0x2060u && c <= 0x2064u) ||
           (c >= 0x2066u && c <= 0x206Fu) || c == 0xFEFFu || (c >= 0xFFF9u && c <= 0xFFFBu) ||
           c == 0xE0001u || (c >= 0xE0020u && c <= 0xE007Fu);
}

// Checks the whole source before lexing: valid UTF-8, nothing invisible, and
// no carriage return outside a CRLF line ending.
static bool check_characters(struct lexer *lx) {
    int line = lx->line;
    int col = 1;
    size_t pos = 0;
    while (pos < lx->len) {
        uint32_t c = 0;
        size_t length = decode(lx->src, lx->len, pos, &c);
        if (length == 0) {
            mo_lexer_error(lx, line, col, "this is not valid UTF-8; Mo source is UTF-8");
            return false;
        }
        if (c == '\r' && (pos + 1 >= lx->len || lx->src[pos + 1] != '\n')) {
            mo_lexer_error(lx, line, col, "a carriage return outside a line ending");
            return false;
        }
        if (invisible(c)) {
            mo_lexer_error(lx, line, col,
                           "U+%04X is an invisible character, which Mo doesn't allow anywhere in "
                           "source; in text, write it as \\u{%X}",
                           (unsigned)c, (unsigned)c);
            return false;
        }
        pos += length;
        if (c == '\n') {
            line += 1;
            col = 1;
        } else if (c != '\r') {
            col += 1;
        }
    }
    return true;
}

static int peek(const struct lexer *lx, size_t ahead) {
    size_t at = lx->pos + ahead;
    return at < lx->len ? lx->src[at] : -1;
}

// Moves past one character, keeping the line and column.
static void advance(struct lexer *lx) {
    MO_ASSERT(lx->pos < lx->len);
    uint32_t c = 0;
    size_t length = decode(lx->src, lx->len, lx->pos, &c);
    MO_ASSERT(length > 0); // check_characters ran first
    lx->pos += length;
    if (c == '\n') {
        lx->line += 1;
        lx->col = 1;
    } else if (c != '\r') {
        lx->col += 1;
    }
}

static bool is_lower(int c) {
    return c >= 'a' && c <= 'z';
}
static bool is_upper(int c) {
    return c >= 'A' && c <= 'Z';
}
static bool is_digit(int c) {
    return c >= '0' && c <= '9';
}
static bool is_alnum(int c) {
    return is_lower(c) || is_upper(c) || is_digit(c);
}

static const struct {
    const char *text;
    int token;
} keywords[] = {
    {"allows", ALLOWS},   {"and", AND},
    {"assert", ASSERT},   {"break", BREAK},
    {"case", CASE},       {"continue", CONTINUE},
    {"else", ELSE},       {"end", END},
    {"enum", ENUM},       {"examples", EXAMPLES},
    {"expose", EXPOSE},   {"false", FALSE},
    {"fn", FN},           {"for", FOR},
    {"forbids", FORBIDS}, {"if", IF},
    {"implies", IMPLIES}, {"in", IN},
    {"is", IS},           {"module", MODULE},
    {"not", NOT},         {"or", OR},
    {"promise", PROMISE}, {"return", RETURN},
    {"stop", STOP},       {"struct", STRUCT},
    {"test", TEST},       {"true", TRUE},
    {"try", TRY},         {"use", USE},
    {"var", VAR},         {"where", WHERE},
    {"while", WHILE},
};

static int keyword(const unsigned char *text, size_t length) {
    for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++) {
        if (strlen(keywords[i].text) == length && memcmp(keywords[i].text, text, length) == 0) {
            return keywords[i].token;
        }
    }
    return 0;
}

const char *mo_other_spelling(const unsigned char *text, size_t length) {
    static const struct {
        const char *word;
        const char *advice;
    } words[] = {
        {"def", "Mo defines a function with fn"},
        {"func", "Mo defines a function with fn"},
        {"function", "Mo defines a function with fn"},
        {"fun", "Mo defines a function with fn"},
        {"lambda", "a function value is written fn(x) x + 1 end"},
        {"do", "a function value is written fn(x) x + 1 end"},
        {"elif", "Mo writes else if"},
        {"elsif", "Mo writes else if"},
        {"then", "an if needs no then: the condition, a line break, the body, and end"},
        {"unless", "Mo writes if not"},
        {"until", "Mo writes while not"},
        {"loop", "Mo loops with for and while"},
        {"match", "Mo matches with case"},
        {"switch", "Mo matches with case"},
        {"when", "Mo matches with case, with arms written Pattern: value"},
        {"let", "Mo binds a name with name = value, and a changeable one with var"},
        {"const", "Mo binds a name with name = value, and a changeable one with var"},
        {"mut", "Mo binds a name with name = value, and a changeable one with var"},
        {"nil", "Mo has no nil: a value that may be missing is an Option, Some(x) or None"},
        {"null", "Mo has no null: a value that may be missing is an Option, Some(x) or None"},
        {"self", "Mo has no self: a function takes the value it works on as a parameter"},
        {"this", "Mo has no this: a function takes the value it works on as a parameter"},
        {"class", "Mo has no classes: data is a struct or an enum, with functions in its module"},
        {"interface", "Mo has no interfaces: see the built-in abilities, such as Order"},
        {"trait", "Mo has no user-defined traits: see the built-in abilities, such as Order"},
        {"impl", "a type customizes an ability with one function, such as to_text"},
        {"new", "a record is built by naming its fields, as in Point(x: 1, y: 2)"},
        {"import", "Mo imports with use"},
        {"export", "a module lists its public names on its expose line"},
        {"pub", "a module lists its public names on its expose line"},
        {"throw", "Mo has no exceptions: a function that can fail returns a Result"},
        {"raise", "Mo has no exceptions: a function that can fail returns a Result"},
        {"catch", "Mo has no exceptions: handle a Result with case"},
        {"rescue", "Mo has no exceptions: handle a Result with case"},
    };
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) {
        if (strlen(words[i].word) == length && memcmp(words[i].word, text, length) == 0) {
            return words[i].advice;
        }
    }
    return NULL;
}

static struct node *leaf(struct lexer *lx, enum node_kind kind, int token, int line, int col) {
    struct node *n = mo_node(lx, kind, line, col, 0, NULL);
    n->op = token;
    return n;
}

// Reads the digits of a number in the given base, with `_` allowed only between
// two digits. Returns false after reporting an error.
static bool digits(struct lexer *lx, int base, int line, int col) {
    bool any = false;
    bool after_underscore = false;
    for (;;) {
        int c = peek(lx, 0);
        bool digit = base == 16  ? (is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
                     : base == 2 ? (c == '0' || c == '1')
                                 : is_digit(c);
        if (digit) {
            any = true;
            after_underscore = false;
            advance(lx);
        } else if (c == '_') {
            if (!any || after_underscore) {
                mo_lexer_error(lx, lx->line, lx->col, "_ in a number goes between two digits");
                return false;
            }
            after_underscore = true;
            advance(lx);
        } else {
            break;
        }
    }
    if (!any) {
        mo_lexer_error(lx, line, col, "a number needs at least one digit here");
        return false;
    }
    if (after_underscore) {
        mo_lexer_error(lx, lx->line, lx->col - 1, "_ in a number goes between two digits");
        return false;
    }
    return true;
}

static int number(struct lexer *lx, struct node **value, int line, int col) {
    int token = INT;
    if (lx->last == DOT) {
        // A tuple position, as in pair.0: digits only.
        while (is_digit(peek(lx, 0))) {
            advance(lx);
        }
    } else if (peek(lx, 0) == '0' && (peek(lx, 1) == 'x' || peek(lx, 1) == 'b')) {
        int base = peek(lx, 1) == 'x' ? 16 : 2;
        advance(lx);
        advance(lx);
        if (!digits(lx, base, line, col)) {
            return YYerror;
        }
    } else {
        bool leading_zero = peek(lx, 0) == '0';
        if (!digits(lx, 10, line, col)) {
            return YYerror;
        }
        if (leading_zero && lx->col - col > 1) {
            mo_lexer_error(lx, line, col, "a whole number doesn't start with 0");
            return YYerror;
        }
        if (peek(lx, 0) == '.' && is_digit(peek(lx, 1))) {
            advance(lx);
            if (!digits(lx, 10, line, col)) {
                return YYerror;
            }
            token = DECIMAL;
        }
    }
    int next = peek(lx, 0);
    if (is_alnum(next) || next == '_') {
        mo_lexer_error(lx, lx->line, lx->col,
                       "a number can't be followed directly by a letter; Mo has no number suffixes "
                       "or exponents");
        return YYerror;
    }
    *value = leaf(lx, token == INT ? K_INT : K_DECIMAL, token, line, col);
    return token;
}

static int name(struct lexer *lx, struct node **value, int line, int col) {
    size_t start = lx->pos;
    bool upper_inside = false;
    while (is_alnum(peek(lx, 0)) || peek(lx, 0) == '_') {
        upper_inside = upper_inside || is_upper(peek(lx, 0));
        advance(lx);
    }
    bool question = peek(lx, 0) == '?';
    if (question) {
        advance(lx);
    }
    const unsigned char *text = lx->src + start;
    size_t length = lx->pos - start;
    if (length == 1 && text[0] == '_') {
        *value = leaf(lx, K_UNDERSCORE, UNDERSCORE, line, col);
        return UNDERSCORE;
    }
    if (upper_inside) {
        mo_lexer_error(lx, line, col,
                       "a name is written in snake_case: lowercase letters, digits and _");
        return YYerror;
    }
    int token = question ? 0 : keyword(text, length);
    if (token != 0) {
        enum node_kind kind = token == TRUE ? K_TRUE : token == FALSE ? K_FALSE : K_TOKEN;
        *value = leaf(lx, kind, token, line, col);
        return token;
    }
    *value = leaf(lx, K_NAME, NAME, line, col);
    return NAME;
}

static int capname(struct lexer *lx, struct node **value, int line, int col) {
    while (is_alnum(peek(lx, 0))) {
        advance(lx);
    }
    if (peek(lx, 0) == '_') {
        mo_lexer_error(lx, line, col,
                       "a capitalized name is written in CapCase, with no _: types, modules and "
                       "cases");
        return YYerror;
    }
    if (peek(lx, 0) == '?') {
        mo_lexer_error(lx, lx->line, lx->col, "only a function's name can end in ?");
        return YYerror;
    }
    *value = leaf(lx, K_CAPNAME, CAPNAME, line, col);
    return CAPNAME;
}

// Reads an escape after its backslash. Text and characters share one set.
static bool escape(struct lexer *lx) {
    int line = lx->line;
    int col = lx->col - 1;
    int c = peek(lx, 0);
    if (c == 'n' || c == 't' || c == 'r' || c == '\\' || c == '"' || c == '\'' || c == '#') {
        advance(lx);
        return true;
    }
    if (c == 'u' && peek(lx, 1) == '{') {
        advance(lx);
        advance(lx);
        uint32_t value = 0;
        int count = 0;
        for (;;) {
            int h = peek(lx, 0);
            int digit = is_digit(h)              ? h - '0'
                        : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                        : (h >= 'A' && h <= 'F') ? h - 'A' + 10
                                                 : -1;
            if (digit < 0) {
                break;
            }
            value = value * 16u + (uint32_t)digit;
            count += 1;
            advance(lx);
            if (count > 6) {
                break;
            }
        }
        if (count == 0 || count > 6 || peek(lx, 0) != '}' || value > 0x10FFFFu ||
            (value >= 0xD800u && value <= 0xDFFFu)) {
            mo_lexer_error(
                lx, line, col,
                "\\u{...} holds 1 to 6 hexadecimal digits naming a Unicode scalar value");
            return false;
        }
        advance(lx);
        return true;
    }
    mo_lexer_error(lx, line, col,
                   "unknown escape; text and characters use \\n, \\t, \\r, \\\\, \\\", \\', \\# "
                   "and \\u{...}");
    return false;
}

// Reads text up to its end or to the next `#{`. The opening quote, or the `}`
// ending an interpolation, has been read. start says which: it decides between
// TEXT and TEXT_HEAD, or TEXT_TAIL and TEXT_MID.
static int text_body(struct lexer *lx, struct node **value, int line, int col, bool start,
                     bool triple, size_t indent, bool line_start) {
    for (;;) {
        if (lx->pos >= lx->len) {
            mo_lexer_error(lx, line, col, "this text has no closing quote");
            return YYerror;
        }
        if (triple && line_start) {
            line_start = false;
            size_t spaces = 0;
            while (peek(lx, (size_t)spaces) == ' ') {
                spaces += 1;
            }
            if (peek(lx, spaces) == '"' && peek(lx, spaces + 1) == '"' &&
                peek(lx, spaces + 2) == '"') {
                for (size_t i = 0; i < spaces + 3; i++) {
                    advance(lx);
                }
                break;
            }
            int after = peek(lx, spaces);
            bool blank = after == '\n' || after == '\r' || after == -1;
            if (!blank && spaces < indent) {
                mo_lexer_error(lx, lx->line, lx->col,
                               "a line of \"\"\" text is indented less than its closing \"\"\"");
                return YYerror;
            }
            continue;
        }
        int c = peek(lx, 0);
        if (!triple && c == '"') {
            advance(lx);
            break;
        }
        if (c == '\n' || c == '\r') {
            if (!triple) {
                mo_lexer_error(
                    lx, line, col,
                    "text ends on the line it starts; write \\n for a line break, or use "
                    "\"\"\" for text on several lines");
                return YYerror;
            }
            advance(lx);
            if (c == '\r') {
                advance(lx);
            }
            line_start = true;
            continue;
        }
        if (c == '\\') {
            advance(lx);
            if (!escape(lx)) {
                return YYerror;
            }
            continue;
        }
        if (c == '#' && peek(lx, 1) == '{') {
            advance(lx);
            advance(lx);
            if (start) {
                if (lx->depth == MO_LEXER_MAX_INTERPOLATION) {
                    mo_lexer_error(lx, line, col, "interpolations are nested too deeply");
                    return YYerror;
                }
                lx->interpolations[lx->depth] =
                    (struct mo_interpolation){.braces = 0, .triple = triple, .indent = indent};
                lx->depth += 1;
            }
            *value = leaf(lx, K_TEXT, start ? TEXT_HEAD : TEXT_MID, line, col);
            return start ? TEXT_HEAD : TEXT_MID;
        }
        advance(lx);
    }
    if (!start) {
        MO_ASSERT(lx->depth > 0);
        lx->depth -= 1;
    }
    *value = leaf(lx, K_TEXT, start ? TEXT : TEXT_TAIL, line, col);
    return start ? TEXT : TEXT_TAIL;
}

static int text(struct lexer *lx, struct node **value, int line, int col) {
    if (peek(lx, 1) == '"' && peek(lx, 2) == '"') {
        advance(lx);
        advance(lx);
        advance(lx);
        while (peek(lx, 0) == ' ' || peek(lx, 0) == '\t') {
            advance(lx);
        }
        if (peek(lx, 0) != '\n' && peek(lx, 0) != '\r') {
            mo_lexer_error(lx, line, col,
                           "\"\"\" text starts on the line after its opening \"\"\"");
            return YYerror;
        }
        // The closing """ sets the indentation every line of the text drops.
        size_t at = lx->pos;
        size_t indent = 0;
        bool found = false;
        while (at < lx->len && !found) {
            while (at < lx->len && lx->src[at] != '\n') {
                at += 1;
            }
            if (at >= lx->len) {
                break;
            }
            at += 1;
            size_t spaces = 0;
            while (at + spaces < lx->len && lx->src[at + spaces] == ' ') {
                spaces += 1;
            }
            if (at + spaces + 2 < lx->len && lx->src[at + spaces] == '"' &&
                lx->src[at + spaces + 1] == '"' && lx->src[at + spaces + 2] == '"') {
                indent = spaces;
                found = true;
            }
        }
        if (!found) {
            mo_lexer_error(lx, line, col, "this \"\"\" text has no closing \"\"\"");
            return YYerror;
        }
        if (peek(lx, 0) == '\r') {
            advance(lx);
        }
        advance(lx); // the line break after the opening """
        return text_body(lx, value, line, col, true, true, indent, true);
    }
    advance(lx);
    return text_body(lx, value, line, col, true, false, 0, false);
}

static int character(struct lexer *lx, struct node **value, int line, int col) {
    advance(lx); // the opening quote
    int c = peek(lx, 0);
    if (c == '\'') {
        mo_lexer_error(lx, line, col, "a character literal holds exactly one character");
        return YYerror;
    }
    if (c == '\n' || c == '\r' || c == -1) {
        mo_lexer_error(lx, line, col, "this character literal has no closing '");
        return YYerror;
    }
    if (c == '\\') {
        advance(lx);
        if (!escape(lx)) {
            return YYerror;
        }
    } else {
        advance(lx);
    }
    if (peek(lx, 0) != '\'') {
        mo_lexer_error(lx, line, col,
                       "a character literal holds exactly one character; text is written in double "
                       "quotes");
        return YYerror;
    }
    advance(lx);
    *value = leaf(lx, K_CHAR, CHAR, line, col);
    return CHAR;
}

// Reads one token, skipping spaces, line breaks and comments before it.
// *newline says whether a line break was skipped, and *nl_line and *nl_col
// where the first one was.
static int scan(struct lexer *lx, struct node **value, int *line, int *col, bool *newline,
                int *nl_line, int *nl_col) {
    *newline = false;
    for (;;) {
        int c = peek(lx, 0);
        if (c == ' ' || c == '\t') {
            advance(lx);
        } else if (c == '\n' || c == '\r') {
            if (!*newline) {
                *newline = true;
                *nl_line = lx->line;
                *nl_col = lx->col;
            }
            advance(lx);
        } else if (c == '#') {
            while (peek(lx, 0) != '\n' && peek(lx, 0) != '\r' && peek(lx, 0) != -1) {
                advance(lx);
            }
        } else {
            break;
        }
    }
    *line = lx->line;
    *col = lx->col;
    lx->token_start = lx->pos;
    int c = peek(lx, 0);
    if (c == -1) {
        *value = NULL;
        return YYEOF;
    }
    if (is_lower(c) || c == '_') {
        return name(lx, value, *line, *col);
    }
    if (is_upper(c)) {
        return capname(lx, value, *line, *col);
    }
    if (is_digit(c)) {
        return number(lx, value, *line, *col);
    }
    if (c == '"') {
        return text(lx, value, *line, *col);
    }
    if (c == '\'') {
        return character(lx, value, *line, *col);
    }
    if (c == '}' && lx->depth > 0 && lx->interpolations[lx->depth - 1].braces == 0) {
        advance(lx);
        const struct mo_interpolation *open = &lx->interpolations[lx->depth - 1];
        return text_body(lx, value, *line, *col, false, open->triple, open->indent, false);
    }
    if (c == '[' && !*newline &&
        (lx->last == NAME || lx->last == CAPNAME || lx->last == RPAREN || lx->last == RBRACK ||
         lx->last == TEXT || lx->last == TEXT_TAIL)) {
        mo_lexer_error(lx, *line, *col,
                       "Mo has no indexing with [...]: items.get(i) and map.get(key) return an "
                       "Option");
        return YYerror;
    }
    int next = peek(lx, 1);
    int token = 0;
    int length = 1;
    const char *problem = NULL;
    switch (c) {
    case '(':
        token = LPAREN;
        break;
    case ')':
        token = RPAREN;
        break;
    case '[':
        token = LBRACK;
        break;
    case ']':
        token = RBRACK;
        break;
    case '{':
        token = LBRACE;
        break;
    case '}':
        token = RBRACE;
        break;
    case ',':
        token = COMMA;
        break;
    case ':':
        token = COLON;
        if (next == ':') {
            problem = "Mo separates a module from a name with ., not ::";
        }
        break;
    case '.':
        token = next == '.' ? DOTDOT : DOT;
        length = next == '.' ? 2 : 1;
        if (next == '.' && peek(lx, 2) == '.') {
            problem = "Mo writes ranges with .., and has no ...";
        }
        break;
    case '=':
        token = next == '=' ? EQ : ASSIGN;
        length = next == '=' ? 2 : 1;
        if (next == '>') {
            problem = "a case arm is written Pattern: value, with no =>";
        }
        break;
    case '!':
        token = NE;
        length = 2;
        if (next != '=') {
            problem = "Mo writes not instead of !";
        }
        break;
    case '<':
        token = next == '=' ? LE : LT;
        length = next == '=' ? 2 : 1;
        break;
    case '>':
        token = next == '=' ? GE : GT;
        length = next == '=' ? 2 : 1;
        break;
    case '+':
        token = next == '=' ? PLUSEQ : PLUS;
        length = next == '=' ? 2 : 1;
        break;
    case '-':
        token = next == '=' ? MINUSEQ : MINUS;
        length = next == '=' ? 2 : 1;
        if (next == '>') {
            problem = "a function's result type follows :, not ->";
        }
        break;
    case '*':
        token = STAR;
        if (next == '*') {
            problem = "Mo has no ** operator";
        } else if (next == '=') {
            problem = "Mo has += and -=, and no other compound assignment";
        }
        break;
    case '/':
        token = SLASH;
        if (next == '/') {
            problem = "comments start with #";
        } else if (next == '=') {
            problem = "Mo has += and -=, and no other compound assignment";
        }
        break;
    case '%':
        token = PERCENT;
        break;
    case '|':
        token = BAR;
        if (next == '|') {
            problem = "Mo writes or instead of ||";
        } else if (next == '>') {
            problem = "Mo has no pipe operator; chain calls with dots, as in text.trim.lines";
        }
        break;
    case '&':
        problem = next == '&' ? "Mo writes and instead of &&" : "Mo has no & operator";
        break;
    case ';':
        problem = "a line break ends a statement; Mo has no semicolons";
        break;
    case '?':
        problem = "? only ends a predicate's name, such as empty?; a choice on one line is "
                  "if c: a else: b, and try passes a failure upward";
        break;
    default:
        if (c >= 0x80) {
            problem = "code is written in ASCII; other characters are allowed in text and comments";
        } else {
            problem = "this character isn't part of Mo";
        }
        break;
    }
    if (problem != NULL) {
        mo_lexer_error(lx, *line, *col, "%s", problem);
        return YYerror;
    }
    if (lx->depth > 0) {
        if (token == LBRACE) {
            lx->interpolations[lx->depth - 1].braces += 1;
        } else if (token == RBRACE) {
            lx->interpolations[lx->depth - 1].braces -= 1;
        }
    }
    for (int i = 0; i < length; i++) {
        advance(lx);
    }
    *value = leaf(lx, K_TOKEN, token, *line, *col);
    return token;
}

// A line that ends with one of these tokens continues on the next line.
static bool continues_after(int token) {
    switch (token) {
    case LPAREN:
    case LBRACK:
    case LBRACE:
    case COMMA:
    case DOT:
    case DOTDOT:
    case ASSIGN:
    case PLUSEQ:
    case MINUSEQ:
    case EQ:
    case NE:
    case LT:
    case LE:
    case GT:
    case GE:
    case PLUS:
    case MINUS:
    case STAR:
    case SLASH:
    case PERCENT:
    case BAR:
    case AND:
    case OR:
    case NOT:
    case IMPLIES:
    case IS:
    case TRY:
    case WHERE:
        return true;
    default:
        return false;
    }
}

// A line that starts with one of these tokens continues the line before it.
// `-` is missing on purpose: a line may start with a negative number.
static bool continues_before(int token) {
    switch (token) {
    case RPAREN:
    case RBRACK:
    case RBRACE:
    case DOT:
    case DOTDOT:
    case EQ:
    case NE:
    case LT:
    case LE:
    case GT:
    case GE:
    case PLUS:
    case STAR:
    case SLASH:
    case PERCENT:
    case BAR:
    case AND:
    case OR:
    case IMPLIES:
    case IS:
    case WHERE:
        return true;
    default:
        return false;
    }
}

int yylex(YYSTYPE *value, YYLTYPE *location, struct lexer *lx) {
    if (lx->start_token != 0) {
        int token = lx->start_token;
        lx->start_token = 0;
        if (!check_characters(lx)) {
            return YYerror;
        }
        *value = NULL;
        location->first_line = location->last_line = lx->line;
        location->first_column = location->last_column = 1;
        return token;
    }
    int token;
    struct node *node = NULL;
    int line = 0;
    int col = 0;
    bool has_span = true;
    size_t span_start = 0;
    size_t span_length = 0;
    if (lx->has_pending) {
        lx->has_pending = false;
        token = lx->pending;
        node = lx->pending_value;
        line = lx->pending_line;
        col = lx->pending_col;
        span_start = lx->pending_start;
        span_length = lx->pending_length;
    } else {
        bool newline = false;
        int nl_line = 0;
        int nl_col = 0;
        int depth = lx->depth; // before the token, which may open an interpolation
        token = scan(lx, &node, &line, &col, &newline, &nl_line, &nl_col);
        if (token == YYerror) {
            return YYerror;
        }
        span_start = lx->token_start;
        span_length = lx->pos - lx->token_start;
        bool line_ends = false;
        if (token == YYEOF && lx->depth > 0) {
            mo_lexer_error(lx, line, col, "a #{ in text is never closed with }");
            return YYerror;
        }
        if (token == YYEOF) {
            line_ends = lx->last != 0 && lx->last != NL;
            if (!newline) {
                nl_line = lx->line;
                nl_col = lx->col;
            }
        } else if (newline && depth == 0 && lx->last != 0 && lx->last != NL) {
            line_ends = !continues_after(lx->last) && !continues_before(token);
        }
        if (line_ends) {
            lx->has_pending = true;
            lx->pending = token;
            lx->pending_value = node;
            lx->pending_line = line;
            lx->pending_col = col;
            lx->pending_start = span_start;
            lx->pending_length = span_length;
            has_span = false;
            token = NL;
            node = leaf(lx, K_TOKEN, NL, nl_line, nl_col);
            line = nl_line;
            col = nl_col;
        }
    }
    *value = node;
    location->first_line = location->last_line = line;
    location->first_column = location->last_column = col;
    lx->previous_start = lx->last_start;
    lx->previous_length = lx->last_length;
    if (token == NL || token == YYEOF) {
        lx->last_start = 0;
        lx->last_length = 0;
    } else if (has_span) {
        lx->last_start = span_start;
        lx->last_length = span_length;
    }
    lx->last = token;
    return token;
}
