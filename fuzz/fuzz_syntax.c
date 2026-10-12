/* Compare verdicts and exact normalized trees with the independent grammar.
 * Limit exclusions are proved from the grammar tree, never from the parser. */
#include "ast.h"
#include "base/buffer.h"
#include "base/codes.h"
#include "lexer.h"
#include "mo.tab.h"
#include "syntax/syntax.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

_Static_assert((int)MO_S_TOKEN == (int)K_TOKEN, "shared kind order");
_Static_assert(MO_T_PERCENT - MO_T_NL == PERCENT - NL, "shared token order");

static MO_MUST_USE uint32_t token(enum mo_syntax_token t) {
    return t < MO_T_NL ? 0 : (uint32_t)t + NL - MO_T_NL;
}

static void word(struct mo_buffer *b, uint32_t value) {
    MO_ASSERT(mo_buffer_append_le32(b, value));
}

static void head(struct mo_buffer *b, uint32_t kind, uint32_t op, uint32_t count) {
    word(b, kind);
    word(b, op);
    word(b, count);
}

static void syntax(struct mo_buffer *b, const struct mo_syntax *n, const struct mo_sources *s);

static void syntax_else(struct mo_buffer *b, const struct mo_syntax *parts,
                        const struct mo_sources *s, bool block) {
    for (uint32_t i = 0; i < parts->count; i++) {
        const struct mo_syntax *part = parts->children[i];
        if (part->kind == MO_S_ELSE) {
            if (block)
                head(b, K_ELSE, 0, 1);
            syntax(b, part->children[0], s);
            return;
        }
        head(b, block ? K_IF : K_LINE_IF, 0, 3);
        syntax(b, part->children[0], s);
        syntax(b, part->children[1], s);
    }
    word(b, UINT32_MAX);
}

static void syntax(struct mo_buffer *b, const struct mo_syntax *n, const struct mo_sources *s) {
    if (n == NULL) {
        word(b, UINT32_MAX);
        return;
    }
    MO_ASSERT(!n->incomplete && n->span.start <= n->span.end);
    if (n->kind == MO_S_BINARY_CHAIN) {
        for (uint32_t i = n->count - 1; i > 1; i -= 2)
            head(b, K_BINARY, token(n->children[i - 1]->token), 2);
        syntax(b, n->children[0], s);
        for (uint32_t i = 2; i < n->count; i += 2)
            syntax(b, n->children[i], s);
        return;
    }
    if (n->kind == MO_S_POSTFIX) {
        for (uint32_t i = n->count - 1; i > 0; i--)
            head(b, (uint32_t)n->children[i]->kind, 0, 2);
        syntax(b, n->children[0], s);
        for (uint32_t i = 1; i < n->count; i++)
            syntax(b, n->children[i]->children[0], s);
        return;
    }
    if (n->kind == MO_S_PREFIX) {
        for (uint32_t i = 0; i + 1 < n->count; i++) {
            enum mo_syntax_token t = n->children[i]->token;
            head(b, t == MO_T_NOT ? K_NOT : t == MO_T_MINUS ? K_NEGATE : K_TRY, 0, 1);
        }
        syntax(b, n->children[n->count - 1], s);
        return;
    }
    bool leaf = n->text != NULL;
    uint32_t op = leaf || n->kind == MO_S_BINARY || n->kind == MO_S_ANCHOR || n->kind == MO_S_TYPE
                      ? token(n->token)
                      : 0;
    head(b, (uint32_t)n->kind, op, n->count);
    if (leaf) {
        struct mo_position p = mo_sources_position(s, n->span.file, n->span.start);
        word(b, p.line);
        word(b, p.column);
        MO_ASSERT(memcmp(n->text, s->files[n->span.file].bytes + n->span.start,
                         n->span.end - n->span.start) == 0);
    }
    if (n->kind == MO_S_IF || n->kind == MO_S_LINE_IF) {
        syntax(b, n->children[0], s);
        syntax(b, n->children[1], s);
        syntax_else(b, n->children[2], s, n->kind == MO_S_IF);
    } else {
        for (uint32_t i = 0; i < n->count; i++)
            syntax(b, n->children[i], s);
    }
}

/* The grammar retains positions but not spans. Locate type openers without
 * borrowing any decision from the source parser. Accepted source is UTF-8. */
static MO_MUST_USE uint32_t offset(const struct mo_source *s, const struct node *n) {
    MO_ASSERT(n->line > 0 && (uint32_t)n->line <= s->line_count && n->col > 0);
    uint32_t at = s->line_starts[(uint32_t)n->line - 1], column = 1;
    while (column < (uint32_t)n->col) {
        MO_ASSERT(at < s->length);
        at++;
        while (at < s->length && (s->bytes[at] & 192u) == 128u)
            at++;
        column++;
    }
    return at;
}

static MO_MUST_USE int type_form(const struct mo_source *s, const struct node *n) {
    uint32_t at = offset(s, n);
    MO_ASSERT(at < s->length);
    return s->bytes[at] == '(' ? LPAREN : s->bytes[at] == 'f' ? FN : 0;
}

struct visit {
    const struct node *node;
    uint32_t depth;
    bool continuation;
};

static void push(struct mo_buffer *stack, const struct node *n, uint32_t depth, bool continuation) {
    struct visit v = {n, depth, continuation};
    MO_ASSERT(mo_buffer_append(stack, &v, sizeof v));
}

static MO_MUST_USE struct visit pop(struct mo_buffer *stack) {
    MO_ASSERT(stack->length >= sizeof(struct visit));
    struct visit v;
    stack->length -= sizeof v;
    memcpy(&v, stack->bytes + stack->length, sizeof v);
    return v;
}

static MO_MUST_USE bool constructor(const struct node *n) {
    return n->kind == K_CAPNAME || (n->kind == K_QUALIFIED && n->kids[1]->kind == K_CAPNAME);
}

static MO_MUST_USE uint32_t bit(uint16_t code) {
    MO_ASSERT(code >= MO_CODE_LINE_TOO_LONG && code <= MO_CODE_TOO_MANY_CASES);
    return 1u << (code - MO_CODE_LINE_TOO_LONG);
}

static MO_MUST_USE uint32_t limits(const struct mo_source *source, const struct node *root) {
    uint32_t found = 0, columns = 0, names = 0;
    for (uint32_t i = 0; i < source->length; i++) {
        uint8_t c = source->bytes[i];
        if (c == '\n')
            columns = 0;
        else if ((c & 192u) != 128u &&
                 !(c == '\r' && i + 1 < source->length && source->bytes[i + 1] == '\n'))
            columns++;
        if (columns > MO_LIMIT_LINE_LENGTH)
            found |= bit(MO_CODE_LINE_TOO_LONG);
    }
    const struct node *items = root->kids[1];
    for (int i = 0; i < items->count; i++) {
        enum node_kind k = items->kids[i]->kind;
        names += k == K_FN || k == K_STRUCT || k == K_ENUM || k == K_CONST ? 1u : 0u;
    }
    if (names > MO_LIMIT_MODULE_NAMES)
        found |= bit(MO_CODE_TOO_MANY_NAMES);
    struct mo_buffer stack;
    mo_buffer_init(&stack, MO_GRAMMAR_ARENA_LIMIT);
    push(&stack, root, 0, false);
    while (stack.length != 0) {
        struct visit v = pop(&stack);
        const struct node *n = v.node;
        if (n == NULL)
            continue;
        enum node_kind k = n->kind;
        bool nested =
            k == K_FN || k == K_STRUCT || k == K_ENUM || k == K_TEST || k == K_CASE ||
            k == K_LAMBDA || k == K_FOR || k == K_WHILE || k == K_LIST || k == K_MAP ||
            k == K_GROUP || k == K_TUPLE || k == K_CALL ||
            ((k == K_IF || k == K_LINE_IF || k == K_TEXT_INTERPOLATED) && !v.continuation) ||
            (k == K_TYPE && (type_form(source, n) != 0 || n->count == 2));
        uint32_t depth = v.depth + (nested ? 1u : 0u);
        if (depth > MO_LIMIT_NESTING_DEPTH)
            found |= bit(MO_CODE_NESTED_TOO_DEEPLY);
        const struct node *params = k == K_FN ? n->kids[1] : k == K_LAMBDA ? n->kids[0] : NULL;
        if (k == K_TYPE && type_form(source, n) == FN && n->count == 2)
            params = n->kids[0];
        if (k == K_CALL && !constructor(n->kids[0]))
            params = n->kids[1];
        if (params != NULL && params->count > MO_LIMIT_PARAMETERS)
            found |= bit(MO_CODE_TOO_MANY_PARAMETERS);
        const struct node *fields = k == K_STRUCT ? n->kids[2] : k == K_TUPLE ? n : NULL;
        if (k == K_CASE_DECL && n->kids[1] != NULL && n->kids[1]->kind == K_LIST_OF)
            fields = n->kids[1];
        if (k == K_CALL && constructor(n->kids[0]))
            fields = n->kids[1];
        if ((fields != NULL && fields->count > MO_LIMIT_FIELDS) ||
            (k == K_TYPE && type_form(source, n) == LPAREN &&
             n->kids[1]->count + 1 > MO_LIMIT_FIELDS))
            found |= bit(MO_CODE_TOO_MANY_FIELDS);
        if (k == K_ENUM && n->kids[2]->count > MO_LIMIT_CHOICE_CASES)
            found |= bit(MO_CODE_TOO_MANY_CASES);
        for (int i = n->count; i > 0; i--) {
            int index = i - 1;
            bool continuation =
                (index == 2 && (k == K_IF || k == K_LINE_IF || k == K_TEXT_INTERPOLATED));
            push(&stack, n->kids[index], k == K_CALL && index == 0 ? v.depth : depth, continuation);
        }
    }
    mo_buffer_free(&stack);
    return found;
}

/* Use an explicit stack: the LR tree has unbounded flat operator chains. Only
 * the three documented grammar-to-source normalizations need custom output. */
static void grammar(struct mo_buffer *b, const struct node *root, const struct mo_source *source) {
    struct mo_buffer stack;
    mo_buffer_init(&stack, MO_GRAMMAR_ARENA_LIMIT);
    push(&stack, root, 0, false);
    while (stack.length != 0) {
        const struct node *n = pop(&stack).node;
        if (n == NULL) {
            word(b, UINT32_MAX);
            continue;
        }
        if (n->kind == K_TYPE) {
            int form = type_form(source, n);
            head(b, K_TYPE, (uint32_t)form, form == LPAREN ? 1 : 2);
            if (form == FN) {
                push(&stack, n->kids[n->count - 1], 0, false);
                if (n->count == 1)
                    head(b, K_LIST_OF, 0, 0);
                else
                    push(&stack, n->kids[0], 0, false);
            } else if (form == LPAREN) {
                const struct node *tail = n->kids[1];
                head(b, K_LIST_OF, 0, (uint32_t)tail->count + 1);
                for (int i = tail->count; i > 0; i--)
                    push(&stack, tail->kids[i - 1], 0, false);
                push(&stack, n->kids[0], 0, false);
            } else {
                push(&stack, n->count == 1 ? NULL : n->kids[1], 0, false);
                push(&stack, n->kids[0], 0, false);
            }
            continue;
        }
        if (n->kind == K_TEXT_INTERPOLATED) {
            uint32_t count = 1;
            for (const struct node *part = n; part->kind == K_TEXT_INTERPOLATED;
                 part = part->kids[2])
                count += 2;
            head(b, K_TEXT_INTERPOLATED, 0, count);
            /* Push parts backward by retaining their stack interval and reversing it. */
            size_t first = stack.length;
            const struct node *part = n;
            while (part->kind == K_TEXT_INTERPOLATED) {
                push(&stack, part->kids[0], 0, false);
                push(&stack, part->kids[1], 0, false);
                part = part->kids[2];
            }
            push(&stack, part, 0, false);
            size_t left = first, right = stack.length - sizeof(struct visit);
            while (left < right) {
                struct visit a, z;
                memcpy(&a, stack.bytes + left, sizeof a);
                memcpy(&z, stack.bytes + right, sizeof z);
                memcpy(stack.bytes + left, &z, sizeof z);
                memcpy(stack.bytes + right, &a, sizeof a);
                left += sizeof a;
                right -= sizeof z;
            }
            continue;
        }
        if (n->kind == K_CASE) {
            /* Normalized arm bodies include subsequent statement lines. */
            head(b, K_CASE, 0, 2);
            const struct node *lines = n->kids[1];
            uint32_t arms = 0;
            for (int i = 0; i < lines->count; i++)
                arms += lines->kids[i]->kind == K_ARM ? 1u : 0u;
            /* Emit case value first using this same independent serializer. */
            grammar(b, n->kids[0], source);
            head(b, K_LIST_OF, 0, arms);
            for (int i = 0; i < lines->count;) {
                const struct node *arm = lines->kids[i++];
                MO_ASSERT(arm->kind == K_ARM);
                int end = i;
                while (end < lines->count && lines->kids[end]->kind != K_ARM)
                    end++;
                head(b, K_ARM, 0, 3);
                grammar(b, arm->kids[0], source);
                grammar(b, arm->kids[1], source);
                head(b, K_LIST_OF, 0, (uint32_t)(end - i) + (arm->kids[2] != NULL ? 1u : 0u));
                if (arm->kids[2] != NULL)
                    grammar(b, arm->kids[2], source);
                while (i < end)
                    grammar(b, lines->kids[i++], source);
            }
            continue;
        }
        head(b, (uint32_t)n->kind, (uint32_t)n->op, (uint32_t)n->count);
        bool leaf = n->kind >= K_INT && n->kind <= K_UNDERSCORE;
        if (leaf || n->kind == K_TOKEN) {
            word(b, (uint32_t)n->line);
            word(b, (uint32_t)n->col);
        }
        for (int i = n->count; i > 0; i--)
            push(&stack, n->kids[i - 1], 0, false);
    }
    mo_buffer_free(&stack);
}

static void equal(const struct mo_buffer *a, const struct mo_buffer *b) {
    MO_ASSERT(a->length == b->length);
    MO_ASSERT(a->length == 0 || memcmp(a->bytes, b->bytes, a->length) == 0);
}

static void allocation(const struct mo_diags *diags) {
    bool found = diags->dropped != 0;
    for (uint32_t i = 0; i < diags->count; i++)
        found = found || diags->items[i].code == MO_CODE_SYNTAX_NO_MEMORY;
    MO_ASSERT(found);
}

static void compare(const uint8_t *data, size_t size, bool fragment) {
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_arena arena, oracle;
    MO_ASSERT(mo_sources_init(&sources));
    MO_ASSERT(mo_diags_init(&diags));
    mo_arena_init(&arena, MO_GRAMMAR_ARENA_LIMIT);
    mo_arena_init(&oracle, MO_GRAMMAR_ARENA_LIMIT);
    uint32_t file;
    struct mo_source_error problem;
    MO_ASSERT(mo_sources_add_bytes(&sources, "fuzz.mo", data, size, &file, &problem));
    struct mo_module_syntax *tree = fragment ? mo_parse_fragment(&arena, &sources, file, &diags)
                                             : mo_parse(&arena, &sources, file, &diags);
    bool accepted = tree != NULL && !mo_diags_any(&diags);
    for (uint32_t i = 0; i < diags.count; i++)
        MO_ASSERT(diags.items[i].code != MO_CODE_SYNTAX_NO_MEMORY);
    struct lexer lx;
    mo_lexer_init(&lx, "fuzz.mo", data, size, 1, fragment, &oracle);
    lx.quiet = true;
    struct node *root = NULL;
    int status = yyparse(&lx, &root);
    bool parsed = status == 0 && lx.errors == 0;
    uint32_t exceeded = parsed ? limits(&sources.files[file], root) : 0;
    bool oracle_accepted = parsed && mo_validate(&lx, root);
    bool excluded = diags.count != 0 && diags.dropped == 0 && oracle_accepted;
    for (uint32_t i = 0; i < diags.count; i++) {
        uint16_t code = diags.items[i].code;
        if (code < MO_CODE_LINE_TOO_LONG || code > MO_CODE_TOO_MANY_CASES ||
            (exceeded & bit(code)) == 0)
            excluded = false;
    }
    MO_ASSERT(excluded || accepted == oracle_accepted);
    struct mo_buffer actual, expected;
    mo_buffer_init(&actual, MO_GRAMMAR_ARENA_LIMIT);
    mo_buffer_init(&expected, MO_GRAMMAR_ARENA_LIMIT);
    if (accepted) {
        syntax(&actual, tree->tree, &sources);
        grammar(&expected, root, &sources.files[file]);
        equal(&actual, &expected);
    }
    struct mo_arena small;
    size_t budget = (size == 0 ? 0 : (size_t)data[0] * 256) + (size < 2 ? 0 : data[1]);
    mo_arena_init(&small, budget);
    struct mo_diags failures;
    MO_ASSERT(mo_diags_init(&failures));
    struct mo_module_syntax *limited = fragment
                                           ? mo_parse_fragment(&small, &sources, file, &failures)
                                           : mo_parse(&small, &sources, file, &failures);
    if (limited == NULL)
        allocation(&failures);
    else {
        MO_ASSERT(accepted == !mo_diags_any(&failures));
        if (accepted) {
            mo_buffer_clear(&expected);
            syntax(&expected, limited->tree, &sources);
            equal(&actual, &expected);
        }
    }
    mo_arena_free(&small);
    mo_diags_free(&failures);
    if (!fragment) {
        for (unsigned limited_header = 0; limited_header < 2; limited_header++) {
            mo_arena_init(&small, limited_header == 0 ? MO_GRAMMAR_ARENA_LIMIT : budget);
            MO_ASSERT(mo_diags_init(&failures));
            struct mo_header h = {.span = {123, 456, 789}};
            bool success = mo_read_header(&small, &sources, file, &h, &failures);
            MO_ASSERT(success || (h.span.file == 123 && h.span.start == 456 && h.span.end == 789));
            if (accepted)
                MO_ASSERT(success || limited_header != 0);
            if (success && accepted) {
                MO_ASSERT(h.span.start == tree->header.span.start &&
                          h.span.end == tree->header.span.end);
                mo_buffer_clear(&actual);
                mo_buffer_clear(&expected);
                syntax(&actual, h.path, &sources);
                syntax(&actual, h.expose, &sources);
                syntax(&actual, h.uses, &sources);
                syntax(&expected, tree->header.path, &sources);
                syntax(&expected, tree->header.expose, &sources);
                syntax(&expected, tree->header.uses, &sources);
                equal(&actual, &expected);
            }
            if (!success && accepted && limited_header != 0)
                allocation(&failures);
            mo_arena_free(&small);
            mo_diags_free(&failures);
        }
    }
    mo_buffer_free(&actual);
    mo_buffer_free(&expected);
    mo_arena_free(&oracle);
    mo_arena_free(&arena);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size <= MO_LIMIT_FILE_SIZE) {
        compare(data, size, false);
        compare(data, size, true);
    }
    return 0;
}
