// mo-grammar: checks that Mo code parses with the executable grammar.
//
//   mo-grammar FILE.mo...                 each file is a module
//   mo-grammar FILE.md...                 each ```mo block is a fragment
//   mo-grammar --fragment FILE.mo...      each file is a fragment
//   mo-grammar --expect-error FILE.mo...  each file must fail, with the error
//                                         its first line names: # error: TEXT
//   mo-grammar --tokens FILE.mo           print the tokens, for debugging
//
// Exits with 0 when everything checked as expected, 1 when something didn't,
// and 2 for a usage or file error.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "lexer.h"
#include "mo.tab.h"

enum mode { CHECK, EXPECT_ERROR, TOKENS };

struct totals {
    int checks;   // sources checked: files, and code blocks in Markdown files
    int blocks;   // of which code blocks
    int failures; // checks that didn't go as expected
};

static bool read_file(const char *path, unsigned char **data, size_t *length) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        (void)fprintf(stderr, "mo-grammar: can't open %s\n", path);
        return false;
    }
    size_t capacity = 4096;
    size_t used = 0;
    unsigned char *buffer = malloc(capacity);
    if (buffer == NULL) {
        (void)fclose(file);
        (void)fprintf(stderr, "mo-grammar: out of memory\n");
        return false;
    }
    for (;;) {
        if (used == capacity) {
            capacity *= 2;
            unsigned char *bigger = realloc(buffer, capacity);
            if (bigger == NULL) {
                free(buffer);
                (void)fclose(file);
                (void)fprintf(stderr, "mo-grammar: out of memory\n");
                return false;
            }
            buffer = bigger;
        }
        size_t got = fread(buffer + used, 1, capacity - used, file);
        used += got;
        if (got == 0) {
            break;
        }
    }
    bool failed = ferror(file) != 0;
    if (fclose(file) != 0 || failed) {
        free(buffer);
        (void)fprintf(stderr, "mo-grammar: can't read %s\n", path);
        return false;
    }
    *data = buffer;
    *length = used;
    return true;
}

// Parses one source and checks the tree. Returns whether it parsed; the first
// error, if any, is in lx->first_error.
static bool parse(struct lexer *lx) {
    struct node *root = NULL;
    int status = yyparse(lx, &root);
    if (status != 0 || lx->errors > 0) {
        if (lx->errors == 0) {
            mo_lexer_error(lx, lx->line, lx->col, "the parser stopped (status %d)", status);
        }
        return false;
    }
    MO_ASSERT(root != NULL);
    return mo_validate(lx, root);
}

static void print_tokens(struct lexer *lx) {
    YYSTYPE value = NULL;
    YYLTYPE location = {0, 0, 0, 0};
    for (;;) {
        int token = yylex(&value, &location, lx);
        if (token == YYEOF || token == YYerror) {
            break;
        }
        printf("%d:%d %s\n", location.first_line, location.first_column, mo_token_name(token));
    }
}

// The text an --expect-error file's first line asks for, or NULL.
static char *expectation(const unsigned char *data, size_t length) {
    const char *prefix = "# error: ";
    size_t prefix_length = strlen(prefix);
    if (length < prefix_length || memcmp(data, prefix, prefix_length) != 0) {
        return NULL;
    }
    size_t end = prefix_length;
    while (end < length && data[end] != '\n' && data[end] != '\r') {
        end += 1;
    }
    size_t size = end - prefix_length;
    char *text = malloc(size + 1);
    if (text == NULL) {
        return NULL;
    }
    memcpy(text, data + prefix_length, size);
    text[size] = '\0';
    return text;
}

static bool check_source(const char *path, const unsigned char *data, size_t length, int first_line,
                         bool fragment, enum mode mode) {
    struct arena *arena = mo_arena_new();
    struct lexer lx;
    mo_lexer_init(&lx, path, data, length, first_line, fragment, arena);
    bool ok;
    if (mode == TOKENS) {
        print_tokens(&lx);
        ok = lx.errors == 0;
    } else if (mode == EXPECT_ERROR) {
        lx.quiet = true;
        char *wanted = expectation(data, length);
        if (wanted == NULL) {
            (void)fprintf(stderr, "%s: the first line must be # error: TEXT\n", path);
            ok = false;
        } else if (parse(&lx)) {
            (void)fprintf(stderr, "%s: parsed, but should fail with: %s\n", path, wanted);
            ok = false;
        } else {
            // Compare without the path, so the expectation can include the line and column.
            const char *message = lx.first_error + strlen(path) + 1;
            ok = strstr(message, wanted) != NULL;
            if (!ok) {
                (void)fprintf(stderr, "%s: failed with\n  %s\nbut should fail with\n  %s\n", path,
                              message, wanted);
            }
        }
        free(wanted);
    } else {
        ok = parse(&lx);
    }
    mo_arena_free(arena);
    return ok;
}

static bool starts_fence(const unsigned char *line, size_t length, const char *language) {
    size_t i = 0;
    while (i < length && line[i] == ' ') {
        i += 1;
    }
    if (length - i < 3 || memcmp(line + i, "```", 3) != 0) {
        return false;
    }
    i += 3;
    size_t language_length = strlen(language);
    if (length - i < language_length || memcmp(line + i, language, language_length) != 0) {
        return false;
    }
    i += language_length;
    while (i < length && (line[i] == ' ' || line[i] == '\r')) {
        i += 1;
    }
    return i == length;
}

// Checks every ```mo block in a Markdown file, each as a fragment.
static void check_markdown(const char *path, const unsigned char *data, size_t length,
                           struct totals *totals) {
    size_t pos = 0;
    int line = 1;
    bool inside = false;
    size_t block_start = 0;
    int block_line = 0;
    while (pos < length) {
        size_t end = pos;
        while (end < length && data[end] != '\n') {
            end += 1;
        }
        const unsigned char *text = data + pos;
        size_t text_length = end - pos;
        if (!inside && starts_fence(text, text_length, "mo")) {
            inside = true;
            block_start = end < length ? end + 1 : end;
            block_line = line + 1;
        } else if (inside && starts_fence(text, text_length, "")) {
            inside = false;
            totals->checks += 1;
            totals->blocks += 1;
            if (!check_source(path, data + block_start, pos - block_start, block_line, true,
                              CHECK)) {
                totals->failures += 1;
            }
        }
        pos = end < length ? end + 1 : end;
        line += 1;
    }
    if (inside) {
        (void)fprintf(stderr, "%s:%d: this ```mo block is never closed\n", path, block_line - 1);
        totals->failures += 1;
    }
}

static bool ends_with(const char *text, const char *suffix) {
    size_t length = strlen(text);
    size_t suffix_length = strlen(suffix);
    return length >= suffix_length && strcmp(text + length - suffix_length, suffix) == 0;
}

int main(int argc, char **argv) {
    enum mode mode = CHECK;
    bool fragment = false;
    int first = 1;
    for (; first < argc && argv[first][0] == '-'; first++) {
        if (strcmp(argv[first], "--fragment") == 0) {
            fragment = true;
        } else if (strcmp(argv[first], "--expect-error") == 0) {
            mode = EXPECT_ERROR;
        } else if (strcmp(argv[first], "--tokens") == 0) {
            mode = TOKENS;
        } else {
            (void)fprintf(stderr, "mo-grammar: unknown option %s\n", argv[first]);
            return 2;
        }
    }
    if (first == argc) {
        (void)fprintf(stderr,
                      "usage: mo-grammar [--fragment | --expect-error | --tokens] FILE...\n");
        return 2;
    }
    struct totals totals = {0, 0, 0};
    for (int i = first; i < argc; i++) {
        unsigned char *data = NULL;
        size_t length = 0;
        if (!read_file(argv[i], &data, &length)) {
            return 2;
        }
        if (ends_with(argv[i], ".md") && mode == CHECK) {
            check_markdown(argv[i], data, length, &totals);
        } else {
            totals.checks += 1;
            if (!check_source(argv[i], data, length, 1, fragment, mode)) {
                totals.failures += 1;
            }
        }
        free(data);
    }
    if (mode == TOKENS) {
        return totals.failures == 0 ? 0 : 1;
    }
    if (totals.failures == 0) {
        int files = totals.checks - totals.blocks;
        printf("mo-grammar: ");
        if (files > 0) {
            printf("%d %s", files, files == 1 ? "file" : "files");
        }
        if (totals.blocks > 0) {
            printf("%s%d code %s", files > 0 ? " and " : "", totals.blocks,
                   totals.blocks == 1 ? "block" : "blocks");
        }
        printf(": all %s\n", mode == EXPECT_ERROR ? "fail as expected" : "parse");
        return 0;
    }
    (void)fprintf(stderr, "mo-grammar: %d of %d checks failed\n", totals.failures, totals.checks);
    return 1;
}
