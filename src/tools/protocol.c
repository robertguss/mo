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
    ABI_SPAN_OPTION,
    ABI_INT_RESULT,
    ABI_TEXT,
    ABI_INTS,
    ABI_INT_OPTION,
    ABI_MCP_KIND,
    ABI_MCP_KINDS,
    ABI_MCP_FIELD,
    ABI_MCP_FIELDS,
    ABI_MCP_CASE,
    ABI_MCP_CASES,
    ABI_MCP_PAYLOAD,
    ABI_MCP_TOOL,
    ABI_MCP_TOOLS,
    ABI_MCP_SERVICE,
    ABI_JSON,
    ABI_JSON_NUMBER,
    ABI_JSON_ARRAY,
    ABI_JSON_OBJECT,
    ABI_FLOAT,
    ABI_CHAR,
    ABI_TIME,
    ABI_DECIMAL,
    ABI_BIGINT,
    ABI_SCALAR,
    ABI_SCALAR_OPTION,
    ABI_VALUE,
    ABI_VALUES,
    ABI_VALUE_PROBLEM,
    ABI_VALUE_RESULT,
    ABI_VALUES_RESULT,
    ABI_JSON_RESULT,
    ABI_TEXT_OPTION,
    ABI_TEXTS,
    ABI_JSON_OPTION,
    ABI_RPC_REQUEST,
    ABI_RPC_ENVELOPE,
    ABI_RPC_METHOD,
    ABI_CATALOG,
    ABI_CATALOG_OPTION,
    ABI_SPANS,
    ABI_INSPECTION,
    ABI_BODY_PLAN,
    ABI_BODY_PHASE,
    ABI_BODY_STATE,
    ABI_BODY_BOUNDS,
    ABI_BODY_STEP,
    ABI_BODY_RESULT,
    ABI_ACTION,
    ABI_TEXT_PAIR,
    ABI_COUNT
};

struct abi_field {
    const char *name;
    enum abi_type type;
};

struct abi_case {
    const char *name;
    uint32_t count;
    struct abi_field fields[6];
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
static const struct abi_case int_result[] = {{"Ok", 1, {{NULL, ABI_INT}}},
                                             {"Error", 1, {{NULL, ABI_PROBLEM}}}};
static const struct abi_case int_option[] = {{.name = "None"}, {"Some", 1, {{NULL, ABI_INT}}}};
static const struct abi_case mcp_kind[] = {
    {.name = "UnitType"},
    {.name = "BoolType"},
    {.name = "ByteType"},
    {.name = "IntType"},
    {.name = "FloatType"},
    {.name = "CharType"},
    {.name = "TimeType"},
    {.name = "DurationType"},
    {.name = "DecimalType"},
    {.name = "TextType"},
    {.name = "BytesType"},
    {.name = "BigIntType"},
    {"TupleType", 1, {{"0", ABI_INTS}}},
    {"ListType", 1, {{"0", ABI_INT}}},
    {"SetType", 1, {{"0", ABI_INT}}},
    {"MapType", 2, {{"key", ABI_INT}, {"value", ABI_INT}}},
    {.name = "RangeType"},
    {"StructType", 1, {{"0", ABI_MCP_FIELDS}}},
    {"ChoiceType", 1, {{"0", ABI_MCP_CASES}}},
};
static const struct abi_case mcp_field[] = {
    {"Field", 2, {{"name", ABI_TEXT}, {"type_index", ABI_INT}}}};
static const struct abi_case mcp_case[] = {
    {"Case", 2, {{"name", ABI_TEXT}, {"payload", ABI_MCP_PAYLOAD}}}};
static const struct abi_case mcp_payload[] = {
    {.name = "Empty"}, {"Unnamed", 1, {{"0", ABI_INT}}}, {"Named", 1, {{"0", ABI_MCP_FIELDS}}}};
static const struct abi_case mcp_tool[] = {{"Tool",
                                            6,
                                            {{"name", ABI_TEXT},
                                             {"description", ABI_TEXT},
                                             {"read_only", ABI_BOOL},
                                             {"arguments", ABI_MCP_FIELDS},
                                             {"reply", ABI_INT},
                                             {"error", ABI_INT_OPTION}}}};
static const struct abi_case mcp_service[] = {
    {"Service", 2, {{"types", ABI_MCP_KINDS}, {"tools", ABI_MCP_TOOLS}}}};
static const struct abi_case json[] = {{.name = "Null"},
                                       {"Boolean", 1, {{"0", ABI_BOOL}}},
                                       {"Number", 1, {{"0", ABI_JSON_NUMBER}}},
                                       {"String", 1, {{"0", ABI_TEXT}}},
                                       {"Array", 1, {{"0", ABI_JSON_ARRAY}}},
                                       {"Object", 1, {{"0", ABI_JSON_OBJECT}}}};
static const struct abi_case json_number[] = {{"JsonNumber", 1, {{"text", ABI_TEXT}}}};

static const struct abi_case scalar[] = {
    {.name = "UnitValue"},
    {"BoolValue", 1, {{"0", ABI_BOOL}}},
    {"ByteValue", 1, {{"0", ABI_BYTE}}},
    {"IntValue", 1, {{"0", ABI_INT}}},
    {"FloatValue", 1, {{"0", ABI_FLOAT}}},
    {"CharValue", 1, {{"0", ABI_CHAR}}},
    {"TimeValue", 1, {{"0", ABI_TIME}}},
    {"DurationValue", 1, {{"0", ABI_INT}}},
    {"DecimalValue", 1, {{"0", ABI_DECIMAL}}},
    {"TextValue", 1, {{"0", ABI_TEXT}}},
    {"BytesValue", 1, {{"0", ABI_BYTES}}},
    {"BigIntValue", 1, {{"0", ABI_BIGINT}}},
    {"RangeValue", 2, {{"start", ABI_INT}, {"finish", ABI_INT}}},
};
static const struct abi_case scalar_option[] = {{.name = "None"},
                                                {"Some", 1, {{NULL, ABI_SCALAR}}}};

static const struct abi_case value[] = {
    {"Leaf", 1, {{"0", ABI_SCALAR}}},
    {"Sequence", 1, {{"0", ABI_VALUES}}},
    {"Variant", 2, {{"index", ABI_INT}, {"fields", ABI_VALUES}}},
};
static const struct abi_case value_problem[] = {{.name = "Invalid"}, {.name = "Limit"}};
static const struct abi_case value_result[] = {{"Ok", 1, {{NULL, ABI_VALUE}}},
                                               {"Error", 1, {{NULL, ABI_VALUE_PROBLEM}}}};
static const struct abi_case values_result[] = {{"Ok", 1, {{NULL, ABI_VALUES}}},
                                                {"Error", 1, {{NULL, ABI_VALUE_PROBLEM}}}};
static const struct abi_case json_result[] = {{"Ok", 1, {{NULL, ABI_JSON}}},
                                              {"Error", 1, {{NULL, ABI_VALUE_PROBLEM}}}};

static const struct abi_case text_option[] = {{.name = "None"}, {"Some", 1, {{NULL, ABI_TEXT}}}};
static const struct abi_case json_option[] = {{.name = "None"}, {"Some", 1, {{NULL, ABI_JSON}}}};
static const struct abi_case rpc_request[] = {
    {"Request",
     4,
     {{"id", ABI_JSON}, {"id_text", ABI_TEXT}, {"method", ABI_TEXT}, {"params", ABI_JSON}}}};
static const struct abi_case rpc_envelope[] = {
    {"Rejected", 1, {{"0", ABI_INT}}},
    {.name = "WrongVersion"},
    {.name = "Unsolicited"},
    {"Notification", 1, {{"params", ABI_JSON}}},
    {"Message", 1, {{"0", ABI_RPC_REQUEST}}},
};
static const struct abi_case rpc_method[] = {
    {.name = "InvalidParams"},
    {.name = "UnknownMethod"},
    {.name = "Initialize"},
    {.name = "Ping"},
    {"ListTools", 1, {{"0", ABI_INT}}},
    {"Call", 2, {{"tool", ABI_INT}, {"arguments", ABI_JSON}}},
};
static const struct abi_case catalog[] = {{"Catalog",
                                           4,
                                           {{"tools", ABI_JSON_ARRAY},
                                            {"sizes", ABI_INTS},
                                            {"image_key", ABI_TEXT},
                                            {"response_bytes", ABI_INT}}}};
static const struct abi_case catalog_option[] = {{.name = "None"},
                                                 {"Some", 1, {{NULL, ABI_CATALOG}}}};

static const struct abi_case inspection[] = {{"Ok", 1, {{NULL, ABI_SPAN_OPTION}}},
                                             {"Error", 1, {{NULL, ABI_INT}}}};
static const struct abi_case body_plan[] = {
    {"Reject", 1, {{"0", ABI_INT}}},
    {"Fixed", 3, {{"length", ABI_INT}, {"send_continue", ABI_BOOL}, {"version", ABI_TEXT_OPTION}}},
    {"Chunked", 2, {{"send_continue", ABI_BOOL}, {"version", ABI_TEXT_OPTION}}}};
static const struct abi_case body_phase[] = {{"Fixed", 1, {{"0", ABI_INT}}},
                                             {"SizeLine", 1, {{"0", ABI_BYTES}}},
                                             {"Data", 1, {{"0", ABI_INT}}},
                                             {.name = "DataCr"},
                                             {.name = "DataLf"},
                                             {"TrailerLine", 1, {{"0", ABI_BYTES}}},
                                             {.name = "Complete"}};
static const struct abi_case body_state[] = {{"State",
                                              6,
                                              {{"phase", ABI_BODY_PHASE},
                                               {"encoded", ABI_INT},
                                               {"decoded", ABI_INT},
                                               {"chunks", ABI_INT},
                                               {"trailer_bytes", ABI_INT},
                                               {"trailer_fields", ABI_INT}}}};
static const struct abi_case body_bounds[] = {{"Bounds",
                                               6,
                                               {{"body", ABI_INT},
                                                {"ingress", ABI_INT},
                                                {"chunks", ABI_INT},
                                                {"line", ABI_INT},
                                                {"trailer_bytes", ABI_INT},
                                                {"trailer_fields", ABI_INT}}}};
static const struct abi_case body_step[] = {
    {"Step", 3, {{"state", ABI_BODY_STATE}, {"consumed", ABI_INT}, {"payload", ABI_SPANS}}}};
static const struct abi_case body_result[] = {{"Ok", 1, {{NULL, ABI_BODY_STEP}}},
                                              {"Error", 1, {{NULL, ABI_INT}}}};
static const struct abi_case action[] = {
    {"Reply", 2, {{"status", ABI_INT}, {"body", ABI_TEXT}}},
    {"Call", 3, {{"request", ABI_RPC_REQUEST}, {"tool", ABI_INT}, {"arguments", ABI_JSON}}}};

static const struct {
    enum mo_type_kind kind;
    enum abi_type element, value;
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
    [ABI_INT_RESULT] = {.kind = MO_TYPE_CHOICE, .count = 2, .cases = int_result},
    [ABI_TEXT] = {.kind = MO_TYPE_TEXT},
    [ABI_INTS] = {.kind = MO_TYPE_LIST, .element = ABI_INT},
    [ABI_INT_OPTION] = {.kind = MO_TYPE_CHOICE, .count = 2, .cases = int_option},
    [ABI_MCP_KIND] = {.kind = MO_TYPE_CHOICE, .count = 19, .cases = mcp_kind},
    [ABI_MCP_KINDS] = {.kind = MO_TYPE_LIST, .element = ABI_MCP_KIND},
    [ABI_MCP_FIELD] = {.kind = MO_TYPE_CHOICE, .count = 1, .cases = mcp_field},
    [ABI_MCP_FIELDS] = {.kind = MO_TYPE_LIST, .element = ABI_MCP_FIELD},
    [ABI_MCP_CASE] = {.kind = MO_TYPE_CHOICE, .count = 1, .cases = mcp_case},
    [ABI_MCP_CASES] = {.kind = MO_TYPE_LIST, .element = ABI_MCP_CASE},
    [ABI_MCP_PAYLOAD] = {.kind = MO_TYPE_CHOICE, .count = 3, .cases = mcp_payload},
    [ABI_MCP_TOOL] = {.kind = MO_TYPE_CHOICE, .count = 1, .cases = mcp_tool},
    [ABI_MCP_TOOLS] = {.kind = MO_TYPE_LIST, .element = ABI_MCP_TOOL},
    [ABI_MCP_SERVICE] = {.kind = MO_TYPE_CHOICE, .count = 1, .cases = mcp_service},
    [ABI_JSON] = {.kind = MO_TYPE_CHOICE, .count = 6, .cases = json},
    [ABI_JSON_NUMBER] = {.kind = MO_TYPE_CHOICE, .count = 1, .cases = json_number},
    [ABI_JSON_ARRAY] = {.kind = MO_TYPE_LIST, .element = ABI_JSON},
    [ABI_JSON_OBJECT] = {.kind = MO_TYPE_MAP, .element = ABI_TEXT, .value = ABI_JSON},
    [ABI_FLOAT] = {.kind = MO_TYPE_FLOAT},
    [ABI_CHAR] = {.kind = MO_TYPE_CHAR},
    [ABI_TIME] = {.kind = MO_TYPE_TIME},
    [ABI_DECIMAL] = {.kind = MO_TYPE_DECIMAL},
    [ABI_BIGINT] = {.kind = MO_TYPE_BIGINT},
    [ABI_SCALAR] = {.kind = MO_TYPE_CHOICE, .count = 13, .cases = scalar},
    [ABI_SCALAR_OPTION] = {.kind = MO_TYPE_CHOICE, .count = 2, .cases = scalar_option},
    [ABI_VALUE] = {.kind = MO_TYPE_CHOICE, .count = 3, .cases = value},
    [ABI_VALUES] = {.kind = MO_TYPE_LIST, .element = ABI_VALUE},
    [ABI_VALUE_PROBLEM] = {.kind = MO_TYPE_CHOICE, .count = 2, .cases = value_problem},
    [ABI_VALUE_RESULT] = {.kind = MO_TYPE_CHOICE, .count = 2, .cases = value_result},
    [ABI_VALUES_RESULT] = {.kind = MO_TYPE_CHOICE, .count = 2, .cases = values_result},
    [ABI_JSON_RESULT] = {.kind = MO_TYPE_CHOICE, .count = 2, .cases = json_result},
    [ABI_TEXT_OPTION] = {.kind = MO_TYPE_CHOICE, .count = 2, .cases = text_option},
    [ABI_TEXTS] = {.kind = MO_TYPE_LIST, .element = ABI_TEXT},
    [ABI_JSON_OPTION] = {.kind = MO_TYPE_CHOICE, .count = 2, .cases = json_option},
    [ABI_RPC_REQUEST] = {.kind = MO_TYPE_CHOICE, .count = 1, .cases = rpc_request},
    [ABI_RPC_ENVELOPE] = {.kind = MO_TYPE_CHOICE, .count = 5, .cases = rpc_envelope},
    [ABI_RPC_METHOD] = {.kind = MO_TYPE_CHOICE, .count = 6, .cases = rpc_method},
    [ABI_CATALOG] = {.kind = MO_TYPE_CHOICE, .count = 1, .cases = catalog},
    [ABI_CATALOG_OPTION] = {.kind = MO_TYPE_CHOICE, .count = 2, .cases = catalog_option},
    [ABI_SPANS] = {.kind = MO_TYPE_LIST, .element = ABI_SPAN},
    [ABI_INSPECTION] = {.kind = MO_TYPE_CHOICE, .count = 2, .cases = inspection},
    [ABI_BODY_PLAN] = {.kind = MO_TYPE_CHOICE, .count = 3, .cases = body_plan},
    [ABI_BODY_PHASE] = {.kind = MO_TYPE_CHOICE, .count = 7, .cases = body_phase},
    [ABI_BODY_STATE] = {.kind = MO_TYPE_CHOICE, .count = 1, .cases = body_state},
    [ABI_BODY_BOUNDS] = {.kind = MO_TYPE_CHOICE, .count = 1, .cases = body_bounds},
    [ABI_BODY_STEP] = {.kind = MO_TYPE_CHOICE, .count = 1, .cases = body_step},
    [ABI_BODY_RESULT] = {.kind = MO_TYPE_CHOICE, .count = 2, .cases = body_result},
    [ABI_ACTION] = {.kind = MO_TYPE_CHOICE, .count = 2, .cases = action},
    [ABI_TEXT_PAIR] = {.kind = MO_TYPE_TUPLE, .element = ABI_TEXT, .value = ABI_TEXT},
};

static bool matches(const struct mo_types *types, uint32_t index, enum abi_type expected,
                    uint32_t *seen) {
    if (seen[expected] != MO_NO_INDEX)
        return seen[expected] == index;
    seen[expected] = index;
    const struct mo_type *type = &types->items[index];
    if (type->kind != abi[expected].kind)
        return false;
    if (type->kind == MO_TYPE_LIST)
        return type->parameter_count == 1 &&
               matches(types, type->parameters[0], abi[expected].element, seen);
    if (type->kind == MO_TYPE_MAP)
        return type->parameter_count == 2 &&
               matches(types, type->parameters[0], abi[expected].element, seen) &&
               matches(types, type->parameters[1], abi[expected].value, seen);
    if (type->kind == MO_TYPE_TUPLE)
        return type->parameter_count == 2 &&
               matches(types, type->parameters[0], abi[expected].element, seen) &&
               matches(types, type->parameters[1], abi[expected].value, seen);
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
                !matches(types, actual->fields[j].type, wanted->fields[j].type, seen))
                return false;
        }
    }
    return true;
}

static bool valid_abi(const struct mo_image *image) {
    static const struct {
        const char *name;
        uint32_t count;
        enum abi_type parameters[5], result;
    } functions[] = {
        {"Http.scan_headers", 3, {ABI_BYTES, ABI_SCAN, ABI_INT}, ABI_SCAN_RESULT},
        {"Http.parse_headers", 2, {ABI_BYTES, ABI_LIMITS}, ABI_HEAD_RESULT},
        {"Http.bearer_span", 2, {ABI_BYTES, ABI_HEAD}, ABI_SPAN_OPTION},
        {"Http.chunk_size", 3, {ABI_BYTES, ABI_INT, ABI_INT}, ABI_INT_RESULT},
        {"HttpAuthority.check", 1, {ABI_TEXTS}, ABI_BOOL},
        {"HttpPolicy.inspect", 3, {ABI_BYTES, ABI_HEAD, ABI_TEXTS}, ABI_INSPECTION},
        {"HttpPolicy.body_plan", 3, {ABI_BYTES, ABI_HEAD, ABI_INT}, ABI_BODY_PLAN},
        {"HttpBody.fixed", 1, {ABI_INT}, ABI_BODY_STATE},
        {"HttpBody.chunked", 0, {0}, ABI_BODY_STATE},
        {"HttpBody.feed", 3, {ABI_BODY_STATE, ABI_BYTES, ABI_BODY_BOUNDS}, ABI_BODY_RESULT},
        {"Mcp.catalog", 1, {ABI_MCP_SERVICE}, ABI_JSON},
        {"Mcp.prepare", 3, {ABI_MCP_SERVICE, ABI_TEXT, ABI_INT}, ABI_CATALOG_OPTION},
        {"Mcp.page", 3, {ABI_CATALOG, ABI_INT, ABI_JSON}, ABI_JSON_OPTION},
        {"Mcp.success", 1, {ABI_JSON}, ABI_JSON},
        {"Mcp.application_error", 1, {ABI_JSON}, ABI_JSON},
        {"McpTransport.receive",
         5,
         {ABI_TEXT, ABI_TEXT_OPTION, ABI_MCP_SERVICE, ABI_CATALOG, ABI_TEXT},
         ABI_ACTION},
        {"McpTransport.reply", 2, {ABI_JSON, ABI_JSON}, ABI_TEXT},
        {"McpTransport.fallback", 2, {ABI_TEXT, ABI_TEXT}, ABI_TEXT_PAIR},
        {"McpTransport.http", 2, {ABI_INT, ABI_TEXT}, ABI_TEXT},
        {"McpTransport.http_head", 2, {ABI_INT, ABI_INT}, ABI_TEXT},
        {"McpTransport.http100", 0, {0}, ABI_TEXT},
        {"McpRpc.parse", 2, {ABI_TEXT, ABI_TEXT_OPTION}, ABI_RPC_ENVELOPE},
        {"McpRpc.method", 3, {ABI_RPC_REQUEST, ABI_TEXTS, ABI_TEXT}, ABI_RPC_METHOD},
        {"McpRpc.result", 2, {ABI_JSON, ABI_JSON}, ABI_JSON},
        {"McpRpc.error", 3, {ABI_JSON_OPTION, ABI_INT, ABI_TEXT}, ABI_JSON},
        {"McpRpc.initialized", 1, {ABI_TEXT}, ABI_JSON},
        {"McpScalar.decode", 3, {ABI_MCP_KIND, ABI_JSON, ABI_INT}, ABI_SCALAR_OPTION},
        {"McpScalar.encode", 1, {ABI_SCALAR}, ABI_JSON},
        {"McpValue.decode",
         5,
         {ABI_MCP_KINDS, ABI_INT, ABI_JSON, ABI_INT, ABI_INT},
         ABI_VALUE_RESULT},
        {"McpValue.encode", 4, {ABI_MCP_KINDS, ABI_INT, ABI_VALUE, ABI_INT}, ABI_JSON_RESULT},
        {"McpValue.arguments",
         5,
         {ABI_MCP_KINDS, ABI_MCP_FIELDS, ABI_JSON, ABI_INT, ABI_INT},
         ABI_VALUES_RESULT},
    };
    const struct mo_bytecode *file = mo_image_bytecode(image);
    const struct mo_types *types = mo_image_types(image);
    uint32_t seen[ABI_COUNT];
    for (uint32_t i = 0; i < ABI_COUNT; i++)
        seen[i] = MO_NO_INDEX;
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
                !matches(types, function->result_type, functions[i].result, seen))
                return false;
            for (uint32_t k = 0; k < function->parameter_count; k++)
                if (!matches(types, function->parameters[k].type, functions[i].parameters[k], seen))
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
