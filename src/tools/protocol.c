#include "tools/protocol.h"

#include "base/limits.h"
#include "base/stack.h"
#include "check/check.h"
#include "codegen/codegen.h"
#include "heap/types.h"
#include "lower/lower.h"
#include "tools/protocol-sources.h"

#include <string.h>

enum abi_type {
    ABI_INT,
    ABI_BOOL,
    ABI_BYTE,
    ABI_BYTES,
    ABI_SPAN,
    ABI_FIELD,
    ABI_FIELDS,
    ABI_HEAD,
    ABI_LIMITS,
    ABI_SCAN,
    ABI_PROBLEM,
    ABI_HEAD_RESULT,
    ABI_SCAN_RESULT,
    ABI_SPAN_OPTION
};

struct abi_field {
    const char *name;
    enum abi_type type;
};

struct abi_case {
    const char *name;
    uint32_t count;
    struct abi_field fields[5];
};

static const struct abi_case span[] = {{"Span", 2, {{"start", ABI_INT}, {"size", ABI_INT}}}};
static const struct abi_case field[] = {{"Field", 2, {{"name", ABI_SPAN}, {"value", ABI_SPAN}}}};
static const struct abi_case head[] = {{"Head",
                                        5,
                                        {{"method", ABI_SPAN},
                                         {"target", ABI_SPAN},
                                         {"version", ABI_SPAN},
                                         {"fields", ABI_FIELDS},
                                         {"after", ABI_INT}}}};
static const struct abi_case bounds[] = {
    {"HeaderLimits", 3, {{"bytes", ABI_INT}, {"line", ABI_INT}, {"fields", ABI_INT}}}};
static const struct abi_case scan[] = {
    {"HeaderScan", 3, {{"matched", ABI_INT}, {"size", ABI_INT}, {"complete", ABI_BOOL}}}};
static const struct abi_case problem[] = {
    {.name = "Incomplete"}, {.name = "Malformed"}, {.name = "TooLarge"}};
static const struct abi_case head_result[] = {{"Ok", 1, {{NULL, ABI_HEAD}}},
                                              {"Error", 1, {{NULL, ABI_PROBLEM}}}};
static const struct abi_case scan_result[] = {{"Ok", 1, {{NULL, ABI_SCAN}}},
                                              {"Error", 1, {{NULL, ABI_PROBLEM}}}};
static const struct abi_case span_option[] = {{.name = "None"}, {"Some", 1, {{NULL, ABI_SPAN}}}};

static const struct {
    enum mo_type_kind kind;
    enum abi_type element;
    uint32_t count;
    const struct abi_case *cases;
} abi[] = {
    [ABI_INT] = {.kind = MO_TYPE_INT},
    [ABI_BOOL] = {.kind = MO_TYPE_BOOL},
    [ABI_BYTE] = {.kind = MO_TYPE_BYTE},
    [ABI_BYTES] = {.kind = MO_TYPE_LIST, .element = ABI_BYTE},
    [ABI_SPAN] = {.kind = MO_TYPE_CHOICE, .count = 1, .cases = span},
    [ABI_FIELD] = {.kind = MO_TYPE_CHOICE, .count = 1, .cases = field},
    [ABI_FIELDS] = {.kind = MO_TYPE_LIST, .element = ABI_FIELD},
    [ABI_HEAD] = {.kind = MO_TYPE_CHOICE, .count = 1, .cases = head},
    [ABI_LIMITS] = {.kind = MO_TYPE_CHOICE, .count = 1, .cases = bounds},
    [ABI_SCAN] = {.kind = MO_TYPE_CHOICE, .count = 1, .cases = scan},
    [ABI_PROBLEM] = {.kind = MO_TYPE_CHOICE, .count = 3, .cases = problem},
    [ABI_HEAD_RESULT] = {.kind = MO_TYPE_CHOICE, .count = 2, .cases = head_result},
    [ABI_SCAN_RESULT] = {.kind = MO_TYPE_CHOICE, .count = 2, .cases = scan_result},
    [ABI_SPAN_OPTION] = {.kind = MO_TYPE_CHOICE, .count = 2, .cases = span_option},
};

static bool matches(const struct mo_types *types, uint32_t index, enum abi_type expected) {
    const struct mo_type *type = &types->items[index];
    if (type->kind != abi[expected].kind)
        return false;
    if (type->kind == MO_TYPE_LIST)
        return type->parameter_count == 1 &&
               matches(types, type->parameters[0], abi[expected].element);
    if (type->kind != MO_TYPE_CHOICE)
        return true;
    if (type->case_count != abi[expected].count)
        return false;
    for (uint32_t i = 0; i < type->case_count; i++) {
        const struct mo_type_case *actual = &type->cases[i];
        const struct abi_case *wanted = &abi[expected].cases[i];
        if (strcmp(actual->name, wanted->name) != 0 || actual->field_count != wanted->count ||
            actual->unnamed != (wanted->count == 1 && wanted->fields[0].name == NULL))
            return false;
        for (uint32_t j = 0; j < actual->field_count; j++) {
            const char *a = actual->fields[j].name, *b = wanted->fields[j].name;
            if ((a == NULL) != (b == NULL) || (a != NULL && strcmp(a, b) != 0) ||
                !matches(types, actual->fields[j].type, wanted->fields[j].type))
                return false;
        }
    }
    return true;
}

static bool valid_abi(const struct mo_image *image) {
    static const struct {
        const char *name;
        uint32_t count;
        enum abi_type parameters[3], result;
    } functions[] = {
        {"Http.scan_headers", 3, {ABI_BYTES, ABI_SCAN, ABI_INT}, ABI_SCAN_RESULT},
        {"Http.parse_headers", 2, {ABI_BYTES, ABI_LIMITS}, ABI_HEAD_RESULT},
        {"Http.bearer_span", 2, {ABI_BYTES, ABI_HEAD}, ABI_SPAN_OPTION},
    };
    const struct mo_bytecode *file = mo_image_bytecode(image);
    const struct mo_types *types = mo_image_types(image);
    for (size_t i = 0; i < sizeof functions / sizeof *functions; i++) {
        bool found = false;
        for (uint32_t j = 0; j < file->function_count; j++) {
            const struct mo_bytecode_function *function = &file->functions[j];
            const struct mo_bytecode_string *name = &file->strings[function->name];
            if (name->length != strlen(functions[i].name) ||
                memcmp(name->bytes, functions[i].name, name->length) != 0)
                continue;
            if (found || function->kind != MO_FUNCTION_FUNCTION || function->spec_code ||
                function->capture_count != 0 || function->parameter_count != functions[i].count ||
                !matches(types, function->result_type, functions[i].result))
                return false;
            for (uint32_t k = 0; k < function->parameter_count; k++)
                if (!matches(types, function->parameters[k].type, functions[i].parameters[k]))
                    return false;
            found = true;
        }
        if (!found)
            return false;
    }
    return true;
}

static void build(void *context) {
    struct mo_image **image = context;
    struct mo_arena input, output;
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_buffer bytes;
    mo_arena_init(&input, MO_LIMIT_LOWER_WORK);
    mo_arena_init(&output, MO_LIMIT_LOWER_WORK);
    mo_buffer_init(&bytes, MO_LIMIT_BYTECODE_SIZE);
    if (!mo_sources_init(&sources))
        goto arenas;
    if (!mo_diags_init(&diags))
        goto sources;
    bool library[sizeof(mo_protocol_sources) / sizeof(*mo_protocol_sources)];
    for (size_t i = 0; i < sizeof(mo_protocol_sources) / sizeof(*mo_protocol_sources); i++) {
        uint32_t file;
        struct mo_source_error error;
        if (!mo_sources_add_bytes(&sources, mo_protocol_sources[i].path,
                                  mo_protocol_sources[i].bytes, mo_protocol_sources[i].size, &file,
                                  &error))
            goto done;
        library[i] = strncmp(mo_protocol_sources[i].path, "lib/", 4) == 0;
    }
    struct mo_module_set *set = mo_load_program_sources(&input, &sources, library, &diags);
    if (set == NULL || mo_diags_any(&diags))
        goto done;
    struct mo_program *program = mo_check(&input, set, &diags);
    if (program == NULL || mo_diags_any(&diags))
        goto done;
    struct mo_core *core = mo_lower(&output, program, &sources, &diags);
    if (core == NULL || mo_diags_any(&diags) || !mo_core_check(core, &diags) ||
        !mo_compile(core, &bytes, &diags) || mo_diags_any(&diags))
        goto done;
    struct mo_load_error error;
    *image = mo_load(bytes.bytes, bytes.length, &error);
done:
    mo_diags_free(&diags);
sources:
    mo_sources_free(&sources);
arenas:
    mo_buffer_free(&bytes);
    mo_arena_free(&output);
    mo_arena_free(&input);
}

struct mo_image *mo_protocol_build(void) {
    struct mo_image *image = NULL;
    if (mo_with_stack(MO_SOURCE_STACK_SIZE, build, &image) != 0)
        return NULL;
    if (image != NULL && !valid_abi(image)) {
        mo_image_free(image);
        return NULL;
    }
    return image;
}

bool mo_protocol_body_marks(const uint8_t *bytes, size_t size, size_t *count) {
    MO_ASSERT((bytes != NULL || size == 0) && count != NULL);
    *count = 0;
    if (size > MO_LIMIT_MCP_BODY_BYTES)
        return false;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < size; i++) {
        uint8_t c = bytes[i];
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                quoted = false;
        } else if (c == '"')
            quoted = true;
        else if (c == '[' || c == '{' || c == ',')
            (*count)++;
    }
    return true;
}
