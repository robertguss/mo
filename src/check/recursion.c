#include "check/checked.h"
#include "check/signatures.h"

#include "base/codes.h"
#include "base/limits.h"

#include <stdio.h>
#include <string.h>

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
    const struct mo_signatures *signatures;
    struct mo_diags *diags;
    struct mo_span span;
    uint32_t *indices;
    uint32_t *offsets;
    uint32_t *masks;
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
    if (*p->signatures->work == MO_LIMIT_PROGRAM_CHECK_WORK)
        return report(p, MO_CODE_CHECK_PROGRAM_WORK,
                      "the program needs too much recursion-checking work", NULL);
    (*p->signatures->work)++;
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
    uint32_t mask = p->masks[function];
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
    if (depth > MO_LIMIT_SOURCE_ABILITY_DEPTH)
        return report(p, MO_CODE_CHECK_ABILITY_DEPTH, "the ability dependency path is too deep",
                      NULL);
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

static MO_MUST_USE uint32_t *components(struct recursion *p, uint32_t count) {
    uint32_t *index = allocate(p, count, sizeof(*index), _Alignof(uint32_t));
    uint32_t *low = allocate(p, count, sizeof(*low), _Alignof(uint32_t));
    uint32_t *component = allocate(p, count, sizeof(*component), _Alignof(uint32_t));
    uint32_t *stack = allocate(p, count, sizeof(*stack), _Alignof(uint32_t));
    struct frame *frames = allocate(p, count, sizeof(*frames), _Alignof(struct frame));
    if (p->failed)
        return NULL;
    for (uint32_t i = 0; i < count; i++)
        index[i] = component[i] = UINT32_MAX;
    uint32_t next = 0, stacked = 0, groups = 0;
    for (uint32_t start = 0; start < count; start++) {
        if (index[start] != UINT32_MAX)
            continue;
        index[start] = low[start] = next++;
        stack[stacked++] = start;
        frames[0] = (struct frame){start, p->edges[start]};
        uint32_t depth = 1;
        while (depth != 0) {
            if (!work(p))
                return NULL;
            struct frame *frame = &frames[depth - 1];
            uint32_t function = frame->function;
            if (frame->next != NULL) {
                uint32_t target = frame->next->target;
                frame->next = frame->next->next;
                if (index[target] == UINT32_MAX) {
                    index[target] = low[target] = next++;
                    MO_ASSERT(stacked < count && depth < count);
                    stack[stacked++] = target;
                    frames[depth++] = (struct frame){target, p->edges[target]};
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
    return component;
}

static MO_MUST_USE bool cycles(struct recursion *p, uint32_t count, bool data) {
    uint32_t *component = components(p, count);
    if (component == NULL)
        return false;
    const struct edge *first = NULL;
    for (uint32_t i = 0; i < count; i++)
        for (const struct edge *edge = p->edges[i]; edge != NULL; edge = edge->next) {
            if (!work(p))
                return false;
            if (!edge->grows || component[i] != component[edge->target])
                continue;
            if (first == NULL || edge->span.file < first->span.file ||
                (edge->span.file == first->span.file && edge->span.start < first->span.start))
                first = edge;
        }
    if (first == NULL)
        return true;
    p->span = first->span;
    char *message = allocate(p, 512, 1, 1);
    if (message == NULL)
        return false;
    int length = snprintf(message, 512,
                          data ? "a generic type holds itself at a bigger type: %.128s holds "
                                 "%.128s in a type-parameter cycle"
                               : "a generic recursion builds a bigger type: %.128s calls %.128s in "
                                 "a type-parameter cycle",
                          first->caller->name, first->callee->name);
    MO_ASSERT(length > 0 && length < 512);
    return report(p, data ? MO_CODE_DATA_RECURSION : MO_CODE_POLYMORPHIC_RECURSION, message,
                  data ? "A generic type can't hold itself at a bigger type built from its "
                         "parameter; it would need types without end (reference 9.1)."
                       : "A generic function can't reach itself with a bigger type built "
                         "from its parameter (reference 5.5).");
}

bool mo_check_recursion(struct mo_arena *arena, const struct mo_program *program,
                        struct mo_diags *diags) {
    struct recursion p = {.arena = arena,
                          .program = program,
                          .signatures = program->signatures,
                          .diags = diags,
                          .span = program->set->modules[0]->span};
    p.indices = allocate(&p, program->function_count, sizeof(*p.indices), _Alignof(uint32_t));
    p.offsets = allocate(&p, program->function_count, sizeof(*p.offsets), _Alignof(uint32_t));
    p.masks = allocate(&p, program->function_count, sizeof(*p.masks), _Alignof(uint32_t));
    if (p.failed)
        return false;
    uint32_t count = 0;
    for (uint32_t i = 0; i < program->function_count; i++) {
        p.indices[(uint32_t)(program->functions[i].signature - program->signatures->items)] = i;
        p.offsets[i] = count;
        p.masks[i] = program->functions[i].signature->type->parameters;
        for (unsigned j = 0; j < 26; j++) {
            if (!work(&p))
                return false;
            count += (p.masks[i] & (UINT32_C(1) << j)) != 0 ? 1u : 0u;
        }
    }
    if (count == 0)
        return true;
    p.edges = allocate(&p, count, sizeof(*p.edges), _Alignof(struct edge *));
    if (p.failed)
        return false;
    memset(p.edges, 0, (size_t)count * sizeof(*p.edges));
    for (uint32_t i = 0; i < program->function_count; i++) {
        if (p.masks[i] == 0)
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
    return cycles(&p, count, false);
}

static MO_MUST_USE bool data_edges(struct recursion *p, uint32_t caller,
                                   const struct mo_source_type *type) {
    if (!work(p))
        return false;
    if (!mo_signature_type_available(p->signatures, type))
        return true; /* An invalid field already has its declaration diagnostic. */
    if (type->kind == MO_SOURCE_NAMED && type->declaration != NULL) {
        const struct mo_nominal_signature *callee =
            mo_nominal_signature_by_symbol(p->signatures, type->declaration);
        MO_ASSERT(callee != NULL);
        uint32_t target = (uint32_t)(callee - p->signatures->nominals);
        for (uint32_t i = 0; i < type->count; i++) {
            const struct mo_source_type *argument = type->arguments[i];
            unsigned target_letter = (unsigned)(callee->type->arguments[i]->name[0] - 'A');
            for (unsigned letter = 0; letter < 26; letter++) {
                if (!work(p))
                    return false;
                if ((argument->parameters & (UINT32_C(1) << letter)) == 0)
                    continue;
                uint32_t source = parameter_node(p, caller, letter);
                struct edge *edge = allocate(p, 1, sizeof(*edge), _Alignof(struct edge));
                if (edge == NULL)
                    return false;
                *edge = (struct edge){.target = parameter_node(p, target, target_letter),
                                      .caller = p->signatures->nominals[caller].type->declaration,
                                      .callee = type->declaration,
                                      .grows = argument->kind != MO_SOURCE_PARAMETER,
                                      .span = p->span,
                                      .next = p->edges[source]};
                p->edges[source] = edge;
            }
        }
    }
    for (uint32_t i = 0; i < type->count; i++)
        if (!data_edges(p, caller, type->arguments[i]))
            return false;
    return true;
}

bool mo_check_data_recursion(struct mo_arena *arena, const struct mo_signatures *signatures,
                             struct mo_diags *diags) {
    struct recursion p = {
        .arena = arena, .signatures = signatures, .diags = diags, .span = {UINT32_MAX, 0, 0}};
    p.offsets = allocate(&p, signatures->nominal_count, sizeof(*p.offsets), _Alignof(uint32_t));
    p.masks = allocate(&p, signatures->nominal_count, sizeof(*p.masks), _Alignof(uint32_t));
    if (p.failed)
        return false;
    uint32_t count = 0;
    for (uint32_t i = 0; i < signatures->nominal_count; i++) {
        p.offsets[i] = count;
        p.masks[i] = signatures->nominals[i].type->parameters;
        for (unsigned j = 0; j < 26; j++) {
            if (!work(&p))
                return false;
            count += (p.masks[i] & (UINT32_C(1) << j)) != 0 ? 1u : 0u;
        }
    }
    if (count == 0)
        return true;
    p.edges = allocate(&p, count, sizeof(*p.edges), _Alignof(struct edge *));
    if (p.failed)
        return false;
    memset(p.edges, 0, (size_t)count * sizeof(*p.edges));
    for (uint32_t i = 0; i < signatures->data_count; i++) {
        const struct mo_data_signature *data = &signatures->data[i];
        const struct mo_nominal_signature *owner =
            mo_nominal_signature_by_symbol(signatures, data->type->declaration);
        MO_ASSERT(owner != NULL);
        if (owner->invalid)
            continue;
        uint32_t caller = (uint32_t)(owner - signatures->nominals);
        for (uint32_t j = 0; j < data->count; j++) {
            p.span = data->by_index[j]->annotation->span;
            if (!data_edges(&p, caller, data->by_index[j]->type))
                return false;
        }
    }
    return cycles(&p, count, true);
}
