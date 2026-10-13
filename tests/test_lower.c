#include "base/codes.h"
#include "base/limits.h"
#include "base/stack.h"
#include "codegen/codegen.h"
#include "lower/lower.h"
#include "lower_fixture.h"
#include "test.h"
#include "vm/vm.h"

#include <string.h>

static const char *corpus;
static void save(struct mo_core *core, const char *name, const char *expected) {
    if (corpus == NULL)
        return;
    struct mo_buffer text;
    mo_buffer_init(&text, MO_LIMIT_CORE_TEXT_SIZE);
    MO_ASSERT(mo_core_print(core, &text));
    char path[MO_LIMIT_PATH_SIZE];
    int n = snprintf(path, sizeof path, "%s/%s.core", corpus, name);
    MO_ASSERT(n > 0 && (size_t)n < sizeof path);
    FILE *file = fopen(path, "wb");
    MO_ASSERT(file != NULL && fwrite(text.bytes, 1, text.length, file) == text.length);
    MO_ASSERT(fclose(file) == 0);
    n = snprintf(path, sizeof path, "%s/%s.want", corpus, name);
    MO_ASSERT(n > 0 && (size_t)n < sizeof path);
    file = fopen(path, "wb");
    MO_ASSERT(file != NULL && fputs(expected, file) >= 0 && fclose(file) == 0);
    mo_buffer_free(&text);
}

static struct mo_core *checked(struct lower_fixture *f, const char *name, const char *expected) {
    struct mo_core *core = mo_lower(&f->arena, &f->program, &f->sources, &f->diags);
    CHECK(core != NULL && !mo_diags_any(&f->diags));
    if (core != NULL) {
        MO_ASSERT(mo_core_check(core, &f->diags));
        save(core, name, expected);
    }
    return core;
}

static void run(struct mo_core *core, int64_t expected) {
    struct mo_diags diags;
    struct mo_buffer bytes;
    CHECK(mo_diags_init(&diags));
    MO_ASSERT(mo_core_check(core, &diags)); /* A refusal is a lowering bug. */
    mo_buffer_init(&bytes, MO_LIMIT_BYTECODE_SIZE);
    CHECK(mo_compile(core, &bytes, &diags));
    if (mo_diags_any(&diags))
        (void)fprintf(stderr, "E%04u: %s (%s)\n", diags.items[0].code, diags.items[0].message,
                      diags.items[0].primary.text == NULL ? "" : diags.items[0].primary.text);
    struct mo_load_error error;
    struct mo_image *image = mo_load(bytes.bytes, bytes.length, &error);
    CHECK(image != NULL);
    if (image != NULL) {
        struct mo_limits limits = mo_limits_default();
        struct mo_defect defect;
        CHECK(mo_image_init(image, &limits, &defect) == MO_RETURNED);
        struct mo_process *process = mo_process_new(image, &limits);
        CHECK(process != NULL);
        if (process != NULL) {
            struct mo_cell result;
            const struct mo_bytecode *file = mo_image_bytecode(image);
            uint32_t entry = UINT32_MAX;
            for (size_t i = 0; i < core->funs.count; i++)
                if (core->entry.length == core->funs.items[i].name.length &&
                    memcmp(core->entry.bytes, core->funs.items[i].name.bytes, core->entry.length) ==
                        0) {
                    mo_core_string name = core->metadata.funs.items[i].name;
                    for (uint32_t j = 0; j < file->function_count; j++) {
                        const struct mo_bytecode_string *s =
                            &file->strings[file->functions[j].name];
                        if (s->length == name.length &&
                            memcmp(s->bytes, name.bytes, name.length) == 0)
                            entry = j;
                    }
                }
            CHECK(entry != UINT32_MAX);
            CHECK(mo_call(process, entry, NULL, 0, &result, &defect) == MO_RETURNED);
            CHECK(mo_cell_get_int(result) == expected);
            mo_process_free(process);
        }
        mo_image_free(image);
    }
    mo_buffer_free(&bytes);
    mo_diags_free(&diags);
}

static void test_literal(void) {
    const char *text = "module Main\nfn main() : Int\n17\nend\n";
    struct mo_arena syntax, arena;
    struct mo_sources sources;
    struct mo_diags diags;
    mo_arena_init(&syntax, 1 << 20);
    mo_arena_init(&arena, 1 << 20);
    CHECK(mo_sources_init(&sources));
    CHECK(mo_diags_init(&diags));
    uint32_t file;
    struct mo_source_error error;
    CHECK(mo_sources_add_bytes(&sources, "main.mo", (const uint8_t *)text, strlen(text), &file,
                               &error));
    struct mo_module_syntax *module = mo_parse(&syntax, &sources, file, &diags);
    CHECK(module != NULL && !mo_diags_any(&diags));
    const char *names[] = {"Main"};
    struct mo_module_set set = {.count = 1, .modules = &module, .names = names};
    struct mo_syntax *fn = module->tree->children[1]->children[0];
    struct mo_symbol symbol = {
        .name = "main", .declaration = fn, .owner = UINT32_MAX, .receiver = UINT32_MAX};
    struct mo_source_type int_type = {.kind = MO_SOURCE_NAMED, .name = "Int", .module = UINT32_MAX};
    const struct mo_source_type *parts[] = {&int_type};
    struct mo_source_type function_type = {
        .kind = MO_SOURCE_FUNCTION, .count = 1, .arguments = parts};
    struct mo_function_signature signature = {&symbol, &function_type};
    struct mo_syntax literal = {
        .kind = MO_S_INT, .span = {0, 28, 30}, .text = (const uint8_t *)"17"};
    struct mo_checked_expression expression = {
        .syntax = &literal, .type = &int_type, .field = UINT32_MAX, .receiver = UINT32_MAX};
    struct mo_checked_function function = {.signature = &signature,
                                           .body = {.count = 1, .items = &expression}};
    struct mo_program program = {
        .set = &set, .function_count = 1, .functions = &function, .entry.result = MO_ENTRY_STATUS};
    struct mo_core *core = mo_lower(&arena, &program, &sources, &diags);
    CHECK(core != NULL && !mo_diags_any(&diags));
    if (core != NULL) {
        save(core, "literal", "returned\n17\n");
        struct mo_buffer output;
        mo_buffer_init(&output, MO_LIMIT_CORE_TEXT_SIZE);
        CHECK(mo_core_print(core, &output));
        CHECK(core->metadata.source && core->metadata.files.count == 1);
        CHECK(core->metadata.funs.items[0].span.some &&
              core->metadata.funs.items[0].span.value.file == 0);
        mo_buffer_free(&output);
        mo_arena_free(&syntax);
        mo_sources_free(&sources); /* The lower result must own its provenance. */
        run(core, 17);
    } else {
        mo_arena_free(&syntax);
        mo_sources_free(&sources);
    }
    mo_arena_free(&arena);
    mo_diags_free(&diags);
}

static void test_branch_variables(void) {
    for (unsigned branch = 0; branch < 2; branch++) {
        struct lower_fixture f;
        lower_fixture_init(&f, "module Main\nfn main() : Int\nvar n = 7\n"
                               "if true\nn = 41\nelse\nn = 9\nend\nn\nend\n");
        f.bindings[0] = (struct mo_checked_binding){.name = "n",
                                                    .syntax = &f.syntax[0],
                                                    .span = {0, 36, 37},
                                                    .type = &f.int_,
                                                    .variable = true};
        f.functions[0].body.binding_count = 1;
        uint32_t seven = lower_fixture_row(&f, MO_S_INT, &f.int_, "7", 0, NULL);
        uint32_t start = lower_fixture_bind(&f, 0, seven, false);
        uint32_t condition =
            lower_fixture_row(&f, branch ? MO_S_TRUE : MO_S_FALSE, &f.bool_, "true", 0, NULL);
        uint32_t forty_one = lower_fixture_row(&f, MO_S_INT, &f.int_, "41", 0, NULL);
        uint32_t yes = lower_fixture_bind(&f, 0, forty_one, true);
        uint32_t nine = lower_fixture_row(&f, MO_S_INT, &f.int_, "9", 0, NULL);
        uint32_t no = lower_fixture_bind(&f, 0, nine, true);
        uint32_t arms[] = {condition, yes, no};
        uint32_t choice = lower_fixture_row(&f, MO_S_IF, &f.unit_, NULL, 3, arms);
        uint32_t value = lower_fixture_read(&f, 0);
        uint32_t statements[] = {start, choice, value};
        (void)lower_fixture_row(&f, MO_S_SEQUENCE, &f.int_, NULL, 3, statements);
        (void)checked(&f, branch ? "branch-true" : "branch-false",
                      branch ? "returned\n41\n" : "returned\n9\n");
        lower_fixture_free(&f);
    }
}

static void test_refusal(void) {
    struct mo_arena arena;
    struct mo_sources sources;
    struct mo_diags diags;
    mo_arena_init(&arena, 1 << 20);
    CHECK(mo_sources_init(&sources));
    CHECK(mo_diags_init(&diags));
    struct mo_program program = {0};
    CHECK(mo_lower(&arena, &program, &sources, &diags) == NULL);
    CHECK(diags.count == 1 && diags.items[0].code == MO_CODE_LOWER_UNFINISHED);
    mo_arena_free(&arena);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
}

static void test_instances(void) {
    struct lower_fixture f;
    lower_fixture_init(
        &f, "module Main\nstruct Box(T)\nvalue: T\nend\nfn identity(value: T) : T\nvalue\nend\n"
            "fn main() : Int\nif identity(true): identity(13) + identity(7) else: 0\nend\n");
    f.program.function_count = 3;
    struct mo_syntax declaration = {.kind = MO_S_STRUCT, .span = f.function_syntax[0].span};
    struct mo_syntax *items[] = {f.items[0], f.items[1], f.items[2], &declaration};
    f.declarations.count = 4;
    f.declarations.children = items;
    struct mo_symbol box = {.name = "Box",
                            .declaration = &declaration,
                            .owner = UINT32_MAX,
                            .receiver = UINT32_MAX,
                            .exported = true};
    struct mo_source_type box_parameter = {
        .kind = MO_SOURCE_PARAMETER, .name = "T", .declaration = &box, .parameters = 1};
    const struct mo_source_type *abstract_parts[] = {&box_parameter}, *concrete_parts[] = {&f.int_};
    struct mo_source_type abstract_box = {.kind = MO_SOURCE_NAMED,
                                          .name = "Box",
                                          .declaration = &box,
                                          .count = 1,
                                          .arguments = abstract_parts,
                                          .parameters = 1};
    struct mo_source_type concrete_box = abstract_box;
    concrete_box.arguments = concrete_parts;
    concrete_box.parameters = 0;
    struct mo_source_type other_box = concrete_box; /* distinct pointer, same nominal type */
    struct mo_syntax field_name = {.text = (const uint8_t *)"value", .span = {0, 26, 31}};
    struct mo_field_signature field = {.name = &field_name, .type = &box_parameter};
    const struct mo_field_signature *fields[] = {&field};
    struct mo_data_signature data = {
        .symbol = &box, .type = &abstract_box, .count = 1, .fields = &field, .by_index = fields};
    struct mo_signatures signatures = {.data_count = 1, .data = &data};
    f.program.signatures = &signatures;
    struct mo_module_syntax *modules[] = {&f.module, &f.module};
    const char *names[] = {"Main", "Other"};
    f.set.count = 2;
    f.set.modules = modules;
    f.set.names = names;
    f.symbols[2].name = "helper";
    f.symbols[2].module = 1;
    f.bindings[1] = (struct mo_checked_binding){.name = "_box", .type = &other_box};
    const struct mo_checked_binding *helper_parameters[] = {&f.bindings[1]};
    f.functions[2].parameter_count = 1;
    f.functions[2].parameters = helper_parameters;
    f.functions[2].body.binding_count = 2;
    f.function_types[2].count = 2;
    f.signature_parts[2][0] = &other_box;
    f.signature_parts[2][1] = &f.int_;
    struct mo_source_type parameter = {
        .kind = MO_SOURCE_PARAMETER, .name = "T", .declaration = &f.symbols[1], .parameters = 1};
    f.function_types[1].count = 2;
    f.function_types[1].parameters = 1;
    f.signature_parts[1][0] = f.signature_parts[1][1] = &parameter;
    f.bindings[0] = (struct mo_checked_binding){.name = "value", .type = &parameter};
    f.functions[0].body.binding_count = f.functions[1].body.binding_count = 1;
    const struct mo_checked_binding *parameters[] = {&f.bindings[0]};
    f.functions[1].parameter_count = 1;
    f.functions[1].parameters = parameters;
    uint32_t read = lower_fixture_read(&f, 0);
    f.functions[1].body.root = read;
    f.functions[1].body.count = 1;
    struct mo_type_argument int_argument = {&parameter, &f.int_},
                            bool_argument = {&parameter, &f.bool_};
    uint32_t calls[3];
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t callee = lower_fixture_row(&f, MO_S_NAME, &f.function_types[1], NULL, 0, NULL);
        f.rows[callee].symbol = &f.symbols[1];
        uint32_t value =
            lower_fixture_row(&f, i == 0 ? MO_S_TRUE : MO_S_INT, i == 0 ? &f.bool_ : &f.int_,
                              i == 0   ? "true"
                              : i == 1 ? "13"
                                       : "7",
                              0, NULL);
        uint32_t args[] = {callee, value};
        calls[i] = lower_fixture_row(&f, MO_S_CALL, i == 0 ? &f.bool_ : &f.int_, NULL, 2, args);
        f.rows[calls[i]].symbol = &f.symbols[1];
        f.rows[calls[i]].arguments =
            (struct mo_type_arguments){1, i == 0 ? &bool_argument : &int_argument};
    }
    f.functions[2].body.root = calls[1];
    f.functions[2].body.count = f.count;
    uint32_t sum = lower_fixture_op(&f, MO_T_PLUS, &f.int_, calls[1], calls[2]);
    uint32_t zero = lower_fixture_row(&f, MO_S_INT, &f.int_, "0", 0, NULL);
    uint32_t arms[] = {calls[0], sum, zero};
    uint32_t conditional = lower_fixture_row(&f, MO_S_IF, &f.int_, NULL, 3, arms);
    uint32_t payload = lower_fixture_row(&f, MO_S_INT, &f.int_, "13", 0, NULL);
    uint32_t record = lower_fixture_row(&f, MO_S_CALL, &concrete_box, NULL, 1, &payload);
    f.rows[record].symbol = &box;
    uint32_t sequence[] = {record, conditional};
    (void)lower_fixture_row(&f, MO_S_SEQUENCE, &f.int_, NULL, 2, sequence);
    struct mo_core *core = checked(&f, "instances", "returned\n20\n");
    if (core != NULL) {
        CHECK(core->funs.count == 6); /* main/helper, three requester-owned instances, wrapper */
        bool int_seen = false, bool_seen = false, other_seen = false;
        for (size_t i = 0; i < core->funs.count; i++) {
            mo_core_string name = core->metadata.funs.items[i].name;
            int_seen |= name.length == strlen("Main::Main.identity(Int)") &&
                        memcmp(name.bytes, "Main::Main.identity(Int)", name.length) == 0;
            bool_seen |= name.length == strlen("Main::Main.identity(Bool)") &&
                         memcmp(name.bytes, "Main::Main.identity(Bool)", name.length) == 0;
            other_seen |= name.length == strlen("Other::Main.identity(Int)") &&
                          memcmp(name.bytes, "Other::Main.identity(Int)", name.length) == 0;
        }
        CHECK(int_seen && bool_seen && other_seen);
        CHECK(core->data.count == 1 && core->metadata.modules.items[0].data.count == 1 &&
              core->metadata.modules.items[1].data.count == 0);
        CHECK(core->data.items[0].name.length == strlen("Main.Box(Int)") &&
              memcmp(core->data.items[0].name.bytes, "Main.Box(Int)", strlen("Main.Box(Int)")) ==
                  0);
        run(core, 20);
    }
    lower_fixture_free(&f);
}

static void test_pattern_scope(void) {
    for (unsigned present = 0; present < 2; present++) {
        struct lower_fixture f;
        lower_fixture_init(&f, "module Main\nfn main() : Int\n"
                               "if Some(7) is Some(n) and n > 3: n + 13 else: 0\nend\n");
        const struct mo_source_type *args[] = {&f.int_};
        struct mo_source_type option = {.kind = MO_SOURCE_NAMED,
                                        .module = UINT32_MAX,
                                        .name = "Option",
                                        .count = 1,
                                        .arguments = args};
        f.bindings[0] = (struct mo_checked_binding){.name = "n", .type = &f.int_};
        f.functions[0].body.binding_count = 1;
        uint32_t seven = lower_fixture_row(&f, MO_S_INT, &f.int_, "7", 0, NULL);
        uint32_t value = lower_fixture_row(&f, MO_S_CALL, &option, NULL, present ? 1 : 0, &seven);
        f.rows[value].native = present ? "Some" : "None";
        struct mo_checked_pattern bind = {.kind = MO_PATTERN_BIND,
                                          .syntax = &f.syntax[seven],
                                          .type = &f.int_,
                                          .binding = &f.bindings[0]};
        struct mo_checked_pattern_part part = {0, &bind};
        struct mo_checked_pattern pattern = {.kind = MO_PATTERN_CONSTRUCTOR,
                                             .syntax = &f.syntax[value],
                                             .type = &option,
                                             .constructor.builtin = "Some",
                                             .part_count = 1,
                                             .parts = &part};
        uint32_t is = lower_fixture_row(&f, MO_S_IS, &f.bool_, NULL, 1, &value);
        f.rows[is].pattern = &pattern;
        uint32_t n = lower_fixture_read(&f, 0);
        uint32_t three = lower_fixture_row(&f, MO_S_INT, &f.int_, "3", 0, NULL);
        uint32_t gt = lower_fixture_op(&f, MO_T_GT, &f.bool_, n, three);
        uint32_t condition = lower_fixture_op(&f, MO_T_AND, &f.bool_, is, gt);
        uint32_t thirteen = lower_fixture_row(&f, MO_S_INT, &f.int_, "13", 0, NULL);
        uint32_t sum = lower_fixture_op(&f, MO_T_PLUS, &f.int_, n, thirteen);
        uint32_t zero = lower_fixture_row(&f, MO_S_INT, &f.int_, "0", 0, NULL);
        uint32_t branches[] = {condition, sum, zero};
        (void)lower_fixture_row(&f, MO_S_IF, &f.int_, NULL, 3, branches);
        struct mo_core *core = checked(&f, present ? "pattern-some" : "pattern-none",
                                       present ? "returned\n20\n" : "returned\n0\n");
        if (core != NULL)
            run(core, present ? 20 : 0);
        lower_fixture_free(&f);
    }
}

static void test_loops(void) {
    struct lower_fixture f;
    lower_fixture_init(&f, "module Main\nfn main() : Int\nvar n = 0\n"
                           "for i in 2..8\ncontinue if i == 3\nn += i\nbreak if n > 12\nend\n"
                           "while n < 25\nn = n + 2\nreturn n if n == 21\nend\n255\nend\n");
    f.bindings[0] = (struct mo_checked_binding){.name = "n", .type = &f.int_, .variable = true};
    f.bindings[1] = (struct mo_checked_binding){.name = "i", .type = &f.int_};
    f.functions[0].body.binding_count = 2;
    uint32_t zero = lower_fixture_row(&f, MO_S_INT, &f.int_, "0", 0, NULL);
    uint32_t start = lower_fixture_bind(&f, 0, zero, false);
    uint32_t two = lower_fixture_row(&f, MO_S_INT, &f.int_, "2", 0, NULL);
    uint32_t eight = lower_fixture_row(&f, MO_S_INT, &f.int_, "8", 0, NULL);
    uint32_t range = lower_fixture_op(&f, MO_T_DOTDOT, &f.range, two, eight);
    uint32_t i = lower_fixture_read(&f, 1);
    uint32_t three = lower_fixture_row(&f, MO_S_INT, &f.int_, "3", 0, NULL);
    uint32_t skip = lower_fixture_op(&f, MO_T_EQ, &f.bool_, i, three);
    uint32_t cont = lower_fixture_row(&f, MO_S_CONTINUE, &f.unit_, NULL, 1, &skip);
    struct mo_syntax *guard[] = {&f.syntax[skip]};
    f.syntax[cont].children = guard;
    uint32_t n = lower_fixture_read(&f, 0);
    uint32_t sum = lower_fixture_op(&f, MO_T_PLUS, &f.int_, n, i);
    f.syntax[sum].kind = MO_S_ADD_ASSIGN; /* normalized operator borrows the write's syntax */
    uint32_t write = lower_fixture_bind(&f, 0, sum, true);
    f.syntax[write].kind = MO_S_ADD_ASSIGN;
    uint32_t twelve = lower_fixture_row(&f, MO_S_INT, &f.int_, "12", 0, NULL);
    uint32_t gt = lower_fixture_op(&f, MO_T_GT, &f.bool_, n, twelve);
    uint32_t brk = lower_fixture_row(&f, MO_S_BREAK, &f.unit_, NULL, 1, &gt);
    f.syntax[brk].children = guard;
    uint32_t body_args[] = {cont, write, brk};
    uint32_t body = lower_fixture_row(&f, MO_S_SEQUENCE, &f.unit_, NULL, 3, body_args);
    uint32_t for_args[] = {range, body};
    uint32_t for_ = lower_fixture_row(&f, MO_S_FOR, &f.unit_, NULL, 2, for_args);
    f.rows[for_].binding = &f.bindings[1];
    uint32_t twenty_five = lower_fixture_row(&f, MO_S_INT, &f.int_, "25", 0, NULL);
    uint32_t lt = lower_fixture_op(&f, MO_T_LT, &f.bool_, n, twenty_five);
    uint32_t add_two = lower_fixture_op(&f, MO_T_PLUS, &f.int_, n, two);
    uint32_t write_two = lower_fixture_bind(&f, 0, add_two, true);
    uint32_t twenty_one = lower_fixture_row(&f, MO_S_INT, &f.int_, "21", 0, NULL);
    uint32_t eq = lower_fixture_op(&f, MO_T_EQ, &f.bool_, n, twenty_one);
    uint32_t return_args[] = {eq, n};
    uint32_t ret = lower_fixture_row(&f, MO_S_RETURN, &f.unit_, NULL, 2, return_args);
    struct mo_syntax *return_syntax[] = {&f.syntax[n], &f.syntax[eq]};
    f.syntax[ret].children = return_syntax;
    uint32_t while_body_args[] = {write_two, ret};
    uint32_t while_body = lower_fixture_row(&f, MO_S_SEQUENCE, &f.unit_, NULL, 2, while_body_args);
    uint32_t while_args[] = {lt, while_body};
    uint32_t while_ = lower_fixture_row(&f, MO_S_WHILE, &f.unit_, NULL, 2, while_args);
    uint32_t fallback = lower_fixture_row(&f, MO_S_INT, &f.int_, "255", 0, NULL);
    uint32_t sequence[] = {start, for_, while_, fallback};
    (void)lower_fixture_row(&f, MO_S_SEQUENCE, &f.int_, NULL, 4, sequence);
    (void)checked(&f, "loops", "returned\n21\n");
    lower_fixture_free(&f);
}

static void test_lambda(void) {
    struct lower_fixture f;
    lower_fixture_init(&f, "module Main\nfn main() : Int\nf = fn(n) n + 1 end\nf(7)\nend\n");
    f.bindings[0] = (struct mo_checked_binding){.name = "n", .type = &f.int_};
    f.functions[0].body.binding_count = 1;
    uint32_t n = lower_fixture_read(&f, 0);
    uint32_t one = lower_fixture_row(&f, MO_S_INT, &f.int_, "1", 0, NULL);
    uint32_t sum = lower_fixture_op(&f, MO_T_PLUS, &f.int_, n, one);
    const struct mo_checked_binding *parameters[] = {&f.bindings[0]};
    struct mo_checked_lambda lambda = {
        .number = 1, .parameter_count = 1, .parameters = parameters, .body = sum};
    const struct mo_source_type *parts[] = {&f.int_, &f.int_};
    struct mo_source_type function = {.kind = MO_SOURCE_FUNCTION, .count = 2, .arguments = parts};
    uint32_t value = lower_fixture_row(&f, MO_S_LAMBDA, &function, NULL, 1, &sum);
    f.rows[value].lambda = &lambda;
    uint32_t seven = lower_fixture_row(&f, MO_S_INT, &f.int_, "7", 0, NULL);
    uint32_t args[] = {value, seven};
    (void)lower_fixture_row(&f, MO_S_CALL, &f.int_, NULL, 2, args);
    struct mo_core *core = checked(&f, "lambda", "returned\n8\n");
    if (core != NULL)
        run(core, 8);
    lambda.capture_count = 1;
    CHECK(mo_lower(&f.arena, &f.program, &f.sources, &f.diags) == NULL);
    CHECK(f.diags.items[0].code == MO_CODE_LOWER_UNFINISHED);
    lower_fixture_free(&f);
}

static void test_text(void) {
    struct lower_fixture f;
    lower_fixture_init(
        &f, "module Main\nfn main() : Int\n"
            "if \"\"\"\r\n  one\r\n    two\r\n  \"\"\" == \"one\\n  two\": 19 else: 29\nend\n");
    uint32_t a = lower_fixture_row(&f, MO_S_TEXT, &f.text_,
                                   "\"\"\"\r\n  one\r\n    two\r\n  \"\"\"", 0, NULL);
    uint32_t b = lower_fixture_row(&f, MO_S_TEXT, &f.text_, "\"one\\n  two\"", 0, NULL);
    uint32_t eq = lower_fixture_op(&f, MO_T_EQ, &f.bool_, a, b);
    uint32_t yes = lower_fixture_row(&f, MO_S_INT, &f.int_, "19", 0, NULL);
    uint32_t no = lower_fixture_row(&f, MO_S_INT, &f.int_, "29", 0, NULL);
    uint32_t arms[] = {eq, yes, no};
    (void)lower_fixture_row(&f, MO_S_IF, &f.int_, NULL, 3, arms);
    struct mo_core *core = checked(&f, "text", "returned\n19\n");
    if (core != NULL)
        run(core, 19);
    lower_fixture_free(&f);
}

static void test_result_entry(void) {
    for (uint32_t capabilities = 0; capabilities < 3; capabilities++) {
        struct lower_fixture f;
        lower_fixture_init(
            &f, "module Main\nfn main() : Result(Unit, Text)\nError(\"problem\")\nend\n");
        const struct mo_source_type *parts[] = {&f.unit_, &f.text_};
        struct mo_source_type result = {.kind = MO_SOURCE_NAMED,
                                        .module = UINT32_MAX,
                                        .name = "Result",
                                        .count = 2,
                                        .arguments = parts};
        struct mo_source_type args = {
            .kind = MO_SOURCE_NAMED, .module = UINT32_MAX, .name = "Args"};
        struct mo_source_type console = args;
        console.name = "Console";
        f.bindings[0] = (struct mo_checked_binding){.name = "args", .type = &args};
        f.bindings[1] = (struct mo_checked_binding){.name = "console", .type = &console};
        const struct mo_checked_binding *parameters[] = {&f.bindings[0], &f.bindings[1]};
        f.functions[0].body.binding_count = capabilities;
        f.functions[0].parameter_count = capabilities;
        f.functions[0].parameters = parameters;
        f.function_types[0].count = capabilities + 1;
        f.signature_parts[0][0] = &args;
        f.signature_parts[0][1] = &console;
        f.signature_parts[0][capabilities] = &result;
        f.program.entry.result = MO_ENTRY_RESULT;
        f.program.entry.capability_count = capabilities;
        f.program.entry.capabilities = parameters;
        struct mo_ability_implementation text = {
            .ability = MO_SOURCE_TEXT, .kind = MO_ABILITY_NATIVE, .type = &f.text_};
        f.program.entry.error_text = &text;
        uint32_t problem = lower_fixture_row(&f, MO_S_TEXT, &f.text_, "\"problem\"", 0, NULL);
        uint32_t error = lower_fixture_row(&f, MO_S_CALL, &result, NULL, 1, &problem);
        f.rows[error].native = "Error";
        const char *names[] = {"result-no-cap", "result-args", "result-console"};
        struct mo_core *core =
            checked(&f, names[capabilities], "returned\n1\n{\"stderr\":[\"problem\"]}\n");
        if (core != NULL) {
            const struct mo_core_fun *entry = &core->funs.items[core->funs.count - 1];
            CHECK(entry->params.count == (capabilities == 0 ? 1 : 2));
            if (capabilities != 0)
                CHECK(entry->params.items[0].ty->as.cap->tag == MO_CORE_CAP_ARGS);
            CHECK(entry->params.items[entry->params.count - 1].ty->as.cap->tag ==
                  MO_CORE_CAP_CONSOLE);
            CHECK(core->funs.items[0].params.count == capabilities);
        }
        lower_fixture_free(&f);
    }
}

static void test_limits(void) {
    struct lower_fixture f;
    lower_fixture_init(&f, "module Main\nfn main() : Int\n0 + 1\nend\n");
    uint32_t zero = lower_fixture_row(&f, MO_S_INT, &f.int_, "0", 0, NULL);
    uint32_t one = lower_fixture_row(&f, MO_S_INT, &f.int_, "1", 0, NULL);
    uint32_t last = zero;
    for (unsigned i = 0; i < 253; i++)
        last = lower_fixture_op(&f, MO_T_PLUS, &f.int_, last, one);
    struct mo_core *core = mo_lower(&f.arena, &f.program, &f.sources, &f.diags);
    CHECK(core != NULL && !mo_diags_any(&f.diags));
    if (core != NULL) {
        MO_ASSERT(mo_core_check(core, &f.diags));
        run(core, 253);
        struct mo_buffer printed;
        mo_buffer_init(&printed, MO_LIMIT_CORE_TEXT_SIZE);
        CHECK(!mo_core_print(core, &printed)); /* Codec depth must not gate compilation. */
        mo_buffer_free(&printed);
    }
    mo_diags_free(&f.diags);
    MO_ASSERT(mo_diags_init(&f.diags));
    mo_arena_free(&f.arena);
    mo_arena_init(&f.arena, 0);
    CHECK(mo_lower(&f.arena, &f.program, &f.sources, &f.diags) == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_LOWER_NO_MEMORY);
    lower_fixture_free(&f);
}

static void test_native_refusal(void) {
    const char *names[] = {"Console.read_line", "unknown"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        struct lower_fixture f;
        lower_fixture_init(&f, "module Main\nfn main() : Int\n1\nend\n");
        uint32_t value = lower_fixture_row(&f, MO_S_INT, &f.int_, "1", 0, NULL);
        uint32_t call = lower_fixture_row(&f, MO_S_CALL, &f.int_, NULL, 1, &value);
        f.rows[call].native = names[i];
        CHECK(mo_lower(&f.arena, &f.program, &f.sources, &f.diags) == NULL);
        CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_LOWER_UNFINISHED);
        lower_fixture_free(&f);
    }
}

static void test_ignored_parameters(void) {
    struct lower_fixture f;
    lower_fixture_init(&f, "module Main\nfn identity(_unused: Int) : Int\n"
                           "f = fn(_)\n17\nend\nf(41)\nend\n"
                           "fn main() : Int\nidentity(9)\nend\n");
    f.program.function_count = f.declarations.count = 2;
    f.function_types[1].count = 2;
    f.signature_parts[1][0] = f.signature_parts[1][1] = &f.int_;
    f.bindings[0] = (struct mo_checked_binding){.name = "_unused", .type = &f.int_};
    const struct mo_checked_binding *parameters[] = {&f.bindings[0]};
    f.functions[1].parameter_count = 1;
    f.functions[1].body.binding_count = 1;
    f.functions[1].parameters = parameters;
    uint32_t body = lower_fixture_row(&f, MO_S_INT, &f.int_, "17", 0, NULL);
    const struct mo_checked_binding *ignored[] = {NULL};
    struct mo_checked_lambda lambda = {.parameter_count = 1, .parameters = ignored, .body = body};
    uint32_t value = lower_fixture_row(&f, MO_S_LAMBDA, &f.function_types[1], NULL, 1, &body);
    f.rows[value].lambda = &lambda;
    uint32_t forty_one = lower_fixture_row(&f, MO_S_INT, &f.int_, "41", 0, NULL);
    uint32_t application[] = {value, forty_one};
    f.functions[1].body.root = lower_fixture_row(&f, MO_S_CALL, &f.int_, NULL, 2, application);
    f.functions[1].body.count = f.count;
    uint32_t callee = lower_fixture_row(&f, MO_S_NAME, &f.function_types[1], NULL, 0, NULL);
    f.rows[callee].symbol = &f.symbols[1];
    uint32_t nine = lower_fixture_row(&f, MO_S_INT, &f.int_, "9", 0, NULL);
    uint32_t arguments[] = {callee, nine};
    uint32_t call = lower_fixture_row(&f, MO_S_CALL, &f.int_, NULL, 2, arguments);
    f.rows[call].symbol = &f.symbols[1];
    struct mo_core *core = checked(&f, "ignored-parameters", "returned\n17\n");
    if (core != NULL) {
        CHECK(core->metadata.funs.items[1].body.params.items[0].some);
        run(core, 17);
    }
    lower_fixture_free(&f);
}

static void test_substituted_depth(void) {
    /* Substitution deepens this generic result without deepening its template. */
    const uint32_t depths[] = {MO_LIMIT_CORE_DEPTH - 16, MO_LIMIT_CORE_DEPTH - 3,
                               MO_LIMIT_CORE_DEPTH - 1};
    for (size_t index = 0; index < sizeof depths / sizeof depths[0]; index++) {
        uint32_t depth = depths[index];
        struct lower_fixture f;
        lower_fixture_init(&f, "module Main\nfn widen(_value: T) : List(T)\n[]\nend\n"
                               "fn main() : Int\nwiden([])\n0\nend\n");
        f.program.function_count = f.declarations.count = 2;
        struct mo_source_type parameter = {.kind = MO_SOURCE_PARAMETER,
                                           .name = "T",
                                           .declaration = &f.symbols[1],
                                           .parameters = 1};
        const struct mo_source_type *part = &parameter;
        struct mo_source_type result = {.kind = MO_SOURCE_NAMED,
                                        .name = "List",
                                        .module = UINT32_MAX,
                                        .count = 1,
                                        .arguments = &part,
                                        .parameters = 1};
        f.function_types[1].count = 2;
        f.function_types[1].parameters = 1;
        f.signature_parts[1][0] = &parameter;
        f.signature_parts[1][1] = &result;
        f.bindings[0] = (struct mo_checked_binding){.name = "_value", .type = &parameter};
        const struct mo_checked_binding *parameters[] = {&f.bindings[0]};
        f.functions[1].parameter_count = f.functions[1].body.binding_count = 1;
        f.functions[1].parameters = parameters;
        f.functions[1].body.root = lower_fixture_row(&f, MO_S_LIST, &result, NULL, 0, NULL);
        f.functions[1].body.count = 1;
        struct mo_source_type types[MO_LIMIT_CORE_DEPTH + 1];
        const struct mo_source_type *parts[MO_LIMIT_CORE_DEPTH];
        types[0] = f.int_;
        for (uint32_t i = 1; i <= depth + 1; i++) {
            parts[i - 1] = &types[i - 1];
            types[i] = (struct mo_source_type){.kind = MO_SOURCE_NAMED,
                                               .name = "List",
                                               .module = UINT32_MAX,
                                               .count = 1,
                                               .arguments = &parts[i - 1],
                                               .height = i};
        }
        uint32_t callee = lower_fixture_row(&f, MO_S_NAME, &f.function_types[1], NULL, 0, NULL);
        f.rows[callee].symbol = &f.symbols[1];
        uint32_t value = lower_fixture_row(&f, MO_S_LIST, &types[depth], NULL, 0, NULL);
        uint32_t arguments[] = {callee, value};
        uint32_t call = lower_fixture_row(&f, MO_S_CALL, &types[depth + 1], NULL, 2, arguments);
        struct mo_type_argument argument = {&parameter, &types[depth]};
        f.rows[call].symbol = &f.symbols[1];
        f.rows[call].arguments = (struct mo_type_arguments){1, &argument};
        uint32_t zero = lower_fixture_row(&f, MO_S_INT, &f.int_, "0", 0, NULL);
        uint32_t statements[] = {call, zero};
        (void)lower_fixture_row(&f, MO_S_SEQUENCE, &f.int_, NULL, 2, statements);
        struct mo_core *core = mo_lower(&f.arena, &f.program, &f.sources, &f.diags);
        if (index < 2) {
            CHECK(core != NULL && !mo_diags_any(&f.diags));
            if (core != NULL)
                MO_ASSERT(mo_core_check(core, &f.diags));
        } else {
            CHECK(core == NULL);
            CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_LOWER_DEPTH);
        }
        lower_fixture_free(&f);
    }
}

static struct mo_core *source_core(struct mo_arena *output, struct mo_buffer *text,
                                   struct mo_diags *diags) {
    struct mo_arena input;
    struct mo_sources sources;
    mo_arena_init(&input, MO_LIMIT_LOWER_WORK);
    MO_ASSERT(mo_sources_init(&sources));
    uint32_t file;
    struct mo_source_error error;
    MO_ASSERT(mo_sources_add_bytes(&sources, "main.mo", text->bytes, text->length, &file, &error));
    struct mo_module_syntax *module = mo_parse(&input, &sources, file, diags);
    const char *name = "Main";
    struct mo_module_set set = {.count = 1, .modules = &module, .names = &name};
    struct mo_program *program = module == NULL ? NULL : mo_check(&input, &set, diags);
    MO_ASSERT(program != NULL);
    struct mo_core *core = mo_lower(output, program, &sources, diags);
    mo_sources_free(&sources);
    mo_arena_free(&input);
    if (core != NULL)
        MO_ASSERT(mo_core_check(core, diags));
    return core;
}

static void test_source(void *unused) {
    (void)unused;
    struct mo_arena output;
    struct mo_diags diags;
    struct mo_buffer source, printed;
    mo_buffer_init(&source, MO_LIMIT_FILE_SIZE);
    mo_buffer_init(&printed, MO_LIMIT_CORE_TEXT_SIZE);
    size_t previous = 0;
    for (uint32_t branches = 4; branches <= 16; branches *= 2) {
        mo_arena_init(&output, MO_LIMIT_LOWER_WORK);
        MO_ASSERT(mo_diags_init(&diags));
        mo_buffer_clear(&source);
        MO_ASSERT(mo_buffer_append_text(&source, "module Main\nfn main() : Int\nx = false\n"));
        for (uint32_t i = 0; i < branches; i++)
            MO_ASSERT(mo_buffer_append_text(&source,
                                            i == 0 ? "if x and x\n7\n" : "else if x and x\n7\n"));
        MO_ASSERT(mo_buffer_append_text(&source, "else\n41\nend\nend\n"));
        struct mo_core *core = source_core(&output, &source, &diags);
        CHECK(core != NULL);
        if (core != NULL) {
            mo_buffer_clear(&printed);
            CHECK(mo_core_print(core, &printed));
            /* Doubling branches may double code, never multiply it sixteenfold. */
            CHECK(previous == 0 || printed.length <= 3 * previous);
            previous = printed.length;
        }
        mo_diags_free(&diags);
        mo_arena_free(&output);
    }
    mo_buffer_clear(&source);
    MO_ASSERT(mo_buffer_append_text(&source, "module Main\nfn main() : Int\nvar n = 0\n"));
    for (uint32_t i = 0; i < 80; i++)
        MO_ASSERT(mo_buffer_append_text(&source, "n += 1\n"));
    MO_ASSERT(mo_buffer_append_text(&source, "n\nend\n"));
    mo_arena_init(&output, MO_LIMIT_LOWER_WORK);
    MO_ASSERT(mo_diags_init(&diags));
    struct mo_core *core = source_core(&output, &source, &diags);
    CHECK(core != NULL);
    if (core != NULL)
        run(core, 80);
    mo_arena_free(&output);
    mo_diags_free(&diags);
    for (uint32_t statements = MO_LIMIT_CORE_DEPTH - 8; statements <= MO_LIMIT_CORE_DEPTH;
         statements++) {
        mo_buffer_clear(&source);
        MO_ASSERT(mo_buffer_append_text(&source, "module Main\nfn main() : Int\nvar n = 0\n"));
        for (uint32_t i = 0; i < statements; i++)
            MO_ASSERT(mo_buffer_append_text(&source, "n += 1\n"));
        MO_ASSERT(mo_buffer_append_text(&source, "n\nend\n"));
        mo_arena_init(&output, MO_LIMIT_LOWER_WORK);
        MO_ASSERT(mo_diags_init(&diags));
        core = source_core(&output, &source, &diags);
        if (statements < MO_LIMIT_CORE_DEPTH - 2) {
            CHECK(core != NULL && !mo_diags_any(&diags));
            if (core != NULL)
                run(core, statements);
        } else {
            CHECK(core == NULL && diags.count == 1);
            CHECK(diags.items[0].code == MO_CODE_LOWER_DEPTH);
        }
        mo_arena_free(&output);
        mo_diags_free(&diags);
    }
    mo_buffer_clear(&source);
    MO_ASSERT(mo_buffer_append_text(&source, "module Main\nfn main() : Int\n17\nend\n"));
    for (uint32_t i = 0; i < 10000; i++) {
        MO_ASSERT(mo_buffer_append_text(&source, "fn f"));
        MO_ASSERT(mo_buffer_append_uint(&source, i));
        MO_ASSERT(
            mo_buffer_append_text(&source, "(a: Int, b: Int, c: Int) : Int\na + b + c\nend\n"));
    }
    mo_arena_init(&output, MO_LIMIT_LOWER_WORK);
    MO_ASSERT(mo_diags_init(&diags));
    core = source_core(&output, &source, &diags);
    CHECK(core != NULL && !mo_diags_any(&diags));
    if (core != NULL) {
        struct mo_buffer bytecode;
        mo_buffer_init(&bytecode, MO_LIMIT_BYTECODE_SIZE);
        CHECK(mo_compile(core, &bytecode, &diags));
        mo_buffer_free(&bytecode);
        mo_buffer_clear(&printed);
        CHECK(!mo_core_print(core, &printed)); /* Metadata exceeds the codec node allowance. */
    }
    mo_diags_free(&diags);
    mo_arena_free(&output);
    mo_buffer_free(&source);
    mo_buffer_free(&printed);
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--corpus") == 0)
        corpus = argv[2];
    else
        MO_ASSERT(argc == 1);
    test_refusal();
    test_literal();
    test_branch_variables();
    test_instances();
    test_pattern_scope();
    test_loops();
    test_lambda();
    test_text();
    test_result_entry();
    test_limits();
    test_native_refusal();
    test_ignored_parameters();
    test_substituted_depth();
    MO_ASSERT(mo_with_stack(MO_SOURCE_STACK_SIZE, test_source, NULL) == 0);
    return mo_test_report("lower");
}
