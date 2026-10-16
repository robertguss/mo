#include "check/signatures.h"
#include "check/check.h"
#include "check/checked.h"
#include "check/data.h"
#include "check/native.h"

#include "base/buffer.h"
#include "base/codes.h"
#include "base/limits.h"

#include <stdlib.h>
#include <string.h>

bool mo_use_import(const struct mo_signatures *signatures, uint32_t module,
                   const struct mo_syntax *origin) {
    uint32_t lo = 0, hi = signatures->import_count;
    while (lo < hi) {
        if (*signatures->work == MO_LIMIT_PROGRAM_CHECK_WORK)
            return false;
        (*signatures->work)++;
        uint32_t mid = lo + (hi - lo) / 2;
        const struct mo_import_usage *at = &signatures->imports[mid];
        if (at->module < module ||
            (at->module == module && at->origin->span.start < origin->span.start))
            lo = mid + 1;
        else
            hi = mid;
    }
    MO_ASSERT(lo < signatures->import_count && signatures->imports[lo].origin == origin);
    signatures->imports[lo].used = true;
    signatures->imports[signatures->imports[lo].parent].used = true;
    return true;
}

static MO_MUST_USE int symbol_order(const struct mo_symbol *a, const struct mo_symbol *b) {
    if (a->module != b->module)
        return a->module < b->module ? -1 : 1;
    return (uintptr_t)a < (uintptr_t)b ? -1 : (uintptr_t)a > (uintptr_t)b ? 1 : 0;
}

const struct mo_data_signature *mo_data_signature_by_symbol(const struct mo_signatures *signatures,
                                                            const struct mo_symbol *symbol) {
    uint32_t lo = 0, hi = signatures->data_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (symbol_order(signatures->data[mid].symbol, symbol) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < signatures->data_count && signatures->data[lo].symbol == symbol
               ? &signatures->data[lo]
               : NULL;
}

const struct mo_function_signature *
mo_function_signature_by_symbol(const struct mo_signatures *signatures,
                                const struct mo_symbol *symbol) {
    uint32_t lo = 0, hi = signatures->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (symbol_order(signatures->items[mid].function, symbol) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < signatures->count && signatures->items[lo].function == symbol
               ? &signatures->items[lo]
               : NULL;
}

const struct mo_nominal_signature *
mo_nominal_signature_by_symbol(const struct mo_signatures *signatures,
                               const struct mo_symbol *symbol) {
    uint32_t lo = 0, hi = signatures->nominal_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (symbol_order(signatures->nominals[mid].type->declaration, symbol) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < signatures->nominal_count && signatures->nominals[lo].type->declaration == symbol
               ? &signatures->nominals[lo]
               : NULL;
}

static MO_MUST_USE bool available(const struct mo_signatures *signatures,
                                  const struct mo_source_type *type) {
    if (*signatures->work == MO_LIMIT_PROGRAM_CHECK_WORK)
        return false;
    (*signatures->work)++;
    if (type->kind == MO_SOURCE_NAMED && type->declaration != NULL) {
        const struct mo_nominal_signature *owner =
            mo_nominal_signature_by_symbol(signatures, type->declaration);
        if (owner == NULL || owner->invalid)
            return false;
    }
    for (uint32_t i = 0; i < type->count; i++)
        if (!available(signatures, type->arguments[i]))
            return false;
    return true;
}

bool mo_signature_type_available(const struct mo_signatures *signatures,
                                 const struct mo_source_type *type) {
    return type != NULL && (!signatures->recovering || available(signatures, type));
}

static MO_MUST_USE int customizer_order(const void *a, const void *b) {
    const struct mo_function_signature *x = *(const struct mo_function_signature *const *)a;
    const struct mo_function_signature *y = *(const struct mo_function_signature *const *)b;
    uintptr_t left = (uintptr_t)x->type->arguments[0]->declaration;
    uintptr_t right = (uintptr_t)y->type->arguments[0]->declaration;
    return left < right ? -1 : left > right ? 1 : strcmp(x->function->name, y->function->name);
}

const struct mo_function_signature *mo_customizer_signature(const struct mo_signatures *signatures,
                                                            const struct mo_source_type *type,
                                                            enum mo_source_ability ability) {
    const char *name = ability == MO_SOURCE_TEXT ? "to_text" : "compare";
    uint32_t lo = 0, hi = signatures->customizer_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const struct mo_function_signature *at = signatures->customizers[mid];
        uintptr_t left = (uintptr_t)at->type->arguments[0]->declaration;
        uintptr_t right = (uintptr_t)type->declaration;
        int order = left < right ? -1 : left > right ? 1 : strcmp(at->function->name, name);
        if (order < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == signatures->customizer_count)
        return NULL;
    const struct mo_function_signature *out = signatures->customizers[lo];
    return out->type->arguments[0]->declaration == type->declaration &&
                   strcmp(out->function->name, name) == 0
               ? out
               : NULL;
}

static const struct mo_builtin_case builtin_cases[] = {
    {"Unit", "Unit", 6, 6, 1, 0, 0, 0, {NULL, NULL}},
    {"None", "Option", 8, 8, 2, 1, 0, 0, {NULL, NULL}},
    {"Some", "Option", 9, 8, 2, 1, 1, 0, {NULL, NULL}},
    {"Ok", "Result", 10, 10, 2, 2, 1, 0, {NULL, NULL}},
    {"Error", "Result", 11, 10, 2, 2, 1, 1, {NULL, NULL}},
    {"Less", "Ordering", 12, 12, 3, 0, 0, 0, {NULL, NULL}},
    {"Equal", "Ordering", 13, 12, 3, 0, 0, 0, {NULL, NULL}},
    {"Greater", "Ordering", 14, 12, 3, 0, 0, 0, {NULL, NULL}},
    {"File", "EntryKind", 15, 15, 4, 0, 0, 0, {NULL, NULL}},
    {"Folder", "EntryKind", 16, 15, 4, 0, 0, 0, {NULL, NULL}},
    {"Link", "EntryKind", 17, 15, 4, 0, 0, 0, {NULL, NULL}},
    {"Other", "EntryKind", 18, 15, 4, 0, 0, 0, {NULL, NULL}},
    {"NotFound", "FileError", 19, 19, 4, 0, 1, 0, {"path", NULL}},
    {"NotAllowed", "FileError", 20, 19, 4, 0, 1, 0, {"path", NULL}},
    {"NotText", "FileError", 21, 19, 4, 0, 1, 0, {"path", NULL}},
    {"Failed", "FileError", 22, 19, 4, 0, 2, 0, {"path", "reason"}},
    {"Entry", "Entry", 23, 23, 1, 0, 2, 0, {"name", "kind"}},
};

static const struct native_text {
    const char *module, *name, *qualified, *type;
    bool spec_only;
} native_texts[] = {
#define PRIM(id, module, name, signature, params, bugs)                                            \
    {module, name, module "." name, signature, false},
#include "base/prims.def"
#define HOSTOP(id, module, name, signature, params, bugs, spec_only)                               \
    {module, name, module "." name, signature, spec_only},
#include "base/hostops.def"
    {"List", "map", "List.map", "fn(List(T), fn(T) : U) : List(U)", false},
};

static const struct mo_symbol option_owner = {.name = "Option", .module = UINT32_MAX};
static const struct mo_symbol result_owner = {.name = "Result", .module = UINT32_MAX};
static const struct mo_source_type option_parameter = {.kind = MO_SOURCE_PARAMETER,
                                                       .name = "T",
                                                       .module = UINT32_MAX,
                                                       .declaration = &option_owner,
                                                       .parameters = UINT32_C(1) << 19};
static const struct mo_source_type result_parameters[] = {
    {.kind = MO_SOURCE_PARAMETER,
     .name = "T",
     .module = UINT32_MAX,
     .declaration = &result_owner,
     .parameters = UINT32_C(1) << 19},
    {.kind = MO_SOURCE_PARAMETER,
     .name = "E",
     .module = UINT32_MAX,
     .declaration = &result_owner,
     .parameters = UINT32_C(1) << 4},
};
static const struct mo_source_type *const option_arguments[] = {&option_parameter};
static const struct mo_source_type *const result_arguments[] = {&result_parameters[0],
                                                                &result_parameters[1]};
static const struct mo_source_type option_template = {.kind = MO_SOURCE_NAMED,
                                                      .name = "Option",
                                                      .module = UINT32_MAX,
                                                      .count = 1,
                                                      .arguments = option_arguments,
                                                      .height = 1,
                                                      .parameters = UINT32_C(1) << 19};
static const struct mo_source_type result_template = {
    .kind = MO_SOURCE_NAMED,
    .name = "Result",
    .module = UINT32_MAX,
    .count = 2,
    .arguments = result_arguments,
    .height = 1,
    .parameters = (UINT32_C(1) << 19) | (UINT32_C(1) << 4),
};

const struct mo_source_type *mo_builtin_template(const struct mo_builtin_case *data) {
    return strcmp(data->type, "Option") == 0   ? &option_template
           : strcmp(data->type, "Result") == 0 ? &result_template
                                               : NULL;
}

const struct mo_builtin_case *mo_builtin_case_by_name(const struct mo_syntax *name) {
    if (name->kind != MO_S_CAPNAME)
        return NULL;
    size_t length = name->span.end - name->span.start;
    for (size_t i = 0; i < sizeof(builtin_cases) / sizeof(builtin_cases[0]); i++)
        if (strlen(builtin_cases[i].name) == length &&
            memcmp(name->text, builtin_cases[i].name, length) == 0)
            return &builtin_cases[i];
    return NULL;
}

const struct mo_builtin_case *mo_builtin_case_by_key(uintptr_t key) {
    for (size_t i = 0; i < sizeof(builtin_cases) / sizeof(builtin_cases[0]); i++)
        if (builtin_cases[i].key == key)
            return &builtin_cases[i];
    return NULL;
}

const struct mo_builtin_case *mo_builtin_case_by_type(const char *name) {
    for (size_t i = 0; i < sizeof(builtin_cases) / sizeof(builtin_cases[0]); i++)
        if (strcmp(builtin_cases[i].type, name) == 0)
            return &builtin_cases[i];
    return NULL;
}

struct type_entry {
    struct mo_source_type type;
    struct type_entry *left;
    struct type_entry *right;
    uint32_t height;
};

struct preparation {
    struct mo_arena *arena;
    const struct mo_module_interfaces *interfaces;
    struct mo_diags *diags;
    const struct mo_symbol *function;
    const struct mo_source_type *parameters[26];
    struct type_entry *root;
    struct mo_signatures *signatures;
    struct mo_span span, native_span;
    uint32_t depth;
    bool failed, native, checked;
};

static MO_MUST_USE bool problem(struct preparation *p, uint16_t code, struct mo_span span,
                                const char *message, const char *reason) {
    if (p->native)
        span = p->native_span;
    const char *label = code == MO_CODE_TYPE_NAME        ? "not a type in scope"
                        : code == MO_CODE_TYPE_ARGUMENTS ? "type parameters don't match"
                        : code == MO_CODE_BOOL_NAME ? "must be Bool or a function returning Bool"
                        : code == MO_CODE_FIELD_REPEATED ? "this field is declared twice"
                        : code == MO_CODE_DATA_PARAMETERS
                            ? "type parameters must be distinct single letters"
                            : message;
    bool kept = mo_diags_add(p->diags, &(struct mo_diag){.code = code,
                                                         .primary = {span, label},
                                                         .message = message,
                                                         .reason = reason});
    p->failed = !kept;
    return false;
}

static MO_MUST_USE void *allocate(struct preparation *p, size_t count, size_t size,
                                  size_t alignment) {
    if (p->failed)
        return NULL;
    void *result = mo_arena_allocate_array(p->arena, count, size, alignment);
    if (result == NULL) {
        bool reported =
            problem(p, MO_CODE_CHECK_NO_MEMORY, p->span, "the checking arena has no room", NULL);
        (void)reported;
        p->failed = true;
    }
    return result;
}

static MO_MUST_USE int compare_types(const struct mo_source_type *a,
                                     const struct mo_source_type *b) {
    if (a->kind != b->kind)
        return a->kind < b->kind ? -1 : 1;
    if (a->module != b->module)
        return a->module < b->module ? -1 : 1;
    uintptr_t x = (uintptr_t)a->declaration, y = (uintptr_t)b->declaration;
    if (x != y)
        return x < y ? -1 : 1;
    int order = strcmp(a->name, b->name);
    if (order != 0)
        return order;
    if (a->count != b->count)
        return a->count < b->count ? -1 : 1;
    for (uint32_t i = 0; i < a->count; i++) {
        x = (uintptr_t)a->arguments[i];
        y = (uintptr_t)b->arguments[i];
        if (x != y)
            return x < y ? -1 : 1;
    }
    return 0;
}

static MO_MUST_USE uint32_t height(const struct type_entry *entry) {
    return entry == NULL ? 0 : entry->height;
}

static void refresh(struct type_entry *entry) {
    uint32_t a = height(entry->left), b = height(entry->right);
    entry->height = 1 + (a > b ? a : b);
}

static MO_MUST_USE struct type_entry *rotate(struct type_entry *entry, bool right) {
    struct type_entry *next = right ? entry->left : entry->right;
    if (right) {
        entry->left = next->right;
        next->right = entry;
    } else {
        entry->right = next->left;
        next->left = entry;
    }
    refresh(entry);
    refresh(next);
    return next;
}

static MO_MUST_USE struct type_entry *insert(struct type_entry *root, struct type_entry *entry) {
    if (root == NULL)
        return entry;
    int order = compare_types(&entry->type, &root->type);
    MO_ASSERT(order != 0);
    if (order < 0)
        root->left = insert(root->left, entry);
    else
        root->right = insert(root->right, entry);
    refresh(root);
    if (height(root->left) > height(root->right) + 1) {
        if (height(root->left->right) > height(root->left->left))
            root->left = rotate(root->left, false);
        return rotate(root, true);
    }
    if (height(root->right) > height(root->left) + 1) {
        if (height(root->right->left) > height(root->right->right))
            root->right = rotate(root->right, true);
        return rotate(root, false);
    }
    return root;
}

static MO_MUST_USE const struct mo_source_type *intern(struct preparation *p,
                                                       struct mo_source_type type) {
    struct type_entry *at = p->root;
    while (at != NULL) {
        int order = compare_types(&type, &at->type);
        if (order == 0)
            return &at->type;
        at = order < 0 ? at->left : at->right;
    }
    struct type_entry *entry = allocate(p, 1, sizeof(*entry), _Alignof(struct type_entry));
    char *name = allocate(p, strlen(type.name) + 1, 1, 1);
    const struct mo_source_type **arguments =
        type.count == 0
            ? NULL
            : allocate(p, type.count, sizeof(*arguments), _Alignof(const struct mo_source_type *));
    if (p->failed)
        return NULL;
    memcpy(name, type.name, strlen(type.name) + 1);
    if (type.count != 0)
        memcpy(arguments, type.arguments, (size_t)type.count * sizeof(*arguments));
    type.name = name;
    type.arguments = arguments;
    type.height = 0;
    type.parameters =
        type.kind == MO_SOURCE_PARAMETER ? UINT32_C(1) << (unsigned)(type.name[0] - 'A') : 0;
    for (uint32_t i = 0; i < type.count; i++) {
        uint32_t path = arguments[i]->height + 1;
        if (path > type.height)
            type.height = path;
        type.parameters |= arguments[i]->parameters;
    }
    *entry = (struct type_entry){.type = type, .height = 1};
    p->root = insert(p->root, entry);
    return &entry->type;
}

static MO_MUST_USE int compare_name(const struct mo_syntax *node, const char *name) {
    size_t length = node->span.end - node->span.start, other = strlen(name);
    size_t shared = length < other ? length : other;
    int order = memcmp(node->text, name, shared);
    return order != 0 ? order : length < other ? -1 : length > other ? 1 : 0;
}

static MO_MUST_USE const struct mo_symbol *lookup(const struct mo_module_scope *scope,
                                                  const struct mo_syntax *name) {
    return mo_find_symbol(scope, name->text, name->span.end - name->span.start);
}

static MO_MUST_USE const struct mo_symbol *definition(struct preparation *p,
                                                      const struct mo_syntax *name) {
    if (p->function->module == UINT32_MAX)
        return NULL;
    const struct mo_module_scope *scope = &p->interfaces->scopes[p->function->module];
    if (name->kind == MO_S_QUALIFIED) {
        const struct mo_syntax *prefix = name->children[0];
        uint32_t lo = 0, hi = scope->alias_count;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (compare_name(prefix, scope->aliases[mid].name) > 0)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo == scope->alias_count || compare_name(prefix, scope->aliases[lo].name) != 0)
            return NULL;
        const struct mo_symbol *symbol =
            lookup(&p->interfaces->scopes[scope->aliases[lo].module], name->children[1]);
        if (symbol != NULL && !symbol->exported) {
            bool reported = problem(p, MO_CODE_IMPORT_PRIVATE, name->span,
                                    "everything not on expose is private to the module",
                                    "Everything not on expose is private to the module "
                                    "(reference 10.1).");
            (void)reported;
            p->failed = true;
            return NULL;
        }
        if (symbol != NULL &&
            !mo_use_import(p->signatures, p->function->module, scope->aliases[lo].origin)) {
            bool reported = problem(p, MO_CODE_CHECK_PROGRAM_WORK, name->span,
                                    "the program needs too much import-checking work", NULL);
            (void)reported;
            p->failed = true;
            return NULL;
        }
        return symbol;
    }
    const struct mo_symbol *symbol = lookup(scope, name);
    if (symbol != NULL)
        return symbol;
    uint32_t lo = 0, hi = scope->imported_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (compare_name(name, scope->imported[mid].name) > 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == scope->imported_count || compare_name(name, scope->imported[lo].name) != 0)
        return NULL;
    if (!mo_use_import(p->signatures, p->function->module, scope->imported[lo].origin)) {
        bool reported = problem(p, MO_CODE_CHECK_PROGRAM_WORK, name->span,
                                "the program needs too much import-checking work", NULL);
        (void)reported;
        p->failed = true;
        return NULL;
    }
    return scope->imported[lo].symbols;
}

static MO_MUST_USE const struct mo_source_type *resolve(struct preparation *p,
                                                        const struct mo_syntax *node);

static MO_MUST_USE const struct mo_source_type *unit(struct preparation *p) {
    return intern(
        p, (struct mo_source_type){.kind = MO_SOURCE_NAMED, .name = "Unit", .module = UINT32_MAX});
}

static MO_MUST_USE const struct mo_source_type *resolve_type(struct preparation *p,
                                                             const struct mo_syntax *node) {
    if (node == NULL)
        return unit(p);
    p->span = node->span;
    if (node->token == MO_T_FN || node->token == MO_T_LPAREN) {
        const struct mo_syntax *sequence = node->children[0];
        uint32_t count = sequence->count + (node->token == MO_T_FN ? 1u : 0u);
        const struct mo_source_type **arguments =
            allocate(p, count, sizeof(*arguments), _Alignof(const struct mo_source_type *));
        if (arguments == NULL)
            return NULL;
        for (uint32_t i = 0; i < sequence->count; i++) {
            arguments[i] = resolve(p, sequence->children[i]);
            if (arguments[i] == NULL)
                return NULL;
        }
        if (node->token == MO_T_FN) {
            arguments[count - 1] = resolve(p, node->children[1]);
            if (arguments[count - 1] == NULL)
                return NULL;
        }
        return intern(p, (struct mo_source_type){.kind = node->token == MO_T_FN ? MO_SOURCE_FUNCTION
                                                                                : MO_SOURCE_TUPLE,
                                                 .name = node->token == MO_T_FN ? "fn" : "()",
                                                 .module = UINT32_MAX,
                                                 .count = count,
                                                 .arguments = arguments});
    }
    const struct mo_syntax *name = node->children[0], *args = node->children[1];
    uint32_t count = args == NULL ? 0 : args->count;
    if (name->kind == MO_S_CAPNAME && name->span.end - name->span.start == 1 &&
        p->parameters[name->text[0] - 'A'] != NULL) {
        if (count == 0)
            return p->parameters[name->text[0] - 'A'];
        bool reported = problem(p, MO_CODE_TYPE_ARGUMENTS, node->span,
                                "a type parameter is used as a generic type",
                                "A type parameter is a single capital letter, not a generic "
                                "type constructor (reference 9.1).");
        (void)reported;
        return NULL;
    }
    const struct mo_symbol *symbol = definition(p, name);
    if (p->failed)
        return NULL;
    struct mo_source_type type = {.kind = MO_SOURCE_NAMED, .module = UINT32_MAX, .count = count};
    uint32_t arity = 0;
    if (symbol != NULL && symbol->owner == UINT32_MAX &&
        (symbol->declaration->kind == MO_S_STRUCT || symbol->declaration->kind == MO_S_ENUM)) {
        type.name = symbol->name;
        type.module = symbol->module;
        type.declaration = symbol;
        const struct mo_syntax *parameters = symbol->declaration->children[1];
        arity = parameters == NULL ? 0 : parameters->count;
    } else if (symbol == NULL && name->kind == MO_S_CAPNAME) {
        static const struct {
            const char *name;
            uint32_t arity;
        } natives[] = {
            {"Bool", 0},     {"Int", 0},       {"BigInt", 0}, {"Byte", 0},      {"Decimal", 0},
            {"Float", 0},    {"Text", 0},      {"Char", 0},   {"Bytes", 0},     {"Unit", 0},
            {"List", 1},     {"Map", 2},       {"Set", 1},    {"Range", 0},     {"Time", 0},
            {"Duration", 0}, {"Option", 1},    {"Result", 2}, {"Ordering", 0},  {"Args", 0},
            {"Env", 0},      {"Console", 0},   {"Files", 0},  {"ReadFiles", 0}, {"Clock", 0},
            {"Random", 0},   {"FileError", 0}, {"Entry", 0},  {"EntryKind", 0}};
        for (size_t i = 0; i < sizeof(natives) / sizeof(natives[0]); i++)
            if (compare_name(name, natives[i].name) == 0) {
                type.name = natives[i].name;
                arity = natives[i].arity;
                break;
            }
        if (type.name == NULL && name->span.end - name->span.start == 1 &&
            p->function->declaration->kind == MO_S_FN) {
            if (count != 0) {
                bool reported = problem(p, MO_CODE_TYPE_ARGUMENTS, node->span,
                                        "a type parameter is used as a generic type",
                                        "A type parameter is a single capital letter, not a "
                                        "generic type constructor (reference 9.1).");
                (void)reported;
                return NULL;
            }
            char parameter[2] = {(char)name->text[0], '\0'};
            type.kind = MO_SOURCE_PARAMETER;
            type.name = parameter;
            type.module = p->function->module;
            type.declaration = p->function;
            return intern(p, type);
        }
    }
    if (type.name == NULL) {
        bool reported = problem(p, MO_CODE_TYPE_NAME, name->span, "a type name isn't in scope",
                                "Use brings a module's exposed type names into scope "
                                "(reference 10.3).");
        (void)reported;
        return NULL;
    }
    if (count != arity) {
        bool reported =
            problem(p, MO_CODE_TYPE_ARGUMENTS, node->span, "a type's type parameters don't match",
                    "List(T), Map(K, V) and declared generic types take type "
                    "parameters (reference 9.1).");
        (void)reported;
        return NULL;
    }
    const struct mo_source_type **arguments =
        count == 0
            ? NULL
            : allocate(p, count, sizeof(*arguments), _Alignof(const struct mo_source_type *));
    if (p->failed)
        return NULL;
    for (uint32_t i = 0; i < count; i++) {
        arguments[i] = resolve(p, args->children[i]);
        if (arguments[i] == NULL)
            return NULL;
    }
    type.arguments = arguments;
    return intern(p, type);
}

static MO_MUST_USE const struct mo_source_type *resolve(struct preparation *p,
                                                        const struct mo_syntax *node) {
    MO_ASSERT(p->depth < MO_LIMIT_NESTING_DEPTH);
    p->depth++;
    const struct mo_source_type *type = resolve_type(p, node);
    p->depth--;
    return type;
}

static MO_MUST_USE bool question_name(struct preparation *p, const struct mo_syntax *name,
                                      const struct mo_source_type *type) {
    if (name->text[name->span.end - name->span.start - 1] != '?')
        return true;
    if (type->kind == MO_SOURCE_FUNCTION)
        type = type->arguments[type->count - 1];
    if (type->kind == MO_SOURCE_NAMED && type->module == UINT32_MAX &&
        strcmp(type->name, "Bool") == 0)
        return true;
    return problem(p, MO_CODE_BOOL_NAME, name->span, "a name ending in ? isn't a yes-or-no answer",
                   "A name ending in ? is a Bool, or a function that returns one (reference 2.3).");
}

static MO_MUST_USE int compare_fields(const void *a, const void *b) {
    const struct mo_field_signature *x = a, *y = b;
    size_t n = x->name->span.end - x->name->span.start;
    size_t m = y->name->span.end - y->name->span.start;
    int order = memcmp(x->name->text, y->name->text, n < m ? n : m);
    if (order == 0)
        order = n < m ? -1 : n > m ? 1 : 0;
    return order != 0 ? order : x->index < y->index ? -1 : x->index > y->index ? 1 : 0;
}

static MO_MUST_USE int nominal_order(const void *a, const void *b) {
    const struct mo_nominal_signature *x = a, *y = b;
    return symbol_order(x->type->declaration, y->type->declaration);
}

static MO_MUST_USE bool nominal_index(struct preparation *p) {
    struct mo_signatures *s = p->signatures;
    s->nominals =
        allocate(p, s->data_count, sizeof(*s->nominals), _Alignof(struct mo_nominal_signature));
    if (s->nominals == NULL)
        return false;
    for (uint32_t i = 0; i < s->data_count; i++)
        s->nominals[i] = (struct mo_nominal_signature){
            .type = s->data[i].type, .field_count = s->data[i].count, .case_count = 1};
    qsort(s->nominals, s->data_count, sizeof(*s->nominals), nominal_order);
    for (uint32_t i = 0; i < s->data_count; i++) {
        struct mo_nominal_signature value = s->nominals[i];
        if (s->nominal_count != 0 &&
            s->nominals[s->nominal_count - 1].type->declaration == value.type->declaration) {
            MO_ASSERT(value.field_count <=
                      UINT32_MAX - s->nominals[s->nominal_count - 1].field_count);
            s->nominals[s->nominal_count - 1].field_count += value.field_count;
            s->nominals[s->nominal_count - 1].case_count++;
        } else
            s->nominals[s->nominal_count++] = value;
    }
    for (uint32_t i = 0; i < s->nominal_count; i++) {
        const struct mo_symbol *owner = s->nominals[i].type->declaration;
        uint32_t cases = owner->declaration->kind == MO_S_ENUM ? owner->case_count : 1;
        s->nominals[i].invalid = s->nominals[i].case_count != cases;
    }
    return true;
}

/* Nominal masks reach a fixed point without expanding recursive layouts. */
static MO_MUST_USE bool dependency_work(struct preparation *p) {
    if (*p->signatures->work == MO_LIMIT_PROGRAM_CHECK_WORK)
        return problem(p, MO_CODE_CHECK_PROGRAM_WORK, p->span,
                       "the program needs too much signature-checking work", NULL);
    (*p->signatures->work)++;
    return true;
}

static MO_MUST_USE bool dependencies(struct preparation *p, const struct mo_source_type *type,
                                     bool keys, uint32_t *out) {
    if (!dependency_work(p))
        return false;
    if (type->parameters == 0)
        return true;
    if (type->kind == MO_SOURCE_PARAMETER) {
        if (!keys)
            *out |= type->parameters;
        return true;
    }
    if (!keys && type->kind == MO_SOURCE_FUNCTION)
        return true; /* The ability check rejects it independently of parameters. */
    if (type->kind == MO_SOURCE_NAMED && type->declaration != NULL) {
        const struct mo_nominal_signature *owner =
            mo_nominal_signature_by_symbol(p->signatures, type->declaration);
        if (owner == NULL) {
            MO_ASSERT(p->signatures->recovering);
            return true;
        }
        uint32_t needed = keys ? owner->key_parameters : owner->hash_parameters;
        for (uint32_t i = 0; i < type->count; i++) {
            if ((needed & owner->type->arguments[i]->parameters) != 0 &&
                !dependencies(p, type->arguments[i], false, out))
                return false;
        }
    }
    if (type->kind == MO_SOURCE_NAMED && type->module == UINT32_MAX && keys &&
        (strcmp(type->name, "Map") == 0 || strcmp(type->name, "Set") == 0) &&
        !dependencies(p, type->arguments[0], false, out))
        return false;
    for (uint32_t i = 0; i < type->count; i++)
        if ((keys || type->declaration == NULL) && !dependencies(p, type->arguments[i], keys, out))
            return false;
    return true;
}

static MO_MUST_USE bool capture_dependencies(struct preparation *p,
                                             const struct mo_source_type *type,
                                             uint32_t *parameters, bool *capability) {
    if (!dependency_work(p))
        return false;
    if (type->kind == MO_SOURCE_PARAMETER) {
        *parameters |= type->parameters;
        return true;
    }
    if (type->kind == MO_SOURCE_FUNCTION)
        return true; /* Parameter/result types aren't values stored in a function. */
    if (type->kind == MO_SOURCE_NAMED && type->declaration != NULL) {
        const struct mo_nominal_signature *owner =
            mo_nominal_signature_by_symbol(p->signatures, type->declaration);
        if (owner == NULL) {
            MO_ASSERT(p->signatures->recovering);
            return true;
        }
        *capability = *capability || owner->holds_capability;
        for (uint32_t i = 0; i < type->count; i++)
            if ((owner->capture_parameters & owner->type->arguments[i]->parameters) != 0 &&
                !capture_dependencies(p, type->arguments[i], parameters, capability))
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
        if (!capture_dependencies(p, type->arguments[i], parameters, capability))
            return false;
    return true;
}

struct dependent {
    uint32_t data;
    struct dependent *next;
};

static MO_MUST_USE bool watch_dependencies(struct preparation *p, const struct mo_source_type *type,
                                           uint32_t data, struct dependent **watchers) {
    if (!dependency_work(p))
        return false;
    /* Closed recursive layouts can still acquire capability membership. */
    if (type->kind == MO_SOURCE_NAMED && type->declaration != NULL) {
        const struct mo_nominal_signature *owner =
            mo_nominal_signature_by_symbol(p->signatures, type->declaration);
        if (owner == NULL) {
            MO_ASSERT(p->signatures->recovering);
            return true;
        }
        uint32_t index = (uint32_t)(owner - p->signatures->nominals);
        struct dependent *edge = allocate(p, 1, sizeof(*edge), _Alignof(struct dependent));
        if (edge == NULL)
            return false;
        *edge = (struct dependent){data, watchers[index]};
        watchers[index] = edge;
    }
    for (uint32_t i = 0; i < type->count; i++)
        if (!watch_dependencies(p, type->arguments[i], data, watchers))
            return false;
    return true;
}

static MO_MUST_USE bool data_dependencies(struct preparation *p) {
    struct mo_signatures *s = p->signatures;
    if (s->data_count == 0)
        return true;
    struct dependent **watchers =
        allocate(p, s->nominal_count, sizeof(*watchers), _Alignof(struct dependent *));
    uint32_t *queue = allocate(p, s->data_count, sizeof(*queue), _Alignof(uint32_t));
    bool *pending = allocate(p, s->data_count, sizeof(*pending), _Alignof(bool));
    if (p->failed)
        return false;
    memset(watchers, 0, (size_t)s->nominal_count * sizeof(*watchers));
    for (uint32_t i = 0; i < s->data_count; i++) {
        queue[i] = i;
        pending[i] = true;
        for (uint32_t j = 0; j < s->data[i].count; j++)
            if (!watch_dependencies(p, s->data[i].fields[j].type, i, watchers))
                return false;
    }
    uint32_t head = 0, tail = 0, count = s->data_count;
    while (count != 0) {
        uint32_t index = queue[head];
        head = (head + 1) % s->data_count;
        count--;
        pending[index] = false;
        struct mo_data_signature *data = &s->data[index];
        p->span = data->symbol->declaration->span;
        const struct mo_nominal_signature *found =
            mo_nominal_signature_by_symbol(s, data->type->declaration);
        MO_ASSERT(found != NULL);
        uint32_t owner_index = (uint32_t)(found - s->nominals);
        struct mo_nominal_signature *owner = &s->nominals[owner_index];
        bool invalid = owner->invalid;
        uint32_t hash = data->hash_parameters, keys = data->key_parameters;
        uint32_t captures = owner->capture_parameters;
        bool capability = owner->holds_capability;
        for (uint32_t j = 0; j < data->count; j++) {
            if (!mo_signature_type_available(s, data->fields[j].type))
                owner->invalid = true;
            if (!dependencies(p, data->fields[j].type, false, &hash) ||
                !dependencies(p, data->fields[j].type, true, &keys) ||
                !capture_dependencies(p, data->fields[j].type, &captures, &capability))
                return false;
        }
        data->hash_parameters = hash;
        data->key_parameters = keys;
        if (owner->invalid == invalid && (hash & ~owner->hash_parameters) == 0 &&
            (keys & ~owner->key_parameters) == 0 && captures == owner->capture_parameters &&
            capability == owner->holds_capability)
            continue;
        owner->hash_parameters |= hash;
        owner->key_parameters |= keys;
        owner->capture_parameters = captures;
        owner->holds_capability = capability;
        for (const struct dependent *edge = watchers[owner_index]; edge != NULL;
             edge = edge->next) {
            if (!dependency_work(p))
                return false;
            if (!pending[edge->data]) {
                MO_ASSERT(count < s->data_count);
                pending[edge->data] = true;
                queue[tail] = edge->data;
                tail = (tail + 1) % s->data_count;
                count++;
            }
        }
    }
    return true;
}

static MO_MUST_USE bool requirements(struct preparation *p, const struct mo_source_type *type,
                                     uint8_t out[26]) {
    memset(out, 0, 26);
    uint32_t keys = 0;
    if (!dependencies(p, type, true, &keys))
        return false;
    for (unsigned i = 0; i < 26; i++)
        if ((keys & (UINT32_C(1) << i)) != 0)
            out[i] |= (uint8_t)(1u << MO_SOURCE_HASH);
    const struct mo_syntax *where = p->function->declaration->children[3];
    static const char *const names[] = {"Equal", "Hash", "Order", "Text", "Number"};
    for (uint32_t i = 0; where != NULL && i < where->count; i++) {
        const struct mo_syntax *name = where->children[i]->children[0];
        const struct mo_syntax *ability = where->children[i]->children[1];
        unsigned letter = (unsigned)(name->text[0] - 'A');
        if (name->span.end - name->span.start != 1 || letter >= 26 ||
            (type->parameters & (UINT32_C(1) << letter)) == 0)
            return problem(p, MO_CODE_ABILITY_REQUIREMENT, name->span,
                           "where doesn't name a type parameter in this signature",
                           "A single capital letter in a signature is a type parameter, and where "
                           "asks for abilities (reference 5.5).");
        unsigned mask = 0;
        for (unsigned j = 0; j < sizeof(names) / sizeof(names[0]); j++)
            if (compare_name(ability, names[j]) == 0)
                mask = 1u << j;
        if (mask == 0)
            return problem(
                p, MO_CODE_ABILITY_REQUIREMENT, ability->span,
                "where names an ability other than the five built in",
                "Where asks for one of Equal, Hash, Order, Text or Number (reference 5.5).");
        out[letter] |= (uint8_t)mask;
    }
    for (unsigned i = 0; i < 26; i++)
        if ((out[i] & ((1u << MO_SOURCE_HASH) | (1u << MO_SOURCE_ORDER))) != 0)
            out[i] |= (uint8_t)(1u << MO_SOURCE_EQUAL);
    return true;
}

static MO_MUST_USE bool customizer(struct preparation *p, const struct mo_source_type *type) {
    bool compare = strcmp(p->function->name, "compare") == 0;
    if (!compare && strcmp(p->function->name, "to_text") != 0)
        return true;
    bool valid = type->count == (compare ? 3u : 2u);
    const struct mo_source_type *receiver = valid ? type->arguments[0] : NULL;
    const struct mo_source_type *result = type->arguments[type->count - 1];
    const struct mo_module_scope *scope = &p->interfaces->scopes[p->function->module];
    bool library = receiver != NULL && receiver->module == UINT32_MAX &&
                   strcmp(receiver->name, "FileError") == 0 && scope->library &&
                   strcmp(scope->name, "FileError") == 0 && !compare;
    bool owned = receiver != NULL && receiver->declaration != NULL &&
                 receiver->module == p->function->module;
    valid = valid && receiver->kind == MO_SOURCE_NAMED && (owned || library) &&
            (!compare || type->arguments[1] == receiver) && result->kind == MO_SOURCE_NAMED &&
            result->module == UINT32_MAX &&
            strcmp(result->name, compare ? "Ordering" : "Text") == 0;
    uint32_t parameters = 0;
    for (uint32_t i = 0; valid && i < receiver->count; i++) {
        const struct mo_source_type *argument = receiver->arguments[i];
        valid = argument->kind == MO_SOURCE_PARAMETER && (parameters & argument->parameters) == 0;
        parameters |= argument->parameters;
    }
    if (valid && parameters == type->parameters)
        return true;
    return problem(p, MO_CODE_ABILITY_CUSTOMIZER, p->function->declaration->children[0]->span,
                   compare ? "compare doesn't have the type's order signature"
                           : "to_text doesn't have the type's text signature",
                   "A type customizes Order or Text in its own module with compare(T, T): Ordering "
                   "or to_text(T): Text (reference 9.2). This stage reserves these names for "
                   "customizers; it doesn't certify unrelated functions with these names.");
}

/* The table spells types in Mo. Add positional parameter names, then use the
 * real parser and annotation resolver; do not maintain a second type grammar.
 * Parsed token text belongs to the caller arena, not the temporary sources. */
static MO_MUST_USE bool native_signature(struct preparation *p, const struct native_text *text,
                                         struct mo_native_signature *out) {
    struct mo_buffer source;
    mo_buffer_init(&source, MO_LIMIT_FILE_SIZE);
    bool ok = mo_buffer_append_text(&source, "module Native\nfn interface(");
    const char *at = text->type;
    bool function = strncmp(at, "fn(", 3) == 0;
    if (function) {
        at += 3;
        unsigned parameter = 0;
        while (ok && *at != ')') {
            if (parameter != 0)
                ok = mo_buffer_append_text(&source, ", ");
            ok = ok && mo_buffer_append_text(&source, "_p") &&
                 mo_buffer_append_uint(&source, parameter++) &&
                 mo_buffer_append_text(&source, ": ");
            const char *first = at;
            uint32_t depth = 0;
            while (*at != '\0' && !(depth == 0 && (*at == ',' || *at == ')'))) {
                if (*at == '(')
                    depth++;
                else if (*at == ')')
                    depth--;
                at++;
            }
            MO_ASSERT(*at != '\0');
            ok = ok && mo_buffer_append(&source, first, (size_t)(at - first));
            if (*at == ',') {
                at++;
                while (*at == ' ')
                    at++;
            }
        }
        at++;
    }
    ok = ok && mo_buffer_append_text(&source, function ? ")" : ") : ") &&
         mo_buffer_append_text(&source, at) && mo_buffer_append_text(&source, "\nend\n");
    struct mo_sources sources = {0};
    struct mo_diags diags = {0};
    ok = ok && mo_sources_init(&sources) && mo_diags_init(&diags);
    uint32_t file = 0;
    struct mo_source_error error;
    ok = ok &&
         mo_sources_add_bytes(&sources, "native.mo", source.bytes, source.length, &file, &error);
    struct mo_module_syntax *module = ok ? mo_parse(p->arena, &sources, file, &diags) : NULL;
    if (module != NULL)
        MO_ASSERT(!mo_diags_any(&diags));
    mo_sources_free(&sources);
    mo_diags_free(&diags);
    mo_buffer_free(&source);
    if (module == NULL)
        return problem(p, MO_CODE_CHECK_NO_MEMORY, p->span, "the checking arena has no room", NULL);
    const struct mo_syntax *fn = module->tree->children[1]->children[0];
    *out = (struct mo_native_signature){
        .module = text->module,
        .name = text->name,
        .qualified = text->qualified,
        .spec_only = text->spec_only,
        .owner = {.name = text->qualified, .module = UINT32_MAX, .declaration = fn}};
    p->function = &out->owner;
    const struct mo_syntax *parameters = fn->children[1];
    const struct mo_source_type *arguments[MO_LIMIT_PARAMETERS + 1];
    for (uint32_t i = 0; i < parameters->count; i++) {
        arguments[i] = resolve(p, parameters->children[i]->children[1]);
        if (arguments[i] == NULL)
            return false;
    }
    arguments[parameters->count] = resolve(p, fn->children[2]);
    if (arguments[parameters->count] == NULL)
        return false;
    out->type = intern(p, (struct mo_source_type){.kind = MO_SOURCE_FUNCTION,
                                                  .name = "fn",
                                                  .module = UINT32_MAX,
                                                  .count = parameters->count + 1,
                                                  .arguments = arguments});
    if (out->type == NULL)
        return false;
    if (!function)
        out->type = arguments[0];
    return requirements(p, out->type, out->requirements);
}

static MO_MUST_USE bool data_signature(struct preparation *p, const struct mo_symbol *symbol,
                                       struct mo_data_signature *out) {
    const struct mo_symbol *owner =
        symbol->owner == UINT32_MAX ? symbol
                                    : &p->interfaces->scopes[symbol->module].symbols[symbol->owner];
    p->function = owner;
    p->span = symbol->declaration->span;
    memset(p->parameters, 0, sizeof(p->parameters));
    const struct mo_syntax *parameters = owner->declaration->children[1];
    uint32_t arity = parameters == NULL ? 0 : parameters->count;
    const struct mo_source_type *arguments[26];
    for (uint32_t i = 0; i < arity; i++) {
        const struct mo_syntax *name = parameters->children[i];
        if (name->span.end - name->span.start != 1 || arity > 26 ||
            p->parameters[name->text[0] - 'A'] != NULL)
            return problem(p, MO_CODE_DATA_PARAMETERS, name->span,
                           "a type parameter isn't a distinct single capital letter",
                           "Your own types take type parameters, which are single capital "
                           "letters (reference 9.1; names in reference 2.3).");
        char letter[2] = {(char)name->text[0], '\0'};
        arguments[i] = intern(p, (struct mo_source_type){.kind = MO_SOURCE_PARAMETER,
                                                         .name = letter,
                                                         .module = owner->module,
                                                         .declaration = owner});
        if (arguments[i] == NULL)
            return false;
        p->parameters[name->text[0] - 'A'] = arguments[i];
    }
    const struct mo_source_type *type = intern(p, (struct mo_source_type){.kind = MO_SOURCE_NAMED,
                                                                          .name = owner->name,
                                                                          .module = owner->module,
                                                                          .declaration = owner,
                                                                          .count = arity,
                                                                          .arguments = arguments});
    if (type == NULL)
        return false;
    const struct mo_syntax *fields =
        symbol->declaration->children[symbol->declaration->kind == MO_S_STRUCT ? 2 : 1];
    bool unnamed = fields != NULL && fields->kind == MO_S_TYPE;
    uint32_t count = fields == NULL ? 0 : unnamed ? 1u : fields->count;
    struct mo_field_signature *items =
        allocate(p, count, sizeof(*items), _Alignof(struct mo_field_signature));
    if (items == NULL)
        return false;
    *out = (struct mo_data_signature){.symbol = symbol,
                                      .type = type,
                                      .parameters = parameters,
                                      .count = count,
                                      .fields = items,
                                      .unnamed = unnamed};
    for (uint32_t i = 0; i < count; i++) {
        const struct mo_syntax *name = unnamed ? NULL : fields->children[i]->children[0];
        const struct mo_source_type *field_type =
            resolve(p, unnamed ? fields : fields->children[i]->children[1]);
        if (field_type == NULL || (name != NULL && !question_name(p, name, field_type)))
            return false;
        items[i] = (struct mo_field_signature){
            .name = name,
            .annotation = unnamed ? fields : fields->children[i]->children[1],
            .type = field_type,
            .index = i};
    }
    if (!unnamed) {
        qsort(items, count, sizeof(*items), compare_fields);
        for (uint32_t i = 1; i < count; i++) {
            const struct mo_syntax *a = items[i - 1].name, *b = items[i].name;
            size_t n = a->span.end - a->span.start, m = b->span.end - b->span.start;
            if (n == m && memcmp(a->text, b->text, n) == 0)
                return problem(p, MO_CODE_FIELD_REPEATED, b->span,
                               "a record or case declares the same field twice",
                               "Records and cases have named fields, each with one type "
                               "(reference 4).");
        }
    }
    out->by_index =
        allocate(p, count, sizeof(*out->by_index), _Alignof(const struct mo_field_signature *));
    if (out->by_index == NULL)
        return false;
    for (uint32_t i = 0; i < count; i++)
        out->by_index[items[i].index] = &items[i];
    return true;
}

struct mo_signatures *mo_prepare_available_signatures(struct mo_arena *arena,
                                                      const struct mo_module_set *set,
                                                      const struct mo_module_interfaces *interfaces,
                                                      struct mo_diags *diags, bool checked) {
    MO_ASSERT(arena != NULL && set != NULL && interfaces != NULL && diags != NULL);
    MO_ASSERT(interfaces->count == set->count && set->count <= MO_LIMIT_FILES);
    struct preparation p = {.arena = arena,
                            .interfaces = interfaces,
                            .diags = diags,
                            .span = {UINT32_MAX, 0, 0},
                            .checked = checked};
    uint32_t count = 0, data_count = 0;
    for (uint32_t i = 0; i < interfaces->count; i++)
        for (uint32_t j = 0; j < interfaces->scopes[i].count; j++) {
            enum mo_syntax_kind kind = interfaces->scopes[i].symbols[j].declaration->kind;
            if (kind == MO_S_FN)
                count++;
            if (kind == MO_S_STRUCT || kind == MO_S_CASE_DECL)
                data_count++;
        }
    struct mo_signatures *out = allocate(&p, 1, sizeof(*out), _Alignof(struct mo_signatures));
    struct mo_function_signature *items =
        allocate(&p, count, sizeof(*items), _Alignof(struct mo_function_signature));
    uint64_t *work = allocate(&p, 1, sizeof(*work), _Alignof(uint64_t));
    struct mo_data_signature *data =
        allocate(&p, data_count, sizeof(*data), _Alignof(struct mo_data_signature));
    if (p.failed)
        return NULL;
    *work = 0;
    *out = (struct mo_signatures){.items = items, .data = data, .work = work};
    p.signatures = out;
    uint32_t import_count = 0;
    for (uint32_t i = 0; i < set->count; i++) {
        const struct mo_syntax *uses = set->modules[i]->header.uses;
        for (uint32_t j = 0; j < uses->count; j++) {
            const struct mo_syntax *names = uses->children[j]->children[1];
            uint32_t added = 1 + (names == NULL ? 0 : names->count);
            if (added > MO_LIMIT_PROGRAM_CHECK_WORK - *work) {
                bool reported = problem(&p, MO_CODE_CHECK_PROGRAM_WORK, uses->children[j]->span,
                                        "the program needs too much import-checking work", NULL);
                (void)reported;
                return NULL;
            }
            *work += added;
            import_count += added;
        }
    }
    out->imports =
        allocate(&p, import_count, sizeof(*out->imports), _Alignof(struct mo_import_usage));
    if (out->imports == NULL)
        return NULL;
    for (uint32_t i = 0; i < set->count; i++) {
        const struct mo_syntax *uses = set->modules[i]->header.uses;
        for (uint32_t j = 0; j < uses->count; j++) {
            const struct mo_syntax *use = uses->children[j], *names = use->children[1];
            uint32_t parent = out->import_count;
            out->imports[out->import_count++] = (struct mo_import_usage){use, i, parent, false};
            for (uint32_t k = 0; names != NULL && k < names->count; k++)
                out->imports[out->import_count++] =
                    (struct mo_import_usage){names->children[k], i, parent, false};
        }
    }
    MO_ASSERT(out->import_count == import_count);
    for (uint32_t i = 0; i < interfaces->count; i++)
        for (uint32_t j = 0; j < interfaces->scopes[i].count; j++) {
            const struct mo_symbol *symbol = &interfaces->scopes[i].symbols[j];
            if (symbol->declaration->kind == MO_S_STRUCT ||
                symbol->declaration->kind == MO_S_CASE_DECL) {
                if (data_signature(&p, symbol, &out->data[out->data_count]))
                    out->data_count++;
                else {
                    if (diags->dropped != 0 || diags->count == 0 ||
                        diags->items[diags->count - 1].code >= 1000)
                        goto failed;
                    p.failed = false;
                    out->recovering = true;
                }
            }
        }
    if (!nominal_index(&p) || !data_dependencies(&p))
        goto failed;
    uint32_t before = diags->count;
    if (!mo_check_data_recursion(arena, out, diags)) {
        if (diags->dropped != 0 || diags->count == before ||
            diags->items[diags->count - 1].code != MO_CODE_DATA_RECURSION)
            goto failed;
        out->recovering = true;
        struct mo_span error = diags->items[diags->count - 1].primary.span;
        for (uint32_t i = 0; i < out->data_count; i++) {
            struct mo_span declaration = out->data[i].symbol->declaration->span;
            if (error.file == declaration.file && error.start >= declaration.start &&
                error.end <= declaration.end) {
                const struct mo_nominal_signature *owner =
                    mo_nominal_signature_by_symbol(out, out->data[i].type->declaration);
                MO_ASSERT(owner != NULL);
                out->nominals[(uint32_t)(owner - out->nominals)].invalid = true;
            }
        }
        if (!data_dependencies(&p))
            goto failed;
    }
    memset(p.parameters, 0, sizeof(p.parameters));
    for (uint32_t i = 0; i < interfaces->count; i++)
        for (uint32_t j = 0; j < interfaces->scopes[i].count; j++) {
            const struct mo_symbol *function = &interfaces->scopes[i].symbols[j];
            if (function->declaration->kind != MO_S_FN)
                continue;
            struct mo_function_signature *signature = &out->items[out->count++];
            *signature = (struct mo_function_signature){.function = function};
            p.function = function;
            p.span = function->declaration->span;
            const struct mo_syntax *parameters = function->declaration->children[1];
            const struct mo_source_type *arguments[MO_LIMIT_PARAMETERS + 1];
            MO_ASSERT(parameters->count <= MO_LIMIT_PARAMETERS);
            for (uint32_t k = 0; k < parameters->count; k++) {
                arguments[k] = resolve(&p, parameters->children[k]->children[1]);
                if (arguments[k] == NULL ||
                    !question_name(&p, parameters->children[k]->children[0], arguments[k]))
                    goto invalid_function;
            }
            arguments[parameters->count] = resolve(&p, function->declaration->children[2]);
            if (arguments[parameters->count] == NULL)
                goto invalid_function;
            const struct mo_source_type *type =
                intern(&p, (struct mo_source_type){.kind = MO_SOURCE_FUNCTION,
                                                   .name = "fn",
                                                   .module = UINT32_MAX,
                                                   .count = parameters->count + 1,
                                                   .arguments = arguments});
            if (type == NULL || !question_name(&p, function->declaration->children[0], type))
                goto invalid_function;
            if (!requirements(&p, type, signature->requirements) || !customizer(&p, type))
                goto invalid_function;
            signature->type = type;
            continue;
        invalid_function:
            if (diags->dropped != 0 || diags->count == 0 ||
                diags->items[diags->count - 1].code >= 1000)
                goto failed;
            p.failed = false;
            out->recovering = true;
        }
    MO_ASSERT(out->count == count);
    out->customizers = allocate(&p, count, sizeof(*out->customizers),
                                _Alignof(const struct mo_function_signature *));
    if (out->customizers == NULL)
        goto failed;
    for (uint32_t i = 0; i < count; i++)
        if (items[i].type != NULL && (strcmp(items[i].function->name, "to_text") == 0 ||
                                      strcmp(items[i].function->name, "compare") == 0))
            out->customizers[out->customizer_count++] = &items[i];
    qsort(out->customizers, out->customizer_count, sizeof(*out->customizers), customizer_order);
    out->native_count = (uint32_t)(sizeof(native_texts) / sizeof(native_texts[0]));
    out->native =
        allocate(&p, out->native_count, sizeof(*out->native), _Alignof(struct mo_native_signature));
    if (p.failed)
        goto failed;
    p.native = true;
    p.native_span = set->modules[0]->span;
    for (uint32_t i = 0; i < out->native_count; i++)
        if (!native_signature(&p, &native_texts[i], &out->native[i]))
            goto failed;
    mo_diags_sort(diags);
    return out;
failed:
    mo_diags_sort(diags);
    return NULL;
}

struct mo_signatures *mo_prepare_signatures(struct mo_arena *arena, const struct mo_module_set *set,
                                            const struct mo_module_interfaces *interfaces,
                                            struct mo_diags *diags) {
    struct mo_signatures *out =
        mo_prepare_available_signatures(arena, set, interfaces, diags, false);
    return mo_diags_any(diags) ? NULL : out;
}
