/* Capability-free type arguments for generic functions whose anonymous functions capture a
 * type parameter. The body checker can't know whether such a value holds a capability, so each
 * use of the function checks its argument (reference 11.4). */
#include "check/checked.h"
#include "check/signatures.h"

#include "base/codes.h"
#include "base/limits.h"

#include <stdio.h>
#include <string.h>

struct proof_seen {
    const struct mo_ability_implementation *proof;
    struct proof_seen *next;
};

struct captures {
    struct mo_arena *arena;
    const struct mo_program *program;
    const struct mo_signatures *signatures;
    struct mo_diags *diags;
    struct mo_span span;
    uint32_t *free; /* per function: type parameters that must not hold capabilities */
    struct proof_seen *seen;
    bool reporting, changed, failed;
};

static MO_MUST_USE bool report_at(struct captures *p, uint16_t code, const char *message,
                                  const char *reason, const struct mo_label *secondary) {
    bool kept = mo_diags_add(p->diags, &(struct mo_diag){.code = code,
                                                         .primary = {p->span, message},
                                                         .secondary = secondary,
                                                         .secondary_count = secondary != NULL,
                                                         .message = message,
                                                         .reason = reason});
    (void)kept;
    p->failed = true;
    return false;
}

static MO_MUST_USE bool report(struct captures *p, uint16_t code, const char *message,
                               const char *reason) {
    return report_at(p, code, message, reason, NULL);
}

static MO_MUST_USE bool work(struct captures *p) {
    if (p->failed)
        return false;
    if (*p->signatures->work == MO_LIMIT_PROGRAM_CHECK_WORK)
        return report(p, MO_CODE_CHECK_PROGRAM_WORK,
                      "the program needs too much capability-checking work", NULL);
    (*p->signatures->work)++;
    return true;
}

/* Mirrors the body checker's view of what a stored value can hold: type parameters it stores,
 * and whether it holds a capability whatever those parameters are. */
static MO_MUST_USE bool stored(struct captures *p, const struct mo_source_type *type,
                               uint32_t *parameters, bool *capability, uint32_t depth) {
    if (!work(p))
        return false;
    if (depth > MO_LIMIT_SOURCE_ABILITY_DEPTH)
        return report(p, MO_CODE_CHECK_ABILITY_DEPTH, "the type is nested too deeply", NULL);
    if (type->kind == MO_SOURCE_FUNCTION)
        return true;
    if (type->kind == MO_SOURCE_PARAMETER) {
        *parameters |= UINT32_C(1) << (unsigned)(type->name[0] - 'A');
        return true;
    }
    if (type->kind == MO_SOURCE_NAMED && type->declaration != NULL) {
        const struct mo_nominal_signature *owner =
            mo_nominal_signature_by_symbol(p->signatures, type->declaration);
        MO_ASSERT(owner != NULL);
        *capability = *capability || owner->holds_capability;
        for (uint32_t i = 0; i < type->count; i++)
            if ((owner->capture_parameters & owner->type->arguments[i]->parameters) != 0 &&
                !stored(p, type->arguments[i], parameters, capability, depth + 1))
                return false;
        return true;
    }
    static const char *const capabilities[] = {"Args",      "Env",   "Console", "Files",
                                               "ReadFiles", "Clock", "Random"};
    for (uint32_t i = 0; i < sizeof(capabilities) / sizeof(capabilities[0]); i++)
        if (type->kind == MO_SOURCE_NAMED && type->module == UINT32_MAX &&
            strcmp(type->name, capabilities[i]) == 0)
            *capability = true;
    for (uint32_t i = 0; i < type->count; i++)
        if (!stored(p, type->arguments[i], parameters, capability, depth + 1))
            return false;
    return true;
}

static MO_MUST_USE bool use(struct captures *p, uint32_t caller, const struct mo_symbol *symbol,
                            const struct mo_type_arguments *arguments) {
    if (symbol == NULL || symbol->declaration->kind != MO_S_FN || arguments->count == 0)
        return true;
    const struct mo_signatures *signatures = p->signatures;
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
    uint32_t callee = UINT32_MAX;
    for (uint32_t i = 0; i < p->program->function_count; i++)
        if (p->program->functions[i].signature == &signatures->items[lo])
            callee = i;
    if (callee == UINT32_MAX || p->free[callee] == 0)
        return true;
    for (uint32_t i = 0; i < arguments->count; i++) {
        const struct mo_type_argument *argument = &arguments->items[i];
        unsigned letter = (unsigned)(argument->parameter->name[0] - 'A');
        if ((p->free[callee] & (UINT32_C(1) << letter)) == 0)
            continue;
        uint32_t parameters = 0;
        bool capability = false;
        if (!stored(p, argument->type, &parameters, &capability, 0))
            return false;
        if (capability) {
            if (p->reporting) {
                char *message = mo_arena_allocate_array(p->arena, 256, 1, 1);
                if (message == NULL)
                    return report(p, MO_CODE_CHECK_NO_MEMORY, "the checking arena has no room",
                                  NULL);
                int length = snprintf(message, 256,
                                      "%.128s makes an anonymous function capture a capability",
                                      symbol->name);
                MO_ASSERT(length > 0 && length < 256);
                struct mo_label *label =
                    mo_arena_allocate_array(p->arena, 1, sizeof(*label), _Alignof(struct mo_label));
                if (label == NULL)
                    return report(p, MO_CODE_CHECK_NO_MEMORY, "the checking arena has no room",
                                  NULL);
                *label = (struct mo_label){symbol->declaration->span, "captures its parameter"};
                return report_at(p, MO_CODE_CAPABILITY_CAPTURE, message,
                                 "This generic function's anonymous functions capture a value of "
                                 "this type argument, and anonymous functions can't capture "
                                 "capabilities, or values holding them (reference 11.4).",
                                 label);
            }
            continue;
        }
        /* The caller's own parameters now flow into a capturing position. A parameter with
         * Number is a native number, which holds nothing. */
        const struct mo_function_signature *own = p->program->functions[caller].signature;
        for (unsigned j = 0; j < 26; j++)
            if ((parameters & (UINT32_C(1) << j)) != 0 &&
                (own->requirements[j] & (1u << MO_SOURCE_NUMBER)) == 0 &&
                (p->free[caller] & (UINT32_C(1) << j)) == 0) {
                p->free[caller] |= UINT32_C(1) << j;
                p->changed = true;
            }
    }
    return true;
}

static MO_MUST_USE bool proofs(struct captures *p, uint32_t caller,
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
    struct proof_seen *seen =
        mo_arena_allocate_array(p->arena, 1, sizeof(*seen), _Alignof(struct proof_seen));
    if (seen == NULL)
        return report(p, MO_CODE_CHECK_NO_MEMORY, "the checking arena has no room", NULL);
    *seen = (struct proof_seen){proof, p->seen};
    p->seen = seen;
    if (!use(p, caller, proof->function, &proof->arguments))
        return false;
    for (uint32_t i = 0; i < proof->part_count; i++)
        if (!proofs(p, caller, proof->parts[i], depth + 1))
            return false;
    return true;
}

static MO_MUST_USE bool body(struct captures *p, uint32_t caller,
                             const struct mo_checked_body *source) {
    p->seen = NULL;
    for (uint32_t i = 0; i < source->count; i++) {
        const struct mo_checked_expression *row = &source->items[i];
        p->span = row->syntax->span;
        if (!work(p) || !use(p, caller, row->symbol, &row->arguments))
            return false;
        for (uint32_t j = 0; j < row->ability_count; j++)
            if (!proofs(p, caller, row->abilities[j].implementation, 0))
                return false;
    }
    return true;
}

static MO_MUST_USE bool sweep(struct captures *p) {
    const struct mo_program *program = p->program;
    for (uint32_t i = 0; i < program->function_count; i++)
        if (!body(p, i, &program->functions[i].body))
            return false;
    for (uint32_t i = 0; i < program->spec_count; i++) {
        const struct mo_checked_spec *spec = &program->specs[i];
        uint32_t caller = spec->subject == UINT32_MAX ? 0 : spec->subject;
        if (spec->subject == UINT32_MAX && program->function_count == 0)
            continue;
        if (!body(p, caller, &spec->body))
            return false;
        if (spec->kind == MO_SPEC_ANCHOR && spec->subject != UINT32_MAX) {
            p->span = spec->syntax->span;
            if (!use(p, caller, program->functions[spec->subject].signature->function,
                     &spec->arguments))
                return false;
        }
    }
    for (uint32_t i = 0; i < program->constant_count; i++)
        if (!body(p, 0, &program->constants[i].initializer))
            return false;
    return true;
}

bool mo_check_capture_arguments(struct mo_arena *arena, const struct mo_program *program,
                                struct mo_diags *diags) {
    struct captures p = {.arena = arena,
                         .program = program,
                         .signatures = program->signatures,
                         .diags = diags,
                         .span = program->set->modules[0]->span};
    if (program->function_count == 0)
        return true;
    p.free = mo_arena_allocate_array(arena, program->function_count, sizeof(*p.free),
                                     _Alignof(uint32_t));
    if (p.free == NULL)
        return report(&p, MO_CODE_CHECK_NO_MEMORY, "the checking arena has no room", NULL);
    uint32_t any = 0;
    for (uint32_t i = 0; i < program->function_count; i++)
        any |= p.free[i] = program->functions[i].body.capture_free_parameters;
    for (uint32_t i = 0; i < program->spec_count; i++)
        if (program->specs[i].subject != UINT32_MAX) {
            p.free[program->specs[i].subject] |= program->specs[i].body.capture_free_parameters;
            any |= program->specs[i].body.capture_free_parameters;
        }
    if (any == 0)
        return true;
    /* Grow each function's capture-free parameters through the calls that pass them on. */
    do {
        p.changed = false;
        if (!sweep(&p))
            return false;
    } while (p.changed);
    p.reporting = true;
    return sweep(&p);
}
