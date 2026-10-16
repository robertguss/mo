/* Constant types come from their values, not from the functions that use them. */
#include "check/checked.h"
#include "check/signatures.h"

#include "base/codes.h"
#include "base/limits.h"

#include <stdlib.h>
#include <string.h>

static MO_MUST_USE bool report(struct mo_diags *diags, uint16_t code, struct mo_span span,
                               const char *message, const char *reason) {
    bool kept = mo_diags_add(
        diags, &(struct mo_diag){
                   .code = code, .primary = {span, message}, .message = message, .reason = reason});
    (void)kept;
    return false;
}

static MO_MUST_USE bool work(const struct mo_signatures *signatures, struct mo_diags *diags,
                             struct mo_span span) {
    if (*signatures->work == MO_LIMIT_PROGRAM_CHECK_WORK)
        return report(diags, MO_CODE_CHECK_PROGRAM_WORK, span,
                      "the program needs too much constant-checking work", NULL);
    (*signatures->work)++;
    return true;
}

static MO_MUST_USE void *allocate(struct mo_arena *arena, struct mo_diags *diags,
                                  struct mo_span span, uint32_t count, size_t size,
                                  size_t alignment) {
    void *out = mo_arena_allocate_array(arena, count, size, alignment);
    if (out == NULL) {
        bool reported =
            report(diags, MO_CODE_CHECK_NO_MEMORY, span, "the checking arena has no room", NULL);
        (void)reported;
    }
    return out;
}

static MO_MUST_USE int source_order(const void *a, const void *b) {
    const struct mo_checked_constant *x = a, *y = b;
    if (x->symbol->module != y->symbol->module)
        return x->symbol->module < y->symbol->module ? -1 : 1;
    uint32_t left = x->symbol->declaration->span.start, right = y->symbol->declaration->span.start;
    return left < right ? -1 : left > right ? 1 : 0;
}

static MO_MUST_USE int symbol_order(const void *a, const void *b) {
    const struct mo_checked_constant *x = *(const struct mo_checked_constant *const *)a;
    const struct mo_checked_constant *y = *(const struct mo_checked_constant *const *)b;
    uintptr_t left = (uintptr_t)x->symbol, right = (uintptr_t)y->symbol;
    return left < right ? -1 : left > right ? 1 : 0;
}

struct mo_checked_constant *mo_constant_by_symbol(const struct mo_constant_context *constants,
                                                  const struct mo_symbol *symbol) {
    MO_ASSERT(constants != NULL && symbol != NULL);
    uint32_t lo = 0, hi = constants->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if ((uintptr_t)constants->by_symbol[mid]->symbol < (uintptr_t)symbol)
            lo = mid + 1;
        else
            hi = mid;
    }
    MO_ASSERT(lo < constants->count && constants->by_symbol[lo]->symbol == symbol);
    return constants->by_symbol[lo];
}

bool mo_check_constants(struct mo_arena *arena, const struct mo_module_interfaces *interfaces,
                        const struct mo_signatures *signatures,
                        struct mo_constant_context *constants, struct mo_diags *diags) {
    struct mo_span span = {0};
    uint32_t count = 0;
    for (uint32_t i = 0; i < interfaces->count; i++)
        for (uint32_t j = 0; j < interfaces->scopes[i].count; j++) {
            if (!work(signatures, diags, span))
                return false;
            if (interfaces->scopes[i].symbols[j].declaration->kind == MO_S_CONST) {
                span = interfaces->scopes[i].symbols[j].declaration->span;
                count++;
            }
        }
    *constants = (struct mo_constant_context){.count = count};
    constants->items = allocate(arena, diags, span, count, sizeof(*constants->items),
                                _Alignof(struct mo_checked_constant));
    constants->by_symbol = allocate(arena, diags, span, count, sizeof(*constants->by_symbol),
                                    _Alignof(struct mo_checked_constant *));
    constants->done = allocate(arena, diags, span, count, sizeof(*constants->done), _Alignof(bool));
    bool *active = allocate(arena, diags, span, count, sizeof(*active), _Alignof(bool));
    uint32_t *stack = allocate(arena, diags, span, count, sizeof(*stack), _Alignof(uint32_t));
    if (constants->items == NULL || constants->by_symbol == NULL || constants->done == NULL ||
        active == NULL || stack == NULL)
        return false;
    uint32_t index = 0;
    for (uint32_t i = 0; i < interfaces->count; i++)
        for (uint32_t j = 0; j < interfaces->scopes[i].count; j++) {
            const struct mo_symbol *symbol = &interfaces->scopes[i].symbols[j];
            if (symbol->declaration->kind == MO_S_CONST)
                constants->items[index++] = (struct mo_checked_constant){.symbol = symbol};
        }
    MO_ASSERT(index == count);
    qsort(constants->items, count, sizeof(*constants->items), source_order);
    for (uint32_t i = 0; i < count; i++) {
        constants->by_symbol[i] = &constants->items[i];
        constants->done[i] = active[i] = false;
    }
    qsort(constants->by_symbol, count, sizeof(*constants->by_symbol), symbol_order);
    for (uint32_t start = 0; start < count; start++) {
        if (constants->done[start])
            continue;
        uint32_t depth = 1;
        stack[0] = start;
        active[start] = true;
        while (depth != 0) {
            if (!work(signatures, diags, span))
                return false;
            index = stack[depth - 1];
            constants->pending = NULL;
            bool halt;
            bool valid = mo_check_constant(arena, interfaces, signatures, constants,
                                           &constants->items[index], diags, &halt);
            if (!valid && halt)
                return false;
            if (constants->pending != NULL) {
                uint32_t target = (uint32_t)(constants->pending - constants->items);
                MO_ASSERT(target < count && !constants->done[target]);
                if (active[target])
                    return report(diags, MO_CODE_CONSTANT_CYCLE, constants->pending_span,
                                  "constants depend on each other in a cycle",
                                  "Constants are computed in the order of their dependencies; "
                                  "a cycle is an error (reference 10.4).");
                MO_ASSERT(depth < count);
                stack[depth++] = target;
                active[target] = true;
            } else {
                constants->done[index] = true;
                active[index] = false;
                depth--;
            }
        }
    }
    return true;
}

struct edge {
    uint32_t target;
    struct mo_span span;
    struct edge *next;
};

struct vertex {
    struct edge *edges;
    bool unknown;
    struct mo_span span;
};

struct proof_seen {
    const struct mo_ability_implementation *proof;
    struct proof_seen *next;
};

struct dependencies {
    struct mo_arena *arena;
    struct mo_program *program;
    const struct mo_constant_context *constants;
    struct mo_diags *diags;
    struct vertex *vertices;
    uint32_t *functions;
    struct proof_seen *seen;
};

static MO_MUST_USE bool add_edge(struct dependencies *p, uint32_t from, uint32_t target,
                                 struct mo_span span) {
    if (!work(p->program->signatures, p->diags, span))
        return false;
    struct edge *edge = allocate(p->arena, p->diags, span, 1, sizeof(*edge), _Alignof(struct edge));
    if (edge == NULL)
        return false;
    *edge = (struct edge){target, span, p->vertices[from].edges};
    p->vertices[from].edges = edge;
    return true;
}

static MO_MUST_USE uint32_t function_index(const struct dependencies *p,
                                           const struct mo_symbol *symbol) {
    const struct mo_function_signature *signature =
        mo_function_signature_by_symbol(p->program->signatures, symbol);
    MO_ASSERT(signature != NULL);
    return p->constants->count + p->functions[signature - p->program->signatures->items];
}

static MO_MUST_USE bool higher_order_native(const char *native) {
    return native != NULL && strcmp(native, "List.map") == 0;
}

/* Resolve only a known function value, never a function call's returned value.
 * Unresolved higher-order calls in an initializer stay explicitly staged. */
static MO_MUST_USE bool callee(struct dependencies *p, const struct mo_checked_body *body,
                               uint32_t at, uint32_t *target, uint32_t *local_lambda) {
    const struct mo_checked_body *initial = body;
    for (;;) {
        MO_ASSERT(at < body->count);
        const struct mo_checked_expression *row = &body->items[at];
        if (!work(p->program->signatures, p->diags, row->syntax->span))
            return false;
        if (row->operand_count == 0 && row->symbol != NULL) {
            if (row->symbol->declaration->kind == MO_S_FN) {
                *target = function_index(p, row->symbol);
                return true;
            }
            if (row->symbol->declaration->kind == MO_S_CONST) {
                const struct mo_checked_constant *constant =
                    mo_constant_by_symbol(p->constants, row->symbol);
                body = &constant->initializer;
                at = body->root;
                continue;
            }
        }
        if (row->operand_count == 0 && row->native != NULL) {
            if (!higher_order_native(row->native))
                *target = UINT32_MAX;
            return true;
        }
        if (local_lambda != NULL && body == initial && row->lambda != NULL) {
            MO_ASSERT(row->operand_count == 1);
            *local_lambda = row->operands[0];
            *target = UINT32_MAX;
            return true;
        }
        if (row->syntax->kind == MO_S_GROUP && row->operand_count == 1) {
            at = row->operands[0];
            continue;
        }
        if (row->binding != NULL && !row->binding->variable) {
            uint32_t i = 0;
            for (; i < body->count; i++) {
                if (!work(p->program->signatures, p->diags, row->syntax->span))
                    return false;
                const struct mo_checked_expression *definition = &body->items[i];
                if (definition->binding == row->binding && definition->operand_count == 1 &&
                    !definition->assignment && definition->syntax->kind == MO_S_BIND)
                    break;
            }
            if (i < body->count) {
                at = body->items[i].operands[0];
                continue;
            }
        }
        return true; /* UINT32_MAX - 1 remains unknown. */
    }
}

static MO_MUST_USE bool proof_edges(struct dependencies *p, uint32_t from,
                                    const struct mo_ability_implementation *proof,
                                    struct mo_span span, uint32_t depth) {
    if (!work(p->program->signatures, p->diags, span))
        return false;
    if (depth > MO_LIMIT_SOURCE_ABILITY_DEPTH)
        return report(p->diags, MO_CODE_CHECK_ABILITY_DEPTH, span,
                      "the ability dependency path is too deep", NULL);
    for (struct proof_seen *seen = p->seen; seen != NULL; seen = seen->next) {
        if (!work(p->program->signatures, p->diags, span))
            return false;
        if (seen->proof == proof)
            return true;
    }
    struct proof_seen *seen =
        allocate(p->arena, p->diags, span, 1, sizeof(*seen), _Alignof(struct proof_seen));
    if (seen == NULL)
        return false;
    *seen = (struct proof_seen){proof, p->seen};
    p->seen = seen;
    if (proof->kind == MO_ABILITY_FUNCTION)
        return add_edge(p, from, function_index(p, proof->function), span);
    if (proof->kind == MO_ABILITY_PARAMETER &&
        (proof->ability == MO_SOURCE_TEXT || proof->ability == MO_SOURCE_ORDER)) {
        p->vertices[from].unknown = true;
        p->vertices[from].span = span;
    }
    for (uint32_t i = 0; i < proof->part_count; i++)
        if (!proof_edges(p, from, proof->parts[i], span, depth + 1))
            return false;
    return true;
}

static MO_MUST_USE bool body_edges(struct dependencies *p, uint32_t from,
                                   const struct mo_checked_body *body) {
    MO_ASSERT(body->root < body->count);
    struct mo_span span = body->items[body->root].syntax->span;
    bool *seen = allocate(p->arena, p->diags, span, body->count, sizeof(*seen), _Alignof(bool));
    uint32_t *stack =
        allocate(p->arena, p->diags, span, body->count, sizeof(*stack), _Alignof(uint32_t));
    if (seen == NULL || stack == NULL)
        return false;
    memset(seen, 0, body->count * sizeof(*seen));
    uint32_t depth = 1;
    stack[0] = body->root;
    seen[body->root] = true;
    p->seen = NULL;
    while (depth != 0) {
        const struct mo_checked_expression *row = &body->items[stack[--depth]];
        span = row->syntax->span;
        if (!work(p->program->signatures, p->diags, span))
            return false;
        if (row->lambda != NULL)
            continue; /* Making a function does not execute its body. */
        if (row->symbol != NULL && row->symbol->declaration->kind == MO_S_CONST) {
            uint32_t target =
                (uint32_t)(mo_constant_by_symbol(p->constants, row->symbol) - p->constants->items);
            if (!add_edge(p, from, target, span))
                return false;
        }
        const struct mo_syntax *syntax = row->syntax;
        if (syntax->kind == MO_S_POSTFIX)
            syntax = syntax->children[syntax->count - 1];
        bool callable = row->operand_count != 0 &&
                        body->items[row->operands[0]].type->kind == MO_SOURCE_FUNCTION;
        bool function = row->symbol != NULL && row->symbol->declaration->kind == MO_S_FN;
        if (row->native == NULL &&
            ((syntax->kind == MO_S_CALL && callable &&
              (row->symbol == NULL || function || row->symbol->declaration->kind == MO_S_CONST)) ||
             (function && row->operand_count != 0 && syntax->kind != MO_S_GROUP))) {
            uint32_t target = UINT32_MAX - 1;
            if (!callee(p, body, row->operands[0], &target, NULL))
                return false;
            if (target == UINT32_MAX - 1) {
                p->vertices[from].unknown = true;
                p->vertices[from].span = span;
            } else if (target != UINT32_MAX && !add_edge(p, from, target, span))
                return false;
        }
        if (higher_order_native(row->native) && row->operand_count != 0) {
            uint32_t first = row->receiver == UINT32_MAX && callable ? 1u : 0u;
            for (uint32_t i = first; i < row->operand_count; i++) {
                if (body->items[row->operands[i]].type->kind != MO_SOURCE_FUNCTION)
                    continue;
                uint32_t target = UINT32_MAX - 1, local_lambda = UINT32_MAX;
                if (!callee(p, body, row->operands[i], &target, &local_lambda))
                    return false;
                if (local_lambda != UINT32_MAX) {
                    if (!seen[local_lambda]) {
                        seen[local_lambda] = true;
                        MO_ASSERT(depth < body->count);
                        stack[depth++] = local_lambda;
                    }
                } else if (target == UINT32_MAX - 1) {
                    p->vertices[from].unknown = true;
                    p->vertices[from].span = span;
                } else if (target != UINT32_MAX && !add_edge(p, from, target, span))
                    return false;
            }
        }
        for (uint32_t i = 0; i < row->ability_count; i++)
            if (row->abilities[i].operand != UINT32_MAX &&
                !proof_edges(p, from, row->abilities[i].implementation, span, 0))
                return false;
        for (uint32_t i = 0; i < row->operand_count; i++) {
            uint32_t target = row->operands[i];
            if (seen[target])
                continue;
            seen[target] = true;
            MO_ASSERT(depth < body->count);
            stack[depth++] = target;
        }
    }
    return true;
}

static void push(uint32_t *heap, uint32_t *count, uint32_t value) {
    uint32_t at = (*count)++;
    while (at != 0 && heap[(at - 1) / 2] > value) {
        heap[at] = heap[(at - 1) / 2];
        at = (at - 1) / 2;
    }
    heap[at] = value;
}

static MO_MUST_USE uint32_t pop(uint32_t *heap, uint32_t *count) {
    MO_ASSERT(*count != 0);
    uint32_t first = heap[0], value = heap[--*count], at = 0;
    while (at < *count / 2) {
        uint32_t child = 2 * at + 1;
        if (child + 1 < *count && heap[child + 1] < heap[child])
            child++;
        if (heap[child] >= value)
            break;
        heap[at] = heap[child];
        at = child;
    }
    heap[at] = value;
    return first;
}

bool mo_order_constants(struct mo_arena *arena, struct mo_program *program,
                        const struct mo_constant_context *constants, struct mo_diags *diags) {
    uint32_t count = constants->count;
    if (count == 0)
        return true;
    struct mo_span span = constants->items[0].symbol->declaration->span;
    uint32_t vertices = count + program->function_count;
    struct dependencies p = {
        .arena = arena, .program = program, .constants = constants, .diags = diags};
    p.vertices =
        allocate(arena, diags, span, vertices, sizeof(*p.vertices), _Alignof(struct vertex));
    p.functions = allocate(arena, diags, span, program->function_count, sizeof(*p.functions),
                           _Alignof(uint32_t));
    struct vertex *reverse =
        allocate(arena, diags, span, count, sizeof(*reverse), _Alignof(struct vertex));
    uint32_t *seen = allocate(arena, diags, span, vertices, sizeof(*seen), _Alignof(uint32_t));
    uint32_t *stack = allocate(arena, diags, span, vertices, sizeof(*stack), _Alignof(uint32_t));
    uint32_t *pending = allocate(arena, diags, span, count, sizeof(*pending), _Alignof(uint32_t));
    uint32_t *heap = allocate(arena, diags, span, count, sizeof(*heap), _Alignof(uint32_t));
    struct mo_checked_constant *ordered =
        allocate(arena, diags, span, count, sizeof(*ordered), _Alignof(struct mo_checked_constant));
    if (p.vertices == NULL || p.functions == NULL || reverse == NULL || seen == NULL ||
        stack == NULL || pending == NULL || heap == NULL || ordered == NULL)
        return false;
    memset(p.vertices, 0, vertices * sizeof(*p.vertices));
    memset(reverse, 0, count * sizeof(*reverse));
    memset(seen, 0, vertices * sizeof(*seen));
    for (uint32_t i = 0; i < program->function_count; i++)
        p.functions[program->functions[i].signature - program->signatures->items] = i;
    for (uint32_t i = 0; i < vertices; i++) {
        const struct mo_checked_body *body =
            i < count ? &constants->items[i].initializer : &program->functions[i - count].body;
        if (!body_edges(&p, i, body))
            return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        pending[i] = 0;
        uint32_t depth = 1, epoch = i + 1;
        stack[0] = i;
        seen[i] = epoch;
        while (depth != 0) {
            uint32_t at = stack[--depth];
            if (p.vertices[at].unknown)
                return report(diags, MO_CODE_CHECK_UNFINISHED, p.vertices[at].span,
                              "this constant's call dependencies are not implemented", NULL);
            for (struct edge *edge = p.vertices[at].edges; edge != NULL; edge = edge->next) {
                if (!work(program->signatures, diags, edge->span))
                    return false;
                uint32_t target = edge->target;
                /* A return path to this constant is still a dependency. */
                if (target < count && (target == i || seen[target] != epoch)) {
                    struct edge *back =
                        allocate(arena, diags, edge->span, 1, sizeof(*back), _Alignof(struct edge));
                    if (back == NULL)
                        return false;
                    *back = (struct edge){i, edge->span, reverse[target].edges};
                    reverse[target].edges = back;
                    pending[i]++;
                }
                if (seen[target] == epoch)
                    continue;
                seen[target] = epoch;
                if (target >= count) {
                    MO_ASSERT(depth < vertices);
                    stack[depth++] = target;
                }
            }
        }
    }
    uint32_t ready = 0, output = 0;
    for (uint32_t i = 0; i < count; i++)
        if (pending[i] == 0)
            push(heap, &ready, i);
    while (ready != 0) {
        uint32_t at = pop(heap, &ready);
        ordered[output++] = constants->items[at];
        for (struct edge *edge = reverse[at].edges; edge != NULL; edge = edge->next) {
            MO_ASSERT(pending[edge->target] != 0);
            pending[edge->target]--;
            if (pending[edge->target] == 0)
                push(heap, &ready, edge->target);
        }
    }
    if (output != count) {
        uint32_t first = 0;
        while (pending[first] == 0)
            first++;
        return report(diags, MO_CODE_CONSTANT_CYCLE,
                      constants->items[first].symbol->declaration->children[0]->span,
                      "constants depend on each other in a cycle",
                      "A constant depends on the constants it uses, directly or through the "
                      "functions it calls; a cycle is an error (reference 10.4).");
    }
    program->constants = ordered;
    return true;
}
