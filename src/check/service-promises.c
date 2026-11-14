#include "check/checked.h"

#include "base/codes.h"

#include <string.h>

static MO_MUST_USE bool refuse(struct mo_diags *diags, uint16_t code, struct mo_span span,
                               const char *message, const char *reason) {
    bool kept = mo_diags_add(
        diags, &(struct mo_diag){
                   .code = code, .primary = {span, message}, .message = message, .reason = reason});
    (void)kept;
    return false;
}

static MO_MUST_USE void *allocate(struct mo_arena *arena, struct mo_diags *diags,
                                  struct mo_span span, size_t count, size_t size,
                                  size_t alignment) {
    void *out = mo_arena_allocate_array(arena, count, size, alignment);
    if (out == NULL) {
        bool refused =
            refuse(diags, MO_CODE_CHECK_NO_MEMORY, span, "the checking arena has no room", NULL);
        (void)refused;
    }
    return out;
}

static MO_MUST_USE bool same_name(const struct mo_syntax *a, const char *text, size_t length) {
    return a->span.end - a->span.start == length && memcmp(a->text, text, length) == 0;
}

static MO_MUST_USE bool reserved(const struct mo_syntax *name) {
    static const char *const words[] = {"before", "after", "now", "reply"};
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++)
        if (same_name(name, words[i], strlen(words[i])))
            return true;
    return false;
}

static MO_MUST_USE struct mo_syntax *bound(struct mo_arena *arena, struct mo_diags *diags,
                                           struct mo_span at, const struct mo_syntax *declared,
                                           const char *word) {
    struct mo_syntax *n = allocate(arena, diags, at, 1, sizeof *n, _Alignof(struct mo_syntax));
    if (n != NULL && declared != NULL)
        *n = *declared;
    else if (n != NULL)
        *n = (struct mo_syntax){.kind = MO_S_NAME,
                                .span = {at.file, at.start, at.start + (uint32_t)strlen(word)},
                                .text = (const uint8_t *)word};
    return n;
}

static MO_MUST_USE const struct mo_source_type *
reply_type(struct mo_arena *arena, struct mo_diags *diags, struct mo_span span,
           const struct mo_checked_service_tool *tool, const struct mo_source_type *result) {
    switch (tool->reply) {
    case MO_SERVICE_READ_ONLY:
        return result;
    case MO_SERVICE_UPDATE:
        return result->arguments[1];
    case MO_SERVICE_RESULT_UPDATE:
        break;
    }
    const struct mo_source_type *ok = result->arguments[0]->arguments[1];
    const struct mo_source_type *error = result->arguments[1];
    const struct mo_source_type **arguments =
        allocate(arena, diags, span, 2, sizeof *arguments, _Alignof(const struct mo_source_type *));
    struct mo_source_type *type =
        allocate(arena, diags, span, 1, sizeof *type, _Alignof(struct mo_source_type));
    if (arguments == NULL || type == NULL)
        return NULL;
    arguments[0] = ok;
    arguments[1] = error;
    *type = *result;
    type->arguments = arguments;
    type->height = (ok->height > error->height ? ok->height : error->height) + 1;
    return type;
}

static MO_MUST_USE const struct mo_function_signature *
context(struct mo_arena *arena, const struct mo_program *program,
        const struct mo_checked_service *service, const struct mo_syntax *item, uint32_t tool,
        struct mo_diags *diags) {
    static const struct mo_source_type time = {
        .kind = MO_SOURCE_NAMED, .name = "Time", .module = UINT32_MAX};
    static const struct mo_source_type boolean = {
        .kind = MO_SOURCE_NAMED, .name = "Bool", .module = UINT32_MAX};
    struct mo_span at = item->span;
    const struct mo_source_type *handler = NULL;
    const struct mo_syntax *declared = NULL;
    uint32_t count = 3;
    if (tool != UINT32_MAX) {
        const struct mo_function_signature *signature =
            program->functions[service->tools[tool].function].signature;
        handler = signature->type;
        declared = signature->function->declaration->children[1];
        count += handler->count - 3 + 1;
    }
    struct mo_syntax **names =
        allocate(arena, diags, at, count, sizeof *names, _Alignof(struct mo_syntax *));
    const struct mo_source_type **types = allocate(
        arena, diags, at, (size_t)count + 1, sizeof *types, _Alignof(struct mo_source_type *));
    struct mo_syntax *parameters =
        allocate(arena, diags, at, count + 2, sizeof *parameters, _Alignof(struct mo_syntax));
    struct mo_syntax **children =
        allocate(arena, diags, at, count + 6, sizeof *children, _Alignof(struct mo_syntax *));
    struct mo_source_type *type =
        allocate(arena, diags, at, 1, sizeof *type, _Alignof(struct mo_source_type));
    struct mo_symbol *owner =
        allocate(arena, diags, at, 1, sizeof *owner, _Alignof(struct mo_symbol));
    struct mo_function_signature *out =
        allocate(arena, diags, at, 1, sizeof *out, _Alignof(struct mo_function_signature));
    if (names == NULL || types == NULL || parameters == NULL || children == NULL || type == NULL ||
        owner == NULL || out == NULL)
        return NULL;
    names[0] = bound(arena, diags, at, NULL, "before");
    names[1] = bound(arena, diags, at, NULL, "after");
    names[2] = bound(arena, diags, at, NULL, "now");
    types[0] = types[1] = service->state;
    types[2] = &time;
    if (tool != UINT32_MAX) {
        for (uint32_t i = 2; i + 1 < handler->count; i++) {
            names[i + 1] = bound(arena, diags, at, declared->children[i]->children[0], NULL);
            types[i + 1] = handler->arguments[i];
        }
        names[count - 1] = bound(arena, diags, at, NULL, "reply");
        types[count - 1] = reply_type(arena, diags, at, &service->tools[tool],
                                      handler->arguments[handler->count - 1]);
        if (types[count - 1] == NULL)
            return NULL;
    }
    types[count] = &boolean;
    uint32_t height = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (names[i] == NULL)
            return NULL;
        if (types[i]->height > height)
            height = types[i]->height;
        parameters[i] = (struct mo_syntax){.kind = MO_S_PARAM, .span = names[i]->span, .count = 1};
        parameters[i].children = &children[i];
        children[i] = names[i];
    }
    struct mo_syntax *list = &parameters[count];
    *list = (struct mo_syntax){.kind = MO_S_SEQUENCE, .span = at, .count = count};
    struct mo_syntax **items =
        allocate(arena, diags, at, count, sizeof *items, _Alignof(struct mo_syntax *));
    if (items == NULL)
        return NULL;
    for (uint32_t i = 0; i < count; i++)
        items[i] = &parameters[i];
    list->children = items;
    struct mo_syntax *declaration = &parameters[count + 1];
    struct mo_syntax **parts = &children[count];
    parts[0] = parts[2] = parts[3] = parts[4] = parts[5] = NULL;
    parts[1] = list;
    *declaration = (struct mo_syntax){.kind = MO_S_FN, .span = at, .count = 6, .children = parts};
    *type = (struct mo_source_type){.kind = MO_SOURCE_FUNCTION,
                                    .name = "fn",
                                    .module = UINT32_MAX,
                                    .count = count + 1,
                                    .arguments = types,
                                    .height = height + 1};
    *owner = (struct mo_symbol){.name = "service",
                                .declaration = declaration,
                                .owner = UINT32_MAX,
                                .receiver = UINT32_MAX,
                                .name_length = 7,
                                .module = service->module};
    *out = (struct mo_function_signature){.function = owner, .type = type};
    return out;
}

static MO_MUST_USE uint32_t named_tool(const struct mo_checked_service *service,
                                       const struct mo_syntax *name) {
    for (uint32_t i = 0; i < service->tool_count; i++) {
        const struct mo_syntax *tool = service->tools[i].name;
        if (same_name(name, (const char *)tool->text, tool->span.end - tool->span.start))
            return i;
    }
    return UINT32_MAX;
}

static MO_MUST_USE bool check_promise(struct mo_arena *arena, const struct mo_program *program,
                                      struct mo_constant_context *constants,
                                      const struct mo_checked_service *service,
                                      const struct mo_syntax *item, uint32_t number,
                                      struct mo_checked_service_promise *out,
                                      struct mo_diags *diags, bool *halt) {
    *halt = true;
    const struct mo_sources *sources = program->set->sources;
    const struct mo_syntax *on = item->children[3];
    uint32_t tool = on == NULL ? UINT32_MAX : named_tool(service, on);
    if (on != NULL && tool == UINT32_MAX) {
        *halt = false;
        return refuse(diags, MO_CODE_SERVICE_PROMISE, on->span,
                      "a service promise names no tool of its service",
                      "on names one of the tools in this service block (reference 12.2).");
    }
    if (tool != UINT32_MAX) {
        const struct mo_syntax *declared =
            program->functions[service->tools[tool].function].signature->function->declaration;
        const struct mo_syntax *parameters = declared->children[1];
        for (uint32_t i = 2; i < parameters->count; i++) {
            const struct mo_syntax *name = parameters->children[i]->children[0];
            if (reserved(name)) {
                *halt = false;
                return refuse(diags, MO_CODE_SERVICE_PROMISE, name->span,
                              "a tool parameter has a name a service promise reads",
                              "A tool promise reads before, after, now and reply, so its tool's "
                              "parameters need other names (reference 12.2).");
            }
        }
    }
    const struct mo_function_signature *signature =
        context(arena, program, service, item, tool, diags);
    if (signature == NULL)
        return false;
    *out = (struct mo_checked_service_promise){.spec = {.kind = MO_SPEC_PROMISE,
                                                        .syntax = item,
                                                        .module = service->module,
                                                        .number = number,
                                                        .subject = UINT32_MAX,
                                                        .promise = UINT32_MAX},
                                               .tool = tool};
    struct mo_span header = item->span;
    header.end = (on != NULL ? on : item->children[0])->span.end;
    const struct mo_syntax *lines = item->children[1];
    struct mo_checked_condition *conditions =
        allocate(arena, diags, header, lines->count, sizeof(*conditions),
                 _Alignof(struct mo_checked_condition));
    if (conditions == NULL || !mo_spec_copy_text(arena, diags, sources, header, &out->spec.text) ||
        !mo_spec_wording(arena, diags, item->children[0], &out->spec))
        return false;
    for (uint32_t k = 0; k < lines->count; k++) {
        conditions[k] = (struct mo_checked_condition){.expression = UINT32_MAX};
        if (!mo_spec_copy_text(arena, diags, sources, lines->children[k]->span,
                               &conditions[k].text))
            return false;
    }
    out->spec.condition_count = lines->count;
    out->spec.conditions = conditions;
    if (!mo_check_spec_body(arena, program->interfaces, program->signatures, constants, signature,
                            &out->spec, conditions, diags, halt))
        return false;
    *halt = true;
    const struct mo_syntax *written = item->children[2];
    struct mo_checked_spec *anchors = allocate(arena, diags, header, written->count,
                                               sizeof *anchors, _Alignof(struct mo_checked_spec));
    if (anchors == NULL)
        return false;
    uint32_t allows = 0, forbids = 0;
    bool valid = true;
    for (uint32_t k = 0; k < written->count; k++) {
        const struct mo_syntax *line = written->children[k];
        bool allow = line->token == MO_T_ALLOWS;
        anchors[k] = (struct mo_checked_spec){.kind = MO_SPEC_ANCHOR,
                                              .syntax = line,
                                              .module = service->module,
                                              .number = allow ? ++allows : ++forbids,
                                              .subject = UINT32_MAX,
                                              .promise = UINT32_MAX,
                                              .allows = allow};
        if (!mo_spec_copy_text(arena, diags, sources, line->span, &anchors[k].text))
            return false;
        if (!mo_check_spec_body(arena, program->interfaces, program->signatures, constants,
                                signature, &anchors[k], NULL, diags, halt)) {
            if (*halt)
                return false;
            valid = false;
            *halt = true;
        }
    }
    out->anchor_count = written->count;
    out->anchors = anchors;
    *halt = false;
    return valid;
}

bool mo_check_service_promises(struct mo_arena *arena, struct mo_program *program,
                               struct mo_constant_context *constants, struct mo_diags *diags) {
    struct mo_span span = {UINT32_MAX, 0, 0};
    struct mo_checked_service *services =
        allocate(arena, diags, span, program->service_count, sizeof *services,
                 _Alignof(struct mo_checked_service));
    if (services == NULL)
        return false;
    memcpy(services, program->services, program->service_count * sizeof *services);
    program->services = services;
    for (uint32_t i = 0; i < program->service_count; i++) {
        struct mo_checked_service *service = &services[i];
        if (service->promise_count == 0)
            continue;
        struct mo_checked_service_promise *promises =
            allocate(arena, diags, service->syntax->span, service->promise_count, sizeof *promises,
                     _Alignof(struct mo_checked_service_promise));
        if (promises == NULL)
            return false;
        service->promises = promises;
        const struct mo_syntax *clauses = service->syntax->children[0];
        uint32_t number = 0;
        for (uint32_t j = 0; j < clauses->count; j++) {
            const struct mo_syntax *item = clauses->children[j];
            if (item->kind != MO_S_PROMISE)
                continue;
            bool halt;
            if (!check_promise(arena, program, constants, service, item, number + 1,
                               &promises[number], diags, &halt) &&
                halt)
                return false;
            number++;
        }
        MO_ASSERT(number == service->promise_count);
    }
    return !mo_diags_any(diags);
}
