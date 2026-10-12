/* Fuzzes the executable grammar: its lexer, its parser and the checks on the
 * tree. Whatever bytes arrive, read as a module and as a fragment, they must
 * parse or be refused with a message, without tripping an assertion or a
 * sanitizer. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ast.h"
#include "lexer.h"
#include "mo.tab.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void check(const uint8_t *data, size_t size, bool fragment) {
    struct mo_arena arena;
    mo_arena_init(&arena, MO_GRAMMAR_ARENA_LIMIT);
    struct lexer lx;
    mo_lexer_init(&lx, "fuzz.mo", data, size, 1, fragment, &arena);
    lx.quiet = true;
    struct node *root = NULL;
    int status = yyparse(&lx, &root);
    if (status == 0 && lx.errors == 0) {
        MO_ASSERT(root != NULL);
        bool valid = mo_validate(&lx, root);
        MO_ASSERT(valid == (lx.errors == 0));
    } else {
        MO_ASSERT(lx.errors > 0); // every refusal comes with a message
    }
    mo_arena_free(&arena);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    check(data, size, false);
    check(data, size, true);
    return 0;
}
