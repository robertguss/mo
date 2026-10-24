#include "base/codes.h"
#include "base/limits.h"
#include "base/stack.h"
#include "check/check.h"
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

static void run_outcome_bytes(struct mo_core *core, const char *name, enum mo_outcome outcome,
                              int64_t expected, const char *detail, size_t detail_length) {
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
            bool integer = false;
            mo_core_string target =
                name == NULL ? core->entry : (mo_core_string){name, strlen(name)};
            for (size_t i = 0; i < core->funs.count; i++)
                if (target.length == core->funs.items[i].name.length &&
                    memcmp(target.bytes, core->funs.items[i].name.bytes, target.length) == 0) {
                    integer = core->funs.items[i].ret->tag == MO_CORE_TY_INT;
                    mo_core_string display = core->metadata.funs.items[i].name;
                    for (uint32_t j = 0; j < file->function_count; j++) {
                        const struct mo_bytecode_string *s =
                            &file->strings[file->functions[j].name];
                        if (s->length == display.length &&
                            memcmp(s->bytes, display.bytes, display.length) == 0)
                            entry = j;
                    }
                }
            CHECK(entry != UINT32_MAX);
            enum mo_outcome actual = mo_call(process, entry, NULL, 0, &result, &defect);
            CHECK(actual == outcome);
            if (actual == MO_RETURNED && integer)
                CHECK(mo_cell_get_int(result) == expected);
            if (actual == outcome && detail != NULL) {
                CHECK(defect.has_detail);
                if (defect.has_detail) {
                    size_t length;
                    const uint8_t *value = mo_value_data(defect.detail, &length);
                    CHECK(length == detail_length && memcmp(value, detail, length) == 0);
                }
            }
            mo_process_free(process);
        }
        mo_image_free(image);
    }
    mo_buffer_free(&bytes);
    mo_diags_free(&diags);
}

static void run_outcome(struct mo_core *core, const char *name, enum mo_outcome outcome,
                        int64_t expected, const char *detail) {
    run_outcome_bytes(core, name, outcome, expected, detail, detail == NULL ? 0 : strlen(detail));
}

static void run(struct mo_core *core, int64_t expected) {
    run_outcome(core, NULL, MO_RETURNED, expected, NULL);
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
    struct mo_function_signature signature = {.function = &symbol, .type = &function_type};
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
    lower_fixture_free(&f);
}

static void test_captures(void) {
    struct lower_fixture f;
    lower_fixture_init(&f, "module Main\nfn main() : Int\nx = 9\ny = 2\n"
                           "f = fn() y * 100 + x end\nf()\nend\n");
    f.bindings[0] = (struct mo_checked_binding){.name = "x", .type = &f.int_};
    f.bindings[1] = (struct mo_checked_binding){.name = "y", .type = &f.int_};
    f.functions[0].body.binding_count = 2;
    uint32_t nine = lower_fixture_row(&f, MO_S_INT, &f.int_, "9", 0, NULL);
    uint32_t x = lower_fixture_bind(&f, 0, nine, false);
    uint32_t two = lower_fixture_row(&f, MO_S_INT, &f.int_, "2", 0, NULL);
    uint32_t y = lower_fixture_bind(&f, 1, two, false);
    uint32_t read_y = lower_fixture_read(&f, 1);
    uint32_t hundred = lower_fixture_row(&f, MO_S_INT, &f.int_, "100", 0, NULL);
    uint32_t product = lower_fixture_op(&f, MO_T_STAR, &f.int_, read_y, hundred);
    uint32_t read_x = lower_fixture_read(&f, 0);
    uint32_t sum = lower_fixture_op(&f, MO_T_PLUS, &f.int_, product, read_x);
    const struct mo_checked_binding *captures[] = {&f.bindings[1], &f.bindings[0]};
    struct mo_checked_lambda lambda = {
        .number = 1, .body = sum, .capture_count = 2, .captures = captures};
    uint32_t value = lower_fixture_row(&f, MO_S_LAMBDA, &f.function_types[0], NULL, 1, &sum);
    f.rows[value].lambda = &lambda;
    uint32_t call = lower_fixture_row(&f, MO_S_CALL, &f.int_, NULL, 1, &value);
    uint32_t sequence[] = {x, y, call};
    (void)lower_fixture_row(&f, MO_S_SEQUENCE, &f.int_, NULL, 3, sequence);
    struct mo_core *core = checked(&f, "captures", "returned\n209\n");
    if (core != NULL)
        run(core, 209);
    lower_fixture_free(&f);
}

static void test_option_or(void) {
    for (uint32_t some = 0; some < 2; some++) {
        struct lower_fixture f;
        lower_fixture_init(&f, some ? "module Main\nfn main() : Int\nSome(17) or 1 / 0\nend\n"
                                    : "module Main\nfn main() : Int\nNone or 37\nend\n");
        const struct mo_source_type *part = &f.int_;
        struct mo_source_type option = {.kind = MO_SOURCE_NAMED,
                                        .name = "Option",
                                        .module = UINT32_MAX,
                                        .count = 1,
                                        .arguments = &part};
        uint32_t value = 0;
        if (some)
            value = lower_fixture_row(&f, MO_S_INT, &f.int_, "17", 0, NULL);
        uint32_t input = lower_fixture_row(&f, MO_S_CALL, &option, NULL, some, &value);
        f.rows[input].native = some ? "Some" : "None";
        uint32_t fallback;
        if (some) {
            uint32_t one = lower_fixture_row(&f, MO_S_INT, &f.int_, "1", 0, NULL);
            uint32_t zero = lower_fixture_row(&f, MO_S_INT, &f.int_, "0", 0, NULL);
            fallback = lower_fixture_op(&f, MO_T_SLASH, &f.int_, one, zero);
        } else
            fallback = lower_fixture_row(&f, MO_S_INT, &f.int_, "37", 0, NULL);
        uint32_t or_ = lower_fixture_op(&f, MO_T_OR, &f.int_, input, fallback);
        f.rows[or_].or_kind = MO_CHECK_OR_OPTION;
        struct mo_core *core = checked(&f, some ? "option-some" : "option-none",
                                       some ? "returned\n17\n" : "returned\n37\n");
        if (core != NULL)
            run(core, some ? 17 : 37);
        lower_fixture_free(&f);
    }
}

static void test_try_option(void) {
    for (uint32_t some = 0; some < 2; some++) {
        struct lower_fixture f;
        lower_fixture_init(&f, "module Main\nfn probe() : Option(Int)\n"
                               "Some(2 + try Some(17))\nend\n"
                               "fn main() : Int\nprobe() or 37\nend\n");
        f.program.function_count = f.declarations.count = 2;
        f.symbols[1].name = "probe";
        const struct mo_source_type *part = &f.int_;
        struct mo_source_type option = {.kind = MO_SOURCE_NAMED,
                                        .name = "Option",
                                        .module = UINT32_MAX,
                                        .count = 1,
                                        .arguments = &part};
        f.signature_parts[1][0] = &option;
        uint32_t two = lower_fixture_row(&f, MO_S_INT, &f.int_, "2", 0, NULL);
        uint32_t seventeen = lower_fixture_row(&f, MO_S_INT, &f.int_, "17", 0, NULL);
        uint32_t input = lower_fixture_row(&f, MO_S_CALL, &option, NULL, some, &seventeen);
        f.rows[input].native = some ? "Some" : "None";
        struct mo_checked_try try_ = {.result = &option, .failure.builtin = "None"};
        uint32_t unwrap = lower_fixture_row(&f, MO_S_TRY, &f.int_, NULL, 1, &input);
        f.rows[unwrap].try_ = &try_;
        uint32_t sum = lower_fixture_op(&f, MO_T_PLUS, &f.int_, two, unwrap);
        f.functions[1].body.root = lower_fixture_row(&f, MO_S_CALL, &option, NULL, 1, &sum);
        f.rows[f.functions[1].body.root].native = "Some";
        f.functions[1].body.count = f.count;
        uint32_t callee = lower_fixture_row(&f, MO_S_NAME, &f.function_types[1], NULL, 0, NULL);
        f.rows[callee].symbol = &f.symbols[1];
        uint32_t call = lower_fixture_row(&f, MO_S_CALL, &option, NULL, 1, &callee);
        f.rows[call].symbol = &f.symbols[1];
        uint32_t fallback = lower_fixture_row(&f, MO_S_INT, &f.int_, "37", 0, NULL);
        uint32_t or_ = lower_fixture_op(&f, MO_T_OR, &f.int_, call, fallback);
        f.rows[or_].or_kind = MO_CHECK_OR_OPTION;
        (void)checked(&f, some ? "try-option-some" : "try-option-none",
                      some ? "returned\n19\n" : "returned\n37\n");
        lower_fixture_free(&f);
    }
}

static void test_try_result(void) {
    for (uint32_t wrapped = 0; wrapped < 2; wrapped++) {
        struct lower_fixture f;
        lower_fixture_init(&f, wrapped ? "module Main\nfn main() : Int\ncase probe()\n"
                                         "Ok(_): 1\nError(Some(\"bad\")): 43\nError(_): 1\n"
                                         "end\nend\nfn probe() : Result(Int, Option(Text))\n"
                                         "Ok(2 + try Error(\"bad\"))\nend\n"
                                       : "module Main\nfn main() : Int\ncase probe()\n"
                                         "Ok(_): 1\nError(\"bad\"): 43\nError(_): 1\n"
                                         "end\nend\nfn probe() : Result(Int, Text)\n"
                                         "Ok(2 + try Error(\"bad\"))\nend\n");
        f.program.function_count = f.declarations.count = 2;
        f.symbols[1].name = "probe";
        const struct mo_source_type *text_part = &f.text_;
        struct mo_source_type option = {.kind = MO_SOURCE_NAMED,
                                        .name = "Option",
                                        .module = UINT32_MAX,
                                        .count = 1,
                                        .arguments = &text_part};
        const struct mo_source_type *inner_parts[] = {&f.int_, &f.text_};
        const struct mo_source_type *outer_parts[] = {&f.int_, wrapped ? &option : &f.text_};
        struct mo_source_type inner = {.kind = MO_SOURCE_NAMED,
                                       .name = "Result",
                                       .module = UINT32_MAX,
                                       .count = 2,
                                       .arguments = inner_parts};
        struct mo_source_type outer = inner;
        outer.arguments = outer_parts;
        f.signature_parts[1][0] = &outer;
        uint32_t two = lower_fixture_row(&f, MO_S_INT, &f.int_, "2", 0, NULL);
        uint32_t text = lower_fixture_row(&f, MO_S_TEXT, &f.text_, "\"bad\"", 0, NULL);
        uint32_t input = lower_fixture_row(&f, MO_S_CALL, &inner, NULL, 1, &text);
        f.rows[input].native = "Error";
        struct mo_checked_try try_ = {.result = &outer,
                                      .failure.builtin = "Error",
                                      .wrapper.builtin = wrapped ? "Some" : NULL};
        uint32_t unwrap = lower_fixture_row(&f, MO_S_TRY, &f.int_, NULL, 1, &input);
        f.rows[unwrap].try_ = &try_;
        uint32_t sum = lower_fixture_op(&f, MO_T_PLUS, &f.int_, two, unwrap);
        f.functions[1].body.root = lower_fixture_row(&f, MO_S_CALL, &outer, NULL, 1, &sum);
        f.rows[f.functions[1].body.root].native = "Ok";
        f.functions[1].body.count = f.count;
        uint32_t callee = lower_fixture_row(&f, MO_S_NAME, &f.function_types[1], NULL, 0, NULL);
        f.rows[callee].symbol = &f.symbols[1];
        uint32_t call = lower_fixture_row(&f, MO_S_CALL, &outer, NULL, 1, &callee);
        f.rows[call].symbol = &f.symbols[1];
        uint32_t yes = lower_fixture_row(&f, MO_S_INT, &f.int_, "43", 0, NULL);
        uint32_t no = lower_fixture_row(&f, MO_S_INT, &f.int_, "1", 0, NULL);
        struct mo_checked_pattern literal = {
            .kind = MO_PATTERN_LITERAL, .syntax = &f.syntax[text], .type = &f.text_};
        struct mo_checked_pattern_part text_payload = {0, &literal};
        struct mo_checked_pattern some = {.kind = MO_PATTERN_CONSTRUCTOR,
                                          .syntax = &f.syntax[text],
                                          .type = &option,
                                          .constructor.builtin = "Some",
                                          .part_count = 1,
                                          .parts = &text_payload};
        struct mo_checked_pattern_part payload = {0, wrapped ? &some : &literal};
        struct mo_checked_pattern error = {.kind = MO_PATTERN_CONSTRUCTOR,
                                           .syntax = &f.syntax[call],
                                           .type = &outer,
                                           .constructor.builtin = "Error",
                                           .part_count = 1,
                                           .parts = &payload};
        struct mo_checked_pattern any_int = {
            .kind = MO_PATTERN_ANY, .syntax = &f.syntax[call], .type = &f.int_};
        struct mo_checked_pattern any_error = {
            .kind = MO_PATTERN_ANY, .syntax = &f.syntax[call], .type = outer_parts[1]};
        struct mo_checked_pattern_part ok_payload = {0, &any_int};
        struct mo_checked_pattern_part any_payload = {0, &any_error};
        struct mo_checked_pattern ok = error, fallback = error;
        ok.constructor.builtin = "Ok";
        ok.parts = &ok_payload;
        fallback.parts = &any_payload;
        const struct mo_checked_pattern *patterns[] = {&ok, &error, &fallback};
        struct mo_checked_arm arms[] = {{.syntax = &f.syntax[call],
                                         .pattern_count = 1,
                                         .patterns = &patterns[0],
                                         .guard = UINT32_MAX,
                                         .body = no},
                                        {.syntax = &f.syntax[call],
                                         .pattern_count = 1,
                                         .patterns = &patterns[1],
                                         .guard = UINT32_MAX,
                                         .body = yes},
                                        {.syntax = &f.syntax[call],
                                         .pattern_count = 1,
                                         .patterns = &patterns[2],
                                         .guard = UINT32_MAX,
                                         .body = no}};
        struct mo_checked_match match = {.scrutinee = call, .arm_count = 3, .arms = arms};
        uint32_t case_ = lower_fixture_row(&f, MO_S_CASE, &f.int_, NULL, 0, NULL);
        f.rows[case_].match = &match;
        (void)checked(&f, wrapped ? "try-result-wrap" : "try-result-direct", "returned\n43\n");
        lower_fixture_free(&f);
    }
}

static void test_interpolation(void) {
    for (uint32_t triple = 0; triple < 2; triple++) {
        struct lower_fixture f;
        lower_fixture_init(
            &f, triple ? "module Main\nfn main() : Int\n"
                         "if \"\"\"\r\n  a\r\n  #{7}: #{false}λ\r\n  \"\"\""
                         " == \"a\\n7: falseλ\": 19 else: 1\nend\n"
                       : "module Main\nfn main() : Int\n"
                         "if \"a\\n#{7}: #{false}λ\" == \"a\\n7: falseλ\": 19 else: 1\nend\n");
        uint32_t seven = lower_fixture_row(&f, MO_S_INT, &f.int_, "7", 0, NULL);
        uint32_t false_ = lower_fixture_row(&f, MO_S_FALSE, &f.bool_, "false", 0, NULL);
        uint32_t operands[] = {seven, false_};
        uint32_t text = lower_fixture_row(&f, MO_S_TEXT_INTERPOLATED, &f.text_, NULL, 2, operands);
        struct mo_syntax fragments[3];
        const char *spellings[] = {triple ? "\"\"\"\r\n  a\r\n  #{" : "\"a\\n#{", "}: #{",
                                   triple ? "}λ\r\n  \"\"\"" : "}λ\""};
        const enum mo_syntax_token tokens[] = {MO_T_TEXT_HEAD, MO_T_TEXT_MID, MO_T_TEXT_TAIL};
        for (uint32_t i = 0; i < 3; i++) {
            uint32_t start = (uint32_t)(strstr(f.source, spellings[i]) - f.source);
            fragments[i] =
                (struct mo_syntax){.kind = MO_S_TOKEN,
                                   .token = tokens[i],
                                   .text = (const uint8_t *)spellings[i],
                                   .span = {0, start, start + (uint32_t)strlen(spellings[i])}};
        }
        struct mo_syntax *children[] = {&fragments[0], &f.syntax[seven], &fragments[1],
                                        &f.syntax[false_], &fragments[2]};
        f.syntax[text].children = children;
        f.syntax[text].count = 5;
        struct mo_ability_implementation proofs[] = {
            {.ability = MO_SOURCE_TEXT, .kind = MO_ABILITY_NATIVE, .type = &f.int_},
            {.ability = MO_SOURCE_TEXT, .kind = MO_ABILITY_NATIVE, .type = &f.bool_}};
        struct mo_checked_ability_use uses[] = {{.operand = seven, .implementation = &proofs[0]},
                                                {.operand = false_, .implementation = &proofs[1]}};
        f.rows[text].ability_count = 2;
        f.rows[text].abilities = uses;
        uint32_t expected =
            lower_fixture_row(&f, MO_S_TEXT, &f.text_, "\"a\\n7: falseλ\"", 0, NULL);
        uint32_t equal = lower_fixture_op(&f, MO_T_EQ, &f.bool_, text, expected);
        uint32_t yes = lower_fixture_row(&f, MO_S_INT, &f.int_, "19", 0, NULL);
        uint32_t no = lower_fixture_row(&f, MO_S_INT, &f.int_, "1", 0, NULL);
        uint32_t arms[] = {equal, yes, no};
        (void)lower_fixture_row(&f, MO_S_IF, &f.int_, NULL, 3, arms);
        struct mo_core *core =
            checked(&f, triple ? "interpolation-triple" : "interpolation", "returned\n19\n");
        if (core != NULL)
            run(core, 19);
        lower_fixture_free(&f);
    }
}

static void test_derived_text(void) {
    struct lower_fixture f;
    lower_fixture_init(
        &f,
        "module Main\nfn main() : Int\n"
        "if (7, \"a\\n\", '\\n').to_text == \"(7, \\\"a\\\\n\\\", '\\\\n')\": 23 else: 1\nend\n");
    struct mo_source_type char_ = f.int_;
    char_.name = "Char";
    const struct mo_source_type *types[] = {&f.int_, &f.text_, &char_};
    struct mo_source_type tuple = {.kind = MO_SOURCE_TUPLE, .count = 3, .arguments = types};
    uint32_t values[] = {lower_fixture_row(&f, MO_S_INT, &f.int_, "7", 0, NULL),
                         lower_fixture_row(&f, MO_S_TEXT, &f.text_, "\"a\\n\"", 0, NULL),
                         lower_fixture_row(&f, MO_S_CHAR, &char_, "'\\n'", 0, NULL)};
    uint32_t value = lower_fixture_row(&f, MO_S_TUPLE, &tuple, NULL, 3, values);
    struct mo_ability_implementation parts[3];
    const struct mo_ability_implementation *pointers[3];
    for (uint32_t i = 0; i < 3; i++) {
        parts[i] = (struct mo_ability_implementation){
            .ability = MO_SOURCE_TEXT, .kind = MO_ABILITY_NATIVE, .type = types[i]};
        pointers[i] = &parts[i];
    }
    struct mo_ability_implementation proof = {.ability = MO_SOURCE_TEXT,
                                              .kind = MO_ABILITY_DERIVED,
                                              .type = &tuple,
                                              .part_count = 3,
                                              .parts = pointers};
    struct mo_checked_ability_use use = {.operand = value, .implementation = &proof};
    uint32_t text = lower_fixture_row(&f, MO_S_FIELD_ACCESS, &f.text_, NULL, 1, &value);
    f.rows[text].native = "to_text";
    f.rows[text].ability_count = 1;
    f.rows[text].abilities = &use;
    uint32_t expected =
        lower_fixture_row(&f, MO_S_TEXT, &f.text_, "\"(7, \\\"a\\\\n\\\", '\\\\n')\"", 0, NULL);
    uint32_t eq = lower_fixture_op(&f, MO_T_EQ, &f.bool_, text, expected);
    uint32_t yes = lower_fixture_row(&f, MO_S_INT, &f.int_, "23", 0, NULL);
    uint32_t no = lower_fixture_row(&f, MO_S_INT, &f.int_, "1", 0, NULL);
    uint32_t arms[] = {eq, yes, no};
    (void)lower_fixture_row(&f, MO_S_IF, &f.int_, NULL, 3, arms);
    struct mo_core *core = checked(&f, "derived-text", "returned\n23\n");
    if (core != NULL)
        run(core, 23);
    lower_fixture_free(&f);
}

static void test_tuple_order(void) {
    struct lower_fixture f;
    lower_fixture_init(&f, "module Main\nfn main() : Int\n"
                           "if (2, 100) < (3, 5): 31 else: 1\nend\n");
    const struct mo_source_type *parts[] = {&f.int_, &f.int_};
    struct mo_source_type tuple = {.kind = MO_SOURCE_TUPLE, .count = 2, .arguments = parts};
    uint32_t two = lower_fixture_row(&f, MO_S_INT, &f.int_, "2", 0, NULL);
    uint32_t hundred = lower_fixture_row(&f, MO_S_INT, &f.int_, "100", 0, NULL);
    uint32_t left_parts[] = {two, hundred};
    uint32_t left = lower_fixture_row(&f, MO_S_TUPLE, &tuple, NULL, 2, left_parts);
    uint32_t three = lower_fixture_row(&f, MO_S_INT, &f.int_, "3", 0, NULL);
    uint32_t five = lower_fixture_row(&f, MO_S_INT, &f.int_, "5", 0, NULL);
    uint32_t right_parts[] = {three, five};
    uint32_t right = lower_fixture_row(&f, MO_S_TUPLE, &tuple, NULL, 2, right_parts);
    struct mo_ability_implementation integer = {
        .ability = MO_SOURCE_ORDER, .kind = MO_ABILITY_NATIVE, .type = &f.int_};
    const struct mo_ability_implementation *proofs[] = {&integer, &integer};
    struct mo_ability_implementation order = {.ability = MO_SOURCE_ORDER,
                                              .kind = MO_ABILITY_DERIVED,
                                              .type = &tuple,
                                              .part_count = 2,
                                              .parts = proofs};
    struct mo_checked_ability_use use = {.operand = left, .implementation = &order};
    uint32_t less = lower_fixture_op(&f, MO_T_LT, &f.bool_, left, right);
    f.rows[less].ability_count = 1;
    f.rows[less].abilities = &use;
    uint32_t yes = lower_fixture_row(&f, MO_S_INT, &f.int_, "31", 0, NULL);
    uint32_t no = lower_fixture_row(&f, MO_S_INT, &f.int_, "1", 0, NULL);
    uint32_t arms[] = {less, yes, no};
    (void)lower_fixture_row(&f, MO_S_IF, &f.int_, NULL, 3, arms);
    struct mo_core *core = checked(&f, "tuple-order", "returned\n31\n");
    if (core != NULL)
        run(core, 31);
    lower_fixture_free(&f);
}

static void test_float_compare(void) {
    for (uint32_t mask = 1; mask < 4; mask++) {
        struct lower_fixture f;
        lower_fixture_init(&f, "module Main\nfn main() : Int\n"
                               "if (0.0 / 0.0).compare(0.0) == Greater: 37 else: 1\nend\n");
        struct mo_source_type float_ = f.int_, order = f.int_;
        float_.name = "Float";
        order.name = "Ordering";
        uint32_t operands[2];
        for (uint32_t i = 0; i < 2; i++) {
            operands[i] = lower_fixture_row(&f, MO_S_DECIMAL, &float_, "0.0", 0, NULL);
            if ((mask & (1u << i)) != 0) {
                uint32_t zero = lower_fixture_row(&f, MO_S_DECIMAL, &float_, "0.0", 0, NULL);
                operands[i] = lower_fixture_op(&f, MO_T_SLASH, &float_, operands[i], zero);
            }
        }
        struct mo_ability_implementation proof = {
            .ability = MO_SOURCE_ORDER, .kind = MO_ABILITY_NATIVE, .type = &float_};
        struct mo_checked_ability_use use = {.operand = operands[0], .implementation = &proof};
        uint32_t compare = lower_fixture_row(&f, MO_S_CALL, &order, NULL, 2, operands);
        f.rows[compare].native = "compare";
        f.rows[compare].ability_count = 1;
        f.rows[compare].abilities = &use;
        uint32_t want = lower_fixture_row(&f, MO_S_NAME, &order, NULL, 0, NULL);
        f.rows[want].native = mask == 1 ? "Greater" : mask == 2 ? "Less" : "Equal";
        uint32_t equal = lower_fixture_op(&f, MO_T_EQ, &f.bool_, compare, want);
        uint32_t yes = lower_fixture_row(&f, MO_S_INT, &f.int_, "37", 0, NULL);
        uint32_t no = lower_fixture_row(&f, MO_S_INT, &f.int_, "1", 0, NULL);
        uint32_t arms[] = {equal, yes, no};
        (void)lower_fixture_row(&f, MO_S_IF, &f.int_, NULL, 3, arms);
        struct mo_core *core = checked(&f,
                                       mask == 1   ? "nan-compare-number"
                                       : mask == 2 ? "number-compare-nan"
                                                   : "nan-compare-nan",
                                       "returned\n37\n");
        if (core != NULL)
            run(core, 37);
        lower_fixture_free(&f);
    }
}

static void test_choice_order(void) {
    for (uint32_t probe = 0; probe < 4; probe++) {
        struct lower_fixture f;
        lower_fixture_init(&f, "module Main\nfn main() : Int\n"
                               "if Some(2).compare(Some(7)) == Less: 43 else: 1\nend\n");
        const struct mo_source_type *part = &f.int_;
        struct mo_source_type option = {.kind = MO_SOURCE_NAMED,
                                        .name = "Option",
                                        .module = UINT32_MAX,
                                        .count = 1,
                                        .arguments = &part};
        struct mo_source_type order = f.int_;
        order.name = "Ordering";
        uint32_t operands[2];
        for (uint32_t i = 0; i < 2; i++) {
            bool some = probe == 2 || probe == 3 || (probe == 0 ? i == 0 : i == 1);
            uint32_t value = 0;
            if (some)
                value = lower_fixture_row(&f, MO_S_INT, &f.int_, i == 0 || probe == 3 ? "2" : "7",
                                          0, NULL);
            operands[i] = lower_fixture_row(&f, MO_S_CALL, &option, NULL, some, &value);
            f.rows[operands[i]].native = some ? "Some" : "None";
        }
        struct mo_ability_implementation integer = {
            .ability = MO_SOURCE_ORDER, .kind = MO_ABILITY_NATIVE, .type = &f.int_};
        const struct mo_ability_implementation *parts[] = {&integer};
        struct mo_ability_implementation proof = {.ability = MO_SOURCE_ORDER,
                                                  .kind = MO_ABILITY_DERIVED,
                                                  .type = &option,
                                                  .part_count = 1,
                                                  .parts = parts};
        struct mo_checked_ability_use use = {.operand = operands[0], .implementation = &proof};
        uint32_t compare = lower_fixture_row(&f, MO_S_CALL, &order, NULL, 2, operands);
        f.rows[compare].native = "compare";
        f.rows[compare].ability_count = 1;
        f.rows[compare].abilities = &use;
        uint32_t want = lower_fixture_row(&f, MO_S_NAME, &order, NULL, 0, NULL);
        f.rows[want].native = probe == 0 ? "Greater" : probe == 3 ? "Equal" : "Less";
        uint32_t equal = lower_fixture_op(&f, MO_T_EQ, &f.bool_, compare, want);
        uint32_t yes = lower_fixture_row(&f, MO_S_INT, &f.int_, "43", 0, NULL);
        uint32_t no = lower_fixture_row(&f, MO_S_INT, &f.int_, "1", 0, NULL);
        uint32_t arms[] = {equal, yes, no};
        (void)lower_fixture_row(&f, MO_S_IF, &f.int_, NULL, 3, arms);
        const char *names[] = {"some-before-none", "none-before-some", "some-order", "some-equal"};
        struct mo_core *core = checked(&f, names[probe], "returned\n43\n");
        if (core != NULL)
            run(core, 43);
        lower_fixture_free(&f);
    }
}

static void test_list_order(void) {
    for (uint32_t probe = 0; probe < 4; probe++) {
        struct lower_fixture f;
        lower_fixture_init(&f, "module Main\nfn main() : Int\n"
                               "if [2, 7] < [2, 7, 0]: 47 else: 1\nend\n");
        const struct mo_source_type *part = &f.int_;
        struct mo_source_type list = {.kind = MO_SOURCE_NAMED,
                                      .name = "List",
                                      .module = UINT32_MAX,
                                      .count = 1,
                                      .arguments = &part};
        uint32_t operands[2];
        for (uint32_t i = 0; i < 2; i++) {
            uint32_t count = probe == 0 ? (i == 0 ? 2u : 3u) : probe == 1 ? 2u : probe == 2 ? i : 0;
            uint32_t values[3];
            for (uint32_t j = 0; j < count; j++)
                values[j] = lower_fixture_row(&f, MO_S_INT, &f.int_,
                                              probe == 1 && j == 0   ? (i == 0 ? "2" : "7")
                                              : probe == 1 && j == 1 ? (i == 0 ? "7" : "0")
                                              : j == 0               ? "2"
                                              : j == 1               ? "7"
                                                                     : "0",
                                              0, NULL);
            operands[i] = lower_fixture_row(&f, MO_S_LIST, &list, NULL, count, values);
        }
        struct mo_ability_implementation integer = {
            .ability = MO_SOURCE_ORDER, .kind = MO_ABILITY_NATIVE, .type = &f.int_};
        const struct mo_ability_implementation *parts[] = {&integer};
        struct mo_ability_implementation proof = {.ability = MO_SOURCE_ORDER,
                                                  .kind = MO_ABILITY_DERIVED,
                                                  .type = &list,
                                                  .part_count = 1,
                                                  .parts = parts};
        struct mo_checked_ability_use use = {.operand = operands[0], .implementation = &proof};
        uint32_t compare = lower_fixture_op(&f, probe == 3 ? MO_T_LE : MO_T_LT, &f.bool_,
                                            operands[0], operands[1]);
        f.rows[compare].ability_count = 1;
        f.rows[compare].abilities = &use;
        uint32_t yes = lower_fixture_row(&f, MO_S_INT, &f.int_, "47", 0, NULL);
        uint32_t no = lower_fixture_row(&f, MO_S_INT, &f.int_, "1", 0, NULL);
        uint32_t arms[] = {compare, yes, no};
        (void)lower_fixture_row(&f, MO_S_IF, &f.int_, NULL, 3, arms);
        const char *names[] = {"list-prefix", "list-first-part", "list-empty", "list-empty-equal"};
        struct mo_core *core = checked(&f, names[probe], "returned\n47\n");
        if (core != NULL)
            run(core, 47);
        lower_fixture_free(&f);
    }
}

static void test_list_text(void) {
    for (uint32_t count = 0; count <= 2; count += 2) {
        struct lower_fixture f;
        lower_fixture_init(&f, "module Main\nfn main() : Int\n"
                               "if [\"z\", \"a\"].to_text == \"[\\\"z\\\", \\\"a\\\"]\": 53 "
                               "else: 1\n# \"[]\"\nend\n");
        const struct mo_source_type *part = &f.text_;
        struct mo_source_type list = {.kind = MO_SOURCE_NAMED,
                                      .name = "List",
                                      .module = UINT32_MAX,
                                      .count = 1,
                                      .arguments = &part};
        uint32_t values[2];
        for (uint32_t i = 0; i < count; i++)
            values[i] =
                lower_fixture_row(&f, MO_S_TEXT, &f.text_, i == 0 ? "\"z\"" : "\"a\"", 0, NULL);
        uint32_t value = lower_fixture_row(&f, MO_S_LIST, &list, NULL, count, values);
        struct mo_ability_implementation element = {
            .ability = MO_SOURCE_TEXT, .kind = MO_ABILITY_NATIVE, .type = &f.text_};
        const struct mo_ability_implementation *parts[] = {&element};
        struct mo_ability_implementation proof = {.ability = MO_SOURCE_TEXT,
                                                  .kind = MO_ABILITY_DERIVED,
                                                  .type = &list,
                                                  .part_count = 1,
                                                  .parts = parts};
        struct mo_checked_ability_use use = {.operand = value, .implementation = &proof};
        uint32_t text = lower_fixture_row(&f, MO_S_FIELD_ACCESS, &f.text_, NULL, 1, &value);
        f.rows[text].native = "to_text";
        f.rows[text].ability_count = 1;
        f.rows[text].abilities = &use;
        uint32_t expected = lower_fixture_row(
            &f, MO_S_TEXT, &f.text_, count == 0 ? "\"[]\"" : "\"[\\\"z\\\", \\\"a\\\"]\"", 0, NULL);
        uint32_t eq = lower_fixture_op(&f, MO_T_EQ, &f.bool_, text, expected);
        uint32_t yes = lower_fixture_row(&f, MO_S_INT, &f.int_, "53", 0, NULL);
        uint32_t no = lower_fixture_row(&f, MO_S_INT, &f.int_, "1", 0, NULL);
        uint32_t arms[] = {eq, yes, no};
        (void)lower_fixture_row(&f, MO_S_IF, &f.int_, NULL, 3, arms);
        struct mo_core *core =
            checked(&f, count == 0 ? "list-text-empty" : "list-text", "returned\n53\n");
        if (core != NULL)
            run(core, 53);
        lower_fixture_free(&f);
    }
}

static void test_constants(void) {
    struct lower_fixture f;
    lower_fixture_init(&f, "module Main\nb = read_a() + 7\na = 7\n"
                           "fn read_a() : Int\na + 5\nend\nfn main() : Int\nb\nend\n");
    f.program.function_count = f.declarations.count = 2;
    f.symbols[1].name = "read_a";
    uint32_t a_start = (uint32_t)(strstr(f.source, "a = 7") - f.source);
    uint32_t b_start = (uint32_t)(strstr(f.source, "b = read_a() + 7") - f.source);
    struct mo_syntax declarations[2] = {{.kind = MO_S_CONST, .span = {0, a_start, a_start + 5}},
                                        {.kind = MO_S_CONST, .span = {0, b_start, b_start + 16}}};
    struct mo_symbol symbols[2] = {{.name = "a", .declaration = &declarations[0]},
                                   {.name = "b", .declaration = &declarations[1]}};
    struct mo_checked_constant constants[2] = {{.symbol = &symbols[0], .type = &f.int_},
                                               {.symbol = &symbols[1], .type = &f.int_}};
    uint32_t seven = lower_fixture_row(&f, MO_S_INT, &f.int_, "7", 0, NULL);
    constants[0].initializer = f.functions[0].body;
    constants[0].initializer.root = seven;
    uint32_t a = lower_fixture_row(&f, MO_S_NAME, &f.int_, NULL, 0, NULL);
    f.rows[a].symbol = &symbols[0];
    uint32_t five = lower_fixture_row(&f, MO_S_INT, &f.int_, "5", 0, NULL);
    uint32_t sum = lower_fixture_op(&f, MO_T_PLUS, &f.int_, a, five);
    f.functions[1].body = f.functions[0].body;
    f.functions[1].body.root = sum;
    uint32_t callee = lower_fixture_row(&f, MO_S_NAME, &f.function_types[1], NULL, 0, NULL);
    f.rows[callee].symbol = &f.symbols[1];
    uint32_t call = lower_fixture_row(&f, MO_S_CALL, &f.int_, NULL, 1, &callee);
    f.rows[call].symbol = &f.symbols[1];
    seven = lower_fixture_row(&f, MO_S_INT, &f.int_, "7", 0, NULL);
    sum = lower_fixture_op(&f, MO_T_PLUS, &f.int_, call, seven);
    constants[1].initializer = f.functions[0].body;
    constants[1].initializer.root = sum;
    uint32_t b = lower_fixture_row(&f, MO_S_NAME, &f.int_, NULL, 0, NULL);
    f.rows[b].symbol = &symbols[1];
    f.program.constant_count = 2;
    f.program.constants = constants;
    struct mo_core *core = checked(&f, "constants-through-call", "returned\n19\n");
    if (core != NULL)
        run(core, 19);
    lower_fixture_free(&f);
}

static void test_specs(void) {
    for (uint32_t probe = 0; probe < 4; probe++) {
        struct lower_fixture f;
        bool test = probe < 2, pass = probe % 2 == 0;
        const char *source = test   ? pass ? "test \"a condition\"\nassert true\nend\n"
                                           : "test \"a condition\"\nassert false\nend\n"
                             : pass ? "example\ntrue\nend\n"
                                    : "example\nfalse\nend\n";
        char bytes[128];
        int length =
            snprintf(bytes, sizeof bytes, "module Main\nfn main() : Int\n7\nend\n%s", source);
        MO_ASSERT(length > 0 && (size_t)length < sizeof bytes);
        lower_fixture_init(&f, bytes);
        (void)lower_fixture_row(&f, MO_S_INT, &f.int_, "7", 0, NULL);
        struct mo_checked_body main = f.functions[0].body;
        uint32_t condition = lower_fixture_row(&f, pass ? MO_S_TRUE : MO_S_FALSE, &f.bool_,
                                               pass ? "true" : "false", 0, NULL);
        if (test)
            (void)lower_fixture_row(&f, MO_S_ASSERT, &f.unit_, NULL, 1, &condition);
        const char *line = pass ? test ? "assert true" : "true" : test ? "assert false" : "false";
        uint32_t start = (uint32_t)(strstr(bytes, line) - bytes);
        struct mo_spec_text text = {.bytes = (const uint8_t *)line,
                                    .length = (uint32_t)strlen(line),
                                    .span = {0, start, start + (uint32_t)strlen(line)}};
        struct mo_checked_spec spec = {.kind = test ? MO_SPEC_TEST : MO_SPEC_EXAMPLE,
                                       .syntax = &f.syntax[condition],
                                       .number = 1,
                                       .subject = test ? UINT32_MAX : 0,
                                       .text = text,
                                       .wording = (const uint8_t *)"a condition",
                                       .wording_length = 11,
                                       .body = f.functions[0].body};
        f.program.spec_count = 1;
        f.program.specs = &spec;
        f.functions[0].body = main;
        const char *names[] = {"test-pass", "test-fail", "example-pass", "example-fail"};
        const char *expected[] = {"returned\nUnit\n{\"test\":\"Main.test.1\"}\n",
                                  "failed\nassertion failed\n{\"test\":\"Main.test.1\"}\n",
                                  "returned\nUnit\n{\"test\":\"Main.main.example.1\"}\n",
                                  "failed\nexample failed\n{\"test\":\"Main.main.example.1\"}\n"};
        struct mo_core *core = checked(&f, names[probe], expected[probe]);
        if (core != NULL)
            run_outcome(core, test ? "Main.test.1" : "Main.main.example.1",
                        pass ? MO_RETURNED : MO_FAILED, 0,
                        pass   ? NULL
                        : test ? "assertion failed"
                               : "example failed");
        lower_fixture_free(&f);
    }
}

static void test_structural_failures(void) {
    struct lower_fixture f;
    lower_fixture_init(&f, "module Main\nfn main() : Int\n7\nend\n"
                           "test \"snapshot operands\"\nvar n = 1\n"
                           "assert n == (n += 1; n)\nend\n");
    (void)lower_fixture_row(&f, MO_S_INT, &f.int_, "7", 0, NULL);
    struct mo_checked_body main = f.functions[0].body;
    f.bindings[0] = (struct mo_checked_binding){.name = "n", .type = &f.int_, .variable = true};
    f.functions[0].body.binding_count = 1;
    uint32_t one = lower_fixture_row(&f, MO_S_INT, &f.int_, "1", 0, NULL);
    uint32_t bind = lower_fixture_bind(&f, 0, one, false);
    uint32_t left = lower_fixture_read(&f, 0);
    uint32_t read = lower_fixture_read(&f, 0);
    uint32_t sum = lower_fixture_op(&f, MO_T_PLUS, &f.int_, read, one);
    uint32_t write = lower_fixture_bind(&f, 0, sum, true);
    uint32_t right = lower_fixture_read(&f, 0);
    uint32_t sequence[] = {write, right};
    uint32_t operand = lower_fixture_row(&f, MO_S_SEQUENCE, &f.int_, NULL, 2, sequence);
    uint32_t eq = lower_fixture_op(&f, MO_T_EQ, &f.bool_, left, operand);
    uint32_t assertion = lower_fixture_row(&f, MO_S_ASSERT, &f.unit_, NULL, 1, &eq);
    uint32_t statements[] = {bind, assertion};
    (void)lower_fixture_row(&f, MO_S_SEQUENCE, &f.unit_, NULL, 2, statements);
    struct mo_checked_spec spec = {.kind = MO_SPEC_TEST,
                                   .syntax = &f.syntax[assertion],
                                   .number = 1,
                                   .subject = UINT32_MAX,
                                   .wording = (const uint8_t *)"snapshot operands",
                                   .wording_length = 17,
                                   .body = f.functions[0].body};
    f.program.spec_count = 1;
    f.program.specs = &spec;
    f.functions[0].body = main;
    struct mo_core *core = checked(&f, "structural-snapshots",
                                   "failed\nleft 1:1,right 1:2,\n{\"test\":\"Main.test.1\"}\n");
    if (core != NULL)
        run_outcome(core, "Main.test.1", MO_FAILED, 0, "left 1:1,right 1:2,");
    lower_fixture_free(&f);
}

static void test_structural_patterns(void) {
    for (uint32_t probe = 0; probe < 4; probe++) {
        struct lower_fixture f;
        lower_fixture_init(&f, "module Main\nfn main() : Int\n7\nend\n"
                               "test \"pattern input\"\nassert None is Some(n)\n"
                               "assert Some(7) is Some(n)\nassert n == 7\n"
                               "assert (Some(7), Some(7)) is (Some(n), Some(9))\nend\n");
        uint32_t seven = lower_fixture_row(&f, MO_S_INT, &f.int_, "7", 0, NULL);
        struct mo_checked_body main = f.functions[0].body;
        const struct mo_source_type *argument = &f.int_;
        struct mo_source_type option = {.kind = MO_SOURCE_NAMED,
                                        .name = "Option",
                                        .module = UINT32_MAX,
                                        .count = 1,
                                        .arguments = &argument};
        const struct mo_source_type *fields[] = {&option, &option};
        struct mo_source_type tuple = {.kind = MO_SOURCE_TUPLE, .count = 2, .arguments = fields};
        f.bindings[0] = (struct mo_checked_binding){.name = "n", .type = &f.int_};
        f.functions[0].body.binding_count = 1;
        uint32_t value =
            lower_fixture_row(&f, MO_S_CALL, &option, NULL, probe == 0 ? 0 : 1, &seven);
        f.rows[value].native = probe == 0 ? "None" : "Some";
        uint32_t nine = lower_fixture_row(&f, MO_S_INT, &f.int_, "9", 0, NULL);
        struct mo_checked_pattern bind = {.kind = MO_PATTERN_BIND,
                                          .syntax = &f.syntax[value],
                                          .type = &f.int_,
                                          .binding = &f.bindings[0]};
        struct mo_checked_pattern literal = {
            .kind = MO_PATTERN_LITERAL, .syntax = &f.syntax[nine], .type = &f.int_};
        struct mo_checked_pattern_part capture = {0, &bind}, constant = {0, &literal};
        struct mo_checked_pattern some = {.kind = MO_PATTERN_CONSTRUCTOR,
                                          .syntax = &f.syntax[value],
                                          .type = &option,
                                          .constructor.builtin = "Some",
                                          .part_count = 1,
                                          .parts = &capture};
        struct mo_checked_pattern some_nine = some;
        some_nine.parts = &constant;
        struct mo_checked_pattern_part parts[] = {{0, &some}, {1, &some_nine}};
        struct mo_checked_pattern pair = {.kind = MO_PATTERN_TUPLE,
                                          .syntax = &f.syntax[value],
                                          .type = &tuple,
                                          .part_count = 2,
                                          .parts = parts};
        if (probe == 3) {
            uint32_t values[] = {value, value};
            value = lower_fixture_row(&f, MO_S_TUPLE, &tuple, NULL, 2, values);
        }
        uint32_t is = lower_fixture_row(&f, MO_S_IS, &f.bool_, NULL, 1, &value);
        f.rows[is].pattern = probe == 3 ? &pair : probe == 2 ? &some_nine : &some;
        bool example = probe == 2, pass = probe == 1;
        if (!example) {
            uint32_t assertion = lower_fixture_row(&f, MO_S_ASSERT, &f.unit_, NULL, 1, &is);
            if (pass) {
                uint32_t n = lower_fixture_read(&f, 0);
                uint32_t eq = lower_fixture_op(&f, MO_T_EQ, &f.bool_, n, seven);
                uint32_t second = lower_fixture_row(&f, MO_S_ASSERT, &f.unit_, NULL, 1, &eq);
                uint32_t statements[] = {assertion, second};
                (void)lower_fixture_row(&f, MO_S_SEQUENCE, &f.unit_, NULL, 2, statements);
            }
        }
        struct mo_checked_spec spec = {.kind = example ? MO_SPEC_EXAMPLE : MO_SPEC_TEST,
                                       .syntax = &f.syntax[is],
                                       .number = 1,
                                       .subject = example ? 0 : UINT32_MAX,
                                       .wording = (const uint8_t *)"pattern input",
                                       .wording_length = 13,
                                       .body = f.functions[0].body};
        f.program.spec_count = 1;
        f.program.specs = &spec;
        f.functions[0].body = main;
        const char *names[] = {"structural-is-none", "structural-is-scope", "structural-is-literal",
                               "structural-is-nested"};
        const char *expected[] = {
            "failed\ninput 4:None,\n{\"test\":\"Main.test.1\"}\n",
            "returned\nUnit\n{\"test\":\"Main.test.1\"}\n",
            "failed\ninput 7:Some(7),\n{\"test\":\"Main.main.example.1\"}\n",
            "failed\ninput 18:(Some(7), Some(7)),\n{\"test\":\"Main.test.1\"}\n"};
        const char *details[] = {"input 4:None,", NULL, "input 7:Some(7),",
                                 "input 18:(Some(7), Some(7)),"};
        struct mo_core *core = checked(&f, names[probe], expected[probe]);
        if (core != NULL)
            run_outcome(core, example ? "Main.main.example.1" : "Main.test.1",
                        pass ? MO_RETURNED : MO_FAILED, 0, details[probe]);
        lower_fixture_free(&f);
    }
}

static void test_structural_binary_text(void) {
    for (uint32_t probe = 0; probe < 2; probe++) {
        struct lower_fixture f;
        lower_fixture_init(&f, "module Main\nfn main() : Int\n7\nend\n"
                               "test \"binary detail\"\n"
                               "assert \"é\\nright 0:,\\u{0}\" == \"\"\n"
                               "assert \"é\".bytes == \"\".bytes\nend\n");
        (void)lower_fixture_row(&f, MO_S_INT, &f.int_, "7", 0, NULL);
        struct mo_checked_body main = f.functions[0].body;
        struct mo_source_type bytes = f.text_;
        bytes.name = "Bytes";
        uint32_t left = lower_fixture_row(
            &f, MO_S_TEXT, &f.text_, probe == 0 ? "\"é\\nright 0:,\\u{0}\"" : "\"é\"", 0, NULL);
        uint32_t right = lower_fixture_row(&f, MO_S_TEXT, &f.text_, "\"\"", 0, NULL);
        if (probe != 0) {
            left = lower_fixture_row(&f, MO_S_FIELD_ACCESS, &bytes, NULL, 1, &left);
            f.rows[left].native = "Text.bytes";
            right = lower_fixture_row(&f, MO_S_FIELD_ACCESS, &bytes, NULL, 1, &right);
            f.rows[right].native = "Text.bytes";
        }
        uint32_t eq = lower_fixture_op(&f, MO_T_EQ, &f.bool_, left, right);
        struct mo_ability_implementation equal = {.kind = MO_ABILITY_NATIVE,
                                                  .ability = MO_SOURCE_EQUAL,
                                                  .type = probe == 0 ? &f.text_ : &bytes};
        struct mo_checked_ability_use use = {.operand = left, .implementation = &equal};
        f.rows[eq].ability_count = 1;
        f.rows[eq].abilities = &use;
        (void)lower_fixture_row(&f, MO_S_ASSERT, &f.unit_, NULL, 1, &eq);
        struct mo_checked_spec spec = {.kind = MO_SPEC_TEST,
                                       .syntax = &f.syntax[eq],
                                       .number = 1,
                                       .subject = UINT32_MAX,
                                       .wording = (const uint8_t *)"binary detail",
                                       .wording_length = 13,
                                       .body = f.functions[0].body};
        f.program.spec_count = 1;
        f.program.specs = &spec;
        f.functions[0].body = main;
        const char *expected[] = {
            "failed\n-\n{\"test\":\"Main.test.1\","
            "\"detail\":\"left 13:é\\nright 0:,\\u0000,right 0:,\"}\n",
            "failed\nleft 15:bytes[195, 169],right 7:bytes[],\n{\"test\":\"Main.test.1\"}\n"};
        const char detail[] = "left 13:é\nright 0:,\0,right 0:,";
        struct mo_core *core =
            checked(&f, probe == 0 ? "structural-raw-text" : "structural-bytes", expected[probe]);
        if (core != NULL) {
            if (probe == 0)
                run_outcome_bytes(core, "Main.test.1", MO_FAILED, 0, detail, sizeof detail - 1);
            else
                run_outcome(core, "Main.test.1", MO_FAILED, 0,
                            "left 15:bytes[195, 169],right 7:bytes[],");
        }
        lower_fixture_free(&f);
    }
}

static void test_structural_lazy_conditions(void) {
    const enum mo_syntax_token operators[] = {MO_T_AND, MO_T_OR, MO_T_IMPLIES,
                                              MO_T_AND, MO_T_OR, MO_T_IMPLIES};
    for (uint32_t probe = 0; probe < 6; probe++) {
        struct lower_fixture f;
        lower_fixture_init(&f, "module Main\nfn main() : Int\n7\nend\n"
                               "test \"lazy details\"\nassert (1 == 2 and 7 == 9)\n"
                               "assert (1 == 2 or 7 == 7)\n"
                               "assert (1 == 2 implies stop \"unreachable\")\nend\n");
        uint32_t seven = lower_fixture_row(&f, MO_S_INT, &f.int_, "7", 0, NULL);
        struct mo_checked_body main = f.functions[0].body;
        uint32_t one = lower_fixture_row(&f, MO_S_INT, &f.int_, "1", 0, NULL);
        uint32_t two = lower_fixture_row(&f, MO_S_INT, &f.int_, "2", 0, NULL);
        uint32_t nine = lower_fixture_row(&f, MO_S_INT, &f.int_, "9", 0, NULL);
        bool left_true = probe == 3 || probe == 5;
        uint32_t left = lower_fixture_op(&f, MO_T_EQ, &f.bool_, one, left_true ? one : two);
        uint32_t right = lower_fixture_op(&f, MO_T_EQ, &f.bool_, seven, probe == 1 ? seven : nine);
        struct mo_syntax *children[1];
        if (probe == 0 || probe == 2) {
            uint32_t message =
                lower_fixture_row(&f, MO_S_TEXT, &f.text_, "\"unreachable\"", 0, NULL);
            right = lower_fixture_row(&f, MO_S_STOP, &f.bool_, NULL, 0, NULL);
            children[0] = &f.syntax[message];
            f.syntax[right].children = children;
        }
        uint32_t condition = lower_fixture_op(&f, operators[probe], &f.bool_, left, right);
        f.rows[condition].or_kind =
            operators[probe] == MO_T_OR ? MO_CHECK_OR_BOOL : MO_CHECK_OR_ABSENT;
        condition = lower_fixture_row(&f, MO_S_GROUP, &f.bool_, NULL, 1, &condition);
        bool example = probe % 2 != 0, pass = probe == 1 || probe == 2;
        if (!example)
            (void)lower_fixture_row(&f, MO_S_ASSERT, &f.unit_, NULL, 1, &condition);
        struct mo_checked_spec spec = {.kind = example ? MO_SPEC_EXAMPLE : MO_SPEC_TEST,
                                       .syntax = &f.syntax[condition],
                                       .number = 1,
                                       .subject = example ? 0 : UINT32_MAX,
                                       .wording = (const uint8_t *)"lazy details",
                                       .wording_length = 12,
                                       .body = f.functions[0].body};
        f.program.spec_count = 1;
        f.program.specs = &spec;
        f.functions[0].body = main;
        const char *names[] = {"structural-and-left",     "structural-or-pass",
                               "structural-implies-skip", "structural-and-right",
                               "structural-or-right",     "structural-implies-right"};
        const char *expected[] = {
            "failed\nleft 1:1,right 1:2,\n{\"test\":\"Main.test.1\"}\n",
            "returned\nUnit\n{\"test\":\"Main.main.example.1\"}\n",
            "returned\nUnit\n{\"test\":\"Main.test.1\"}\n",
            "failed\nleft 1:7,right 1:9,\n{\"test\":\"Main.main.example.1\"}\n",
            "failed\nleft 1:7,right 1:9,\n{\"test\":\"Main.test.1\"}\n",
            "failed\nleft 1:7,right 1:9,\n{\"test\":\"Main.main.example.1\"}\n"};
        struct mo_core *core = checked(&f, names[probe], expected[probe]);
        if (core != NULL)
            run_outcome(core, example ? "Main.main.example.1" : "Main.test.1",
                        pass ? MO_RETURNED : MO_FAILED, 0,
                        pass         ? NULL
                        : probe == 0 ? "left 1:1,right 1:2,"
                                     : "left 1:7,right 1:9,");
        lower_fixture_free(&f);
    }
}

static void test_structural_customizers(void) {
    for (uint32_t probe = 0; probe < 4; probe++) {
        struct lower_fixture f;
        lower_fixture_init(&f, "module Main\nstruct Box\nvalue: Int\nend\n"
                               "fn main() : Int\n7\nend\n"
                               "fn to_text(box: Box) : Text\nstop \"customizer ran\"\nend\n"
                               "test \"no diagnostic callback\"\n"
                               "assert Box(7) == Box(9)\nassert Box(7).to_text == \"\"\nend\n");
        uint32_t seven = lower_fixture_row(&f, MO_S_INT, &f.int_, "7", 0, NULL);
        struct mo_checked_body main = f.functions[0].body;
        struct mo_syntax declaration = {.kind = MO_S_STRUCT, .span = f.function_syntax[0].span};
        struct mo_symbol symbol = {.name = "Box", .declaration = &declaration};
        struct mo_source_type box = {
            .kind = MO_SOURCE_NAMED, .name = "Box", .declaration = &symbol};
        struct mo_syntax field_name = {.text = (const uint8_t *)"value", .span = {0, 23, 28}};
        struct mo_field_signature field = {.name = &field_name, .type = &f.int_};
        const struct mo_field_signature *fields[] = {&field};
        struct mo_data_signature data = {
            .symbol = &symbol, .type = &box, .count = 1, .by_index = fields};
        struct mo_signatures signatures = {.data_count = 1, .data = &data};
        f.program.signatures = &signatures;
        f.program.function_count = f.declarations.count = 2;
        f.symbols[1].name = "to_text";
        f.function_types[1].count = 2;
        f.signature_parts[1][0] = &box;
        f.signature_parts[1][1] = &f.text_;
        f.bindings[0] = (struct mo_checked_binding){.name = "box", .type = &box};
        const struct mo_checked_binding *parameter = &f.bindings[0];
        f.functions[0].body.binding_count = 1;
        f.functions[1].parameter_count = 1;
        f.functions[1].parameters = &parameter;
        uint32_t message =
            lower_fixture_row(&f, MO_S_TEXT, &f.text_, "\"customizer ran\"", 0, NULL);
        uint32_t stop = lower_fixture_row(&f, MO_S_STOP, &f.text_, NULL, 0, NULL);
        struct mo_syntax *children[] = {&f.syntax[message]};
        f.syntax[stop].children = children;
        f.functions[1].body = f.functions[0].body;
        uint32_t left = lower_fixture_row(&f, MO_S_CALL, &box, NULL, 1, &seven);
        f.rows[left].symbol = &symbol;
        uint32_t nine = lower_fixture_row(&f, MO_S_INT, &f.int_, "9", 0, NULL);
        uint32_t right = lower_fixture_row(&f, MO_S_CALL, &box, NULL, 1, &nine);
        f.rows[right].symbol = &symbol;
        struct mo_ability_implementation proof = {.ability = MO_SOURCE_TEXT,
                                                  .kind = MO_ABILITY_FUNCTION,
                                                  .type = &box,
                                                  .function = &f.symbols[1]};
        struct mo_checked_ability_use use = {.operand = left, .implementation = &proof};
        if (probe == 2) {
            left = lower_fixture_row(&f, MO_S_FIELD_ACCESS, &f.text_, NULL, 1, &left);
            f.rows[left].native = "to_text";
            f.rows[left].ability_count = 1;
            f.rows[left].abilities = &use;
            right = lower_fixture_row(&f, MO_S_TEXT, &f.text_, "\"\"", 0, NULL);
        }
        uint32_t condition =
            lower_fixture_op(&f, MO_T_EQ, &f.bool_, left, probe == 1 ? left : right);
        uint32_t assertion = lower_fixture_row(&f, MO_S_ASSERT, &f.unit_, NULL, 1, &condition);
        struct mo_checked_spec spec = {.kind = MO_SPEC_TEST,
                                       .syntax = &f.syntax[assertion],
                                       .number = 1,
                                       .subject = UINT32_MAX,
                                       .wording = (const uint8_t *)"no diagnostic callback",
                                       .wording_length = 22,
                                       .body = f.functions[0].body};
        if (probe == 3) {
            uint32_t statements[] = {assertion, seven};
            (void)lower_fixture_row(&f, MO_S_SEQUENCE, &f.int_, NULL, 2, statements);
        } else {
            f.program.spec_count = 1;
            f.program.specs = &spec;
            f.functions[0].body = main;
        }
        const char *names[] = {"structural-custom-fail", "structural-custom-pass",
                               "structural-custom-control", "structural-ordinary-stop"};
        const char *expected[] = {
            "failed\nleft 13:Box(value: 7),right 13:Box(value: 9),\n{\"test\":\"Main.test.1\"}\n",
            "returned\nUnit\n{\"test\":\"Main.test.1\"}\n",
            "stopped\ncustomizer ran\n{\"test\":\"Main.test.1\"}\n", "stopped\nassertion failed\n"};
        struct mo_core *core = checked(&f, names[probe], expected[probe]);
        if (core != NULL)
            run_outcome(core, probe == 3 ? NULL : "Main.test.1",
                        probe == 0   ? MO_FAILED
                        : probe == 1 ? MO_RETURNED
                                     : MO_STOPPED,
                        0, probe == 0 ? "left 13:Box(value: 7),right 13:Box(value: 9)," : NULL);
        lower_fixture_free(&f);
    }
}

static void test_promises(void) {
    const char *numbers[] = {"5", "2", "20"};
    const int64_t answers[] = {0, 1, 2};
    for (uint32_t probe = 0; probe < 3; probe++) {
        struct lower_fixture f;
        lower_fixture_init(
            &f, "module Main\nfn main() : Int\n7\nend\nfn identity(input: Int) : Int\ninput\n"
                "promise \"bounded result\"\nresult > input\nresult < 8\n"
                "allows result: 5, input: 2\nforbids result: 2, input: 2\n"
                "forbids result: 20, input: 2\nend\nend\n");
        f.program.function_count = f.declarations.count = 2;
        (void)lower_fixture_row(&f, MO_S_INT, &f.int_, "7", 0, NULL);
        struct mo_checked_body main = f.functions[0].body;
        f.bindings[0] = (struct mo_checked_binding){.name = "input", .type = &f.int_};
        f.bindings[1] = (struct mo_checked_binding){.name = "result", .type = &f.int_};
        const struct mo_checked_binding *parameters[] = {&f.bindings[0], &f.bindings[1]};
        f.functions[1].parameter_count = 1;
        f.functions[1].parameters = parameters;
        f.function_types[1].count = 2;
        f.signature_parts[1][0] = f.signature_parts[1][1] = &f.int_;
        f.functions[0].body.binding_count = 1;
        (void)lower_fixture_read(&f, 0);
        f.functions[1].body = f.functions[0].body;
        f.functions[0].body.binding_count = 2;
        uint32_t result = lower_fixture_read(&f, 1), input = lower_fixture_read(&f, 0);
        uint32_t first = lower_fixture_op(&f, MO_T_GT, &f.bool_, result, input);
        result = lower_fixture_read(&f, 1);
        uint32_t eight = lower_fixture_row(&f, MO_S_INT, &f.int_, "8", 0, NULL);
        uint32_t last = lower_fixture_op(&f, MO_T_LT, &f.bool_, result, eight);
        struct mo_checked_condition conditions[2] = {{.expression = first}, {.expression = last}};
        const char *lines[] = {"result > input", "result < 8"};
        for (uint32_t i = 0; i < 2; i++) {
            uint32_t start = (uint32_t)(strstr(f.source, lines[i]) - f.source);
            uint32_t length = (uint32_t)strlen(lines[i]);
            conditions[i].text = (struct mo_spec_text){.bytes = (const uint8_t *)lines[i],
                                                       .length = length,
                                                       .span = {0, start, start + length}};
        }
        struct mo_checked_spec specs[2] = {{.kind = MO_SPEC_PROMISE,
                                            .syntax = &f.syntax[first],
                                            .subject = 1,
                                            .number = 1,
                                            .wording = (const uint8_t *)"bounded result",
                                            .wording_length = 14,
                                            .parameter_count = 2,
                                            .parameters = parameters,
                                            .body = f.functions[0].body,
                                            .condition_count = 2,
                                            .conditions = conditions},
                                           {.kind = MO_SPEC_ANCHOR,
                                            .subject = 1,
                                            .promise = 0,
                                            .number = probe == 2 ? 2 : 1,
                                            .allows = probe == 0}};
        uint32_t value = lower_fixture_row(&f, MO_S_INT, &f.int_, numbers[probe], 0, NULL);
        input = lower_fixture_row(&f, MO_S_INT, &f.int_, "2", 0, NULL);
        uint32_t values[] = {input, value};
        specs[1].syntax = &f.syntax[value];
        specs[1].body = f.functions[0].body;
        specs[1].body.root = UINT32_MAX;
        specs[1].value_count = 2;
        specs[1].values = values;
        specs[1].text = (struct mo_spec_text){.bytes = (const uint8_t *)numbers[probe],
                                              .length = strlen(numbers[probe]),
                                              .span = f.syntax[value].span};
        f.program.spec_count = 2;
        f.program.specs = specs;
        f.functions[0].body = main;
        const char *names[] = {"promise-allows", "promise-first", "promise-second"};
        const char *calls[] = {"Main.identity.promise.1.allows.1",
                               "Main.identity.promise.1.forbids.1",
                               "Main.identity.promise.1.forbids.2"};
        char expected[128];
        int length = snprintf(expected, sizeof expected, "returned\n%lld\n{\"call\":\"%s\"}\n",
                              (long long)answers[probe], calls[probe]);
        MO_ASSERT(length > 0 && (size_t)length < sizeof expected);
        struct mo_core *core = checked(&f, names[probe], expected);
        if (core != NULL)
            run_outcome(core, calls[probe], MO_RETURNED, answers[probe], NULL);
        lower_fixture_free(&f);
    }
}

static void test_record_text(void) {
    struct lower_fixture f;
    lower_fixture_init(&f, "module Main\nstruct Point\nx: Int\nlabel: Text\nend\n"
                           "fn main() : Int\n"
                           "if Point(label: \"v\", x: 7).to_text == \"Point(x: 7, label: "
                           "\\\"v\\\")\": 29 else: 1\nend\n");
    struct mo_syntax declaration = {.kind = MO_S_STRUCT, .span = f.function_syntax[0].span};
    struct mo_symbol symbol = {.name = "Point", .declaration = &declaration};
    struct mo_source_type type = {.kind = MO_SOURCE_NAMED, .name = "Point", .declaration = &symbol};
    const char *field_names[] = {"x", "label"};
    struct mo_syntax names[2];
    struct mo_field_signature fields[2];
    const struct mo_field_signature *indices[2];
    const struct mo_source_type *types[] = {&f.int_, &f.text_};
    struct mo_ability_implementation parts[2];
    const struct mo_ability_implementation *proofs[2];
    for (uint32_t i = 0; i < 2; i++) {
        const char *at = strstr(f.source, field_names[i]);
        names[i] = (struct mo_syntax){
            .text = (const uint8_t *)field_names[i],
            .span = {0, (uint32_t)(at - f.source),
                     (uint32_t)(at - f.source) + (uint32_t)strlen(field_names[i])}};
        fields[i] = (struct mo_field_signature){.name = &names[i], .type = types[i], .index = i};
        indices[i] = &fields[i];
        parts[i] = (struct mo_ability_implementation){
            .ability = MO_SOURCE_TEXT, .kind = MO_ABILITY_NATIVE, .type = types[i]};
        proofs[i] = &parts[i];
    }
    struct mo_data_signature data = {
        .symbol = &symbol, .type = &type, .count = 2, .by_index = indices};
    struct mo_signatures signatures = {.data_count = 1, .data = &data};
    f.program.signatures = &signatures;
    uint32_t label = lower_fixture_row(&f, MO_S_TEXT, &f.text_, "\"v\"", 0, NULL);
    uint32_t label_arg = lower_fixture_row(&f, MO_S_NAMED_ARG, &f.text_, NULL, 1, &label);
    f.rows[label_arg].field = 1;
    uint32_t x = lower_fixture_row(&f, MO_S_INT, &f.int_, "7", 0, NULL);
    uint32_t x_arg = lower_fixture_row(&f, MO_S_NAMED_ARG, &f.int_, NULL, 1, &x);
    f.rows[x_arg].field = 0;
    uint32_t payload[] = {label_arg, x_arg};
    uint32_t value = lower_fixture_row(&f, MO_S_CALL, &type, NULL, 2, payload);
    f.rows[value].symbol = &symbol;
    struct mo_ability_implementation proof = {.ability = MO_SOURCE_TEXT,
                                              .kind = MO_ABILITY_DERIVED,
                                              .type = &type,
                                              .part_count = 2,
                                              .parts = proofs};
    struct mo_checked_ability_use use = {.operand = value, .implementation = &proof};
    uint32_t text = lower_fixture_row(&f, MO_S_FIELD_ACCESS, &f.text_, NULL, 1, &value);
    f.rows[text].native = "to_text";
    f.rows[text].ability_count = 1;
    f.rows[text].abilities = &use;
    uint32_t want =
        lower_fixture_row(&f, MO_S_TEXT, &f.text_, "\"Point(x: 7, label: \\\"v\\\")\"", 0, NULL);
    uint32_t equal = lower_fixture_op(&f, MO_T_EQ, &f.bool_, text, want);
    uint32_t yes = lower_fixture_row(&f, MO_S_INT, &f.int_, "29", 0, NULL);
    uint32_t no = lower_fixture_row(&f, MO_S_INT, &f.int_, "1", 0, NULL);
    uint32_t arms[] = {equal, yes, no};
    (void)lower_fixture_row(&f, MO_S_IF, &f.int_, NULL, 3, arms);
    struct mo_core *core = checked(&f, "record-text", "returned\n29\n");
    if (core != NULL)
        run(core, 29);
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
    const char *names[] = {"unknown"};
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

static void test_abstract_ability_refusal(void) {
    struct lower_fixture f;
    lower_fixture_init(&f,
                       "module Main\nfn double(value: T) : T where T: Number\nvalue + value\nend\n"
                       "fn main() : Int\ndouble(17)\nend\n");
    f.program.function_count = f.declarations.count = 2;
    f.symbols[1].name = "double";
    struct mo_source_type parameter = {
        .kind = MO_SOURCE_PARAMETER, .name = "T", .declaration = &f.symbols[1], .parameters = 1};
    f.function_types[1].count = 2;
    f.function_types[1].parameters = 1;
    f.signature_parts[1][0] = f.signature_parts[1][1] = &parameter;
    f.bindings[0] = (struct mo_checked_binding){.name = "value", .type = &parameter};
    const struct mo_checked_binding *parameters[] = {&f.bindings[0]};
    f.functions[1].parameter_count = 1;
    f.functions[1].parameters = parameters;
    f.functions[0].body.binding_count = 1;
    uint32_t left = lower_fixture_read(&f, 0), right = lower_fixture_read(&f, 0);
    uint32_t sum = lower_fixture_op(&f, MO_T_PLUS, &parameter, left, right);
    struct mo_ability_implementation abstract = {
        .ability = MO_SOURCE_NUMBER, .kind = MO_ABILITY_PARAMETER, .type = &parameter};
    struct mo_checked_ability_use requirement = {.operand = left, .implementation = &abstract};
    f.rows[sum].ability_count = 1;
    f.rows[sum].abilities = &requirement;
    f.functions[1].body = f.functions[0].body;
    struct mo_type_argument argument = {&parameter, &f.int_};
    uint32_t callee = lower_fixture_row(&f, MO_S_NAME, &f.function_types[1], NULL, 0, NULL);
    f.rows[callee].symbol = &f.symbols[1];
    uint32_t value = lower_fixture_row(&f, MO_S_INT, &f.int_, "17", 0, NULL);
    uint32_t operands[] = {callee, value};
    uint32_t call = lower_fixture_row(&f, MO_S_CALL, &f.int_, NULL, 2, operands);
    f.rows[call].symbol = &f.symbols[1];
    f.rows[call].arguments = (struct mo_type_arguments){1, &argument};
    CHECK(mo_lower(&f.arena, &f.program, &f.sources, &f.diags) == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_LOWER_UNFINISHED);
    lower_fixture_free(&f);
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
    /* Source type walks retain their bound even when Core permits longer paths. */
    const uint32_t depths[] = {MO_LIMIT_SOURCE_TYPE_DEPTH - 16, MO_LIMIT_SOURCE_TYPE_DEPTH - 3,
                               MO_LIMIT_SOURCE_TYPE_DEPTH + 1};
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
        struct mo_source_type types[MO_LIMIT_SOURCE_TYPE_DEPTH + 3];
        const struct mo_source_type *parts[MO_LIMIT_SOURCE_TYPE_DEPTH + 2];
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
    struct mo_module_set set = {
        .count = 1, .modules = &module, .names = &name, .sources = &sources};
    struct mo_program *program = module == NULL ? NULL : mo_check(&input, &set, diags);
    MO_ASSERT(program != NULL);
    struct mo_core *core = mo_lower(output, program, &sources, diags);
    mo_sources_free(&sources);
    mo_arena_free(&input);
    if (core != NULL)
        MO_ASSERT(mo_core_check(core, diags));
    return core;
}

static void test_source_specs(void *unused) {
    (void)unused;
    struct mo_arena output;
    struct mo_diags diags;
    struct mo_buffer source;
    mo_arena_init(&output, MO_LIMIT_LOWER_WORK);
    MO_ASSERT(mo_diags_init(&diags));
    mo_buffer_init(&source, MO_LIMIT_FILE_SIZE);
    MO_ASSERT(mo_buffer_append_text(
        &source, "module Main\nfn twice(value: T) : T where T: Number, T: Equal\n"
                 "promise \"returns twice its input\"\nresult == value + value\n"
                 "allows result: 6, value: 3\nallows value: 4, result: 8\n"
                 "forbids value: 1.25, result: 2.0\nend\n"
                 "value + value\nend\nfn main() : Int\n17\nend\n"));
    struct mo_core *core = source_core(&output, &source, &diags);
    CHECK(core != NULL && !mo_diags_any(&diags));
    if (core != NULL) {
        run_outcome(core, "Main::Main.twice(Int).promise.1.allows.1", MO_RETURNED, 0, NULL);
        run_outcome(core, "Main::Main.twice(Int).promise.1.allows.2", MO_RETURNED, 0, NULL);
        run_outcome(core, "Main::Main.twice(Decimal).promise.1.forbids.1", MO_RETURNED, 1, NULL);
        uint32_t last = 0, promises = 0;
        const struct mo_core_module_meta *module = &core->metadata.modules.items[0];
        for (size_t i = 0; i < module->funs.count; i++) {
            uint32_t index = module->funs.items[i];
            if (core->funs.items[index].kind == MO_CORE_FUNCTION)
                continue;
            const struct mo_core_fun_meta *meta = &core->metadata.funs.items[index];
            CHECK(meta->span.some && meta->span.value.start >= last);
            last = meta->span.value.start;
            if (core->funs.items[index].kind == MO_CORE_PROMISE) {
                promises++;
                CHECK(meta->lines.count == 1 && meta->lines.items[0].text.length == 23 &&
                      memcmp(meta->lines.items[0].text.bytes, "result == value + value", 23) == 0);
            }
        }
        CHECK(promises == 2); /* Same subject/type list shares one promise. */
    }
    mo_arena_free(&output);
    mo_diags_free(&diags);
    mo_buffer_free(&source);
}

/* Counts the spec functions (examples, promises, anchors, tests) lowered for module. */
static uint32_t spec_funs(const struct mo_core *core, const char *module) {
    uint32_t count = 0;
    for (size_t i = 0; i < core->funs.count; i++) {
        const struct mo_core_fun *fun = &core->funs.items[i];
        if (fun->kind != MO_CORE_FUNCTION && fun->module.length == strlen(module) &&
            memcmp(fun->module.bytes, module, fun->module.length) == 0)
            count++;
    }
    return count;
}

/* A library module's specs run only under `mo test` of its own file, so a program using it
 * lowers none of them; given that file, its example, promise, two anchors and test lower. */
static void test_source_library_specs(void *unused) {
    (void)unused;
    static const struct {
        const char *entry;
        const char *module;
        uint32_t own, library;
    } cases[] = {{"tests/lower/library-specs/main.mo", "Main", 1, 0},
                 {"tests/lower/library-specs/lib/shapes.mo", "Shapes", 5, 5}};
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        struct mo_arena input, output;
        struct mo_sources sources;
        struct mo_diags diags;
        mo_arena_init(&input, MO_LIMIT_LOWER_WORK);
        mo_arena_init(&output, MO_LIMIT_LOWER_WORK);
        MO_ASSERT(mo_sources_init(&sources) && mo_diags_init(&diags));
        struct mo_module_set *set = mo_load_program(&input, &sources, cases[c].entry,
                                                    "tests/lower/library-specs/lib", &diags);
        MO_ASSERT(set != NULL && !mo_diags_any(&diags));
        struct mo_program *program = mo_check(&input, set, &diags);
        MO_ASSERT(program != NULL && !mo_diags_any(&diags));
        struct mo_core *core = mo_lower(&output, program, &sources, &diags);
        CHECK(core != NULL && !mo_diags_any(&diags));
        if (core != NULL) {
            CHECK(spec_funs(core, cases[c].module) == cases[c].own);
            CHECK(spec_funs(core, "Shapes") == cases[c].library);
        }
        mo_diags_free(&diags);
        mo_sources_free(&sources);
        mo_arena_free(&output);
        mo_arena_free(&input);
    }
}

static MO_MUST_USE bool count_instruction(void *context) {
    uint64_t *count = context;
    MO_ASSERT(*count != UINT64_MAX);
    (*count)++;
    return true;
}

/* Instructions a fresh process executes for Main.<name>(arguments), which must return a List(Int)
 * of n elements, ascending when order is 1 and descending when it is -1. */
static uint64_t list_work(const struct mo_image *image, const char *name, const int64_t *arguments,
                          uint32_t count, int64_t n, int order) {
    const struct mo_bytecode *file = mo_image_bytecode(image);
    uint32_t function = UINT32_MAX;
    for (uint32_t j = 0; j < file->function_count; j++) {
        const struct mo_bytecode_string *s = &file->strings[file->functions[j].name];
        if (s->length == strlen(name) && memcmp(s->bytes, name, s->length) == 0)
            function = j;
    }
    MO_ASSERT(function != UINT32_MAX && count <= 3);
    struct mo_limits limits = mo_limits_default();
    struct mo_process *process = mo_process_new(image, &limits);
    MO_ASSERT(process != NULL);
    struct mo_cell cells[3], result;
    for (uint32_t i = 0; i < count; i++)
        cells[i] = mo_cell_int(arguments[i]);
    struct mo_defect defect;
    uint64_t instructions = 0;
    MO_ASSERT(mo_call_controlled(process, function, cells, count, count_instruction, &instructions,
                                 &result, &defect) == MO_CONTROL_RETURNED);
    CHECK(mo_list_size(result) == n);
    for (uint32_t i = 1; i < mo_list_size(result); i++) {
        struct mo_cell before, after;
        MO_ASSERT(mo_list_get(result, i - 1, &before) && mo_list_get(result, i, &after));
        int64_t step = mo_cell_get_int(after) - mo_cell_get_int(before);
        CHECK(order * step >= 0);
    }
    mo_process_free(process);
    return instructions;
}

/* Reference 13.3: sort, sort_by and sort_by_desc take n log n work. They are Mo code in
 * lib/list.mo, so the VM's own instruction count measures them, without a clock. For each input
 * shape the sort's work is sorted(...) minus input(...), at N and 4N elements. n log n work
 * grows by 4 log(4N) / log(N), 5 at N = 256; quadratic work grows by 16. The bound allows a
 * quarter more than n log n. Natives such as append run uncounted; 13.3's other rows cover them. */
static void test_source_sort_work(void *unused) {
    (void)unused;
    enum { N = 256 };
    struct mo_arena input, output;
    struct mo_sources sources;
    struct mo_diags diags;
    mo_arena_init(&input, MO_LIMIT_LOWER_WORK);
    mo_arena_init(&output, MO_LIMIT_LOWER_WORK);
    MO_ASSERT(mo_sources_init(&sources) && mo_diags_init(&diags));
    struct mo_module_set *set =
        mo_load_program(&input, &sources, "tests/lower/sort-work/main.mo", "lib", &diags);
    MO_ASSERT(set != NULL && !mo_diags_any(&diags));
    struct mo_program *program = mo_check(&input, set, &diags);
    MO_ASSERT(program != NULL && !mo_diags_any(&diags));
    struct mo_core *core = mo_lower(&output, program, &sources, &diags);
    MO_ASSERT(core != NULL && !mo_diags_any(&diags) && mo_core_check(core, &diags));
    struct mo_buffer bytes;
    mo_buffer_init(&bytes, MO_LIMIT_BYTECODE_SIZE);
    MO_ASSERT(mo_compile(core, &bytes, &diags));
    struct mo_load_error error;
    struct mo_image *image = mo_load(bytes.bytes, bytes.length, &error);
    MO_ASSERT(image != NULL);
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    MO_ASSERT(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    static const char *const ways[] = {"sort", "sort_by", "sort_by_desc"};
    static const char *const shapes[] = {"scrambled", "sorted", "reversed", "equal"};
    for (int64_t way = 0; way < 3; way++)
        for (int64_t shape = 0; shape < 4; shape++) {
            uint64_t work[2];
            for (int64_t size = 0; size < 2; size++) {
                int64_t n = size == 0 ? N : 4 * N, arguments[] = {n, shape, way};
                uint64_t sorting =
                    list_work(image, "Main.sorted", arguments, 3, n, way == 2 ? -1 : 1);
                uint64_t building = list_work(image, "Main.input", arguments, 2, n, 0);
                MO_ASSERT(sorting > building);
                work[size] = sorting - building;
            }
            /* work[1] / work[0] <= 1.25 * 4 * log2(4N) / log2(N), with N = 256: 6.25. */
            bool bounded = work[1] * 4 <= work[0] * 25;
            CHECK(bounded);
            if (!bounded)
                (void)fprintf(stderr, "%s on %s input: %llu instructions at %d, %llu at %d\n",
                              ways[way], shapes[shape], (unsigned long long)work[0], N,
                              (unsigned long long)work[1], 4 * N);
        }
    mo_image_free(image);
    mo_buffer_free(&bytes);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
    mo_arena_free(&output);
    mo_arena_free(&input);
}

/* FileError's text form is lib/file-error.mo's to_text; without the library, lowering refuses it
 * rather than derive constructor syntax. */
static void test_source_file_error_text(void *unused) {
    (void)unused;
    struct mo_arena output;
    struct mo_diags diags;
    struct mo_buffer source;
    mo_arena_init(&output, MO_LIMIT_LOWER_WORK);
    MO_ASSERT(mo_diags_init(&diags));
    mo_buffer_init(&source, MO_LIMIT_FILE_SIZE);
    MO_ASSERT(mo_buffer_append_text(
        &source, "module Main\nfn main() : Int\n"
                 "missing = NotFound(path: \"a\")\n\"#{missing}\".byte_count\nend\n"));
    struct mo_core *core = source_core(&output, &source, &diags);
    CHECK(core == NULL && diags.count == 1 && diags.items[0].code == MO_CODE_LOWER_UNFINISHED &&
          strcmp(diags.items[0].message, "FileError's text form needs the library") == 0);
    mo_arena_free(&output);
    mo_diags_free(&diags);
    mo_buffer_free(&source);
}

static void test_source_constants(void *unused) {
    (void)unused;
    struct mo_arena output;
    struct mo_diags diags;
    struct mo_buffer source;
    mo_arena_init(&output, MO_LIMIT_LOWER_WORK);
    MO_ASSERT(mo_diags_init(&diags));
    mo_buffer_init(&source, MO_LIMIT_FILE_SIZE);
    MO_ASSERT(mo_buffer_append_text(&source,
                                    "module Main\nbase = 13\nhandler = first\nanswer = handler()\n"
                                    "reader = fn() Some(try Some(1)) end\n"
                                    "fn first() : Int\nbase\nend\n"
                                    "fn main() : Int\nanswer + (reader() or 7)\nend\n"));
    struct mo_core *core = source_core(&output, &source, &diags);
    CHECK(core != NULL && !mo_diags_any(&diags));
    if (core != NULL)
        run(core, 14);
    mo_arena_free(&output);
    mo_diags_free(&diags);
    mo_arena_init(&output, MO_LIMIT_LOWER_WORK);
    MO_ASSERT(mo_diags_init(&diags));
    mo_buffer_clear(&source);
    MO_ASSERT(mo_buffer_append_text(&source,
                                    "module Main\nanswer = handler(-7)\nhandler = Int.abs\n"
                                    "fn main() : Int\nanswer + 30\nend\n"));
    core = source_core(&output, &source, &diags);
    CHECK(core != NULL && !mo_diags_any(&diags));
    if (core != NULL)
        run(core, 37);
    mo_arena_free(&output);
    mo_diags_free(&diags);
    const char *deferred[] = {
        "handler = fn() base end\nbase = 13\nfn main() : Int\nhandler() + 24\nend\n",
        ("answer = handler()\nhandler = first\nbase = 13\n"
         "fn first() : Int\nbase\nend\nfn main() : Int\nanswer + 24\nend\n"),
        "handler = first\nfn first() : Int\nhandler()\nend\nfn main() : Int\n17\nend\n"};
    for (size_t i = 0; i < sizeof deferred / sizeof deferred[0]; i++) {
        mo_arena_init(&output, MO_LIMIT_LOWER_WORK);
        MO_ASSERT(mo_diags_init(&diags));
        mo_buffer_clear(&source);
        MO_ASSERT(mo_buffer_append_text(&source, "module Main\n") &&
                  mo_buffer_append_text(&source, deferred[i]));
        core = source_core(&output, &source, &diags);
        CHECK(core == NULL && diags.count == 1 && diags.items[0].code == MO_CODE_LOWER_UNFINISHED);
        mo_arena_free(&output);
        mo_diags_free(&diags);
    }
    mo_buffer_free(&source);
}

static void test_source_captures(void *unused) {
    (void)unused;
    struct mo_arena output;
    struct mo_diags diags;
    struct mo_buffer source;
    mo_arena_init(&output, MO_LIMIT_LOWER_WORK);
    MO_ASSERT(mo_diags_init(&diags));
    mo_buffer_init(&source, MO_LIMIT_FILE_SIZE);
    MO_ASSERT(mo_buffer_append_text(
        &source,
        "module Main\nfn make(left: Int, right: Int) : fn(Int) : fn() : Int\n"
        "fn(shift)\ntemp = left * 7\nfn() right + temp + shift + left + right end\nend\nend\n"
        "fn main() : Int\nmake(3, 29)(5)()\nend\n"));
    struct mo_core *core = source_core(&output, &source, &diags);
    CHECK(core != NULL && !mo_diags_any(&diags));
    if (core != NULL)
        run(core, 87);
    mo_buffer_free(&source);
    mo_diags_free(&diags);
    mo_arena_free(&output);
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
        MO_ASSERT(mo_buffer_append_text(&source, "module Main\nfn count() : Int\nvar n = 0\n"));
        for (uint32_t i = 0; i < statements; i++)
            MO_ASSERT(mo_buffer_append_text(&source, "n += 1\n"));
        MO_ASSERT(mo_buffer_append_text(&source, "n\nend\nfn main() : Int\ncount() % 255\nend\n"));
        mo_arena_init(&output, MO_LIMIT_LOWER_WORK);
        MO_ASSERT(mo_diags_init(&diags));
        core = source_core(&output, &source, &diags);
        if (statements < MO_LIMIT_CORE_DEPTH - 2) {
            CHECK(core != NULL && !mo_diags_any(&diags));
            if (core != NULL) {
                run(core, statements % 255);
                mo_buffer_clear(&printed);
                CHECK(!mo_core_print(core, &printed)); /* Codec wrappers consume extra depth. */
            }
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
    test_captures();
    test_option_or();
    test_try_option();
    test_try_result();
    test_interpolation();
    test_derived_text();
    test_tuple_order();
    test_float_compare();
    test_choice_order();
    test_list_order();
    test_list_text();
    test_constants();
    test_specs();
    test_structural_failures();
    test_structural_patterns();
    test_structural_binary_text();
    test_structural_lazy_conditions();
    test_structural_customizers();
    test_promises();
    test_record_text();
    test_text();
    test_result_entry();
    test_limits();
    test_native_refusal();
    test_abstract_ability_refusal();
    test_ignored_parameters();
    test_substituted_depth();
    MO_ASSERT(mo_with_stack(MO_SOURCE_STACK_SIZE, test_source_specs, NULL) == 0);
    MO_ASSERT(mo_with_stack(MO_SOURCE_STACK_SIZE, test_source_constants, NULL) == 0);
    MO_ASSERT(mo_with_stack(MO_SOURCE_STACK_SIZE, test_source_library_specs, NULL) == 0);
    MO_ASSERT(mo_with_stack(MO_SOURCE_STACK_SIZE, test_source_sort_work, NULL) == 0);
    MO_ASSERT(mo_with_stack(MO_SOURCE_STACK_SIZE, test_source_file_error_text, NULL) == 0);
    MO_ASSERT(mo_with_stack(MO_SOURCE_STACK_SIZE, test_source_captures, NULL) == 0);
    MO_ASSERT(mo_with_stack(MO_SOURCE_STACK_SIZE, test_source, NULL) == 0);
    return mo_test_report("lower");
}
