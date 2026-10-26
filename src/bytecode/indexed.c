#include "bytecode/assemble.h"

static MO_MUST_USE bool reference(struct mo_text_reader *r, char prefix, bool optional) {
    uint32_t value, source = r->token.start;
    return mo_text_ref(r, prefix, optional, &value) && mo_text_word(r, value, source);
}

static MO_MUST_USE bool punctuation(struct mo_text_reader *r, const char *s) {
    static const char *const expected[] = {"expected [", "expected ]", "expected (", "expected )",
                                           "expected ,"};
    static const char *const items[] = {"[", "]", "(", ")", ","};
    for (unsigned i = 0; i < 5; i++)
        if (s[0] == items[i][0])
            return mo_text_take(r, s, expected[i]);
    MO_UNREACHABLE();
}

static MO_MUST_USE bool boolean(struct mo_text_reader *r) {
    uint32_t value, source = r->token.start;
    if (!mo_text_ref(r, 0, false, &value))
        return false;
    if (value > 1) {
        mo_text_refuse(r, source, "expected boolean 0 or 1");
        return false;
    }
    return mo_text_word(r, value, source);
}

typedef bool (*item_reader)(struct mo_text_reader *r);

static MO_MUST_USE bool list(struct mo_text_reader *r, uint32_t limit, item_reader item) {
    uint32_t source = r->token.start, offset = (uint32_t)r->raw.length, count = 0;
    if (!punctuation(r, "[") || !mo_text_word(r, 0, source))
        return false;
    if (!mo_text_is(r, "]")) {
        do {
            if (count == limit) {
                mo_text_refuse(r, r->token.start, "expected list within count limit");
                return false;
            }
            if (!item(r))
                return false;
            count++;
            if (mo_text_is(r, "]"))
                break;
        } while (punctuation(r, ","));
    }
    if (!punctuation(r, "]"))
        return false;
    mo_text_patch(r, offset, count);
    return true;
}

static MO_MUST_USE bool type_index(struct mo_text_reader *r) {
    return reference(r, 't', false);
}
static MO_MUST_USE bool function_index(struct mo_text_reader *r) {
    return reference(r, 'f', false);
}
static MO_MUST_USE bool global_index(struct mo_text_reader *r) {
    return reference(r, 'g', false);
}

static MO_MUST_USE bool field(struct mo_text_reader *r) {
    return punctuation(r, "(") && reference(r, 's', true) && punctuation(r, ",") && type_index(r) &&
           punctuation(r, ")");
}
static MO_MUST_USE bool choice_case(struct mo_text_reader *r) {
    return punctuation(r, "(") && reference(r, 's', false) && punctuation(r, ",") &&
           list(r, MO_LIMIT_FIELDS, field) && punctuation(r, ")");
}
static MO_MUST_USE bool parameter(struct mo_text_reader *r) {
    static const char *const ownership[] = {"owned", "borrowed"};
    uint32_t value, source;
    if (!punctuation(r, "(") || !type_index(r) || !punctuation(r, ","))
        return false;
    source = r->token.start;
    return mo_text_enum(r, ownership, 2, "expected owned or borrowed", &value) &&
           mo_text_word(r, value, source) && punctuation(r, ")");
}

static MO_MUST_USE bool type(struct mo_text_reader *r) {
    uint32_t kind, source = r->token.start;
    if (!mo_text_enum(r, mo_bytecode_type_names, MO_TYPE_CAPABILITY + 1, "expected type kind",
                      &kind) ||
        !mo_text_word(r, kind, source))
        return false;
    if (kind <= MO_TYPE_BIGINT)
        return true;
    if (kind == MO_TYPE_TUPLE || kind == MO_TYPE_FUNCTION) {
        return list(r, kind == MO_TYPE_FUNCTION ? MO_LIMIT_BYTECODE_PARAMETERS : MO_LIMIT_FIELDS,
                    type_index) &&
               (kind != MO_TYPE_FUNCTION || type_index(r));
    }
    if (kind == MO_TYPE_LIST || kind == MO_TYPE_SET || kind == MO_TYPE_MAP) {
        return mo_text_word(r, kind == MO_TYPE_MAP ? 2 : 1, source) && type_index(r) &&
               (kind != MO_TYPE_MAP || type_index(r));
    }
    if (kind == MO_TYPE_RECORD || kind == MO_TYPE_CHOICE) {
        return reference(r, 's', false) &&
               list(r, kind == MO_TYPE_RECORD ? MO_LIMIT_FIELDS : MO_LIMIT_CHOICE_CASES,
                    kind == MO_TYPE_RECORD ? field : choice_case);
    }
    if (kind == MO_TYPE_ENV)
        return function_index(r);
    uint32_t capability;
    source = r->token.start;
    return mo_text_enum(r, mo_bytecode_capability_names, MO_CAP_RANDOM + 1,
                        "expected capability name", &capability) &&
           mo_text_word(r, capability, source);
}

static MO_MUST_USE bool layout(struct mo_text_reader *r) {
    static const char *const names[] = {"record", "tuple", "case", "closure"};
    uint32_t kind, source = r->token.start;
    return mo_text_enum(r, names, 4, "expected layout kind", &kind) &&
           mo_text_word(r, kind, source) && type_index(r) &&
           (kind != MO_LAYOUT_CASE || reference(r, 0, false)) &&
           (kind != MO_LAYOUT_CLOSURE || function_index(r)) && list(r, MO_LIMIT_FIELDS, type_index);
}

static MO_MUST_USE bool constant(struct mo_text_reader *r) {
    uint32_t index, source = r->token.start;
    if (!mo_text_ref(r, 't', false, &index) || !mo_text_word(r, index, source))
        return false;
    if (!r->shorthand) {
        if (index >= r->tables[MO_SECTION_TYPES].count) {
            mo_text_refuse(r, source, "expected type index in final pool");
            return false;
        }
        return mo_text_payload(r, index, false);
    }
    /* Seed types may themselves be supplied by a later shorthand header. */
    source = r->token.start;
    if (!mo_text_word(r, source, source))
        return false;
    while (r->token.kind != 'n' && r->token.kind != 0 && !r->failed)
        mo_text_next(r);
    return !r->failed;
}

bool mo_text_instruction(struct mo_text_reader *r) {
    const struct mo_opcode_info *op = NULL;
    uint32_t source = r->token.start;
    for (unsigned i = 0; i < MO_OPCODE_COUNT; i++) {
        if (mo_text_is(r, mo_opcodes[i].name)) {
            op = &mo_opcodes[i];
            break;
        }
    }
    if (op == NULL) {
        mo_text_refuse(r, source, "expected opcode");
        return false;
    }
    mo_text_next(r);
    uint32_t offset = (uint32_t)r->raw.length, count = 0;
    if (!mo_text_word(r, op->number, source))
        return false;
    while (r->token.kind != 'n' && r->token.kind != 0 && !r->failed) {
        if (count >= (uint32_t)op->fixed_count + op->variable_limit) {
            mo_text_refuse(r, r->token.start, "expected end of instruction within operand limit");
            return false;
        }
        enum mo_operand_kind kind =
            count < op->fixed_count ? op->fixed[count].kind : op->variable.kind;
        if ((count != 0 && !punctuation(r, ",")) ||
            !reference(r, mo_bytecode_operand_prefixes[kind][0], false))
            return false;
        count++;
    }
    if (count < op->fixed_count) {
        mo_text_refuse(r, r->token.start, "expected instruction operand");
        return false;
    }
    mo_text_patch(r, offset, op->number | ((count - op->fixed_count) << MO_OPCODE_VARIABLE_SHIFT));
    return mo_text_end_line(r);
}

static MO_MUST_USE bool function(struct mo_text_reader *r) {
    uint32_t value, source;
    if (!reference(r, 's', false) || !reference(r, 'm', false))
        return false;
    source = r->token.start;
    if (!mo_text_enum(r, mo_bytecode_function_names, MO_FUNCTION_ANCHOR + 1,
                      "expected function kind", &value) ||
        !mo_text_word(r, value, source) || !mo_text_take(r, "spec", "expected spec") ||
        !boolean(r) || !mo_text_end_line(r) || !mo_text_take(r, "params", "expected params") ||
        !list(r, MO_LIMIT_BYTECODE_PARAMETERS, parameter) || !mo_text_end_line(r) ||
        !mo_text_take(r, "result", "expected result") || !type_index(r) || !mo_text_end_line(r) ||
        !mo_text_take(r, "captures", "expected captures") ||
        !list(r, MO_LIMIT_FIELDS, type_index) || !mo_text_end_line(r) ||
        !mo_text_take(r, "registers", "expected registers") ||
        !list(r, MO_LIMIT_FUNCTION_REGISTERS, type_index) || !mo_text_end_line(r) ||
        !mo_text_take(r, "code", "expected code") || !mo_text_end_line(r))
        return false;
    uint32_t offset = (uint32_t)r->raw.length, instructions = 0;
    if (!mo_text_word(r, 0, r->token.start))
        return false;
    while (!r->failed && !mo_text_is(r, "end")) {
        if (++instructions > MO_LIMIT_FUNCTION_INSTRUCTIONS) {
            mo_text_refuse(r, r->token.start, "expected code within instruction limit");
            return false;
        }
        if (!mo_text_instruction(r))
            return false;
        if ((r->raw.length - offset - 4) / 4 > MO_LIMIT_FUNCTION_CODE_WORDS) {
            mo_text_refuse(r, r->token.start, "expected code within word limit");
            return false;
        }
    }
    mo_text_patch(r, offset, (uint32_t)((r->raw.length - offset - 4) / 4));
    return mo_text_take(r, "end", "expected code end") && mo_text_end_line(r) &&
           mo_text_take(r, "end", "expected function end");
}

static MO_MUST_USE bool exposure(struct mo_text_reader *r) {
    return punctuation(r, "(") && type_index(r) && punctuation(r, ",") && boolean(r) &&
           punctuation(r, ")");
}
static MO_MUST_USE bool module(struct mo_text_reader *r) {
    return reference(r, 's', false) && mo_text_take(r, "types", "expected types") &&
           list(r, MO_LIMIT_MODULE_NAMES, exposure) &&
           mo_text_take(r, "functions", "expected functions") &&
           list(r, MO_LIMIT_MODULE_NAMES, function_index) &&
           mo_text_take(r, "globals", "expected globals") &&
           list(r, MO_LIMIT_MODULE_NAMES, global_index);
}

static MO_MUST_USE bool location(struct mo_text_reader *r) {
    return reference(r, 'P', true) && punctuation(r, ",") && reference(r, 0, false) &&
           punctuation(r, ",") && reference(r, 0, false);
}
static MO_MUST_USE bool span(struct mo_text_reader *r) {
    return punctuation(r, "(") && reference(r, '@', false) && punctuation(r, ",") && location(r) &&
           punctuation(r, ")");
}
static MO_MUST_USE bool variable(struct mo_text_reader *r) {
    return punctuation(r, "(") && reference(r, 'r', false) && punctuation(r, ",") &&
           reference(r, 's', false) && punctuation(r, ",") && reference(r, '@', false) &&
           punctuation(r, ",") && reference(r, '@', false) && punctuation(r, ")");
}
static MO_MUST_USE bool condition(struct mo_text_reader *r) {
    return punctuation(r, "(") && location(r) && punctuation(r, ",") && reference(r, 's', false) &&
           punctuation(r, ")");
}
static MO_MUST_USE bool debug(struct mo_text_reader *r) {
    uint32_t value, source;
    if (!mo_text_take(r, "spans", "expected spans") || !list(r, MO_LIMIT_DEBUG_ENTRIES, span) ||
        !mo_text_take(r, "variables", "expected variables") ||
        !list(r, MO_LIMIT_DEBUG_ENTRIES, variable) || !mo_text_take(r, "text", "expected text") ||
        !reference(r, 's', true) || !mo_text_take(r, "declaration", "expected declaration") ||
        !punctuation(r, "(") || !location(r) || !punctuation(r, ")") ||
        !mo_text_take(r, "subject", "expected subject") || !reference(r, 'f', true) ||
        !mo_text_take(r, "expectation", "expected expectation"))
        return false;
    source = r->token.start;
    return mo_text_enum(r, mo_bytecode_expectation_names, MO_BYTECODE_FORBIDS + 1,
                        "expected none, allows or forbids", &value) &&
           mo_text_word(r, value, source) && mo_text_take(r, "lines", "expected lines") &&
           list(r, MO_LIMIT_DEBUG_ENTRIES, condition);
}

static MO_MUST_USE bool string_index(struct mo_text_reader *r) {
    return reference(r, 's', false);
}

static MO_MUST_USE bool service_tool(struct mo_text_reader *r) {
    return punctuation(r, "(") && reference(r, 's', false) && punctuation(r, ",") &&
           function_index(r) && punctuation(r, ",") && boolean(r) && punctuation(r, ",") &&
           list(r, MO_LIMIT_BYTECODE_PARAMETERS - 2, type_index) && punctuation(r, ",") &&
           type_index(r) && punctuation(r, ",") && string_index(r) && punctuation(r, ",") &&
           list(r, MO_LIMIT_BYTECODE_PARAMETERS - 2, string_index) && punctuation(r, ")");
}

static MO_MUST_USE bool service_timer(struct mo_text_reader *r) {
    return punctuation(r, "(") && function_index(r) && punctuation(r, ",") && function_index(r) &&
           punctuation(r, ")");
}

static MO_MUST_USE bool service(struct mo_text_reader *r) {
    return reference(r, 'm', false) && type_index(r) && function_index(r) &&
           reference(r, 's', false) && mo_text_take(r, "tools", "expected tools") &&
           list(r, MO_LIMIT_SERVICE_HANDLERS, service_tool) &&
           mo_text_take(r, "timers", "expected timers") &&
           list(r, MO_LIMIT_SERVICE_HANDLERS, service_timer);
}

bool mo_text_indexed_section(struct mo_text_reader *r, enum mo_bytecode_section section) {
    static const char *const names[] = {"strings", "types", "layouts",   "constants",
                                        "globals", "init",  "functions", "modules",
                                        "files",   "debug", "services"};
    static const char prefixes[] = "stLkgifmPdS";
    static const char *const expected[] = {
        "expected strings section",   "expected types section",   "expected layouts section",
        "expected constants section", "expected globals section", "expected init section",
        "expected functions section", "expected modules section", "expected files section",
        "expected debug section",     "expected services section"};
    r->tables[section].source = r->token.start;
    if (!mo_text_take(r, names[section], expected[section]) || !mo_text_end_line(r))
        return false;
    while (!r->failed && !mo_text_is(r, "end")) {
        uint32_t index, source = r->token.start;
        if (!mo_text_ref(r, prefixes[section], false, &index))
            return false;
        if (index != r->tables[section].count) {
            mo_text_refuse(r, source, "expected consecutive entry index within section limit");
            return false;
        }
        bool ok = false;
        switch (section) {
        case MO_SECTION_STRINGS: {
            struct mo_bytecode_string s;
            if (r->token.kind != 'q') {
                mo_text_refuse(r, r->token.start, "expected quoted UTF-8 string");
                return false;
            }
            ok = mo_text_quote(r, &s) && mo_text_word(r, s.length, source) &&
                 mo_text_bytes(r, s.bytes, s.length, source);
            break;
        }
        case MO_SECTION_TYPES:
            ok = type(r);
            break;
        case MO_SECTION_LAYOUTS:
            ok = layout(r);
            break;
        case MO_SECTION_CONSTANTS:
            ok = constant(r);
            break;
        case MO_SECTION_GLOBALS:
            ok = type_index(r) && reference(r, 's', false) && reference(r, 'm', false);
            break;
        case MO_SECTION_INIT:
            ok = function_index(r);
            break;
        case MO_SECTION_FUNCTIONS:
            ok = function(r);
            break;
        case MO_SECTION_MODULES:
            ok = module(r);
            break;
        case MO_SECTION_FILES: {
            struct mo_bytecode_string path;
            ok = mo_text_quote(r, &path) && mo_text_word(r, path.length, source) &&
                 mo_text_bytes(r, path.bytes, path.length, source);
            break;
        }
        case MO_SECTION_DEBUG:
            ok = debug(r);
            break;
        case MO_SECTION_SERVICES:
            ok = service(r);
            break;
        case MO_SECTION_COUNT:
            MO_UNREACHABLE();
        }
        if (!ok || !mo_text_end_line(r) || !mo_text_save(r, section, index, source))
            return false;
    }
    return mo_text_take(r, "end", "expected section end") && mo_text_end_line(r);
}
