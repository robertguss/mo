#include "check/abilities.h"
#include "check/check.h"
#include "check/checked.h"
#include "check/data.h"
#include "check/infer.h"
#include "check/internal.h"
#include "check/native.h"
#include "check/patterns.h"
#include "check/signatures.h"

#include "base/codes.h"
#include "base/limits.h"
#include "base/simops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The reason for an unknown name, called or dot-called, that another language uses for a length
 * (reference appendix C), or NULL. */
static const char *length_habit(const struct mo_syntax *name) {
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

static MO_MUST_USE struct name_slot *slot(struct inference *p, const struct mo_syntax *name,
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

static MO_MUST_USE bool bind(struct inference *p, const struct mo_syntax *name, struct term *type,
                             bool variable) {
    if (name->kind == MO_S_UNDERSCORE)
        return record_binding(p, name, type, variable);
    struct name_slot *s = slot(p, name, true);
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

static MO_MUST_USE bool leave(struct inference *p, struct local *mark) {
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

static MO_MUST_USE bool capture_write(struct inference *p, struct local *local,
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

static MO_MUST_USE bool capture_path(struct inference *p, const struct mo_syntax *group,
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

static MO_MUST_USE const struct mo_symbol *
find_symbol(const struct mo_module_scope *scope, const struct mo_syntax *name, uint32_t *count) {
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

static MO_MUST_USE const struct mo_field_signature *find_field(const struct mo_data_signature *data,
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
static MO_MUST_USE const struct mo_module_scope *
prefix_module(struct inference *p, const struct mo_syntax *prefix, uint32_t *alias) {
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
static MO_MUST_USE bool use_alias(struct inference *p, uint32_t alias,
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

static MO_MUST_USE const struct mo_symbol *definition(struct inference *p,
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
        const struct mo_module_scope *target = prefix_module(p, prefix, &alias);
        if (p->failed)
            return NULL;
        if (target != NULL && name->kind == MO_S_NAME && prefix->kind == MO_S_CAPNAME) {
            /* Red.f with both a used module Red and a case Red in scope could call either. Look
             * the case up without definition, which would count an import of Red as used. */
            uint32_t meanings = 0;
            const struct mo_symbol *other = find_symbol(scope, prefix, &meanings);
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
            const struct mo_symbol *type = definition(p, prefix, NULL, &meanings);
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
        const struct mo_symbol *s = find_symbol(target, name, count);
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
        if (!use_alias(p, alias, name))
            return NULL;
        return s;
    }
    const struct mo_symbol *s = find_symbol(scope, name, count);
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

static MO_MUST_USE struct value builtin_construct(struct inference *p,
                                                  const struct mo_builtin_case *data,
                                                  const struct mo_syntax *args,
                                                  struct term *expected);

/* Whether the capitalized name before a dot is a case, a value as in Equal.to_text, rather than a
 * module or type qualifier as in Int.compare (reference 5.4). A module alias keeps the qualifier.
 */
static MO_MUST_USE bool case_receiver(struct inference *p, const struct mo_syntax *name) {
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
    const struct mo_symbol *s = definition(p, name, NULL, &count);
    if (s == NULL || s->declaration->kind != MO_S_CASE_DECL)
        return false;
    const struct mo_data_signature *data = infer_data_type(p, s);
    return data != NULL && data->count == 0;
}

/* Whether T.f, where T names both a type and its only value (Unit), is the
 * qualified form (D297): called with the receiver plus f's own arguments, or
 * compare without a call, which the dot form can't be. args is NULL for no call. */
static MO_MUST_USE bool type_reading(const struct mo_syntax *prefix, const struct mo_syntax *name,
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

static MO_MUST_USE bool missing_field(struct inference *p, struct mo_span span, const uint8_t *name,
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

static MO_MUST_USE const struct mo_native_signature *
native_definition(struct inference *p, const char *module, const struct mo_syntax *name) {
    for (uint32_t i = 0; i < p->interfaces->count; i++) {
        if (!infer_work(p))
            return NULL;
        const struct mo_module_scope *scope = &p->interfaces->scopes[i];
        if (!scope->library || scope->name == NULL || strcmp(scope->name, module) != 0)
            continue;
        uint32_t count = 0;
        const struct mo_symbol *s = find_symbol(scope, name, &count);
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

static MO_MUST_USE struct value function_value(struct inference *p,
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

static MO_MUST_USE struct value native_identifier(struct inference *p,
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
    struct value out = function_value(p, signature->type, signature->requirements);
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
    struct value out = function_value(p, signature->type, signature->requirements);
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

static MO_MUST_USE struct value deferred_function(struct inference *p, const struct mo_syntax *name,
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

static MO_MUST_USE const struct mo_module_scope *type_scope(struct inference *p,
                                                            struct term *type) {
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

static MO_MUST_USE struct value ability_function(struct inference *p, struct value receiver,
                                                 const struct mo_syntax *name);

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
        builtin != NULL ? NULL : prefix_module(p, prefix, &alias);
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
        if (!use_alias(p, alias, name))
            return (struct value){0};
        type = infer_fresh(p, 0);
    } else if ((p->function->type->parameters & bit) != 0) {
        type = infer_annotation(p, infer_parameter_type(p->function->type, bit), NULL, 0);
    } else {
        uint32_t count = 0;
        const struct mo_symbol *s = definition(p, prefix, NULL, &count);
        if (p->failed)
            return (struct value){0};
        const struct mo_nominal_signature *nominal =
            s == NULL || s->owner != UINT32_MAX ||
                    (s->declaration->kind != MO_S_STRUCT && s->declaration->kind != MO_S_ENUM)
                ? NULL
                : mo_nominal_signature_by_symbol(p->signatures, s);
        if (nominal == NULL) {
            /* Report an unknown prefix as any other qualified name does. */
            if (definition(p, name, prefix, &count) == NULL && !p->failed) {
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
    struct value out = ability_function(p, (struct value){.type = type}, name);
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

static MO_MUST_USE struct value simulation_call(struct inference *p, const struct mo_syntax *name,
                                                const struct mo_syntax *args);

static MO_MUST_USE struct value identifier(struct inference *p, const struct mo_syntax *node,
                                           const struct mo_syntax *prefix) {
    if (node->kind == MO_S_QUALIFIED) {
        prefix = node->children[0];
        node = node->children[1];
    }
    if (prefix != NULL && infer_compare_text(prefix, "Simulation") == 0)
        return simulation_call(p, node, NULL);
    if (prefix == NULL && node->kind == MO_S_NAME) {
        struct name_slot *s = slot(p, node, false);
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
        return builtin_construct(p, builtin, NULL, NULL);
    if (prefix != NULL &&
        (infer_compare_text(node, "to_text") == 0 || infer_compare_text(node, "compare") == 0))
        return qualified_ability(p, prefix, node);
    uint32_t count = 0;
    const struct mo_symbol *s = definition(p, node, prefix, &count);
    if (p->failed)
        return (struct value){0};
    if (s == NULL) {
        if (prefix != NULL) {
            for (uint32_t i = 0; i < p->signatures->native_count; i++) {
                const struct mo_native_signature *native = &p->signatures->native[i];
                if (infer_compare_text(prefix, native->module) == 0 &&
                    infer_compare_text(node, native->name) == 0)
                    return native_identifier(p, native);
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
            struct name_slot *entry = slot(p, node, false);
            if (entry != NULL && entry->expired != NULL && p->error.code == MO_CODE_UNUSED_NAME &&
                p->error_unused == entry->expired) {
                /* An attempted read outside the binding's scope explains why
                 * it looked unused. Report that use, not a destructive rename. */
                p->error.code = 0;
            }
            const char *habit = entry == NULL ? length_habit(node) : NULL;
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
            missing_field(p, node->span, field == NULL ? NULL : field->text,
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
        struct name_slot *entry = slot(p, node, true);
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
        return deferred_function(p, node, s, count);
    return infer_declared_function(p, s);
}

static MO_MUST_USE struct value infer_expression(struct inference *p, const struct mo_syntax *node,
                                                 struct term *expected);

static MO_MUST_USE struct value infer_as(struct inference *p, const struct mo_syntax *node,
                                         struct term *expected, uint16_t code) {
    uint16_t saved = p->expectation;
    p->expectation = code;
    struct value v = infer_expression(p, node, expected);
    p->expectation = saved;
    return v;
}

static MO_MUST_USE struct value discard(struct inference *p, const struct mo_syntax *node) {
    struct value v = infer_expression(p, node, NULL);
    if (v.type != NULL) {
        p->last->discarded = !v.never;
        v.type = infer_native(p, "Unit", 0);
    }
    return v;
}

static MO_MUST_USE bool lambda_argument(const struct mo_syntax *node) {
    while (node->kind == MO_S_GROUP)
        node = node->children[0];
    return node->kind == MO_S_LAMBDA;
}

/* Constraint order must never become evaluation order in output metadata. */
static void order_notes(struct inference *p, struct note *before, const struct note_range *ranges,
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

static MO_MUST_USE struct term *
builtin_type(struct inference *p, const struct mo_builtin_case *data, struct term *expected) {
    struct term *type = infer_native(p, data->type, data->parameters);
    if (type == NULL)
        return NULL;
    for (uint32_t i = 0; i < data->parameters; i++) {
        type->arguments[i] = infer_fresh(p, 0);
        if (type->arguments[i] == NULL)
            return NULL;
    }
    if (expected != NULL && !infer_unify(p, type, expected, 0))
        return NULL;
    return type;
}

MO_MUST_USE uint32_t infer_builtin_field(const struct mo_builtin_case *data,
                                         const struct mo_syntax *arg) {
    if (data->fields[0] == NULL)
        return arg->kind == MO_S_NAMED_ARG ? UINT32_MAX : 0;
    if (arg->kind == MO_S_NAMED_ARG)
        for (uint32_t i = 0; i < data->count; i++)
            if (infer_compare_text(arg->children[0], data->fields[i]) == 0)
                return i;
    return UINT32_MAX;
}

MO_MUST_USE struct term *infer_builtin_part(struct inference *p, const struct mo_builtin_case *data,
                                            struct term *type, uint32_t index) {
    MO_ASSERT(index < data->count);
    if (data->fields[0] == NULL)
        return infer_argument(p, type, data->parameter);
    return infer_native(p, strcmp(data->name, "Entry") == 0 && index == 1 ? "EntryKind" : "Text",
                        0);
}

static MO_MUST_USE struct value builtin_construct(struct inference *p,
                                                  const struct mo_builtin_case *data,
                                                  const struct mo_syntax *args,
                                                  struct term *expected) {
    struct term *type = builtin_type(p, data, expected);
    if (type == NULL)
        return (struct value){0};
    if ((args == NULL ? 0 : args->count) < data->count) {
        bool provided[2] = {false, false};
        for (uint32_t i = 0; args != NULL && i < args->count; i++) {
            uint32_t index = infer_builtin_field(data, args->children[i]);
            if (index < data->count)
                provided[index] = true;
        }
        for (uint32_t i = 0; i < data->count; i++) {
            if (provided[i])
                continue;
            const char *name = data->fields[i];
            bool reported =
                missing_field(p, args == NULL ? p->span : args->span, (const uint8_t *)name,
                              name == NULL ? 0 : strlen(name), NULL);
            (void)reported;
            return (struct value){0};
        }
        MO_ASSERT(false);
    }
    if ((args == NULL ? 0 : args->count) != data->count) {
        bool reported =
            infer_problem(p, MO_CODE_DATA_ARGUMENTS, args == NULL ? p->span : args->span,
                          "the case hasn't been given every field",
                          "A case is built with every named field or its one unnamed "
                          "value (reference 4).",
                          NULL);
        (void)reported;
        return (struct value){0};
    }
    bool seen[2] = {false, false};
    struct value out =
        infer_graph(p, (struct value){.type = type, .builtin_case = data->name}, data->count);
    if (out.type == NULL)
        return out;
    if (p->checked) {
        out.graph->generic = mo_builtin_template(data);
        out.graph->instantiated = type;
    }
    for (uint32_t i = 0; args != NULL && i < args->count; i++) {
        const struct mo_syntax *arg = args->children[i];
        uint32_t index = infer_builtin_field(data, arg);
        if (index == UINT32_MAX || seen[index]) {
            bool reported = infer_problem(p, MO_CODE_DATA_ARGUMENTS, arg->span,
                                          "a supplied field doesn't match the case",
                                          "A case is built with every named field once or its "
                                          "one unnamed value (reference 4).",
                                          NULL);
            (void)reported;
            return (struct value){0};
        }
        seen[index] = true;
        struct term *field = infer_builtin_part(p, data, type, index);
        if (field == NULL)
            return (struct value){0};
        struct value v =
            infer_expression(p, arg->kind == MO_S_NAMED_ARG ? arg->children[1] : arg, field);
        if (v.type == NULL)
            return (struct value){0};
        if (p->checked) {
            if (arg->kind == MO_S_NAMED_ARG) {
                struct value supplied =
                    infer_graph(p, (struct value){.type = v.type, .never = v.never}, 1);
                if (supplied.type == NULL)
                    return supplied;
                supplied.graph->output.field = index;
                supplied.graph->operands[0] = v.note;
                if (!infer_record(p, arg, supplied))
                    return (struct value){0};
                v.note = p->last;
            }
            out.graph->operands[i] = v.note;
        }
        out.never = out.never || v.never;
    }
    return out;
}

static MO_MUST_USE struct value construct(struct inference *p, const struct mo_data_signature *data,
                                          const struct mo_syntax *args, struct term *expected) {
    if (!infer_visible_data(p, data->type->declaration))
        return (struct value){0};
    struct term *parameters[26] = {NULL};
    struct term *type = infer_annotation(p, data->type, parameters, 0);
    if (type == NULL || (expected != NULL && !infer_unify(p, type, expected, 0)))
        return (struct value){0};
    struct value out =
        infer_graph(p, (struct value){.type = type, .symbol = data->symbol}, args->count);
    if (out.type == NULL)
        return out;
    if (p->checked) {
        out.graph->generic = data->type;
        out.graph->instantiated = type;
    }
    bool *seen = infer_allocate(p, data->count, sizeof(*seen), _Alignof(bool));
    const struct mo_field_signature **selected = infer_allocate(
        p, args->count, sizeof(*selected), _Alignof(const struct mo_field_signature *));
    struct note_range *ranges =
        infer_allocate(p, args->count, sizeof(*ranges), _Alignof(struct note_range));
    if (p->failed)
        return (struct value){0};
    memset(seen, 0, data->count * sizeof(*seen));
    for (uint32_t i = 0; i < args->count; i++) {
        const struct mo_syntax *arg = args->children[i];
        const struct mo_field_signature *field =
            data->unnamed && arg->kind != MO_S_NAMED_ARG ? &data->fields[0]
            : arg->kind == MO_S_NAMED_ARG                ? find_field(data, arg->children[0])
                                                         : NULL;
        if (field == NULL || seen[field->index]) {
            bool reported =
                infer_problem(p, MO_CODE_DATA_ARGUMENTS, arg->span,
                              "a supplied field doesn't match the record or case",
                              "A record or case is built by naming every field once, or "
                              "from its one unnamed value (reference 4).",
                              field == NULL ? data->symbol->declaration->children[0] : field->name);
            (void)reported;
            return (struct value){0};
        }
        seen[field->index] = true;
        selected[i] = field;
    }
    for (uint32_t i = 0; i < data->count; i++) {
        if (seen[i])
            continue;
        const struct mo_syntax *name = data->by_index[i]->name;
        bool reported = missing_field(p, args->span, name == NULL ? NULL : name->text,
                                      name == NULL ? 0 : name->span.end - name->span.start, name);
        (void)reported;
        return (struct value){0};
    }
    bool never = false;
    struct note *before = p->last;
    for (unsigned pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < args->count; i++) {
            const struct mo_syntax *arg = args->children[i];
            const struct mo_syntax *value = data->unnamed ? arg : arg->children[1];
            if (lambda_argument(value) != (pass != 0))
                continue;
            struct term *wanted = infer_annotation(p, selected[i]->type, parameters, 0);
            if (wanted == NULL)
                return (struct value){0};
            struct note *previous = p->last;
            struct value v = infer_as(p, value, wanted, MO_CODE_ARGUMENT_TYPE);
            if (v.type == NULL)
                return (struct value){0};
            p->last->output.field = selected[i];
            if (p->checked) {
                if (!data->unnamed) {
                    struct value supplied =
                        infer_graph(p, (struct value){.type = v.type, .never = v.never}, 1);
                    if (supplied.type == NULL)
                        return supplied;
                    supplied.graph->output.field = selected[i]->index;
                    supplied.graph->operands[0] = v.note;
                    if (!infer_record(p, arg, supplied))
                        return (struct value){0};
                    v.note = p->last;
                }
                out.graph->operands[i] = v.note;
            }
            ranges[i] = (struct note_range){previous == NULL ? p->notes : previous->next, p->last};
            never = never || v.never;
        }
    }
    order_notes(p, before, ranges, args->count);
    out.never = never;
    return out;
}

static MO_MUST_USE struct value call(struct inference *p, struct value callee,
                                     const struct mo_syntax *args, const struct value *receiver);

static MO_MUST_USE struct value ability_function(struct inference *p, struct value receiver,
                                                 const struct mo_syntax *name) {
    bool text = infer_compare_text(name, "to_text") == 0;
    struct term *type = infer_make_type(p, (struct mo_source_type){.kind = MO_SOURCE_FUNCTION,
                                                                   .name = "fn",
                                                                   .module = UINT32_MAX,
                                                                   .count = text ? 2u : 3u});
    if (type == NULL)
        return (struct value){0};
    type->arguments[0] = receiver.type;
    if (!text)
        type->arguments[1] = receiver.type;
    type->arguments[type->shape.count - 1] = infer_native(p, text ? "Text" : "Ordering", 0);
    if (p->failed)
        return (struct value){0};
    struct value out = infer_graph(p, (struct value){.type = type}, 0);
    if (p->checked && out.type != NULL)
        out.graph->output.native = text ? "to_text" : "compare";
    return out;
}

/* Resolve a member without walking its explicit arguments. A method returns
 * its function type; a visible field returns its projection. */
MO_MUST_USE struct value infer_member(struct inference *p, struct value v,
                                      const struct mo_syntax *name, bool *called) {
    *called = true;
    if (v.type == NULL)
        return v;
    p->span = name->span;
    struct term *type = infer_root(v.type);
    MO_ASSERT(!type->variable);
    if (!type->variable && type->shape.kind == MO_SOURCE_NAMED &&
        type->shape.module == UINT32_MAX) {
        if (infer_named(type, "Entry") &&
            (infer_compare_text(name, "name") == 0 || infer_compare_text(name, "kind") == 0)) {
            *called = false;
            uint32_t index = infer_compare_text(name, "name") == 0 ? 0u : 1u;
            const struct mo_builtin_case *data = mo_builtin_case_by_type("Entry");
            struct value out = infer_graph(
                p,
                (struct value){.type = infer_builtin_part(p, data, type, index), .never = v.never},
                1);
            if (p->checked && out.type != NULL) {
                out.graph->output.field = index;
                out.graph->output.native = index == 0 ? "Entry.name" : "Entry.kind";
                out.graph->operands[0] = v.note;
            }
            return out;
        }
        const struct mo_native_signature *signature = native_definition(p, type->shape.name, name);
        if (signature != NULL)
            return native_identifier(p, signature);
    }
    const struct mo_data_signature *data = type->variable || type->shape.declaration == NULL
                                               ? NULL
                                               : infer_data_type(p, type->shape.declaration);
    const struct mo_field_signature *field = data == NULL ? NULL : find_field(data, name);
    bool visible = data != NULL && (type->shape.module == p->function->function->module ||
                                    type->shape.declaration->insides);
    if ((field == NULL || !visible) &&
        (infer_compare_text(name, "to_text") == 0 || infer_compare_text(name, "compare") == 0))
        return ability_function(p, v, name);
    if (field == NULL || !visible) {
        uint32_t count = 0;
        const struct mo_module_scope *owner = type_scope(p, type);
        if (p->failed)
            return (struct value){0};
        const struct mo_symbol *method = owner == NULL ? NULL : find_symbol(owner, name, &count);
        bool plausible = method != NULL && method->declaration->kind == MO_S_FN &&
                         (method->module == p->function->function->module || method->exported);
        if (!plausible) {
            method = definition(p, name, NULL, &count);
            plausible = method != NULL && method->declaration->kind == MO_S_FN;
        }
        if (plausible) {
            method = infer_select_overload(p, method, count, type);
            if (method == NULL)
                return (struct value){0};
            return infer_declared_function(p, method);
        }
    }
    if (type->shape.declaration == NULL) {
        const char *habit = length_habit(name);
        if (infer_named(type, "Simulation"))
            habit = "Simulation operations are called through the module, as in "
                    "Simulation.crash(sim), not as dot calls (reference 12.4).";
        bool reported = infer_problem(p, MO_CODE_NAME_UNKNOWN, name->span,
                                      "no visible function has this dot-call name",
                                      habit != NULL ? habit
                                                    : "A dot call looks in the type's module, then "
                                                      "among the names in scope (reference 5.4).",
                                      NULL);
        (void)reported;
        return (struct value){0};
    }
    if (!infer_visible_data(p, type->shape.declaration))
        return (struct value){0};
    if (field == NULL) {
        bool reported =
            infer_problem(p, MO_CODE_FIELD_MISSING, name->span, "the record has no such field",
                          "A struct declares named fields, each with a type (reference 4.1).",
                          type->shape.declaration->declaration->children[0]);
        (void)reported;
        return (struct value){0};
    }
    *called = false;
    struct term *parameters[26] = {NULL};
    for (uint32_t i = 0; i < type->shape.count; i++) {
        MO_ASSERT(data->parameters != NULL && i < data->parameters->count);
        parameters[data->parameters->children[i]->text[0] - 'A'] = infer_argument(p, type, i);
        if (p->failed)
            return (struct value){0};
    }
    struct value out =
        infer_graph(p,
                    (struct value){.type = infer_annotation(p, field->type, parameters, 0),
                                   .field = field,
                                   .never = v.never},
                    1);
    if (p->checked && out.type != NULL) {
        out.graph->output.field = field->index;
        out.graph->operands[0] = v.note;
        out.graph->generic = data->type;
        out.graph->instantiated = type;
    }
    return out;
}

bool infer_receiver_only(struct term *type) {
    type = infer_root(type);
    return !type->variable && type->shape.kind == MO_SOURCE_FUNCTION && type->shape.count == 2;
}

/* `x.f()` where `f` takes only the receiver: reference 5.4 spells it `x.f`. A
 * field holding a function is called with `()` and never reaches here. */
MO_MUST_USE bool infer_empty_dot_call(struct inference *p, const struct mo_syntax *site) {
    struct mo_span span = site->span;
    bool reported =
        infer_problem(p, MO_CODE_DOT_CALL_PARENTHESES, span, "write x.f, not x.f()",
                      "x.f, with no other arguments, calls f(x) (reference 5.4).", NULL);
    (void)reported;
    if (p->error.code == MO_CODE_DOT_CALL_PARENTHESES &&
        p->error.primary.span.start == span.start) {
        p->edit = (struct mo_edit){span, ""};
        p->fix = (struct mo_fix){"remove the parentheses", &p->edit, 1};
        p->error.fixes = &p->fix;
        p->error.fix_count = 1;
    }
    return false;
}

static MO_MUST_USE struct value field_value(struct inference *p, struct value receiver,
                                            const struct mo_syntax *name,
                                            const struct mo_syntax *syntax,
                                            const struct mo_syntax *site, bool *called) {
    if (receiver.type == NULL)
        return receiver;
    const struct mo_syntax *args = site == NULL ? NULL : site->children[0];
    if (infer_root(receiver.type)->variable) {
        *called = true;
        uint32_t count = args == NULL ? 0u : args->count;
        struct value out = deferred_function(p, name, NULL, count);
        if (out.type == NULL)
            return out;
        struct dispatch *pending = out.dispatch;
        pending->syntax = syntax;
        pending->call = site;
        pending->arguments = args;
        pending->receiver = receiver;
        pending->inputs =
            infer_allocate(p, count, sizeof(*pending->inputs), _Alignof(struct value));
        if (pending->inputs == NULL)
            return (struct value){0};
        out.never = receiver.never;
        for (uint32_t i = 0; i < count; i++) {
            if (args->children[i]->kind == MO_S_NAMED_ARG) {
                bool reported =
                    infer_problem(p, MO_CODE_CALL_ARGUMENTS, args->children[i]->span,
                                  "a function is called with named fields",
                                  "Arguments are passed by position (reference 5.2).", NULL);
                (void)reported;
                return (struct value){0};
            }
            pending->inputs[i] = infer_expression(p, args->children[i], NULL);
            if (pending->inputs[i].type == NULL)
                return (struct value){0};
            out.never = out.never || pending->inputs[i].never;
        }
        return out;
    }
    struct value resolved = infer_member(p, receiver, name, called);
    if (resolved.type == NULL || !*called)
        return resolved;
    if (args != NULL && args->count == 0 && infer_receiver_only(resolved.type) &&
        !infer_empty_dot_call(p, site))
        return (struct value){0};
    if (p->checked && resolved.symbol != NULL) {
        if (!infer_record(p, name, resolved))
            return (struct value){0};
        resolved.note = p->last;
    }
    return call(p, resolved, args, &receiver);
}

static MO_MUST_USE struct value simulation_call(struct inference *p, const struct mo_syntax *name,
                                                const struct mo_syntax *args) {
    static const struct {
        const char *name, *qualified;
        enum mo_sim_operation operation;
    } operations[] = {
#define HOSTOP(id, module, member, signature, params, bugs, spec_only)
#define SIMOP(id, module, member, spec_only) {member, module "." member, MO_SIM_##id},
#include "base/hostops.def"
    };
    uint32_t op = 0;
    while (op < MO_SIM_COUNT && infer_compare_text(name, operations[op].name) != 0)
        op++;
    if (op == MO_SIM_COUNT) {
        bool kept = infer_problem(p, MO_CODE_NAME_UNKNOWN, name->span, infer_unknown_member,
                                  "Simulation's operations are start, send, advance, crash and "
                                  "restart (reference 12.4).",
                                  NULL);
        (void)kept;
        return (struct value){0};
    }
    if (args == NULL) {
        bool kept = infer_problem(p, MO_CODE_CALL_ARGUMENTS, name->span,
                                  "a simulation operation needs a direct call",
                                  "Simulation operations are not values; call one directly, as in "
                                  "Simulation.crash(sim) (reference 12.4).",
                                  NULL);
        (void)kept;
        if (p->error.code == MO_CODE_CALL_ARGUMENTS &&
            p->error.primary.span.start == name->span.start)
            p->error.primary.text = "call this operation directly";
        return (struct value){0};
    }
    enum mo_sim_operation operation = operations[op].operation;
    if (operation == MO_SIM_START && !p->spec) {
        bool kept = infer_problem(p, MO_CODE_FAKE_OUTSIDE_SPEC, name->span,
                                  "a fake capability is made only in a test, example or promise",
                                  "Fakes are made only in tests, examples and promises, never in a "
                                  "module's constants or its functions (reference 12.4).",
                                  NULL);
        (void)kept;
        return (struct value){0};
    }
    uint32_t selector = operation == MO_SIM_START ? 0 : operation == MO_SIM_SEND ? 1 : UINT32_MAX;
    uint32_t count = operation == MO_SIM_START || operation == MO_SIM_ADVANCE ? 2 : 1;
    uint32_t service_index = 0, tool_index = 0;
    const struct mo_source_type *signature = NULL;
    if ((operation == MO_SIM_SEND && args->count < 2) ||
        (operation != MO_SIM_SEND && args->count != count))
        goto bad_arguments;
    if (selector != UINT32_MAX) {
        const struct mo_syntax *prefix = args->count > selector ? args->children[selector] : NULL;
        const struct mo_syntax *tool = NULL;
        if (operation == MO_SIM_SEND) {
            if (prefix != NULL && prefix->count == 2 &&
                (prefix->kind == MO_S_FIELD_ACCESS || prefix->kind == MO_S_QUALIFIED)) {
                tool = prefix->children[1];
                prefix = prefix->children[0];
            } else if (prefix != NULL && prefix->kind == MO_S_POSTFIX && prefix->count == 2 &&
                       (prefix->children[1]->kind == MO_S_FIELD_ACCESS ||
                        prefix->children[1]->kind == MO_S_QUALIFIED)) {
                tool = prefix->children[1]->children[0];
                prefix = prefix->children[0];
            } else
                prefix = NULL;
        }
        const struct mo_module_scope *scope = NULL;
        uint32_t alias = UINT32_MAX;
        const struct mo_program *program = p->signatures->service_program;
        if (prefix != NULL && prefix->kind == MO_S_CAPNAME) {
            const struct mo_module_scope *own =
                &p->interfaces->scopes[p->function->function->module];
            const struct mo_syntax *path =
                program == NULL ? NULL
                                : program->set->modules[p->function->function->module]->header.path;
            bool current = own->name != NULL
                               ? infer_compare_text(prefix, own->name) == 0
                               : path != NULL && path->count == 1 &&
                                     infer_compare_name(prefix, path->children[0]) == 0;
            scope = current ? own : prefix_module(p, prefix, &alias);
        }
        const struct mo_checked_service *service = NULL;
        if (scope != NULL && program != NULL) {
            for (uint32_t i = 0; i < program->service_count; i++) {
                if (!infer_work(p))
                    return (struct value){0};
                if (&p->interfaces->scopes[program->services[i].module] == scope) {
                    service = &program->services[i];
                    service_index = i;
                    break;
                }
            }
        }
        if (service != NULL && operation == MO_SIM_SEND) {
            for (tool_index = 0; tool_index < service->tool_count; tool_index++) {
                if (!infer_work(p))
                    return (struct value){0};
                if (infer_compare_name(tool, service->tools[tool_index].name) == 0) {
                    signature =
                        program->functions[service->tools[tool_index].function].signature->type;
                    count = signature->count - 1;
                    break;
                }
            }
        }
        if (service == NULL || (operation == MO_SIM_SEND && signature == NULL)) {
            bool start = operation == MO_SIM_START;
            bool kept = infer_problem(
                p, MO_CODE_SERVICE_HANDLER, prefix == NULL ? args->span : prefix->span,
                start ? "Simulation.start must name a service module"
                      : "Simulation.send must name a registered tool",
                start ? "Name the current module or one it uses directly, as in "
                        "Simulation.start(TaskQueue, at); that module must declare a "
                        "service (reference 12.4)."
                      : "Write the service module and one of its tools directly, as in "
                        "TaskQueue.submit; timers and other functions are not tools "
                        "(reference 12.4).",
                NULL);
            (void)kept;
            return (struct value){0};
        }
        if (!use_alias(p, alias, prefix))
            return (struct value){0};
    }
    if (args->count != count)
        goto bad_arguments;
    struct term *result = infer_native(p,
                                       operation == MO_SIM_START  ? "Simulation"
                                       : operation == MO_SIM_SEND ? "Result"
                                                                  : "Unit",
                                       operation == MO_SIM_SEND ? 2 : 0);
    if (result == NULL)
        return (struct value){0};
    if (operation == MO_SIM_SEND) {
        result->arguments[0] =
            infer_annotation(p, signature->arguments[signature->count - 1], NULL, 0);
        result->arguments[1] = infer_native(p, "SimulationError", 0);
        if (result->arguments[0] == NULL || result->arguments[1] == NULL)
            return (struct value){0};
    }
    struct value out =
        infer_graph(p, (struct value){.type = result}, count - (selector != UINT32_MAX));
    if (out.type == NULL)
        return out;
    if (p->checked) {
        out.graph->output.native = operations[op].qualified;
        out.graph->output.service = service_index;
        out.graph->output.tool = tool_index;
    }
    uint32_t emitted = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (i == selector)
            continue;
        struct term *wanted = operation == MO_SIM_START ? infer_native(p, "Time", 0)
                              : i == 0                  ? infer_native(p, "Simulation", 0)
                              : operation == MO_SIM_ADVANCE
                                  ? infer_native(p, "Duration", 0)
                                  : infer_annotation(p, signature->arguments[i], NULL, 0);
        if (wanted == NULL)
            return (struct value){0};
        struct value input = infer_as(p, args->children[i], wanted, MO_CODE_ARGUMENT_TYPE);
        if (input.type == NULL)
            return (struct value){0};
        if (p->checked)
            out.graph->operands[emitted] = input.note;
        emitted++;
        out.never = out.never || input.never;
    }
    return out;

bad_arguments : {
    const char *reasons[MO_SIM_COUNT] = {
        [MO_SIM_START] = "Simulation.start takes a service module and a start Time.",
        [MO_SIM_SEND] = ("Simulation.send takes the handle, Module.tool, then the tool's arguments "
                         "after State and Time, which send supplies."),
        [MO_SIM_ADVANCE] = "Simulation.advance takes the handle and a Duration.",
        [MO_SIM_CRASH] = "Simulation.crash takes only the handle.",
        [MO_SIM_RESTART] = "Simulation.restart takes only the handle."};
    bool kept = infer_problem(p, MO_CODE_CALL_ARGUMENTS, args->span,
                              "a simulation call's arguments don't match its parameters",
                              reasons[operation], NULL);
    (void)kept;
    return (struct value){0};
}
}

static MO_MUST_USE struct value sequence(struct inference *p, const struct mo_syntax *node,
                                         struct term *expected) {
    struct value last = {.type = infer_native(p, "Unit", 0)};
    struct value emitted = infer_graph(p, last, node->count);
    if (emitted.type == NULL)
        return (struct value){0};
    bool never = false;
    for (uint32_t i = 0; i < node->count; i++) {
        last = infer_as(p, node->children[i], i + 1 == node->count && !never ? expected : NULL,
                        p->node_expectation);
        if (last.type == NULL)
            return (struct value){0};
        if (p->checked)
            emitted.graph->operands[i] = last.note;
        never = never || last.never;
        /* The last note belongs to this statement, not its nested expressions. */
        if (i + 1 != node->count)
            p->last->discarded = !last.never;
    }
    last.never = never;
    last.graph = emitted.graph;
    return last;
}

static MO_MUST_USE struct value call(struct inference *p, struct value callee,
                                     const struct mo_syntax *args, const struct value *receiver) {
    if (callee.type == NULL)
        return (struct value){0};
    struct term *t = infer_root(callee.type);
    if (t->variable) {
        struct term *fn = infer_make_type(p, (struct mo_source_type){.kind = MO_SOURCE_FUNCTION,
                                                                     .name = "fn",
                                                                     .module = UINT32_MAX,
                                                                     .count = args->count + 1});
        if (fn == NULL)
            return (struct value){0};
        for (uint32_t i = 0; i < fn->shape.count; i++) {
            fn->arguments[i] = infer_fresh(p, 0);
            if (fn->arguments[i] == NULL)
                return (struct value){0};
        }
        if (!infer_unify(p, t, fn, 0))
            return (struct value){0};
        t = infer_root(fn);
    }
    if (t->shape.kind != MO_SOURCE_FUNCTION) {
        if (callee.binding != NULL) {
            uint32_t count = 0;
            const struct mo_symbol *symbol = definition(p, callee.binding, NULL, &count);
            if (symbol != NULL && symbol->declaration->kind == MO_S_FN) {
                bool reported =
                    infer_problem(p, MO_CODE_BIND_REPEATED, p->span,
                                  "a name means both a local and a module function",
                                  "A function can't refer to a module function by a name it "
                                  "uses for a local (reference 6.1).",
                                  callee.binding);
                (void)reported;
                return (struct value){0};
            }
        }
        struct term function = {.shape = {.kind = MO_SOURCE_FUNCTION, .name = "fn"}};
        bool valid = infer_unify_as(p, t, &function, MO_CODE_NOT_FUNCTION);
        (void)valid;
        return (struct value){0};
    }
    uint32_t offset = receiver == NULL ? 0u : 1u;
    uint32_t count = args == NULL ? 0u : args->count;
    if (t->shape.count != count + offset + 1) {
        bool reported =
            infer_problem(p, MO_CODE_CALL_ARGUMENTS,
                          args == NULL || args->span.start == args->span.end ? p->span : args->span,
                          "a call's arguments don't match its parameters",
                          "A function has no default or optional parameters; pass every "
                          "argument by position (reference 5.2).",
                          NULL);
        (void)reported;
        return (struct value){0};
    }
    bool never = callee.never;
    if (receiver != NULL) {
        if (!infer_unify_as(p, receiver->type, infer_argument(p, t, 0), MO_CODE_ARGUMENT_TYPE))
            return (struct value){0};
        never = never || receiver->never;
    }
    for (uint32_t i = 0; i < count; i++)
        if (args->children[i]->kind == MO_S_NAMED_ARG) {
            bool reported =
                infer_problem(p, MO_CODE_CALL_ARGUMENTS, args->children[i]->span,
                              "a function is called with named fields",
                              "Arguments are passed by position; only records and cases "
                              "are built with named fields (reference 5.2).",
                              NULL);
            (void)reported;
            return (struct value){0};
        }
    struct note_range *ranges =
        infer_allocate(p, count, sizeof(*ranges), _Alignof(struct note_range));
    if (ranges == NULL)
        return (struct value){0};
    struct value result = {0};
    if (p->checked) {
        bool declared_dot = receiver != NULL && callee.note != NULL;
        uint32_t extra = declared_dot ? 1u : 0u;
        result = infer_graph(p, (struct value){.type = infer_argument(p, t, t->shape.count - 1)},
                             count + 1 + extra);
        if (result.type == NULL)
            return result;
        result.graph->operands[0] = receiver == NULL || declared_dot ? callee.note : receiver->note;
        if (declared_dot)
            result.graph->operands[1] = receiver->note;
        result.graph->output.symbol = callee.symbol;
        if (callee.graph != NULL) {
            result.graph->output.native = callee.graph->output.native;
            result.graph->function_type = callee.graph->function_type;
            result.graph->requirements = callee.graph->requirements;
            result.graph->parameters = callee.graph->parameters;
        }
        if (receiver != NULL)
            result.graph->output.receiver = extra;
    }
    struct note *before = p->last;
    for (unsigned pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < count; i++) {
            if (lambda_argument(args->children[i]) != (pass != 0))
                continue;
            struct note *previous = p->last;
            struct value arg = infer_as(p, args->children[i], infer_argument(p, t, i + offset),
                                        MO_CODE_ARGUMENT_TYPE);
            if (arg.type == NULL)
                return (struct value){0};
            ranges[i] = (struct note_range){previous == NULL ? p->notes : previous->next, p->last};
            if (p->checked)
                result.graph->operands[i + (receiver != NULL && callee.note != NULL ? 2u : 1u)] =
                    arg.note;
            never = never || arg.never;
        }
    }
    /* Constraint solving may visit lambdas last; metadata retains written order
     * so this never suggests reordering evaluation to downstream consumers. */
    order_notes(p, before, ranges, count);
    if (!p->checked)
        result.type = infer_argument(p, t, t->shape.count - 1);
    result.callee = callee.note;
    result.never = never;
    return result;
}

static MO_MUST_USE struct value condition(struct inference *p, const struct mo_syntax *node,
                                          bool keep);

static MO_MUST_USE struct value binary(struct inference *p, enum mo_syntax_token op, struct value a,
                                       const struct mo_syntax *right, struct term *expected) {
    if (a.type == NULL)
        return (struct value){0};
    bool logical = op == MO_T_AND || op == MO_T_OR || op == MO_T_IMPLIES;
    bool comparison = op == MO_T_EQ || op == MO_T_NE || op == MO_T_LT || op == MO_T_LE ||
                      op == MO_T_GT || op == MO_T_GE;
    if (op == MO_T_OR && infer_root(a.type)->variable) {
        struct mo_span span = p->span;
        struct value fallback = infer_expression(p, right, expected);
        struct value out =
            infer_graph(p, (struct value){.type = infer_fresh(p, 0), .never = a.never}, 2);
        struct failure_check *pending =
            infer_allocate(p, 1, sizeof(*pending), _Alignof(struct failure_check));
        if (fallback.type == NULL || out.type == NULL || pending == NULL)
            return (struct value){0};
        *pending = (struct failure_check){.next = p->failures,
                                          .input = a.type,
                                          .value = out.type,
                                          .fallback = fallback.type,
                                          .graph = out.graph,
                                          .span = span,
                                          .never = fallback.never};
        p->failures = pending;
        if (p->checked) {
            out.graph->operands[0] = a.note;
            out.graph->operands[1] = fallback.note;
            out.graph->output.operation = MO_T_OR;
        }
        return out;
    }
    if (logical && op == MO_T_OR && infer_named(a.type, "Option")) {
        struct term *payload = infer_argument(p, a.type, 0);
        if (payload == NULL ||
            (expected != NULL && !infer_unify_as(p, payload, expected, p->node_expectation)))
            return (struct value){0};
        struct value fallback = infer_expression(p, right, payload);
        if (fallback.type == NULL)
            return fallback;
        struct value out = infer_graph(p, (struct value){.type = payload, .never = a.never}, 2);
        if (p->checked && out.type != NULL) {
            out.graph->operands[0] = a.note;
            out.graph->operands[1] = fallback.note;
            out.graph->output.operation = MO_T_OR;
            out.graph->output.or_kind = MO_CHECK_OR_OPTION;
        }
        return out;
    }
    struct term *operand = logical             ? infer_native(p, "Bool", 0)
                           : op == MO_T_DOTDOT ? infer_native(p, "Int", 0)
                                               : a.type;
    if (operand == NULL ||
        !infer_unify_as(p, a.type, operand, logical ? MO_CODE_CONDITION_TYPE : 0))
        return (struct value){0};
    struct term *right_type = operand;
    struct term *result = logical || comparison ? infer_native(p, "Bool", 0)
                          : op == MO_T_DOTDOT   ? infer_native(p, "Range", 0)
                                                : operand;
    struct value b = {0};
    if (!logical && !comparison && op != MO_T_DOTDOT) {
        if (!p->checked && (infer_named(operand, "Time") || infer_named(operand, "Duration") ||
                            infer_root(operand)->shape.kind == MO_SOURCE_PARAMETER)) {
            bool reported = infer_unfinished(p);
            (void)reported;
            return (struct value){0};
        }
        if (infer_named(operand, "Time") && (op == MO_T_PLUS || op == MO_T_MINUS)) {
            struct term *duration = infer_native(p, "Duration", 0);
            if (duration == NULL)
                return (struct value){0};
            if (op == MO_T_PLUS) {
                if (expected != NULL && !infer_unify_as(p, operand, expected, p->node_expectation))
                    return (struct value){0};
                right_type = duration;
            } else {
                struct mo_span span = p->span;
                b = infer_expression(p, right, NULL);
                if (b.type == NULL)
                    return (struct value){0};
                if (!infer_root(b.type)->variable || infer_root(b.type)->numeric != 0) {
                    if (infer_named(b.type, "Time")) {
                        result = duration;
                    } else {
                        if (!infer_unify(p, b.type, duration, 0))
                            return (struct value){0};
                        result = operand;
                    }
                    if (expected != NULL &&
                        !infer_unify_as(p, result, expected, p->node_expectation))
                        return (struct value){0};
                } else {
                    result = infer_fresh(p, 0);
                    struct failure_check *pending =
                        infer_allocate(p, 1, sizeof(*pending), _Alignof(struct failure_check));
                    if (result == NULL || pending == NULL ||
                        (expected != NULL && !infer_unify(p, result, expected, 0)))
                        return (struct value){0};
                    infer_root(result)->deferred = true;
                    *pending = (struct failure_check){.next = p->failures,
                                                      .input = operand,
                                                      .fallback = b.type,
                                                      .value = result,
                                                      .op = MO_T_MINUS,
                                                      .code = p->node_expectation,
                                                      .span = span};
                    p->failures = pending;
                }
            }
        } else if (infer_named(operand, "Duration") &&
                   (op == MO_T_PLUS || op == MO_T_MINUS || op == MO_T_STAR)) {
            if (expected != NULL && !infer_unify_as(p, operand, expected, p->node_expectation))
                return (struct value){0};
            if (op == MO_T_STAR) {
                right_type = infer_native(p, "Int", 0);
                if (right_type == NULL)
                    return (struct value){0};
            }
        } else if (p->checked && (op == MO_T_PLUS || op == MO_T_MINUS || op == MO_T_STAR) &&
                   infer_root(operand)->variable && infer_root(operand)->numeric == 0 &&
                   infer_root(operand)->deferred) {
            struct mo_span span = p->span;
            b = infer_expression(p, right, NULL);
            result = infer_fresh(p, 0);
            struct failure_check *pending =
                infer_allocate(p, 1, sizeof(*pending), _Alignof(struct failure_check));
            if (b.type == NULL || result == NULL || pending == NULL ||
                (expected != NULL && !infer_unify(p, result, expected, 0)))
                return (struct value){0};
            infer_root(result)->deferred = true;
            *pending = (struct failure_check){.next = p->failures,
                                              .input = operand,
                                              .fallback = b.type,
                                              .value = result,
                                              .op = op,
                                              .code = p->node_expectation,
                                              .span = span};
            p->failures = pending;
        } else {
            struct term *numeric = infer_root(operand)->shape.kind == MO_SOURCE_PARAMETER
                                       ? operand
                                       : infer_fresh(p, INT | BIG | DECIMAL | FLOAT);
            if (numeric == NULL ||
                !infer_unify_as(p, operand, numeric,
                                infer_named(operand, "Byte") ? MO_CODE_BYTE_ARITHMETIC
                                                             : MO_CODE_NUMBER_ABILITY))
                return (struct value){0};
            if (expected != NULL && !infer_unify_as(p, operand, expected, p->node_expectation))
                return (struct value){0};
        }
    }
    if (b.type == NULL)
        b = logical ? condition(p, right, op == MO_T_AND) : infer_expression(p, right, right_type);
    if (b.type == NULL)
        return (struct value){0};
    struct value out = infer_graph(
        p, (struct value){.type = result, .never = a.never || (!logical && b.never)}, 2);
    if (p->checked && out.type != NULL) {
        out.graph->operands[0] = a.note;
        out.graph->operands[1] = b.note;
        out.graph->output.operation = op;
        out.graph->output.or_kind = op == MO_T_OR ? MO_CHECK_OR_BOOL : MO_CHECK_OR_ABSENT;
    }
    return out;
}

static MO_MUST_USE struct value propagate(struct inference *p, struct value input,
                                          struct term *expected) {
    if (input.type == NULL)
        return input;
    enum mo_syntax_kind owner = p->function->function->declaration->kind;
    if ((owner == MO_S_CONST || owner == MO_S_SERVICE_STATE || owner == MO_S_SERVICE_EVERY) &&
        p->lambda == 0) {
        bool reported = infer_problem(p, MO_CODE_TRY_RESULT, p->span,
                                      "try has no enclosing function returning Option or Result",
                                      "try passes a failure upwards from the nearest function or "
                                      "anonymous function (reference 8.3).",
                                      NULL);
        (void)reported;
        return (struct value){0};
    }
    struct value out =
        infer_graph(p,
                    (struct value){.type = expected == NULL ? infer_fresh(p, 0) : expected,
                                   .never = input.never},
                    1);
    struct failure_check *pending =
        infer_allocate(p, 1, sizeof(*pending), _Alignof(struct failure_check));
    if (out.type == NULL || pending == NULL)
        return (struct value){0};
    *pending = (struct failure_check){.next = p->failures,
                                      .input = input.type,
                                      .value = out.type,
                                      .context = p->result,
                                      .input_note = input.note,
                                      .graph = out.graph,
                                      .span = p->span,
                                      .never = input.never};
    p->failures = pending;
    if (p->checked && out.type != NULL) {
        out.graph->operands[0] = input.note;
        out.graph->try_result = p->result;
        out.graph->failure = pending;
    }
    if (!infer_root(p->result)->variable &&
        (infer_named(input.type, "Option") || infer_named(input.type, "Result")) &&
        !infer_solve_failure(p, pending, false))
        return (struct value){0};
    return out;
}

static MO_MUST_USE struct mo_match_pattern *
match_pattern(struct inference *p, const struct mo_syntax *node, struct term *type, uintptr_t key,
              uint32_t signature, uint32_t count, const char *name) {
    if (!infer_work(p))
        return NULL;
    if (count > MO_LIMIT_SOURCE_CHECK_WORK - p->work) {
        bool reported =
            infer_problem(p, MO_CODE_CHECK_WORK, p->span,
                          "the function needs too much pattern-checking work", NULL, NULL);
        (void)reported;
        return NULL;
    }
    if (count > MO_LIMIT_PROGRAM_CHECK_WORK - *p->signatures->work) {
        bool reported =
            infer_problem(p, MO_CODE_CHECK_PROGRAM_WORK, p->span,
                          "the program needs too much pattern-checking work", NULL, NULL);
        (void)reported;
        return NULL;
    }
    p->work += count;
    *p->signatures->work += count;
    struct typed_pattern *out = infer_allocate(p, 1, sizeof(*out), _Alignof(struct typed_pattern));
    struct mo_match_pattern **items =
        infer_allocate(p, count, sizeof(*items), _Alignof(struct mo_match_pattern *));
    if (p->failed)
        return NULL;
    memset(items, 0, count * sizeof(*items));
    *out = (struct typed_pattern){
        .match = {.key = key, .signature = signature, .count = count, .name = name, .items = items},
        .type = type,
        .syntax = node,
        .next = p->patterns};
    p->patterns = out;
    return &out->match;
}

static MO_MUST_USE bool pattern_bind(struct inference *p, const struct mo_syntax *node,
                                     struct term *type, uint32_t group, uint32_t alternative) {
    struct name_slot *entry = node->kind == MO_S_NAME ? slot(p, node, false) : NULL;
    struct local *local = entry == NULL ? NULL : entry->active;
    if (alternative > 1 && node->kind != MO_S_UNDERSCORE &&
        (local == NULL || local->pattern_group != group))
        return infer_problem(p, MO_CODE_PATTERN_ALTERNATIVES, node->span,
                             "the alternatives don't bind the same names",
                             "Arms with the same body share a line; their names must be visible "
                             "throughout that body (reference 7.2).",
                             NULL);
    if (alternative > 1 && local != NULL && local->pattern_group == group &&
        local->pattern_alternative != alternative) {
        if (!infer_unify(p, local->type, type, 0))
            return false;
        local->pattern_alternative = alternative;
        return infer_record(
            p, node, (struct value){.type = type, .binding = local->name, .bound = local->binding});
    } else {
        if (!bind(p, node, type, false))
            return false;
        if (node->kind != MO_S_UNDERSCORE) {
            local = p->locals;
            local->pattern_group = group;
            local->pattern_alternative = alternative;
        }
    }
    return true;
}

static MO_MUST_USE bool pattern(struct inference *p, const struct mo_syntax *node,
                                struct term *type, uint32_t group, uint32_t alternative,
                                struct mo_match_pattern **out);

static MO_MUST_USE bool pattern_node(struct inference *p, const struct mo_syntax *node,
                                     struct term *type, uint32_t group, uint32_t alternative,
                                     struct mo_match_pattern **out) {
    *out = NULL;
    if (node->kind == MO_S_GROUP)
        return pattern(p, node->children[0], type, group, alternative, out);
    if (node->kind == MO_S_UNDERSCORE || node->kind == MO_S_NAME)
        return pattern_bind(p, node, type, group, alternative);
    if (node->kind == MO_S_TUPLE) {
        struct term *tuple = infer_make_type(p, (struct mo_source_type){.kind = MO_SOURCE_TUPLE,
                                                                        .name = "()",
                                                                        .module = UINT32_MAX,
                                                                        .count = node->count});
        if (tuple == NULL)
            return false;
        for (uint32_t i = 0; i < node->count; i++) {
            tuple->arguments[i] = infer_fresh(p, 0);
            if (tuple->arguments[i] == NULL)
                return false;
        }
        if (!infer_unify(p, type, tuple, 0))
            return false;
        *out = match_pattern(p, node, type, 5, 1, node->count, "tuple");
        if (*out == NULL)
            return false;
        for (uint32_t i = 0; i < node->count; i++)
            if (!pattern(p, node->children[i], tuple->arguments[i], group, alternative,
                         &(*out)->items[i]))
                return false;
        return infer_record(p, node, (struct value){.type = type});
    }
    if (node->kind == MO_S_LIST) {
        struct term *list = infer_native(p, "List", 1);
        if (list == NULL)
            return false;
        list->arguments[0] = infer_fresh(p, 0);
        if (list->arguments[0] == NULL || !infer_unify(p, type, list, 0))
            return false;
        struct mo_match_pattern **parts =
            infer_allocate(p, node->count, sizeof(*parts), _Alignof(struct mo_match_pattern *));
        if (parts == NULL)
            return false;
        uint32_t length = node->count;
        bool rest = length != 0 && node->children[length - 1]->kind == MO_S_REST;
        if (rest)
            length--;
        for (uint32_t i = 0; i < length; i++)
            if (!pattern(p, node->children[i], list->arguments[0], group, alternative, &parts[i]))
                return false;
        struct mo_match_pattern *tail = NULL;
        if (rest) {
            if (!pattern(p, node->children[length]->children[0], type, group, alternative, &tail))
                return false;
        } else {
            tail = match_pattern(p, node, type, 3, 2, 0, "[]");
            if (tail == NULL)
                return false;
        }
        /* Flat list prefixes do not consume C stack. */
        for (uint32_t i = length; i != 0; i--) {
            struct mo_match_pattern *cons = match_pattern(p, node, type, 4, 2, 2, "list cell");
            if (cons == NULL)
                return false;
            cons->items[0] = parts[i - 1];
            cons->items[1] = tail;
            tail = cons;
        }
        *out = tail;
        return infer_record(p, node, (struct value){.type = type});
    }
    bool qualified_call =
        node->kind == MO_S_POSTFIX && node->count == 3 && node->children[0]->kind == MO_S_CAPNAME &&
        node->children[1]->kind == MO_S_QUALIFIED && node->children[2]->kind == MO_S_CALL;
    if (node->kind == MO_S_CAPNAME || node->kind == MO_S_QUALIFIED || node->kind == MO_S_CALL ||
        qualified_call) {
        const struct mo_syntax *name = node->kind == MO_S_CALL ? node->children[0] : node;
        const struct mo_syntax *args = node->kind == MO_S_CALL ? node->children[1] : NULL;
        const struct mo_syntax *prefix = NULL;
        if (qualified_call) {
            prefix = node->children[0];
            name = node->children[1]->children[0];
            args = node->children[2]->children[0];
        }
        const struct mo_builtin_case *builtin = mo_builtin_case_by_name(name);
        if (prefix != NULL)
            builtin = NULL;
        if (builtin != NULL) {
            struct term *nominal = builtin_type(p, builtin, type);
            if (nominal == NULL)
                return false;
            bool unnamed = builtin->count != 0 && builtin->fields[0] == NULL;
            if ((unnamed &&
                 (args == NULL || args->count != 1 || args->children[0]->kind == MO_S_NAMED_ARG)) ||
                (!unnamed && builtin->count != 0 && args == NULL))
                return infer_problem(p, MO_CODE_PATTERN_FIELDS, node->span,
                                     "the pattern doesn't match the case's fields",
                                     "A case with named fields is matched by naming them; Some(p) "
                                     "matches one unnamed value (reference 7.3).",
                                     NULL);
            *out = match_pattern(p, node, type, builtin->key, builtin->cases, builtin->count,
                                 builtin->name);
            if (*out == NULL)
                return false;
            bool seen[2] = {false, false};
            for (uint32_t i = 0; args != NULL && i < args->count; i++) {
                const struct mo_syntax *arg = args->children[i];
                uint32_t index = infer_builtin_field(builtin, arg);
                if (index == UINT32_MAX || index >= builtin->count || seen[index])
                    return infer_problem(
                        p, MO_CODE_PATTERN_FIELDS, arg->span,
                        "a matched field is absent or repeated",
                        "A case is matched with its named fields; fields not named "
                        "are ignored (reference 7.3).",
                        NULL);
                seen[index] = true;
                struct term *field = infer_builtin_part(p, builtin, nominal, index);
                if (field == NULL || !pattern(p, unnamed ? arg : arg->children[1], field, group,
                                              alternative, &(*out)->items[index]))
                    return false;
            }
            return infer_record(p, node,
                                (struct value){.type = type, .builtin_case = builtin->name});
        }
        uint32_t count = 0;
        const struct mo_symbol *symbol = definition(p, name, prefix, &count);
        if (p->failed)
            return false;
        const struct mo_data_signature *data = symbol == NULL ? NULL : infer_data_type(p, symbol);
        if (data == NULL)
            return infer_problem(p, MO_CODE_PATTERN_CONSTRUCTOR, name->span,
                                 "a pattern doesn't name a record or case",
                                 "A pattern names a case or record, a literal, a tuple or a list "
                                 "(reference 7.3).",
                                 NULL);
        if (!infer_visible_data(p, data->type->declaration))
            return false;
        struct term *parameters[26] = {NULL};
        struct term *nominal = infer_annotation(p, data->type, parameters, 0);
        if (nominal == NULL || !infer_unify(p, type, nominal, 0))
            return false;
        if ((data->unnamed &&
             (args == NULL || args->count != 1 || args->children[0]->kind == MO_S_NAMED_ARG)) ||
            (!data->unnamed && args == NULL && data->count != 0))
            return infer_problem(p, MO_CODE_PATTERN_FIELDS, node->span,
                                 "the pattern doesn't match the case's fields",
                                 "A case with named fields is matched by naming them; Circle(p) is "
                                 "only for one unnamed value (reference 7.3).",
                                 symbol->declaration->children[0]);
        const struct mo_symbol *owner = data->type->declaration;
        *out = match_pattern(p, node, type, (uintptr_t)symbol,
                             symbol->owner == UINT32_MAX ? 1u : owner->case_count, data->count,
                             symbol->name);
        if (*out == NULL)
            return false;
        (*out)->choice = symbol->owner == UINT32_MAX ? NULL : owner;
        (*out)->data = data;
        bool *seen = infer_allocate(p, data->count, sizeof(*seen), _Alignof(bool));
        if (seen == NULL)
            return false;
        memset(seen, 0, data->count * sizeof(*seen));
        for (uint32_t i = 0; args != NULL && i < args->count; i++) {
            const struct mo_syntax *arg = args->children[i];
            const struct mo_field_signature *field = data->unnamed ? &data->fields[0]
                                                     : arg->kind == MO_S_NAMED_ARG
                                                         ? find_field(data, arg->children[0])
                                                         : NULL;
            if (!data->unnamed && data->count == 1 && args->count == 1 &&
                arg->kind != MO_S_NAMED_ARG) {
                const struct mo_syntax *field_name = data->fields[0].name;
                size_t length = field_name->span.end - field_name->span.start;
                char *replacement = infer_allocate(p, length + 3, 1, 1);
                if (replacement == NULL)
                    return false;
                memcpy(replacement, field_name->text, length);
                memcpy(replacement + length, ": ", 3);
                bool reported = infer_problem(p, MO_CODE_PATTERN_FIELDS, arg->span,
                                              "a matched field hasn't been named",
                                              "A case with named fields is matched by naming them "
                                              "(reference 7.3).",
                                              field_name);
                (void)reported;
                if (p->error.code == MO_CODE_PATTERN_FIELDS &&
                    p->error.primary.span.start == arg->span.start) {
                    p->edit = (struct mo_edit){{arg->span.file, arg->span.start, arg->span.start},
                                               replacement};
                    p->fix = (struct mo_fix){"name the matched field", &p->edit, 1};
                    p->error.fixes = &p->fix;
                    p->error.fix_count = 1;
                }
                return false;
            }
            if (field == NULL || seen[field->index])
                return infer_problem(p, MO_CODE_PATTERN_FIELDS, arg->span,
                                     "a matched field is absent or repeated",
                                     "A record or case is matched with its named fields; fields "
                                     "not named are ignored (reference 7.3).",
                                     symbol->declaration->children[0]);
            seen[field->index] = true;
            struct term *field_type = infer_annotation(p, field->type, parameters, 0);
            if (field_type == NULL ||
                !pattern(p, data->unnamed ? arg : arg->children[1], field_type, group, alternative,
                         &(*out)->items[field->index]))
                return false;
            p->last->output.field = field;
        }
        return infer_record(p, node, (struct value){.type = type, .symbol = symbol});
    }
    bool literal =
        node->kind == MO_S_INT || node->kind == MO_S_DECIMAL || node->kind == MO_S_TEXT ||
        node->kind == MO_S_CHAR || node->kind == MO_S_TRUE || node->kind == MO_S_FALSE ||
        (node->kind == MO_S_NEGATE &&
         (node->children[0]->kind == MO_S_INT || node->children[0]->kind == MO_S_DECIMAL));
    if (!literal)
        return infer_problem(p, MO_CODE_PATTERN_CONSTRUCTOR, node->span,
                             "a pattern doesn't name a record or case",
                             "A pattern names a case or record, a literal, a tuple or a list "
                             "(reference 7.3).",
                             NULL);
    /* Retain inference notes for contextual range checking, but no executable
     * rows: the normalized LITERAL already carries this spelling and type. */
    bool checked = p->checked;
    p->checked = false;
    struct value literal_value = infer_expression(p, node, type);
    p->checked = checked;
    if (literal_value.type == NULL)
        return false;
    bool boolean = node->kind == MO_S_TRUE || node->kind == MO_S_FALSE;
    *out =
        match_pattern(p, node, type, node->kind == MO_S_TRUE ? 1u : 2u, boolean ? 2u : UINT32_MAX,
                      0, boolean ? (node->kind == MO_S_TRUE ? "true" : "false") : "literal");
    return *out != NULL;
}

static MO_MUST_USE bool pattern(struct inference *p, const struct mo_syntax *node,
                                struct term *type, uint32_t group, uint32_t alternative,
                                struct mo_match_pattern **out) {
    if (!infer_work(p))
        return false;
    MO_ASSERT(p->pattern_depth < MO_LIMIT_NESTING_DEPTH);
    p->span = node->span;
    p->pattern_depth++;
    bool ok = pattern_node(p, node, type, group, alternative, out);
    p->pattern_depth--;
    return ok;
}

static MO_MUST_USE struct value condition(struct inference *p, const struct mo_syntax *node,
                                          bool keep) {
    bool previous = p->keep_patterns;
    p->keep_patterns = keep;
    struct term *boolean = infer_native(p, "Bool", 0);
    struct value v =
        boolean == NULL ? (struct value){0} : infer_as(p, node, boolean, MO_CODE_CONDITION_TYPE);
    p->keep_patterns = previous;
    return v;
}

static MO_MUST_USE struct value match(struct inference *p, const struct mo_syntax *node,
                                      struct term *expected) {
    struct value input = infer_expression(p, node->children[0], NULL);
    struct term *result = expected == NULL ? infer_fresh(p, 0) : expected;
    if (input.type == NULL || result == NULL)
        return (struct value){0};
    const struct mo_syntax *arms = node->children[1];
    uint32_t count = 0;
    for (uint32_t i = 0; i < arms->count; i++)
        count += arms->children[i]->children[0]->count;
    struct match_check *check = infer_allocate(p, 1, sizeof(*check), _Alignof(struct match_check));
    struct mo_match_pattern **patterns =
        infer_allocate(p, count, sizeof(*patterns), _Alignof(struct mo_match_pattern *));
    if (p->failed)
        return (struct value){0};
    struct mo_span header = node->span;
    header.end = node->children[0]->span.end;
    *check = (struct match_check){.patterns = patterns,
                                  .span = header,
                                  .end = node->span.end,
                                  .scrutinee = node->children[0],
                                  .type = input.type,
                                  .next = p->matches};
    p->matches = check;
    struct value emitted = infer_graph(p, (struct value){.type = result}, 1 + 2 * arms->count);
    if (emitted.type == NULL)
        return emitted;
    uint32_t operand = 1;
    if (p->checked) {
        struct emitted_match *match =
            infer_allocate(p, 1, sizeof(*match), _Alignof(struct emitted_match));
        struct mo_checked_arm *rows =
            infer_allocate(p, arms->count, sizeof(*rows), _Alignof(struct mo_checked_arm));
        if (p->failed)
            return (struct value){0};
        *match = (struct emitted_match){.output = {.arm_count = arms->count, .arms = rows},
                                        .arms = rows};
        emitted.graph->match = match;
        emitted.graph->operands[0] = input.note;
    }
    bool never = true;
    for (uint32_t i = 0; i < arms->count; i++) {
        struct local *mark = p->locals;
        struct capture_path *old_path = p->capture_path;
        const struct mo_syntax *arm = arms->children[i], *alternatives = arm->children[0];
        uint32_t group = ++p->pattern_group;
        for (uint32_t j = 0; j < alternatives->count; j++) {
            struct mo_match_pattern *normalized = NULL;
            if (!pattern(p, alternatives->children[j], input.type, group, j + 1, &normalized))
                return (struct value){0};
            if (normalized == NULL && check->catch_all == NULL)
                check->catch_all = alternatives->children[j];
            if (arm->children[1] == NULL)
                check->patterns[check->count++] = normalized;
            for (struct local *local = p->locals; local != mark; local = local->previous) {
                if (local->pattern_alternative != j + 1) {
                    bool reported = infer_problem(
                        p, MO_CODE_PATTERN_ALTERNATIVES, alternatives->children[j]->span,
                        "the alternatives don't bind the same names",
                        "Arms with the same body share a line; their names must be "
                        "visible throughout that body (reference 7.2).",
                        local->name);
                    (void)reported;
                    return (struct value){0};
                }
            }
        }
        if (p->checked)
            emitted.graph->match->arms[i] = (struct mo_checked_arm){
                .syntax = arm, .pattern_count = alternatives->count, .guard = UINT32_MAX};
        if (arm->children[1] != NULL) {
            struct value guard = condition(p, arm->children[1], true);
            if (guard.type == NULL)
                return guard;
            if (p->checked) {
                emitted.graph->match->arms[i].guard = operand;
                emitted.graph->operands[operand++] = guard.note;
            }
        }
        if (!capture_path(p, node, i))
            return (struct value){0};
        struct value body = infer_expression(p, arm->children[2], result);
        if (body.type == NULL || !leave(p, mark))
            return (struct value){0};
        p->capture_path = old_path;
        if (p->checked) {
            emitted.graph->match->arms[i].body = operand;
            emitted.graph->operands[operand++] = body.note;
        }
        never = never && body.never;
    }
    if (p->checked)
        emitted.graph->output.operand_count = operand;
    emitted.never = never;
    return emitted;
}

static MO_MUST_USE struct value conditional(struct inference *p, const struct mo_syntax *node,
                                            struct term *expected) {
    const struct mo_syntax *parts = node->children[2];
    bool has_else = parts->count != 0 && parts->children[parts->count - 1]->kind == MO_S_ELSE;
    struct term *result =
        has_else ? (expected == NULL ? infer_fresh(p, 0) : expected) : infer_native(p, "Unit", 0);
    struct term *condition = infer_native(p, "Bool", 0);
    if (result == NULL || condition == NULL)
        return (struct value){0};
    struct value emitted =
        infer_graph(p, (struct value){.type = result}, 2 + 2 * parts->count - (has_else ? 1u : 0u));
    if (emitted.type == NULL)
        return emitted;
    uint32_t operand = 0;
    struct local *mark = p->locals;
    bool previous = p->keep_patterns;
    p->keep_patterns = true;
    struct value test = infer_as(p, node->children[0], condition, MO_CODE_CONDITION_TYPE);
    p->keep_patterns = previous;
    if (test.type == NULL)
        return (struct value){0};
    if (p->checked)
        emitted.graph->operands[operand++] = test.note;
    struct capture_path *old_path = p->capture_path;
    if (!capture_path(p, node, 0))
        return (struct value){0};
    struct value branch = has_else ? infer_as(p, node->children[1], result, p->node_expectation)
                                   : discard(p, node->children[1]);
    if (branch.type == NULL || !leave(p, mark))
        return (struct value){0};
    p->capture_path = old_path;
    if (p->checked)
        emitted.graph->operands[operand++] = branch.note;
    bool never = has_else && branch.never;
    for (uint32_t i = 0; i < parts->count; i++) {
        const struct mo_syntax *part = parts->children[i];
        if (part->kind == MO_S_IF_PART) {
            p->keep_patterns = true;
            test = infer_as(p, part->children[0], condition, MO_CODE_CONDITION_TYPE);
            p->keep_patterns = previous;
            if (test.type == NULL)
                return (struct value){0};
            if (p->checked)
                emitted.graph->operands[operand++] = test.note;
        }
        const struct mo_syntax *body = part->children[part->kind == MO_S_ELSE ? 0 : 1];
        if (!capture_path(p, node, i + 1))
            return (struct value){0};
        branch = has_else ? infer_as(p, body, result, p->node_expectation) : discard(p, body);
        if (branch.type == NULL || !leave(p, mark))
            return (struct value){0};
        p->capture_path = old_path;
        if (p->checked)
            emitted.graph->operands[operand++] = branch.note;
        never = never && branch.never;
    }
    if (never)
        result = expected == NULL ? infer_native(p, "Unit", 0) : expected;
    emitted.type = result;
    emitted.never = never;
    return emitted;
}

static MO_MUST_USE bool binder(struct inference *p, const struct mo_syntax *node,
                               struct term *type) {
    if (node->kind == MO_S_BINDER)
        return binder(p, node->children[0], type);
    if (node->kind != MO_S_TUPLE)
        return bind(p, node, type, false);
    struct term *tuple = infer_make_type(
        p, (struct mo_source_type){
               .kind = MO_SOURCE_TUPLE, .name = "()", .module = UINT32_MAX, .count = node->count});
    if (tuple == NULL)
        return false;
    for (uint32_t i = 0; i < node->count; i++) {
        tuple->arguments[i] = infer_fresh(p, 0);
        if (tuple->arguments[i] == NULL)
            return false;
    }
    if (!infer_unify(p, tuple, type, 0))
        return false;
    for (uint32_t i = 0; i < node->count; i++)
        if (!binder(p, node->children[i], tuple->arguments[i]))
            return false;
    return true;
}

/* The part of tuple type `t` that `index` reads, or NULL after reporting. */
MO_MUST_USE struct term *infer_tuple_part(struct inference *p, struct term *t,
                                          const struct mo_syntax *index, uint32_t *part) {
    uint32_t at = 0;
    bool fits = true;
    for (uint32_t i = 0; i < index->span.end - index->span.start; i++) {
        uint8_t c = index->text[i];
        if (c == '_')
            continue;
        uint32_t digit = (uint32_t)(c - '0');
        if (digit > 9 || at > (UINT32_MAX - digit) / 10) {
            fits = false;
            break;
        }
        at = at * 10 + digit;
    }
    if (!fits || t->shape.kind != MO_SOURCE_TUPLE || at >= t->shape.count) {
        bool reported =
            infer_problem(p, MO_CODE_TUPLE_POSITION, index->span, "the tuple has no such part",
                          "Tuple parts are read with .0, .1 and so on (reference 3.6).", NULL);
        (void)reported;
        return NULL;
    }
    *part = at;
    return infer_argument(p, t, at);
}

static MO_MUST_USE struct value position(struct inference *p, struct value v,
                                         const struct mo_syntax *index) {
    if (v.type == NULL)
        return v;
    struct term *t = infer_root(v.type);
    if (t->variable) {
        /* A later use may settle the tuple's type (reference 5.1); solved with the dispatches. */
        struct value out = deferred_function(p, index, NULL, 0);
        if (out.type == NULL)
            return out;
        out.dispatch->receiver = v;
        out.dispatch->position = true;
        out.never = v.never;
        return out;
    }
    uint32_t at;
    struct term *part = infer_tuple_part(p, t, index, &at);
    if (part == NULL)
        return (struct value){0};
    struct value out = infer_graph(p, (struct value){.type = part, .never = v.never}, 1);
    if (p->checked && out.type != NULL) {
        out.graph->operands[0] = v.note;
        out.graph->output.field = at;
    }
    return out;
}

static MO_MUST_USE struct value infer_node(struct inference *p, const struct mo_syntax *node,
                                           struct term *expected) {
    switch (node->kind) {
    case MO_S_SEQUENCE:
        return sequence(p, node, expected);
    case MO_S_EXPR_STMT:
    case MO_S_GROUP:
        return infer_as(p, node->children[0], expected, p->node_expectation);
    case MO_S_TEXT_INTERPOLATED: {
        uint32_t count = node->count / 2;
        struct value out =
            infer_graph(p, (struct value){.type = infer_native(p, "Text", 0)}, count);
        if (out.type == NULL)
            return out;
        for (uint32_t i = 0; i < count; i++) {
            struct value hole = infer_expression(p, node->children[2 * i + 1], NULL);
            if (hole.type == NULL)
                return (struct value){0};
            if (p->checked)
                out.graph->operands[i] = hole.note;
            out.never = out.never || hole.never;
        }
        return out;
    }
    case MO_S_NAME:
    case MO_S_CAPNAME:
    case MO_S_QUALIFIED:
        return identifier(p, node, NULL);
    case MO_S_FIELD_ACCESS: {
        if (node->children[0]->kind == MO_S_CAPNAME &&
            (!case_receiver(p, node->children[0]) ||
             type_reading(node->children[0], node->children[1], NULL)))
            return identifier(p, node->children[1], node->children[0]);
        bool called;
        return field_value(p, infer_expression(p, node->children[0], NULL), node->children[1], node,
                           NULL, &called);
    }
    case MO_S_INT:
        return (struct value){.type = infer_fresh(p, INT | BIG | BYTE | DECIMAL | FLOAT)};
    case MO_S_DECIMAL:
        return (struct value){.type = infer_fresh(p, DECIMAL | FLOAT)};
    case MO_S_CHAR:
        return (struct value){.type = infer_native(p, "Char", 0)};
    case MO_S_TEXT:
        return (struct value){.type = infer_native(p, "Text", 0)};
    case MO_S_TRUE:
    case MO_S_FALSE:
        return (struct value){.type = infer_native(p, "Bool", 0)};
    case MO_S_BIND:
    case MO_S_VAR:
    case MO_S_ADD_ASSIGN:
    case MO_S_SUB_ASSIGN: {
        const struct mo_syntax *name = node->children[0];
        if (name->kind == MO_S_FIELD_ACCESS || name->kind == MO_S_POSTFIX) {
            const struct mo_syntax *base = name;
            bool fields = true;
            while (base->kind == MO_S_FIELD_ACCESS || base->kind == MO_S_POSTFIX) {
                if (base->kind == MO_S_POSTFIX)
                    for (uint32_t i = 1; i < base->count; i++)
                        fields = fields && base->children[i]->kind == MO_S_FIELD_ACCESS;
                base = base->children[0];
            }
            struct name_slot *entry = base->kind == MO_S_NAME ? slot(p, base, false) : NULL;
            struct local *variable = entry == NULL ? NULL : entry->active;
            if (!fields || variable == NULL || !variable->variable ||
                variable->function != p->lambda) {
                bool reported =
                    infer_problem(p, MO_CODE_NOT_VARIABLE, name->span,
                                  "a field is assigned without an assignable var",
                                  "A var or its fields can be assigned within the function "
                                  "that declares it (reference 6.2).",
                                  variable == NULL ? NULL : variable->name);
                (void)reported;
                return (struct value){0};
            }
            if (p->lambda != 0) {
                bool reported = infer_problem(
                    p, MO_CODE_LAMBDA_WRITE, name->span, "an anonymous function can't assign a var",
                    "An anonymous function can read names in scope, but can't "
                    "assign a var (reference 5.6).",
                    variable->name);
                (void)reported;
                return (struct value){0};
            }
            if (!capture_write(p, variable, node, name))
                return (struct value){0};
            variable->version++;
            struct value target = infer_expression(p, name, NULL);
            if (target.type == NULL)
                return (struct value){0};
            p->last->output.binding = variable->name;
            struct value rhs =
                node->kind == MO_S_BIND
                    ? infer_expression(p, node->children[1], target.type)
                    : binary(p, node->kind == MO_S_ADD_ASSIGN ? MO_T_PLUS : MO_T_MINUS, target,
                             node->children[1], target.type);
            if (rhs.type == NULL)
                return (struct value){0};
            if (p->checked && node->kind != MO_S_BIND) {
                if (!infer_record(p, node, rhs))
                    return (struct value){0};
                rhs.note = p->last;
            }
            struct value out = infer_graph(p,
                                           (struct value){.type = infer_native(p, "Unit", 0),
                                                          .binding = variable->name,
                                                          .bound = variable->binding,
                                                          .field = target.field,
                                                          .never = rhs.never},
                                           2);
            if (p->checked && out.type != NULL) {
                out.graph->output.assignment = true;
                out.graph->operands[0] = target.note;
                out.graph->operands[1] = rhs.note;
            }
            return out;
        }
        if (name->kind != MO_S_NAME && name->kind != MO_S_TUPLE && name->kind != MO_S_UNDERSCORE) {
            bool reported = infer_unfinished(p);
            (void)reported;
            return (struct value){0};
        }
        struct name_slot *s = name->kind == MO_S_NAME ? slot(p, name, false) : NULL;
        struct local *l = s == NULL ? NULL : s->active;
        bool assignment = l != NULL && l->variable && node->kind != MO_S_VAR;
        if (l != NULL && !assignment) {
            /* Assigning a name first bound by `name = value` in this function needs only `var`
             * there, when the new value has the name's type (D290); a value of another type
             * needs a new name. Any other earlier meaning keeps the plain rule. */
            bool statement = l->statement && l->function == p->lambda && node->kind != MO_S_VAR;
            bool to_var = statement, differs = false;
            if (statement && node->kind == MO_S_BIND) {
                struct value rhs = infer_expression(p, node->children[1], NULL);
                if (p->fatal)
                    return (struct value){0};
                to_var = rhs.type != NULL && infer_same_type(p, rhs.type, l->type, 0);
                if (p->fatal)
                    return (struct value){0};
                differs = rhs.type != NULL && !to_var;
            }
            bool reported = infer_problem(
                p, MO_CODE_BIND_REPEATED, name->span, "a visible name is bound again",
                to_var    ? "While a name is visible it can't be bound again; to assign it again, "
                            "declare it with var where it is first bound (reference 6.2)."
                : differs ? "While a name is visible it can't be bound again; this value has "
                            "another type, so bind it to a new name instead (reference 6.1)."
                          : "While a local name or parameter is visible it can't be bound "
                            "again (reference 6.1).",
                l->name);
            (void)reported;
            if (to_var && p->error.code == MO_CODE_BIND_REPEATED &&
                p->error.primary.span.start == name->span.start) {
                struct mo_span at = l->name->span;
                p->edit = (struct mo_edit){{at.file, at.start, at.start}, "var "};
                p->fix = (struct mo_fix){"declare it with var", &p->edit, 1};
                p->error.fixes = &p->fix;
                p->error.fix_count = 1;
            }
            return (struct value){0};
        }
        if ((node->kind == MO_S_ADD_ASSIGN || node->kind == MO_S_SUB_ASSIGN) && !assignment) {
            bool reported = infer_problem(
                p, MO_CODE_NOT_VARIABLE, name->span, "a name that isn't var is assigned",
                "+= and -= assign a var or its fields (reference 6.2).", NULL);
            (void)reported;
            return (struct value){0};
        }
        if (assignment && l->function != p->lambda) {
            bool reported =
                infer_problem(p, MO_CODE_NOT_VARIABLE, name->span,
                              "a var is assigned outside the function that declares it",
                              "A var can be assigned again within the function that declares "
                              "it (reference 6.2).",
                              l->name);
            (void)reported;
            return (struct value){0};
        }
        if (assignment && p->lambda != 0) {
            bool reported = infer_problem(
                p, MO_CODE_LAMBDA_WRITE, name->span, "an anonymous function can't assign a var",
                "An anonymous function can read names in scope, but can't "
                "assign a var (reference 5.6).",
                l->name);
            (void)reported;
            return (struct value){0};
        }
        if (assignment && !capture_write(p, l, node, name))
            return (struct value){0};
        if (assignment)
            l->version++;
        struct value target = {0};
        if (assignment) {
            target = (struct value){.type = l->type, .binding = l->name, .bound = l->binding};
            if (node->kind != MO_S_BIND)
                target = infer_graph(p, target, 0);
            if (target.type == NULL || !infer_record(p, name, target))
                return (struct value){0};
            target.note = p->last;
        }
        struct value rhs;
        if (assignment && node->kind != MO_S_BIND) {
            l->used = true;
            rhs = binary(p, node->kind == MO_S_ADD_ASSIGN ? MO_T_PLUS : MO_T_MINUS, target,
                         node->children[1], l->type);
        } else
            rhs = infer_expression(p, node->children[1], assignment ? l->type : NULL);
        if (rhs.type == NULL)
            return (struct value){0};
        if (p->checked && assignment && node->kind != MO_S_BIND) {
            if (!infer_record(p, node, rhs))
                return (struct value){0};
            rhs.note = p->last;
        }
        if (!assignment &&
            !(node->kind == MO_S_VAR ? bind(p, name, rhs.type, true) : binder(p, name, rhs.type)))
            return (struct value){0};
        if (!assignment && node->kind == MO_S_BIND && name->kind == MO_S_NAME)
            p->locals->statement = true;
        struct value result =
            infer_graph(p,
                        (struct value){.type = infer_native(p, "Unit", 0),
                                       .binding = assignment ? l->name : NULL,
                                       .bound = assignment                 ? l->binding
                                                : name->kind == MO_S_TUPLE ? NULL
                                                                           : p->last->binding,
                                       .never = rhs.never},
                        1);
        if (p->checked && result.type != NULL) {
            result.graph->operands[0] = rhs.note;
            result.graph->output.assignment = assignment;
            if (name->kind == MO_S_TUPLE)
                result.graph->pattern_type = rhs.type;
        }
        return result;
    }
    case MO_S_BINARY:
    case MO_S_RANGE: {
        bool logical =
            node->token == MO_T_AND || node->token == MO_T_OR || node->token == MO_T_IMPLIES;
        struct local *mark = p->locals;
        struct value left = logical && node->token != MO_T_OR
                                ? condition(p, node->children[0], true)
                                : infer_expression(p, node->children[0], NULL);
        struct value result = binary(p, node->token, left, node->children[1], expected);
        if (result.type == NULL || (logical && node->token != MO_T_AND && !leave(p, mark)))
            return (struct value){0};
        return result;
    }
    case MO_S_BINARY_CHAIN: {
        enum mo_syntax_token op = node->children[1]->token;
        struct value result = op == MO_T_AND ? condition(p, node->children[0], true)
                                             : infer_expression(p, node->children[0], NULL);
        for (uint32_t i = 1; i < node->count && result.type != NULL; i += 2) {
            result = binary(p, node->children[i]->token, result, node->children[i + 1],
                            i + 2 == node->count ? expected : NULL);
            if (p->checked && result.type != NULL && i + 2 < node->count) {
                if (!infer_record(p, node->children[i], result))
                    return (struct value){0};
                result.note = p->last;
            }
        }
        return result;
    }
    case MO_S_CALL: {
        const struct mo_syntax *callee = node->children[0];
        const struct mo_builtin_case *builtin = mo_builtin_case_by_name(callee);
        if (builtin != NULL)
            return builtin_construct(p, builtin, node->children[1], expected);
        if (callee->kind == MO_S_CAPNAME || callee->kind == MO_S_QUALIFIED) {
            uint32_t count = 0;
            const struct mo_symbol *symbol = definition(p, callee, NULL, &count);
            if (p->failed)
                return (struct value){0};
            const struct mo_data_signature *data =
                symbol == NULL ? NULL : infer_data_type(p, symbol);
            if (data != NULL)
                return construct(p, data, node->children[1], expected);
        }
        return call(p, infer_expression(p, node->children[0], NULL), node->children[1], NULL);
    }
    case MO_S_POSITION:
        return position(p, infer_expression(p, node->children[0], NULL), node->children[1]);
    case MO_S_POSTFIX: {
        bool qualified = node->children[0]->kind == MO_S_CAPNAME &&
                         (node->children[1]->kind == MO_S_QUALIFIED ||
                          (node->children[1]->kind == MO_S_FIELD_ACCESS &&
                           (!case_receiver(p, node->children[0]) ||
                            type_reading(node->children[0], node->children[1]->children[0],
                                         node->count > 2 && node->children[2]->kind == MO_S_CALL
                                             ? node->children[2]
                                             : NULL))));
        uint32_t start = qualified ? 2u : 1u;
        const struct mo_syntax *name =
            qualified ? node->children[1]->children[0] : node->children[0];
        bool simulation = qualified && infer_compare_text(node->children[0], "Simulation") == 0;
        const struct mo_data_signature *data = NULL;
        const struct mo_builtin_case *builtin =
            !qualified && start < node->count && node->children[start]->kind == MO_S_CALL
                ? mo_builtin_case_by_name(name)
                : NULL;
        bool ability = qualified && (infer_compare_text(name, "to_text") == 0 ||
                                     infer_compare_text(name, "compare") == 0);
        if (!simulation && (name->kind == MO_S_CAPNAME || qualified) && !ability &&
            start < node->count && node->children[start]->kind == MO_S_CALL) {
            uint32_t count = 0;
            const struct mo_symbol *symbol =
                definition(p, name, qualified ? node->children[0] : NULL, &count);
            if (p->failed)
                return (struct value){0};
            data = symbol == NULL ? NULL : infer_data_type(p, symbol);
        }
        struct value v =
            simulation
                ? simulation_call(p, name,
                                  start < node->count && node->children[start]->kind == MO_S_CALL
                                      ? node->children[start]->children[0]
                                      : NULL)
            : builtin != NULL ? builtin_construct(p, builtin, node->children[start]->children[0],
                                                  start + 1 == node->count ? expected : NULL)
            : data != NULL    ? construct(p, data, node->children[start]->children[0],
                                       start + 1 == node->count ? expected : NULL)
            : qualified       ? identifier(p, node->children[1]->children[0], node->children[0])
                              : infer_expression(p, node->children[0], NULL);
        if (simulation || data != NULL || builtin != NULL)
            start++;
        if (p->checked && v.type != NULL && v.note == NULL && start < node->count) {
            if (v.graph == NULL)
                v = infer_graph(p, v, 0);
            if (v.type == NULL || !infer_record(p, node->children[start - 1], v))
                return (struct value){0};
            v.note = p->last;
        }
        for (uint32_t i = start; i < node->count && v.type != NULL; i++) {
            const struct mo_syntax *step = node->children[i];
            if (step->kind == MO_S_POSITION)
                v = position(p, v, step->children[0]);
            else if (step->kind == MO_S_CALL)
                v = call(p, v, step->children[0], NULL);
            else if (step->kind == MO_S_FIELD_ACCESS) {
                const struct mo_syntax *site = NULL;
                if (i + 1 < node->count && node->children[i + 1]->kind == MO_S_CALL)
                    site = node->children[i + 1];
                bool called;
                v = field_value(p, v, step->children[0], step, site, &called);
                if (called && site != NULL)
                    i++;
            } else {
                bool reported = infer_unfinished(p);
                (void)reported;
                return (struct value){0};
            }
            if (p->checked && v.type != NULL && i + 1 < node->count) {
                if (!infer_record(p, step, v))
                    return (struct value){0};
                v.note = p->last;
            }
        }
        return v;
    }
    case MO_S_LAMBDA: {
        const struct mo_syntax *parameters = node->children[0];
        struct term *fn =
            infer_make_type(p, (struct mo_source_type){.kind = MO_SOURCE_FUNCTION,
                                                       .name = "fn",
                                                       .module = UINT32_MAX,
                                                       .count = parameters->count + 1});
        if (fn == NULL)
            return (struct value){0};
        for (uint32_t i = 0; i < fn->shape.count; i++) {
            fn->arguments[i] = infer_fresh(p, 0);
            if (fn->arguments[i] == NULL)
                return (struct value){0};
        }
        if (expected != NULL && !infer_unify_as(p, fn, expected, p->node_expectation))
            return (struct value){0};
        struct value emitted = infer_graph(p, (struct value){.type = fn}, 1);
        if (emitted.type == NULL)
            return emitted;
        if (p->checked) {
            struct emitted_lambda *lambda =
                infer_allocate(p, 1, sizeof(*lambda), _Alignof(struct emitted_lambda));
            struct note **notes =
                infer_allocate(p, parameters->count, sizeof(*notes), _Alignof(struct note *));
            if (p->failed)
                return (struct value){0};
            *lambda = (struct emitted_lambda){.output = {.parameter_count = parameters->count},
                                              .parameters = notes,
                                              .parent = p->lambda_frame,
                                              .next = p->lambdas,
                                              .syntax = node,
                                              .loop = p->capture_loop,
                                              .path = p->capture_path,
                                              .depth = p->lambda + 1};
            p->lambdas = lambda;
            emitted.graph->lambda = lambda;
            p->lambda_count++;
        }
        struct local *mark = p->locals;
        struct term *old_result = p->result;
        struct emitted_lambda *old_frame = p->lambda_frame;
        if (p->checked)
            p->lambda_frame = emitted.graph->lambda;
        struct capture_loop *old_loop = p->capture_loop;
        p->capture_loop = NULL;
        uint32_t old_loops = p->loops;
        p->loops = 0;
        p->lambda++;
        p->result = fn->arguments[parameters->count];
        for (uint32_t i = 0; i < parameters->count; i++) {
            if (!bind(p, parameters->children[i], fn->arguments[i], false))
                return (struct value){0};
            if (p->checked)
                emitted.graph->lambda->parameters[i] = p->last->binding;
        }
        struct value body = infer_named(p->result, "Unit")
                                ? discard(p, node->children[1])
                                : infer_as(p, node->children[1], p->result, MO_CODE_RESULT_TYPE);
        if (body.type == NULL || !leave(p, mark))
            return (struct value){0};
        p->result = old_result;
        p->lambda_frame = old_frame;
        p->capture_loop = old_loop;
        p->loops = old_loops;
        p->lambda--;
        if (p->checked)
            emitted.graph->operands[0] = body.note;
        return emitted;
    }
    case MO_S_IF:
    case MO_S_LINE_IF:
        return conditional(p, node, expected);
    case MO_S_CASE:
        return match(p, node, expected);
    case MO_S_IS: {
        struct value input = infer_expression(p, node->children[0], NULL);
        struct mo_match_pattern *normalized = NULL;
        if (input.type == NULL || !pattern(p, node->children[1], input.type, 0, 1, &normalized))
            return (struct value){0};
        struct value out = infer_graph(
            p, (struct value){.type = infer_native(p, "Bool", 0), .never = input.never}, 1);
        if (p->checked && out.type != NULL)
            out.graph->operands[0] = input.note;
        return out;
    }
    case MO_S_RETURN: {
        struct local *mark = p->locals;
        struct value guard = {0};
        if (node->children[1] != NULL) {
            guard = condition(p, node->children[1], true);
            if (guard.type == NULL)
                return guard;
        }
        struct value value = node->children[0] == NULL
                                 ? (struct value){.type = infer_native(p, "Unit", 0)}
                                 : infer_as(p, node->children[0], p->result, MO_CODE_RESULT_TYPE);
        if (value.type == NULL || !infer_unify_as(p, value.type, p->result, MO_CODE_RESULT_TYPE) ||
            !leave(p, mark))
            return (struct value){0};
        uint32_t count =
            (node->children[0] == NULL ? 0u : 1u) + (node->children[1] == NULL ? 0u : 1u);
        struct value out = infer_graph(
            p,
            (struct value){.type = infer_native(p, "Unit", 0), .never = node->children[1] == NULL},
            count);
        if (p->checked && out.type != NULL) {
            if (guard.note != NULL)
                out.graph->operands[0] = guard.note;
            if (value.note != NULL)
                out.graph->operands[count - 1] = value.note;
        }
        return out;
    }
    case MO_S_STOP: {
        struct term *text = infer_native(p, "Text", 0);
        struct value reason =
            text == NULL ? (struct value){0} : infer_expression(p, node->children[0], text);
        if (reason.type == NULL)
            return (struct value){0};
        struct value out = infer_graph(
            p,
            (struct value){.type = expected == NULL ? infer_native(p, "Unit", 0) : expected,
                           .never = true},
            1);
        if (p->checked && out.type != NULL)
            out.graph->operands[0] = reason.note;
        return out;
    }
    case MO_S_ASSERT: {
        struct value test = condition(p, node->children[0], true);
        if (test.type == NULL)
            return (struct value){0};
        struct value out = infer_graph(p, (struct value){.type = infer_native(p, "Unit", 0)}, 1);
        if (p->checked && out.type != NULL)
            out.graph->operands[0] = test.note;
        return out;
    }
    case MO_S_FOR:
    case MO_S_WHILE: {
        struct local *mark = p->locals;
        struct note *binding = NULL;
        struct term *element = NULL;
        uint32_t body_index = node->kind == MO_S_FOR ? 2 : 1;
        struct capture_loop *old_loop = p->capture_loop;
        struct capture_loop *loop = NULL;
        if (p->checked) {
            loop = infer_allocate(p, 1, sizeof(*loop), _Alignof(struct capture_loop));
            if (loop == NULL)
                return (struct value){0};
            *loop = (struct capture_loop){node, old_loop};
            if (node->kind == MO_S_WHILE)
                p->capture_loop = loop;
        }
        struct value test = node->kind == MO_S_WHILE ? condition(p, node->children[0], true)
                                                     : infer_expression(p, node->children[1], NULL);
        if (test.type == NULL)
            return (struct value){0};
        if (node->kind == MO_S_WHILE) {
            struct term *boolean = infer_native(p, "Bool", 0);
            if (boolean == NULL || !infer_unify_as(p, test.type, boolean, MO_CODE_CONDITION_TYPE))
                return (struct value){0};
        } else {
            struct term *container = infer_root(test.type);
            if (infer_named(container, "Range"))
                element = infer_native(p, "Int", 0);
            else if (infer_named(container, "List") || infer_named(container, "Set"))
                element = infer_argument(p, container, 0);
            else if (infer_named(container, "Map")) {
                element = infer_make_type(p, (struct mo_source_type){.kind = MO_SOURCE_TUPLE,
                                                                     .name = "()",
                                                                     .module = UINT32_MAX,
                                                                     .count = 2});
                if (element != NULL) {
                    element->arguments[0] = infer_argument(p, container, 0);
                    element->arguments[1] = infer_argument(p, container, 1);
                    if (p->failed)
                        return (struct value){0};
                }
            } else {
                bool reported =
                    container->variable && container->numeric == 0
                        ? infer_unfinished(p)
                        : infer_problem(
                              p, MO_CODE_NOT_ITERABLE, node->children[body_index - 1]->span,
                              "for is given a value that can't be iterated",
                              "For walks a list, map, set or range (reference 7.5).", NULL);
                (void)reported;
            }
            if (element == NULL || !binder(p, node->children[0], element))
                return (struct value){0};
            const struct mo_syntax *name = node->children[0];
            while (name->kind == MO_S_BINDER)
                name = name->children[0];
            if (name->kind != MO_S_TUPLE)
                binding = p->last->binding;
        }
        p->capture_loop = loop;
        p->loops++;
        struct term *unit = infer_native(p, "Unit", 0);
        struct value body =
            unit == NULL ? (struct value){0} : discard(p, node->children[body_index]);
        p->loops--;
        p->capture_loop = old_loop;
        if (body.type == NULL || !leave(p, mark))
            return (struct value){0};
        struct value out = infer_graph(p, (struct value){.type = unit, .bound = binding}, 2);
        if (p->checked && out.type != NULL) {
            out.graph->operands[0] = test.note;
            out.graph->operands[1] = body.note;
            out.graph->pattern_type = element;
        }
        return out;
    }
    case MO_S_BREAK:
    case MO_S_CONTINUE: {
        if (p->loops == 0) {
            bool reported = infer_problem(
                p, MO_CODE_EXIT_OUTSIDE_LOOP, node->span, "break or continue is outside a loop",
                "Break leaves the loop and continue starts its next round "
                "(reference 7.5).",
                NULL);
            (void)reported;
            return (struct value){0};
        }
        struct value guard = {0};
        if (node->children[0] != NULL) {
            struct term *condition = infer_native(p, "Bool", 0);
            guard = condition == NULL
                        ? (struct value){0}
                        : infer_as(p, node->children[0], condition, MO_CODE_CONDITION_TYPE);
            if (guard.type == NULL)
                return (struct value){0};
        }
        struct value out = infer_graph(
            p,
            (struct value){.type = infer_native(p, "Unit", 0), .never = node->children[0] == NULL},
            guard.type == NULL ? 0u : 1u);
        if (p->checked && guard.type != NULL && out.type != NULL)
            out.graph->operands[0] = guard.note;
        return out;
    }
    case MO_S_LIST:
    case MO_S_TUPLE:
    case MO_S_MAP: {
        uint32_t arity = node->kind == MO_S_TUPLE ? node->count : node->kind == MO_S_MAP ? 2u : 1u;
        struct term *t = infer_make_type(
            p, (struct mo_source_type){.kind = node->kind == MO_S_TUPLE ? MO_SOURCE_TUPLE
                                                                        : MO_SOURCE_NAMED,
                                       .name = node->kind == MO_S_TUPLE ? "()"
                                               : node->kind == MO_S_MAP ? "Map"
                                                                        : "List",
                                       .module = UINT32_MAX,
                                       .count = arity});
        if (t == NULL)
            return (struct value){0};
        for (uint32_t i = 0; i < arity; i++) {
            t->arguments[i] = infer_fresh(p, 0);
            if (t->arguments[i] == NULL)
                return (struct value){0};
        }
        if (expected != NULL && !infer_unify_as(p, t, expected, p->node_expectation))
            return (struct value){0};
        struct value out = infer_graph(p, (struct value){.type = t},
                                       node->kind == MO_S_MAP ? 2 * node->count : node->count);
        if (out.type == NULL)
            return out;
        uint32_t *versions = NULL;
        if (node->kind == MO_S_MAP && node->count > 1) {
            versions = infer_allocate(p, node->count, sizeof(*versions), _Alignof(uint32_t));
            if (versions == NULL)
                return (struct value){0};
        }
        for (uint32_t i = 0; i < node->count; i++) {
            const struct mo_syntax *part = node->children[i];
            if (node->kind == MO_S_MAP) {
                const struct mo_syntax *key = part->children[0];
                while (key->kind == MO_S_GROUP)
                    key = key->children[0];
                struct name_slot *name = key->kind == MO_S_NAME ? slot(p, key, false) : NULL;
                if (versions != NULL)
                    versions[i] = name != NULL && name->active != NULL && name->active->variable
                                      ? name->active->version
                                      : 0;
                for (uint32_t j = 0; j < 2; j++) {
                    struct value v = infer_expression(p, part->children[j], t->arguments[j]);
                    if (v.type == NULL)
                        return (struct value){0};
                    if (p->checked)
                        out.graph->operands[2 * i + j] = v.note;
                    out.never = out.never || v.never;
                }
            } else {
                struct value v =
                    infer_expression(p, part, t->arguments[node->kind == MO_S_TUPLE ? i : 0]);
                if (v.type == NULL)
                    return (struct value){0};
                if (p->checked)
                    out.graph->operands[i] = v.note;
                out.never = out.never || v.never;
            }
        }
        if (node->kind == MO_S_MAP && !infer_map_keys(p, node, versions))
            return (struct value){0};
        return out;
    }
    case MO_S_TRY: {
        struct value input = infer_expression(p, node->children[0], NULL);
        p->span = (struct mo_span){node->span.file, node->span.start, node->span.start + 3};
        return propagate(p, input, expected);
    }
    case MO_S_NEGATE:
    case MO_S_NOT:
    case MO_S_PREFIX: {
        const struct mo_syntax *operand = node->children[node->count - 1];
        struct value v = infer_expression(p, operand, NULL);
        if (v.type == NULL)
            return v;
        bool negative =
            node->kind == MO_S_NEGATE ||
            (node->kind == MO_S_PREFIX && node->children[node->count - 2]->token == MO_T_MINUS);
        if (negative && (operand->kind == MO_S_INT || operand->kind == MO_S_DECIMAL)) {
            p->last->negative = true;
        }
        uint32_t count = node->kind == MO_S_PREFIX ? node->count - 1 : 1;
        for (uint32_t i = count; i != 0; i--) {
            enum mo_syntax_token op = node->kind == MO_S_PREFIX ? node->children[i - 1]->token
                                      : node->kind == MO_S_NOT  ? MO_T_NOT
                                                                : MO_T_MINUS;
            if (op == MO_T_TRY) {
                p->span = node->children[i - 1]->span;
                v = propagate(p, v, i == 1 ? expected : NULL);
                if (v.type == NULL)
                    return v;
                if (p->checked && i != 1) {
                    if (!infer_record(p, node->children[i - 1], v))
                        return (struct value){0};
                    v.note = p->last;
                }
                continue;
            }
            if (op == MO_T_MINUS && !p->checked && infer_named(v.type, "Duration")) {
                bool reported = infer_unfinished(p);
                (void)reported;
                return (struct value){0};
            }
            if (op == MO_T_MINUS && p->checked && infer_root(v.type)->variable &&
                infer_root(v.type)->numeric == 0 && infer_root(v.type)->deferred) {
                struct term *result = infer_fresh(p, 0);
                struct failure_check *pending =
                    infer_allocate(p, 1, sizeof(*pending), _Alignof(struct failure_check));
                if (result == NULL || pending == NULL)
                    return (struct value){0};
                result->deferred = true;
                *pending = (struct failure_check){.next = p->failures,
                                                  .input = v.type,
                                                  .value = result,
                                                  .op = MO_T_MINUS,
                                                  .code = i == 1 ? p->node_expectation : 0,
                                                  .span = node->span};
                p->failures = pending;
                v.type = result;
            } else {
                struct term *wanted =
                    op == MO_T_NOT ? infer_native(p, "Bool", 0)
                    : op == MO_T_MINUS
                        ? ((p->checked && infer_named(v.type, "Duration")) ||
                                   infer_root(v.type)->shape.kind == MO_SOURCE_PARAMETER
                               ? v.type
                               : infer_fresh(p, INT | BIG | DECIMAL | FLOAT))
                        : NULL;
                if (wanted == NULL) {
                    bool reported = infer_unfinished(p);
                    (void)reported;
                    return (struct value){0};
                }
                uint16_t code = op == MO_T_NOT                ? MO_CODE_CONDITION_TYPE
                                : infer_named(v.type, "Byte") ? MO_CODE_BYTE_ARITHMETIC
                                                              : MO_CODE_NUMBER_ABILITY;
                if (!infer_unify_as(p, v.type, wanted, code))
                    return (struct value){0};
            }
            if (p->checked) {
                struct note *input = v.note;
                v = infer_graph(p, (struct value){.type = v.type, .never = v.never}, 1);
                if (v.type == NULL)
                    return v;
                v.graph->operands[0] = input;
                v.graph->output.operation = op;
                if (i != 1) {
                    if (!infer_record(p, node->children[i - 1], v))
                        return (struct value){0};
                    v.note = p->last;
                }
            }
        }
        return v;
    }
    case MO_S_FILE:
    case MO_S_MODULE:
    case MO_S_EXPOSE:
    case MO_S_EXPOSE_ALL:
    case MO_S_USE:
    case MO_S_FN:
    case MO_S_PARAM:
    case MO_S_WHERE:
    case MO_S_STRUCT:
    case MO_S_FIELD:
    case MO_S_ENUM:
    case MO_S_CASE_DECL:
    case MO_S_TEST:
    case MO_S_CONST:
    case MO_S_SERVICE:
    case MO_S_SERVICE_STATE:
    case MO_S_SERVICE_TOOLS:
    case MO_S_SERVICE_EVERY:
    case MO_S_SERVICE_TOKEN:
    case MO_S_PROMISE:
    case MO_S_ANCHOR:
    case MO_S_EXAMPLES:
    case MO_S_TYPE:
    case MO_S_BINDER:
    case MO_S_ELSE:
    case MO_S_ARM:
    case MO_S_NAMED_ARG:
    case MO_S_UNDERSCORE:
    case MO_S_REST:
    case MO_S_MAP_ENTRY:
    case MO_S_TOKEN:
    case MO_S_IF_PART:
        break;
    }
    bool reported = infer_unfinished(p);
    (void)reported;
    return (struct value){0};
}

static MO_MUST_USE struct value infer_expression(struct inference *p, const struct mo_syntax *node,
                                                 struct term *expected) {
    if (!infer_work(p))
        return (struct value){0};
    MO_ASSERT(node != NULL && !node->incomplete);
    struct local *mark = p->locals;
    bool keep = p->keep_patterns;
    bool propagates = node->kind == MO_S_IS || node->kind == MO_S_GROUP ||
                      (node->kind == MO_S_BINARY && node->token == MO_T_AND) ||
                      (node->kind == MO_S_BINARY_CHAIN && node->children[1]->token == MO_T_AND);
    p->keep_patterns = node->kind == MO_S_GROUP && keep;
    p->span = node->span;
    if (p->depth == MO_LIMIT_SOURCE_INFERENCE_DEPTH) {
        bool reported = infer_problem(p, MO_CODE_CHECK_SYNTAX_DEPTH, node->span,
                                      "the body needs too many inference frames", NULL, NULL);
        (void)reported;
        return (struct value){0};
    }
    uint16_t code = p->expectation, saved = p->node_expectation;
    p->node_expectation = code;
    p->expectation = 0;
    p->depth++;
    if (p->checked && node->kind != MO_S_SEQUENCE && node->kind != MO_S_EXPR_STMT &&
        node->kind != MO_S_GROUP && node->kind != MO_S_INT && node->kind != MO_S_DECIMAL &&
        node->kind != MO_S_CHAR && node->kind != MO_S_TEXT && node->kind != MO_S_TRUE &&
        node->kind != MO_S_FALSE && node->kind != MO_S_NAME && node->kind != MO_S_VAR &&
        node->kind != MO_S_BIND && node->kind != MO_S_BINARY && node->kind != MO_S_BINARY_CHAIN &&
        node->kind != MO_S_RANGE && node->kind != MO_S_NEGATE && node->kind != MO_S_NOT &&
        node->kind != MO_S_PREFIX && node->kind != MO_S_CALL && node->kind != MO_S_POSTFIX &&
        node->kind != MO_S_LAMBDA && node->kind != MO_S_IF && node->kind != MO_S_LINE_IF &&
        node->kind != MO_S_CAPNAME && node->kind != MO_S_QUALIFIED &&
        node->kind != MO_S_FIELD_ACCESS && node->kind != MO_S_RETURN && node->kind != MO_S_STOP &&
        node->kind != MO_S_ASSERT && node->kind != MO_S_CASE && node->kind != MO_S_IS &&
        node->kind != MO_S_FOR && node->kind != MO_S_WHILE && node->kind != MO_S_BREAK &&
        node->kind != MO_S_CONTINUE && node->kind != MO_S_LIST && node->kind != MO_S_MAP &&
        node->kind != MO_S_TUPLE && node->kind != MO_S_POSITION && node->kind != MO_S_ADD_ASSIGN &&
        node->kind != MO_S_SUB_ASSIGN && node->kind != MO_S_TEXT_INTERPOLATED &&
        node->kind != MO_S_TRY) {
        bool reported = infer_unfinished(p);
        (void)reported;
        p->depth--;
        return (struct value){0};
    }
    struct value v = infer_node(p, node, expected);
    p->depth--;
    p->expectation = code;
    p->node_expectation = saved;
    p->keep_patterns = keep;
    p->span = node->span;
    if (v.type == NULL || (propagates && !keep && !leave(p, mark)) ||
        (expected != NULL && !v.never && !infer_unify(p, v.type, expected, 0)))
        return (struct value){0};
    if (p->checked && (node->kind == MO_S_EXPR_STMT || node->kind == MO_S_GROUP)) {
        struct note *operand = v.note;
        v = infer_graph(p, (struct value){.type = v.type, .never = v.never}, 1);
        if (v.type == NULL)
            return v;
        v.graph->operands[0] = operand;
    } else if (p->checked && v.graph == NULL)
        v = infer_graph(p, v, 0);
    if (v.type == NULL)
        return v;
    if (!infer_record(p, node, v))
        return (struct value){0};
    v.note = p->last;
    return v;
}

static MO_MUST_USE struct mo_inferred_body *
infer_function(struct mo_arena *arena, const struct mo_module_interfaces *interfaces,
               const struct mo_signatures *signatures, struct mo_constant_context *constants,
               const struct mo_function_signature *signature, struct mo_diags *diags,
               struct mo_checked_function *checked, bool *halt, struct mo_checked_spec *spec,
               struct mo_checked_condition *conditions) {
    MO_ASSERT(arena != NULL && interfaces != NULL && signatures != NULL && diags != NULL);
    MO_ASSERT(halt != NULL && signature != NULL && signatures->work != NULL);
    MO_ASSERT(*signatures->work <= MO_LIMIT_PROGRAM_CHECK_WORK);
    *halt = false;
    const struct mo_syntax *syntax = signature->function->declaration;
    const struct mo_syntax *written = spec == NULL ? syntax : spec->syntax;
    bool promise = spec != NULL && spec->kind == MO_SPEC_PROMISE;
    bool anchor = spec != NULL && spec->kind == MO_SPEC_ANCHOR;
    bool test = spec != NULL && spec->kind == MO_SPEC_TEST;
    bool service_promise = (promise || anchor) && spec->subject == UINT32_MAX;
    bool constant = syntax->kind == MO_S_CONST;
    bool service = syntax->kind == MO_S_SERVICE_STATE || syntax->kind == MO_S_SERVICE_EVERY;
    struct inference p = {.arena = arena,
                          .diags = diags,
                          .interfaces = interfaces,
                          .signatures = signatures,
                          .function = signature,
                          .constants = constants,
                          .checked = checked != NULL,
                          .spec = spec != NULL && !service_promise,
                          .span = written->span};
    p.abilities = (struct mo_ability_context){
        .arena = arena, .interfaces = interfaces, .signatures = signatures, .work = &p.work};
    bool finalized = false;
    if (checked == NULL && syntax->children[4]->count != 0) {
        p.span = syntax->children[4]->children[0]->span;
        bool reported = infer_unfinished(&p);
        (void)reported;
        goto failed;
    }
    struct value instance =
        anchor ? function_value(&p, signature->type, signature->requirements)
               : (struct value){.type = infer_annotation(&p, signature->type, NULL, 0)};
    struct term *fn = instance.type;
    if (fn == NULL)
        goto failed;
    p.result = constant       ? infer_fresh(&p, 0)
               : spec == NULL ? infer_argument(&p, fn, fn->shape.count - 1)
                              : infer_native(&p, test ? "Unit" : "Bool", 0);
    if (p.result == NULL)
        goto failed;
    const struct mo_syntax *parameters = test || constant || service ? NULL : syntax->children[1];
    uint32_t slots = service_promise ? fn->shape.count - 1 : fn->shape.count;
    uint32_t parameter_count = constant || service ? 0
                               : spec == NULL      ? parameters->count
                               : promise           ? slots
                                                   : 0;
    const struct mo_syntax **inputs = NULL;
    struct note **spec_notes = NULL;
    if (promise || anchor) {
        inputs = infer_allocate(&p, slots, sizeof(*inputs), _Alignof(const struct mo_syntax *));
        if (inputs == NULL ||
            !infer_anchor_inputs(&p, anchor ? written : written->children[2]->children[0],
                                 parameters, slots, inputs))
            goto failed;
        uint32_t count = anchor ? slots : written->children[1]->count;
        spec_notes = infer_allocate(&p, count, sizeof(*spec_notes), _Alignof(struct note *));
        if (spec_notes == NULL)
            goto failed;
    }
    struct note **parameter_notes =
        checked == NULL ? NULL
                        : infer_allocate(&p, parameter_count, sizeof(*parameter_notes),
                                         _Alignof(struct note *));
    if (checked != NULL && parameter_notes == NULL)
        goto failed;
    for (uint32_t i = 0; i < parameter_count; i++) {
        struct term *type = infer_argument(&p, fn, i);
        const struct mo_syntax *name =
            i < parameters->count ? parameters->children[i]->children[0] : inputs[i]->children[0];
        if (type == NULL || !bind(&p, name, type, false))
            goto failed;
        if (promise && p.locals != NULL)
            p.locals->used = true; /* A promise need not mention every subject parameter. */
        if (checked != NULL)
            parameter_notes[i] = p.last;
    }
    struct value v = {0};
    if (anchor) {
        const struct mo_syntax *values = written->children[0];
        for (uint32_t i = 0; i < values->count; i++) {
            uint32_t slot = 0;
            while (inputs[slot] != values->children[i]) {
                if (!infer_work(&p))
                    goto failed;
                slot++;
                MO_ASSERT(slot < slots);
            }
            v = infer_as(&p, inputs[slot]->children[1], infer_argument(&p, fn, slot),
                         MO_CODE_ARGUMENT_TYPE);
            if (v.type == NULL)
                goto failed;
            spec_notes[slot] = v.note;
        }
    } else if (promise) {
        const struct mo_syntax *lines = written->children[1];
        for (uint32_t i = 0; i < lines->count; i++) {
            struct value line = infer_as(&p, lines->children[i], p.result, MO_CODE_CONDITION_TYPE);
            if (line.type == NULL)
                goto failed;
            spec_notes[i] = line.note;
            if (i == 0)
                v = line;
            else {
                struct value both =
                    infer_graph(&p, (struct value){.type = p.result, .never = v.never}, 2);
                if (both.type == NULL)
                    goto failed;
                both.graph->output.operation = MO_T_AND;
                both.graph->operands[0] = v.note;
                both.graph->operands[1] = line.note;
                if (!infer_record(&p, written, both))
                    goto failed;
                both.note = p.last;
                v = both;
            }
        }
    } else if (spec != NULL)
        v = test ? discard(&p, written->children[1])
                 : infer_as(&p, written, p.result, MO_CODE_CONDITION_TYPE);
    else if (service)
        v = infer_as(&p, syntax->children[syntax->kind == MO_S_SERVICE_STATE ? 1 : 0], p.result,
                     MO_CODE_SERVICE_VALUE);
    else if (constant)
        v = infer_as(&p, syntax->children[1], p.result, MO_CODE_RESULT_TYPE);
    else
        v = infer_named(p.result, "Unit")
                ? discard(&p, syntax->children[5])
                : infer_as(&p, syntax->children[5], p.result, MO_CODE_RESULT_TYPE);
    if (v.type == NULL || !leave(&p, NULL))
        goto failed;
    if (!infer_finish_dispatches(&p) || !infer_finish_wrapping(&p))
        goto failed;
    struct mo_inferred_body *out =
        infer_allocate(&p, 1, sizeof(*out), _Alignof(struct mo_inferred_body));
    struct mo_inferred_expression *items =
        infer_allocate(&p, p.count, sizeof(*items), _Alignof(struct mo_inferred_expression));
    if (p.failed)
        goto failed;
    *out = (struct mo_inferred_body){.function = signature, .count = p.count, .items = items};
    finalized = true;
    bool valid = infer_finish_notes(&p, items);
    (void)valid;
    if (!p.fatal && !infer_finish_patterns(&p)) {
        *halt = true;
        mo_diags_sort(diags);
        return NULL;
    }
    if (!infer_finish_captures(&p))
        goto failed;
    if (p.error.code != 0)
        goto failed;
    if (checked != NULL) {
        struct mo_checked_expression *rows =
            infer_allocate(&p, p.count, sizeof(*rows), _Alignof(struct mo_checked_expression));
        struct mo_checked_binding *bindings = infer_allocate(&p, p.binding_count, sizeof(*bindings),
                                                             _Alignof(struct mo_checked_binding));
        const struct mo_checked_binding **parameter_bindings =
            infer_allocate(&p, parameter_count, sizeof(*parameter_bindings),
                           _Alignof(const struct mo_checked_binding *));
        struct note **lambdas =
            infer_allocate(&p, p.lambda_count, sizeof(*lambdas), _Alignof(struct note *));
        struct note **notes = infer_allocate(&p, p.count, sizeof(*notes), _Alignof(struct note *));
        if (p.failed)
            goto failed;
        uint32_t count = 0, binding_count = 0, lambda_count = 0, note_count = 0;
        for (struct note *n = p.notes; n != NULL; n = n->next) {
            notes[note_count++] = n;
            if (n->graph != NULL) {
                n->index = count++;
                if (n->graph->lambda != NULL)
                    lambdas[lambda_count++] = n;
            }
            if (n->binding != n)
                continue;
            const struct mo_syntax *name = n->output.syntax;
            uint32_t length = name->span.end - name->span.start;
            char *text = infer_allocate(&p, (size_t)length + 1, 1, 1);
            if (text == NULL)
                goto failed;
            memcpy(text, name->text, length);
            text[length] = '\0';
            n->binding_index = binding_count++;
            bindings[n->binding_index] = (struct mo_checked_binding){.syntax = name,
                                                                     .span = name->span,
                                                                     .name = text,
                                                                     .type = n->output.type,
                                                                     .variable = n->variable};
        }
        MO_ASSERT(binding_count == p.binding_count);
        MO_ASSERT(lambda_count == p.lambda_count);
        MO_ASSERT(note_count == p.count);
        qsort(notes, note_count, sizeof(*notes), infer_syntax_note_order);
        qsort(lambdas, lambda_count, sizeof(*lambdas), infer_source_note_order);
        for (uint32_t i = 0; i < lambda_count; i++)
            lambdas[i]->graph->lambda->output.number = i + 1;
        for (uint32_t i = 0; i < parameter_count; i++) {
            MO_ASSERT(parameter_notes[i]->binding_index < binding_count);
            parameter_bindings[i] = &bindings[parameter_notes[i]->binding_index];
        }
        for (struct note *n = p.notes; n != NULL; n = n->next) {
            if (n->graph == NULL)
                continue;
            struct emitted *g = n->graph;
            uint32_t *operands =
                infer_allocate(&p, g->output.operand_count, sizeof(*operands), _Alignof(uint32_t));
            if (operands == NULL)
                goto failed;
            for (uint32_t i = 0; i < g->output.operand_count; i++) {
                MO_ASSERT(g->operands[i] != NULL && g->operands[i]->index < n->index);
                operands[i] = g->operands[i]->index;
            }
            p.span = n->output.syntax->span;
            if (!mo_check_type_abilities(&p.abilities, n->output.type)) {
                bool reported = infer_problem(&p, p.abilities.error, p.span, p.abilities.message,
                                              p.abilities.reason, NULL);
                (void)reported;
                goto failed;
            }
            if (!infer_finish_operator(&p, n) || !infer_finish_function_use(&p, g) ||
                !infer_finish_interpolation(&p, n))
                goto failed;
            if (g->try_result != NULL) {
                struct mo_checked_try *try_ =
                    infer_allocate(&p, 1, sizeof(*try_), _Alignof(struct mo_checked_try));
                if (try_ == NULL)
                    goto failed;
                const struct mo_source_type *result = infer_finish_type(&p, g->try_result, 0);
                if (result == NULL)
                    goto failed;
                *try_ = (struct mo_checked_try){
                    .result = result,
                    .failure = {.builtin = infer_named(g->try_result, "Option") ? "None" : "Error"},
                    .wrapper = g->failure->wrapper};
                if (g->failure->generic != NULL &&
                    (!infer_type_arguments(&p, g->failure->generic, result->arguments[1],
                                           &try_->arguments) ||
                     !infer_data_requirements(&p, &try_->arguments, &try_->ability_count,
                                              &try_->abilities)))
                    goto failed;
                g->output.try_ = try_;
            }
            if (g->generic != NULL && g->generic->count != 0) {
                const struct mo_source_type *actual = infer_finish_type(&p, g->instantiated, 0);
                if (actual == NULL ||
                    !infer_type_arguments(&p, g->generic, actual, &g->output.arguments) ||
                    !infer_data_requirements(&p, &g->output.arguments, &g->output.ability_count,
                                             &g->output.abilities))
                    goto failed;
            }
            if (g->match != NULL) {
                struct emitted_match *match = g->match;
                match->output.scrutinee = operands[0];
                for (uint32_t i = 0; i < match->output.arm_count; i++) {
                    struct mo_checked_arm *arm = &match->arms[i];
                    if (arm->guard != UINT32_MAX)
                        arm->guard = operands[arm->guard];
                    arm->body = operands[arm->body];
                    const struct mo_checked_pattern **patterns =
                        infer_allocate(&p, arm->pattern_count, sizeof(*patterns),
                                       _Alignof(const struct mo_checked_pattern *));
                    if (patterns == NULL)
                        goto failed;
                    arm->patterns = patterns;
                    for (uint32_t j = 0; j < arm->pattern_count; j++) {
                        patterns[j] = infer_checked_pattern(
                            &p, arm->syntax->children[0]->children[j], g->operands[0]->output.type,
                            notes, note_count, bindings, binding_count, 0);
                        if (patterns[j] == NULL)
                            goto failed;
                    }
                }
                g->output.match = &match->output;
            } else if (n->output.syntax->kind == MO_S_IS) {
                g->output.pattern = infer_checked_pattern(&p, n->output.syntax->children[1],
                                                          g->operands[0]->output.type, notes,
                                                          note_count, bindings, binding_count, 0);
                if (g->output.pattern == NULL)
                    goto failed;
            }
            if (g->pattern_type != NULL) {
                const struct mo_source_type *type = infer_finish_type(&p, g->pattern_type, 0);
                if (type == NULL)
                    goto failed;
                g->output.pattern =
                    infer_checked_pattern(&p, n->output.syntax->children[0], type, notes,
                                          note_count, bindings, binding_count, 0);
                if (g->output.pattern == NULL)
                    goto failed;
            }
            if (g->lambda != NULL) {
                struct emitted_lambda *lambda = g->lambda;
                const struct mo_checked_binding **bound =
                    infer_allocate(&p, lambda->output.parameter_count, sizeof(*bound),
                                   _Alignof(const struct mo_checked_binding *));
                if (bound == NULL)
                    goto failed;
                for (uint32_t i = 0; i < lambda->output.parameter_count; i++) {
                    const struct note *parameter = lambda->parameters[i];
                    MO_ASSERT(parameter == NULL || parameter->binding_index < binding_count);
                    bound[i] = parameter == NULL ? NULL : &bindings[parameter->binding_index];
                }
                lambda->output.parameters = bound;
                const struct mo_checked_binding **captures =
                    infer_allocate(&p, lambda->output.capture_count, sizeof(*captures),
                                   _Alignof(const struct mo_checked_binding *));
                if (captures == NULL)
                    goto failed;
                uint32_t at = 0;
                for (const struct capture *c = lambda->captures; c != NULL; c = c->next) {
                    MO_ASSERT(c->local->binding->binding_index < binding_count);
                    captures[at++] = &bindings[c->local->binding->binding_index];
                }
                MO_ASSERT(at == lambda->output.capture_count);
                lambda->output.captures = captures;
                lambda->output.body = operands[0];
                g->output.lambda = &lambda->output;
            }
            rows[n->index] = g->output;
            rows[n->index].syntax = n->output.syntax;
            rows[n->index].type = n->output.type;
            rows[n->index].operands = operands;
            rows[n->index].never = n->output.never;
            if (g->output.receiver != UINT32_MAX) {
                MO_ASSERT(g->output.receiver < g->output.operand_count);
                rows[n->index].receiver = operands[g->output.receiver];
            }
            if (n->binding != NULL) {
                MO_ASSERT(n->binding->binding_index < binding_count);
                rows[n->index].binding = &bindings[n->binding->binding_index];
            }
        }
        if (promise) {
            MO_ASSERT(conditions != NULL);
            for (uint32_t i = 0; i < spec->condition_count; i++)
                conditions[i].expression = spec_notes[i]->index;
        }
        if (anchor) {
            uint32_t *values = infer_allocate(&p, slots, sizeof(*values), _Alignof(uint32_t));
            if (values == NULL || infer_finish_type(&p, fn, 0) == NULL ||
                !infer_finish_function_use(&p, instance.graph))
                goto failed;
            for (uint32_t i = 0; i < slots; i++)
                values[i] = spec_notes[i]->index;
            spec->value_count = slots;
            spec->values = values;
            spec->arguments = instance.graph->output.arguments;
            spec->ability_count = instance.graph->output.ability_count;
            spec->abilities = instance.graph->output.abilities;
        }
        *checked =
            (struct mo_checked_function){.signature = signature,
                                         .parameter_count = parameter_count,
                                         .parameters = parameter_bindings,
                                         .body = {.count = count,
                                                  .items = rows,
                                                  .root = anchor ? UINT32_MAX : v.note->index,
                                                  .capture_free_parameters = p.capture_free,
                                                  .binding_count = binding_count,
                                                  .bindings = bindings}};
    }
    return out;
failed:
    if (constants != NULL && constants->pending != NULL)
        return NULL; /* The explicit constant driver resumes after its dependency. */
    if (!p.fatal && !finalized) {
        p.failed = false;
        p.partial = true;
        bool partial_valid = infer_finish_notes(&p, NULL);
        (void)partial_valid;
        if (!p.fatal && !infer_finish_patterns(&p)) {
            *halt = true;
            mo_diags_sort(diags);
            return NULL;
        }
        if (!p.fatal) {
            bool captures_valid = infer_finish_captures(&p);
            (void)captures_valid;
        }
    }
    infer_emit_problem(&p);
    *halt = p.error.code >= 1000 || (diags->dropped != 0 && diags->count < MO_LIMIT_DIAGNOSTICS);
    mo_diags_sort(diags);
    return NULL;
}

struct mo_inferred_body *mo_infer_function(struct mo_arena *arena,
                                           const struct mo_module_interfaces *interfaces,
                                           const struct mo_signatures *signatures,
                                           uint32_t function, struct mo_diags *diags) {
    bool halt;
    MO_ASSERT(function < signatures->count);
    return infer_function(arena, interfaces, signatures, NULL, &signatures->items[function], diags,
                          NULL, &halt, NULL, NULL);
}

bool mo_check_function(struct mo_arena *arena, const struct mo_module_interfaces *interfaces,
                       const struct mo_signatures *signatures,
                       struct mo_constant_context *constants, uint32_t function,
                       struct mo_diags *diags, struct mo_checked_function *out, bool *halt) {
    MO_ASSERT(out != NULL && function < signatures->count);
    return infer_function(arena, interfaces, signatures, constants, &signatures->items[function],
                          diags, out, halt, NULL, NULL) != NULL;
}

bool mo_check_spec_body(struct mo_arena *arena, const struct mo_module_interfaces *interfaces,
                        const struct mo_signatures *signatures,
                        struct mo_constant_context *constants,
                        const struct mo_function_signature *signature, struct mo_checked_spec *spec,
                        struct mo_checked_condition *conditions, struct mo_diags *diags,
                        bool *halt) {
    MO_ASSERT(spec != NULL && spec->syntax != NULL);
    struct mo_checked_function checked;
    if (infer_function(arena, interfaces, signatures, constants, signature, diags, &checked, halt,
                       spec, conditions) == NULL)
        return false;
    spec->body = checked.body;
    spec->parameter_count = checked.parameter_count;
    spec->parameters = checked.parameters;
    return true;
}

bool mo_check_constant(struct mo_arena *arena, const struct mo_module_interfaces *interfaces,
                       const struct mo_signatures *signatures,
                       struct mo_constant_context *constants, struct mo_checked_constant *constant,
                       struct mo_diags *diags, bool *halt) {
    struct mo_source_type unit = {.kind = MO_SOURCE_NAMED, .name = "Unit", .module = UINT32_MAX};
    const struct mo_source_type *result = &unit;
    struct mo_source_type type = {.kind = MO_SOURCE_FUNCTION,
                                  .name = "fn",
                                  .count = 1,
                                  .module = UINT32_MAX,
                                  .arguments = &result,
                                  .height = 1};
    struct mo_function_signature context = {.function = constant->symbol, .type = &type};
    struct mo_checked_function checked;
    if (infer_function(arena, interfaces, signatures, constants, &context, diags, &checked, halt,
                       NULL, NULL) == NULL)
        return false;
    const struct mo_source_type *value = checked.body.items[checked.body.root].type;
    if (constant->symbol->name[constant->symbol->name_length - 1] == '?') {
        const struct mo_source_type *answer =
            value->kind == MO_SOURCE_FUNCTION ? value->arguments[value->count - 1] : value;
        if (answer->kind != MO_SOURCE_NAMED || answer->module != UINT32_MAX ||
            strcmp(answer->name, "Bool") != 0) {
            bool kept = mo_diags_add(
                diags, &(struct mo_diag){
                           .code = MO_CODE_BOOL_NAME,
                           .primary = {constant->symbol->declaration->children[0]->span,
                                       "must be Bool or a function returning Bool"},
                           .message = "a name ending in ? isn't a yes-or-no answer",
                           .reason = "A name ending in ? is a Bool, or a function that returns "
                                     "one (reference 2.3)."});
            (void)kept;
            *halt = diags->dropped != 0 && diags->count < MO_LIMIT_DIAGNOSTICS;
            return false;
        }
    }
    constant->initializer = checked.body;
    constant->type = value;
    return true;
}

bool mo_check_service_value(struct mo_arena *arena, const struct mo_module_interfaces *interfaces,
                            const struct mo_signatures *signatures,
                            struct mo_constant_context *constants, uint32_t module,
                            const struct mo_syntax *clause, const struct mo_source_type *expected,
                            struct mo_diags *diags, struct mo_checked_body *out) {
    MO_ASSERT(clause->kind == MO_S_SERVICE_STATE || clause->kind == MO_S_SERVICE_EVERY);
    struct mo_symbol owner = {.name = "service", .declaration = clause, .module = module};
    struct mo_source_type type = {.kind = MO_SOURCE_FUNCTION,
                                  .name = "fn",
                                  .module = UINT32_MAX,
                                  .count = 1,
                                  .arguments = &expected,
                                  .height = expected->height + 1};
    struct mo_function_signature signature = {.function = &owner, .type = &type};
    struct mo_checked_function checked;
    bool halt;
    if (infer_function(arena, interfaces, signatures, constants, &signature, diags, &checked, &halt,
                       NULL, NULL) == NULL)
        return false;
    *out = checked.body;
    return true;
}
