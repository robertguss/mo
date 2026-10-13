#include "lower/internal.h"

#include <string.h>

struct type_cache {
    struct type_cache *next;
    const struct mo_source_type *source;
    const struct substitution *substitution;
    mo_core_ty type;
};
struct canonical_type {
    struct canonical_type *next;
    mo_core_string name;
    mo_core_ty type;
};

static mo_core_ty cached(struct lowering *p, const struct mo_source_type *source,
                         const struct substitution *substitution) {
    for (struct type_cache *c = p->cache; c != NULL; c = c->next) {
        lower_work(p, 1);
        if (c->source == source && c->substitution == substitution)
            return c->type;
    }
    return NULL;
}
static void remember(struct lowering *p, const struct mo_source_type *source,
                     const struct substitution *substitution, mo_core_ty type) {
    struct type_cache *c = LOWER_NEW(p, struct type_cache, 1);
    *c = (struct type_cache){p->cache, source, substitution, type};
    p->cache = c;
}
mo_core_string lower_type_name(struct lowering *p, mo_core_ty type) {
    for (struct canonical_type *c = p->types; c != NULL; c = c->next) {
        lower_work(p, 1);
        if (c->type == type)
            return c->name;
    }
    MO_UNREACHABLE();
}
static bool same_parameter(const struct mo_source_type *a, const struct mo_source_type *b) {
    return a == b || (a->kind == MO_SOURCE_PARAMETER && b->kind == MO_SOURCE_PARAMETER &&
                      a->declaration == b->declaration && a->module == b->module &&
                      strcmp(a->name, b->name) == 0);
}

static const struct mo_data_signature *signature(struct lowering *p,
                                                 const struct mo_symbol *symbol) {
    for (uint32_t i = 0; i < p->program->signatures->data_count; i++) {
        lower_work(p, 1);
        if (p->program->signatures->data[i].symbol == symbol)
            return &p->program->signatures->data[i];
    }
    MO_UNREACHABLE();
}

static mo_core_string applied_name(struct lowering *p, mo_core_string name, uint32_t count,
                                   mo_core_ty const *types) {
    if (count == 0)
        return name;
    name = lower_concat(p, name, "(", lower_text(p, ""));
    for (uint32_t i = 0; i < count; i++)
        name = lower_concat(p, name, i == 0 ? "" : ", ", lower_type_name(p, types[i]));
    return lower_concat(p, name, ")", lower_text(p, ""));
}

static mo_core_ty make_type(struct lowering *p, const struct mo_source_type *source,
                            mo_core_ty *args) {
    mo_core_ty t = LOWER_NEW(p, struct mo_core_ty, 1);
    mo_core_string name;
    if (source->kind == MO_SOURCE_FUNCTION) {
        MO_ASSERT(source->count != 0);
        t->tag = MO_CORE_TY_FN;
        t->as.fn.params = (mo_core_ty_list){args, source->count - 1};
        t->as.fn.ret = args[source->count - 1];
        name = applied_name(p, lower_text(p, "fn"), source->count - 1, args);
        if (source->count == 1)
            name = lower_concat(p, name, "()", lower_text(p, ""));
        name = lower_concat(p, name, " : ", lower_type_name(p, t->as.fn.ret));
    } else if (source->kind == MO_SOURCE_TUPLE) {
        t->tag = MO_CORE_TY_TUPLE;
        t->as.tuple = (mo_core_ty_list){args, source->count};
        name = applied_name(p, lower_text(p, ""), source->count, args);
    } else {
        MO_ASSERT(source->kind == MO_SOURCE_NAMED);
        name = lower_text(p, source->name);
        if (source->module != UINT32_MAX) {
            MO_ASSERT(source->module < p->program->set->count);
            name =
                lower_concat(p, lower_text(p, p->program->set->names[source->module]), ".", name);
            t->tag = MO_CORE_TY_DATA;
        } else {
            static const struct {
                const char *name;
                enum mo_core_ty_tag tag;
            } names[] = {
                {"Int", MO_CORE_TY_INT},           {"BigInt", MO_CORE_TY_BIG_INT},
                {"Byte", MO_CORE_TY_BYTE},         {"Decimal", MO_CORE_TY_DECIMAL},
                {"Float", MO_CORE_TY_FLOAT},       {"Bool", MO_CORE_TY_BOOL},
                {"Char", MO_CORE_TY_CHAR},         {"Text", MO_CORE_TY_TEXT},
                {"Bytes", MO_CORE_TY_BYTES},       {"Unit", MO_CORE_TY_UNIT},
                {"Range", MO_CORE_TY_RANGE},       {"Time", MO_CORE_TY_TIME},
                {"Duration", MO_CORE_TY_DURATION}, {"List", MO_CORE_TY_LIST},
                {"Map", MO_CORE_TY_MAP},           {"Set", MO_CORE_TY_SET},
                {"Option", MO_CORE_TY_OPTION},     {"Result", MO_CORE_TY_RESULT},
                {"Ordering", MO_CORE_TY_ORDERING}, {"FileError", MO_CORE_TY_FILE_ERROR},
                {"Entry", MO_CORE_TY_ENTRY},       {"EntryKind", MO_CORE_TY_ENTRY_KIND},
            };
            bool found = false;
            for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
                if (strcmp(source->name, names[i].name) == 0) {
                    t->tag = names[i].tag;
                    found = true;
                    break;
                }
            if (!found) {
                static const char *const caps[] = {"Console", "Files", "ReadFiles", "Clock",
                                                   "Random",  "Args",  "Env"};
                for (size_t i = 0; i < sizeof caps / sizeof caps[0]; i++)
                    if (strcmp(source->name, caps[i]) == 0) {
                        t->tag = MO_CORE_TY_CAP;
                        t->as.cap = LOWER_NEW(p, struct mo_core_cap, 1);
                        t->as.cap->tag = (enum mo_core_cap_tag)i;
                        found = true;
                        break;
                    }
            }
            MO_ASSERT(found);
        }
        name = applied_name(p, name, source->count, args);
        if (t->tag == MO_CORE_TY_LIST)
            t->as.list = args[0];
        if (t->tag == MO_CORE_TY_MAP) {
            t->as.map.k = args[0];
            t->as.map.v = args[1];
        }
        if (t->tag == MO_CORE_TY_SET)
            t->as.set = args[0];
        if (t->tag == MO_CORE_TY_OPTION)
            t->as.option = args[0];
        if (t->tag == MO_CORE_TY_RESULT) {
            t->as.result.t = args[0];
            t->as.result.e = args[1];
        }
        if (t->tag == MO_CORE_TY_DATA)
            t->as.data = name;
    }
    for (struct canonical_type *c = p->types; c != NULL; c = c->next) {
        lower_work(p, 1 + name.length);
        if (lower_same(c->name, name))
            return c->type;
    }
    struct canonical_type *c = LOWER_NEW(p, struct canonical_type, 1);
    *c = (struct canonical_type){p->types, name, t};
    p->types = c;
    if (t->tag == MO_CORE_TY_DATA) {
        struct data_instance *d = LOWER_NEW(p, struct data_instance, 1);
        d->source = source;
        d->type = t;
        d->data.name = name;
        d->owner = source->module;
        if (source->count != 0) {
            const struct mo_symbol *symbol = source->declaration;
            MO_ASSERT(symbol != NULL);
            if (symbol->declaration->kind == MO_S_ENUM) {
                MO_ASSERT(symbol->case_count != 0);
                symbol = &p->program->interfaces->scopes[symbol->module].symbols[symbol->cases[0]];
            }
            const struct mo_source_type *abstract = signature(p, symbol)->type;
            MO_ASSERT(abstract->count == source->count);
            d->substitution.count = source->count;
            d->substitution.parameters = LOWER_NEW(p, const struct mo_source_type *, source->count);
            d->substitution.types = args;
            for (uint32_t i = 0; i < source->count; i++)
                d->substitution.parameters[i] = abstract->arguments[i];
        }
        *p->data_tail = d;
        p->data_tail = &d->next;
        p->data_count++;
    }
    return t;
}

struct type_frame {
    const struct mo_source_type *source;
    mo_core_ty *out, *arguments;
    uint32_t at;
};
mo_core_ty lower_type(struct lowering *p, const struct mo_source_type *type,
                      const struct substitution *substitution) {
    MO_ASSERT(type != NULL);
    mo_core_ty answer = cached(p, type, substitution);
    if (answer != NULL)
        return answer;
    struct type_frame *stack = LOWER_NEW(p, struct type_frame, MO_LIMIT_SOURCE_TYPE_DEPTH + 1);
    size_t depth = 1;
    stack[0] = (struct type_frame){.source = type, .out = &answer};
    while (depth != 0) {
        lower_work(p, 1);
        struct type_frame *f = &stack[depth - 1];
        mo_core_ty result = cached(p, f->source, substitution);
        if (result == NULL && f->source->kind == MO_SOURCE_PARAMETER) {
            MO_ASSERT(substitution != NULL);
            for (uint32_t i = 0; i < substitution->count; i++)
                if (same_parameter(f->source, substitution->parameters[i])) {
                    result = substitution->types[i];
                    break;
                }
            MO_ASSERT(result != NULL);
        }
        if (result == NULL && f->at < f->source->count) {
            if (f->arguments == NULL)
                f->arguments = LOWER_NEW(p, mo_core_ty, f->source->count);
            if (depth == MO_LIMIT_SOURCE_TYPE_DEPTH + 1)
                lower_refuse(p, MO_CODE_LOWER_DEPTH, "lowering exceeds its type path limit");
            uint32_t at = f->at++;
            stack[depth++] =
                (struct type_frame){.source = f->source->arguments[at], .out = &f->arguments[at]};
            continue;
        }
        if (result == NULL)
            result = make_type(p, f->source, f->arguments);
        *f->out = result;
        remember(p, f->source, substitution, result);
        depth--;
    }
    return answer;
}

struct instance *lower_instance(struct lowering *p, const struct mo_symbol *symbol,
                                struct mo_type_arguments arguments,
                                const struct substitution *substitution, uint32_t owner,
                                uint32_t depth) {
    MO_ASSERT(symbol != NULL);
    const struct mo_checked_function *source = NULL;
    for (uint32_t i = 0; i < p->program->function_count; i++) {
        lower_work(p, 1);
        if (p->program->functions[i].signature->function == symbol) {
            source = &p->program->functions[i];
            break;
        }
    }
    MO_ASSERT(source != NULL);
    uint32_t instance_owner = arguments.count == 0 ? symbol->module : owner;
    mo_core_ty *types = LOWER_NEW(p, mo_core_ty, arguments.count);
    for (uint32_t i = 0; i < arguments.count; i++)
        types[i] = lower_type(p, arguments.items[i].type, substitution);
    for (struct instance *f = p->functions; f != NULL; f = f->next) {
        lower_work(p, 1 + arguments.count);
        if (f->source != source || f->owner != instance_owner ||
            f->substitution.count != arguments.count)
            continue;
        uint32_t i = 0;
        while (i < arguments.count && f->substitution.types[i] == types[i])
            i++;
        if (i == arguments.count)
            return f;
    }
    if (depth > MO_LIMIT_INSTANTIATION_DEPTH)
        lower_refuse(p, MO_CODE_LOWER_DEPTH, "generic instantiation is nested too deeply");
    struct instance *f = LOWER_NEW(p, struct instance, 1);
    f->source = source;
    f->owner = instance_owner;
    f->depth = depth;
    f->index = p->function_count++;
    f->substitution.count = arguments.count;
    f->substitution.types = types;
    f->substitution.parameters = LOWER_NEW(p, const struct mo_source_type *, arguments.count);
    for (uint32_t i = 0; i < arguments.count; i++)
        f->substitution.parameters[i] = arguments.items[i].parameter;
    mo_core_string name = lower_concat(p, lower_text(p, p->program->set->names[symbol->module]),
                                       ".", lower_text(p, symbol->name));
    /* Overload ownership is a nominal receiver, independent of discovery. */
    if (symbol->receiver != UINT32_MAX) {
        const struct mo_module_scope *scope = &p->program->interfaces->scopes[symbol->module];
        const struct mo_symbol *group =
            mo_find_symbol(scope, (const uint8_t *)symbol->name, strlen(symbol->name));
        MO_ASSERT(group != NULL);
        if (group->overload_count > 1) {
            const struct mo_symbol *receiver = &scope->symbols[symbol->receiver];
            name = lower_concat(p, name, "@", lower_text(p, receiver->name));
        }
    }
    name = applied_name(p, name, arguments.count, types);
    if (arguments.count != 0)
        name = lower_concat(p, lower_text(p, p->program->set->names[owner]), "::", name);
    f->fun.name = name;
    f->meta.name = name;
    f->meta.span = lower_span(p, symbol->declaration->span);
    f->fun.module = lower_text(p, p->program->set->names[f->owner]);
    f->fun.const_count.digits = lower_text(p, "0");
    *p->function_tail = f;
    p->function_tail = &f->next;
    return f;
}

void lower_data(struct lowering *p, struct data_instance *d) {
    if (d->data.ctors.count != 0)
        return;
    const struct mo_symbol *type = d->source->declaration;
    MO_ASSERT(type != NULL);
    bool choice = type->declaration->kind == MO_S_ENUM;
    d->data.ctors.count = choice ? type->case_count : 1;
    d->data.ctors.items = LOWER_NEW(p, struct mo_core_ctor, d->data.ctors.count);
    for (size_t i = 0; i < d->data.ctors.count; i++) {
        const struct mo_symbol *symbol =
            choice ? &p->program->interfaces->scopes[type->module].symbols[type->cases[i]] : type;
        const struct mo_data_signature *sig = signature(p, symbol);
        struct mo_core_ctor *ctor = &d->data.ctors.items[i];
        ctor->name = lower_text(p, symbol->name);
        ctor->fields.count = sig->count;
        ctor->fields.items = LOWER_NEW(p, struct mo_core_param, sig->count);
        for (uint32_t j = 0; j < sig->count; j++) {
            const struct mo_field_signature *field = sig->by_index[j];
            ctor->fields.items[j].name =
                field->name == NULL ? lower_text(p, "0")
                                    : lower_copy(p, (const char *)field->name->text,
                                                 field->name->span.end - field->name->span.start);
            ctor->fields.items[j].ty = lower_type(p, field->type, &d->substitution);
        }
    }
}

mo_core_ctors lower_ctors(struct lowering *p, mo_core_ty type) {
    if (type->tag == MO_CORE_TY_DATA) {
        for (struct data_instance *d = p->data; d != NULL; d = d->next) {
            lower_work(p, 1);
            if (d->type == type) {
                lower_data(p, d);
                return d->data.ctors;
            }
        }
        MO_UNREACHABLE();
    }
    const char *names[4] = {0};
    uint32_t counts[4] = {0}, count = 0;
    mo_core_ty fields[4][2] = {{0}};
    static const struct mo_source_type text = {
        .kind = MO_SOURCE_NAMED, .name = "Text", .module = UINT32_MAX};
    static const struct mo_source_type entry_kind = {
        .kind = MO_SOURCE_NAMED, .name = "EntryKind", .module = UINT32_MAX};
    if (type->tag == MO_CORE_TY_OPTION) {
        count = 2;
        names[0] = "None";
        names[1] = "Some";
        counts[1] = 1;
        fields[1][0] = type->as.option;
    } else if (type->tag == MO_CORE_TY_RESULT) {
        count = 2;
        names[0] = "Ok";
        names[1] = "Error";
        counts[0] = counts[1] = 1;
        fields[0][0] = type->as.result.t;
        fields[1][0] = type->as.result.e;
    } else if (type->tag == MO_CORE_TY_ORDERING) {
        count = 3;
        names[0] = "Less";
        names[1] = "Equal";
        names[2] = "Greater";
    } else if (type->tag == MO_CORE_TY_ENTRY_KIND) {
        count = 4;
        names[0] = "File";
        names[1] = "Folder";
        names[2] = "Link";
        names[3] = "Other";
    } else if (type->tag == MO_CORE_TY_FILE_ERROR) {
        count = 4;
        names[0] = "NotFound";
        names[1] = "NotAllowed";
        names[2] = "NotText";
        names[3] = "Failed";
        for (uint32_t i = 0; i < count; i++) {
            counts[i] = i == 3 ? 2 : 1;
            fields[i][0] = fields[i][1] = lower_type(p, &text, NULL);
        }
    } else if (type->tag == MO_CORE_TY_ENTRY) {
        count = 1;
        names[0] = "Entry";
        counts[0] = 2;
        fields[0][0] = lower_type(p, &text, NULL);
        fields[0][1] = lower_type(p, &entry_kind, NULL);
    } else
        MO_UNREACHABLE();
    mo_core_ctors ctors = {.count = count, .items = LOWER_NEW(p, struct mo_core_ctor, count)};
    for (uint32_t i = 0; i < count; i++) {
        ctors.items[i].name = lower_text(p, names[i]);
        ctors.items[i].fields.count = counts[i];
        ctors.items[i].fields.items = LOWER_NEW(p, struct mo_core_param, counts[i]);
        for (uint32_t j = 0; j < counts[i]; j++) {
            ctors.items[i].fields.items[j].name = lower_number(p, j);
            ctors.items[i].fields.items[j].ty = fields[i][j];
        }
    }
    return ctors;
}
