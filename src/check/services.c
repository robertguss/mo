#include "check/abilities.h"
#include "check/checked.h"
#include "check/map_keys.h"
#include "check/service-fixes.h"
#include "check/signatures.h"

#include "base/codes.h"
#include "base/limits.h"

#include <string.h>

struct type_pair {
    const struct mo_source_type *left, *right;
    struct type_pair *next;
};

struct service_check {
    struct mo_arena *arena;
    struct mo_program *program;
    struct mo_diags *diags;
    struct mo_checked_service *service;
    struct mo_span span;
    struct type_pair *equal;
    uint32_t *functions;
    bool *named;
    bool failed;
};

static MO_MUST_USE bool report(struct service_check *p, uint16_t code, const char *message,
                               const char *reason) {
    if (!p->failed) {
        bool kept = mo_diags_add(p->diags, &(struct mo_diag){.code = code,
                                                             .primary = {p->span, message},
                                                             .message = message,
                                                             .reason = reason});
        (void)kept;
    }
    p->failed = true;
    return false;
}

static MO_MUST_USE bool work(void *context, size_t amount) {
    struct service_check *p = context;
    uint64_t *used = p->program->signatures->work;
    if (p->failed)
        return false;
    if (amount > MO_LIMIT_PROGRAM_CHECK_WORK - *used)
        return report(p, MO_CODE_CHECK_PROGRAM_WORK,
                      "the program needs too much service-checking work", NULL);
    *used += amount;
    return true;
}

static MO_MUST_USE void *allocate(struct service_check *p, size_t count, size_t size,
                                  size_t alignment) {
    if (p->failed)
        return NULL;
    void *out = mo_arena_allocate_array(p->arena, count, size, alignment);
    if (out == NULL) {
        bool kept = report(p, MO_CODE_CHECK_NO_MEMORY, "the checking arena has no room", NULL);
        (void)kept;
    }
    return out;
}

static MO_MUST_USE bool same(struct service_check *p, const struct mo_source_type *left,
                             const struct mo_source_type *right, uint32_t depth) {
    if (!work(p, 1))
        return false;
    if (depth > MO_LIMIT_SOURCE_TYPE_DEPTH)
        return report(p, MO_CODE_CHECK_TYPE_DEPTH, "the type is nested too deeply", NULL);
    if (left == right)
        return true;
    if (left->kind != right->kind || left->module != right->module ||
        left->declaration != right->declaration || left->count != right->count ||
        strcmp(left->name, right->name) != 0)
        return false;
    for (const struct type_pair *pair = p->equal; pair != NULL; pair = pair->next) {
        if (!work(p, 1))
            return false;
        if (pair->left == left && pair->right == right)
            return true;
    }
    for (uint32_t i = 0; i < left->count; i++)
        if (!same(p, left->arguments[i], right->arguments[i], depth + 1))
            return false;
    struct type_pair *pair = allocate(p, 1, sizeof(*pair), _Alignof(struct type_pair));
    if (pair == NULL)
        return false;
    *pair = (struct type_pair){left, right, p->equal};
    p->equal = pair;
    return true;
}

static MO_MUST_USE bool native(const struct mo_source_type *type, const char *name) {
    return type->kind == MO_SOURCE_NAMED && type->module == UINT32_MAX &&
           strcmp(type->name, name) == 0;
}

static MO_MUST_USE bool update_pair(struct service_check *p, const struct mo_source_type *type) {
    return type->kind == MO_SOURCE_TUPLE && type->count == 2 &&
           same(p, type->arguments[0], p->service->state, 0);
}

static MO_MUST_USE bool handler(struct service_check *p, const struct mo_syntax *name, bool timer,
                                uint32_t *function, enum mo_service_reply *reply) {
    p->span = name->span;
    const struct mo_module_scope *scope = &p->program->interfaces->scopes[p->service->module];
    const struct mo_symbol *group =
        mo_find_symbol(scope, name->text, name->span.end - name->span.start);
    const struct mo_function_signature *selected = NULL;
    if (group != NULL) {
        uint32_t slot = (uint32_t)(group - scope->symbols);
        if (p->named[slot])
            return report(
                p, MO_CODE_SERVICE_BLOCK, "a service names a handler twice",
                "Each function is named at most once in a service block (reference 11.5).");
        p->named[slot] = true;
        for (uint32_t i = 0; i < group->overload_count; i++) {
            if (!work(p, 1))
                return false;
            const struct mo_function_signature *candidate =
                mo_function_signature_by_symbol(p->program->signatures, group + i);
            if (candidate == NULL || candidate->type->count < 2 ||
                !same(p, candidate->type->arguments[0], p->service->state, 0))
                continue;
            MO_ASSERT(selected == NULL);
            selected = candidate;
        }
    }
    if (p->failed)
        return false;
    if (selected == NULL)
        return report(p, MO_CODE_SERVICE_HANDLER,
                      "a service handler has no matching local function",
                      "A handler is a function in this module whose first parameter exactly "
                      "matches the service state type (reference 11.5).");
    const struct mo_source_type *type = selected->type;
    if (type->parameters != 0)
        return report(p, MO_CODE_SERVICE_HANDLER, "a service handler is generic",
                      "Tools and timers have concrete signatures; call generic helpers "
                      "through concrete wrappers (reference 11.5).");
    if (type->count < 3 || !native(type->arguments[1], "Time"))
        return report(p, MO_CODE_SERVICE_HANDLER, "a service handler has the wrong parameters",
                      "A tool starts with (State, Time); a timer takes exactly (State, Time) "
                      "(reference 11.5).");
    const struct mo_source_type *result = type->arguments[type->count - 1];
    bool bare = same(p, result, p->service->state, 0);
    if (p->failed)
        return false;
    if (timer && (type->count != 3 || !bare))
        return report(p, MO_CODE_SERVICE_HANDLER, "a timer handler has the wrong signature",
                      "A timer takes exactly (State, Time) and returns State (reference 11.5).");
    for (uint32_t i = 0; i < type->count; i++)
        if (!mo_check_service_purity(p->arena, p->program, type->arguments[i], p->span, p->diags))
            return false;
    uint32_t index = (uint32_t)(selected - p->program->signatures->items);
    *function = p->functions[index];
    *reply = update_pair(p, result) ? MO_SERVICE_UPDATE
             : native(result, "Result") && update_pair(p, result->arguments[0])
                 ? MO_SERVICE_RESULT_UPDATE
                 : MO_SERVICE_READ_ONLY;
    return !p->failed;
}

static MO_MUST_USE void *literal_allocate(void *context, size_t bytes) {
    return allocate(context, bytes, 1, 1);
}

static MO_MUST_USE bool token_source(struct service_check *p, const struct mo_syntax *token) {
    p->span = token->span;
    struct mo_map_key_context context = {p, work, literal_allocate};
    struct mo_map_literal_key value;
    if (!mo_map_literal_key(&context, token->children[0], &value))
        return false;
    MO_ASSERT(value.category == MO_MAP_KEY_TEXT);
    bool valid = value.length != 0;
    for (size_t i = 0; i < value.length; i++) {
        uint8_t c = value.bytes[i];
        if (!work(p, 1))
            return false;
        valid = valid && ((c >= 'A' && c <= 'Z') || c == '_' || (i != 0 && c >= '0' && c <= '9'));
    }
    if (!valid)
        return report(p, MO_CODE_SERVICE_TOKEN,
                      "the token source is not an environment variable name",
                      "Use capital letters, digits and underscores, without a leading digit "
                      "(reference 11.5).");
    p->service->token_source = token->children[0];
    return true;
}

static MO_MUST_USE bool interval(struct service_check *p, const struct mo_syntax *value) {
    p->span = value->span;
    bool valid = value->kind == MO_S_FIELD_ACCESS && value->count == 2 &&
                 value->children[0]->kind == MO_S_INT;
    if (valid) {
        const struct mo_syntax *number = value->children[0];
        const struct mo_syntax *unit = value->children[1];
        static const char *const units[] = {"second", "seconds", "millisecond", "milliseconds",
                                            "minute", "minutes", "hour",        "hours",
                                            "day",    "days"};
        valid = false;
        for (size_t i = 0; i < sizeof units / sizeof units[0]; i++)
            if (unit->span.end - unit->span.start == strlen(units[i]) &&
                memcmp(unit->text, units[i], strlen(units[i])) == 0)
                valid = true;
        bool positive = false;
        uint32_t length = number->span.end - number->span.start;
        uint32_t start = length > 2 && number->text[0] == '0' &&
                                 (number->text[1] == 'x' || number->text[1] == 'b')
                             ? 2
                             : 0;
        for (uint32_t i = start; i < length; i++) {
            if (!work(p, 1))
                return false;
            positive = positive || (number->text[i] != '0' && number->text[i] != '_');
        }
        valid = valid && positive;
    }
    return valid ||
           report(p, MO_CODE_SERVICE_VALUE, "a service timer needs a positive duration literal",
                  "Use a duration literal with a whole number of at least 1, such as "
                  "1.minute (reference 11.5).");
}

static MO_MUST_USE bool check_service(struct service_check *p, const struct mo_syntax *state,
                                      const struct mo_syntax *tools,
                                      const struct mo_syntax *token) {
    struct mo_checked_service *service = p->service;
    const struct mo_program *program = p->program;
    p->span = state->span;
    service->state = mo_resolve_service_type(p->arena, program->interfaces, program->signatures,
                                             service->module, state, p->diags);
    if (service->state == NULL)
        return false;
    uint32_t used = 0;
    struct mo_ability_context abilities = {.arena = p->arena,
                                           .interfaces = program->interfaces,
                                           .signatures = program->signatures,
                                           .work = &used};
    if (!mo_check_type_abilities(&abilities, service->state))
        return report(p, abilities.error, abilities.message, abilities.reason);
    if (!mo_check_service_purity(p->arena, program, service->state, state->span, p->diags) ||
        !token_source(p, token))
        return false;
    struct mo_checked_service_tool *checked_tools = allocate(
        p, service->tool_count, sizeof(*checked_tools), _Alignof(struct mo_checked_service_tool));
    struct mo_checked_service_timer *timers = allocate(p, service->timer_count, sizeof(*timers),
                                                       _Alignof(struct mo_checked_service_timer));
    if (checked_tools == NULL || timers == NULL)
        return false;
    service->tools = checked_tools;
    service->timers = timers;
    for (uint32_t i = 0; i < service->tool_count; i++) {
        struct mo_checked_service_tool *tool = &checked_tools[i];
        tool->name = tools->children[0]->children[i];
        if (!handler(p, tool->name, false, &tool->function, &tool->reply))
            return false;
    }
    const struct mo_syntax *clauses = service->syntax->children[0];
    uint32_t timer = 0;
    for (uint32_t i = 0; i < clauses->count; i++) {
        const struct mo_syntax *clause = clauses->children[i];
        if (!work(p, 1))
            return false;
        if (clause->kind != MO_S_SERVICE_EVERY)
            continue;
        struct mo_checked_service_timer *out = &timers[timer++];
        out->syntax = clause;
        enum mo_service_reply ignored;
        if (!interval(p, clause->children[0]) ||
            !handler(p, clause->children[1], true, &out->function, &ignored))
            return false;
    }
    MO_ASSERT(timer == service->timer_count);
    return true;
}

bool mo_prepare_services(struct mo_arena *arena, struct mo_program *program,
                         struct mo_diags *diags) {
    struct service_check p = {
        .arena = arena, .program = program, .diags = diags, .span = program->set->modules[0]->span};
    struct mo_checked_service *services =
        allocate(&p, program->set->count, sizeof(*services), _Alignof(struct mo_checked_service));
    p.functions = allocate(&p, program->function_count, sizeof(*p.functions), _Alignof(uint32_t));
    if (services == NULL || p.functions == NULL)
        return false;
    for (uint32_t i = 0; i < program->function_count; i++)
        p.functions[program->functions[i].signature - program->signatures->items] = i;
    program->services = services;
    for (uint32_t module = 0; module < program->set->count; module++) {
        const struct mo_syntax *items = program->set->modules[module]->tree->children[1];
        const struct mo_syntax *syntax = NULL;
        for (uint32_t i = 0; i < items->count; i++) {
            if (!work(&p, 1))
                return false;
            if (items->children[i]->kind != MO_S_SERVICE)
                continue;
            p.span = items->children[i]->span;
            if (syntax != NULL)
                return report(&p, MO_CODE_SERVICE_BLOCK, "a module declares two services",
                              "A module has at most one service block (reference 11.5).");
            syntax = items->children[i];
        }
        if (syntax == NULL)
            continue;
        p.service = &services[program->service_count++];
        *p.service = (struct mo_checked_service){.syntax = syntax, .module = module};
        p.span = syntax->span;
        const struct mo_syntax *clauses = syntax->children[0];
        uint32_t stage = 0;
        const struct mo_syntax *state = NULL, *tools = NULL, *token = NULL;
        for (uint32_t i = 0; i < clauses->count; i++) {
            const struct mo_syntax *clause = clauses->children[i];
            p.span = clause->span;
            if (!work(&p, 1))
                return false;
            if (stage == 0 && clause->kind == MO_S_SERVICE_STATE) {
                state = clause;
                stage = 1;
            } else if (stage == 1 && clause->kind == MO_S_SERVICE_TOOLS) {
                tools = clause;
                p.service->tool_count = tools->children[0]->count;
                stage = 2;
            } else if (stage == 2 && clause->kind == MO_S_SERVICE_EVERY)
                p.service->timer_count++;
            else if (stage == 2 && clause->kind == MO_S_SERVICE_TOKEN) {
                token = clause;
                stage = 3;
            } else
                return report(&p, MO_CODE_SERVICE_BLOCK,
                              "a service clause is repeated or out of order",
                              "Write state, tools, zero or more every clauses, then token, in that "
                              "order (reference 11.5).");
        }
        if (stage != 3 || p.service->tool_count == 0)
            return report(
                &p, MO_CODE_SERVICE_BLOCK, "a service is missing a required clause",
                "A service needs one state, one nonempty tools clause and one token "
                "clause, in that order, with timers between tools and token (reference 11.5).");
        uint32_t count = program->interfaces->scopes[module].count;
        p.named = allocate(&p, count, sizeof(*p.named), _Alignof(bool));
        if (p.named == NULL)
            return false;
        memset(p.named, 0, (size_t)count * sizeof(*p.named));
        if (!check_service(&p, state, tools, token))
            return false;
    }
    return true;
}

bool mo_check_services(struct mo_arena *arena, struct mo_program *program,
                       struct mo_constant_context *constants, struct mo_diags *diags) {
    struct service_check p = {.arena = arena, .program = program, .diags = diags};
    struct mo_checked_service *services = allocate(&p, program->service_count, sizeof(*services),
                                                   _Alignof(struct mo_checked_service));
    if (services == NULL)
        return false;
    memcpy(services, program->services, program->service_count * sizeof(*services));
    program->services = services;
    static const struct mo_source_type duration = {
        .kind = MO_SOURCE_NAMED, .name = "Duration", .module = UINT32_MAX};
    for (uint32_t i = 0; i < program->service_count; i++) {
        struct mo_checked_service *s = &services[i];
        p.service = s;
        const struct mo_syntax *state = s->syntax->children[0]->children[0];
        if (!mo_check_service_value(arena, program->interfaces, program->signatures, constants,
                                    s->module, state, s->state, diags, &s->initializer))
            return false;
        for (uint32_t j = 0; j < s->tool_count; j++) {
            const struct mo_checked_service_tool *tool = &s->tools[j];
            const struct mo_source_type *signature =
                program->functions[tool->function].signature->type;
            const struct mo_source_type *result = signature->arguments[signature->count - 1];
            bool bare = same(&p, result, s->state, 0);
            bool wrapped = native(result, "Result") && same(&p, result->arguments[0], s->state, 0);
            if (p.failed)
                return false;
            if (bare || wrapped)
                return mo_service_state_error(arena, program, tool->name, tool->function, wrapped,
                                              diags);
        }
        struct mo_checked_service_timer *timers = allocate(
            &p, s->timer_count, sizeof(*timers), _Alignof(struct mo_checked_service_timer));
        if (timers == NULL)
            return false;
        memcpy(timers, s->timers, s->timer_count * sizeof(*timers));
        s->timers = timers;
        for (uint32_t j = 0; j < s->timer_count; j++) {
            struct mo_checked_service_timer *timer = &timers[j];
            if (!mo_check_service_value(arena, program->interfaces, program->signatures, constants,
                                        s->module, timer->syntax, &duration, diags,
                                        &timer->interval))
                return false;
        }
    }
    return true;
}
