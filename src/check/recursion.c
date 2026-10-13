/* Instantiation dependencies, including function values and ability callbacks. */
#include "check/checked.h"

#include "base/codes.h"
#include "base/limits.h"

#include <stdio.h>

struct edge {
    uint32_t target;
    const struct mo_symbol *caller, *callee;
    bool grows;
    struct mo_span span;
    struct edge *next;
};

struct proof_seen {
    const struct mo_ability_implementation *proof;
    struct proof_seen *next;
};

struct frame {
    uint32_t function;
    const struct edge *next;
};

struct recursion {
    struct mo_arena *arena;
    const struct mo_program *program;
    struct mo_diags *diags;
    struct mo_span span;
    uint32_t *indices;
    uint32_t *offsets;
    struct edge **edges;
    struct proof_seen *seen;
    bool failed;
};

static MO_MUST_USE bool report(struct recursion *p, uint16_t code, const char *message,
                               const char *reason) {
    bool kept = mo_diags_add(p->diags, &(struct mo_diag){.code = code,
                                                         .primary = {p->span, message},
                                                         .message = message,
                                                         .reason = reason});
    (void)kept;
    p->failed = true;
    return false;
}

static MO_MUST_USE bool work(struct recursion *p) {
    if (p->failed)
        return false;
    if (*p->program->signatures->work == MO_LIMIT_PROGRAM_CHECK_WORK)
        return report(p, MO_CODE_CHECK_PROGRAM_WORK,
                      "the program needs too much recursion-checking work", NULL);
    (*p->program->signatures->work)++;
    return true;
}

static MO_MUST_USE void *allocate(struct recursion *p, uint32_t count, size_t size,
                                  size_t alignment) {
    if (!work(p))
        return NULL;
    void *out = mo_arena_allocate_array(p->arena, count, size, alignment);
    if (out == NULL) {
        bool failed = report(p, MO_CODE_CHECK_NO_MEMORY, "the checking arena has no room", NULL);
        (void)failed;
    }
    return out;
}

static MO_MUST_USE uint32_t parameter_node(const struct recursion *p, uint32_t function,
                                           unsigned letter) {
    uint32_t mask = p->program->functions[function].signature->type->parameters;
    MO_ASSERT(letter < 26 && (mask & (UINT32_C(1) << letter)) != 0);
    uint32_t node = p->offsets[function];
    for (unsigned i = 0; i < letter; i++)
        node += (mask & (UINT32_C(1) << i)) != 0 ? 1u : 0u;
    return node;
}

static MO_MUST_USE bool add(struct recursion *p, uint32_t caller, const struct mo_symbol *symbol,
                            const struct mo_type_arguments *arguments) {
    if (symbol == NULL || symbol->declaration->kind != MO_S_FN || arguments->count == 0)
        return true;
    const struct mo_signatures *signatures = p->program->signatures;
    uint32_t lo = 0, hi = signatures->count;
    while (lo < hi) {
        if (!work(p))
            return false;
        uint32_t mid = lo + (hi - lo) / 2;
        const struct mo_symbol *at = signatures->items[mid].function;
        if (at->module < symbol->module ||
            (at->module == symbol->module && (uintptr_t)at < (uintptr_t)symbol))
            lo = mid + 1;
        else
            hi = mid;
    }
    MO_ASSERT(lo < signatures->count && signatures->items[lo].function == symbol);
    for (uint32_t i = 0; i < arguments->count; i++) {
        const struct mo_type_argument *argument = &arguments->items[i];
        unsigned target_letter = (unsigned)(argument->parameter->name[0] - 'A');
        for (unsigned letter = 0; letter < 26; letter++) {
            if (!work(p))
                return false;
            if ((argument->type->parameters & (UINT32_C(1) << letter)) == 0)
                continue;
            uint32_t source = parameter_node(p, caller, letter);
            struct edge *edge = allocate(p, 1, sizeof(*edge), _Alignof(struct edge));
            if (edge == NULL)
                return false;
            *edge = (struct edge){.target = parameter_node(p, p->indices[lo], target_letter),
                                  .caller = p->program->functions[caller].signature->function,
                                  .callee = symbol,
                                  .grows = argument->type->kind != MO_SOURCE_PARAMETER,
                                  .span = p->span,
                                  .next = p->edges[source]};
            p->edges[source] = edge;
        }
    }
    return true;
}

static MO_MUST_USE bool proofs(struct recursion *p, uint32_t caller,
                               const struct mo_ability_implementation *proof, uint32_t depth) {
    if (!work(p))
        return false;
    if (depth > MO_LIMIT_SOURCE_TYPE_DEPTH)
        return report(p, MO_CODE_CHECK_TYPE_DEPTH, "the ability dependency path is too deep", NULL);
    for (struct proof_seen *at = p->seen; at != NULL; at = at->next) {
        if (!work(p))
            return false;
        if (at->proof == proof)
            return true;
    }
    struct proof_seen *seen = allocate(p, 1, sizeof(*seen), _Alignof(struct proof_seen));
    if (seen == NULL)
        return false;
    *seen = (struct proof_seen){proof, p->seen};
    p->seen = seen;
    if (!add(p, caller, proof->function, &proof->arguments))
        return false;
    for (uint32_t i = 0; i < proof->part_count; i++)
        if (!proofs(p, caller, proof->parts[i], depth + 1))
            return false;
    return true;
}

bool mo_check_recursion(struct mo_arena *arena, const struct mo_program *program,
                        struct mo_diags *diags) {
    struct recursion p = {
        .arena = arena, .program = program, .diags = diags, .span = program->set->modules[0]->span};
    p.indices = allocate(&p, program->function_count, sizeof(*p.indices), _Alignof(uint32_t));
    p.offsets = allocate(&p, program->function_count, sizeof(*p.offsets), _Alignof(uint32_t));
    if (p.failed)
        return false;
    uint32_t count = 0;
    for (uint32_t i = 0; i < program->function_count; i++) {
        p.indices[(uint32_t)(program->functions[i].signature - program->signatures->items)] = i;
        p.offsets[i] = count;
        for (unsigned j = 0; j < 26; j++) {
            if (!work(&p))
                return false;
            count += (program->functions[i].signature->type->parameters & (UINT32_C(1) << j)) != 0
                         ? 1u
                         : 0u;
        }
    }
    if (count == 0)
        return true;
    p.edges = allocate(&p, count, sizeof(*p.edges), _Alignof(struct edge *));
    uint32_t *index = allocate(&p, count, sizeof(*index), _Alignof(uint32_t));
    uint32_t *low = allocate(&p, count, sizeof(*low), _Alignof(uint32_t));
    uint32_t *component = allocate(&p, count, sizeof(*component), _Alignof(uint32_t));
    uint32_t *stack = allocate(&p, count, sizeof(*stack), _Alignof(uint32_t));
    struct frame *frames = allocate(&p, count, sizeof(*frames), _Alignof(struct frame));
    if (p.failed)
        return false;
    for (uint32_t i = 0; i < count; i++) {
        index[i] = component[i] = UINT32_MAX;
        p.edges[i] = NULL;
    }
    for (uint32_t i = 0; i < program->function_count; i++) {
        if (program->functions[i].signature->type->parameters == 0)
            continue;
        p.seen = NULL;
        const struct mo_checked_body *body = &program->functions[i].body;
        for (uint32_t j = 0; j < body->count; j++) {
            const struct mo_checked_expression *row = &body->items[j];
            p.span = row->syntax->span;
            if (!work(&p) || !add(&p, i, row->symbol, &row->arguments))
                return false;
            for (uint32_t k = 0; k < row->ability_count; k++)
                if (!proofs(&p, i, row->abilities[k].implementation, 0))
                    return false;
        }
    }
    uint32_t next = 0, stacked = 0, groups = 0;
    for (uint32_t start = 0; start < count; start++) {
        if (index[start] != UINT32_MAX)
            continue;
        index[start] = low[start] = next++;
        stack[stacked++] = start;
        frames[0] = (struct frame){start, p.edges[start]};
        uint32_t depth = 1;
        while (depth != 0) {
            if (!work(&p))
                return false;
            struct frame *frame = &frames[depth - 1];
            uint32_t function = frame->function;
            if (frame->next != NULL) {
                uint32_t target = frame->next->target;
                frame->next = frame->next->next;
                if (index[target] == UINT32_MAX) {
                    index[target] = low[target] = next++;
                    MO_ASSERT(stacked < count && depth < count);
                    stack[stacked++] = target;
                    frames[depth++] = (struct frame){target, p.edges[target]};
                } else if (component[target] == UINT32_MAX && index[target] < low[function])
                    low[function] = index[target];
                continue;
            }
            if (low[function] == index[function]) {
                uint32_t member;
                do {
                    MO_ASSERT(stacked != 0);
                    member = stack[--stacked];
                    component[member] = groups;
                } while (member != function);
                groups++;
            }
            depth--;
            if (depth != 0 && low[function] < low[frames[depth - 1].function])
                low[frames[depth - 1].function] = low[function];
        }
    }
    for (uint32_t i = 0; i < count; i++)
        for (const struct edge *edge = p.edges[i]; edge != NULL; edge = edge->next) {
            if (!work(&p))
                return false;
            if (!edge->grows || component[i] != component[edge->target])
                continue;
            p.span = edge->span;
            char *message = allocate(&p, 512, 1, 1);
            if (message == NULL)
                return false;
            int length = snprintf(message, 512,
                                  "a generic recursion builds a bigger type: %.128s calls %.128s "
                                  "in a type-parameter cycle",
                                  edge->caller->name, edge->callee->name);
            MO_ASSERT(length > 0 && length < 512);
            return report(&p, MO_CODE_POLYMORPHIC_RECURSION, message,
                          "A generic function can't reach itself with a bigger type built "
                          "from its parameter (reference 5.5).");
        }
    return true;
}
