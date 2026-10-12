/* Checks the tables of primitives and capability operations, base/prims.def and
 * base/hostops.def (docs/design/bytecode.md, sections 6 and 7): their ids and names, that
 * every signature is a type in Mo's syntax and every ownership string has a letter for each
 * of its parameters, the form of the bugs, and which operations only spec code may use.
 * ci/library_map.sh checks the tables against docs/design/library.md. */
#include "base/base.h"
#include "test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* A line of either table. A primitive is never for spec code alone. */
struct line {
    const char *table; /* "prims.def" or "hostops.def", for messages */
    const char *id;
    const char *module;
    const char *name;
    const char *signature;
    const char *params;
    const char *bugs;
    bool spec_only;
};

static const struct line prims[] = {
#define PRIM(id, module, name, signature, params, bugs)                                            \
    {"prims.def", #id, module, name, signature, params, bugs, false},
#include "base/prims.def"
};

static const struct line hostops[] = {
#define HOSTOP(id, module, name, signature, params, bugs, spec_only)                               \
    {"hostops.def", #id, module, name, signature, params, bugs, spec_only},
#include "base/hostops.def"
};

enum {
    PRIM_COUNT = sizeof prims / sizeof prims[0],
    HOSTOP_COUNT = sizeof hostops / sizeof hostops[0],
    LINE_COUNT = PRIM_COUNT + HOSTOP_COUNT,
};

/* Limits on what a signature may hold. A signature beyond one is refused, not misread. */
enum {
    MAX_PARAMS = 8,       /* parameters of one signature */
    MAX_TYPE_DEPTH = 8,   /* types nested in types */
    MAX_NAME_LENGTH = 32, /* a type's or an ability's name, with its terminating NUL */
};

/* The built-in types a signature may name (reference 3 and 11), with the number of type
 * arguments each takes. Immediate types are those whose values sit in a cell (bytecode.md
 * 2.1), for which ownership means nothing. */
struct type {
    const char *name;
    size_t arity;
    bool immediate;
    bool capability;
};

static const struct type types[] = {
    {"Unit", 0, true, false},      {"Bool", 0, true, false},       {"Int", 0, true, false},
    {"BigInt", 0, false, false},   {"Byte", 0, true, false},       {"Decimal", 0, true, false},
    {"Float", 0, true, false},     {"Text", 0, false, false},      {"Char", 0, true, false},
    {"Bytes", 0, false, false},    {"List", 1, false, false},      {"Map", 2, false, false},
    {"Set", 1, false, false},      {"Range", 0, true, false},      {"Time", 0, true, false},
    {"Duration", 0, true, false},  {"Option", 1, false, false},    {"Result", 2, false, false},
    {"Ordering", 0, true, false},  {"FileError", 0, false, false}, {"Args", 0, false, true},
    {"Env", 0, false, true},       {"Console", 0, false, true},    {"Files", 0, false, true},
    {"ReadFiles", 0, false, true}, {"Clock", 0, false, true},      {"Random", 0, false, true},
};

/* The five abilities (reference 9.2). */
static const char *const abilities[] = {"Equal", "Hash", "Order", "Text", "Number"};

static bool is_upper(char c) {
    return c >= 'A' && c <= 'Z';
}

static bool is_lower(char c) {
    return c >= 'a' && c <= 'z';
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static const struct type *find_type(const char *name) {
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++) {
        if (strcmp(types[i].name, name) == 0) {
            return &types[i];
        }
    }
    return NULL;
}

static bool is_ability(const char *name) {
    for (size_t i = 0; i < sizeof abilities / sizeof abilities[0]; i++) {
        if (strcmp(abilities[i], name) == 0) {
            return true;
        }
    }
    return false;
}

/* A type parameter, one capital letter (reference 2.3), as a bit of a set. */
static uint32_t type_parameter_bit(char letter) {
    MO_ASSERT(is_upper(letter));
    return (uint32_t)1 << (unsigned)(letter - 'A');
}

/* Text from start, length bytes long, inside a table's string. */
struct span {
    const char *start;
    size_t length;
};

static bool span_is(struct span span, const char *text) {
    return span.length == strlen(text) && strncmp(span.start, text, span.length) == 0;
}

/* What reading a type found in it. */
struct facts {
    uint32_t type_parameters; /* the type parameters it names */
    bool function;            /* whether it holds a function type */
    bool capability;          /* whether it holds a capability type */
};

/* A signature taken apart. A constant's signature is just its type: it has no parameters,
 * and its type is its result. */
struct signature {
    bool constant;
    size_t param_count;
    struct span params[MAX_PARAMS];
    struct span result; /* empty when the function returns Unit */
    struct facts param_facts;
    struct facts result_facts;
    uint32_t constrained; /* the type parameters its where clause names */
};

/* Moves *at past prefix if the text there starts with it. */
MO_MUST_USE static bool take(const char **at, const char *prefix) {
    size_t length = strlen(prefix);
    if (strncmp(*at, prefix, length) != 0) {
        return false;
    }
    *at += length;
    return true;
}

/* Reads a CapCase name, such as List or T, into name, and moves *at past it. */
MO_MUST_USE static bool read_cap_name(const char **at, char name[MAX_NAME_LENGTH]) {
    const char *end = *at;
    if (!is_upper(*end)) {
        return false;
    }
    while (is_upper(*end) || is_lower(*end) || is_digit(*end)) {
        end++;
    }
    size_t length = (size_t)(end - *at);
    if (length >= MAX_NAME_LENGTH) {
        return false;
    }
    memcpy(name, *at, length);
    name[length] = '\0';
    *at = end;
    return true;
}

/* Reads one type, in Mo's syntax as mo format writes it (reference appendix B), and moves *at
 * past it: a built-in type with its type arguments, a type parameter, a tuple, or a function
 * type. Records in facts what the type holds. */
MO_MUST_USE static bool read_type(const char **at, unsigned depth, struct facts *facts) {
    if (depth >= MAX_TYPE_DEPTH) {
        return false;
    }
    if (take(at, "fn(")) {
        facts->function = true;
        if (!take(at, ")")) {
            do {
                if (!read_type(at, depth + 1, facts)) {
                    return false;
                }
            } while (take(at, ", "));
            if (!take(at, ")")) {
                return false;
            }
        }
        return !take(at, " : ") || read_type(at, depth + 1, facts);
    }
    if (take(at, "(")) {
        size_t parts = 0;
        do {
            if (!read_type(at, depth + 1, facts)) {
                return false;
            }
            parts++;
        } while (take(at, ", "));
        return parts >= 2 && take(at, ")");
    }
    char name[MAX_NAME_LENGTH];
    if (!read_cap_name(at, name)) {
        return false;
    }
    if (name[1] == '\0') {
        facts->type_parameters |= type_parameter_bit(name[0]);
        return true;
    }
    const struct type *type = find_type(name);
    if (type == NULL) {
        return false;
    }
    facts->capability = facts->capability || type->capability;
    size_t arguments = 0;
    if (take(at, "(")) {
        do {
            if (!read_type(at, depth + 1, facts)) {
                return false;
            }
            arguments++;
        } while (take(at, ", "));
        if (!take(at, ")")) {
            return false;
        }
    }
    return arguments == type->arity;
}

/* Reads a where clause's abilities, such as "T: Equal, K: Hash" (reference 5.5). */
MO_MUST_USE static bool read_abilities(const char **at, uint32_t *constrained) {
    do {
        char parameter[MAX_NAME_LENGTH];
        char ability[MAX_NAME_LENGTH];
        if (!read_cap_name(at, parameter) || parameter[1] != '\0' || !take(at, ": ") ||
            !read_cap_name(at, ability) || !is_ability(ability)) {
            return false;
        }
        *constrained |= type_parameter_bit(parameter[0]);
    } while (take(at, ", "));
    return true;
}

/* Takes a signature apart: "fn(" parameters ")", then " : " and the result unless it is
 * Unit, then " where " and abilities if it needs some; or, for a constant, just its type. */
MO_MUST_USE static bool parse_signature(const char *text, struct signature *out) {
    memset(out, 0, sizeof *out);
    const char *at = text;
    if (!take(&at, "fn(")) {
        out->constant = true;
        if (!read_type(&at, 0, &out->result_facts)) {
            return false;
        }
        out->result = (struct span){text, (size_t)(at - text)};
        return *at == '\0';
    }
    if (!take(&at, ")")) {
        do {
            if (out->param_count == MAX_PARAMS) {
                return false;
            }
            const char *start = at;
            if (!read_type(&at, 0, &out->param_facts)) {
                return false;
            }
            out->params[out->param_count++] = (struct span){start, (size_t)(at - start)};
        } while (take(&at, ", "));
        if (!take(&at, ")")) {
            return false;
        }
    }
    if (take(&at, " : ")) {
        const char *start = at;
        if (!read_type(&at, 0, &out->result_facts)) {
            return false;
        }
        out->result = (struct span){start, (size_t)(at - start)};
    }
    if (take(&at, " where ") && !read_abilities(&at, &out->constrained)) {
        return false;
    }
    return *at == '\0';
}

/* The name a type starts with, such as List for List(T); "" for a tuple or a function type. */
static void type_head(struct span type, char head[MAX_NAME_LENGTH]) {
    const char *at = type.start;
    if (!read_cap_name(&at, head) || (size_t)(at - type.start) > type.length) {
        head[0] = '\0';
    }
}

/* Whether a span is a built-in immediate type, for which ownership means nothing. */
static bool is_immediate(struct span type) {
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++) {
        if (types[i].immediate && span_is(type, types[i].name)) {
            return true;
        }
    }
    return false;
}

/* A type that a module declares, for reference 5.1's rule. */
struct declaration {
    const char *module;
    const char *type;
};

static bool declares(const struct declaration *declarations, size_t count, const char *module,
                     const char *type) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(declarations[i].module, module) == 0 &&
            strcmp(declarations[i].type, type) == 0) {
            return true;
        }
    }
    return false;
}

/* Reference 5.1: two functions of a module share a name only when their first parameters are
 * different types declared in that module, as Order and OrderLine can both have a total. Each
 * type is given by its name, "" for a function without parameters. */
static bool may_share_name(const char *module, const char *first, const char *second,
                           const struct declaration *declarations, size_t declaration_count) {
    return first[0] != '\0' && second[0] != '\0' && strcmp(first, second) != 0 &&
           declares(declarations, declaration_count, module, first) &&
           declares(declarations, declaration_count, module, second);
}

/* The name of a line's first parameter's type, such as List for List(T); "" when it has no
 * parameters, or its signature can't be read, which check_line reports. */
static void first_parameter_type(const struct line *line, char type[MAX_NAME_LENGTH]) {
    struct signature signature;
    if (!parse_signature(line->signature, &signature) || signature.param_count == 0) {
        type[0] = '\0';
        return;
    }
    type_head(signature.params[0], type);
}

/* Counts the pairs of lines that share a module and a name without reference 5.1's leave,
 * and names each one when report is set. */
static size_t count_shared_names(const struct line *lines, size_t count,
                                 const struct declaration *declarations, size_t declaration_count,
                                 bool report) {
    size_t shared = 0;
    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            if (strcmp(lines[i].module, lines[j].module) != 0 ||
                strcmp(lines[i].name, lines[j].name) != 0) {
                continue;
            }
            char first[MAX_NAME_LENGTH];
            char second[MAX_NAME_LENGTH];
            first_parameter_type(&lines[i], first);
            first_parameter_type(&lines[j], second);
            if (may_share_name(lines[i].module, first, second, declarations, declaration_count)) {
                continue;
            }
            if (report) {
                (void)fprintf(stderr,
                              "%s and %s: both are %s.%s, and reference 5.1 doesn't allow it\n",
                              lines[i].id, lines[j].id, lines[i].module, lines[i].name);
            }
            shared++;
        }
    }
    return shared;
}

/* Records a failed expectation about a line, naming it. */
static void expect(bool holds, const struct line *line, const char *what) {
    if (!holds) {
        (void)fprintf(stderr, "%s: %s: %s\n", line->table, line->id, what);
        mo_test_failures++;
    }
}

/* A module's name is CapCase, such as ReadFiles or Unicode16 (reference 2.3). */
static bool is_module_name(const char *name) {
    const char *at = name;
    char ignored[MAX_NAME_LENGTH];
    return read_cap_name(&at, ignored) && *at == '\0';
}

/* A name is snake_case, maybe ending in ?, as Mo writes the names of values and functions
 * (reference 2.3); a primitive that only lowering emits starts its name with %, which no Mo
 * name can. */
static bool is_function_name(const char *name, bool lowering_may_name) {
    const char *at = name;
    if (lowering_may_name && *at == '%') {
        at++;
    }
    if (!is_lower(*at)) {
        return false;
    }
    while (is_lower(*at) || is_digit(*at) || *at == '_') {
        at++;
    }
    if (*at == '?') {
        at++;
    }
    return *at == '\0' && at[-1] != '_';
}

/* Ids are the module's name and the function's in capitals, as LIST_APPEND for List.append,
 * without a name's % or ?; a name ending in ? may start with IS_, as LIST_IS_EMPTY does. */
static bool follows_the_id_rule(const struct line *line) {
    char expected[2][64];
    size_t lengths[2] = {0, 0};
    for (size_t form = 0; form < 2; form++) {
        size_t length = 0;
        for (const char *c = line->module; *c != '\0'; c++) {
            MO_ASSERT(length + 1 < sizeof expected[form]);
            expected[form][length++] = is_lower(*c) ? (char)(*c - 'a' + 'A') : *c;
        }
        const char *prefix = form == 0 ? "_" : "_IS_";
        for (const char *c = prefix; *c != '\0'; c++) {
            MO_ASSERT(length + 1 < sizeof expected[form]);
            expected[form][length++] = *c;
        }
        for (const char *c = line->name; *c != '\0'; c++) {
            if (*c == '%' || *c == '?') {
                continue;
            }
            MO_ASSERT(length + 1 < sizeof expected[form]);
            expected[form][length++] = is_lower(*c) ? (char)(*c - 'a' + 'A') : *c;
        }
        expected[form][length] = '\0';
        lengths[form] = length;
    }
    MO_ASSERT(lengths[0] > 0 && lengths[1] > 0);
    size_t name_length = strlen(line->name);
    bool question = name_length > 0 && line->name[name_length - 1] == '?';
    return strcmp(line->id, expected[0]) == 0 || (question && strcmp(line->id, expected[1]) == 0);
}

/* Bugs are phrases in the reference's words, separated by "; ", or nothing: each starts with
 * a lowercase letter and holds lowercase letters, digits and single spaces. */
static bool bugs_are_well_formed(const char *bugs) {
    if (*bugs == '\0') {
        return true;
    }
    const char *at = bugs;
    for (;;) {
        if (!is_lower(*at)) {
            return false;
        }
        while (is_lower(*at) || is_digit(*at) ||
               (*at == ' ' && (is_lower(at[1]) || is_digit(at[1])))) {
            at++;
        }
        if (*at == '\0') {
            return true;
        }
        if (!take(&at, "; ")) {
            return false;
        }
    }
}

/* Checks what holds for every line of both tables. */
static void check_line(const struct line *line) {
    expect(is_module_name(line->module), line, "the module isn't a CapCase name");
    bool primitive = strcmp(line->table, "prims.def") == 0;
    expect(is_function_name(line->name, primitive), line,
           "the name isn't a Mo name, or % and one for a primitive only lowering emits");
    expect(follows_the_id_rule(line), line, "the id isn't MODULE_NAME, as LIST_APPEND");
    expect(bugs_are_well_formed(line->bugs), line, "the bugs aren't phrases separated by \"; \"");

    struct signature signature;
    if (!parse_signature(line->signature, &signature)) {
        expect(false, line, "the signature isn't a type in Mo's syntax");
        return;
    }
    expect(strlen(line->params) == signature.param_count, line,
           "the ownership string doesn't have one letter for each parameter");
    for (size_t i = 0; i < signature.param_count && line->params[i] != '\0'; i++) {
        char letter = line->params[i];
        expect(letter == 'o' || letter == 'b', line, "an ownership letter isn't o or b");
        expect(letter == 'b' || !is_immediate(signature.params[i]), line,
               "a parameter of an immediate type isn't b");
    }
    expect(!signature.param_facts.function && !signature.result_facts.function, line,
           "it takes or returns a function, which only Mo code in lib/ may (bytecode.md 6)");
    expect(signature.param_count == 0 || (signature.result_facts.type_parameters &
                                          ~signature.param_facts.type_parameters) == 0,
           line, "its result names a type parameter that no parameter does");
    expect((signature.constrained &
            ~(signature.param_facts.type_parameters | signature.result_facts.type_parameters)) == 0,
           line, "its where clause names a type parameter the signature doesn't");
}

/* The capability type a module is about, or NULL for a module that isn't a capability's. */
static const struct type *capability_of(const char *module) {
    const struct type *type = find_type(module);
    return type != NULL && type->capability ? type : NULL;
}

static void test_every_line_is_well_formed(void) {
    for (size_t i = 0; i < PRIM_COUNT; i++) {
        check_line(&prims[i]);
    }
    for (size_t i = 0; i < HOSTOP_COUNT; i++) {
        check_line(&hostops[i]);
    }
}

static void test_ids_are_unique(void) {
    const struct line *lines[LINE_COUNT];
    for (size_t i = 0; i < PRIM_COUNT; i++) {
        lines[i] = &prims[i];
    }
    for (size_t i = 0; i < HOSTOP_COUNT; i++) {
        lines[PRIM_COUNT + i] = &hostops[i];
    }
    for (size_t i = 0; i < LINE_COUNT; i++) {
        for (size_t j = i + 1; j < LINE_COUNT; j++) {
            expect(strcmp(lines[i]->id, lines[j]->id) != 0, lines[j], "another line has its id");
        }
    }
}

/* Each module of the library declares the type named like it (reference 10.5); Unicode16
 * declares none. */
static void test_names_are_unique_within_a_module(void) {
    struct declaration declarations[sizeof types / sizeof types[0]];
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++) {
        declarations[i] = (struct declaration){types[i].name, types[i].name};
    }
    struct line lines[LINE_COUNT];
    memcpy(lines, prims, sizeof prims);
    memcpy(lines + PRIM_COUNT, hostops, sizeof hostops);
    size_t shared = count_shared_names(lines, LINE_COUNT, declarations,
                                       sizeof declarations / sizeof declarations[0], true);
    CHECK(shared == 0);
}

/* Reference 5.1's rule, on the reference's own example, and on the tables: if module Files
 * declared ReadFiles as well as Files, each could have its own read_text. */
static void test_the_rule_for_shared_names(void) {
    static const struct declaration shop[] = {{"Shop", "Order"}, {"Shop", "OrderLine"}};
    CHECK(may_share_name("Shop", "Order", "OrderLine", shop, 2));
    CHECK(!may_share_name("Shop", "Order", "Order", shop, 2));
    CHECK(!may_share_name("Shop", "Order", "Text", shop, 2));
    CHECK(!may_share_name("Shop", "Order", "", shop, 2));
    CHECK(!may_share_name("Bill", "Order", "OrderLine", shop, 2));

    static const struct declaration files[] = {{"Files", "Files"}, {"Files", "ReadFiles"}};
    static const struct line read_text[] = {
        {"test", "FILES_READ_TEXT", "Files", "read_text",
         "fn(Files, Text) : Result(Text, FileError)", "bb", "", false},
        {"test", "FILES_READ_TEXT", "Files", "read_text",
         "fn(ReadFiles, Text) : Result(Text, FileError)", "bb", "", false},
        {"test", "FILES_READ_TEXT", "Files", "read_text", "fn(Files) : Text", "b", "", false},
    };
    CHECK(count_shared_names(read_text, 2, files, 2, false) == 0);
    CHECK(count_shared_names(read_text, 2, files, 1, false) == 1);
    CHECK(count_shared_names(read_text, 3, files, 2, false) == 1);
}

static void test_signatures_are_read_as_mo_writes_them(void) {
    struct signature signature;
    CHECK(parse_signature("fn(List(T), Int) : Option(T)", &signature));
    CHECK(signature.param_count == 2 && span_is(signature.params[1], "Int"));
    CHECK(span_is(signature.result, "Option(T)"));
    CHECK(parse_signature("fn(List((K, V))) : Map(K, V) where K: Hash", &signature));
    CHECK(signature.param_count == 1 && signature.constrained == type_parameter_bit('K'));
    CHECK(parse_signature("fn(Console, Text)", &signature));
    CHECK(signature.param_count == 2 && signature.result.length == 0);
    CHECK(parse_signature("fn() : Console", &signature) && signature.param_count == 0);
    CHECK(parse_signature("Set(T)", &signature) && signature.constant);
    CHECK(parse_signature("fn(fn(T) : U, List(T)) : List(U)", &signature));
    CHECK(signature.param_count == 2 && signature.param_facts.function);
    CHECK(!parse_signature("fn(List(T), Int) : Option(T", &signature));
    CHECK(!parse_signature("fn(List(T),Int) : Option(T)", &signature));
    CHECK(!parse_signature("fn(Map(K)) : Int", &signature));
    CHECK(!parse_signature("fn(Lists(T)) : Int", &signature));
    CHECK(!parse_signature("fn((Int)) : Int", &signature));
    CHECK(!parse_signature("fn(Int) : Int where T: Sortable", &signature));
    CHECK(!parse_signature("fn(Int, Int, Int, Int, Int, Int, Int, Int, Int)", &signature));
    CHECK(!parse_signature("fn(List(List(List(List(List(List(List(List(Int)))))))))", &signature));
}

/* Primitives compute; only host operations touch capabilities (bytecode.md 7). */
static void test_primitives_take_no_capabilities(void) {
    for (size_t i = 0; i < PRIM_COUNT; i++) {
        struct signature signature;
        if (!parse_signature(prims[i].signature, &signature)) {
            continue; /* check_line reports it */
        }
        expect(!signature.param_facts.capability && !signature.result_facts.capability, &prims[i],
               "a primitive names a capability type");
        expect(capability_of(prims[i].module) == NULL, &prims[i],
               "a primitive is in a capability's module");
    }
}

/* A host operation either takes its module's capability first, borrowed, or makes a fake of
 * it from no capability at all. Only spec code may use the fake constructors, printed and
 * printed_errors (reference 12.4). */
static void test_host_operations_take_their_capability(void) {
    for (size_t i = 0; i < HOSTOP_COUNT; i++) {
        const struct line *line = &hostops[i];
        struct signature signature;
        if (!parse_signature(line->signature, &signature)) {
            continue; /* check_line reports it */
        }
        expect(capability_of(line->module) != NULL, line, "its module isn't a capability's");
        bool operation = signature.param_count > 0 && span_is(signature.params[0], line->module);
        bool constructor =
            span_is(signature.result, line->module) && !signature.param_facts.capability;
        expect(operation != constructor, line,
               "it neither takes its module's capability first nor makes one from no capability");
        expect(!operation || line->params[0] == 'b', line,
               "the capability isn't borrowed, though host only reads it (bytecode.md 4.6)");
        bool printed =
            strcmp(line->module, "Console") == 0 &&
            (strcmp(line->name, "printed") == 0 || strcmp(line->name, "printed_errors") == 0);
        expect(line->spec_only == (constructor || printed), line,
               "spec code alone may use exactly the fake constructors, printed and printed_errors");
    }
}

/* Every operation of bytecode.md 7's list, and ReadFiles's of reference 14, has its line. */
static void test_every_capability_operation_is_there(void) {
    static const char *const operations[][2] = {
        {"Console", "print"},
        {"Console", "error"},
        {"Console", "read_line"},
        {"Console", "printed"},
        {"Console", "printed_errors"},
        {"Files", "read_text"},
        {"Files", "read_bytes"},
        {"Files", "write_text"},
        {"Files", "append_text"},
        {"Files", "remove"},
        {"Files", "list"},
        {"Files", "exists?"},
        {"Files", "within"},
        {"Files", "read_only"},
        {"ReadFiles", "read_text"},
        {"ReadFiles", "read_bytes"},
        {"ReadFiles", "list"},
        {"ReadFiles", "exists?"},
        {"ReadFiles", "within"},
        {"Clock", "now"},
        {"Random", "int"},
        {"Args", "list"},
        {"Env", "get"},
        {"Console", "fake"},
        {"Console", "fake_input"},
        {"Files", "in_memory"},
        {"Clock", "fixed"},
        {"Random", "seeded"},
        {"Args", "of"},
        {"Env", "of"},
    };
    size_t count = sizeof operations / sizeof operations[0];
    for (size_t i = 0; i < count; i++) {
        size_t found = 0;
        for (size_t j = 0; j < HOSTOP_COUNT; j++) {
            found += strcmp(hostops[j].module, operations[i][0]) == 0 &&
                     strcmp(hostops[j].name, operations[i][1]) == 0;
        }
        if (found != 1) {
            (void)fprintf(stderr, "hostops.def: %s.%s: %zu lines, not 1\n", operations[i][0],
                          operations[i][1], found);
            mo_test_failures++;
        }
    }
    CHECK(HOSTOP_COUNT == count);
}

int main(void) {
    test_signatures_are_read_as_mo_writes_them();
    test_the_rule_for_shared_names();
    test_every_line_is_well_formed();
    test_ids_are_unique();
    test_names_are_unique_within_a_module();
    test_primitives_take_no_capabilities();
    test_host_operations_take_their_capability();
    test_every_capability_operation_is_there();
    return mo_test_report("test_tables");
}
