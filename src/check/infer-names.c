#include "check/checked.h"
#include "check/infer.h"
#include "check/internal.h"
#include "check/native.h"
#include "check/signatures.h"

#include "base/codes.h"
#include "base/limits.h"

#include <stdio.h>
#include <string.h>

/* The reason for an unknown name, called or dot-called, that another language uses for a length
 * (reference appendix C), or NULL. */
const char *infer_length_habit(const struct mo_syntax *name) {
    return infer_compare_text(name, "len") == 0 || infer_compare_text(name, "length") == 0
               ? "The library has no len or length: a list's length is list.size, and a text's "
                 "is text.char_count or text.byte_count (reference 3.4)."
               : NULL;
}

static MO_MUST_USE uint32_t height(const struct name_slot *s) {
    return s == NULL ? 0 : s->height;
}

static void refresh(struct name_slot *s) {
    uint32_t a = height(s->left), b = height(s->right);
    s->height = 1 + (a > b ? a : b);
}

static MO_MUST_USE struct name_slot *rotate(struct name_slot *s, bool right) {
    struct name_slot *next = right ? s->left : s->right;
    if (right) {
        s->left = next->right;
        next->right = s;
    } else {
        s->right = next->left;
        next->left = s;
    }
    refresh(s);
    refresh(next);
    return next;
}

static MO_MUST_USE struct name_slot *insert(struct name_slot *s, struct name_slot *entry) {
    if (s == NULL)
        return entry;
    int order = infer_compare_name(entry->name, s->name);
    MO_ASSERT(order != 0);
    if (order < 0)
        s->left = insert(s->left, entry);
    else
        s->right = insert(s->right, entry);
    refresh(s);
    if (height(s->left) > height(s->right) + 1) {
        if (height(s->left->right) > height(s->left->left))
            s->left = rotate(s->left, false);
        return rotate(s, true);
    }
    if (height(s->right) > height(s->left) + 1) {
        if (height(s->right->left) > height(s->right->right))
            s->right = rotate(s->right, true);
        return rotate(s, false);
    }
    return s;
}

MO_MUST_USE struct name_slot *infer_slot(struct inference *p, const struct mo_syntax *name,
                                         bool create) {
    struct name_slot *at = p->names;
    while (at != NULL) {
        int order = infer_compare_name(name, at->name);
        if (order == 0)
            return at;
        at = order < 0 ? at->left : at->right;
    }
    if (!create)
        return NULL;
    struct name_slot *entry = infer_allocate(p, 1, sizeof(*entry), _Alignof(struct name_slot));
    if (entry != NULL) {
        *entry = (struct name_slot){.name = name, .height = 1};
        p->names = insert(p->names, entry);
    }
    return entry;
}

static MO_MUST_USE bool record_binding(struct inference *p, const struct mo_syntax *name,
                                       struct term *type, bool variable) {
    if (!infer_record(p, name, (struct value){.type = type, .binding = name}))
        return false;
    if (p->checked && name->kind != MO_S_UNDERSCORE) {
        p->last->binding = p->last;
        p->last->variable = variable;
        p->binding_count++;
    }
    return true;
}

MO_MUST_USE bool infer_bind(struct inference *p, const struct mo_syntax *name, struct term *type,
                            bool variable) {
    if (name->kind == MO_S_UNDERSCORE)
        return record_binding(p, name, type, variable);
    struct name_slot *s = infer_slot(p, name, true);
    if (s == NULL)
        return false;
    if (s->active != NULL)
        return infer_problem(p, MO_CODE_BIND_REPEATED, name->span, "a visible name is bound again",
                             "While a local name or parameter is visible it can't be bound again, "
                             "even in an inner block (reference 6.1).",
                             s->active->name);
    if (s->module_use != NULL)
        return infer_problem(
            p, MO_CODE_BIND_REPEATED, name->span, "a name means both a local and a module function",
            "A function can't refer to its module's function by a name it uses for "
            "a local (reference 6.1).",
            s->module_use);
    struct local *l = infer_allocate(p, 1, sizeof(*l), _Alignof(struct local));
    if (l == NULL)
        return false;
    *l = (struct local){.slot = s,
                        .name = name,
                        .type = type,
                        .variable = variable,
                        .previous = p->locals,
                        .next = p->all_locals,
                        .function = p->lambda};
    s->local_name = name;
    s->active = l;
    p->locals = l;
    p->all_locals = l;
    if (!record_binding(p, name, type, variable))
        return false;
    l->binding = p->last;
    return true;
}

MO_MUST_USE bool infer_leave(struct inference *p, struct local *mark) {
    while (p->locals != mark) {
        struct local *l = p->locals;
        if (!l->used && l->name->text[0] != '_') {
            p->unused = l;
            infer_defer(p, MO_CODE_UNUSED_NAME, l->name->span, "a name or parameter is unused",
                        "A name starting with _ is unused on purpose (reference 6.1).");
        }
        l->slot->active = NULL;
        l->slot->expired = l;
        p->locals = l->previous;
    }
    return true;
}

static MO_MUST_USE bool capture_local(struct inference *p, struct local *local,
                                      struct mo_span use) {
    for (struct emitted_lambda *lambda = p->lambda_frame;
         lambda != NULL && local->function < lambda->depth; lambda = lambda->parent) {
        if (!infer_work(p))
            return false;
        struct capture **at = &lambda->captures;
        while (*at != NULL && (*at)->local != local) {
            if (!infer_work(p))
                return false;
            at = &(*at)->next;
        }
        struct capture *value = *at;
        if (value != NULL) {
            if (value->use.start <= use.start)
                continue;
            *at = value->next;
        } else {
            if (lambda->output.capture_count == MO_LIMIT_FIELDS)
                return infer_problem(p, MO_CODE_TOO_MANY_FIELDS, use,
                                     "an anonymous function captures too many values", NULL, NULL);
            value = infer_allocate(p, 1, sizeof(*value), _Alignof(struct capture));
            if (value == NULL)
                return false;
            lambda->output.capture_count++;
        }
        *value = (struct capture){.local = local, .use = use};
        at = &lambda->captures;
        while (*at != NULL && (*at)->use.start < use.start) {
            if (!infer_work(p))
                return false;
            at = &(*at)->next;
        }
        value->next = *at;
        *at = value;
    }
    return true;
}

MO_MUST_USE bool infer_capture_write(struct inference *p, struct local *local,
                                     const struct mo_syntax *syntax,
                                     const struct mo_syntax *target) {
    if (!p->checked)
        return true;
    struct capture_write *write =
        infer_allocate(p, 1, sizeof(*write), _Alignof(struct capture_write));
    if (write == NULL)
        return false;
    *write = (struct capture_write){syntax, target, p->capture_path, local->writes};
    local->writes = write;
    return true;
}

MO_MUST_USE bool infer_capture_path(struct inference *p, const struct mo_syntax *group,
                                    uint32_t arm) {
    if (!p->checked)
        return true;
    struct capture_path *path = infer_allocate(p, 1, sizeof(*path), _Alignof(struct capture_path));
    if (path == NULL)
        return false;
    *path = (struct capture_path){group, arm, p->capture_path};
    p->capture_path = path;
    return true;
}

MO_MUST_USE const struct mo_symbol *infer_find_symbol(const struct mo_module_scope *scope,
                                                      const struct mo_syntax *name,
                                                      uint32_t *count) {
    const struct mo_symbol *s =
        mo_find_symbol(scope, name->text, name->span.end - name->span.start);
    if (s != NULL)
        *count = s->overload_count;
    return s;
}

MO_MUST_USE const struct mo_data_signature *infer_data_type(struct inference *p,
                                                            const struct mo_symbol *symbol) {
    if (p->signatures->recovering &&
        (symbol->declaration->kind == MO_S_STRUCT || symbol->declaration->kind == MO_S_CASE_DECL ||
         symbol->declaration->kind == MO_S_ENUM)) {
        const struct mo_symbol *owner =
            symbol->owner == UINT32_MAX
                ? symbol
                : &p->interfaces->scopes[symbol->module].symbols[symbol->owner];
        const struct mo_nominal_signature *nominal =
            mo_nominal_signature_by_symbol(p->signatures, owner);
        if (nominal == NULL || nominal->invalid) {
            p->failed = p->fatal = p->unavailable = true;
            return NULL;
        }
    }
    return mo_data_signature_by_symbol(p->signatures, symbol);
}

MO_MUST_USE bool infer_visible_data(struct inference *p, const struct mo_symbol *owner) {
    if (owner->module == p->function->function->module)
        return true;
    if (!owner->insides)
        return infer_problem(
            p, MO_CODE_DATA_PRIVATE, p->span, "the type's fields or cases are hidden",
            "Outside its module a type exposed without (..) can't be built, taken apart "
            "or have its fields read (reference 10.2).",
            owner->declaration->children[0]);
    return true;
}

MO_MUST_USE const struct mo_field_signature *infer_find_field(const struct mo_data_signature *data,
                                                              const struct mo_syntax *name) {
    if (data->unnamed)
        return NULL;
    uint32_t lo = 0, hi = data->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (infer_compare_name(data->fields[mid].name, name) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < data->count && infer_compare_name(data->fields[lo].name, name) == 0
               ? &data->fields[lo]
               : NULL;
}

/* The index of the module alias spelled prefix, or alias_count when there is none. */
static MO_MUST_USE uint32_t find_alias(const struct mo_module_scope *scope,
                                       const struct mo_syntax *prefix) {
    uint32_t lo = 0, hi = scope->alias_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (infer_compare_text(prefix, scope->aliases[mid].name) > 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < scope->alias_count && infer_compare_text(prefix, scope->aliases[lo].name) == 0
               ? lo
               : scope->alias_count;
}

/* The module a call prefix names: a use alias, or a library module always in
 * scope. *alias is the alias index, or UINT32_MAX. */
MO_MUST_USE const struct mo_module_scope *
infer_prefix_module(struct inference *p, const struct mo_syntax *prefix, uint32_t *alias) {
    const struct mo_module_scope *scope = &p->interfaces->scopes[p->function->function->module];
    uint32_t lo = find_alias(scope, prefix);
    *alias = UINT32_MAX;
    if (lo < scope->alias_count) {
        *alias = lo;
        return &p->interfaces->scopes[scope->aliases[lo].module];
    }
    bool implicit = mo_source_builtin(prefix->text, prefix->span.end - prefix->span.start) ||
                    (scope->library && infer_compare_text(prefix, scope->name) == 0);
    for (uint32_t i = 0; implicit && i < p->interfaces->count; i++) {
        if (!infer_work(p))
            return NULL;
        const struct mo_module_scope *library = &p->interfaces->scopes[i];
        if (library->library && infer_compare_text(prefix, library->name) == 0)
            return library;
    }
    return NULL;
}

/* Credit the use line a prefix went through. */
MO_MUST_USE bool infer_use_alias(struct inference *p, uint32_t alias,
                                 const struct mo_syntax *name) {
    const struct mo_module_scope *scope = &p->interfaces->scopes[p->function->function->module];
    if (alias == UINT32_MAX ||
        mo_use_import(p->signatures, p->function->function->module, scope->aliases[alias].origin))
        return true;
    bool reported = infer_problem(p, MO_CODE_CHECK_PROGRAM_WORK, name->span,
                                  "the program needs too much import-checking work", NULL, NULL);
    (void)reported;
    return false;
}

MO_MUST_USE const struct mo_symbol *infer_definition(struct inference *p,
                                                     const struct mo_syntax *name,
                                                     const struct mo_syntax *prefix,
                                                     uint32_t *count) {
    const struct mo_module_scope *scope = &p->interfaces->scopes[p->function->function->module];
    if (name->kind == MO_S_QUALIFIED) {
        prefix = name->children[0];
        name = name->children[1];
    }
    if (prefix != NULL) {
        uint32_t alias;
        const struct mo_module_scope *target = infer_prefix_module(p, prefix, &alias);
        if (p->failed)
            return NULL;
        if (target != NULL && name->kind == MO_S_NAME && prefix->kind == MO_S_CAPNAME) {
            /* Red.f with both a used module Red and a case Red in scope could call either. Look
             * the case up without definition, which would count an import of Red as used. */
            uint32_t meanings = 0;
            const struct mo_symbol *other = infer_find_symbol(scope, prefix, &meanings);
            uint32_t low = 0, high = scope->imported_count;
            while (other == NULL && low < high) {
                uint32_t mid = low + (high - low) / 2;
                int order = infer_compare_text(prefix, scope->imported[mid].name);
                if (order == 0)
                    other = scope->imported[mid].symbols;
                else if (order > 0)
                    low = mid + 1;
                else
                    high = mid;
            }
            if (other != NULL && other->declaration->kind == MO_S_CASE_DECL) {
                bool reported =
                    infer_problem(p, MO_CODE_NAME_CLASH, prefix->span, "a clash between names",
                                  "A clash between names is an error; use the qualified "
                                  "form (reference 10.3).",
                                  NULL);
                (void)reported;
                return NULL;
            }
        }
        if (target == NULL) {
            uint32_t meanings = 0;
            const struct mo_symbol *type = infer_definition(p, prefix, NULL, &meanings);
            size_t length = prefix->span.end - prefix->span.start;
            bool parameter = length == 1 && prefix->text[0] >= 'A' && prefix->text[0] <= 'Z' &&
                             (p->function->type->parameters &
                              (UINT32_C(1) << (unsigned)(prefix->text[0] - 'A'))) != 0;
            if (type != NULL || mo_source_builtin(prefix->text, length) || parameter)
                return NULL; /* native dispatch follows in identifier */
            bool reported = infer_problem(
                p, MO_CODE_IMPORT_MISSING, prefix->span, "a module prefix isn't in scope",
                "Use names the modules available to this file (reference 10.3).", NULL);
            (void)reported;
            return NULL;
        }
        const struct mo_symbol *s = infer_find_symbol(target, name, count);
        if (s != NULL && s->owner != UINT32_MAX) {
            const struct mo_symbol *owner = &p->interfaces->scopes[s->module].symbols[s->owner];
            if (owner->exported && !infer_visible_data(p, owner))
                return NULL;
        }
        if (s != NULL && !s->exported && s->module != p->function->function->module) {
            bool reported = infer_problem(p, MO_CODE_IMPORT_PRIVATE, name->span,
                                          "everything not on expose is private to the module",
                                          "Everything not on expose is private to the module "
                                          "(reference 10.1).",
                                          s->declaration->children[0]);
            (void)reported;
            return NULL;
        }
        if (!infer_use_alias(p, alias, name))
            return NULL;
        return s;
    }
    const struct mo_symbol *s = infer_find_symbol(scope, name, count);
    if (s != NULL)
        return s;
    uint32_t lo = 0, hi = scope->imported_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (infer_compare_text(name, scope->imported[mid].name) > 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == scope->imported_count || infer_compare_text(name, scope->imported[lo].name) != 0)
        return NULL;
    if (!mo_use_import(p->signatures, p->function->function->module, scope->imported[lo].origin)) {
        bool reported =
            infer_problem(p, MO_CODE_CHECK_PROGRAM_WORK, name->span,
                          "the program needs too much import-checking work", NULL, NULL);
        (void)reported;
        return NULL;
    }
    *count = scope->imported[lo].count;
    return scope->imported[lo].symbols;
}

/* Whether the capitalized name before a dot is a case, a value as in Equal.to_text, rather than a
 * module or type qualifier as in Int.compare (reference 5.4). A module alias keeps the qualifier.
 */
MO_MUST_USE bool infer_case_receiver(struct inference *p, const struct mo_syntax *name) {
    if (name->kind != MO_S_CAPNAME)
        return false;
    const struct mo_module_scope *scope = &p->interfaces->scopes[p->function->function->module];
    if (find_alias(scope, name) != scope->alias_count)
        return false;
    /* Only a case without fields is a value on its own; Entry.to_text(e) and a
     * declared Money.to_text(m) name the type (D285). */
    const struct mo_builtin_case *builtin = mo_builtin_case_by_name(name);
    if (builtin != NULL)
        return builtin->count == 0;
    uint32_t count = 0;
    const struct mo_symbol *s = infer_definition(p, name, NULL, &count);
    if (s == NULL || s->declaration->kind != MO_S_CASE_DECL)
        return false;
    const struct mo_data_signature *data = infer_data_type(p, s);
    return data != NULL && data->count == 0;
}

/* Whether T.f, where T names both a type and its only value (Unit), is the
 * qualified form (D297): called with the receiver plus f's own arguments, or
 * compare without a call, which the dot form can't be. args is NULL for no call. */
MO_MUST_USE bool infer_type_reading(const struct mo_syntax *prefix, const struct mo_syntax *name,
                                    const struct mo_syntax *call) {
    uint32_t arity = 0;
    if (mo_builtin_type(prefix, &arity) == NULL)
        return false;
    bool compare = infer_compare_text(name, "compare") == 0;
    if (!compare && infer_compare_text(name, "to_text") != 0)
        return false;
    if (call == NULL)
        return compare;
    const struct mo_syntax *args = call->children[0];
    return (args == NULL ? 0u : args->count) == (compare ? 2u : 1u);
}

MO_MUST_USE bool infer_missing_field(struct inference *p, struct mo_span span, const uint8_t *name,
                                     size_t length, const struct mo_syntax *declaration) {
    char *message = infer_allocate(p, 256, 1, 1);
    if (message == NULL)
        return false;
    int n = name == NULL ? snprintf(message, 256, "the case is missing its unnamed value")
                         : snprintf(message, 256, "the record or case is missing field '%.*s%s'",
                                    (int)(length > 128 ? 128 : length), (const char *)name,
                                    length > 128 ? "..." : "");
    MO_ASSERT(n > 0 && n < 256);
    return infer_problem(p, MO_CODE_DATA_ARGUMENTS, span, message,
                         "There are no optional fields; a record or case is built with "
                         "every field (reference 4).",
                         declaration);
}

MO_MUST_USE const struct mo_native_signature *
infer_native_definition(struct inference *p, const char *module, const struct mo_syntax *name) {
    for (uint32_t i = 0; i < p->interfaces->count; i++) {
        if (!infer_work(p))
            return NULL;
        const struct mo_module_scope *scope = &p->interfaces->scopes[i];
        if (!scope->library || scope->name == NULL || strcmp(scope->name, module) != 0)
            continue;
        uint32_t count = 0;
        const struct mo_symbol *s = infer_find_symbol(scope, name, &count);
        if (s != NULL && s->declaration->kind == MO_S_FN &&
            (s->module == p->function->function->module || s->exported))
            return NULL;
    }
    for (uint32_t i = 0; i < p->signatures->native_count; i++) {
        const struct mo_native_signature *native = &p->signatures->native[i];
        if (strcmp(native->module, module) == 0 && infer_compare_text(name, native->name) == 0)
            return native;
    }
    return NULL;
}

MO_MUST_USE struct value infer_function_value(struct inference *p,
                                              const struct mo_source_type *type,
                                              const uint8_t requirements[26]) {
    struct term *parameters[26] = {NULL};
    struct value out =
        infer_graph(p, (struct value){.type = infer_annotation(p, type, parameters, 0)}, 0);
    if (!p->checked || out.type == NULL || type->parameters == 0)
        return out;
    uint32_t count = 0;
    for (unsigned i = 0; i < 26; i++)
        count += (type->parameters & (UINT32_C(1) << i)) != 0 ? 1u : 0u;
    out.graph->parameters =
        infer_allocate(p, count, sizeof(*out.graph->parameters), _Alignof(struct term *));
    if (p->failed)
        return (struct value){0};
    out.graph->function_type = type;
    out.graph->requirements = requirements;
    uint32_t at = 0;
    for (unsigned i = 0; i < 26; i++)
        if ((type->parameters & (UINT32_C(1) << i)) != 0)
            out.graph->parameters[at++] = parameters[i];
    return out;
}

MO_MUST_USE struct value infer_native_identifier(struct inference *p,
                                                 const struct mo_native_signature *signature) {
    if (signature->spec_only && !p->spec) {
        bool observes = strcmp(signature->name, "printed") == 0 ||
                        strcmp(signature->name, "printed_errors") == 0;
        bool reported =
            observes
                ? infer_problem(
                      p, MO_CODE_FAKE_OUTSIDE_SPEC, p->span,
                      "printed and printed_errors are read only in a test, example or promise",
                      "They can be used only where every console is a fake: in tests, "
                      "examples and promises (reference 12.4).",
                      NULL)
                : infer_problem(p, MO_CODE_FAKE_OUTSIDE_SPEC, p->span,
                                "a fake capability is made only in a test, example or promise",
                                "Fakes are made only in tests, examples and promises, never in a "
                                "module's constants or its functions (reference 12.4).",
                                NULL);
        (void)reported;
        return (struct value){0};
    }
    struct value out = infer_function_value(p, signature->type, signature->requirements);
    if (p->checked && out.type != NULL)
        out.graph->output.native = signature->qualified;
    return out;
}

MO_MUST_USE struct value infer_declared_function(struct inference *p, const struct mo_symbol *s) {
    uint32_t lo = 0, hi = p->signatures->count;
    uintptr_t wanted = (uintptr_t)s;
    while (lo < hi) {
        if (!infer_work(p))
            return (struct value){0};
        uint32_t mid = lo + (hi - lo) / 2;
        const struct mo_symbol *at = p->signatures->items[mid].function;
        if (at->module < s->module || (at->module == s->module && (uintptr_t)at < wanted))
            lo = mid + 1;
        else
            hi = mid;
    }
    MO_ASSERT(lo < p->signatures->count && p->signatures->items[lo].function == s);
    const struct mo_function_signature *signature = &p->signatures->items[lo];
    if (signature->type == NULL) {
        MO_ASSERT(p->signatures->recovering);
        p->failed = p->fatal = p->unavailable = true;
        return (struct value){0};
    }
    struct value out = infer_function_value(p, signature->type, signature->requirements);
    out.symbol = s;
    return out;
}

MO_MUST_USE const struct mo_symbol *infer_select_overload(struct inference *p,
                                                          const struct mo_symbol *group,
                                                          uint32_t count, struct term *receiver) {
    if (count == 1)
        return group;
    receiver = infer_root(receiver);
    if (receiver->variable)
        return NULL;
    const struct mo_module_scope *scope = &p->interfaces->scopes[group->module];
    for (uint32_t i = 0; i < count; i++) {
        if (!infer_work(p))
            return NULL;
        MO_ASSERT(group[i].receiver < scope->count);
        if (receiver->shape.declaration == &scope->symbols[group[i].receiver])
            return &group[i];
    }
    bool reported = infer_problem(
        p, MO_CODE_ARGUMENT_TYPE, p->span, "no overload takes this first argument's type",
        "A call picks the function whose first parameter matches its first "
        "argument (reference 5.1).",
        NULL);
    (void)reported;
    return NULL;
}

MO_MUST_USE struct value infer_deferred_function(struct inference *p, const struct mo_syntax *name,
                                                 const struct mo_symbol *group, uint32_t count) {
    struct dispatch *pending = infer_allocate(p, 1, sizeof(*pending), _Alignof(struct dispatch));
    struct value out = infer_graph(p, (struct value){.type = infer_fresh(p, 0)}, 0);
    if (pending == NULL || out.type == NULL)
        return (struct value){0};
    out.type->deferred = group == NULL;
    *pending = (struct dispatch){
        .name = name, .group = group, .count = count, .type = out.type, .graph = out.graph};
    if (p->last_dispatch == NULL)
        p->dispatches = pending;
    else
        p->last_dispatch->next = pending;
    p->last_dispatch = pending;
    out.dispatch = pending;
    return out;
}

MO_MUST_USE const struct mo_module_scope *infer_type_scope(struct inference *p, struct term *type) {
    if (type->shape.declaration != NULL)
        return &p->interfaces->scopes[type->shape.module];
    if (type->shape.kind != MO_SOURCE_NAMED)
        return NULL;
    for (uint32_t i = 0; i < p->interfaces->count; i++) {
        if (!infer_work(p))
            return NULL;
        const struct mo_module_scope *scope = &p->interfaces->scopes[i];
        if (scope->library && scope->name != NULL && strcmp(scope->name, type->shape.name) == 0)
            return scope;
    }
    return NULL;
}

/* T.to_text and T.compare (D285, reference 5.4) go through the ability, so a
 * customizer applies exposed or not (9.2). T is a type always in scope, a
 * module and any type it declares, a type parameter, or a declared type. */
static MO_MUST_USE struct value qualified_ability(struct inference *p,
                                                  const struct mo_syntax *prefix,
                                                  const struct mo_syntax *name) {
    struct term *type = NULL;
    uint32_t arity = 0, alias;
    const char *builtin = mo_builtin_type(prefix, &arity);
    const struct mo_module_scope *module =
        builtin != NULL ? NULL : infer_prefix_module(p, prefix, &alias);
    size_t length = prefix->span.end - prefix->span.start;
    uint32_t bit = length == 1 && prefix->text[0] >= 'A' && prefix->text[0] <= 'Z'
                       ? UINT32_C(1) << (unsigned)(prefix->text[0] - 'A')
                       : 0;
    if (p->failed)
        return (struct value){0};
    if (builtin != NULL) {
        type = infer_native(p, builtin, arity);
        for (uint32_t i = 0; type != NULL && i < arity; i++)
            type->arguments[i] = infer_fresh(p, 0);
    } else if (module != NULL) {
        if (!infer_use_alias(p, alias, name))
            return (struct value){0};
        type = infer_fresh(p, 0);
    } else if ((p->function->type->parameters & bit) != 0) {
        type = infer_annotation(p, infer_parameter_type(p->function->type, bit), NULL, 0);
    } else {
        uint32_t count = 0;
        const struct mo_symbol *s = infer_definition(p, prefix, NULL, &count);
        if (p->failed)
            return (struct value){0};
        const struct mo_nominal_signature *nominal =
            s == NULL || s->owner != UINT32_MAX ||
                    (s->declaration->kind != MO_S_STRUCT && s->declaration->kind != MO_S_ENUM)
                ? NULL
                : mo_nominal_signature_by_symbol(p->signatures, s);
        if (nominal == NULL) {
            /* Report an unknown prefix as any other qualified name does. */
            if (infer_definition(p, name, prefix, &count) == NULL && !p->failed) {
                bool reported = infer_unfinished(p);
                (void)reported;
            }
            return (struct value){0};
        }
        struct term *parameters[26] = {NULL};
        type = infer_annotation(p, nominal->type, parameters, 0);
    }
    if (p->failed || type == NULL)
        return (struct value){0};
    struct value out = infer_ability_function(p, (struct value){.type = type}, name);
    if (out.graph != NULL && module != NULL) {
        struct ability_module *owner =
            infer_allocate(p, 1, sizeof(*owner), _Alignof(struct ability_module));
        if (owner == NULL)
            return (struct value){0};
        *owner = (struct ability_module){out.graph, module, p->ability_modules};
        p->ability_modules = owner;
    }
    return out;
}

MO_MUST_USE struct value infer_identifier(struct inference *p, const struct mo_syntax *node,
                                          const struct mo_syntax *prefix) {
    if (node->kind == MO_S_QUALIFIED) {
        prefix = node->children[0];
        node = node->children[1];
    }
    if (prefix != NULL && infer_compare_text(prefix, "Simulation") == 0)
        return infer_simulation_call(p, node, NULL);
    if (prefix == NULL && node->kind == MO_S_NAME) {
        struct name_slot *s = infer_slot(p, node, false);
        if (s != NULL && s->active != NULL) {
            if (p->checked && s->active->function != p->lambda) {
                if (!capture_local(p, s->active, node->span))
                    return (struct value){0};
            }
            s->active->used = true;
            return (struct value){
                .type = s->active->type, .binding = s->active->name, .bound = s->active->binding};
        }
    }
    const struct mo_builtin_case *builtin = prefix == NULL ? mo_builtin_case_by_name(node) : NULL;
    if (builtin != NULL)
        return infer_builtin_construct(p, builtin, NULL, NULL);
    if (prefix != NULL &&
        (infer_compare_text(node, "to_text") == 0 || infer_compare_text(node, "compare") == 0))
        return qualified_ability(p, prefix, node);
    uint32_t count = 0;
    const struct mo_symbol *s = infer_definition(p, node, prefix, &count);
    if (p->failed)
        return (struct value){0};
    if (s == NULL) {
        if (prefix != NULL) {
            for (uint32_t i = 0; i < p->signatures->native_count; i++) {
                const struct mo_native_signature *native = &p->signatures->native[i];
                if (infer_compare_text(prefix, native->module) == 0 &&
                    infer_compare_text(node, native->name) == 0)
                    return infer_native_identifier(p, native);
            }
        }
        if (prefix != NULL) {
            bool reported = infer_problem(p, MO_CODE_NAME_UNKNOWN, node->span, infer_unknown_member,
                                          "T.f names a function in the module of T, or one T has "
                                          "through its abilities (reference 5.4, 10.3).",
                                          NULL);
            (void)reported;
        } else if (node->kind == MO_S_CAPNAME || node->kind == MO_S_QUALIFIED) {
            bool reported = infer_unfinished(p);
            (void)reported;
        } else {
            struct name_slot *entry = infer_slot(p, node, false);
            if (entry != NULL && entry->expired != NULL && p->error.code == MO_CODE_UNUSED_NAME &&
                p->error_unused == entry->expired) {
                /* An attempted read outside the binding's scope explains why
                 * it looked unused. Report that use, not a destructive rename. */
                p->error.code = 0;
            }
            const char *habit = entry == NULL ? infer_length_habit(node) : NULL;
            bool reported = infer_problem(
                p, MO_CODE_NAME_UNKNOWN, node->span, "a name isn't visible in this block",
                habit != NULL ? habit
                              : "A local name is visible from its binding to the end of "
                                "its block (reference 6.1).",
                NULL);
            (void)reported;
        }
        return (struct value){0};
    }
    if (s->declaration->kind == MO_S_STRUCT || s->declaration->kind == MO_S_CASE_DECL) {
        const struct mo_data_signature *data = infer_data_type(p, s);
        if (data == NULL) {
            MO_ASSERT(p->unavailable);
            return (struct value){0};
        }
        if (!infer_visible_data(p, data->type->declaration))
            return (struct value){0};
        if (data->count == 0) {
            struct term *parameters[26] = {NULL};
            struct value out = infer_graph(
                p,
                (struct value){.type = infer_annotation(p, data->type, parameters, 0), .symbol = s},
                0);
            if (p->checked && out.type != NULL) {
                out.graph->generic = data->type;
                out.graph->instantiated = out.type;
            }
            return out;
        }
        const struct mo_syntax *field = data->by_index[0]->name;
        bool reported =
            infer_missing_field(p, node->span, field == NULL ? NULL : field->text,
                                field == NULL ? 0 : field->span.end - field->span.start, field);
        (void)reported;
        return (struct value){0};
    }
    bool constant = s->declaration->kind == MO_S_CONST;
    if (s->declaration->kind != MO_S_FN && !constant) {
        bool reported = infer_unfinished(p);
        (void)reported;
        return (struct value){0};
    }
    if (prefix == NULL && node->kind == MO_S_NAME) {
        struct name_slot *entry = infer_slot(p, node, true);
        if (entry == NULL)
            return (struct value){0};
        if (entry->local_name != NULL) {
            bool reported =
                infer_problem(p, MO_CODE_BIND_REPEATED, node->span,
                              "a name means both a local and a module function",
                              "A function can't refer to its module's function by a name "
                              "it uses for a local (reference 6.1).",
                              entry->local_name);
            (void)reported;
            return (struct value){0};
        }
        entry->module_use = node;
    }
    if (constant) {
        if (p->constants == NULL) {
            bool reported = infer_unfinished(p);
            (void)reported;
            return (struct value){0};
        }
        struct mo_checked_constant *value = mo_constant_by_symbol(p->constants, s);
        if (value->type == NULL) {
            uint32_t index = (uint32_t)(value - p->constants->items);
            p->failed = p->fatal = p->unavailable = true;
            if (!p->constants->done[index]) {
                p->constants->pending = value;
                p->constants->pending_span = node->span;
            }
            return (struct value){0};
        }
        return infer_graph(
            p, (struct value){.type = infer_annotation(p, value->type, NULL, 0), .symbol = s}, 0);
    }
    if (count > 1)
        return infer_deferred_function(p, node, s, count);
    return infer_declared_function(p, s);
}

MO_MUST_USE struct value infer_as(struct inference *p, const struct mo_syntax *node,
                                  struct term *expected, uint16_t code) {
    uint16_t saved = p->expectation;
    p->expectation = code;
    struct value v = infer_expression(p, node, expected);
    p->expectation = saved;
    return v;
}

MO_MUST_USE struct value infer_discard(struct inference *p, const struct mo_syntax *node) {
    struct value v = infer_expression(p, node, NULL);
    if (v.type != NULL) {
        p->last->discarded = !v.never;
        v.type = infer_native(p, "Unit", 0);
    }
    return v;
}

MO_MUST_USE bool infer_lambda_argument(const struct mo_syntax *node) {
    while (node->kind == MO_S_GROUP)
        node = node->children[0];
    return node->kind == MO_S_LAMBDA;
}

/* Constraint order must never become evaluation order in output metadata. */
void infer_order_notes(struct inference *p, struct note *before, const struct note_range *ranges,
                       uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (before == NULL)
            p->notes = ranges[i].first;
        else
            before->next = ranges[i].first;
        before = ranges[i].last;
    }
    if (before != NULL)
        before->next = NULL;
    p->last = before;
}
