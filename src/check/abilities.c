#include "check/abilities.h"
#include "check/native.h"

#include "base/codes.h"
#include "base/limits.h"

#include <stdio.h>
#include <string.h>

struct mo_ability_cache {
    const struct mo_source_type *type;
    struct mo_ability_cache *next;
    const struct mo_ability_implementation *proofs[5];
    bool constraints;
};

static MO_MUST_USE bool fail(struct mo_ability_context *p, uint16_t code, const char *message,
                             const char *reason) {
    if (p->error == 0) {
        p->error = code;
        p->message = message;
        p->reason = reason;
    }
    return false;
}

static MO_MUST_USE bool work(struct mo_ability_context *p, uint32_t depth) {
    if (p->error != 0)
        return false;
    if (depth > MO_LIMIT_SOURCE_TYPE_DEPTH)
        return fail(p, MO_CODE_CHECK_TYPE_DEPTH, "the ability type path is too deep", NULL);
    if (*p->work == MO_LIMIT_SOURCE_CHECK_WORK)
        return fail(p, MO_CODE_CHECK_WORK, "the function needs too much ability-checking work",
                    NULL);
    if (*p->signatures->work == MO_LIMIT_PROGRAM_CHECK_WORK)
        return fail(p, MO_CODE_CHECK_PROGRAM_WORK,
                    "the program needs too much ability-checking work", NULL);
    (*p->work)++;
    (*p->signatures->work)++;
    return true;
}

static MO_MUST_USE void *allocate(struct mo_ability_context *p, uint32_t count, size_t size,
                                  size_t alignment) {
    if (!work(p, 0))
        return NULL;
    void *out = mo_arena_allocate_array(p->arena, count, size, alignment);
    if (out == NULL) {
        bool failed = fail(p, MO_CODE_CHECK_NO_MEMORY, "the checking arena has no room", NULL);
        (void)failed;
    }
    return out;
}

static MO_MUST_USE bool same(struct mo_ability_context *p, const struct mo_source_type *a,
                             const struct mo_source_type *b, uint32_t depth) {
    if (a == b)
        return true;
    if (!work(p, depth) || a->kind != b->kind || a->module != b->module ||
        a->declaration != b->declaration || a->count != b->count || strcmp(a->name, b->name) != 0)
        return false;
    for (uint32_t i = 0; i < a->count; i++)
        if (!same(p, a->arguments[i], b->arguments[i], depth + 1))
            return false;
    return true;
}

static MO_MUST_USE struct mo_ability_cache *cache(struct mo_ability_context *p,
                                                  const struct mo_source_type *type) {
    for (struct mo_ability_cache *at = p->cache; at != NULL; at = at->next) {
        if (!work(p, 0))
            return NULL;
        if (same(p, at->type, type, 0))
            return at;
    }
    struct mo_ability_cache *out = allocate(p, 1, sizeof(*out), _Alignof(struct mo_ability_cache));
    if (out != NULL) {
        *out = (struct mo_ability_cache){.type = type, .next = p->cache};
        p->cache = out;
    }
    return out;
}

static MO_MUST_USE bool named(const struct mo_source_type *type, const char *name) {
    return type->kind == MO_SOURCE_NAMED && type->module == UINT32_MAX &&
           strcmp(type->name, name) == 0;
}

static MO_MUST_USE const struct mo_source_type *substitute(struct mo_ability_context *p,
                                                           const struct mo_source_type *type,
                                                           const struct mo_source_type *generic,
                                                           const struct mo_source_type *actual,
                                                           uint32_t depth) {
    if (!work(p, depth))
        return NULL;
    if (type->parameters == 0)
        return type;
    if (type->kind == MO_SOURCE_PARAMETER) {
        for (uint32_t i = 0; i < generic->count; i++)
            if (generic->arguments[i] == type)
                return actual->arguments[i];
        return type;
    }
    const struct mo_source_type **arguments =
        allocate(p, type->count, sizeof(*arguments), _Alignof(const struct mo_source_type *));
    if (arguments == NULL)
        return NULL;
    bool changed = false;
    uint32_t height = 0, parameters = 0;
    for (uint32_t i = 0; i < type->count; i++) {
        arguments[i] = substitute(p, type->arguments[i], generic, actual, depth + 1);
        if (arguments[i] == NULL)
            return NULL;
        changed = changed || arguments[i] != type->arguments[i];
        if (arguments[i]->height + 1 > height)
            height = arguments[i]->height + 1;
        parameters |= arguments[i]->parameters;
    }
    if (!changed)
        return type;
    if (!work(p, height))
        return NULL;
    struct mo_source_type *out = allocate(p, 1, sizeof(*out), _Alignof(struct mo_source_type));
    if (out != NULL) {
        *out = *type;
        out->arguments = arguments;
        out->height = height;
        out->parameters = parameters;
    }
    return out;
}

static MO_MUST_USE const struct mo_ability_implementation *prove(struct mo_ability_context *p,
                                                                 const struct mo_source_type *type,
                                                                 enum mo_source_ability ability,
                                                                 uint32_t depth);

static MO_MUST_USE bool missing(struct mo_ability_context *p, const struct mo_source_type *type,
                                enum mo_source_ability ability) {
    static const char *const names[] = {"Equal", "Hash", "Order", "Text", "Number"};
    char *message = allocate(p, 256, 1, 1);
    if (message == NULL)
        return false;
    int length = snprintf(message, 256, "type %.128s doesn't have %s", type->name, names[ability]);
    MO_ASSERT(length > 0 && length < 256);
    return fail(p, MO_CODE_ABILITY_MISSING, message,
                "An operation needs its type's ability; functions and capabilities have no "
                "abilities (reference 9.2).");
}

static MO_MUST_USE unsigned parameter_requirements(struct mo_ability_context *p,
                                                   const struct mo_source_type *type) {
    unsigned letter = (unsigned)(type->name[0] - 'A');
    MO_ASSERT(letter < 26 && type->declaration != NULL);
    for (uint32_t i = 0; i < p->signatures->count; i++)
        if (p->signatures->items[i].function == type->declaration)
            return p->signatures->items[i].requirements[letter];
    for (uint32_t i = 0; i < p->signatures->native_count; i++)
        if (&p->signatures->native[i].owner == type->declaration)
            return p->signatures->native[i].requirements[letter];
    for (uint32_t i = 0; i < p->signatures->data_count; i++)
        if (p->signatures->data[i].type->declaration == type->declaration &&
            (p->signatures->data[i].key_parameters & type->parameters) != 0)
            return (1u << MO_SOURCE_HASH) | (1u << MO_SOURCE_EQUAL);
    return 0;
}

static MO_MUST_USE bool derive_data(struct mo_ability_context *p,
                                    struct mo_ability_implementation *out, uint32_t depth) {
    const struct mo_source_type *type = out->type;
    uint32_t count = 0;
    for (uint32_t i = 0; i < p->signatures->data_count; i++)
        if (p->signatures->data[i].type->declaration == type->declaration)
            count += p->signatures->data[i].count;
    const struct mo_ability_implementation **parts =
        allocate(p, count, sizeof(*parts), _Alignof(const struct mo_ability_implementation *));
    if (parts == NULL)
        return false;
    out->parts = parts;
    out->part_count = count;
    uint32_t at = 0;
    const struct mo_symbol *owner = type->declaration;
    uint32_t cases = owner->declaration->kind == MO_S_ENUM ? owner->case_count : 1;
    for (uint32_t i = 0; i < cases; i++) {
        const struct mo_symbol *symbol =
            owner->declaration->kind == MO_S_ENUM
                ? &p->interfaces->scopes[owner->module].symbols[owner->cases[i]]
                : owner;
        for (uint32_t j = 0; j < p->signatures->data_count; j++) {
            const struct mo_data_signature *data = &p->signatures->data[j];
            if (data->symbol != symbol)
                continue;
            for (uint32_t k = 0; k < data->count; k++) {
                const struct mo_source_type *field =
                    substitute(p, data->by_index[k]->type, data->type, type, 0);
                parts[at] = field == NULL ? NULL : prove(p, field, out->ability, depth + 1);
                if (parts[at++] == NULL)
                    return false;
            }
        }
    }
    MO_ASSERT(at == count);
    return true;
}

static MO_MUST_USE bool customizer(struct mo_ability_context *p,
                                   struct mo_ability_implementation *out) {
    const struct mo_source_type *type = out->type;
    if (type->declaration == NULL ||
        (out->ability != MO_SOURCE_TEXT && out->ability != MO_SOURCE_ORDER))
        return true;
    const char *name = out->ability == MO_SOURCE_TEXT ? "to_text" : "compare";
    for (uint32_t i = 0; i < p->signatures->count; i++) {
        const struct mo_function_signature *fn = &p->signatures->items[i];
        if (fn->function->module != type->module || strcmp(fn->function->name, name) != 0 ||
            fn->type->count < 2)
            continue;
        const struct mo_source_type *receiver = fn->type->arguments[0];
        if (receiver->declaration != type->declaration)
            continue;
        out->kind = MO_ABILITY_FUNCTION;
        out->function = fn->function;
        struct mo_type_argument *arguments =
            allocate(p, type->count, sizeof(*arguments), _Alignof(struct mo_type_argument));
        if (arguments == NULL)
            return false;
        for (uint32_t j = 0; j < type->count; j++)
            arguments[j] = (struct mo_type_argument){receiver->arguments[j], type->arguments[j]};
        out->arguments = (struct mo_type_arguments){type->count, arguments};
        uint32_t count = 0;
        for (uint32_t j = 0; j < type->count; j++) {
            unsigned letter = (unsigned)(receiver->arguments[j]->name[0] - 'A');
            for (unsigned k = 0; k < 5; k++)
                count += (fn->requirements[letter] & (1u << k)) != 0 ? 1u : 0u;
        }
        const struct mo_ability_implementation **parts =
            allocate(p, count, sizeof(*parts), _Alignof(const struct mo_ability_implementation *));
        const struct mo_source_type **parameters =
            allocate(p, count, sizeof(*parameters), _Alignof(const struct mo_source_type *));
        if (parts == NULL || parameters == NULL)
            return false;
        out->part_count = count;
        out->parts = parts;
        out->part_parameters = parameters;
        uint32_t at = 0;
        for (uint32_t j = 0; j < type->count; j++) {
            unsigned letter = (unsigned)(receiver->arguments[j]->name[0] - 'A');
            for (unsigned k = 0; k < 5; k++) {
                if ((fn->requirements[letter] & (1u << k)) == 0)
                    continue;
                parameters[at] = receiver->arguments[j];
                parts[at] = prove(p, type->arguments[j], (enum mo_source_ability)k, 1);
                if (parts[at++] == NULL)
                    return false;
            }
        }
        return true;
    }
    return true;
}

static MO_MUST_USE const struct mo_ability_implementation *prove(struct mo_ability_context *p,
                                                                 const struct mo_source_type *type,
                                                                 enum mo_source_ability ability,
                                                                 uint32_t depth) {
    if (!work(p, depth))
        return NULL;
    struct mo_ability_cache *entry = cache(p, type);
    if (entry == NULL)
        return NULL;
    if (entry->proofs[ability] != NULL)
        return entry->proofs[ability];
    struct mo_ability_implementation *out =
        allocate(p, 1, sizeof(*out), _Alignof(struct mo_ability_implementation));
    if (out == NULL)
        return NULL;
    *out = (struct mo_ability_implementation){
        .ability = ability, .type = type, .kind = MO_ABILITY_NATIVE};
    if (type->kind == MO_SOURCE_PARAMETER) {
        unsigned mask = parameter_requirements(p, type);
        if (ability == MO_SOURCE_EQUAL &&
            (mask & ((1u << MO_SOURCE_HASH) | (1u << MO_SOURCE_ORDER))) != 0)
            mask |= 1u << MO_SOURCE_EQUAL;
        if ((mask & (1u << ability)) == 0) {
            bool failed = missing(p, type, ability);
            (void)failed;
            return NULL;
        }
        out->kind = MO_ABILITY_PARAMETER;
        entry->proofs[ability] = out;
        return out;
    }
    bool numeric = named(type, "Int") || named(type, "BigInt") || named(type, "Decimal") ||
                   named(type, "Float");
    bool denied = type->kind == MO_SOURCE_FUNCTION || (ability == MO_SOURCE_NUMBER && !numeric) ||
                  (ability == MO_SOURCE_HASH && named(type, "Float")) ||
                  (ability == MO_SOURCE_TEXT && named(type, "Bytes")) ||
                  (ability == MO_SOURCE_ORDER &&
                   (named(type, "Range") || named(type, "Map") || named(type, "Set")));
    static const char *const capabilities[] = {"Args",      "Env",   "Console", "Files",
                                               "ReadFiles", "Clock", "Random"};
    for (unsigned i = 0; i < sizeof(capabilities) / sizeof(capabilities[0]); i++)
        denied = denied || named(type, capabilities[i]);
    if (denied) {
        bool failed = missing(p, type, ability);
        (void)failed;
        return NULL;
    }
    /* Publish before descending: recursive nominal proofs form a shared graph.
     * Any failed descendant fails the entire context; provisional edges can
     * therefore never escape in a certified program. */
    entry->proofs[ability] = out;
    if ((ability == MO_SOURCE_HASH || ability == MO_SOURCE_ORDER) &&
        prove(p, type, MO_SOURCE_EQUAL, depth) == NULL)
        return NULL;
    if (!customizer(p, out))
        return NULL;
    if (out->kind == MO_ABILITY_FUNCTION)
        return out;
    if (type->declaration != NULL) {
        out->kind = MO_ABILITY_DERIVED;
        return derive_data(p, out, depth) ? out : NULL;
    }
    if (type->kind == MO_SOURCE_TUPLE || type->count != 0) {
        out->kind = MO_ABILITY_DERIVED;
        const struct mo_ability_implementation **parts = allocate(
            p, type->count, sizeof(*parts), _Alignof(const struct mo_ability_implementation *));
        if (parts == NULL)
            return NULL;
        out->part_count = type->count;
        out->parts = parts;
        for (uint32_t i = 0; i < type->count; i++) {
            parts[i] = prove(p, type->arguments[i], ability, depth + 1);
            if (parts[i] == NULL)
                return NULL;
        }
    }
    return out;
}

const struct mo_ability_implementation *mo_prove_ability(struct mo_ability_context *p,
                                                         const struct mo_source_type *type,
                                                         enum mo_source_ability ability) {
    return prove(p, type, ability, 0);
}

static MO_MUST_USE bool constraints(struct mo_ability_context *p, const struct mo_source_type *type,
                                    uint32_t depth) {
    if (!work(p, depth))
        return false;
    struct mo_ability_cache *entry = cache(p, type);
    if (entry == NULL)
        return false;
    if (entry->constraints)
        return true;
    entry->constraints = true;
    if ((named(type, "Map") || named(type, "Set")) &&
        prove(p, type->arguments[0], MO_SOURCE_HASH, 0) == NULL)
        return false;
    for (uint32_t i = 0; i < type->count; i++)
        if (!constraints(p, type->arguments[i], depth + 1))
            return false;
    if (type->kind == MO_SOURCE_NAMED && type->declaration != NULL) {
        for (uint32_t i = 0; i < p->signatures->data_count; i++) {
            const struct mo_data_signature *data = &p->signatures->data[i];
            if (!work(p, depth))
                return false;
            if (data->type->declaration != type->declaration)
                continue;
            for (uint32_t j = 0; j < type->count; j++) {
                if ((data->key_parameters & data->type->arguments[j]->parameters) != 0 &&
                    prove(p, type->arguments[j], MO_SOURCE_HASH, 0) == NULL)
                    return false;
            }
        }
    }
    return true;
}

bool mo_check_type_abilities(struct mo_ability_context *p, const struct mo_source_type *type) {
    return constraints(p, type, 0);
}
