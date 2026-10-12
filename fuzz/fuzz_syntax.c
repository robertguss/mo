/* Independent recursive descent against Bison plus the grammar's AST checks. */
#include "ast.h"
#include "base/codes.h"
#include "lexer.h"
#include "mo.tab.h"
#include "syntax/syntax.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void compare(const uint8_t *data, size_t size, bool fragment) {
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_arena arena;
    MO_ASSERT(mo_sources_init(&sources));
    MO_ASSERT(mo_diags_init(&diags));
    mo_arena_init(&arena, MO_GRAMMAR_ARENA_LIMIT);
    uint32_t file;
    struct mo_source_error problem;
    MO_ASSERT(mo_sources_add_bytes(&sources, "fuzz.mo", data, size, &file, &problem));
    struct mo_module_syntax *tree = fragment ? mo_parse_fragment(&arena, &sources, file, &diags)
                                             : mo_parse(&arena, &sources, file, &diags);
    bool accepted = tree != NULL && !mo_diags_any(&diags);
    bool within_limits = true;
    for (uint32_t i = 0; i < diags.count; i++) {
        uint16_t code = diags.items[i].code;
        MO_ASSERT(code != MO_CODE_SYNTAX_NO_MEMORY);
        if (code == MO_CODE_LINE_TOO_LONG || code == MO_CODE_NESTED_TOO_DEEPLY ||
            code == MO_CODE_TOO_MANY_NAMES || code == MO_CODE_TOO_MANY_PARAMETERS ||
            code == MO_CODE_TOO_MANY_FIELDS || code == MO_CODE_TOO_MANY_CASES)
            within_limits = false;
    }
    mo_arena_free(&arena);
    if (within_limits) {
        struct lexer lx;
        mo_lexer_init(&lx, "fuzz.mo", data, size, 1, fragment, &arena);
        lx.quiet = true;
        struct node *root = NULL;
        int status = yyparse(&lx, &root);
        bool grammar = status == 0 && lx.errors == 0 && mo_validate(&lx, root);
        MO_ASSERT(accepted == grammar);
        mo_arena_free(&arena);
    }
    mo_diags_free(&diags);
    mo_sources_free(&sources);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > MO_LIMIT_FILE_SIZE)
        return 0;
    compare(data, size, false);
    compare(data, size, true);
    return 0;
}
