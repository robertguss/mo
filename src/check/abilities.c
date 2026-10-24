#include "check/abilities.h"
#include "check/data.h"
#include "check/native.h"
#include "check/signatures.h"

#include "base/codes.h"
#include "base/limits.h"

#include <stdio.h>
#include <string.h>

struct mo_ability_cache {
    const struct mo_source_type *type;
    struct mo_ability_cache **arguments;
    uint64_t hash;
    const struct mo_ability_implementation *proofs[5];
    bool constraints;
};

struct mo_ability_pointer {
    const struct mo_source_type *type;
    struct mo_ability_cache *entry;
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

static MO_MUST_USE uint64_t hash_value(uint64_t hash, uint64_t value) {
    return (hash ^ value) * UINT64_C(1099511628211);
}

/* What a declaration hashes to: its module, name and place, never its address, so that where the
 * allocator put a symbol can't change which structural slots collide or the work they cost. */
static MO_MUST_USE uint64_t hash_declaration(uint64_t hash, const struct mo_symbol *symbol) {
    if (symbol == NULL)
        return hash_value(hash, UINT64_MAX);
    hash = hash_value(hash, symbol->module);
    size_t length = symbol->name_length != 0 ? symbol->name_length
                    : symbol->name != NULL   ? strlen(symbol->name)
                                             : 0;
    for (size_t i = 0; i < length; i++)
        hash = hash_value(hash, (unsigned char)symbol->name[i]);
    return hash_value(hash,
                      symbol->declaration == NULL ? UINT32_MAX : symbol->declaration->span.start);
}

/* The pointer index is keyed by address, which differs from run to run, so its probes are not
 * charged as work: charging them moved where E1104 was reported between runs of one program.
 * The index is at most half full and its slots mix every address bit, so probes stay short;
 * each lookup is charged once by cache(). */
static MO_MUST_USE uint32_t pointer_slot(const struct mo_source_type *type, uint32_t capacity) {
    uint64_t value = (uint64_t)(uintptr_t)type;
    value ^= value >> 33;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33;
    return (uint32_t)value & (capacity - 1);
}

static MO_MUST_USE bool grow_cache(struct mo_ability_context *p) {
    MO_ASSERT(p->capacity <= MO_LIMIT_PROGRAM_CHECK_WORK);
    uint32_t capacity = p->capacity == 0 ? 64 : p->capacity * 2;
    struct mo_ability_cache **entries =
        allocate(p, capacity, sizeof(*entries), _Alignof(struct mo_ability_cache *));
    struct mo_ability_pointer *pointers =
        allocate(p, capacity, sizeof(*pointers), _Alignof(struct mo_ability_pointer));
    if (entries == NULL || pointers == NULL)
        return false;
    memset(entries, 0, (size_t)capacity * sizeof(*entries));
    memset(pointers, 0, (size_t)capacity * sizeof(*pointers));
    /* The uncharged probes below end at an empty slot, which this leaves. */
    MO_ASSERT(p->count < capacity);
    for (uint32_t i = 0; i < p->capacity; i++) {
        if (!work(p, 0))
            return false;
        if (p->cache[i] != NULL) {
            uint32_t slot = (uint32_t)p->cache[i]->hash & (capacity - 1);
            while (entries[slot] != NULL) {
                if (!work(p, 0))
                    return false;
                slot = (slot + 1) & (capacity - 1);
            }
            entries[slot] = p->cache[i];
        }
        if (p->pointers[i].type != NULL) {
            uint32_t slot = pointer_slot(p->pointers[i].type, capacity);
            while (pointers[slot].type != NULL)
                slot = (slot + 1) & (capacity - 1);
            pointers[slot] = p->pointers[i];
        }
    }
    p->capacity = capacity;
    p->cache = entries;
    p->pointers = pointers;
    return true;
}

/* Child identities make structural equality constant per edge. The pointer
 * index prevents expanding shared paths while interning an incoming type DAG. */
static MO_MUST_USE struct mo_ability_cache *cache(struct mo_ability_context *p,
                                                  const struct mo_source_type *type) {
    if (!work(p, type->height))
        return NULL;
    if (p->capacity != 0) {
        MO_ASSERT(p->count < p->capacity);
        uint32_t slot = pointer_slot(type, p->capacity);
        while (p->pointers[slot].type != NULL) {
            if (p->pointers[slot].type == type)
                return p->pointers[slot].entry;
            slot = (slot + 1) & (p->capacity - 1);
        }
    }
    struct mo_ability_cache **arguments =
        allocate(p, type->count, sizeof(*arguments), _Alignof(struct mo_ability_cache *));
    if (arguments == NULL)
        return NULL;
    uint64_t hash = hash_value(UINT64_C(14695981039346656037), (uint64_t)type->kind);
    hash = hash_value(hash, type->module);
    hash = hash_declaration(hash, type->declaration);
    hash = hash_value(hash, type->count);
    for (const char *name = type->name; *name != '\0'; name++)
        hash = hash_value(hash, (unsigned char)*name);
    for (uint32_t i = 0; i < type->count; i++) {
        arguments[i] = cache(p, type->arguments[i]);
        if (arguments[i] == NULL)
            return NULL;
        hash = hash_value(hash, arguments[i]->hash);
    }
    if (p->count >= p->capacity / 2 && !grow_cache(p))
        return NULL;
    uint32_t slot = (uint32_t)hash & (p->capacity - 1);
    struct mo_ability_cache *out = NULL;
    while (p->cache[slot] != NULL) {
        if (!work(p, 0))
            return NULL;
        struct mo_ability_cache *at = p->cache[slot];
        const struct mo_source_type *a = at->type;
        bool equal = at->hash == hash && a->kind == type->kind && a->module == type->module &&
                     a->declaration == type->declaration && a->count == type->count &&
                     strcmp(a->name, type->name) == 0;
        for (uint32_t i = 0; equal && i < type->count; i++) {
            if (!work(p, 0))
                return NULL;
            equal = at->arguments[i] == arguments[i];
        }
        if (equal) {
            out = at;
            break;
        }
        slot = (slot + 1) & (p->capacity - 1);
    }
    if (out == NULL) {
        out = allocate(p, 1, sizeof(*out), _Alignof(struct mo_ability_cache));
        if (out == NULL)
            return NULL;
        *out = (struct mo_ability_cache){.type = type, .arguments = arguments, .hash = hash};
        p->cache[slot] = out;
    }
    slot = pointer_slot(type, p->capacity);
    MO_ASSERT(p->count < p->capacity / 2);
    while (p->pointers[slot].type != NULL)
        slot = (slot + 1) & (p->capacity - 1);
    p->pointers[slot] = (struct mo_ability_pointer){type, out};
    p->count++;
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

struct type_preview {
    char text[512];
    size_t length;
};

static void preview_text(struct type_preview *out, const char *text) {
    while (*text != '\0' && out->length + 4 < sizeof(out->text))
        out->text[out->length++] = *text++;
    if (*text != '\0' && out->length + 4 <= sizeof(out->text)) {
        memcpy(out->text + out->length, "...", 3);
        out->length += 3;
    }
    out->text[out->length] = '\0';
}

static MO_MUST_USE bool preview_type(struct mo_ability_context *p, struct type_preview *out,
                                     const struct mo_source_type *type, uint32_t depth) {
    if (out->length + 4 >= sizeof(out->text))
        return true;
    if (!work(p, depth))
        return false;
    bool tuple = type->kind == MO_SOURCE_TUPLE, function = type->kind == MO_SOURCE_FUNCTION;
    if (!tuple)
        preview_text(out, type->name);
    if (type->count == 0)
        return true;
    preview_text(out, "(");
    uint32_t count = type->count - (function ? 1u : 0u);
    for (uint32_t i = 0; i < count && out->length + 4 < sizeof(out->text); i++) {
        if (i != 0)
            preview_text(out, ", ");
        if (!preview_type(p, out, type->arguments[i], depth + 1))
            return false;
    }
    preview_text(out, ")");
    if (function) {
        preview_text(out, " : ");
        if (!preview_type(p, out, type->arguments[count], depth + 1))
            return false;
    }
    return true;
}

static MO_MUST_USE bool missing(struct mo_ability_context *p, const struct mo_source_type *type,
                                enum mo_source_ability ability) {
    static const char *const names[] = {"Equal", "Hash", "Order", "Text", "Number"};
    struct type_preview preview = {0};
    if (!preview_type(p, &preview, type, 0))
        return false;
    char *message = allocate(p, 768, 1, 1);
    if (message == NULL)
        return false;
    int length = snprintf(message, 768, "type %s doesn't have %s", preview.text, names[ability]);
    MO_ASSERT(length > 0 && length < 768);
    return fail(p, MO_CODE_ABILITY_MISSING, message,
                "An operation needs its type's ability; functions and capabilities have no "
                "abilities (reference 9.2).");
}

static MO_MUST_USE unsigned parameter_requirements(struct mo_ability_context *p,
                                                   const struct mo_source_type *type) {
    unsigned letter = (unsigned)(type->name[0] - 'A');
    MO_ASSERT(letter < 26 && type->declaration != NULL);
    const struct mo_function_signature *fn =
        mo_function_signature_by_symbol(p->signatures, type->declaration);
    if (fn != NULL)
        return fn->requirements[letter];
    /* Only a native owner (module UINT32_MAX) is in native[]; searching for any other symbol
     * would charge steps that depend on where the two arrays sit in memory. */
    uint32_t lo = 0, hi = type->declaration->module == UINT32_MAX ? p->signatures->native_count : 0;
    while (lo < hi) {
        if (!work(p, 0))
            return 0;
        uint32_t mid = lo + (hi - lo) / 2;
        if ((uintptr_t)&p->signatures->native[mid].owner < (uintptr_t)type->declaration)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < p->signatures->native_count && &p->signatures->native[lo].owner == type->declaration)
        return p->signatures->native[lo].requirements[letter];
    const struct mo_nominal_signature *data =
        mo_nominal_signature_by_symbol(p->signatures, type->declaration);
    if (data != NULL && (data->key_parameters & type->parameters) != 0)
        return (1u << MO_SOURCE_HASH) | (1u << MO_SOURCE_EQUAL);
    return 0;
}

static MO_MUST_USE bool derive_data(struct mo_ability_context *p,
                                    struct mo_ability_implementation *out, uint32_t depth) {
    const struct mo_source_type *type = out->type;
    const struct mo_nominal_signature *nominal =
        mo_nominal_signature_by_symbol(p->signatures, type->declaration);
    MO_ASSERT(nominal != NULL);
    uint32_t count = nominal->field_count;
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
        if (!work(p, 0))
            return false;
        const struct mo_data_signature *data = mo_data_signature_by_symbol(p->signatures, symbol);
        MO_ASSERT(data != NULL);
        for (uint32_t k = 0; k < data->count; k++) {
            const struct mo_source_type *field =
                substitute(p, data->by_index[k]->type, data->type, type, 0);
            parts[at] = field == NULL ? NULL : prove(p, field, out->ability, depth + 1);
            if (parts[at++] == NULL)
                return false;
        }
    }
    MO_ASSERT(at == count);
    return true;
}

static MO_MUST_USE bool customizer(struct mo_ability_context *p,
                                   struct mo_ability_implementation *out, uint32_t depth) {
    const struct mo_source_type *type = out->type;
    bool file_error = named(type, "FileError") && out->ability == MO_SOURCE_TEXT;
    if ((type->declaration == NULL && !file_error) ||
        (out->ability != MO_SOURCE_TEXT && out->ability != MO_SOURCE_ORDER))
        return true;
    const struct mo_function_signature *fn =
        mo_customizer_signature(p->signatures, type, out->ability);
    if (fn != NULL) {
        const struct mo_source_type *receiver = fn->type->arguments[0];
        out->kind = MO_ABILITY_FUNCTION;
        out->function = fn->function;
        struct mo_type_argument *arguments =
            allocate(p, type->count, sizeof(*arguments), _Alignof(struct mo_type_argument));
        if (arguments == NULL)
            return false;
        MO_ASSERT(type->count <= 26);
        for (uint32_t j = 0; j < type->count; j++) {
            struct mo_type_argument argument = {receiver->arguments[j], type->arguments[j]};
            uint32_t at = j;
            while (at > 0 && arguments[at - 1].parameter->name[0] > argument.parameter->name[0]) {
                arguments[at] = arguments[at - 1];
                at--;
            }
            arguments[at] = argument;
        }
        out->arguments = (struct mo_type_arguments){type->count, arguments};
        uint32_t count = 0;
        for (uint32_t j = 0; j < type->count; j++) {
            unsigned letter = (unsigned)(arguments[j].parameter->name[0] - 'A');
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
            unsigned letter = (unsigned)(arguments[j].parameter->name[0] - 'A');
            for (unsigned k = 0; k < 5; k++) {
                if ((fn->requirements[letter] & (1u << k)) == 0)
                    continue;
                parameters[at] = arguments[j].parameter;
                parts[at] = prove(p, arguments[j].type, (enum mo_source_ability)k, depth + 1);
                if (parts[at++] == NULL)
                    return false;
            }
        }
        return true;
    }
    return true;
}

static MO_MUST_USE bool derive_native(struct mo_ability_context *p,
                                      struct mo_ability_implementation *out, uint32_t depth) {
    static const struct mo_source_type text = {
        .kind = MO_SOURCE_NAMED, .module = UINT32_MAX, .name = "Text"};
    static const struct mo_source_type kind = {
        .kind = MO_SOURCE_NAMED, .module = UINT32_MAX, .name = "EntryKind"};
    const struct mo_builtin_case *first = mo_builtin_case_by_type(out->type->name);
    MO_ASSERT(first != NULL);
    uint32_t count = 0;
    for (uint32_t i = 0; i < first->cases; i++) {
        const struct mo_builtin_case *data = mo_builtin_case_by_key(first->first_key + i);
        MO_ASSERT(data != NULL);
        count += data->count;
    }
    const struct mo_ability_implementation **parts =
        allocate(p, count, sizeof(*parts), _Alignof(const struct mo_ability_implementation *));
    if (parts == NULL)
        return false;
    out->kind = MO_ABILITY_DERIVED;
    out->parts = parts;
    out->part_count = count;
    uint32_t at = 0;
    for (uint32_t i = 0; i < first->cases; i++) {
        const struct mo_builtin_case *data = mo_builtin_case_by_key(first->first_key + i);
        for (uint32_t j = 0; j < data->count; j++) {
            const struct mo_source_type *field =
                named(out->type, "Entry") && j == 1 ? &kind : &text;
            parts[at] = prove(p, field, out->ability, depth + 1);
            if (parts[at++] == NULL)
                return false;
        }
    }
    MO_ASSERT(at == count);
    return true;
}

static MO_MUST_USE const struct mo_ability_implementation *prove(struct mo_ability_context *p,
                                                                 const struct mo_source_type *type,
                                                                 enum mo_source_ability ability,
                                                                 uint32_t depth) {
    if (!work(p, 0))
        return NULL;
    if (depth > MO_LIMIT_SOURCE_ABILITY_DEPTH) {
        bool failed =
            fail(p, MO_CODE_CHECK_ABILITY_DEPTH, "the ability dependency path is too deep", NULL);
        (void)failed;
        return NULL;
    }
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
        if (p->error != 0)
            return NULL;
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
    if (!customizer(p, out, depth))
        return NULL;
    if (out->kind == MO_ABILITY_FUNCTION)
        return out;
    if (type->declaration != NULL) {
        out->kind = MO_ABILITY_DERIVED;
        return derive_data(p, out, depth) ? out : NULL;
    }
    if (named(type, "Entry") || named(type, "FileError") || named(type, "Ordering") ||
        named(type, "EntryKind"))
        return derive_native(p, out, depth) ? out : NULL;
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
        const struct mo_nominal_signature *data =
            mo_nominal_signature_by_symbol(p->signatures, type->declaration);
        MO_ASSERT(data != NULL);
        for (uint32_t j = 0; j < type->count; j++) {
            if ((data->key_parameters & data->type->arguments[j]->parameters) != 0 &&
                prove(p, type->arguments[j], MO_SOURCE_HASH, 0) == NULL)
                return false;
        }
    }
    return true;
}

bool mo_check_type_abilities(struct mo_ability_context *p, const struct mo_source_type *type) {
    return constraints(p, type, 0);
}
