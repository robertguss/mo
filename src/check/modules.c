#include "check/check.h"

#include "base/codes.h"
#include "base/limits.h"

#include <stdlib.h>
#include <string.h>

struct requested {
    uint32_t module; /* requesting module + 1; zero is unused */
    const struct mo_syntax *origin;
};

struct resolver {
    struct mo_arena *arena;
    struct mo_diags *diags;
    const struct mo_module_set *set;
    struct mo_module_interfaces *out;
    struct requested **requested;
    bool failed;
};

static MO_MUST_USE void *allocate(struct resolver *r, size_t count, size_t size, size_t alignment) {
    if (r->failed)
        return NULL;
    void *result = mo_arena_allocate_array(r->arena, count, size, alignment);
    if (result == NULL) {
        r->failed = true;
        bool kept =
            mo_diags_add(r->diags, &(struct mo_diag){.code = MO_CODE_CHECK_NO_MEMORY,
                                                     .primary = {{UINT32_MAX, 0, 0}, NULL},
                                                     .message = "the checking arena has no room"});
        (void)kept;
    }
    return result;
}

static MO_MUST_USE char *spelling(struct resolver *r, const struct mo_syntax *name) {
    size_t length = name->span.end - name->span.start;
    char *text = allocate(r, length + 1, 1, 1);
    if (text != NULL) {
        memcpy(text, name->text, length);
        text[length] = '\0';
    }
    return text;
}

static MO_MUST_USE bool error(struct resolver *r, uint16_t code, struct mo_span span,
                              const char *message, const char *reason,
                              const struct mo_syntax *previous) {
    struct mo_label secondary = {previous == NULL ? span : previous->span, "declared here"};
    bool kept =
        mo_diags_add(r->diags, &(struct mo_diag){.code = code,
                                                 .primary = {span, message},
                                                 .message = message,
                                                 .reason = reason,
                                                 .secondary = previous == NULL ? NULL : &secondary,
                                                 .secondary_count = previous == NULL ? 0 : 1});
    r->failed = !kept;
    return false;
}

static MO_MUST_USE int compare_symbols(const void *a, const void *b) {
    const struct mo_symbol *x = a, *y = b;
    int order = strcmp(x->name, y->name);
    if (order != 0)
        return order;
    return x->declaration->span.start < y->declaration->span.start   ? -1
           : x->declaration->span.start > y->declaration->span.start ? 1
                                                                     : 0;
}

static MO_MUST_USE uint32_t lower(const struct mo_module_scope *scope, const char *name) {
    uint32_t lo = 0, hi = scope->count;
    while (lo < hi) {
        uint32_t middle = lo + (hi - lo) / 2;
        if (strcmp(scope->symbols[middle].name, name) < 0)
            lo = middle + 1;
        else
            hi = middle;
    }
    return lo;
}

static MO_MUST_USE struct mo_symbol *lookup(struct mo_module_scope *scope, const char *name) {
    uint32_t index = lower(scope, name);
    return index == scope->count || strcmp(scope->symbols[index].name, name) != 0
               ? NULL
               : &scope->symbols[index];
}

static MO_MUST_USE bool add_symbol(struct resolver *r, struct mo_module_scope *scope,
                                   uint32_t module, const struct mo_syntax *node,
                                   const struct mo_syntax *owner) {
    char *name = spelling(r, node->children[0]);
    if (name == NULL)
        return false;
    scope->symbols[scope->count++] =
        (struct mo_symbol){.name = name, .declaration = node, .owner = owner, .module = module};
    return true;
}

static MO_MUST_USE bool builtin(const char *name) {
    static const char *const names[] = {
        "Bool",       "Int",      "BigInt",    "Byte",  "Decimal",   "Float",     "Text",
        "Char",       "Bytes",    "Unit",      "List",  "Map",       "Set",       "Range",
        "Time",       "Duration", "Option",    "None",  "Some",      "Result",    "Ok",
        "Error",      "Ordering", "Less",      "Equal", "Greater",   "Args",      "Env",
        "Console",    "Files",    "ReadFiles", "Clock", "Random",    "FileError", "NotFound",
        "NotAllowed", "NotText",  "Failed",    "Entry", "EntryKind", "File",      "Folder",
        "Link",       "Other"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(name, names[i]) == 0)
            return true;
    return false;
}

static MO_MUST_USE bool declarations(struct resolver *r, uint32_t module) {
    struct mo_module_scope *scope = &r->out->scopes[module];
    const struct mo_syntax *items = r->set->modules[module]->tree->children[1];
    uint32_t count = 0;
    for (uint32_t i = 0; i < items->count; i++) {
        const struct mo_syntax *node = items->children[i];
        if (node->kind != MO_S_TEST)
            count++;
        if (node->kind == MO_S_ENUM)
            count += node->children[2]->count;
    }
    scope->symbols = allocate(r, count, sizeof(*scope->symbols), _Alignof(struct mo_symbol));
    if (scope->symbols == NULL)
        return false;
    for (uint32_t i = 0; i < items->count; i++) {
        const struct mo_syntax *node = items->children[i];
        if (node->kind == MO_S_TEST)
            continue;
        if (!add_symbol(r, scope, module, node, NULL))
            return false;
        if (node->kind == MO_S_ENUM) {
            const struct mo_syntax *cases = node->children[2];
            for (uint32_t j = 0; j < cases->count; j++)
                if (!add_symbol(r, scope, module, cases->children[j], node))
                    return false;
        }
    }
    qsort(scope->symbols, scope->count, sizeof(*scope->symbols), compare_symbols);
    r->requested[module] =
        allocate(r, scope->count, sizeof(struct requested), _Alignof(struct requested));
    if (r->requested[module] == NULL)
        return false;
    memset(r->requested[module], 0, (size_t)scope->count * sizeof(struct requested));
    uint32_t *seen = allocate(r, scope->count, sizeof(*seen), _Alignof(uint32_t));
    if (seen == NULL)
        return false;
    memset(seen, 0, (size_t)scope->count * sizeof(*seen));
    for (uint32_t i = 0; i < scope->count;) {
        struct mo_symbol *first = &scope->symbols[i];
        uint32_t end = i + 1;
        while (end < scope->count && strcmp(first->name, scope->symbols[end].name) == 0)
            end++;
        if (first->owner != NULL && builtin(first->name))
            return error(r, MO_CODE_CASE_CLASH, first->declaration->children[0]->span,
                         "a case reuses a type or case that is always in scope",
                         "No case reuses a type or case that is always in scope (reference 4.2).",
                         NULL);
        if (end - i > 1) {
            bool functions = true, cases = false;
            for (uint32_t j = i; j < end; j++) {
                functions = functions && scope->symbols[j].declaration->kind == MO_S_FN;
                cases = cases || scope->symbols[j].owner != NULL;
            }
            if (!functions)
                return error(r, cases ? MO_CODE_CASE_CLASH : MO_CODE_NAME_CLASH,
                             scope->symbols[i + 1].declaration->children[0]->span,
                             "a clash between names",
                             cases ? "A module's cases share one name space (reference 4.2)."
                                   : "A clash between names is an error (reference 10.3).",
                             first->declaration->children[0]);
            for (uint32_t j = i; j < end; j++) {
                struct mo_symbol *symbol = &scope->symbols[j];
                const struct mo_syntax *fn = symbol->declaration;
                const struct mo_syntax *parameters = fn->children[1];
                const struct mo_syntax *type =
                    parameters->count == 0 ? NULL : parameters->children[0]->children[1];
                struct mo_symbol *receiver = NULL;
                if (type != NULL && type->token != MO_T_FN && type->token != MO_T_LPAREN &&
                    type->children[0]->kind == MO_S_CAPNAME) {
                    char *name = spelling(r, type->children[0]);
                    if (name == NULL)
                        return false;
                    receiver = lookup(scope, name);
                }
                uint32_t id = receiver == NULL ? 0 : (uint32_t)(receiver - scope->symbols);
                if (receiver == NULL ||
                    (receiver->declaration->kind != MO_S_STRUCT &&
                     receiver->declaration->kind != MO_S_ENUM) ||
                    seen[id] == i + 1)
                    return error(r, MO_CODE_OVERLOAD, fn->children[0]->span,
                                 "functions share a name only for different types in their module",
                                 "Two functions share a name only when their first parameters are "
                                 "different types declared in that module (reference 5.1).",
                                 j == i ? NULL : first->declaration->children[0]);
                seen[id] = i + 1;
            }
        }
        i = end;
    }
    return true;
}

static MO_MUST_USE bool expose(struct resolver *r, uint32_t module) {
    struct mo_module_scope *scope = &r->out->scopes[module];
    const struct mo_syntax *list = r->set->modules[module]->header.expose;
    if (list != NULL) {
        for (uint32_t i = 0; i < list->count; i++) {
            const struct mo_syntax *item = list->children[i];
            bool all = item->kind == MO_S_EXPOSE_ALL;
            const struct mo_syntax *node = all ? item->children[0] : item;
            char *name = spelling(r, node);
            if (name == NULL)
                return false;
            struct mo_symbol *symbol = lookup(scope, name);
            if (symbol == NULL)
                return error(
                    r, MO_CODE_EXPOSE_NAME, node->span, "expose lists the module's public names",
                    "A module's expose line lists its public names (reference 10.1).", NULL);
            if (symbol->owner != NULL)
                return error(r, MO_CODE_EXPOSE_INSIDES, item->span,
                             "expose a type with (..) to expose its cases",
                             "Exposing a type without (..) hides its fields or cases (reference "
                             "10.2).",
                             symbol->owner->children[0]);
            if (symbol->exported && (!all || symbol->insides))
                continue;
            uint32_t at = (uint32_t)(symbol - scope->symbols);
            do {
                scope->symbols[at].exported = true;
                scope->symbols[at].insides = scope->symbols[at].insides || all;
                at++;
            } while (at < scope->count && strcmp(scope->symbols[at].name, name) == 0);
        }
    }
    const struct mo_syntax *items = r->set->modules[module]->tree->children[1];
    for (uint32_t i = 0; i < items->count; i++) {
        const struct mo_syntax *node = items->children[i];
        if (node->kind == MO_S_ENUM) {
            char *name = spelling(r, node->children[0]);
            if (name == NULL)
                return false;
            struct mo_symbol *owner = lookup(scope, name);
            MO_ASSERT(owner != NULL);
            const struct mo_syntax *cases = node->children[2];
            for (uint32_t j = 0; j < cases->count; j++) {
                char *case_name = spelling(r, cases->children[j]->children[0]);
                if (case_name == NULL)
                    return false;
                struct mo_symbol *symbol = lookup(scope, case_name);
                MO_ASSERT(symbol != NULL);
                symbol->exported = owner->insides;
            }
        }
    }
    return true;
}

static MO_MUST_USE int compare_imports(const void *a, const void *b) {
    const struct mo_imported_name *x = a, *y = b;
    int order = strcmp(x->name, y->name);
    if (order != 0)
        return order;
    return x->origin->span.start < y->origin->span.start   ? -1
           : x->origin->span.start > y->origin->span.start ? 1
                                                           : 0;
}

static MO_MUST_USE int compare_aliases(const void *a, const void *b) {
    const struct mo_module_alias *x = a, *y = b;
    int order = strcmp(x->name, y->name);
    if (order != 0)
        return order;
    return x->origin->span.start < y->origin->span.start   ? -1
           : x->origin->span.start > y->origin->span.start ? 1
                                                           : 0;
}

static MO_MUST_USE bool imports(struct resolver *r, uint32_t module) {
    struct mo_module_scope *scope = &r->out->scopes[module];
    const struct mo_syntax *uses = r->set->modules[module]->header.uses;
    scope->aliases =
        allocate(r, uses->count, sizeof(*scope->aliases), _Alignof(struct mo_module_alias));
    if (scope->aliases == NULL)
        return false;
    uint32_t count = 0;
    /* Count exact output first; enum cases are found through their owner once
     * per requested type, rather than re-scanning the whole module per import. */
    for (uint32_t i = 0; i < uses->count; i++) {
        const struct mo_syntax *use = uses->children[i];
        uint32_t target = r->set->uses[module][i];
        MO_ASSERT(target < r->set->count);
        const struct mo_syntax *path = use->children[0];
        char *alias = spelling(r, path->children[path->count - 1]);
        if (alias == NULL)
            return false;
        scope->aliases[scope->alias_count++] = (struct mo_module_alias){alias, target, use};
        const struct mo_syntax *names = use->children[1];
        if (names == NULL)
            continue;
        struct mo_module_scope *other = &r->out->scopes[target];
        for (uint32_t j = 0; j < names->count; j++) {
            char *name = spelling(r, names->children[j]);
            if (name == NULL)
                return false;
            struct mo_symbol *symbol = lookup(other, name);
            if (symbol == NULL || !symbol->exported)
                return error(r, MO_CODE_IMPORT_PRIVATE, names->children[j]->span,
                             "everything not on expose is private to the module",
                             "Everything not on expose is private to the module (reference "
                             "10.1).",
                             symbol == NULL ? NULL : symbol->declaration->children[0]);
            uint32_t id = (uint32_t)(symbol - other->symbols);
            struct requested *request = &r->requested[target][id];
            if (request->module == module + 1) {
                if (request->origin->span.start >= names->span.start) {
                    MO_ASSERT(j > 0);
                    struct mo_edit edit = {{names->span.file, names->children[j - 1]->span.end,
                                            names->children[j]->span.end},
                                           ""};
                    struct mo_fix fix = {"remove the repeated import", &edit, 1};
                    struct mo_label secondary = {request->origin->span, "declared here"};
                    bool kept = mo_diags_add(
                        r->diags,
                        &(struct mo_diag){
                            .code = MO_CODE_NAME_CLASH,
                            .primary = {names->children[j]->span, "a clash between names"},
                            .message = "a clash between names",
                            .reason = "A clash between names is an error; use the "
                                      "qualified form (reference 10.3).",
                            .secondary = &secondary,
                            .secondary_count = 1,
                            .fixes = &fix,
                            .fix_count = 1});
                    r->failed = !kept;
                    return false;
                }
                return error(r, MO_CODE_NAME_CLASH, names->children[j]->span,
                             "a clash between names",
                             "A clash between names is an error; use the qualified form "
                             "(reference 10.3).",
                             request->origin);
            }
            *request = (struct requested){module + 1, names->children[j]};
            uint32_t added = 1;
            if (symbol->insides && symbol->declaration->kind == MO_S_ENUM)
                added += symbol->declaration->children[2]->count;
            if (added > MO_LIMIT_MODULE_IMPORTS - count)
                return error(r, MO_CODE_TOO_MANY_IMPORTS, names->children[j]->span,
                             "the module imports too many names", NULL, NULL);
            count += added;
        }
    }
    scope->imported =
        allocate(r, count, sizeof(*scope->imported), _Alignof(struct mo_imported_name));
    if (scope->imported == NULL)
        return false;
    for (uint32_t i = 0; i < uses->count; i++) {
        const struct mo_syntax *names = uses->children[i]->children[1];
        if (names == NULL)
            continue;
        struct mo_module_scope *other = &r->out->scopes[r->set->uses[module][i]];
        for (uint32_t j = 0; j < names->count; j++) {
            char *name = spelling(r, names->children[j]);
            if (name == NULL)
                return false;
            struct mo_symbol *symbol = lookup(other, name);
            MO_ASSERT(symbol != NULL);
            uint32_t at = (uint32_t)(symbol - other->symbols), end = at + 1;
            while (end < other->count && strcmp(other->symbols[end].name, name) == 0)
                end++;
            scope->imported[scope->imported_count++] =
                (struct mo_imported_name){symbol->name, symbol, end - at, names->children[j]};
            if (symbol->insides && symbol->declaration->kind == MO_S_ENUM) {
                const struct mo_syntax *cases = symbol->declaration->children[2];
                for (uint32_t k = 0; k < cases->count; k++) {
                    char *case_name = spelling(r, cases->children[k]->children[0]);
                    if (case_name == NULL)
                        return false;
                    struct mo_symbol *case_symbol = lookup(other, case_name);
                    MO_ASSERT(case_symbol != NULL);
                    scope->imported[scope->imported_count++] = (struct mo_imported_name){
                        case_symbol->name, case_symbol, 1, names->children[j]};
                }
            }
        }
    }
    qsort(scope->aliases, scope->alias_count, sizeof(*scope->aliases), compare_aliases);
    for (uint32_t i = 1; i < scope->alias_count; i++)
        if (strcmp(scope->aliases[i - 1].name, scope->aliases[i].name) == 0)
            return error(r, MO_CODE_NAME_CLASH, scope->aliases[i].origin->span,
                         "a clash between names",
                         "A clash between names is an error (reference 10.3).",
                         scope->aliases[i - 1].origin);
    qsort(scope->imported, scope->imported_count, sizeof(*scope->imported), compare_imports);
    for (uint32_t i = 0; i < scope->imported_count; i++) {
        struct mo_imported_name *item = &scope->imported[i];
        struct mo_symbol *local = lookup(scope, item->name);
        const struct mo_syntax *previous = local == NULL ? NULL : local->declaration->children[0];
        if (i != 0 && strcmp(scope->imported[i - 1].name, item->name) == 0)
            previous = scope->imported[i - 1].origin;
        if (previous != NULL)
            return error(r, MO_CODE_NAME_CLASH, item->origin->span, "a clash between names",
                         "A clash between names is an error; use the qualified form "
                         "(reference 10.3).",
                         previous);
    }
    return true;
}

struct mo_module_interfaces *mo_resolve_modules(struct mo_arena *arena,
                                                const struct mo_module_set *set,
                                                struct mo_diags *diags) {
    MO_ASSERT(arena != NULL && set != NULL && diags != NULL);
    MO_ASSERT(set->count > 0 && set->count <= MO_LIMIT_FILES);
    struct resolver r = {.arena = arena, .diags = diags, .set = set};
    r.out = allocate(&r, 1, sizeof(*r.out), _Alignof(struct mo_module_interfaces));
    if (r.out == NULL)
        return NULL;
    *r.out = (struct mo_module_interfaces){.count = set->count};
    r.out->scopes =
        allocate(&r, set->count, sizeof(*r.out->scopes), _Alignof(struct mo_module_scope));
    if (r.out->scopes == NULL)
        return NULL;
    memset(r.out->scopes, 0, (size_t)set->count * sizeof(*r.out->scopes));
    r.requested = allocate(&r, set->count, sizeof(*r.requested), _Alignof(struct requested *));
    if (r.requested == NULL)
        return NULL;
    for (uint32_t i = 0; i < set->count; i++)
        if (!declarations(&r, i) || !expose(&r, i)) {
            mo_diags_sort(diags);
            return NULL;
        }
    for (uint32_t i = 0; i < set->count; i++)
        if (!imports(&r, i)) {
            mo_diags_sort(diags);
            return NULL;
        }
    return r.out;
}
