#include "check/abilities.h"
#include "check/checked.h"
#include "check/map_keys.h"
#include "check/signatures.h"

#include "base/codes.h"
#include "base/limits.h"

#include <stdio.h>
#include <string.h>

static const char *const api_cap_order[] = {"Sqlite",    "Network", "Clock",   "Random", "Files",
                                            "ReadFiles", "Env",     "Console", "Command"};
enum { API_CAP_KIND_COUNT = sizeof api_cap_order / sizeof api_cap_order[0] };

struct api_type_pair {
    const struct mo_source_type *left, *right;
    struct api_type_pair *next;
};

struct api_check {
    struct mo_arena *arena;
    const struct mo_program *program;
    struct mo_ability_context *abilities;
    struct mo_diags *diags;
    struct mo_checked_api *api;
    struct mo_span span;
    struct api_type_pair *equal;
    const struct mo_source_type *req_type;
    const struct mo_source_type *res_type;
    uint32_t *functions;
    bool cap_used[API_CAP_KIND_COUNT];
    bool failed;
};

static MO_MUST_USE bool api_report(struct api_check *p, uint16_t code, const char *message,
                                   const char *reason) {
    if (code >= 108 && code <= 110) {
        if (reason == NULL) {
            reason = "See the api block rules (reference 11.6).";
        } else if (strstr(reason, "(reference 11.6)") == NULL) {
            size_t rlen = strlen(reason);
            if (rlen > 0 && reason[rlen - 1] == '.')
                rlen--;
            char *cited = mo_arena_allocate_array(p->arena, rlen + 24, 1, 1);
            if (cited != NULL) {
                memcpy(cited, reason, rlen);
                memcpy(cited + rlen, " (reference 11.6).", sizeof " (reference 11.6).");
                reason = cited;
            }
        }
    }
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

static MO_MUST_USE bool api_work(void *context, size_t amount) {
    struct api_check *p = context;
    uint64_t *used = p->program->signatures->work;
    if (p->failed)
        return false;
    if (amount > MO_LIMIT_PROGRAM_CHECK_WORK - *used)
        return api_report(p, MO_CODE_CHECK_PROGRAM_WORK,
                          "the program needs too much api-checking work", NULL);
    *used += amount;
    return true;
}

static MO_MUST_USE void *api_alloc(struct api_check *p, size_t count, size_t size,
                                   size_t alignment) {
    if (p->failed)
        return NULL;
    void *out = mo_arena_allocate_array(p->arena, count, size, alignment);
    if (out == NULL) {
        bool kept = api_report(p, MO_CODE_CHECK_NO_MEMORY, "the checking arena has no room", NULL);
        (void)kept;
    }
    return out;
}

static MO_MUST_USE void *api_literal_alloc(void *context, size_t bytes) {
    return api_alloc(context, bytes, 1, 1);
}

static MO_MUST_USE char *api_copy_text(struct api_check *p, const uint8_t *bytes, size_t length) {
    char *out = api_alloc(p, length + 1, 1, 1);
    if (out == NULL)
        return NULL;
    if (length != 0)
        memcpy(out, bytes, length);
    out[length] = '\0';
    return out;
}

static MO_MUST_USE char *api_syntax_cstr(struct api_check *p, const struct mo_syntax *node) {
    return api_copy_text(p, node->text, node->span.end - node->span.start);
}

static MO_MUST_USE bool api_same_type(struct api_check *p, const struct mo_source_type *left,
                                      const struct mo_source_type *right, uint32_t depth) {
    if (!api_work(p, 1))
        return false;
    if (depth > MO_LIMIT_SOURCE_TYPE_DEPTH)
        return api_report(p, MO_CODE_CHECK_TYPE_DEPTH, "the type is nested too deeply", NULL);
    if (left == right)
        return true;
    if (left->kind != right->kind || left->module != right->module ||
        left->declaration != right->declaration || left->count != right->count ||
        strcmp(left->name, right->name) != 0)
        return false;
    for (const struct api_type_pair *pair = p->equal; pair != NULL; pair = pair->next) {
        if (!api_work(p, 1))
            return false;
        if (pair->left == left && pair->right == right)
            return true;
    }
    for (uint32_t i = 0; i < left->count; i++)
        if (!api_same_type(p, left->arguments[i], right->arguments[i], depth + 1))
            return false;
    struct api_type_pair *pair = api_alloc(p, 1, sizeof(*pair), _Alignof(struct api_type_pair));
    if (pair == NULL)
        return false;
    *pair = (struct api_type_pair){left, right, p->equal};
    p->equal = pair;
    return true;
}

static MO_MUST_USE bool api_native_type(const struct mo_source_type *type, const char *name) {
    return type != NULL && type->kind == MO_SOURCE_NAMED && type->module == UINT32_MAX &&
           strcmp(type->name, name) == 0;
}

static MO_MUST_USE bool api_is_path_scalar(const struct mo_source_type *type) {
    return api_native_type(type, "Int") || api_native_type(type, "Byte") ||
           api_native_type(type, "BigInt") || api_native_type(type, "Decimal") ||
           api_native_type(type, "Bool") || api_native_type(type, "Text");
}

static MO_MUST_USE bool api_is_query_scalar(const struct mo_source_type *type) {
    return api_is_path_scalar(type) || api_native_type(type, "Float");
}

static MO_MUST_USE uint32_t api_cap_slot(const struct mo_source_type *type) {
    if (type == NULL || type->kind != MO_SOURCE_NAMED || type->module != UINT32_MAX)
        return UINT32_MAX;
    for (uint32_t i = 0; i < API_CAP_KIND_COUNT; i++)
        if (strcmp(type->name, api_cap_order[i]) == 0)
            return i;
    return UINT32_MAX;
}

static MO_MUST_USE const struct mo_ability_implementation *
api_prove_json(struct api_check *p, const struct mo_source_type *type, uint16_t fallback_code,
               const char *fallback_msg, const char *fallback_reason) {
    *p->abilities->work = 0;
    const struct mo_ability_implementation *proof =
        mo_prove_ability(p->abilities, type, MO_SOURCE_JSON);
    if (proof != NULL)
        return proof;
    uint16_t err = p->abilities->error >= 1000 ? p->abilities->error : fallback_code;
    const char *msg = p->abilities->error >= 1000 ? p->abilities->message : fallback_msg;
    const char *rsn = p->abilities->error >= 1000 ? p->abilities->reason : fallback_reason;
    *p->abilities = (struct mo_ability_context){.arena = p->abilities->arena,
                                                .interfaces = p->abilities->interfaces,
                                                .signatures = p->abilities->signatures,
                                                .work = p->abilities->work};
    bool reported = api_report(p, err, msg, rsn);
    (void)reported;
    return NULL;
}

static MO_MUST_USE bool parse_u16_literal(struct api_check *p, const struct mo_syntax *lit,
                                          uint16_t *out) {
    const uint8_t *bytes = lit->text;
    uint32_t len = lit->span.end - lit->span.start;
    uint32_t base = 10, pos = 0;
    if (len > 2 && bytes[0] == '0' && (bytes[1] == 'x' || bytes[1] == 'b')) {
        base = bytes[1] == 'x' ? 16u : 2u;
        pos = 2;
    }
    uint64_t val = 0;
    bool any_digit = false;
    for (; pos < len; pos++) {
        if (!api_work(p, 1))
            return false;
        uint8_t c = bytes[pos];
        if (c == '_')
            continue;
        unsigned d = c >= '0' && c <= '9'   ? (unsigned)(c - '0')
                     : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a') + 10u
                     : c >= 'A' && c <= 'F' ? (unsigned)(c - 'A') + 10u
                                            : 99u;
        if (d >= base)
            return false;
        any_digit = true;
        val = val * base + d;
        if (val > UINT16_MAX)
            return false;
    }
    if (!any_digit)
        return false;
    *out = (uint16_t)val;
    return true;
}

static MO_MUST_USE bool decode_plain_text(struct api_check *p, const struct mo_syntax *text_node,
                                          struct mo_map_literal_key *out) {
    struct mo_map_key_context ctx = {p, api_work, api_literal_alloc};
    if (!mo_map_literal_key(&ctx, text_node, out))
        return false;
    MO_ASSERT(out->category == MO_MAP_KEY_TEXT);
    return true;
}

static MO_MUST_USE bool valid_sqlite_path(const uint8_t *bytes, size_t len) {
    if (len == 0 || bytes[0] == '/' || bytes[0] == '\\')
        return false;
    for (size_t i = 0; i < len; i++)
        if (bytes[i] == '\0' || bytes[i] == '\\')
            return false;
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && bytes[i] != '/')
            i++;
        size_t seg = i - start;
        if (seg == 0 || (seg == 2 && bytes[start] == '.' && bytes[start + 1] == '.'))
            return false;
        if (i < len)
            i++;
    }
    return true;
}

struct route_path_info {
    const char *clean_path;
    const char *shape;
    uint32_t param_count;
    const char **param_names;
};

static MO_MUST_USE bool parse_route_path(struct api_check *p, const struct mo_syntax *lit,
                                         struct route_path_info *out) {
    p->span = lit->span;
    struct mo_map_literal_key raw;
    if (!decode_plain_text(p, lit, &raw))
        return false;
    if (raw.length == 0 || raw.bytes[0] != '/')
        return api_report(p, MO_CODE_API_ROUTE, "an api route path must start with /",
                          "Write a path template starting with /, such as \"/users/:id\".");
    for (size_t i = 0; i < raw.length; i++) {
        uint8_t c = raw.bytes[i];
        if (c <= 0x20 || c == 0x7f || c == '?' || c == '#')
            return api_report(p, MO_CODE_API_ROUTE,
                              "an api route path contains an invalid character",
                              "Route templates cannot contain whitespace, control bytes, ? or #.");
    }
    size_t clean_len = raw.length;
    if (clean_len > 1 && raw.bytes[clean_len - 1] == '/')
        clean_len--;
    char *clean = api_copy_text(p, raw.bytes, clean_len);
    char *shape = api_alloc(p, clean_len + 2, 1, 1);
    const char **params =
        api_alloc(p, MO_LIMIT_API_PATH_SEGMENTS, sizeof(*params), _Alignof(const char *));
    if (clean == NULL || shape == NULL || params == NULL)
        return false;
    if (clean_len == 1) {
        shape[0] = '/';
        shape[1] = '\0';
        *out = (struct route_path_info){clean, shape, 0, params};
        return true;
    }
    size_t shape_at = 0;
    uint32_t seg_count = 0, param_count = 0;
    size_t pos = 1;
    while (pos <= clean_len) {
        size_t start = pos;
        while (pos < clean_len && clean[pos] != '/')
            pos++;
        size_t seg_len = pos - start;
        if (seg_len == 0)
            return api_report(p, MO_CODE_API_ROUTE, "an api route path has an empty segment",
                              "Avoid repeated slashes in route paths.");
        if (++seg_count > MO_LIMIT_API_PATH_SEGMENTS)
            return api_report(p, MO_CODE_API_ROUTE, "an api route path has too many segments",
                              "An api route path has at most 64 segments.");
        shape[shape_at++] = '/';
        if (clean[start] == ':') {
            if (seg_len < 2)
                return api_report(p, MO_CODE_API_ROUTE, "an api path parameter has no name",
                                  "Name path parameters after :, such as :id.");
            bool ok_name =
                (clean[start + 1] >= 'a' && clean[start + 1] <= 'z') || clean[start + 1] == '_';
            for (size_t k = 2; k < seg_len; k++) {
                char ch = clean[start + k];
                bool ok_ch = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' ||
                             (ch == '?' && k + 1 == seg_len);
                ok_name = ok_name && ok_ch;
            }
            if (!ok_name)
                return api_report(
                    p, MO_CODE_API_ROUTE, "an api path parameter is not a valid parameter name",
                    "Use snake_case parameter names matching the handler's parameter.");
            const char *pname = api_copy_text(p, (const uint8_t *)(clean + start + 1), seg_len - 1);
            if (pname == NULL)
                return false;
            for (uint32_t j = 0; j < param_count; j++)
                if (strcmp(params[j], pname) == 0)
                    return api_report(p, MO_CODE_API_ROUTE,
                                      "an api route repeats a path parameter name",
                                      "Each :parameter in a route path must have a distinct name.");
            params[param_count++] = pname;
            shape[shape_at++] = ':';
        } else {
            memcpy(shape + shape_at, clean + start, seg_len);
            shape_at += seg_len;
        }
        pos++;
    }
    shape[shape_at] = '\0';
    *out = (struct route_path_info){clean, shape, param_count, params};
    return true;
}

static MO_MUST_USE bool resolve_local_fn(struct api_check *p, const struct mo_syntax *name_node,
                                         uint16_t err_code, const char *role,
                                         const struct mo_function_signature **out_sig,
                                         uint32_t *out_fn_index) {
    p->span = name_node->span;
    const struct mo_module_scope *scope = &p->program->interfaces->scopes[p->api->module];
    const struct mo_symbol *group =
        mo_find_symbol(scope, name_node->text, name_node->span.end - name_node->span.start);
    if (group == NULL || group->declaration->kind != MO_S_FN) {
        char *msg = api_alloc(p, 128, 1, 1);
        if (msg == NULL)
            return false;
        (void)snprintf(msg, 128, "an api %s names an unknown local function", role);
        return api_report(p, err_code, msg,
                          "Name a function declared in the same module as the api block.");
    }
    if (group->overload_count > 1) {
        char *msg = api_alloc(p, 128, 1, 1);
        if (msg == NULL)
            return false;
        (void)snprintf(msg, 128, "an api %s function cannot be overloaded", role);
        return api_report(p, err_code, msg,
                          "Give each api route or auth handler a distinct function name.");
    }
    const struct mo_function_signature *sig =
        mo_function_signature_by_symbol(p->program->signatures, group);
    if (sig == NULL || sig->type == NULL)
        return false;
    if (sig->type->parameters != 0) {
        char *msg = api_alloc(p, 128, 1, 1);
        if (msg == NULL)
            return false;
        (void)snprintf(msg, 128, "an api %s function cannot be generic", role);
        return api_report(p, err_code, msg,
                          "Api handlers have concrete types; wrap generic helpers in a concrete "
                          "function.");
    }
    *out_sig = sig;
    *out_fn_index = p->functions[sig - p->program->signatures->items];
    return true;
}

static MO_MUST_USE bool check_error_type(struct api_check *p, const struct mo_source_type *err_ty,
                                         struct mo_span span,
                                         const struct mo_ability_implementation **out_json) {
    p->span = span;
    *out_json = NULL;
    if (api_same_type(p, err_ty, p->res_type, 0))
        return true;
    if (p->failed)
        return false;
    if (err_ty->kind != MO_SOURCE_NAMED || err_ty->declaration == NULL ||
        err_ty->declaration->declaration->kind != MO_S_ENUM || err_ty->parameters != 0 ||
        err_ty->count != 0)
        return api_report(
            p, MO_CODE_API_STATUS, "an api error type must be a concrete enum or Response",
            "Return Result(T, AppError) where AppError is a non-generic enum with status "
            "clauses, or Result(T, Response).");
    if (err_ty->module != p->api->module && !err_ty->declaration->insides)
        return api_report(p, MO_CODE_API_STATUS,
                          "an api error enum's cases are hidden from this module",
                          "Expose the error enum with (..) so the api block can match its cases.");
    *out_json =
        api_prove_json(p, err_ty, MO_CODE_API_STATUS, "an api error enum lacks the Json ability",
                       "Every case field of an api error enum must support Json encoding.");
    return *out_json != NULL;
}

static MO_MUST_USE bool find_sqlite_slot(const struct mo_checked_api *api, const char *name,
                                         uint32_t *out) {
    const char *stripped = name[0] == '_' && name[1] != '\0' ? name + 1 : name;
    for (uint32_t i = 0; i < api->sqlite_count; i++)
        if (strcmp(api->sqlites[i].name, name) == 0 ||
            strcmp(api->sqlites[i].name, stripped) == 0) {
            *out = i;
            return true;
        }
    return false;
}

static MO_MUST_USE bool is_actor_type(const struct mo_source_type *type) {
    return type != NULL && type->kind == MO_SOURCE_NAMED && type->declaration != NULL &&
           type->declaration->declaration->kind == MO_S_ACTOR;
}

static MO_MUST_USE bool find_actor_slot(struct api_check *p, const char *name,
                                        const struct mo_source_type *type, uint32_t *out) {
    const char *stripped = name[0] == '_' && name[1] != '\0' ? name + 1 : name;
    for (uint32_t i = 0; i < p->api->actor_count; i++)
        if ((strcmp(p->api->actors[i].name, name) == 0 ||
             strcmp(p->api->actors[i].name, stripped) == 0) &&
            api_same_type(p, type, p->api->actors[i].type, 0)) {
            *out = i;
            return true;
        }
    return false;
}

static MO_MUST_USE bool check_auth_clause(struct api_check *p, const struct mo_syntax *clause) {
    p->span = clause->span;
    const struct mo_source_type *principal = mo_resolve_service_type(
        p->arena, p->program->interfaces, p->program->signatures, p->api->module, clause, p->diags);
    if (principal == NULL) {
        p->failed = true;
        return false;
    }
    *p->abilities->work = 0;
    if (!mo_check_type_abilities(p->abilities, principal))
        return api_report(p, p->abilities->error, p->abilities->message, p->abilities->reason);
    if (principal->parameters != 0 || principal->kind != MO_SOURCE_NAMED ||
        principal->declaration == NULL ||
        (principal->declaration->declaration->kind != MO_S_STRUCT &&
         principal->declaration->declaration->kind != MO_S_ENUM) ||
        api_same_type(p, principal, p->req_type, 0) || api_same_type(p, principal, p->res_type, 0))
        return api_report(p, MO_CODE_API_BLOCK,
                          "an api auth principal must be a concrete struct or enum",
                          "Declare a nominal principal type such as struct CurrentUser.");
    const struct mo_function_signature *sig = NULL;
    uint32_t fn_idx = 0;
    if (!resolve_local_fn(p, clause->children[1], MO_CODE_API_BLOCK, "auth", &sig, &fn_idx))
        return false;
    const struct mo_syntax *fn_decl = sig->function->declaration;
    const struct mo_syntax *param_nodes = fn_decl->children[1];
    uint32_t param_count = param_nodes->count;
    struct mo_checked_api_param *params =
        api_alloc(p, param_count, sizeof(*params), _Alignof(struct mo_checked_api_param));
    if (params == NULL && param_count != 0)
        return false;
    bool seen_req = false;
    uint32_t seen_caps = 0;
    uint32_t seen_dbs = 0;
    uint32_t seen_actors = 0;
    for (uint32_t i = 0; i < param_count; i++) {
        const struct mo_syntax *pnode = param_nodes->children[i];
        p->span = pnode->span;
        const char *pname = api_syntax_cstr(p, pnode->children[0]);
        if (pname == NULL)
            return false;
        const struct mo_source_type *ptype = sig->type->arguments[i];
        if (api_same_type(p, ptype, p->req_type, 0)) {
            if (seen_req)
                return api_report(p, MO_CODE_API_BLOCK, "an api auth function takes Request twice",
                                  NULL);
            seen_req = true;
            params[i] = (struct mo_checked_api_param){
                .kind = MO_API_PARAM_REQUEST, .name = pname, .type = ptype};
            continue;
        }
        if (api_native_type(ptype, "Sqlite.Database")) {
            uint32_t db_slot = 0;
            if (!find_sqlite_slot(p->api, pname, &db_slot))
                return api_report(
                    p, MO_CODE_API_BLOCK,
                    "an api auth Sqlite.Database parameter has no matching sqlite clause",
                    "Declare sqlite <name> from \"<path>\" in the api block with the same name.");
            if ((seen_dbs & (UINT32_C(1) << db_slot)) != 0)
                return api_report(p, MO_CODE_API_BLOCK,
                                  "an api auth function takes the same sqlite database twice",
                                  NULL);
            seen_dbs |= UINT32_C(1) << db_slot;
            params[i] = (struct mo_checked_api_param){
                .kind = MO_API_PARAM_SQLITE_DB, .name = pname, .type = ptype, .index = db_slot};
            continue;
        }
        if (is_actor_type(ptype)) {
            uint32_t actor_slot = 0;
            if (!find_actor_slot(p, pname, ptype, &actor_slot))
                return p->failed ? false
                                 : api_report(p, MO_CODE_API_BLOCK,
                                              "an api auth actor parameter has no matching actor "
                                              "clause",
                                              "Declare actor <name>: <ActorType> in the api block "
                                              "with the same name and type.");
            if ((seen_actors & (UINT32_C(1) << actor_slot)) != 0)
                return api_report(p, MO_CODE_API_BLOCK,
                                  "an api auth function takes the same actor twice", NULL);
            seen_actors |= UINT32_C(1) << actor_slot;
            params[i] = (struct mo_checked_api_param){
                .kind = MO_API_PARAM_ACTOR, .name = pname, .type = ptype, .index = actor_slot};
            continue;
        }
        uint32_t cap_slot = api_cap_slot(ptype);
        if (cap_slot != UINT32_MAX) {
            if ((seen_caps & (UINT32_C(1) << cap_slot)) != 0)
                return api_report(p, MO_CODE_API_BLOCK,
                                  "an api auth function repeats a capability type", NULL);
            seen_caps |= UINT32_C(1) << cap_slot;
            p->cap_used[cap_slot] = true;
            params[i] = (struct mo_checked_api_param){
                .kind = MO_API_PARAM_CAPABILITY, .name = pname, .type = ptype, .index = cap_slot};
            continue;
        }
        return api_report(
            p, MO_CODE_API_BLOCK, "an api auth function has an unsupported parameter",
            "An auth function takes Request, declared sqlite databases, or capabilities.");
    }
    const struct mo_source_type *ret = sig->type->arguments[sig->type->count - 1];
    p->span = fn_decl->children[2] != NULL ? fn_decl->children[2]->span : clause->span;
    if (!api_native_type(ret, "Result") || !api_same_type(p, ret->arguments[0], principal, 0))
        return api_report(p, MO_CODE_API_BLOCK,
                          "an api auth function must return Result(Principal, E)",
                          "Return Result(<Type>, <ErrorEnum>) matching the auth <Type> clause.");
    const struct mo_ability_implementation *err_json = NULL;
    if (!check_error_type(p, ret->arguments[1], p->span, &err_json))
        return false;
    p->api->has_auth = true;
    p->api->auth = (struct mo_checked_api_auth){.syntax = clause,
                                                .principal = principal,
                                                .function = fn_idx,
                                                .param_count = param_count,
                                                .params = params,
                                                .error_type = ret->arguments[1],
                                                .error_json = err_json};
    return true;
}

static MO_MUST_USE enum mo_api_method parse_method_word(const struct mo_syntax *word) {
    size_t len = word->span.end - word->span.start;
    if (len == 3 && memcmp(word->text, "get", 3) == 0)
        return MO_API_GET;
    if (len == 4 && memcmp(word->text, "post", 4) == 0)
        return MO_API_POST;
    if (len == 3 && memcmp(word->text, "put", 3) == 0)
        return MO_API_PUT;
    if (len == 5 && memcmp(word->text, "patch", 5) == 0)
        return MO_API_PATCH;
    return MO_API_DELETE;
}

static MO_MUST_USE bool check_route_clause(struct api_check *p, const struct mo_syntax *clause,
                                           const char **shapes, uint32_t route_idx,
                                           struct mo_checked_api_route *out) {
    enum mo_api_method method = parse_method_word(clause->children[0]);
    struct route_path_info path_info;
    if (!parse_route_path(p, clause->children[1], &path_info))
        return false;
    for (uint32_t i = 0; i < route_idx; i++) {
        if (!api_work(p, 1))
            return false;
        if (p->api->routes[i].method == method && strcmp(shapes[i], path_info.shape) == 0) {
            p->span = clause->children[1]->span;
            return api_report(p, MO_CODE_API_ROUTE,
                              "two api routes have the same HTTP method and path shape",
                              "Each (method, path) pair in an api block must be distinct.");
        }
    }
    shapes[route_idx] = path_info.shape;

    const struct mo_function_signature *sig = NULL;
    uint32_t fn_idx = 0;
    if (!resolve_local_fn(p, clause->children[2], MO_CODE_API_ROUTE, "route", &sig, &fn_idx))
        return false;
    const struct mo_syntax *fn_decl = sig->function->declaration;
    const struct mo_syntax *param_nodes = fn_decl->children[1];
    uint32_t param_count = param_nodes->count;
    struct mo_checked_api_param *params =
        api_alloc(p, param_count, sizeof(*params), _Alignof(struct mo_checked_api_param));
    bool *matched_path = api_alloc(p, path_info.param_count == 0 ? 1 : path_info.param_count,
                                   sizeof(*matched_path), _Alignof(bool));
    if ((param_count != 0 && params == NULL) || matched_path == NULL)
        return false;
    if (path_info.param_count != 0)
        memset(matched_path, 0, path_info.param_count * sizeof(*matched_path));

    bool seen_auth = false, seen_req = false, seen_body = false;
    uint32_t seen_caps = 0, seen_dbs = 0, seen_actors = 0;
    bool allows_body = method == MO_API_POST || method == MO_API_PUT || method == MO_API_PATCH;

    for (uint32_t i = 0; i < param_count; i++) {
        const struct mo_syntax *pnode = param_nodes->children[i];
        p->span = pnode->span;
        const char *pname = api_syntax_cstr(p, pnode->children[0]);
        if (pname == NULL)
            return false;
        const struct mo_source_type *ptype = sig->type->arguments[i];
        if (p->api->has_auth && api_same_type(p, ptype, p->api->auth.principal, 0)) {
            if (seen_auth)
                return api_report(p, MO_CODE_API_ROUTE,
                                  "an api route handler takes the auth principal twice", NULL);
            seen_auth = true;
            params[i] = (struct mo_checked_api_param){
                .kind = MO_API_PARAM_AUTH, .name = pname, .type = ptype};
            continue;
        }
        if (api_same_type(p, ptype, p->req_type, 0)) {
            if (seen_req)
                return api_report(p, MO_CODE_API_ROUTE, "an api route handler takes Request twice",
                                  NULL);
            seen_req = true;
            params[i] = (struct mo_checked_api_param){
                .kind = MO_API_PARAM_REQUEST, .name = pname, .type = ptype};
            continue;
        }
        if (api_native_type(ptype, "Sqlite.Database")) {
            uint32_t db_slot = 0;
            if (!find_sqlite_slot(p->api, pname, &db_slot))
                return api_report(
                    p, MO_CODE_API_ROUTE,
                    "an api route Sqlite.Database parameter has no matching sqlite clause",
                    "Declare sqlite <name> from \"<path>\" in the api block with the same name.");
            if ((seen_dbs & (UINT32_C(1) << db_slot)) != 0)
                return api_report(p, MO_CODE_API_ROUTE,
                                  "an api route handler takes the same sqlite database twice",
                                  NULL);
            seen_dbs |= UINT32_C(1) << db_slot;
            params[i] = (struct mo_checked_api_param){
                .kind = MO_API_PARAM_SQLITE_DB, .name = pname, .type = ptype, .index = db_slot};
            continue;
        }
        if (is_actor_type(ptype)) {
            uint32_t actor_slot = 0;
            if (!find_actor_slot(p, pname, ptype, &actor_slot))
                return p->failed ? false
                                 : api_report(p, MO_CODE_API_ROUTE,
                                              "an api route actor parameter has no matching actor "
                                              "clause",
                                              "Declare actor <name>: <ActorType> in the api block "
                                              "with the same name and type.");
            if ((seen_actors & (UINT32_C(1) << actor_slot)) != 0)
                return api_report(p, MO_CODE_API_ROUTE,
                                  "an api route handler takes the same actor twice", NULL);
            seen_actors |= UINT32_C(1) << actor_slot;
            params[i] = (struct mo_checked_api_param){
                .kind = MO_API_PARAM_ACTOR, .name = pname, .type = ptype, .index = actor_slot};
            continue;
        }
        uint32_t cap_slot = api_cap_slot(ptype);
        if (cap_slot != UINT32_MAX) {
            if ((seen_caps & (UINT32_C(1) << cap_slot)) != 0)
                return api_report(p, MO_CODE_API_ROUTE,
                                  "an api route handler repeats a capability type", NULL);
            seen_caps |= UINT32_C(1) << cap_slot;
            p->cap_used[cap_slot] = true;
            params[i] = (struct mo_checked_api_param){
                .kind = MO_API_PARAM_CAPABILITY, .name = pname, .type = ptype, .index = cap_slot};
            continue;
        }
        uint32_t path_slot = UINT32_MAX;
        for (uint32_t k = 0; k < path_info.param_count; k++)
            if (strcmp(path_info.param_names[k], pname) == 0) {
                path_slot = k;
                break;
            }
        if (path_slot != UINT32_MAX) {
            if (!api_is_path_scalar(ptype))
                return api_report(p, MO_CODE_API_ROUTE,
                                  "an api path parameter must be Int, Byte, BigInt, Decimal, Bool "
                                  "or Text",
                                  "Path segments are decoded into scalar values.");
            matched_path[path_slot] = true;
            params[i] = (struct mo_checked_api_param){
                .kind = MO_API_PARAM_PATH, .name = pname, .type = ptype, .index = path_slot};
            continue;
        }
        if (api_native_type(ptype, "Option") && api_is_query_scalar(ptype->arguments[0])) {
            params[i] = (struct mo_checked_api_param){
                .kind = MO_API_PARAM_QUERY_OPTIONAL, .name = pname, .type = ptype};
            continue;
        }
        if (!allows_body) {
            if (api_is_query_scalar(ptype)) {
                params[i] = (struct mo_checked_api_param){
                    .kind = MO_API_PARAM_QUERY_REQUIRED, .name = pname, .type = ptype};
                continue;
            }
            return api_report(
                p, MO_CODE_API_ROUTE,
                "a GET or DELETE route parameter must be a path or scalar query parameter",
                "Use scalar or Option(Scalar) query parameters on GET/DELETE, or POST/PUT/PATCH "
                "for a JSON request body.");
        }
        if (seen_body)
            return api_report(
                p, MO_CODE_API_ROUTE, "an api route handler takes more than one JSON body",
                "Pass one struct or enum body parameter (and Option(Scalar) for optional query "
                "parameters).");
        const struct mo_ability_implementation *body_json = api_prove_json(
            p, ptype, MO_CODE_API_ROUTE, "an api request body parameter lacks the Json ability",
            "Request body types must support Json decoding.");
        if (body_json == NULL)
            return false;
        seen_body = true;
        params[i] = (struct mo_checked_api_param){
            .kind = MO_API_PARAM_BODY, .name = pname, .type = ptype, .json = body_json};
    }

    for (uint32_t k = 0; k < path_info.param_count; k++)
        if (!matched_path[k]) {
            p->span = clause->children[1]->span;
            return api_report(
                p, MO_CODE_API_ROUTE,
                "an api route path parameter has no matching parameter on the handler",
                "Add a parameter with the same name to the route handler.");
        }

    const struct mo_source_type *ret = sig->type->arguments[sig->type->count - 1];
    struct mo_span ret_span =
        fn_decl->children[2] != NULL ? fn_decl->children[2]->span : clause->span;
    p->span = ret_span;
    enum mo_api_reply_kind reply = MO_API_REPLY_JSON;
    const struct mo_source_type *ok_ty = NULL, *err_ty = NULL;
    const struct mo_ability_implementation *ok_json = NULL, *err_json = NULL;

    if (api_same_type(p, ret, p->res_type, 0)) {
        reply = MO_API_REPLY_RESPONSE;
    } else if (api_native_type(ret, "Unit")) {
        reply = MO_API_REPLY_UNIT;
    } else if (api_native_type(ret, "Result")) {
        ok_ty = ret->arguments[0];
        err_ty = ret->arguments[1];
        if (api_same_type(p, ok_ty, p->res_type, 0)) {
            reply = MO_API_REPLY_RESULT_RESPONSE;
        } else if (api_native_type(ok_ty, "Unit")) {
            reply = MO_API_REPLY_RESULT_UNIT;
        } else {
            reply = MO_API_REPLY_RESULT_JSON;
            ok_json = api_prove_json(p, ok_ty, MO_CODE_API_ROUTE,
                                     "an api route return type lacks the Json ability",
                                     "Route return types must be Response, Unit, or a Json type.");
            if (ok_json == NULL)
                return false;
        }
        if (!check_error_type(p, err_ty, ret_span, &err_json))
            return false;
    } else {
        if (p->failed)
            return false;
        ok_ty = ret;
        reply = MO_API_REPLY_JSON;
        ok_json = api_prove_json(p, ok_ty, MO_CODE_API_ROUTE,
                                 "an api route return type lacks the Json ability",
                                 "Route return types must be Response, Unit, or a Json type.");
        if (ok_json == NULL)
            return false;
    }

    *out = (struct mo_checked_api_route){.syntax = clause,
                                         .method = method,
                                         .path = path_info.clean_path,
                                         .function = fn_idx,
                                         .authenticated = seen_auth,
                                         .param_count = param_count,
                                         .params = params,
                                         .reply = reply,
                                         .ok_type = ok_ty,
                                         .ok_json = ok_json,
                                         .error_type = err_ty,
                                         .error_json = err_json};
    return true;
}

static MO_MUST_USE bool add_error_enum(struct api_check *p, const struct mo_source_type *err_ty,
                                       const struct mo_symbol **enums, uint32_t *count) {
    if (err_ty == NULL || api_same_type(p, err_ty, p->res_type, 0))
        return !p->failed;
    const struct mo_symbol *sym = err_ty->declaration;
    MO_ASSERT(sym != NULL && sym->declaration->kind == MO_S_ENUM);
    for (uint32_t i = 0; i < *count; i++)
        if (enums[i] == sym)
            return true;
    enums[(*count)++] = sym;
    return true;
}

static MO_MUST_USE bool check_statuses(struct api_check *p, const struct mo_syntax *clauses) {
    uint32_t max_enums = 1 + p->api->route_count;
    const struct mo_symbol **enums =
        api_alloc(p, max_enums, sizeof(*enums), _Alignof(const struct mo_symbol *));
    if (enums == NULL)
        return false;
    uint32_t enum_count = 0;
    if (p->api->has_auth && !add_error_enum(p, p->api->auth.error_type, enums, &enum_count))
        return false;
    for (uint32_t i = 0; i < p->api->route_count; i++)
        if (!add_error_enum(p, p->api->routes[i].error_type, enums, &enum_count))
            return false;

    uint32_t status_count = 0;
    for (uint32_t i = 0; i < clauses->count; i++)
        if (clauses->children[i]->kind == MO_S_API_STATUS)
            status_count++;
    if (status_count > MO_LIMIT_API_STATUSES) {
        p->span = p->api->syntax->span;
        return api_report(p, MO_CODE_API_STATUS, "an api block has too many status clauses",
                          "An api block has at most 256 status clauses.");
    }
    struct mo_checked_api_status *statuses =
        api_alloc(p, status_count == 0 ? 1 : status_count, sizeof(*statuses),
                  _Alignof(struct mo_checked_api_status));
    if (statuses == NULL)
        return false;

    uint32_t at = 0;
    const struct mo_module_scope *own_scope = &p->program->interfaces->scopes[p->api->module];
    for (uint32_t i = 0; i < clauses->count; i++) {
        const struct mo_syntax *clause = clauses->children[i];
        if (clause->kind != MO_S_API_STATUS)
            continue;
        p->span = clause->children[2]->span;
        uint16_t code = 0;
        if (!parse_u16_literal(p, clause->children[2], &code) || code < 100 || code > 599)
            return api_report(p, MO_CODE_API_STATUS,
                              "an api status code must be an HTTP status from 100 to 599",
                              "Use a standard HTTP status code such as 400, 401, 404 or 409.");

        const struct mo_syntax *variant_node = clause->children[0];
        p->span = variant_node->span;
        const struct mo_symbol *matched_enum = NULL;
        const struct mo_symbol *matched_case = NULL;

        if (variant_node->kind == MO_S_QUALIFIED) {
            const struct mo_syntax *prefix = variant_node->children[0];
            const struct mo_syntax *cname = variant_node->children[1];
            size_t plen = prefix->span.end - prefix->span.start;
            size_t clen = cname->span.end - cname->span.start;
            uint32_t alias_module = UINT32_MAX;
            for (uint32_t a = 0; a < own_scope->alias_count; a++)
                if (strlen(own_scope->aliases[a].name) == plen &&
                    memcmp(own_scope->aliases[a].name, prefix->text, plen) == 0)
                    alias_module = own_scope->aliases[a].module;
            for (uint32_t e = 0; e < enum_count; e++) {
                const struct mo_symbol *en = enums[e];
                bool prefix_ok =
                    (en->name_length == plen && memcmp(en->name, prefix->text, plen) == 0) ||
                    (alias_module != UINT32_MAX && en->module == alias_module);
                if (!prefix_ok)
                    continue;
                const struct mo_module_scope *escope = &p->program->interfaces->scopes[en->module];
                for (uint32_t c = 0; c < en->case_count; c++) {
                    const struct mo_symbol *cs = &escope->symbols[en->cases[c]];
                    if (cs->name_length == clen && memcmp(cs->name, cname->text, clen) == 0) {
                        if (matched_case != NULL)
                            return api_report(p, MO_CODE_API_STATUS,
                                              "a status clause matches multiple error cases", NULL);
                        matched_enum = en;
                        matched_case = cs;
                    }
                }
            }
        } else {
            size_t clen = variant_node->span.end - variant_node->span.start;
            for (uint32_t e = 0; e < enum_count; e++) {
                const struct mo_symbol *en = enums[e];
                const struct mo_module_scope *escope = &p->program->interfaces->scopes[en->module];
                for (uint32_t c = 0; c < en->case_count; c++) {
                    const struct mo_symbol *cs = &escope->symbols[en->cases[c]];
                    if (cs->name_length == clen &&
                        memcmp(cs->name, variant_node->text, clen) == 0) {
                        if (matched_case != NULL)
                            return api_report(
                                p, MO_CODE_API_STATUS,
                                "a status case name belongs to multiple api error enums",
                                "Qualify the case name as EnumName.CaseName.");
                        matched_enum = en;
                        matched_case = cs;
                    }
                }
            }
        }

        if (matched_case == NULL)
            return api_report(
                p, MO_CODE_API_STATUS,
                "a status clause names an error case not returned by any route or auth function",
                "Map only cases of error enums returned by this api's routes or auth function.");

        const struct mo_data_signature *cdata =
            mo_data_signature_by_symbol(p->program->signatures, matched_case);
        MO_ASSERT(cdata != NULL);
        const struct mo_syntax *wildcards = clause->children[1];
        if (cdata->count == 0 && wildcards != NULL)
            return api_report(p, MO_CODE_API_STATUS,
                              "a payload-free error case cannot take (_) in status",
                              "Write status CaseName = code without parentheses.");
        if (cdata->count != 0 &&
            (wildcards == NULL || (wildcards->count != cdata->count && wildcards->count != 1)))
            return api_report(p, MO_CODE_API_STATUS,
                              "an error case with fields needs (_) in status",
                              "Write status CaseName(_) = code for error cases carrying data.");

        for (uint32_t prev = 0; prev < at; prev++)
            if (statuses[prev].error_enum == matched_enum && statuses[prev].variant == matched_case)
                return api_report(p, MO_CODE_API_STATUS, "a status clause maps the same case twice",
                                  "Map each error enum case at most once.");

        statuses[at++] = (struct mo_checked_api_status){.syntax = clause,
                                                        .error_enum = matched_enum,
                                                        .variant = matched_case,
                                                        .status_code = code};
    }

    p->span = p->api->syntax->span;
    for (uint32_t e = 0; e < enum_count; e++) {
        const struct mo_symbol *en = enums[e];
        const struct mo_module_scope *escope = &p->program->interfaces->scopes[en->module];
        for (uint32_t c = 0; c < en->case_count; c++) {
            const struct mo_symbol *cs = &escope->symbols[en->cases[c]];
            bool covered = false;
            for (uint32_t s = 0; s < at; s++)
                if (statuses[s].error_enum == en && statuses[s].variant == cs) {
                    covered = true;
                    break;
                }
            if (!covered) {
                char *msg = api_alloc(p, 192, 1, 1);
                if (msg == NULL)
                    return false;
                (void)snprintf(msg, 192, "an api error case has no status clause: %s.%s", en->name,
                               cs->name);
                return api_report(
                    p, MO_CODE_API_STATUS, msg,
                    "Every case of every error enum returned by an api must map to an HTTP status "
                    "code.");
            }
        }
    }

    p->api->status_count = at;
    p->api->statuses = statuses;
    return true;
}

static MO_MUST_USE const struct mo_source_type *
find_library_data_type(const struct mo_program *program, const char *mod_name,
                       const char *type_name) {
    for (uint32_t m = 0; m < program->interfaces->count; m++) {
        const struct mo_module_scope *scope = &program->interfaces->scopes[m];
        if (!scope->library || scope->name == NULL || strcmp(scope->name, mod_name) != 0)
            continue;
        const struct mo_symbol *sym =
            mo_find_symbol(scope, (const uint8_t *)type_name, strlen(type_name));
        if (sym == NULL)
            return NULL;
        const struct mo_nominal_signature *nom =
            mo_nominal_signature_by_symbol(program->signatures, sym);
        return nom == NULL ? NULL : nom->type;
    }
    return NULL;
}

bool mo_check_apis(struct mo_arena *arena, struct mo_program *program,
                   struct mo_ability_context *abilities, struct mo_diags *diags) {
    struct api_check p = {.arena = arena,
                          .program = program,
                          .abilities = abilities,
                          .diags = diags,
                          .span = program->set->modules[0]->span};

    bool any_api = false;
    for (uint32_t m = 0; m < program->set->count; m++) {
        const struct mo_syntax *items = program->set->modules[m]->tree->children[1];
        for (uint32_t i = 0; i < items->count; i++) {
            if (!api_work(&p, 1))
                return false;
            if (items->children[i]->kind == MO_S_API)
                any_api = true;
        }
    }
    if (!any_api)
        return true;

    p.req_type = find_library_data_type(program, "Api", "Request");
    p.res_type = find_library_data_type(program, "Api", "Response");
    const struct mo_source_type *json_type = find_library_data_type(program, "Json", "Json");
    if (p.req_type == NULL || p.res_type == NULL || json_type == NULL)
        return api_report(&p, MO_CODE_IMPORT_MISSING,
                          "an api block needs the Api and Json standard library modules",
                          "Load the program with the standard library enabled.");

    struct mo_checked_api *apis =
        api_alloc(&p, program->set->count, sizeof(*apis), _Alignof(struct mo_checked_api));
    p.functions = api_alloc(&p, program->function_count == 0 ? 1 : program->function_count,
                            sizeof(*p.functions), _Alignof(uint32_t));
    if (apis == NULL || p.functions == NULL)
        return false;
    for (uint32_t i = 0; i < program->function_count; i++)
        p.functions[program->functions[i].signature - program->signatures->items] = i;
    program->apis = apis;

    for (uint32_t module = 0; module < program->set->count; module++) {
        const struct mo_syntax *items = program->set->modules[module]->tree->children[1];
        const struct mo_syntax *syntax = NULL;
        bool has_service = false;
        for (uint32_t i = 0; i < items->count; i++) {
            if (!api_work(&p, 1))
                return false;
            const struct mo_syntax *item = items->children[i];
            if (item->kind == MO_S_SERVICE)
                has_service = true;
            if (item->kind != MO_S_API)
                continue;
            p.span = item->span;
            if (syntax != NULL)
                return api_report(&p, MO_CODE_API_BLOCK, "a module declares two api blocks",
                                  "A module has at most one api block.");
            syntax = item;
        }
        if (syntax == NULL)
            continue;
        p.span = syntax->span;
        if (has_service)
            return api_report(&p, MO_CODE_API_BLOCK, "a module cannot declare both service and api",
                              "Put stateful actors/services in their own module and import them.");

        memset(p.cap_used, 0, sizeof p.cap_used);
        p.api = &apis[program->api_count++];
        *p.api = (struct mo_checked_api){.syntax = syntax, .module = module, .port = 8000};

        const struct mo_syntax *clauses = syntax->children[0];
        uint32_t sqlite_count = 0, actor_count = 0, route_count = 0;
        const struct mo_syntax *auth_clause = NULL;
        for (uint32_t i = 0; i < clauses->count; i++) {
            if (!api_work(&p, 1))
                return false;
            const struct mo_syntax *c = clauses->children[i];
            p.span = c->span;
            if (c->kind == MO_S_API_PORT) {
                if (p.api->has_port)
                    return api_report(&p, MO_CODE_API_BLOCK, "an api block repeats port",
                                      "Write at most one port clause in an api block.");
                uint16_t port = 0;
                p.span = c->children[0]->span;
                if (!parse_u16_literal(&p, c->children[0], &port))
                    return api_report(&p, MO_CODE_API_BLOCK,
                                      "an api port must be an integer from 0 to 65535",
                                      "Use a valid TCP port number such as 8000.");
                p.api->has_port = true;
                p.api->port = port;
            } else if (c->kind == MO_S_API_CORS) {
                if (p.api->cors != NULL)
                    return api_report(&p, MO_CODE_API_BLOCK, "an api block repeats cors",
                                      "Write at most one cors clause in an api block.");
                p.span = c->children[0]->span;
                struct mo_map_literal_key cors_val;
                if (!decode_plain_text(&p, c->children[0], &cors_val))
                    return false;
                bool valid_cors = cors_val.length != 0;
                for (size_t k = 0; k < cors_val.length; k++)
                    if (cors_val.bytes[k] == '\0' || cors_val.bytes[k] == '\r' ||
                        cors_val.bytes[k] == '\n')
                        valid_cors = false;
                if (!valid_cors)
                    return api_report(&p, MO_CODE_API_BLOCK,
                                      "an api cors origin must be a nonempty single-line text",
                                      "Write cors \"*\" or an origin URL such as "
                                      "\"https://app.example.com\".");
                p.api->cors = api_copy_text(&p, cors_val.bytes, cors_val.length);
                if (p.api->cors == NULL)
                    return false;
            } else if (c->kind == MO_S_API_SQLITE) {
                if (++sqlite_count > MO_LIMIT_API_SQLITES)
                    return api_report(&p, MO_CODE_API_BLOCK,
                                      "an api block declares too many sqlite databases",
                                      "An api block declares at most 16 sqlite clauses.");
            } else if (c->kind == MO_S_API_ACTOR) {
                if (++actor_count > MO_LIMIT_API_ACTORS)
                    return api_report(&p, MO_CODE_API_BLOCK,
                                      "an api block declares too many actors",
                                      "An api block declares at most 16 actor clauses.");
            } else if (c->kind == MO_S_API_AUTH) {
                if (auth_clause != NULL)
                    return api_report(&p, MO_CODE_API_BLOCK, "an api block repeats auth",
                                      "Write at most one auth <Type> via <fn> clause.");
                auth_clause = c;
            } else if (c->kind == MO_S_API_ROUTE) {
                if (++route_count > MO_LIMIT_API_ROUTES)
                    return api_report(&p, MO_CODE_API_ROUTE, "an api block has too many routes",
                                      "An api block declares at most 1024 routes.");
            } else {
                MO_ASSERT(c->kind == MO_S_API_STATUS);
            }
        }

        if (route_count == 0) {
            p.span = syntax->span;
            return api_report(&p, MO_CODE_API_BLOCK, "an api block has no routes",
                              "Declare at least one get, post, put, patch or delete route.");
        }

        struct mo_checked_api_sqlite *sqlites =
            api_alloc(&p, sqlite_count == 0 ? 1 : sqlite_count, sizeof(*sqlites),
                      _Alignof(struct mo_checked_api_sqlite));
        if (sqlites == NULL)
            return false;
        uint32_t sq_at = 0;
        for (uint32_t i = 0; i < clauses->count; i++) {
            const struct mo_syntax *c = clauses->children[i];
            if (c->kind != MO_S_API_SQLITE)
                continue;
            p.span = c->children[0]->span;
            const char *sname = api_syntax_cstr(&p, c->children[0]);
            if (sname == NULL)
                return false;
            for (uint32_t prev = 0; prev < sq_at; prev++)
                if (strcmp(sqlites[prev].name, sname) == 0)
                    return api_report(&p, MO_CODE_API_BLOCK,
                                      "an api block declares two sqlite clauses with the same name",
                                      "Give each sqlite database binding a distinct name.");
            p.span = c->children[1]->span;
            struct mo_map_literal_key spath;
            if (!decode_plain_text(&p, c->children[1], &spath))
                return false;
            if (!valid_sqlite_path(spath.bytes, spath.length))
                return api_report(&p, MO_CODE_API_BLOCK,
                                  "an api sqlite path must be a relative path without ..",
                                  "Use a relative filename inside the data folder, such as "
                                  "\"app.db\".");
            const char *copied_path = api_copy_text(&p, spath.bytes, spath.length);
            if (copied_path == NULL)
                return false;
            sqlites[sq_at++] =
                (struct mo_checked_api_sqlite){.syntax = c, .name = sname, .path = copied_path};
        }
        p.api->sqlite_count = sq_at;
        p.api->sqlites = sqlites;
        if (sq_at != 0)
            p.cap_used[0] = true; /* Sqlite capability opens declared sqlite bindings */

        struct mo_checked_api_actor *actors =
            api_alloc(&p, actor_count == 0 ? 1 : actor_count, sizeof(*actors),
                      _Alignof(struct mo_checked_api_actor));
        if (actors == NULL)
            return false;
        uint32_t ac_at = 0;
        for (uint32_t i = 0; i < clauses->count; i++) {
            const struct mo_syntax *c = clauses->children[i];
            if (c->kind != MO_S_API_ACTOR)
                continue;
            p.span = c->children[0]->span;
            const char *aname = api_syntax_cstr(&p, c->children[0]);
            if (aname == NULL)
                return false;
            for (uint32_t prev = 0; prev < sq_at; prev++)
                if (strcmp(sqlites[prev].name, aname) == 0)
                    return api_report(
                        &p, MO_CODE_API_BLOCK,
                        "an api actor binding has the same name as a sqlite binding",
                        "Give each sqlite and actor clause in an api block a distinct name.");
            for (uint32_t prev = 0; prev < ac_at; prev++)
                if (strcmp(actors[prev].name, aname) == 0)
                    return api_report(&p, MO_CODE_API_BLOCK,
                                      "an api block declares two actor clauses with the same name",
                                      "Give each actor binding in an api block a distinct name.");
            p.span = c->children[1]->span;
            const struct mo_source_type *atype = mo_resolve_service_type(
                p.arena, p.program->interfaces, p.program->signatures, p.api->module, c, p.diags);
            if (atype == NULL) {
                p.failed = true;
                return false;
            }
            if (!is_actor_type(atype))
                return api_report(
                    &p, MO_CODE_API_BLOCK, "an api actor clause must name a declared actor type",
                    "Declare actor Name ... end and reference Name in actor <binding>: Name.");
            for (uint32_t prev = 0; prev < ac_at; prev++)
                if (actors[prev].type->declaration == atype->declaration)
                    return api_report(
                        &p, MO_CODE_API_BLOCK,
                        "an api block declares two actor clauses of the same actor type",
                        "Declare at most one actor clause per actor type in an api block.");
            uint32_t svc_idx = UINT32_MAX;
            for (uint32_t s = 0; s < program->service_count; s++)
                if (program->services[s].is_actor &&
                    program->services[s].actor_symbol == atype->declaration) {
                    svc_idx = s;
                    break;
                }
            MO_ASSERT(svc_idx != UINT32_MAX);
            actors[ac_at++] = (struct mo_checked_api_actor){
                .syntax = c, .name = aname, .type = atype, .service_index = svc_idx};
        }
        p.api->actor_count = ac_at;
        p.api->actors = actors;

        if (auth_clause != NULL && !check_auth_clause(&p, auth_clause))
            return false;

        struct mo_checked_api_route *routes =
            api_alloc(&p, route_count, sizeof(*routes), _Alignof(struct mo_checked_api_route));
        const char **shapes = api_alloc(&p, route_count, sizeof(*shapes), _Alignof(const char *));
        if (routes == NULL || shapes == NULL)
            return false;
        p.api->routes = routes;

        uint32_t rt_at = 0;
        for (uint32_t i = 0; i < clauses->count; i++) {
            const struct mo_syntax *c = clauses->children[i];
            if (c->kind != MO_S_API_ROUTE)
                continue;
            if (!check_route_clause(&p, c, shapes, rt_at, &routes[rt_at]))
                return false;
            rt_at++;
        }
        p.api->route_count = rt_at;

        if (!check_statuses(&p, clauses))
            return false;

        uint32_t cap_map[API_CAP_KIND_COUNT];
        uint32_t cap_count = 0;
        for (uint32_t c = 0; c < API_CAP_KIND_COUNT; c++) {
            if (p.cap_used[c])
                cap_map[c] = cap_count++;
            else
                cap_map[c] = UINT32_MAX;
        }
        const char **caps =
            api_alloc(&p, cap_count == 0 ? 1 : cap_count, sizeof(*caps), _Alignof(const char *));
        if (caps == NULL)
            return false;
        uint32_t cap_at = 0;
        for (uint32_t c = 0; c < API_CAP_KIND_COUNT; c++)
            if (p.cap_used[c])
                caps[cap_at++] = api_cap_order[c];
        p.api->capability_count = cap_count;
        p.api->capabilities = caps;

        if (p.api->has_auth) {
            struct mo_checked_api_param *auth_params =
                (struct mo_checked_api_param *)(uintptr_t)p.api->auth.params;
            for (uint32_t k = 0; k < p.api->auth.param_count; k++)
                if (auth_params[k].kind == MO_API_PARAM_CAPABILITY)
                    auth_params[k].index = cap_map[auth_params[k].index];
        }
        for (uint32_t r = 0; r < p.api->route_count; r++) {
            struct mo_checked_api_param *rparams =
                (struct mo_checked_api_param *)(uintptr_t)routes[r].params;
            for (uint32_t k = 0; k < routes[r].param_count; k++)
                if (rparams[k].kind == MO_API_PARAM_CAPABILITY)
                    rparams[k].index = cap_map[rparams[k].index];
        }
    }
    return true;
}
