#include "check/service-fixes.h"

#include "base/codes.h"
#include "base/limits.h"

#include <stdio.h>
#include <string.h>

struct proposal {
    struct mo_arena *arena;
    const struct mo_program *program;
    const struct mo_source *source;
    const struct mo_syntax *declaration;
    const char *handler;
    char helper[64], record[64];
    uint8_t *changes; /* source bytes: 0 unchanged, 1 renamed name, 2 moved documentation */
    uint64_t work;
    uint16_t error;
};

struct text {
    char *bytes;
    size_t length, limit;
    uint32_t column;
};

static MO_MUST_USE bool charge(struct proposal *p, size_t amount) {
    uint64_t *shared = p->program->signatures->work;
    if (amount > MO_LIMIT_SOURCE_CHECK_WORK - p->work) {
        p->error = MO_CODE_CHECK_WORK;
        return false;
    }
    if (amount > MO_LIMIT_PROGRAM_CHECK_WORK - *shared) {
        p->error = MO_CODE_CHECK_PROGRAM_WORK;
        return false;
    }
    p->work += amount;
    *shared += amount;
    return true;
}

static MO_MUST_USE void *allocate(struct proposal *p, size_t size) {
    if (!charge(p, size))
        return NULL;
    void *out = mo_arena_allocate(p->arena, size, 1);
    if (out == NULL)
        p->error = MO_CODE_CHECK_NO_MEMORY;
    return out;
}

static bool name_byte(uint8_t byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '_' || byte >= 0x80;
}

static void reserve_suffix(uint8_t *used, size_t count, const uint8_t *name, size_t length,
                           const char *prefix, size_t prefix_length) {
    if (length <= prefix_length || memcmp(name, prefix, prefix_length) != 0)
        return;
    size_t suffix = 0;
    for (size_t j = prefix_length; j < length; j++) {
        if (name[j] < '0' || name[j] > '9' || suffix > count / 10)
            return;
        suffix = suffix * 10 + (size_t)(name[j] - '0');
        if (suffix > count)
            return;
    }
    used[suffix] = 1;
}

/* Include local bindings as well as module names: a renamed reference must
 * never become captured. Counting names in comments too is harmless. */
static MO_MUST_USE bool fresh_name(struct proposal *p, const struct mo_module_scope *scope,
                                   const char *prefix, char out[64]) {
    size_t prefix_length = strlen(prefix);
    size_t count = p->source->length / (prefix_length + 1) + scope->imported_count;
    uint8_t *used = allocate(p, count + 1);
    if (used == NULL || !charge(p, p->source->length))
        return false;
    memset(used, 0, count + 1);
    for (size_t i = 0; i < p->source->length;) {
        if (!name_byte(p->source->bytes[i])) {
            i++;
            continue;
        }
        size_t start = i;
        while (i < p->source->length && name_byte(p->source->bytes[i]))
            i++;
        reserve_suffix(used, count, p->source->bytes + start, i - start, prefix, prefix_length);
    }
    for (uint32_t i = 0; i < scope->imported_count; i++) {
        const char *name = scope->imported[i].name;
        size_t length = strlen(name);
        if (!charge(p, length + 1))
            return false;
        reserve_suffix(used, count, (const uint8_t *)name, length, prefix, prefix_length);
    }
    size_t suffix = 0;
    while (used[suffix] != 0) {
        MO_ASSERT(suffix < count);
        suffix++;
    }
    int n = snprintf(out, 64, "%s%zu", prefix, suffix);
    MO_ASSERT(n > 0 && n < 64);
    return true;
}

static MO_MUST_USE bool append(struct proposal *p, struct text *out, const void *bytes,
                               size_t length) {
    if (length > out->limit - out->length) {
        p->error = MO_CODE_FILE_TOO_BIG;
        return false;
    }
    if (!charge(p, length))
        return false;
    const uint8_t *characters = bytes;
    for (size_t i = 0; i < length; i++) {
        if (characters[i] == '\n')
            out->column = 0;
        else if (characters[i] != '\r' && (characters[i] & 0xc0) != 0x80)
            out->column++;
        if (out->column > MO_LIMIT_LINE_LENGTH) {
            p->error = MO_CODE_LINE_TOO_LONG;
            return false;
        }
    }
    if (out->bytes != NULL)
        memcpy(out->bytes + out->length, bytes, length);
    out->length += length;
    return true;
}

static MO_MUST_USE bool literal(struct proposal *p, struct text *out, const char *value) {
    return append(p, out, value, strlen(value));
}

static MO_MUST_USE bool annotation(struct proposal *p, struct text *out,
                                   const struct mo_syntax *type) {
    if (type == NULL)
        return literal(p, out, "Unit");
    MO_ASSERT(type->span.file == p->declaration->span.file && type->span.start <= type->span.end &&
              type->span.end <= p->source->length);
    return append(p, out, p->source->bytes + type->span.start, type->span.end - type->span.start);
}

static MO_MUST_USE bool argument(struct proposal *p, struct text *out, uint32_t index) {
    char name[64];
    int n = snprintf(name, sizeof name, "service_argument%u", index);
    MO_ASSERT(n > 0 && (size_t)n < sizeof name);
    return append(p, out, name, (size_t)n);
}

static MO_MUST_USE bool wrapper(struct proposal *p, struct text *out, bool update) {
    const struct mo_syntax *parameters = p->declaration->children[1];
    const struct mo_syntax *result = p->declaration->children[2];
    if (!literal(p, out, "\n\n"))
        return false;
    if (!update && (!literal(p, out, "struct ") || !literal(p, out, p->record) ||
                    !literal(p, out, "\nvalue: ") || !annotation(p, out, result) ||
                    !literal(p, out, "\nend\n\n")))
        return false;
    for (const struct mo_syntax_comment *c = p->declaration->leading; c != NULL; c = c->next) {
        if (!charge(p, 1))
            return false;
        if (c->documentation &&
            (!append(p, out, c->text, c->span.end - c->span.start) || !literal(p, out, "\n")))
            return false;
    }
    if (!literal(p, out, "fn ") || !literal(p, out, p->handler) || !literal(p, out, "(\n"))
        return false;
    for (uint32_t i = 0; i < parameters->count; i++) {
        if (!argument(p, out, i) || !literal(p, out, ": ") ||
            !annotation(p, out, parameters->children[i]->children[1]) ||
            !literal(p, out, i + 1 == parameters->count ? "\n" : ",\n"))
            return false;
    }
    if (!literal(p, out, ") : "))
        return false;
    if (update) {
        if (!literal(p, out, "(") || !annotation(p, out, result) || !literal(p, out, ", Unit)\n("))
            return false;
    } else if (!literal(p, out, p->record) || !literal(p, out, "\n") ||
               !literal(p, out, p->record) || !literal(p, out, "(value: "))
        return false;
    if (!literal(p, out, p->helper) || !literal(p, out, "(\n"))
        return false;
    for (uint32_t i = 0; i < parameters->count; i++) {
        if (!argument(p, out, i) || !literal(p, out, i + 1 == parameters->count ? "\n" : ",\n"))
            return false;
    }
    return literal(p, out, update ? "), Unit)\nend\n" : "))\nend\n");
}

static MO_MUST_USE bool references(struct proposal *p, const struct mo_checked_body *body,
                                   const struct mo_symbol *symbol) {
    for (uint32_t i = 0; i < body->count; i++) {
        if (!charge(p, 1))
            return false;
        const struct mo_checked_expression *row = &body->items[i];
        if (row->symbol != symbol || row->operand_count != 0 ||
            row->type->kind != MO_SOURCE_FUNCTION)
            continue;
        const struct mo_syntax *name = row->syntax;
        if (name->kind == MO_S_POSTFIX)
            name = name->children[name->count - 1];
        if (name->kind == MO_S_FIELD_ACCESS || name->kind == MO_S_QUALIFIED)
            name = name->children[name->count - 1];
        MO_ASSERT(name->kind == MO_S_NAME && name->span.file == p->declaration->span.file &&
                  name->span.end <= p->source->length &&
                  name->span.end - name->span.start == symbol->name_length &&
                  memcmp(name->text, symbol->name, symbol->name_length) == 0);
        p->changes[name->span.start] = 1;
    }
    return true;
}

static MO_MUST_USE bool replacement(struct proposal *p, struct text *out, bool update) {
    if (!charge(p, p->source->length))
        return false;
    size_t length = strlen(p->handler);
    for (size_t i = 0; i < p->source->length;) {
        if (p->changes[i] == 1) {
            if (!literal(p, out, p->helper))
                return false;
            i += length;
        } else if (p->changes[i] == 2) {
            i++;
        } else {
            size_t start = i++;
            while (i < p->source->length && p->changes[i] == 0)
                i++;
            if (!append(p, out, p->source->bytes + start, i - start))
                return false;
        }
    }
    return wrapper(p, out, update);
}

bool mo_service_state_error(struct mo_arena *arena, const struct mo_program *program,
                            const struct mo_syntax *tool_name, uint32_t function,
                            struct mo_diags *diags) {
    MO_ASSERT(function < program->function_count);
    const struct mo_symbol *symbol = program->functions[function].signature->function;
    const struct mo_module_scope *scope = &program->interfaces->scopes[symbol->module];
    const struct mo_sources *sources = program->set->sources;
    struct proposal p = {.arena = arena,
                         .program = program,
                         .declaration = symbol->declaration,
                         .handler = symbol->name};
    struct mo_diag diagnostic = {
        .code = MO_CODE_SERVICE_STATE_RESULT,
        .primary = {tool_name->span, "a timer returns this; a tool returns (State, reply)"},
        .message = "a service tool returns only its state",
        .reason = "A tool changes the state only by returning (state, reply). Returning the state "
                  "alone is how a timer changes it, so a tool's result would never be saved "
                  "(reference 11.5).",
    };
    struct mo_edit edits[2];
    struct mo_fix fixes[2] = {
        {"return (state, Unit) to save the new state", &edits[0], 1},
        {"return the state inside a record, as read-only data", &edits[1], 1}};
    if (sources == NULL) {
        bool kept = mo_diags_add(diags, &diagnostic);
        (void)kept;
        return false;
    }
    MO_ASSERT(tool_name->span.file < sources->count);
    p.source = &sources->files[tool_name->span.file];
    MO_ASSERT(p.declaration->span.file == tool_name->span.file);
    struct text label = {.limit = MO_LIMIT_FILE_SIZE};
    for (uint32_t pass = 0; pass < 2; pass++) {
        if (!literal(&p, &label, "a timer returns this; a tool returns (") ||
            !annotation(&p, &label, p.declaration->children[2]) || !literal(&p, &label, ", reply)"))
            goto failed;
        if (pass == 0) {
            label.bytes = allocate(&p, label.length + 1);
            if (label.bytes == NULL)
                goto failed;
            label.length = 0;
            label.column = 0;
        }
    }
    label.bytes[label.length] = '\0';
    diagnostic.primary.text = label.bytes;
    if (symbol->exported || symbol->name[symbol->name_length - 1] == '?') {
        bool kept = mo_diags_add(diags, &diagnostic);
        (void)kept;
        return false;
    }
    if (scope->count > MO_LIMIT_MODULE_NAMES - 2) {
        p.error = MO_CODE_TOO_MANY_NAMES;
        goto failed;
    }
    const struct mo_source_type *signature = program->functions[function].signature->type;
    const struct mo_source_type *result = signature->arguments[signature->count - 1];
    /* The tuple result adds one level inside the adapter's function type. */
    if (result->height >= MO_LIMIT_SOURCE_TYPE_DEPTH - 2) {
        p.error = MO_CODE_CHECK_TYPE_DEPTH;
        goto failed;
    }
    char prefix[48];
    int n = snprintf(prefix, sizeof prefix, "service_tool_%u_", function);
    MO_ASSERT(n > 0 && (size_t)n < sizeof prefix);
    if (!fresh_name(&p, scope, prefix, p.helper))
        goto failed;
    n = snprintf(prefix, sizeof prefix, "ServiceReply%uCase", function);
    MO_ASSERT(n > 0 && (size_t)n < sizeof prefix);
    if (!fresh_name(&p, scope, prefix, p.record))
        goto failed;
    p.changes = allocate(&p, p.source->length);
    if (p.changes == NULL)
        goto failed;
    memset(p.changes, 0, p.source->length);
    p.changes[p.declaration->children[0]->span.start] = 1;
    for (const struct mo_syntax_comment *c = p.declaration->leading; c != NULL; c = c->next) {
        if (!charge(&p, c->span.end - c->span.start + 1))
            goto failed;
        if (c->documentation)
            memset(p.changes + c->span.start, 2, c->span.end - c->span.start);
    }
    for (uint32_t i = 0; i < program->function_count; i++)
        if (!charge(&p, 1) ||
            (program->functions[i].signature->function->module == symbol->module &&
             !references(&p, &program->functions[i].body, symbol)))
            goto failed;
    for (uint32_t i = 0; i < program->constant_count; i++)
        if (!charge(&p, 1) || (program->constants[i].symbol->module == symbol->module &&
                               !references(&p, &program->constants[i].initializer, symbol)))
            goto failed;
    for (uint32_t i = 0; i < program->spec_count; i++)
        if (!charge(&p, 1) || (program->specs[i].module == symbol->module &&
                               !references(&p, &program->specs[i].body, symbol)))
            goto failed;
    for (uint32_t i = 0; i < program->service_count; i++)
        if (!charge(&p, 1) || (program->services[i].module == symbol->module &&
                               !references(&p, &program->services[i].initializer, symbol)))
            goto failed;
    size_t program_base = sources->size - p.source->length;
    for (uint32_t i = 0; i < 2; i++) {
        struct text out = {.limit = MO_LIMIT_FILE_SIZE};
        if (!replacement(&p, &out, i == 0))
            goto failed;
        size_t length = out.length;
        if (length > MO_LIMIT_PROGRAM_SIZE - program_base) {
            p.error = MO_CODE_PROGRAM_TOO_BIG;
            goto failed;
        }
        out.bytes = allocate(&p, length + 1);
        if (out.bytes == NULL)
            goto failed;
        out.length = 0;
        out.column = 0;
        if (!replacement(&p, &out, i == 0))
            goto failed;
        MO_ASSERT(out.length == length);
        out.bytes[length] = '\0';
        edits[i] = (struct mo_edit){{tool_name->span.file, 0, p.source->length}, out.bytes};
    }
    diagnostic.fixes = fixes;
    diagnostic.fix_count = 2;
    {
        bool kept = mo_diags_add(diags, &diagnostic);
        (void)kept;
    }
    return false;

failed:;
    struct mo_diag failure = {.code = p.error,
                              .primary = {tool_name->span, NULL},
                              .message = "the service state fixes exceed checking resources"};
    bool kept = mo_diags_add(diags, &failure);
    (void)kept;
    return false;
}
