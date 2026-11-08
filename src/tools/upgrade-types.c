#include "tools/upgrade.h"
#include <string.h>

struct old_types {
    const struct mo_program *program;
    const struct mo_symbol **symbols;
    uint32_t count, capacity, owner;
};
static bool put(struct mo_buffer *out, const char *text) {
    return mo_buffer_append(out, text, strlen(text));
}
static bool collect_type(struct old_types *old, const struct mo_source_type *type, unsigned depth) {
    if (depth > MO_LIMIT_SOURCE_TYPE_DEPTH)
        return false;
    for (uint32_t i = 0; i < type->count; i++)
        if (!collect_type(old, type->arguments[i], depth + 1))
            return false;
    if (type->kind != MO_SOURCE_NAMED || type->declaration == NULL ||
        (old->program->set->library != NULL && old->program->set->library[type->module]))
        return true;
    for (uint32_t i = 0; i < old->count; i++)
        if (old->symbols[i] == type->declaration)
            return true;
    if (old->count == old->capacity)
        return false;
    old->symbols[old->count++] = type->declaration;
    for (uint32_t i = 0; i < old->program->signatures->data_count; i++) {
        const struct mo_data_signature *data = &old->program->signatures->data[i];
        if (data->type->declaration != type->declaration)
            continue;
        for (uint32_t j = 0; j < data->count; j++)
            if (!collect_type(old, data->by_index[j]->type, depth + 1))
                return false;
    }
    return true;
}
static bool module_name(struct mo_buffer *out, const struct old_types *old, uint32_t module) {
    if (old->program->set->library != NULL && old->program->set->library[module])
        return put(out, old->program->set->names[module]);
    return put(out, "UpgradeOld") &&
           (module == old->owner || (put(out, ".") && put(out, old->program->set->names[module])));
}
static bool type_text(struct mo_buffer *out, const struct old_types *old, uint32_t module,
                      const struct mo_source_type *type, unsigned depth) {
    if (depth > MO_LIMIT_SOURCE_TYPE_DEPTH || type->kind == MO_SOURCE_FUNCTION)
        return false;
    bool tuple = type->kind == MO_SOURCE_TUPLE;
    if (!tuple) {
        if (type->module != UINT32_MAX && type->module != module &&
            type->kind != MO_SOURCE_PARAMETER &&
            !(module_name(out, old, type->module) && put(out, ".")))
            return false;
        if (!put(out, type->name))
            return false;
    }
    if (type->count != 0 || tuple) {
        if (!put(out, "("))
            return false;
        for (uint32_t i = 0; i < type->count; i++)
            if ((i != 0 && !put(out, ", ")) ||
                !type_text(out, old, module, type->arguments[i], depth + 1))
                return false;
        if (!put(out, ")"))
            return false;
    }
    return true;
}
static void imports(const struct mo_source_type *type, uint32_t module, bool *needed) {
    if (type->module != UINT32_MAX && type->module != module && type->kind != MO_SOURCE_PARAMETER)
        needed[type->module] = true;
    for (uint32_t i = 0; i < type->count; i++)
        imports(type->arguments[i], module, needed);
}
static bool selected(const struct old_types *old, const struct mo_symbol *symbol) {
    for (uint32_t i = 0; i < old->count; i++)
        if (old->symbols[i] == symbol)
            return true;
    return false;
}
static bool fields(struct mo_buffer *out, const struct old_types *old, uint32_t module,
                   const struct mo_data_signature *data, bool record) {
    if (!record && data->count != 0 && !put(out, "("))
        return false;
    for (uint32_t i = 0; i < data->count; i++) {
        const struct mo_field_signature *field = data->by_index[i];
        if (!record && i != 0 && !put(out, ", "))
            return false;
        if (field->name != NULL &&
            (!mo_buffer_append(out, field->name->text,
                               field->name->span.end - field->name->span.start) ||
             !put(out, ": ")))
            return false;
        if (!type_text(out, old, module, field->type, 0) || (record && !put(out, "\n")))
            return false;
    }
    return record || data->count == 0 || put(out, ")");
}
static bool declarations(struct mo_buffer *out, const struct old_types *old, uint32_t module) {
    for (uint32_t i = 0; i < old->count; i++) {
        const struct mo_symbol *symbol = old->symbols[i];
        if (symbol->module != module)
            continue;
        const struct mo_data_signature *first = NULL;
        for (uint32_t j = 0; j < old->program->signatures->data_count; j++) {
            const struct mo_data_signature *data = &old->program->signatures->data[j];
            if (data->type->declaration == symbol) {
                first = data;
                break;
            }
        }
        if (first == NULL)
            return false;
        bool record = symbol->declaration->kind == MO_S_STRUCT;
        if (!put(out, record ? "struct " : "enum ") ||
            !type_text(out, old, module, first->type, 0) || !put(out, "\n"))
            return false;
        if (record) {
            if (!fields(out, old, module, first, true))
                return false;
        } else {
            const struct mo_module_scope *scope = &old->program->interfaces->scopes[module];
            for (uint32_t c = 0; c < symbol->case_count; c++) {
                const struct mo_symbol *case_symbol = &scope->symbols[symbol->cases[c]];
                const struct mo_data_signature *data = NULL;
                for (uint32_t j = 0; j < old->program->signatures->data_count; j++)
                    if (old->program->signatures->data[j].symbol == case_symbol)
                        data = &old->program->signatures->data[j];
                if (data == NULL || !put(out, case_symbol->name) ||
                    !fields(out, old, module, data, false) || !put(out, "\n"))
                    return false;
            }
        }
        if (!put(out, "end\n"))
            return false;
    }
    return true;
}
bool mo_upgrade_old_sources(const struct mo_program *program, struct mo_sources *candidate) {
    const struct mo_source_type *state = NULL;
    for (uint32_t i = 0; i < program->service_count; i++)
        if (program->services[i].module == 0)
            state = program->services[i].state;
    if (state == NULL)
        return false;
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_PROGRAM_CHECK_WORK);
    struct old_types old = {.program = program,
                            .capacity = program->signatures->data_count,
                            .owner = state->module == UINT32_MAX ? 0 : state->module};
    old.symbols = mo_arena_allocate_array(&arena, old.capacity, sizeof *old.symbols,
                                          _Alignof(const struct mo_symbol *));
    bool *needed =
        mo_arena_allocate_array(&arena, program->set->count, sizeof *needed, _Alignof(bool));
    bool ok = old.symbols != NULL && needed != NULL && collect_type(&old, state, 0);
    struct mo_buffer source, path;
    mo_buffer_init(&source, MO_LIMIT_FILE_SIZE);
    mo_buffer_init(&path, MO_LIMIT_PATH_SIZE);
    for (uint32_t module = 0; ok && module < program->set->count; module++) {
        bool used = module == old.owner;
        for (uint32_t i = 0; i < old.count; i++)
            used |= old.symbols[i]->module == module;
        if (!used)
            continue;
        source.length = 0;
        path.length = 0;
        memset(needed, 0, program->set->count * sizeof *needed);
        for (uint32_t i = 0; i < program->signatures->data_count; i++) {
            const struct mo_data_signature *data = &program->signatures->data[i];
            if (data->symbol->module == module && selected(&old, data->type->declaration))
                for (uint32_t f = 0; f < data->count; f++)
                    imports(data->fields[f].type, module, needed);
        }
        if (module == old.owner)
            imports(state, module, needed);
        ok = put(&source, "module ") && module_name(&source, &old, module) && put(&source, "\n");
        bool expose = false;
        for (uint32_t i = 0; ok && i < old.count; i++) {
            if (old.symbols[i]->module != module)
                continue;
            ok = put(&source, expose ? ", " : "expose ") && put(&source, old.symbols[i]->name) &&
                 put(&source, "(..)");
            expose = true;
        }
        if (expose)
            ok = ok && put(&source, "\n");
        for (uint32_t i = 0; ok && i < program->set->count; i++)
            if (needed[i])
                ok = put(&source, "use ") && module_name(&source, &old, i) && put(&source, "\n");
        ok = ok && declarations(&source, &old, module);
        if (ok && module == old.owner)
            ok = put(&source, "fn upgrade_input(value: ") &&
                 type_text(&source, &old, module, state, 0) && put(&source, ") : ") &&
                 type_text(&source, &old, module, state, 0) && put(&source, "\nvalue\nend\n");
        ok = ok && module_name(&path, &old, module) && put(&path, ".mo") &&
             mo_buffer_append_byte(&path, 0);
        uint32_t file;
        struct mo_source_error error;
        ok = ok && mo_sources_add_bytes(candidate, (const char *)path.bytes, source.bytes,
                                        source.length, &file, &error);
    }
    mo_buffer_free(&path);
    mo_buffer_free(&source);
    mo_arena_free(&arena);
    return ok;
}
