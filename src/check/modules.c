#include "check/check.h"
#include "check/internal.h"

#include "base/codes.h"
#include "base/limits.h"

#include <stdlib.h>
#include <string.h>

struct requested {
    uint32_t module; /* requesting module + 1; zero is unused */
    uint32_t listed; /* explicitly listed, including later use lines */
    const struct mo_syntax *origin;
};

struct resolver {
    struct mo_arena *arena;
    struct mo_diags *diags;
    const struct mo_module_set *set;
    struct mo_module_interfaces *out;
    struct requested **requested;
    bool *valid;
    uint32_t expanded;
    bool failed;
    bool errors;
    bool import_limit;
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

/* Language errors do not stop independent name groups. Only failure to keep a
 * diagnostic or allocate storage stops the resolver. */
static MO_MUST_USE bool error(struct resolver *r, uint16_t code, struct mo_span span,
                              const char *message, const char *reason, const char *name,
                              const struct mo_syntax *previous) {
    struct mo_label secondary = {previous == NULL ? span : previous->span,
                                 "declared or imported here"};
    r->errors = true;
    bool kept =
        mo_diags_add(r->diags, &(struct mo_diag){.code = code,
                                                 .primary = {span, name},
                                                 .message = message,
                                                 .reason = reason,
                                                 .secondary = previous == NULL ? NULL : &secondary,
                                                 .secondary_count = previous == NULL ? 0 : 1});
    r->failed = !kept;
    return kept;
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

static MO_MUST_USE int compare_name(const struct mo_symbol *stored, const uint8_t *name,
                                    size_t length) {
    size_t n = stored->name_length, common = n < length ? n : length;
    int order = memcmp(stored->name, name, common);
    return order != 0 ? order : n < length ? -1 : n > length ? 1 : 0;
}

static MO_MUST_USE uint32_t lower(const struct mo_module_scope *scope, const uint8_t *name,
                                  size_t length) {
    uint32_t lo = 0, hi = scope->count;
    while (lo < hi) {
        uint32_t middle = lo + (hi - lo) / 2;
        if (compare_name(&scope->symbols[middle], name, length) < 0)
            lo = middle + 1;
        else
            hi = middle;
    }
    return lo;
}

const struct mo_symbol *mo_find_symbol(const struct mo_module_scope *scope, const uint8_t *name,
                                       size_t length) {
    MO_ASSERT(scope != NULL && name != NULL);
    uint32_t index = lower(scope, name, length);
    return index == scope->count || compare_name(&scope->symbols[index], name, length) != 0
               ? NULL
               : &scope->symbols[index];
}

static MO_MUST_USE struct mo_symbol *lookup(struct mo_module_scope *scope,
                                            const struct mo_syntax *name) {
    uint32_t index = lower(scope, name->text, name->span.end - name->span.start);
    return index == scope->count || compare_name(&scope->symbols[index], name->text,
                                                 name->span.end - name->span.start) != 0
               ? NULL
               : &scope->symbols[index];
}

static MO_MUST_USE bool add_symbol(struct resolver *r, struct mo_module_scope *scope,
                                   uint32_t module, const struct mo_syntax *node, uint32_t owner) {
    char *name = spelling(r, node->children[0]);
    if (name == NULL)
        return false;
    /* receiver temporarily holds the original index until sorting remaps owners. */
    scope->symbols[scope->count] = (struct mo_symbol){.name = name,
                                                      .declaration = node,
                                                      .owner = owner,
                                                      .name_length = node->children[0]->span.end -
                                                                     node->children[0]->span.start,
                                                      .overload_count = 1,
                                                      .receiver = scope->count,
                                                      .module = module};
    scope->count++;
    return true;
}

bool mo_source_builtin(const uint8_t *name, size_t length) {
    MO_ASSERT(name != NULL);
    static const char *const names[] = {"Bool",        "Int",
                                        "BigInt",      "Byte",
                                        "Decimal",     "Float",
                                        "Text",        "Char",
                                        "Bytes",       "Unit",
                                        "List",        "Map",
                                        "Set",         "Range",
                                        "Time",        "Duration",
                                        "Option",      "None",
                                        "Some",        "Result",
                                        "Ok",          "Error",
                                        "Ordering",    "Less",
                                        "Equal",       "Greater",
                                        "Args",        "Env",
                                        "Console",     "Files",
                                        "ReadFiles",   "Clock",
                                        "Random",      "FileError",
                                        "NotFound",    "NotAllowed",
                                        "NotText",     "Failed",
                                        "Entry",       "EntryKind",
                                        "File",        "Folder",
                                        "Link",        "Other",
                                        "Simulation",  "SimulationError",
                                        "ServiceDown", "ToolFailed"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (length == strlen(names[i]) && memcmp(name, names[i], length) == 0)
            return true;
    return false;
}

static MO_MUST_USE bool reserved(struct resolver *r, const struct mo_syntax *node,
                                 const char *name) {
    return error(r, MO_CODE_ALWAYS_IN_SCOPE, node->span,
                 "a name clashes with a name always in scope",
                 "A clash between names is an error (reference 10.3); these names are always "
                 "in scope (reference 10.5).",
                 name, NULL);
}

static MO_MUST_USE bool declarations(struct resolver *r, uint32_t module) {
    struct mo_module_scope *scope = &r->out->scopes[module];
    const struct mo_syntax *items = r->set->modules[module]->tree->children[1];
    uint32_t count = 0, case_count = 0;
    for (uint32_t i = 0; i < items->count; i++) {
        const struct mo_syntax *node = items->children[i];
        if (node->kind != MO_S_TEST && node->kind != MO_S_SERVICE)
            count++;
        if (node->kind == MO_S_ENUM)
            case_count += node->children[2]->count;
    }
    count += case_count;
    scope->symbols = allocate(r, count, sizeof(*scope->symbols), _Alignof(struct mo_symbol));
    if (scope->symbols == NULL)
        return false;
    for (uint32_t i = 0; i < items->count; i++) {
        const struct mo_syntax *node = items->children[i];
        if (node->kind == MO_S_TEST || node->kind == MO_S_SERVICE)
            continue;
        uint32_t owner = scope->count;
        if (!add_symbol(r, scope, module, node, UINT32_MAX))
            return false;
        if (node->kind == MO_S_ENUM) {
            const struct mo_syntax *cases = node->children[2];
            for (uint32_t j = 0; j < cases->count; j++)
                if (!add_symbol(r, scope, module, cases->children[j], owner))
                    return false;
        }
    }
    qsort(scope->symbols, scope->count, sizeof(*scope->symbols), compare_symbols);
    uint32_t *remap = allocate(r, count, sizeof(*remap), _Alignof(uint32_t));
    uint32_t *seen = allocate(r, count, sizeof(*seen), _Alignof(uint32_t));
    uint32_t *cases = allocate(r, case_count, sizeof(*cases), _Alignof(uint32_t));
    r->requested[module] = allocate(r, count, sizeof(struct requested), _Alignof(struct requested));
    if (r->failed)
        return false;
    memset(seen, 0, (size_t)count * sizeof(*seen));
    memset(r->requested[module], 0, (size_t)count * sizeof(struct requested));
    for (uint32_t i = 0; i < count; i++)
        remap[scope->symbols[i].receiver] = i;
    for (uint32_t i = 0; i < count; i++) {
        struct mo_symbol *symbol = &scope->symbols[i];
        symbol->receiver = UINT32_MAX;
        if (symbol->owner != UINT32_MAX) {
            symbol->owner = remap[symbol->owner];
            scope->symbols[symbol->owner].case_count++;
        }
    }
    uint32_t next = 0;
    for (uint32_t i = 0; i < count; i++) {
        struct mo_symbol *symbol = &scope->symbols[i];
        if (symbol->case_count != 0) {
            symbol->cases = cases + next;
            seen[i] = next;
            next += symbol->case_count;
        }
    }
    MO_ASSERT(next == case_count);
    /* Iterate original indices, keeping cases in source order, not name order. */
    for (uint32_t i = 0; i < count; i++) {
        uint32_t id = remap[i], owner = scope->symbols[id].owner;
        if (owner != UINT32_MAX)
            cases[seen[owner]++] = id;
    }
    memset(seen, 0, (size_t)count * sizeof(*seen));
    for (uint32_t i = 0; i < count; i++) {
        struct mo_symbol *symbol = &scope->symbols[i];
        if (symbol->declaration->kind != MO_S_FN)
            continue;
        const struct mo_syntax *params = symbol->declaration->children[1];
        const struct mo_syntax *type = params->count == 0 ? NULL : params->children[0]->children[1];
        if (type != NULL && type->token != MO_T_FN && type->token != MO_T_LPAREN &&
            type->children[0]->kind == MO_S_CAPNAME) {
            struct mo_symbol *receiver = lookup(scope, type->children[0]);
            if (receiver != NULL && (receiver->declaration->kind == MO_S_STRUCT ||
                                     receiver->declaration->kind == MO_S_ENUM))
                symbol->receiver = (uint32_t)(receiver - scope->symbols);
        }
    }
    for (uint32_t i = 0; i < count;) {
        struct mo_symbol *first = &scope->symbols[i];
        uint32_t end = i + 1;
        while (end < count && strcmp(first->name, scope->symbols[end].name) == 0)
            end++;
        bool functions = true, all_cases = true;
        for (uint32_t j = i; j < end; j++) {
            const struct mo_symbol *symbol = &scope->symbols[j];
            functions = functions && symbol->declaration->kind == MO_S_FN;
            all_cases = all_cases && symbol->owner != UINT32_MAX;
        }
        if (mo_source_builtin((const uint8_t *)first->name, first->name_length)) {
            bool kept =
                all_cases
                    ? error(
                          r, MO_CODE_CASE_CLASH, first->declaration->children[0]->span,
                          "a choice case clashes with another name",
                          "No case reuses a type or case that is always in scope (reference 4.2).",
                          first->name, NULL)
                    : reserved(r, first->declaration->children[0], first->name);
            if (!kept)
                return false;
        } else if (end - i > 1 && !functions) {
            for (uint32_t j = i + 1; j < end; j++)
                if (!error(r, all_cases ? MO_CODE_CASE_CLASH : MO_CODE_NAME_CLASH,
                           scope->symbols[j].declaration->children[0]->span,
                           all_cases ? "a choice case clashes with another name"
                                     : "a clash between names",
                           all_cases ? "A module's cases share one name space (reference 4.2)."
                                     : "A clash between names is an error (reference 10.3).",
                           first->name, first->declaration->children[0]))
                    return false;
        } else if (end - i > 1) {
            first->overload_count = end - i;
            for (uint32_t j = i; j < end; j++) {
                struct mo_symbol *symbol = &scope->symbols[j];
                uint32_t receiver = symbol->receiver;
                if (receiver == UINT32_MAX || seen[receiver] == i + 1) {
                    /* One invalid overload group, with both declarations labeled. */
                    if (!error(r, MO_CODE_OVERLOAD, symbol->declaration->children[0]->span,
                               "the functions don't have different local receiver types",
                               "Two functions share a name only when their first parameters are "
                               "different types declared in that module (reference 5.1).",
                               symbol->name,
                               scope->symbols[j == i ? i + 1 : i].declaration->children[0]))
                        return false;
                    break;
                }
                seen[receiver] = i + 1;
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
            struct mo_symbol *symbol = lookup(scope, node);
            if (symbol == NULL) {
                char *name = spelling(r, node);
                if (name == NULL ||
                    !error(r, MO_CODE_EXPOSE_NAME, node->span, "an exposed name doesn't exist",
                           "A module's expose line lists its public names "
                           "(reference 10.1).",
                           name, NULL))
                    return false;
                continue;
            }
            if (symbol->owner != UINT32_MAX) {
                if (!error(
                        r, MO_CODE_EXPOSE_INSIDES, item->span, "a case is exposed without its type",
                        "Exposing a type without (..) hides its fields or cases (reference 10.2).",
                        symbol->name, scope->symbols[symbol->owner].declaration->children[0]))
                    return false;
                continue;
            }
            if (symbol->exported && (!all || symbol->insides))
                continue;
            uint32_t at = (uint32_t)(symbol - scope->symbols);
            do {
                scope->symbols[at].exported = true;
                scope->symbols[at].insides = scope->symbols[at].insides || all;
                at++;
            } while (at < scope->count && strcmp(scope->symbols[at].name, symbol->name) == 0);
        }
    }
    for (uint32_t i = 0; i < scope->count; i++) {
        struct mo_symbol *symbol = &scope->symbols[i];
        if (symbol->owner != UINT32_MAX)
            symbol->exported = scope->symbols[symbol->owner].insides;
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

static MO_MUST_USE bool case_import(struct resolver *r, const struct mo_module_scope *other,
                                    const struct mo_symbol *symbol, const struct mo_syntax *names,
                                    uint32_t index, uint32_t module) {
    const struct mo_syntax *node = names->children[index];
    const struct mo_symbol *owner = &other->symbols[symbol->owner];
    struct mo_edit edit = {node->span, owner->name};
    struct mo_fix fix = {"import the choice type instead", &edit, 1};
    bool already = r->requested[symbol->module][symbol->owner].listed == module + 1;
    bool offer = symbol->exported && (!already || names->count > 1);
    if (already && names->count > 1) {
        edit.span.start = index == 0 ? node->span.start : names->children[index - 1]->span.end;
        edit.span.end = index == 0 ? names->children[1]->span.start : node->span.end;
        edit.replacement = "";
        fix.title = "remove the case; its type is already imported";
    }
    struct mo_label secondary = {owner->declaration->children[0]->span, "the owning choice type"};
    r->errors = true;
    bool kept = mo_diags_add(
        r->diags,
        &(struct mo_diag){
            .code = symbol->exported ? MO_CODE_IMPORT_CASE : MO_CODE_IMPORT_HIDDEN_CASE,
            .primary = {node->span, symbol->name},
            .message = symbol->exported ? "a case is imported without its type"
                                        : "the choice's cases are hidden",
            .reason =
                symbol->exported
                    ? "Importing a choice type brings its exposed cases with it (reference 10.3)."
                    : "Exposing a type without (..) hides its fields or cases (reference 10.2).",
            .secondary = &secondary,
            .secondary_count = 1,
            .fixes = offer ? &fix : NULL,
            .fix_count = offer ? 1 : 0});
    r->failed = !kept;
    return kept;
}

static MO_MUST_USE bool imports(struct resolver *r, uint32_t module) {
    struct mo_module_scope *scope = &r->out->scopes[module];
    const struct mo_syntax *uses = r->set->modules[module]->header.uses;
    scope->aliases =
        allocate(r, uses->count, sizeof(*scope->aliases), _Alignof(struct mo_module_alias));
    if (scope->aliases == NULL)
        return false;
    uint32_t count = 0;
    for (uint32_t i = 0; i < uses->count; i++) {
        uint32_t target = r->set->uses[module][i];
        MO_ASSERT(target < r->set->count);
        const struct mo_syntax *names = uses->children[i]->children[1];
        if (names == NULL || !r->valid[target])
            continue;
        struct mo_module_scope *other = &r->out->scopes[target];
        for (uint32_t j = 0; j < names->count; j++) {
            struct mo_symbol *symbol = lookup(other, names->children[j]);
            if (symbol != NULL)
                r->requested[target][(uint32_t)(symbol - other->symbols)].listed = module + 1;
        }
    }
    for (uint32_t i = 0; i < uses->count; i++) {
        const struct mo_syntax *use = uses->children[i];
        uint32_t target = r->set->uses[module][i];
        MO_ASSERT(target < r->set->count);
        const struct mo_syntax *path = use->children[0];
        const struct mo_syntax *last = path->children[path->count - 1];
        char *alias = spelling(r, last);
        if (alias == NULL)
            return false;
        if (mo_source_builtin((const uint8_t *)alias, strlen(alias))) {
            if (!reserved(r, last, alias))
                return false;
        } else
            scope->aliases[scope->alias_count++] = (struct mo_module_alias){alias, target, use};
        const struct mo_syntax *names = use->children[1];
        if (names == NULL || !r->valid[target])
            continue;
        struct mo_module_scope *other = &r->out->scopes[target];
        for (uint32_t j = 0; j < names->count; j++) {
            const struct mo_syntax *node = names->children[j];
            struct mo_symbol *symbol = lookup(other, node);
            char *name = symbol == NULL ? spelling(r, node) : NULL;
            if (symbol == NULL && name == NULL)
                return false;
            const char *text = symbol == NULL ? name : symbol->name;
            if (mo_source_builtin((const uint8_t *)text, strlen(text))) {
                if (!reserved(r, node, text))
                    return false;
                continue;
            }
            if (symbol == NULL || (!symbol->exported && symbol->owner == UINT32_MAX)) {
                if (!error(
                        r, symbol == NULL ? MO_CODE_IMPORT_MISSING : MO_CODE_IMPORT_PRIVATE,
                        node->span,
                        symbol == NULL ? "the imported name doesn't exist"
                                       : "the imported name is private",
                        symbol == NULL
                            ? "Use brings in names declared by the module (reference 10.3)."
                            : "Everything not on expose is private to the module (reference 10.1).",
                        text, symbol == NULL ? NULL : symbol->declaration->children[0]))
                    return false;
                continue;
            }
            if (symbol->owner != UINT32_MAX) {
                if (!case_import(r, other, symbol, names, j, module))
                    return false;
                continue;
            }
            uint32_t id = (uint32_t)(symbol - other->symbols);
            struct requested *request = &r->requested[target][id];
            if (request->module == module + 1) {
                if (request->origin->span.start >= names->span.start) {
                    MO_ASSERT(j > 0);
                    struct mo_edit edit = {
                        {names->span.file, names->children[j - 1]->span.end, node->span.end}, ""};
                    struct mo_fix fix = {"remove the repeated import", &edit, 1};
                    struct mo_label secondary = {request->origin->span, "first imported here"};
                    r->errors = true;
                    bool kept = mo_diags_add(
                        r->diags,
                        &(struct mo_diag){.code = MO_CODE_NAME_CLASH,
                                          .primary = {node->span, text},
                                          .message = "a clash between names",
                                          .reason = "A clash between names is an error; use the "
                                                    "qualified form (reference 10.3).",
                                          .secondary = &secondary,
                                          .secondary_count = 1,
                                          .fixes = &fix,
                                          .fix_count = 1});
                    r->failed = !kept;
                    if (!kept)
                        return false;
                } else if (!error(r, MO_CODE_NAME_CLASH, node->span, "a clash between names",
                                  "A clash between names is an error; use the qualified form "
                                  "(reference 10.3).",
                                  text, request->origin))
                    return false;
                continue;
            }
            uint32_t added = 1 + (symbol->insides ? symbol->case_count : 0);
            if (added > MO_LIMIT_MODULE_IMPORTS - count) {
                bool kept = error(r, MO_CODE_TOO_MANY_IMPORTS, node->span,
                                  "the module imports too many names", NULL, text, NULL);
                (void)kept;
                return false;
            }
            if (added > MO_LIMIT_PROGRAM_IMPORTS - r->expanded) {
                r->import_limit = true;
                bool kept = error(r, MO_CODE_TOO_MANY_PROGRAM_IMPORTS, node->span,
                                  "the program imports too many names", NULL, text, NULL);
                (void)kept;
                return false;
            }
            request->module = module + 1;
            request->origin = node;
            count += added;
            r->expanded += added;
        }
    }
    scope->imported =
        allocate(r, count, sizeof(*scope->imported), _Alignof(struct mo_imported_name));
    if (scope->imported == NULL)
        return false;
    for (uint32_t i = 0; i < uses->count; i++) {
        const struct mo_syntax *names = uses->children[i]->children[1];
        uint32_t target = r->set->uses[module][i];
        if (names == NULL || !r->valid[target])
            continue;
        struct mo_module_scope *other = &r->out->scopes[target];
        for (uint32_t j = 0; j < names->count; j++) {
            struct mo_symbol *symbol = lookup(other, names->children[j]);
            if (symbol == NULL)
                continue;
            uint32_t at = (uint32_t)(symbol - other->symbols);
            struct requested *request = &r->requested[target][at];
            if (request->module != module + 1 || request->origin != names->children[j])
                continue;
            scope->imported[scope->imported_count++] = (struct mo_imported_name){
                symbol->name, symbol, symbol->overload_count, names->children[j]};
            if (symbol->insides) {
                for (uint32_t k = 0; k < symbol->case_count; k++) {
                    const struct mo_symbol *member = &other->symbols[symbol->cases[k]];
                    scope->imported[scope->imported_count++] =
                        (struct mo_imported_name){member->name, member, 1, names->children[j]};
                }
            }
        }
    }
    MO_ASSERT(scope->imported_count == count);
    qsort(scope->aliases, scope->alias_count, sizeof(*scope->aliases), compare_aliases);
    for (uint32_t i = 1; i < scope->alias_count; i++)
        if (strcmp(scope->aliases[i - 1].name, scope->aliases[i].name) == 0 &&
            !error(r, MO_CODE_NAME_CLASH, scope->aliases[i].origin->span, "a clash between names",
                   "A clash between names is an error (reference 10.3).", scope->aliases[i].name,
                   scope->aliases[i - 1].origin))
            return false;
    qsort(scope->imported, scope->imported_count, sizeof(*scope->imported), compare_imports);
    for (uint32_t i = 0; i < scope->imported_count; i++) {
        struct mo_imported_name *item = &scope->imported[i];
        const struct mo_symbol *local =
            mo_find_symbol(scope, (const uint8_t *)item->name, strlen(item->name));
        const struct mo_syntax *previous = local == NULL ? NULL : local->declaration->children[0];
        if (i != 0 && strcmp(scope->imported[i - 1].name, item->name) == 0)
            previous = scope->imported[i - 1].origin;
        if (previous != NULL &&
            !error(r, MO_CODE_NAME_CLASH, item->origin->span, "a clash between names",
                   "A clash between names is an error; use the qualified form (reference 10.3).",
                   item->name, previous))
            return false;
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
    r.requested = allocate(&r, set->count, sizeof(*r.requested), _Alignof(struct requested *));
    r.valid = allocate(&r, set->count, sizeof(*r.valid), _Alignof(bool));
    if (r.failed)
        return NULL;
    memset(r.out->scopes, 0, (size_t)set->count * sizeof(*r.out->scopes));
    for (uint32_t i = 0; i < set->count; i++) {
        struct mo_module_scope *scope = &r.out->scopes[i];
        scope->library = set->library != NULL && set->library[i];
        if (set->names != NULL) {
            size_t length = strlen(set->names[i]) + 1;
            char *name = allocate(&r, length, 1, 1);
            if (name == NULL)
                goto done;
            memcpy(name, set->names[i], length);
            scope->name = name;
        }
        uint32_t before = diags->count;
        if (!declarations(&r, i) || !expose(&r, i))
            goto done;
        r.valid[i] = before == diags->count;
    }
    for (uint32_t i = 0; i < set->count && !r.import_limit; i++) {
        bool ok = imports(&r, i);
        if (!ok && r.failed)
            break;
    }
done:
    mo_diags_sort(diags);
    return r.errors || r.failed ? NULL : r.out;
}
