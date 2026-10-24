/* Token spelling and logical lines follow grammar/lexer.c. Keep the two
 * implementations independent: the differential target checks their verdicts. */
#include "base/codes.h"
#include "syntax/internal.h"

#include <stdio.h>
#include <string.h>

void mo_syntax_error(struct mo_lex *lx, uint16_t code, struct mo_span span, const char *message,
                     const char *reason) {
    bool allocation = code == MO_CODE_SYNTAX_NO_MEMORY;
    if ((lx->failed || lx->fatal) && !allocation)
        return;
    lx->failed = true;
    if (allocation)
        lx->fatal = true;
    if (lx->diags->count != 0) {
        const struct mo_diag *last = &lx->diags->items[lx->diags->count - 1];
        if ((allocation && last->code == code) ||
            (!allocation && last->primary.span.file == span.file &&
             last->primary.span.start == span.start && last->primary.span.end == span.end))
            return;
    }
    if (lx->recovering && !allocation)
        return;
    if (code >= MO_CODE_LINE_TOO_LONG && code <= MO_CODE_TOO_MANY_CASES) {
        uint8_t bit = (uint8_t)(1u << (code - MO_CODE_LINE_TOO_LONG));
        if ((lx->limits_reported & bit) != 0)
            return;
        lx->limits_reported |= bit;
    }
    struct mo_diag diag = {
        .code = code, .primary = {span, NULL}, .message = message, .reason = reason};
    if (!mo_diags_add(lx->diags, &diag))
        lx->fatal = true;
}

void mo_syntax_lex_init(struct mo_lex *lx, struct mo_arena *arena, const struct mo_sources *sources,
                        uint32_t file, struct mo_diags *diags) {
    MO_ASSERT(file < sources->count);
    *lx = (struct mo_lex){.source = &sources->files[file],
                          .file = file,
                          .line = 1,
                          .column = 1,
                          .arena = arena,
                          .diags = diags};
    lx->comment_tail = &lx->comments;
}

static MO_MUST_USE int peek(const struct mo_lex *lx, uint32_t ahead) {
    return ahead < lx->source->length - lx->pos ? lx->source->bytes[lx->pos + ahead] : -1;
}

uint32_t mo_syntax_decode(const uint8_t *bytes, uint32_t length, uint32_t *cp) {
    if (length == 0)
        return 0;
    uint32_t c = bytes[0];
    uint32_t n = c < 128              ? 1
                 : (c & 224u) == 192u ? 2
                 : (c & 240u) == 224u ? 3
                 : (c & 248u) == 240u ? 4
                                      : 0;
    if (n == 0 || n > length)
        return 0;
    uint32_t value = c & (n == 1 ? 127u : n == 2 ? 31u : n == 3 ? 15u : 7u);
    for (uint32_t i = 1; i < n; i++) {
        if ((bytes[i] & 192u) != 128u)
            return 0;
        value = (value << 6) | (bytes[i] & 63u);
    }
    if ((n == 2 && value < 128) || (n == 3 && value < 2048) || (n == 4 && value < 65536) ||
        value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
        return 0;
    *cp = value;
    return n;
}

static MO_MUST_USE bool invisible(uint32_t c) {
    return (c < 32 && c != 9 && c != 10 && c != 13) || c == 127 || (c >= 128 && c <= 159) ||
           c == 0xad || c == 0x61c || c == 0x180e || (c >= 0x200b && c <= 0x200f) ||
           (c >= 0x2028 && c <= 0x202e) || (c >= 0x2060 && c <= 0x2064) ||
           (c >= 0x2066 && c <= 0x206f) || c == 0xfeff || (c >= 0xfff9 && c <= 0xfffb) ||
           c == 0xe0001 || (c >= 0xe0020 && c <= 0xe007f);
}

static void lex_error_from(struct mo_lex *lx, uint16_t code, const char *message, uint32_t from) {
    /* A span that starts before the current byte marks the whole token up to it. */
    uint32_t end = from < lx->pos ? lx->pos : lx->pos < lx->source->length ? lx->pos + 1 : lx->pos;
    struct mo_span span = {lx->file, from, end};
    const struct mo_code_info *info = mo_code_find(code);
    MO_ASSERT(info != NULL);
    const char *rule =
        info->section == NULL ? NULL
        : code == MO_CODE_SOURCE_UTF8 || code == MO_CODE_SOURCE_INVISIBLE
            ? "Source is UTF-8; invisible characters are an error anywhere"
        : code == MO_CODE_SOURCE_NAME   ? "Names are ASCII, in snake_case or CapCase"
        : code == MO_CODE_SOURCE_NUMBER ? "Whole numbers may put _ between digits"
        : code == MO_CODE_SOURCE_TEXT
            ? "Text and characters use the listed escapes and Unicode scalar values"
        : code == MO_CODE_SOURCE_INDEX ? "List positions are read with get, which returns an Option"
        : code == MO_CODE_SOURCE_BOOLEAN ? "Boolean expressions use and, or and not"
        : code == MO_CODE_SOURCE_FUNCTION
            ? "A function signature uses fn and : before its result type"
        : code == MO_CODE_SOURCE_ASSIGNMENT    ? "Compound assignment uses only += and -="
        : code == MO_CODE_SOURCE_QUALIFICATION ? "Module-qualified names use ."
        : code == MO_CODE_SOURCE_COMMENT       ? "Comments start with #"
        : code == MO_CODE_SOURCE_OPERATOR      ? "Use Mo's arithmetic operators"
        : code == MO_CODE_SOURCE_CALL          ? "Dot calls chain with ."
        : code == MO_CODE_SOURCE_CASE          ? "A case arm is Pattern: value"
                                               : "A line break ends a statement; use Mo's tokens";
    char reason[256];
    if (rule != NULL) {
        int n = snprintf(reason, sizeof reason, "%s (reference %s).", rule, info->section);
        MO_ASSERT(n >= 0 && (size_t)n < sizeof reason);
    }
    mo_syntax_error(lx, code, span, message, rule == NULL ? NULL : reason);
}

static void lex_error(struct mo_lex *lx, uint16_t code, const char *message) {
    lex_error_from(lx, code, message, lx->pos);
}

static MO_MUST_USE bool advance(struct mo_lex *lx) {
    MO_ASSERT(lx->pos < lx->source->length);
    uint32_t c = 0;
    uint32_t n = mo_syntax_decode(lx->source->bytes + lx->pos, lx->source->length - lx->pos, &c);
    if (n == 0) {
        lex_error(lx, MO_CODE_SOURCE_UTF8, "source is UTF-8");
        lx->pos++;
        return false;
    }
    if (invisible(c) || (c == 13 && peek(lx, 1) != 10)) {
        lex_error(lx, MO_CODE_SOURCE_INVISIBLE, "invisible characters are an error anywhere");
        lx->pos += n;
        return false;
    }
    lx->pos += n;
    if (c == 10) {
        lx->line++;
        lx->column = 1;
    } else if (c != 13)
        lx->column++;
    if (lx->column > MO_LIMIT_LINE_LENGTH + 1u) {
        lex_error(lx, MO_CODE_LINE_TOO_LONG, "a source line is too long");
        return false;
    }
    return true;
}

static MO_MUST_USE bool lower(int c) {
    return c >= 'a' && c <= 'z';
}
static MO_MUST_USE bool upper(int c) {
    return c >= 'A' && c <= 'Z';
}
static MO_MUST_USE bool digit(int c) {
    return c >= '0' && c <= '9';
}
static MO_MUST_USE bool alnum(int c) {
    return lower(c) || upper(c) || digit(c);
}

static const struct {
    const char *text;
    enum mo_syntax_token kind;
} words[] = {
    {"module", MO_T_MODULE},
    {"expose", MO_T_EXPOSE},
    {"use", MO_T_USE},
    {"fn", MO_T_FN},
    {"struct", MO_T_STRUCT},
    {"enum", MO_T_ENUM},
    {"var", MO_T_VAR},
    {"if", MO_T_IF},
    {"else", MO_T_ELSE},
    {"case", MO_T_CASE},
    {"for", MO_T_FOR},
    {"in", MO_T_IN},
    {"while", MO_T_WHILE},
    {"break", MO_T_BREAK},
    {"continue", MO_T_CONTINUE},
    {"return", MO_T_RETURN},
    {"try", MO_T_TRY},
    {"and", MO_T_AND},
    {"or", MO_T_OR},
    {"not", MO_T_NOT},
    {"is", MO_T_IS},
    {"implies", MO_T_IMPLIES},
    {"true", MO_T_TRUE},
    {"false", MO_T_FALSE},
    {"stop", MO_T_STOP},
    {"promise", MO_T_PROMISE},
    {"allows", MO_T_ALLOWS},
    {"forbids", MO_T_FORBIDS},
    {"examples", MO_T_EXAMPLES},
    {"test", MO_T_TEST},
    {"assert", MO_T_ASSERT},
    {"where", MO_T_WHERE},
    {"end", MO_T_END},
};

static MO_MUST_USE bool digits(struct mo_lex *lx, int base) {
    bool any = false, underscore = false;
    for (;;) {
        int c = peek(lx, 0);
        bool valid = base == 16  ? digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')
                     : base == 2 ? c == '0' || c == '1'
                                 : digit(c);
        if (valid) {
            any = true;
            underscore = false;
        } else if (c == '_' && any && !underscore)
            underscore = true;
        else
            break;
        if (!advance(lx))
            return false;
    }
    if (!any || underscore || peek(lx, 0) == '_') {
        lex_error(lx, MO_CODE_SOURCE_NUMBER, "whole numbers put _ between digits");
        return false;
    }
    return true;
}

static MO_MUST_USE bool escape(struct mo_lex *lx) {
    int c = peek(lx, 0);
    if (c == 'n' || c == 't' || c == 'r' || c == '\\' || c == '"' || c == '\'' || c == '#')
        return advance(lx);
    if (c == 'u' && peek(lx, 1) == '{') {
        if (!advance(lx) || !advance(lx))
            return false;
        uint32_t value = 0, count = 0;
        for (;;) {
            c = peek(lx, 0);
            int d = digit(c)               ? c - '0'
                    : c >= 'a' && c <= 'f' ? c - 'a' + 10
                    : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                           : -1;
            if (d < 0)
                break;
            value = value * 16u + (uint32_t)d;
            count++;
            if (!advance(lx))
                return false;
            if (count > 6)
                break;
        }
        if (count > 0 && count <= 6 && value <= 0x10ffff && !(value >= 0xd800 && value <= 0xdfff) &&
            peek(lx, 0) == '}')
            return advance(lx);
    }
    lex_error(lx, MO_CODE_SOURCE_TEXT,
              "unknown escape; Mo has \\n, \\t, \\r, \\\\, \\\", \\', \\# and \\u{...}");
    return false;
}

static MO_MUST_USE enum mo_syntax_token text_body(struct mo_lex *lx, bool start, bool triple,
                                                  uint32_t indent, bool line_start) {
    while (peek(lx, 0) != -1) {
        if (triple && line_start) {
            line_start = false;
            uint32_t spaces = 0;
            while (peek(lx, spaces) == ' ')
                spaces++;
            if (peek(lx, spaces) == '"' && peek(lx, spaces + 1) == '"' &&
                peek(lx, spaces + 2) == '"') {
                for (uint32_t i = 0; i < spaces + 3; i++)
                    if (!advance(lx))
                        return MO_T_ERROR;
                if (!start)
                    lx->depth--;
                lx->recovery_end = 0;
                return start ? MO_T_TEXT : MO_T_TEXT_TAIL;
            }
            int after = peek(lx, spaces);
            if (after != 10 && after != 13 && after != -1 && spaces < indent) {
                (void)lex_error(
                    lx, MO_CODE_SOURCE_TEXT,
                    "a non-empty line has less indentation than the closing triple quote");
                return MO_T_ERROR;
            }
        }
        int c = peek(lx, 0);
        if (!triple && c == '"') {
            if (!advance(lx))
                return MO_T_ERROR;
            if (!start)
                lx->depth--;
            return start ? MO_T_TEXT : MO_T_TEXT_TAIL;
        }
        if (c == 10 || c == 13) {
            if (!triple) {
                uint32_t at = lx->pos + 1;
                while (at < lx->source->length && lx->source->bytes[at] != '\n' &&
                       lx->source->bytes[at] != '"')
                    at++;
                if (at < lx->source->length && lx->source->bytes[at] == '"')
                    lx->recovery_end = at + 1;
                (void)lex_error(lx, MO_CODE_SOURCE_TEXT,
                                "a line break in text needs triple quotes: \"\"\"");
                return MO_T_ERROR;
            }
            if (!advance(lx) || (c == 13 && !advance(lx)))
                return MO_T_ERROR;
            line_start = true;
        } else if (c == '\\') {
            if (!advance(lx) || !escape(lx))
                return MO_T_ERROR;
        } else if (c == '#' && peek(lx, 1) == '{') {
            if (!advance(lx) || !advance(lx))
                return MO_T_ERROR;
            if (start) {
                if (lx->depth == MO_LIMIT_INTERPOLATION_DEPTH) {
                    (void)lex_error(lx, MO_CODE_INTERPOLATIONS_TOO_DEEP,
                                    "interpolations are nested too deeply");
                    return MO_T_ERROR;
                }
                MO_ASSERT(lx->depth < MO_LIMIT_INTERPOLATION_DEPTH);
                lx->interpolations[lx->depth].braces = 0;
                lx->interpolations[lx->depth].triple = triple;
                lx->interpolations[lx->depth].indent = indent;
                lx->depth++;
            }
            return start ? MO_T_TEXT_HEAD : MO_T_TEXT_MID;
        } else if (!advance(lx))
            return MO_T_ERROR;
    }
    (void)lex_error(lx, MO_CODE_SOURCE_TEXT, "text has a closing quote");
    return MO_T_ERROR;
}

static MO_MUST_USE enum mo_syntax_token scan_word(struct mo_lex *lx) {
    uint32_t start = lx->pos;
    bool cap = upper(peek(lx, 0)), bad = false;
    while (alnum(peek(lx, 0)) || (!cap && peek(lx, 0) == '_')) {
        bad = bad || (!cap && upper(peek(lx, 0)));
        if (!advance(lx))
            return MO_T_ERROR;
    }
    if (peek(lx, 0) == '?') {
        bad = bad || cap;
        if (!advance(lx))
            return MO_T_ERROR;
    }
    if (bad || (cap && peek(lx, 0) == '_')) {
        lex_error_from(lx, MO_CODE_SOURCE_NAME,
                       "names are snake_case; types and modules are CapCase", start);
        return MO_T_ERROR;
    }
    uint32_t length = lx->pos - start;
    const uint8_t *text = lx->source->bytes + start;
    if (cap)
        return MO_T_CAPNAME;
    if (length == 1 && text[0] == '_')
        return MO_T_UNDERSCORE;
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++)
        if (strlen(words[i].text) == length && memcmp(text, words[i].text, length) == 0)
            return words[i].kind;
    return MO_T_NAME;
}

static MO_MUST_USE enum mo_syntax_token scan_literal(struct mo_lex *lx) {
    int c = peek(lx, 0);
    if (digit(c)) {
        uint32_t start = lx->pos;
        enum mo_syntax_token kind = MO_T_INT;
        if (lx->last == MO_T_DOT) {
            while (digit(peek(lx, 0)))
                if (!advance(lx))
                    return MO_T_ERROR;
        } else if (c == '0' && (peek(lx, 1) == 'x' || peek(lx, 1) == 'b')) {
            int base = peek(lx, 1) == 'x' ? 16 : 2;
            if (!advance(lx) || !advance(lx) || !digits(lx, base))
                return MO_T_ERROR;
        } else {
            if (!digits(lx, 10))
                return MO_T_ERROR;
            if (c == '0' && lx->pos - start > 1) {
                (void)lex_error(lx, MO_CODE_SOURCE_NUMBER, "a whole number doesn't start with 0");
                return MO_T_ERROR;
            }
            if (peek(lx, 0) == '.' && digit(peek(lx, 1))) {
                if (!advance(lx) || !digits(lx, 10))
                    return MO_T_ERROR;
                kind = MO_T_DECIMAL;
            }
        }
        if (alnum(peek(lx, 0)) || peek(lx, 0) == '_') {
            (void)lex_error(lx, MO_CODE_SOURCE_NUMBER, "Mo has no number suffixes or exponents");
            return MO_T_ERROR;
        }
        return kind;
    }
    if (c == '\'') {
        if (!advance(lx))
            return MO_T_ERROR;
        c = peek(lx, 0);
        if (c == -1 || c == 10 || c == 13 || c == '\'') {
            (void)lex_error(lx, MO_CODE_SOURCE_TEXT, "a Char is one Unicode character");
            return MO_T_ERROR;
        }
        if (!advance(lx) || (c == '\\' && !escape(lx)))
            return MO_T_ERROR;
        if (peek(lx, 0) != '\'') {
            (void)lex_error(lx, MO_CODE_SOURCE_TEXT, "a Char is one Unicode character");
            return MO_T_ERROR;
        }
        if (!advance(lx))
            return MO_T_ERROR;
        return MO_T_CHAR;
    }
    MO_ASSERT(c == '"');
    bool triple = peek(lx, 1) == '"' && peek(lx, 2) == '"';
    if (!advance(lx))
        return MO_T_ERROR;
    uint32_t indent = 0;
    if (triple) {
        if (!advance(lx) || !advance(lx))
            return MO_T_ERROR;
        while (peek(lx, 0) == ' ' || peek(lx, 0) == 9)
            if (!advance(lx))
                return MO_T_ERROR;
        if (peek(lx, 0) != 10 && peek(lx, 0) != 13) {
            (void)lex_error(lx, MO_CODE_SOURCE_TEXT,
                            "text on several lines starts at the end of a line");
            return MO_T_ERROR;
        }
        if (!lx->triple_cached || lx->triple_close < lx->pos) {
            uint32_t at = lx->pos;
            lx->triple_close = lx->source->length;
            lx->triple_cached = true;
            while (at < lx->source->length) {
                while (at < lx->source->length && lx->source->bytes[at] != 10)
                    at++;
                if (at == lx->source->length)
                    break;
                at++;
                uint32_t spaces = 0;
                while (spaces < lx->source->length - at && lx->source->bytes[at + spaces] == ' ')
                    spaces++;
                if (lx->source->length - at - spaces >= 3 &&
                    memcmp(lx->source->bytes + at + spaces, "\"\"\"", 3) == 0) {
                    lx->triple_close = at;
                    lx->triple_indent = spaces;
                    break;
                }
            }
        }
        if (lx->triple_close == lx->source->length) {
            lx->recovery_end = lx->source->length;
            (void)lex_error(lx, MO_CODE_SOURCE_TEXT, "text has a closing triple quote");
            return MO_T_ERROR;
        }
        indent = lx->triple_indent;
        lx->recovery_end = lx->triple_close + indent + 3;
        if (peek(lx, 0) == 13 && !advance(lx))
            return MO_T_ERROR;
        if (!advance(lx))
            return MO_T_ERROR;
    }
    return text_body(lx, true, triple, indent, triple);
}

#define LAMBDA_REASON                                                                              \
    "An anonymous function is written fn(x) ... end, on one line or several, and its parameter "   \
    "types are inferred (reference 5.6)."

static MO_MUST_USE enum mo_syntax_token punctuation(struct mo_lex *lx) {
    static const struct {
        const char *text;
        enum mo_syntax_token kind;
    } pairs[] = {
        {"..", MO_T_DOTDOT}, {"+=", MO_T_PLUSEQ}, {"-=", MO_T_MINUSEQ}, {"==", MO_T_EQ},
        {"!=", MO_T_NE},     {"<=", MO_T_LE},     {">=", MO_T_GE},
    };
    static const struct {
        const char *text;
        const char *message;
        uint16_t code;
    } bad[] = {
        {"::", "Mo separates a module from a name with ., not ::", MO_CODE_SOURCE_QUALIFICATION},
        {"...", "a range is written with .., not ...", MO_CODE_SOURCE_OPERATOR},
        {"=>", "a case arm is written Pattern: value, with no =>", MO_CODE_SOURCE_CASE},
        {"->", "a function's result type follows :, not ->", MO_CODE_SOURCE_FUNCTION},
        {"**", "Mo has no ** operator", MO_CODE_SOURCE_OPERATOR},
        {"*=", "Mo has += and -=, and no other compound assignment", MO_CODE_SOURCE_ASSIGNMENT},
        {"//", "comments start with #", MO_CODE_SOURCE_COMMENT},
        {"/=", "Mo has += and -=, and no other compound assignment", MO_CODE_SOURCE_ASSIGNMENT},
        {"||", "Mo writes or instead of ||", MO_CODE_SOURCE_BOOLEAN},
        {"&&", "Mo writes and instead of &&", MO_CODE_SOURCE_BOOLEAN},
        {"|>", "Mo has no pipe operator; chain calls with dots, as in text.trim.lines",
         MO_CODE_SOURCE_CALL},
    };
    /* A => inside an open bracket, after an = or after fn(x) is a function, not a case arm. */
    if (peek(lx, 0) == '=' && peek(lx, 1) == '>' &&
        (lx->line_opens > 0 || lx->line_assign || lx->lambda_close)) {
        mo_syntax_error(lx, MO_CODE_SOURCE_LAMBDA, (struct mo_span){lx->file, lx->pos, lx->pos + 2},
                        "Mo has no => functions: write fn(x) ... end", LAMBDA_REASON);
        return MO_T_ERROR;
    }
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        size_t n = strlen(bad[i].text);
        if (n <= lx->source->length - lx->pos &&
            memcmp(lx->source->bytes + lx->pos, bad[i].text, n) == 0) {
            (void)lex_error(lx, bad[i].code, bad[i].message);
            return MO_T_ERROR;
        }
    }
    enum mo_syntax_token kind = MO_T_ERROR;
    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
        if (peek(lx, 0) == pairs[i].text[0] && peek(lx, 1) == pairs[i].text[1]) {
            kind = pairs[i].kind;
            if (!advance(lx))
                return MO_T_ERROR;
            break;
        }
    }
    if (kind == MO_T_ERROR) {
        const char *chars = "()[]{} ,:.|=<>+-*/%";
        const enum mo_syntax_token kinds[] = {MO_T_LPAREN, MO_T_RPAREN, MO_T_LBRACK, MO_T_RBRACK,
                                              MO_T_LBRACE, MO_T_RBRACE, MO_T_ERROR,  MO_T_COMMA,
                                              MO_T_COLON,  MO_T_DOT,    MO_T_BAR,    MO_T_ASSIGN,
                                              MO_T_LT,     MO_T_GT,     MO_T_PLUS,   MO_T_MINUS,
                                              MO_T_STAR,   MO_T_SLASH,  MO_T_PERCENT};
        for (size_t i = 0; chars[i] != '\0'; i++)
            if (peek(lx, 0) == chars[i]) {
                kind = kinds[i];
                break;
            }
    }
    /* An alternative's | follows a pattern; one that starts a value opens |x| parameters. */
    if (kind == MO_T_BAR &&
        (lx->last == MO_T_LPAREN || lx->last == MO_T_LBRACK || lx->last == MO_T_LBRACE ||
         lx->last == MO_T_COMMA || lx->last == MO_T_COLON || lx->last == MO_T_ASSIGN)) {
        mo_syntax_error(lx, MO_CODE_SOURCE_LAMBDA, (struct mo_span){lx->file, lx->pos, lx->pos + 1},
                        "Mo has no |x| functions: write fn(x) ... end", LAMBDA_REASON);
        return MO_T_ERROR;
    }
    if (kind == MO_T_ERROR) {
        uint32_t start = lx->pos;
        int c = peek(lx, 0);
        if (!advance(lx))
            return MO_T_ERROR;
        enum mo_syntax_token last = lx->last;
        if (c == '!' && lx->preceding.span.end == start &&
            (last == MO_T_NAME || last == MO_T_CAPNAME || last == MO_T_RPAREN ||
             last == MO_T_RBRACK || last == MO_T_RBRACE || last == MO_T_TEXT ||
             last == MO_T_TEXT_TAIL)) {
            mo_syntax_error(lx, MO_CODE_SOURCE_UNWRAP, (struct mo_span){lx->file, start, lx->pos},
                            "Mo has no postfix ! to unwrap a value: use case, with stop \"why it "
                            "can't happen\"",
                            "There is no unwrap: where a failure truly can't happen, the code says "
                            "why, with stop (reference 8.4).");
            return MO_T_ERROR;
        }
        const char *message = c == ';'   ? "a line break ends a statement; Mo has no semicolons"
                              : c == '!' ? "Mo writes not instead of !"
                              : c == '&' ? "Mo has no & operator"
                                         : "this character isn't part of Mo";
        uint16_t code = c == '!'   ? MO_CODE_SOURCE_BOOLEAN
                        : c == '&' ? MO_CODE_SOURCE_OPERATOR
                                   : MO_CODE_SOURCE_TOKEN;
        const char *reason =
            c == '!'   ? "Boolean expressions use and, or and not (reference 3.1)."
            : c == '&' ? "Use Mo's arithmetic operators (reference 3.2)."
                       : "A line break ends a statement; use Mo's tokens (reference 2.4).";
        (void)mo_syntax_error(lx, code, (struct mo_span){lx->file, start, lx->pos}, message,
                              reason);
        return MO_T_ERROR;
    }
    if (!advance(lx))
        return MO_T_ERROR;
    if (lx->depth > 0) {
        if (kind == MO_T_LBRACE)
            lx->interpolations[lx->depth - 1].braces++;
        if (kind == MO_T_RBRACE)
            lx->interpolations[lx->depth - 1].braces--;
    }
    return kind;
}

/* Line continuation (reference 2.4). An interpolation's #{ and } act as brackets: text ending in
 * #{ continues, and so does a line starting with the } that closes it. */
static MO_MUST_USE bool continues(enum mo_syntax_token token, bool before) {
    return token == MO_T_DOT || token == MO_T_DOTDOT || token == MO_T_EQ || token == MO_T_NE ||
           token == MO_T_LT || token == MO_T_LE || token == MO_T_GT || token == MO_T_GE ||
           token == MO_T_PLUS || token == MO_T_STAR || token == MO_T_SLASH ||
           token == MO_T_PERCENT || token == MO_T_BAR || token == MO_T_AND || token == MO_T_OR ||
           token == MO_T_IMPLIES || token == MO_T_IS || token == MO_T_WHERE ||
           token == MO_T_TEXT_MID ||
           (before ? token == MO_T_RPAREN || token == MO_T_RBRACK || token == MO_T_RBRACE ||
                         token == MO_T_TEXT_TAIL
                   : token == MO_T_LPAREN || token == MO_T_LBRACK || token == MO_T_LBRACE ||
                         token == MO_T_TEXT_HEAD || token == MO_T_COMMA || token == MO_T_ASSIGN ||
                         token == MO_T_PLUSEQ || token == MO_T_MINUSEQ || token == MO_T_MINUS ||
                         token == MO_T_NOT || token == MO_T_TRY);
}

struct mo_token mo_syntax_next(struct mo_lex *lx) {
    struct mo_token token = {
        .span = {lx->file, lx->pos, lx->pos}, .line = lx->line, .column = lx->column};
    if (lx->fatal || lx->failed) {
        token.kind = MO_T_ERROR;
        return token;
    }
    if (lx->has_pending) {
        lx->has_pending = false;
        lx->last = lx->pending.kind;
        return lx->pending;
    }
    bool newline = false;
    struct mo_token nl = token;
    uint32_t depth = lx->depth;
    for (;;) {
        int c = peek(lx, 0);
        if (c == ' ' || c == 9 || c == 10 || c == 13) {
            if ((c == 10 || c == 13) && !newline) {
                newline = true;
                nl.span.start = lx->pos;
                nl.line = lx->line;
                nl.column = lx->column;
            }
            if (!advance(lx))
                goto bad_gap;
        } else if (c == '#') {
            if (lx->ignore_comments) {
                while (peek(lx, 0) != -1 && peek(lx, 0) != 10 && peek(lx, 0) != 13)
                    if (!advance(lx))
                        goto bad_gap;
                continue;
            }
            struct mo_comment_storage *storage =
                mo_arena_allocate(lx->arena, sizeof *storage, _Alignof(struct mo_comment_storage));
            if (storage == NULL) {
                (void)lex_error(lx, MO_CODE_SYNTAX_NO_MEMORY, "the syntax arena has no room");
                token.kind = MO_T_ERROR;
                return token;
            }
            *storage = (struct mo_comment_storage){.previous = lx->last_comment};
            if (lx->preceding.kind != MO_T_EOF && lx->preceding_line == lx->line &&
                lx->preceding.span.end <= lx->pos)
                storage->preceding = lx->preceding;
            struct mo_syntax_comment *comment = &storage->value;
            *comment = (struct mo_syntax_comment){.span = {lx->file, lx->pos, lx->pos},
                                                  .indentation = lx->column - 1,
                                                  .documentation = peek(lx, 1) == '#',
                                                  .anchor = storage->preceding.kind == MO_T_EOF
                                                                ? MO_COMMENT_OWN_LINE
                                                                : MO_COMMENT_TOKEN,
                                                  .token = storage->preceding.kind};
            while (peek(lx, 0) != -1 && peek(lx, 0) != 10 && peek(lx, 0) != 13)
                if (!advance(lx))
                    goto bad_gap;
            comment->span.end = lx->pos;
            comment->text = mo_arena_copy(lx->arena, lx->source->bytes + comment->span.start,
                                          comment->span.end - comment->span.start, 1);
            if (comment->text == NULL) {
                lex_error(lx, MO_CODE_SYNTAX_NO_MEMORY, "the syntax arena has no room");
                lx->fatal = true;
                token.kind = MO_T_ERROR;
                return token;
            }
            *lx->comment_tail = comment;
            lx->comment_tail = &comment->next;
            lx->last_comment = storage;
        } else
            break;
    }
    token.span.start = lx->pos;
    token.line = lx->line;
    token.column = lx->column;
    int c = peek(lx, 0);
    if (lx->header_only && newline && depth == 0 && lx->last != MO_T_EOF &&
        !continues(lx->last, false) && c != '.' && c != ')' && c != ']' && c != '}' && c != '+' &&
        c != '*' && c != '/' && c != '%' && c != '|' && c != '<' && c != '>' && c != '=') {
        uint32_t length = 0;
        while (lower(peek(lx, length)))
            length++;
        bool header_word = (length == 3 && memcmp(lx->source->bytes + lx->pos, "use", 3) == 0) ||
                           (length == 6 && memcmp(lx->source->bytes + lx->pos, "expose", 6) == 0);
        header_word = header_word && !alnum(peek(lx, length)) && peek(lx, length) != '_' &&
                      peek(lx, length) != '?';
        if (!header_word) {
            lx->pending = token;
            lx->pending.kind = MO_T_EOF;
            lx->pending.span.end = lx->pending.span.start;
            lx->has_pending = true;
            nl.kind = MO_T_NL;
            nl.span.end = nl.span.start;
            lx->last = MO_T_NL;
            return nl;
        }
    }
    if (c == -1)
        token.kind = MO_T_EOF;
    else if (lower(c) || upper(c) || c == '_')
        token.kind = scan_word(lx);
    else if (digit(c) || c == '"' || c == '\'')
        token.kind = scan_literal(lx);
    else if (c == '[' && !newline &&
             (lx->last == MO_T_NAME || lx->last == MO_T_CAPNAME ||
              (lx->last == MO_T_RPAREN && !lx->lambda_close) || lx->last == MO_T_RBRACK ||
              lx->last == MO_T_TEXT || lx->last == MO_T_TEXT_TAIL)) {
        (void)lex_error(
            lx, MO_CODE_SOURCE_INDEX,
            "Mo has no indexing with [...]: items.get(i) and map.get(key) return an Option");
        token.kind = MO_T_ERROR;
    } else if (c == '}' && depth > 0 && lx->interpolations[depth - 1].braces == 0) {
        if (!advance(lx))
            token.kind = MO_T_ERROR;
        else
            token.kind = text_body(lx, false, lx->interpolations[depth - 1].triple,
                                   lx->interpolations[depth - 1].indent, false);
    } else
        token.kind = punctuation(lx);
scanned:
    /* Lambda parameters cannot nest: only names, _ and commas are allowed.
     * Update once per scanned token, not again when returning pending tokens. */
    lx->lambda_close = token.kind == MO_T_RPAREN && lx->lambda_parameters;
    lx->lambda_parameters =
        (token.kind == MO_T_LPAREN && lx->last == MO_T_FN) ||
        (lx->lambda_parameters &&
         (token.kind == MO_T_NAME || token.kind == MO_T_UNDERSCORE || token.kind == MO_T_COMMA));
    token.span.end = lx->pos;
    if (token.kind != MO_T_EOF && token.kind != MO_T_ERROR) {
        lx->preceding = token;
        lx->preceding_line = lx->line;
    }
    if (token.kind == MO_T_EOF && depth > 0) {
        (void)lex_error(lx, MO_CODE_SOURCE_TEXT, "an interpolation is closed with }");
        token.kind = MO_T_ERROR;
    }
    bool split = lx->last != MO_T_EOF && lx->last != MO_T_NL &&
                 (token.kind == MO_T_EOF ||
                  (newline && !continues(lx->last, false) && !continues(token.kind, true)));
    if (split || lx->last == MO_T_EOF || lx->last == MO_T_NL) {
        lx->line_opens = 0;
        lx->line_assign = false;
    }
    if (token.kind == MO_T_LPAREN || token.kind == MO_T_LBRACK || token.kind == MO_T_LBRACE)
        lx->line_opens++;
    else if ((token.kind == MO_T_RPAREN || token.kind == MO_T_RBRACK ||
              token.kind == MO_T_RBRACE) &&
             lx->line_opens > 0)
        lx->line_opens--;
    else if (token.kind == MO_T_ASSIGN)
        lx->line_assign = true;
    if (split) {
        lx->pending = token;
        lx->has_pending = true;
        nl.kind = MO_T_NL;
        nl.span.end = nl.span.start;
        lx->last = MO_T_NL;
        return nl;
    }
    lx->last = token.kind;
    return token;
bad_gap:
    /* A bad comment on the following physical line must not erase the
     * statement whose newline we have already crossed. */
    token.kind = MO_T_ERROR;
    token.span.start = lx->pos;
    token.line = lx->line;
    token.column = lx->column;
    goto scanned;
}

void mo_syntax_resume(struct mo_lex *lx) {
    /* Skip the remainder of the erroneous physical line, even when it isn't
     * valid UTF-8. Reset interpolations and logical-line continuation. */
    while (lx->pos < lx->source->length &&
           (lx->pos < lx->recovery_end || lx->source->bytes[lx->pos] != '\n')) {
        if (lx->source->bytes[lx->pos++] == '\n')
            lx->line++;
    }
    if (lx->pos < lx->source->length) {
        lx->pos++;
        lx->line++;
    }
    lx->column = 1;
    lx->last = MO_T_EOF;
    lx->lambda_parameters = false;
    lx->lambda_close = false;
    lx->line_opens = 0;
    lx->line_assign = false;
    lx->has_pending = false;
    lx->depth = 0;
    lx->recovery_end = 0;
    lx->failed = false;
}
