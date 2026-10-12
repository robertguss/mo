// The syntax tree for Mo's executable grammar, and the checks a context-free
// grammar can't make. See ast.h.
#include "ast.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buffer.h"
#include "lexer.h"
#include "mo.tab.h"

// Memory from the parse's arena. The grammar is an instrument, so running out of memory ends
// it, as any other failure to run does.
static void *allocate(struct mo_arena *arena, size_t size) {
    void *memory = mo_arena_allocate(arena, size, _Alignof(max_align_t));
    if (memory == NULL) {
        (void)fprintf(stderr, "mo-grammar: out of memory\n");
        exit(2);
    }
    return memory;
}

struct node *mo_node(struct lexer *lx, enum node_kind kind, int line, int col, int count,
                     struct node *const kids[]) {
    MO_ASSERT(count >= 0 && (count == 0 || kids != NULL));
    struct node *n = allocate(lx->arena, sizeof *n);
    int capacity = count < 4 ? 4 : count;
    *n = (struct node){
        .kind = kind, .line = line, .col = col, .op = 0, .count = count, .capacity = capacity};
    n->kids = allocate(lx->arena, sizeof(struct node *) * (size_t)capacity);
    for (int i = 0; i < count; i++) {
        n->kids[i] = kids[i];
    }
    return n;
}

struct node *mo_node_append(struct lexer *lx, struct node *list, struct node *kid) {
    MO_ASSERT(list != NULL);
    if (list->count == list->capacity) {
        int capacity = list->capacity * 2;
        struct node **kids = allocate(lx->arena, sizeof(struct node *) * (size_t)capacity);
        memcpy(kids, list->kids, sizeof(struct node *) * (size_t)list->count);
        list->kids = kids;
        list->capacity = capacity;
    }
    list->kids[list->count] = kid;
    list->count += 1;
    return list;
}

static bool fail(struct lexer *lx, const struct node *n, const char *message) {
    mo_lexer_error(lx, n->line, n->col, "%s", message);
    return false;
}

struct pending_check {
    const struct node *node;
    bool pattern;
};

static void pending(struct mo_buffer *stack, const struct node *n, bool pattern) {
    if (n == NULL)
        return;
    struct pending_check item = {n, pattern};
    if (!mo_buffer_append(stack, &item, sizeof item)) {
        (void)fprintf(stderr, "mo-grammar: out of memory\n");
        exit(2);
    }
}

static bool is_constructor(const struct node *n) {
    return n->kind == K_CAPNAME ||
           (n->kind == K_QUALIFIED && n->count == 2 && n->kids[1]->kind == K_CAPNAME);
}

static bool check_pattern(struct lexer *lx, const struct node *n);

static bool check_patterns(struct lexer *lx, const struct node *n) {
    for (int i = 0; i < n->count; i++) {
        if (!check_pattern(lx, n->kids[i])) {
            return false;
        }
    }
    return true;
}

static bool check_list_pattern(struct lexer *lx, const struct node *n) {
    for (int i = 0; i < n->count; i++) {
        const struct node *item = n->kids[i];
        if (item->kind == K_REST) {
            if (i != n->count - 1) {
                return fail(lx, item, "..rest can only end a list pattern");
            }
        } else if (!check_pattern(lx, item)) {
            return false;
        }
    }
    return true;
}

static bool check_case_pattern(struct lexer *lx, const struct node *n) {
    if (!is_constructor(n->kids[0])) {
        return fail(lx, n,
                    "a function call can't be matched; a pattern names a case, as in Some(x)");
    }
    const struct node *args = n->kids[1];
    int named = 0;
    for (int i = 0; i < args->count; i++) {
        const struct node *arg = args->kids[i];
        bool is_named = arg->kind == K_NAMED_ARG;
        named += is_named;
        if (!check_pattern(lx, is_named ? arg->kids[1] : arg)) {
            return false;
        }
    }
    if (args->count == 0) {
        return fail(lx, n, "a case with no fields is matched without parentheses");
    }
    if (named != args->count && !(named == 0 && args->count == 1)) {
        return fail(
            lx, n,
            "a case's fields are matched by name, as in Short(by: amount), unless it has one "
            "unnamed field, as in Some(x)");
    }
    return true;
}

// A pattern: `_`, a name, a literal, a case with its fields, a tuple, or a list.
static bool check_pattern(struct lexer *lx, const struct node *n) {
    enum node_kind kind = n->kind;
    if (kind == K_UNDERSCORE || kind == K_NAME || kind == K_INT || kind == K_DECIMAL ||
        kind == K_CHAR || kind == K_TEXT || kind == K_TRUE || kind == K_FALSE ||
        kind == K_CAPNAME) {
        return true;
    }
    if (kind == K_NEGATE && (n->kids[0]->kind == K_INT || n->kids[0]->kind == K_DECIMAL)) {
        return true;
    }
    if (kind == K_QUALIFIED && is_constructor(n)) {
        return true;
    }
    if (kind == K_GROUP) {
        return check_pattern(lx, n->kids[0]);
    }
    if (kind == K_TUPLE) {
        return check_patterns(lx, n);
    }
    if (kind == K_LIST) {
        return check_list_pattern(lx, n);
    }
    if (kind == K_CALL) {
        return check_case_pattern(lx, n);
    }
    return fail(
        lx, n,
        "this can't be matched: a pattern is a name, _, a literal, a case, a tuple or a list");
}

// What may stand on the left of `=`, `+=` and `-=`.
static MO_MUST_USE bool is_place(const struct node *n) {
    while (n->kind == K_FIELD_ACCESS)
        n = n->kids[0];
    return n->kind == K_NAME;
}

static bool is_name_tuple(const struct node *n) {
    if (n->kind != K_TUPLE) {
        return false;
    }
    for (int i = 0; i < n->count; i++) {
        const struct node *kid = n->kids[i];
        if (kid->kind != K_NAME && kid->kind != K_UNDERSCORE && !is_name_tuple(kid)) {
            return false;
        }
    }
    return true;
}

static MO_MUST_USE bool check_call(struct lexer *lx, const struct node *n,
                                   struct mo_buffer *stack) {
    const struct node *callee = n->kids[0];
    const struct node *args = n->kids[1];
    int named = 0;
    for (int i = 0; i < args->count; i++) {
        named += args->kids[i]->kind == K_NAMED_ARG;
    }
    if (is_constructor(callee)) {
        if (args->count == 0) {
            return fail(lx, n, "a record or case with no fields is written without parentheses");
        }
        if (named != args->count && !(named == 0 && args->count == 1)) {
            return fail(lx, n,
                        "a record or case is built with named fields, as in Point(x: 1, y: 2), "
                        "unless it has one unnamed field, as in Some(x)");
        }
    } else if (named > 0) {
        return fail(lx, n,
                    "a function's arguments are passed by position; only records and cases are "
                    "built with names");
    }
    for (int i = args->count; i > 0; i--) {
        const struct node *arg = args->kids[i - 1];
        pending(stack, arg->kind == K_NAMED_ARG ? arg->kids[1] : arg, false);
    }
    pending(stack, callee, false);
    return true;
}

static MO_MUST_USE bool check_kids(const struct node *n, int from, struct mo_buffer *stack) {
    for (int i = n->count; i > from; i--)
        pending(stack, n->kids[i - 1], false);
    return true;
}

static MO_MUST_USE bool check(struct lexer *lx, const struct node *n, struct mo_buffer *stack) {
    if (n == NULL) {
        return true;
    }
    switch (n->kind) {
    case K_UNDERSCORE:
        return fail(lx, n, "_ stands only in a pattern, a loop's name or a function's parameter");
    case K_REST:
        return fail(lx, n, "..rest stands only at the end of a list pattern");
    case K_TYPE:
    case K_PARAM:
    case K_FIELD:
    case K_WHERE:
    case K_EXPOSE:
    case K_EXPOSE_ALL:
    case K_USE:
    case K_BINDER:
    case K_TOKEN:
        return true;
    case K_STRUCT:
        if (n->kids[2]->count == 0) {
            return fail(lx, n, "a struct has at least one field");
        }
        return true;
    case K_ENUM:
        if (n->kids[2]->count == 0) {
            return fail(lx, n, "an enum has at least one case");
        }
        return true;
    case K_FN:
        return check_kids(n, 4, stack);
    case K_PROMISE: {
        bool allows = false;
        bool forbids = false;
        const struct node *anchors = n->kids[2];
        for (int i = 0; i < anchors->count; i++) {
            allows = allows || anchors->kids[i]->op == ALLOWS;
            forbids = forbids || anchors->kids[i]->op == FORBIDS;
        }
        if (!allows || !forbids) {
            return fail(lx, n, "a promise has at least one allows line and one forbids line");
        }
        return check_kids(n, 1, stack);
    }
    case K_NAMED_ARG:
    case K_TEST:
    case K_CONST:
        pending(stack, n->kids[1], false);
        return true;
    case K_BIND:
        if (!is_place(n->kids[0]) && !is_name_tuple(n->kids[0])) {
            return fail(lx, n->kids[0],
                        "the left of = is a name, a var's field, or a tuple of names, as in "
                        "(low, high)");
        }
        pending(stack, n->kids[1], false);
        return true;
    case K_ADD_ASSIGN:
    case K_SUB_ASSIGN:
        if (!is_place(n->kids[0])) {
            return fail(lx, n->kids[0], "the left of += or -= is a var or a var's field");
        }
        pending(stack, n->kids[1], false);
        return true;
    case K_VAR:
        pending(stack, n->kids[1], false);
        return true;
    case K_FOR:
        return check_kids(n, 1, stack);
    case K_LAMBDA:
        pending(stack, n->kids[1], false);
        return true;
    case K_IS:
        pending(stack, n->kids[1], true);
        pending(stack, n->kids[0], false);
        return true;
    case K_CASE: {
        const struct node *lines = n->kids[1];
        if (lines->count == 0 || lines->kids[0]->kind != K_ARM) {
            return fail(lx, n, "a case starts with an arm: Pattern: value");
        }
        return check_kids(n, 0, stack);
    }
    case K_ARM: {
        const struct node *alternatives = n->kids[0];
        pending(stack, n->kids[2], false);
        pending(stack, n->kids[1], false);
        for (int i = alternatives->count; i > 0; i--)
            pending(stack, alternatives->kids[i - 1], true);
        return true;
    }
    case K_CALL:
        return check_call(lx, n, stack);
    case K_FIELD_ACCESS:
    case K_QUALIFIED:
    case K_POSITION:
        pending(stack, n->kids[0], false);
        return true;
    case K_FILE:
    case K_MODULE:
    case K_CASE_DECL:
    case K_ANCHOR:
    case K_EXAMPLES:
    case K_LIST_OF:
    case K_EXPR_STMT:
    case K_RETURN:
    case K_BREAK:
    case K_CONTINUE:
    case K_ASSERT:
    case K_WHILE:
    case K_IF:
    case K_ELSE:
    case K_LINE_IF:
    case K_BINARY:
    case K_NEGATE:
    case K_NOT:
    case K_TRY:
    case K_RANGE:
    case K_INT:
    case K_DECIMAL:
    case K_CHAR:
    case K_TEXT:
    case K_TEXT_INTERPOLATED:
    case K_TRUE:
    case K_FALSE:
    case K_NAME:
    case K_CAPNAME:
    case K_GROUP:
    case K_TUPLE:
    case K_LIST:
    case K_MAP:
    case K_MAP_ENTRY:
    case K_STOP:
        return check_kids(n, 0, stack);
    }
    MO_UNREACHABLE();
}

bool mo_validate(struct lexer *lx, const struct node *root) {
    MO_ASSERT(root != NULL);
    struct mo_buffer stack;
    mo_buffer_init(&stack, MO_GRAMMAR_ARENA_LIMIT);
    pending(&stack, root, false);
    bool valid = true;
    // A left-recursive grammar gives flat chains nested trees. Keep their
    // traversal off the C stack, preserving the original first-error order.
    while (valid && stack.length != 0) {
        struct pending_check item;
        MO_ASSERT(stack.length >= sizeof item);
        stack.length -= sizeof item;
        memcpy(&item, stack.bytes + stack.length, sizeof item);
        valid = item.pattern ? check_pattern(lx, item.node) : check(lx, item.node, &stack);
    }
    mo_buffer_free(&stack);
    return valid;
}
