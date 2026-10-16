#include "base/buffer.h"
#include "base/codes.h"
#include "base/limits.h"
#include "base/stack.h"
#include "check/check.h"
#include "test.h"

#include <string.h>

struct fixture {
    struct mo_arena syntax, arena;
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_module_syntax *module;
    uint32_t order, *uses;
    struct mo_module_set set;
    struct mo_program *program;
};

static void init(struct fixture *f, const char *text) {
    *f = (struct fixture){0};
    mo_arena_init(&f->syntax, 64 << 20);
    mo_arena_init(&f->arena, MO_CLI_CHECK_SIZE);
    MO_ASSERT(mo_sources_init(&f->sources) && mo_diags_init(&f->diags));
    uint32_t file;
    struct mo_source_error error;
    MO_ASSERT(mo_sources_add_bytes(&f->sources, "main.mo", (const uint8_t *)text, strlen(text),
                                   &file, &error));
    f->module = mo_parse(&f->syntax, &f->sources, file, &f->diags);
    MO_ASSERT(f->module != NULL && !mo_diags_any(&f->diags));
    f->set = (struct mo_module_set){.count = 1,
                                    .modules = &f->module,
                                    .order = &f->order,
                                    .uses = &f->uses,
                                    .sources = &f->sources};
    f->program = mo_check(&f->arena, &f->set, &f->diags);
}

static void free_fixture(struct fixture *f) {
    mo_arena_free(&f->arena);
    mo_arena_free(&f->syntax);
    mo_diags_free(&f->diags);
    mo_sources_free(&f->sources);
}

static void number_literal_bound(void) {
    struct fixture f;
    init(&f, "module Main\nfn f(x: T) : T where T: Number\nx + 1\nend\n"
             "fn main() : Int\nf(1)\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_body *body = &f.program->functions[1].body;
        bool instantiated = false;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            if (row->syntax->kind != MO_S_CALL || row->arguments.count == 0)
                continue;
            CHECK(strcmp(row->arguments.items[0].type->name, "Int") == 0);
            instantiated = true;
        }
        CHECK(instantiated);
    }
    free_fixture(&f);

    const char *rejected[] = {
        "module Main\nfn f(x: T) : T where T: Number\nx + 9223372036854775808\nend\n"
        "fn main() : Int\nf(1)\nend\n",
        "module Main\nfn f() : T where T: Number\n"
        "170141183460469231731687303715884105728\nend\nfn main() : Int\nf()\nend\n",
        "module Main\nfn f() : T where T: Number\n-9223372036854775809\nend\n"
        "fn main() : Int\nf()\nend\n",
    };
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        init(&f, rejected[i]);
        CHECK(f.program == NULL);
        CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_LITERAL_RANGE);
        free_fixture(&f);
    }

    init(&f, "module Main\nfn f(x: T) : T where T: Number\nx + 1.5\nend\n"
             "fn main() : Int\nf(1)\nend\n");
    CHECK(f.program == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_TYPE_MISMATCH);
    free_fixture(&f);
}

static void map_graph(void) {
    struct fixture f;
    init(&f, "module Main\nfn main() : Int\nm = {\"a\": 1}\ncase m\n_: 1\nend\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_body *body = &f.program->functions[f.program->entry.function].body;
        bool found = false;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            if (row->syntax->kind != MO_S_MAP)
                continue;
            CHECK(row->operand_count == 2);
            if (row->operand_count == 2) {
                CHECK(body->items[row->operands[0]].syntax->kind == MO_S_TEXT);
                CHECK(body->items[row->operands[1]].syntax->kind == MO_S_INT);
            }
            found = true;
        }
        CHECK(found);
    }
    free_fixture(&f);
}

static void constant_graph(void) {
    struct fixture f;
    init(&f, "module Main\nanswer = base - 5\nbase = 13\nindependent = true\n"
             "fn main() : Int\nanswer\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        CHECK(f.program->constant_count == 3);
        MO_ASSERT(f.program->constant_count == 3);
        const struct mo_checked_constant *base = &f.program->constants[0];
        const struct mo_checked_constant *answer = &f.program->constants[1];
        CHECK(strcmp(base->symbol->name, "base") == 0 && strcmp(base->type->name, "Int") == 0);
        CHECK(strcmp(answer->symbol->name, "answer") == 0 &&
              strcmp(answer->type->name, "Int") == 0);
        CHECK(strcmp(f.program->constants[2].symbol->name, "independent") == 0);
        const struct mo_checked_body *body = &answer->initializer;
        MO_ASSERT(body->root < body->count);
        const struct mo_checked_expression *root = &body->items[body->root];
        CHECK(root->operation == MO_T_MINUS && root->type == answer->type);
        MO_ASSERT(root->operand_count == 2);
        CHECK(body->items[root->operands[0]].symbol == base->symbol &&
              body->items[root->operands[1]].syntax->text[0] == '5');
        const struct mo_checked_body *main = &f.program->functions[f.program->entry.function].body;
        bool read = false;
        for (uint32_t i = 0; i < main->count; i++)
            read = read || main->items[i].symbol == answer->symbol;
        CHECK(read);
    }
    free_fixture(&f);
}

static void constant_names(void) {
    struct fixture f;
    init(&f, "module Main\nnot_boolean? = 17\nfn main()\nend\n");
    CHECK(f.program == NULL && f.diags.count == 1 && f.diags.items[0].code == MO_CODE_BOOL_NAME);
    if (f.diags.count != 0)
        CHECK(f.diags.items[0].primary.span.start == strlen("module Main\n") &&
              f.diags.items[0].primary.span.end == strlen("module Main\nnot_boolean?"));
    free_fixture(&f);
    init(&f, "module Main\nready? = fn() 17 end\nfn main()\nend\n");
    CHECK(f.program == NULL && f.diags.count == 1 && f.diags.items[0].code == MO_CODE_BOOL_NAME);
    free_fixture(&f);
    init(&f, "module Main\nready? = fn() false end\nfn main()\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    free_fixture(&f);
}

static void constant_context(void) {
    struct fixture f;
    init(&f, "module Main\nvalue = Some(try Some(1))\nfn main()\nend\n");
    CHECK(f.program == NULL && f.diags.count == 1 && f.diags.items[0].code == MO_CODE_TRY_RESULT);
    free_fixture(&f);
    init(&f, "module Main\nreader = fn() Some(try Some(1)) end\nfn main()\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    free_fixture(&f);
}

static void constant_dependencies(void) {
    static const struct {
        const char *source;
        const char *names[3];
        uint32_t count;
    } cases[] = {
        {"answer = first()\nindependent = 7\nbase = 13\n"
         "fn first() : Int\nsecond()\nend\nfn second() : Int\nbase - 5\nend\n",
         {"independent", "base", "answer"},
         3},
        {"answer = first(2)\nbase = 13\n"
         "fn first(n: Int) : Int\nif n == 0: base else: second(n - 1)\nend\n"
         "fn second(n: Int) : Int\nfirst(n)\nend\n",
         {"base", "answer"},
         2},
        {"answer = handler()\nhandler = first\nbase = 13\n"
         "fn first() : Int\nbase\nend\n",
         {"handler", "base", "answer"},
         3},
        {"answer = handler(-5)\nhandler = Int.abs\n", {"handler", "answer"}, 2},
        {"answer = render()\nbase = \"label\"\nstruct Label\nvalue: Int\nend\n"
         "fn to_text(label: Label) : Text\n\"#{base}#{label.value}\"\nend\n"
         "fn render() : Text\n\"#{(Label(value: 3), [Label(value: 4)])}\"\nend\n",
         {"base", "answer"},
         2},
        {"answer = Label(value: 3) < Label(value: 4)\nbase = Less\n"
         "struct Label\nvalue: Int\nend\n"
         "fn compare(_a: Label, _b: Label) : Ordering\nbase\nend\n",
         {"base", "answer"},
         2},
        {"answer = ignore(Label(value: 3))\nstruct Label\nvalue: Int\nend\n"
         "fn to_text(_label: Label) : Text\nanswer.to_text\nend\n"
         "fn ignore(_value: T) : Int where T: Text\n42\nend\n",
         {"answer"},
         1},
        {"answer = [1, 2].map(first).sum\nbase = 13\n"
         "fn first(n: Int) : Int\nn + base\nend\n",
         {"base", "answer"},
         2},
        {"answer = [1, 2].map(fn(n) n + base end).sum\nbase = 13\n", {"base", "answer"}, 2},
        {"answer = {\"a\": 1}.update(\"a\", 0, fn(n) n + base end).get(\"a\") or 0\nbase = 13\n",
         {"base", "answer"},
         2},
        {"answer = [Label(value: 3), Label(value: 4)].sort_by_desc(fn(x) x end).size\nbase = Less\n"
         "struct Label\nvalue: Int\nend\n"
         "fn compare(_a: Label, _b: Label) : Ordering\nbase\nend\n",
         {"base", "answer"},
         2},
        {"answer = [1, 2, 3].count(fn(n) n > base end)\nbase = 1\n", {"base", "answer"}, 2},
        {"value = [reader].append(reader).size\nfn reader() : Int\nvalue\nend\n", {"value"}, 1},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct mo_buffer source;
        mo_buffer_init(&source, 4096);
        MO_ASSERT(mo_buffer_append_text(&source, "module Main\n") &&
                  mo_buffer_append_text(&source, cases[i].source) &&
                  mo_buffer_append_text(&source, "fn main()\nend\n") &&
                  mo_buffer_append_byte(&source, 0));
        struct fixture f;
        init(&f, (const char *)source.bytes);
        CHECK(f.program != NULL && !mo_diags_any(&f.diags));
        if (f.program != NULL) {
            CHECK(f.program->constant_count == cases[i].count);
            for (uint32_t j = 0; j < f.program->constant_count && j < cases[i].count; j++) {
                const struct mo_checked_constant *constant = &f.program->constants[j];
                CHECK(strcmp(constant->symbol->name, cases[i].names[j]) == 0);
                CHECK(constant->type->parameters == 0);
            }
        }
        free_fixture(&f);
        mo_buffer_free(&source);
    }
}

static void constant_refusals(void) {
    static const struct {
        const char *source;
        uint16_t code;
    } cases[] = {
        {"value = value\n", MO_CODE_CONSTANT_CYCLE},
        {"first = second\nsecond = first\n", MO_CODE_CONSTANT_CYCLE},
        {"value = reader()\nfn reader() : Int\nvalue\nend\n", MO_CODE_CONSTANT_CYCLE},
        {"first = reader()\nsecond = first + 1\nfn reader() : Int\nsecond\nend\n",
         MO_CODE_CONSTANT_CYCLE},
        {"value = \"#{Label(value: 3)}\"\nstruct Label\nvalue: Int\nend\n"
         "fn to_text(_label: Label) : Text\nvalue\nend\n",
         MO_CODE_CONSTANT_CYCLE},
        {"value = [1].map(reader).size\nfn reader(_n: Int) : Int\nvalue\nend\n",
         MO_CODE_CONSTANT_CYCLE},
        {"b = helper()\na = [1, 2].map(fn(x) x + b end).sum\nfn helper() : Int\na\nend\n",
         MO_CODE_CONSTANT_CYCLE},
        {"value = {\"a\": 1}.update(\"a\", 0, reader).size\nfn reader(_n: Int) : Int\nvalue\nend\n",
         MO_CODE_CONSTANT_CYCLE},
        {"value = [Label(value: 3), Label(value: 4)].sort_by_desc(fn(x) x end).size\n"
         "struct Label\nvalue: Int\nend\n"
         "fn compare(_a: Label, _b: Label) : Ordering\nif value == 0: Less else: Equal\nend\n",
         MO_CODE_CONSTANT_CYCLE},
        {"value = [1].count(reader)\nfn reader(_n: Int) : Bool\nvalue == 0\nend\n",
         MO_CODE_CONSTANT_CYCLE},
        {"values = []\nfn read() : List(Int)\nvalues\nend\n", MO_CODE_CANNOT_INFER},
        {"reader = fn(x) x end\n", MO_CODE_CANNOT_INFER},
        {"value = 17\nfn read() : Byte\nvalue\nend\n", MO_CODE_RESULT_TYPE},
        {"value = apply(reader)\nfn apply(f: fn() : Int) : Int\nf()\nend\n"
         "fn reader() : Int\n1\nend\n",
         MO_CODE_CHECK_UNFINISHED},
        {"value = render(3)\nfn render(x: T) : Text where T: Text\nx.to_text\nend\n",
         MO_CODE_CHECK_UNFINISHED},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct mo_buffer source;
        mo_buffer_init(&source, 4096);
        MO_ASSERT(mo_buffer_append_text(&source, "module Main\n") &&
                  mo_buffer_append_text(&source, cases[i].source) &&
                  mo_buffer_append_text(&source, "fn main()\nend\n") &&
                  mo_buffer_append_byte(&source, 0));
        struct fixture f;
        init(&f, (const char *)source.bytes);
        CHECK(f.program == NULL && f.diags.count == 1 && f.diags.items[0].code == cases[i].code);
        free_fixture(&f);
        mo_buffer_free(&source);
    }
}

static void constant_chain(void) {
    const uint32_t count = 5000;
    struct mo_buffer source;
    mo_buffer_init(&source, 1 << 20);
    MO_ASSERT(mo_buffer_append_text(&source, "module Main\n"));
    for (uint32_t i = 0; i < count; i++) {
        MO_ASSERT(mo_buffer_append_text(&source, "c") && mo_buffer_append_uint(&source, i) &&
                  mo_buffer_append_text(&source, " = "));
        if (i + 1 < count)
            MO_ASSERT(mo_buffer_append_text(&source, "c") && mo_buffer_append_uint(&source, i + 1));
        else
            MO_ASSERT(mo_buffer_append_text(&source, "13"));
        MO_ASSERT(mo_buffer_append_text(&source, "\n"));
    }
    MO_ASSERT(mo_buffer_append_text(&source, "fn main() : Int\nc0\nend\n") &&
              mo_buffer_append_byte(&source, 0));
    struct fixture f;
    init(&f, (const char *)source.bytes);
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        CHECK(f.program->constant_count == count);
        for (uint32_t i = 1; i < f.program->constant_count; i++) {
            const struct mo_checked_constant *constant = &f.program->constants[i];
            const struct mo_checked_body *body = &constant->initializer;
            CHECK(body->items[body->root].symbol == f.program->constants[i - 1].symbol);
        }
    }
    free_fixture(&f);
    mo_buffer_free(&source);
}

static void example_graph(void) {
    struct fixture f;
    init(&f, "module Main\nfn plus(a: Int) : Int\nexamples\n"
             "plus(3) == 4\nend\nexamples\nplus(8) == 9\nend\na + 1\nend\n"
             "fn main() : Int\nexamples\nfalse\nend\nplus(2)\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        CHECK(f.program->spec_count == 3);
        for (uint32_t i = 0; i < 2 && i < f.program->spec_count; i++) {
            const struct mo_checked_spec *spec = &f.program->specs[i];
            CHECK(spec->kind == MO_SPEC_EXAMPLE && spec->module == 0 && spec->subject == 0 &&
                  spec->number == i + 1 && spec->promise == UINT32_MAX);
            CHECK(spec->parameter_count == 0 && spec->condition_count == 0);
            MO_ASSERT(spec->body.root < spec->body.count);
            const struct mo_checked_expression *root = &spec->body.items[spec->body.root];
            CHECK(strcmp(root->type->name, "Bool") == 0 && root->operation == MO_T_EQ);
            MO_ASSERT(root->operand_count == 2);
            const struct mo_checked_expression *call = &spec->body.items[root->operands[0]];
            CHECK(call->symbol == f.program->functions[0].signature->function);
            MO_ASSERT(call->operand_count == 2);
            CHECK(spec->body.items[call->operands[1]].syntax->text[0] == (i == 0 ? '3' : '8'));
            CHECK(spec->body.items[root->operands[1]].syntax->text[0] == (i == 0 ? '4' : '9'));
            struct mo_span span = spec->text.span;
            CHECK(spec->text.length == span.end - span.start);
            CHECK(memcmp(spec->text.bytes, f.sources.files[span.file].bytes + span.start,
                         spec->text.length) == 0);
        }
        if (f.program->spec_count == 3) {
            const struct mo_checked_spec *spec = &f.program->specs[2];
            CHECK(spec->subject == 1 && spec->number == 1);
            MO_ASSERT(spec->body.root < spec->body.count);
            CHECK(spec->body.items[spec->body.root].syntax->kind == MO_S_FALSE);
        }
        mo_sources_free(&f.sources);
        const struct mo_checked_spec *first = &f.program->specs[0];
        CHECK(first->text.length == 12 && memcmp(first->text.bytes, "plus(3) == 4", 12) == 0);
        MO_ASSERT(mo_sources_init(&f.sources));
    }
    free_fixture(&f);
}

static void promise_graph(void) {
    struct fixture f;
    init(&f, "module Main\nfn ordered(a: Int, b: Int) : (Int, Int)\n"
             "promise \"values stay\\nordered\"\nresult.0 <= result.1\n"
             "result == (a, b) or result == (b, a)\n"
             "allows result: (2, 5), b: 2, a: 5\n"
             "forbids a: 5, result: (2, 2), b: 2\nend\n"
             "if a <= b: (a, b) else: (b, a)\nend\nfn main()\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        CHECK(f.program->spec_count == 3);
        const struct mo_checked_spec *promise = &f.program->specs[0];
        CHECK(promise->kind == MO_SPEC_PROMISE && promise->subject == 0 && promise->number == 1);
        CHECK(promise->parameter_count == 3 && promise->condition_count == 2);
        CHECK(strcmp(promise->parameters[0]->name, "a") == 0 &&
              strcmp(promise->parameters[1]->name, "b") == 0 &&
              strcmp(promise->parameters[2]->name, "result") == 0);
        CHECK(promise->wording_length == 19 &&
              memcmp(promise->wording, "values stay\nordered", 19) == 0);
        CHECK(promise->conditions[0].text.length == 20 &&
              memcmp(promise->conditions[0].text.bytes, "result.0 <= result.1", 20) == 0);
        CHECK(promise->body.items[promise->conditions[0].expression].operation == MO_T_LE);
        CHECK(promise->body.items[promise->conditions[1].expression].operation == MO_T_OR);
        for (uint32_t i = 1; i < 3; i++) {
            const struct mo_checked_spec *anchor = &f.program->specs[i];
            CHECK(anchor->kind == MO_SPEC_ANCHOR && anchor->promise == 0 && anchor->subject == 0 &&
                  anchor->number == 1 && anchor->allows == (i == 1));
            CHECK(anchor->body.root == UINT32_MAX && anchor->value_count == 3);
            CHECK(anchor->body.items[anchor->values[0]].syntax->text[0] == '5');
            CHECK(anchor->body.items[anchor->values[1]].syntax->text[0] == '2');
            CHECK(anchor->body.items[anchor->values[2]].operand_count == 2);
        }
    }
    free_fixture(&f);
}

static void generic_spec_graph(void) {
    struct fixture f;
    init(&f, "module Main\n"
             "test \"private \\u{3bb}\\u{0}\"\nassert twice(3) == 6\nend\n"
             "fn twice(value: T) : T where T: Number, T: Equal\n"
             "promise \"returns twice its input\"\nresult == value + value\n"
             "allows result: 6, value: 3\nforbids value: 1.25, result: 2.0\nend\n"
             "value + value\nend\nfn main()\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        CHECK(f.program->spec_count == 4);
        const struct mo_checked_spec *test = &f.program->specs[0];
        CHECK(test->kind == MO_SPEC_TEST && test->subject == UINT32_MAX && test->number == 1);
        CHECK(test->wording_length == 11 && memcmp(test->wording, "private \xce\xbb\0", 11) == 0);
        CHECK(strcmp(test->body.items[test->body.root].type->name, "Unit") == 0);
        const struct mo_checked_spec *promise = &f.program->specs[1];
        const struct mo_source_type *parameter =
            f.program->functions[0].signature->type->arguments[0];
        CHECK(promise->parameter_count == 2 && promise->parameters[0]->type == parameter &&
              promise->parameters[1]->type == parameter);
        const struct mo_checked_expression *condition =
            &promise->body.items[promise->conditions[0].expression];
        CHECK(condition->ability_count == 1 &&
              condition->abilities[0].implementation->kind == MO_ABILITY_PARAMETER &&
              condition->abilities[0].implementation->type == parameter);
        for (uint32_t i = 2; i < 4; i++) {
            const struct mo_checked_spec *anchor = &f.program->specs[i];
            CHECK(anchor->promise == 1 && anchor->arguments.count == 1 &&
                  anchor->ability_count == 2);
            CHECK(anchor->arguments.items[0].parameter == parameter &&
                  strcmp(anchor->arguments.items[0].type->name, i == 2 ? "Int" : "Decimal") == 0);
            for (uint32_t j = 0; j < anchor->ability_count; j++) {
                CHECK(anchor->abilities[j].parameter == parameter &&
                      anchor->abilities[j].operand == UINT32_MAX &&
                      anchor->abilities[j].implementation->kind == MO_ABILITY_NATIVE);
            }
        }
        CHECK(f.program->specs[2].values[1] < f.program->specs[2].values[0]);
        CHECK(f.program->specs[3].values[0] < f.program->specs[3].values[1]);
    }
    free_fixture(&f);
}

static void fake_specs(void) {
    struct fixture f;
    init(&f, "module Main\nfn main(args: Args, console: Console)\n"
             "console.print(args.list.first or \"world\")\nend\n"
             "test \"fake handles keep their output\"\nconsole = Console.fake()\n"
             "main(Args.of([\"Ada\", \"Grace\"]), console)\n"
             "assert console.printed == [\"Ada\"]\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    free_fixture(&f);
}

static void spec_refusals(void) {
    static const struct {
        const char *values;
        uint16_t code;
    } cases[] = {
        {"value: 3", MO_CODE_ANCHOR_ARGUMENTS},
        {"result: 3", MO_CODE_ANCHOR_ARGUMENTS},
        {"value: 3, result: 3, value: 4", MO_CODE_ANCHOR_ARGUMENTS},
        {"value: 3, result: 3, extra: 4", MO_CODE_ANCHOR_ARGUMENTS},
        {"value: true, result: 3", MO_CODE_ARGUMENT_TYPE},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct mo_buffer source;
        mo_buffer_init(&source, 4096);
        MO_ASSERT(mo_buffer_append_text(&source,
                                        "module Main\nfn identity(value: Int) : Int\n"
                                        "promise \"keeps its input\"\nresult == value\nallows ") &&
                  mo_buffer_append_text(&source, cases[i].values) &&
                  mo_buffer_append_text(&source, "\nforbids value: 3, result: 4\nend\n"
                                                 "value\nend\nfn main()\nend\n") &&
                  mo_buffer_append_byte(&source, 0));
        struct fixture f;
        init(&f, (const char *)source.bytes);
        CHECK(f.program == NULL && f.diags.count == 1 && f.diags.items[0].code == cases[i].code);
        free_fixture(&f);
        mo_buffer_free(&source);
    }
    struct fixture f;
    init(&f, "module Main\nfn identity(value: Int) : Int\n"
             "promise \"not Boolean\"\nresult + value\nallows value: 1, result: 1\n"
             "forbids value: 1, result: 2\nend\nvalue\nend\nfn main()\nend\n");
    CHECK(f.program == NULL && f.diags.count == 1 &&
          f.diags.items[0].code == MO_CODE_CONDITION_TYPE);
    free_fixture(&f);
    init(&f, "module Main\ntest \"not Boolean\"\nassert 17\nend\nfn main()\nend\n");
    CHECK(f.program == NULL && f.diags.count == 1 &&
          f.diags.items[0].code == MO_CODE_CONDITION_TYPE);
    free_fixture(&f);
    init(&f, "module Main\ntest \"verbatim indentation\"\nend\n"
             "test \"\"\"\n  first\n    second\n  \"\"\"\nend\nfn main()\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_spec *spec = &f.program->specs[1];
        CHECK(spec->number == 2 && spec->wording_length == 14 &&
              memcmp(spec->wording, "first\n  second", 14) == 0);
    }
    free_fixture(&f);
}

static void entry_graph(void) {
    struct fixture f;
    init(&f, "module Main\nfn identity(value: Int) : Int\n"
             "var copy = value\ncopy = 42\ncopy\nend\n"
             "fn main() : Int\nidentity(7 + 12 * 3 - 1)\nend\n");
    const struct mo_program *program = f.program;
    CHECK(program != NULL && !mo_diags_any(&f.diags));
    if (program != NULL) {
        CHECK(program->function_count == 2);
        CHECK(program->entry.function == 1 && program->entry.result == MO_ENTRY_STATUS);
        const struct mo_checked_function *identity = &program->functions[0];
        CHECK(identity->parameter_count == 1 && identity->body.binding_count == 2);
        if (identity->parameter_count == 1 && identity->body.binding_count == 2) {
            const struct mo_checked_binding *parameter = identity->parameters[0];
            const struct mo_checked_binding *copy = &identity->body.bindings[1];
            CHECK(strcmp(parameter->name, "value") == 0);
            CHECK(strcmp(parameter->type->name, "Int") == 0 && !parameter->variable);
            CHECK(strcmp(copy->name, "copy") == 0 && copy->variable);
            CHECK(strcmp(copy->type->name, "Int") == 0);
            bool read = false, initialized = false, assigned = false;
            for (uint32_t i = 0; i < identity->body.count; i++) {
                const struct mo_checked_expression *row = &identity->body.items[i];
                if (row->syntax->kind == MO_S_NAME && row->binding == parameter)
                    read = true;
                if (row->syntax->kind == MO_S_VAR || row->syntax->kind == MO_S_BIND) {
                    CHECK(row->binding == copy);
                    CHECK(row->operand_count == 1 && row->operands[0] < i);
                    CHECK(row->assignment == (row->syntax->kind == MO_S_BIND));
                    if (row->syntax->kind == MO_S_VAR)
                        initialized = true;
                    else
                        assigned = true;
                }
            }
            CHECK(read && initialized && assigned);
        }
        const struct mo_checked_body *body = &program->functions[program->entry.function].body;
        CHECK(body->root < body->count);
        CHECK(strcmp(body->items[body->root].type->name, "Int") == 0);
        MO_ASSERT(body->count <= 64);
        int64_t values[64] = {0};
        uint32_t operators = 0;
        bool found = false, called = false;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            for (uint32_t j = 0; j < row->operand_count; j++)
                CHECK(row->operands[j] < i);
            if (row->syntax->kind == MO_S_INT) {
                for (uint32_t j = 0; j < row->syntax->span.end - row->syntax->span.start; j++)
                    values[i] = values[i] * 10 + (int64_t)(row->syntax->text[j] - '0');
                found = found || values[i] == 7;
            } else if (row->operation != MO_T_EOF) {
                operators++;
                MO_ASSERT(row->operand_count == 2 && row->operands[0] < i && row->operands[1] < i);
                int64_t left = values[row->operands[0]], right = values[row->operands[1]];
                CHECK(row->ability_count == 1);
                if (row->ability_count == 1) {
                    CHECK(row->abilities[0].operand == row->operands[0]);
                    CHECK(row->abilities[0].implementation->kind == MO_ABILITY_NATIVE);
                    CHECK(row->abilities[0].implementation->ability == MO_SOURCE_NUMBER);
                    CHECK(strcmp(row->abilities[0].implementation->type->name, "Int") == 0);
                }
                if (row->operation == MO_T_PLUS)
                    values[i] = left + right;
                else if (row->operation == MO_T_MINUS)
                    values[i] = left - right;
                else {
                    CHECK(row->operation == MO_T_STAR);
                    values[i] = left * right;
                }
            } else if (row->syntax->kind == MO_S_CALL) {
                CHECK(row->symbol == identity->signature->function);
                MO_ASSERT(row->operand_count == 2 && row->operands[0] < i && row->operands[1] < i);
                CHECK(body->items[row->operands[0]].symbol == row->symbol);
                CHECK(body->items[row->operands[0]].type->kind == MO_SOURCE_FUNCTION);
                CHECK(strcmp(body->items[row->operands[1]].type->name, "Int") == 0);
                CHECK(row->arguments.count == 0);
                CHECK(values[row->operands[1]] == 42);
                called = true;
            } else if (row->operand_count != 0) {
                values[i] = values[row->operands[row->operand_count - 1]];
            }
        }
        CHECK(found && called && operators == 3);
    }
    free_fixture(&f);
}

static void console_graph(void) {
    struct fixture f;
    init(&f, "module Main\nfn main(console: Console)\nconsole.print(\"hello\")\nend\n");
    const struct mo_program *program = f.program;
    CHECK(program != NULL && !mo_diags_any(&f.diags));
    if (program != NULL) {
        CHECK(program->entry.result == MO_ENTRY_UNIT);
        CHECK(program->entry.capability_count == 1);
        const struct mo_checked_function *main = &program->functions[program->entry.function];
        MO_ASSERT(main->parameter_count == 1 && program->entry.capability_count == 1);
        CHECK(program->entry.capabilities[0] == main->parameters[0]);
        CHECK(strcmp(main->parameters[0]->type->name, "Console") == 0);
        bool printed = false;
        for (uint32_t i = 0; i < main->body.count; i++) {
            const struct mo_checked_expression *row = &main->body.items[i];
            if (row->native == NULL)
                continue;
            CHECK(strcmp(row->native, "Console.print") == 0);
            MO_ASSERT(row->operand_count == 2 && row->operands[0] < i && row->operands[1] < i);
            CHECK(row->receiver == row->operands[0]);
            CHECK(main->body.items[row->receiver].binding == main->parameters[0]);
            const struct mo_checked_expression *text = &main->body.items[row->operands[1]];
            CHECK(text->syntax->kind == MO_S_TEXT);
            CHECK(text->syntax->span.end - text->syntax->span.start == 7);
            CHECK(memcmp(text->syntax->text, "\"hello\"", 7) == 0);
            CHECK(strcmp(row->type->name, "Unit") == 0);
            printed = true;
        }
        CHECK(printed);
    }
    free_fixture(&f);
    init(&f, "module Main\nfn main(console: Console, _args: Args) : Int\n"
             "console.print(\"two capabilities\")\n37\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        CHECK(f.program->entry.result == MO_ENTRY_STATUS);
        CHECK(f.program->entry.capability_count == 2);
        if (f.program->entry.capability_count == 2) {
            CHECK(strcmp(f.program->entry.capabilities[0]->type->name, "Console") == 0);
            CHECK(strcmp(f.program->entry.capabilities[1]->type->name, "Args") == 0);
        }
    }
    free_fixture(&f);
    init(&f, "module Lib\nexpose helper\nfn helper() : Int\n37\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        CHECK(f.program->entry.function == UINT32_MAX);
        CHECK(f.program->entry.capability_count == 0 && f.program->entry.capabilities == NULL);
    }
    free_fixture(&f);
    init(&f, "module Main\nfn main(files: ReadFiles) : Result(Unit, FileError)\n"
             "_text = try files.read_text(\"notes\")\nOk(Unit)\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        CHECK(f.program->entry.result == MO_ENTRY_RESULT);
        CHECK(f.program->entry.capability_count == 1);
        CHECK(f.program->entry.error_text != NULL &&
              f.program->entry.error_text->ability == MO_SOURCE_TEXT &&
              strcmp(f.program->entry.error_text->type->name, "FileError") == 0);
    }
    free_fixture(&f);
}

static void native_graph(void) {
    struct fixture f;
    init(
        &f,
        "module Main\nfn main() : Int\n"
        "text = \" abc \".trim\nnumber = Int.abs(-4)\nInt.min(text.byte_count, number.abs)\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_body *body = &f.program->functions[0].body;
        bool trim = false, count = false, abs = false, min = false;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            if (row->native == NULL)
                continue;
            if (strcmp(row->native, "Text.trim") == 0) {
                CHECK(row->operand_count == 1 && row->receiver == row->operands[0]);
                CHECK(strcmp(row->type->name, "Text") == 0);
                trim = true;
            } else if (strcmp(row->native, "Text.byte_count") == 0) {
                CHECK(row->operand_count == 1 && row->receiver == row->operands[0]);
                count = true;
            } else if (strcmp(row->native, "Int.abs") == 0) {
                if (row->receiver != UINT32_MAX) {
                    CHECK(row->operand_count == 1 && row->receiver == row->operands[0]);
                    abs = true;
                }
            } else if (strcmp(row->native, "Int.min") == 0 &&
                       row->type->kind != MO_SOURCE_FUNCTION) {
                CHECK(row->operand_count == 3 && row->receiver == UINT32_MAX);
                min = true;
            }
        }
        CHECK(trim && count && abs && min);
    }
    free_fixture(&f);
    init(&f, "module Main\nfn main() : Int\nset = Set.empty.add(4)\n"
             "counts = {\"a\": 1}.update(\"a\", 0, fn(n) n + 1 end)\n"
             "positives = [1, 2, 3].count(fn(x) x > 1 end)\n"
             "texts = [1, 2, 3].sort_by_desc(fn(x) x end).map(fn(x) x.to_text end)\n"
             "if set.has?(4) and positives == 2 and (counts.get(\"a\") or 0) == 2: "
             "List.map(texts, Text.byte_count).sum else: 0\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_body *body = &f.program->functions[0].body;
        bool empty = false, sum = false, dot_map = false, qualified_map = false, update = false,
             sort_desc = false, list_count = false;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            if (row->native == NULL)
                continue;
            if (strcmp(row->native, "Set.empty") == 0) {
                CHECK(strcmp(row->type->name, "Set") == 0 && row->operand_count == 0);
                CHECK(row->arguments.count == 1 && row->ability_count == 2);
                empty = true;
            } else if (strcmp(row->native, "List.sum") == 0) {
                CHECK(strcmp(row->type->name, "Int") == 0);
                CHECK(row->ability_count == 1 &&
                      row->abilities[0].implementation->ability == MO_SOURCE_NUMBER);
                sum = true;
            } else if (strcmp(row->native, "List.count") == 0 &&
                       row->type->kind != MO_SOURCE_FUNCTION) {
                CHECK(strcmp(row->type->name, "Int") == 0 && row->arguments.count == 1);
                CHECK(row->operand_count == 2 && row->receiver == row->operands[0]);
                list_count = true;
            } else if (strcmp(row->native, "Map.update") == 0 &&
                       row->type->kind != MO_SOURCE_FUNCTION) {
                CHECK(strcmp(row->type->name, "Map") == 0 && row->arguments.count == 2);
                CHECK(row->operand_count == 4 && row->receiver == row->operands[0] &&
                      row->ability_count == 2);
                update = true;
            } else if (strcmp(row->native, "List.sort_by_desc") == 0 &&
                       row->type->kind != MO_SOURCE_FUNCTION) {
                CHECK(strcmp(row->type->name, "List") == 0 && row->arguments.count == 2);
                CHECK(row->operand_count == 2 && row->receiver == row->operands[0] &&
                      row->ability_count == 2 &&
                      row->abilities[1].implementation->ability == MO_SOURCE_ORDER);
                sort_desc = true;
            } else if (strcmp(row->native, "List.map") == 0 &&
                       row->type->kind != MO_SOURCE_FUNCTION) {
                CHECK(strcmp(row->type->name, "List") == 0 && row->arguments.count == 2);
                if (row->receiver != UINT32_MAX) {
                    CHECK(row->operand_count == 2 && row->receiver == row->operands[0]);
                    dot_map = true;
                } else {
                    CHECK(row->operand_count == 3);
                    qualified_map = true;
                }
            }
        }
        CHECK(empty && sum && dot_map && qualified_map && update && sort_desc && list_count);
    }
    free_fixture(&f);
}

static void interpolation_graph(void) {
    struct fixture f;
    init(&f, "module Main\nfn render(value: T) : Text where T: Text\n"
             "\"a #{value} b #{true} c\"\nend\nfn main() : Int\nrender(7).byte_count\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_body *body = &f.program->functions[0].body;
        bool found = false;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            CHECK(row->syntax->kind != MO_S_TOKEN);
            if (row->syntax->kind != MO_S_TEXT_INTERPOLATED)
                continue;
            MO_ASSERT(row->operand_count == 2 && row->ability_count == 2);
            CHECK(row->syntax->count == 5 && strcmp(row->type->name, "Text") == 0);
            CHECK(body->items[row->operands[0]].binding == f.program->functions[0].parameters[0]);
            CHECK(body->items[row->operands[1]].syntax->kind == MO_S_TRUE);
            for (uint32_t j = 0; j < 2; j++) {
                CHECK(row->abilities[j].operand == row->operands[j]);
                CHECK(row->abilities[j].parameter == NULL);
                CHECK(row->abilities[j].implementation->ability == MO_SOURCE_TEXT);
            }
            CHECK(row->abilities[0].implementation->kind == MO_ABILITY_PARAMETER);
            CHECK(row->abilities[1].implementation->kind == MO_ABILITY_NATIVE);
            found = true;
        }
        CHECK(found);
    }
    free_fixture(&f);
}

static void ability_dot_graph(void) {
    struct fixture f;
    init(&f, "module Main\nstruct Box(T)\nvalue: T\nend\n"
             "fn to_text(box: Box(T)) : Text where T: Text\n\"#{box.value}\"\nend\n"
             "fn show(x: T) : Text where T: Text\nx.to_text\nend\n"
             "fn order(a: T, b: T) : Ordering where T: Order\na.compare(b)\nend\n"
             "fn main() : Int\nassert order(1, 2) == Less\n"
             "show(Box(value: 7)).byte_count + (true, 3).to_text.byte_count\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        uint32_t conversions = 0, comparisons = 0;
        for (uint32_t j = 0; j < f.program->function_count; j++) {
            const struct mo_checked_body *body = &f.program->functions[j].body;
            for (uint32_t i = 0; i < body->count; i++) {
                const struct mo_checked_expression *row = &body->items[i];
                if (row->native == NULL ||
                    (strcmp(row->native, "to_text") != 0 && strcmp(row->native, "compare") != 0))
                    continue;
                bool text = strcmp(row->native, "to_text") == 0;
                MO_ASSERT(row->ability_count == 1 && row->operand_count == (text ? 1u : 2u));
                CHECK(row->receiver == row->operands[0] && row->symbol == NULL);
                CHECK(row->abilities[0].operand == row->receiver);
                CHECK(row->abilities[0].implementation->ability ==
                      (text ? MO_SOURCE_TEXT : MO_SOURCE_ORDER));
                CHECK(strcmp(row->type->name, text ? "Text" : "Ordering") == 0);
                if (text)
                    conversions++;
                else
                    comparisons++;
            }
        }
        CHECK(conversions == 2 && comparisons == 1);
    }
    free_fixture(&f);
}

static void customizer_helpers(void) {
    struct fixture f;
    static const char *const sources[] = {
        "fn compare(a: Int, b: Int) : Bool\na == b\nend\n"
        "fn main() : Int\nif compare(3, 4): 9 else: 7\nend\n",
        "fn to_text(_n: Int) : Int\n7\nend\n"
        "fn main() : Int\nto_text(1) + 2.to_text.byte_count\nend\n",
        "fn to_text() : Int\n7\nend\nfn main() : Int\nto_text()\nend\n",
        "fn compare() : Bool\ntrue\nend\nfn main() : Int\nif compare(): 7 else: 9\nend\n",
        "fn to_text(x: T) : T\nx\nend\nfn main() : Int\nto_text(7)\nend\n",
        "fn to_text(f: fn(Int) : Int) : Int\nf(7)\nend\n"
        "fn main() : Int\nto_text(fn(x) x end)\nend\n",
        "fn to_text(_error: FileError) : Text\n\"helper\"\nend\n"
        "fn main() : Int\nto_text(NotFound(path: \"notes\")).byte_count\nend\n",
    };
    for (uint32_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        struct mo_buffer source;
        mo_buffer_init(&source, MO_LIMIT_FILE_SIZE);
        MO_ASSERT(mo_buffer_append_text(&source, "module Main\n") &&
                  mo_buffer_append_text(&source, sources[i]) && mo_buffer_append_byte(&source, 0));
        init(&f, (const char *)source.bytes);
        CHECK(f.program == NULL && f.diags.count == 1 &&
              f.diags.items[0].code == MO_CODE_ABILITY_CUSTOMIZER);
        free_fixture(&f);
        mo_buffer_free(&source);
    }
}

static void method_graph(void) {
    struct fixture f;
    init(&f, "module Main\nstruct Box(T)\nvalue: T\nend\n"
             "fn bump(box: Box(T), by: T) : T where T: Number\nbox.value + by\nend\n"
             "fn main() : Int\nBox(value: 3).bump(4)\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_body *body = &f.program->functions[1].body;
        bool called = false;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            if (row->receiver == UINT32_MAX)
                continue;
            MO_ASSERT(row->operand_count == 3 && row->symbol != NULL);
            CHECK(strcmp(row->symbol->name, "bump") == 0);
            CHECK(row->receiver == row->operands[1]);
            CHECK(body->items[row->operands[0]].type->kind == MO_SOURCE_FUNCTION);
            CHECK(body->items[row->operands[0]].symbol == row->symbol);
            CHECK(strcmp(body->items[row->receiver].type->name, "Box") == 0);
            CHECK(body->items[row->operands[2]].syntax->text[0] == '4');
            MO_ASSERT(row->arguments.count == 1 && row->ability_count == 1);
            CHECK(row->abilities[0].parameter == row->arguments.items[0].parameter);
            CHECK(row->abilities[0].implementation->ability == MO_SOURCE_NUMBER);
            called = true;
        }
        CHECK(called);
    }
    free_fixture(&f);
}

static void overload_graph(void) {
    struct fixture f;
    init(&f, "module Main\nstruct P\nvalue: Int\nend\nstruct Q\nvalue: Int\nend\n"
             "fn score(p: P) : Int\np.value\nend\n"
             "fn score(q: Q) : Int\nq.value + 10\nend\n"
             "fn main() : Int\nf = fn(value) score(value) end\n"
             "other = score\nf(P(value: 7)) + other(Q(value: 2)) + P(value: 3).score\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_body *body = &f.program->functions[2].body;
        uint32_t p = 0, q = 0;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            if (row->symbol == NULL || strcmp(row->symbol->name, "score") != 0)
                continue;
            if (row->symbol == f.program->functions[0].signature->function)
                p++;
            else {
                CHECK(row->symbol == f.program->functions[1].signature->function);
                q++;
            }
            if (row->receiver != UINT32_MAX)
                CHECK(row->receiver == row->operands[1]);
        }
        CHECK(p == 4 && q == 1);
    }
    free_fixture(&f);
}

static void delayed_dot_graph(void) {
    struct fixture f;
    init(&f, "module Main\nstruct P\nto_text: Int\napply: fn(Int) : Int\nend\n"
             "fn main() : Int\n"
             "field = fn(value) value.to_text end\n"
             "invoke = fn(value) value.apply(4).abs end\n"
             "method = fn(value) value.abs end\n"
             "p = P(to_text: 7, apply: fn(x) x + 1 end)\n"
             "field(p) + invoke(p) + method(-3)\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_body *body = &f.program->functions[0].body;
        uint32_t projections = 0;
        bool abs = false;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            for (uint32_t j = 0; j < row->operand_count; j++)
                CHECK(row->operands[j] < i);
            if (row->operand_count == 2 && row->operation == MO_T_EOF && row->symbol == NULL &&
                row->native == NULL && row->receiver == UINT32_MAX &&
                body->items[row->operands[0]].type->kind == MO_SOURCE_FUNCTION)
                CHECK(row->syntax->kind == MO_S_CALL || row->syntax->kind == MO_S_POSTFIX);
            if (row->field != UINT32_MAX && row->operand_count == 1 &&
                row->syntax->kind != MO_S_NAMED_ARG && row->symbol == NULL) {
                projections++;
                CHECK(row->native == NULL);
                CHECK(row->syntax->kind == MO_S_FIELD_ACCESS || row->syntax->kind == MO_S_POSTFIX);
                CHECK(strcmp(body->items[row->operands[0]].type->name, "P") == 0);
                if (row->field == 0)
                    CHECK(strcmp(row->type->name, "Int") == 0);
                else
                    CHECK(row->field == 1 && row->type->kind == MO_SOURCE_FUNCTION);
            }
            if (row->native != NULL && strcmp(row->native, "Int.abs") == 0) {
                abs = true;
                CHECK(row->operand_count == 1 && row->receiver == row->operands[0]);
            }
        }
        CHECK(projections == 2 && abs);
    }
    free_fixture(&f);
}

static void library_graph(void) {
    struct fixture f = {0};
    mo_arena_init(&f.syntax, 1 << 20);
    mo_arena_init(&f.arena, 16 << 20);
    MO_ASSERT(mo_sources_init(&f.sources) && mo_diags_init(&f.diags));
    struct mo_module_set *set =
        mo_load_program(&f.syntax, &f.sources, "tests/check/program-main/main.mo",
                        "tests/check/program-lib", &f.diags);
    CHECK(set != NULL && !mo_diags_any(&f.diags));
    f.program = set == NULL ? NULL : mo_check(&f.arena, set, &f.diags);
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_body *body = &f.program->functions[f.program->entry.function].body;
        bool called = false, static_call = false, custom = false;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            for (uint32_t j = 0; j < row->ability_count; j++) {
                const struct mo_ability_implementation *proof = row->abilities[j].implementation;
                if (strcmp(proof->type->name, "FileError") != 0)
                    continue;
                CHECK(proof->kind == MO_ABILITY_FUNCTION);
                MO_ASSERT(proof->function != NULL);
                const struct mo_module_scope *owner =
                    &f.program->interfaces->scopes[proof->function->module];
                CHECK(owner->library && strcmp(owner->name, "FileError") == 0);
                CHECK(strcmp(proof->function->name, "to_text") == 0);
                CHECK(proof->arguments.count == 0 && proof->part_count == 0);
                custom = true;
            }
            if (row->symbol == NULL || row->operand_count == 0)
                continue;
            CHECK(strcmp(row->symbol->name, "map") == 0);
            const struct mo_module_scope *scope =
                &f.program->interfaces->scopes[row->symbol->module];
            CHECK(scope->library && strcmp(scope->name, "List") == 0);
            MO_ASSERT(row->operand_count == 3 && row->arguments.count == 2);
            if (row->receiver == UINT32_MAX) {
                CHECK(body->items[row->operands[1]].syntax->kind == MO_S_LIST);
                static_call = true;
            } else {
                CHECK(row->receiver == row->operands[1]);
                called = true;
            }
            CHECK(body->items[row->operands[2]].lambda != NULL);
            CHECK(strcmp(row->arguments.items[0].type->name, "Int") == 0);
            CHECK(strcmp(row->arguments.items[1].type->name, "Int") == 0);
        }
        CHECK(called && static_call && custom);
    }
    free_fixture(&f);
}

static void library_scope(void) {
    struct fixture f = {0};
    mo_arena_init(&f.syntax, 1 << 20);
    mo_arena_init(&f.arena, 16 << 20);
    MO_ASSERT(mo_sources_init(&f.sources) && mo_diags_init(&f.diags));
    const char *texts[] = {"module Main\nfn main()\n_j = Json.parse(\"null\")\nend\n",
                           "module Json\nexpose parse\nfn parse(text: Text) : Int\n"
                           "text.byte_count\nend\n"};
    struct mo_module_syntax *modules[2];
    uint32_t order[] = {0, 1};
    uint32_t *uses[] = {NULL, NULL};
    bool library[] = {false, true};
    const char *names[] = {"Main", "Json"};
    for (uint32_t i = 0; i < 2; i++) {
        uint32_t file;
        struct mo_source_error error;
        MO_ASSERT(mo_sources_add_bytes(&f.sources, i == 0 ? "main.mo" : "json.mo",
                                       (const uint8_t *)texts[i], strlen(texts[i]), &file, &error));
        modules[i] = mo_parse(&f.syntax, &f.sources, file, &f.diags);
        MO_ASSERT(modules[i] != NULL && !mo_diags_any(&f.diags));
    }
    struct mo_module_set set = {.count = 2,
                                .modules = modules,
                                .order = order,
                                .uses = uses,
                                .library = library,
                                .names = names};
    f.program = mo_check(&f.arena, &set, &f.diags);
    CHECK(f.program == NULL && f.diags.count == 1);
    if (f.diags.count == 1)
        CHECK(f.diags.items[0].code == MO_CODE_IMPORT_MISSING);
    free_fixture(&f);
}

static void entry_record(void) {
    struct fixture f;
    init(&f, "module Main\nfn describe(entry: Entry) : Text\n"
             "case entry\nEntry(kind: File, name: name): name\n"
             "Entry(name: _): entry.name\nend\nend\n"
             "fn main() : Int\nentry = Entry(kind: File, name: \"notes\")\n"
             "describe(entry).byte_count\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_body *body = &f.program->functions[0].body;
        bool match = false, field = false;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            if (row->match != NULL) {
                const struct mo_checked_pattern *pattern = row->match->arms[0].patterns[0];
                CHECK(strcmp(pattern->constructor.builtin, "Entry") == 0);
                MO_ASSERT(pattern->part_count == 2);
                CHECK(strcmp(pattern->parts[0].pattern->type->name, "Text") == 0);
                CHECK(strcmp(pattern->parts[1].pattern->type->name, "EntryKind") == 0);
                CHECK(strcmp(pattern->parts[1].pattern->constructor.builtin, "File") == 0);
                match = true;
            }
            if (row->native != NULL && strcmp(row->native, "Entry.name") == 0) {
                CHECK(row->field == 0 && row->operand_count == 1);
                field = true;
            }
        }
        CHECK(match && field);
    }
    free_fixture(&f);
}

static void ability_graph(void) {
    struct fixture f;
    init(&f, "module Main\nstruct Pair\nfirst: Int\nsecond: Text\nend\n"
             "fn permitted(_keys: Set(Pair), _ints: Set(Int))\nend\n"
             "fn main() : Int\na = Pair(first: 1, second: \"a\")\n"
             "b = Pair(first: 2, second: \"b\")\nif a < b: 7 else: 9\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_body *body = &f.program->functions[1].body;
        bool ordered = false;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            if (row->operation != MO_T_LT)
                continue;
            MO_ASSERT(row->ability_count == 1);
            const struct mo_ability_implementation *proof = row->abilities[0].implementation;
            CHECK(proof->kind == MO_ABILITY_DERIVED && proof->ability == MO_SOURCE_ORDER);
            MO_ASSERT(proof->part_count == 2);
            CHECK(strcmp(proof->parts[0]->type->name, "Int") == 0);
            CHECK(strcmp(proof->parts[1]->type->name, "Text") == 0);
            ordered = true;
        }
        CHECK(ordered);
    }
    free_fixture(&f);
}

static void native_choice_abilities(void) {
    struct fixture f;
    init(&f, "module Main\nfn main() : Int\n"
             "_entry = Entry(name: \"a\", kind: File).to_text\n"
             "_message = NotFound(path: \"a\").to_text\n"
             "if NotFound(path: \"a\") == Failed(path: \"a\", reason: \"b\"): 1 else: 0\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_body *body = &f.program->functions[0].body;
        bool entry = false, error_text = false, error_equal = false;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            for (uint32_t j = 0; j < row->ability_count; j++) {
                const struct mo_ability_implementation *proof = row->abilities[j].implementation;
                if (strcmp(proof->type->name, "Entry") == 0) {
                    CHECK(proof->kind == MO_ABILITY_DERIVED && proof->ability == MO_SOURCE_TEXT);
                    MO_ASSERT(proof->part_count == 2);
                    CHECK(strcmp(proof->parts[0]->type->name, "Text") == 0);
                    CHECK(strcmp(proof->parts[1]->type->name, "EntryKind") == 0);
                    entry = true;
                }
                if (strcmp(proof->type->name, "FileError") == 0) {
                    CHECK(proof->kind == MO_ABILITY_DERIVED &&
                          (proof->ability == MO_SOURCE_TEXT || proof->ability == MO_SOURCE_EQUAL));
                    MO_ASSERT(proof->part_count == 5);
                    for (uint32_t k = 0; k < proof->part_count; k++)
                        CHECK(strcmp(proof->parts[k]->type->name, "Text") == 0);
                    if (proof->ability == MO_SOURCE_TEXT)
                        error_text = true;
                    if (proof->ability == MO_SOURCE_EQUAL)
                        error_equal = true;
                }
            }
        }
        CHECK(entry && error_text && error_equal);
    }
    free_fixture(&f);
}

static void generic_graph(void) {
    struct fixture f;
    init(&f, "module Main\nfn add(a: T, b: T) : T where T: Number\na + b\nend\n"
             "fn main() : Int\nadd(3, 4)\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_body *generic = &f.program->functions[0].body;
        bool abstract = false, instantiated = false;
        const struct mo_source_type *parameter =
            f.program->functions[0].signature->type->arguments[0];
        for (uint32_t i = 0; i < generic->count; i++) {
            const struct mo_checked_expression *row = &generic->items[i];
            if (row->operation != MO_T_PLUS)
                continue;
            MO_ASSERT(row->ability_count == 1);
            CHECK(row->abilities[0].implementation->kind == MO_ABILITY_PARAMETER);
            CHECK(row->abilities[0].implementation->type == parameter);
            abstract = true;
        }
        const struct mo_checked_body *body = &f.program->functions[1].body;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            if (row->syntax->kind != MO_S_CALL)
                continue;
            MO_ASSERT(row->arguments.count == 1 && row->ability_count == 1);
            CHECK(row->arguments.items[0].parameter == parameter);
            CHECK(strcmp(row->arguments.items[0].type->name, "Int") == 0);
            CHECK(row->abilities[0].operand == UINT32_MAX);
            CHECK(row->abilities[0].parameter == parameter);
            CHECK(row->abilities[0].implementation->ability == MO_SOURCE_NUMBER);
            CHECK(row->abilities[0].implementation->kind == MO_ABILITY_NATIVE);
            instantiated = true;
        }
        CHECK(abstract && instantiated);
    }
    free_fixture(&f);
    init(&f, "module Main\nfn same(value: T, again?: Bool) : T\n"
             "if again?: same(value, false) else: value\nend\n"
             "fn main() : Int\nsame(7, true)\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    free_fixture(&f);
    init(&f, "module Main\nfn a(value: T)\nb([value])\nend\n"
             "fn b(_value: U)\na(0)\nend\nfn main()\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    free_fixture(&f);
    init(&f, "module Main\nstruct Box(T)\nvalue: T\nend\n"
             "fn compare(a: Box(T), b: Box(T)) : Ordering where T: Order\n"
             "if a.value < b.value: Less else: Greater\nend\n"
             "fn main() : Int\nif Box(value: 1) < Box(value: 2): 3 else: 4\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_source_type *parameter =
            f.program->functions[0].signature->type->arguments[0]->arguments[0];
        const struct mo_checked_body *body = &f.program->functions[1].body;
        bool customized = false;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            if (row->operation != MO_T_LT)
                continue;
            MO_ASSERT(row->ability_count == 1);
            const struct mo_ability_implementation *proof = row->abilities[0].implementation;
            CHECK(proof->kind == MO_ABILITY_FUNCTION);
            CHECK(proof->function == f.program->functions[0].signature->function);
            MO_ASSERT(proof->arguments.count == 1 && proof->part_count == 2);
            CHECK(proof->arguments.items[0].parameter == parameter);
            CHECK(proof->part_parameters[0] == parameter && proof->part_parameters[1] == parameter);
            CHECK(proof->parts[0]->ability == MO_SOURCE_EQUAL &&
                  proof->parts[1]->ability == MO_SOURCE_ORDER);
            CHECK(strcmp(proof->parts[0]->type->name, "Int") == 0 &&
                  strcmp(proof->parts[1]->type->name, "Int") == 0);
            customized = true;
        }
        CHECK(customized);
    }
    free_fixture(&f);
}

static void requirement_keys(void) {
    struct fixture f;
    init(&f, "module Main\nstruct Bag(T)\nvalues: Set(T)\nend\n"
             "fn same(a: A, b: B) : Bool where A: Equal, B: Equal\na == a and b == b\nend\n"
             "fn main() : Int\nbag = Bag(values: Set.empty.add(1))\ncase bag\n"
             "Bag(values: _): if same(1, 2): 3 else: 4\nend\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_source_type *a = f.program->functions[0].signature->type->arguments[0];
        const struct mo_source_type *b = f.program->functions[0].signature->type->arguments[1];
        const struct mo_checked_body *body = &f.program->functions[1].body;
        bool called = false, constructed = false, patterned = false;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            if (row->symbol == f.program->functions[0].signature->function &&
                row->syntax->kind == MO_S_CALL) {
                MO_ASSERT(row->ability_count == 2);
                CHECK(row->abilities[0].parameter == a && row->abilities[1].parameter == b);
                CHECK(row->abilities[0].operand == UINT32_MAX &&
                      row->abilities[1].operand == UINT32_MAX);
                CHECK(row->abilities[0].implementation == row->abilities[1].implementation);
                called = true;
            }
            if (row->symbol != NULL && strcmp(row->symbol->name, "Bag") == 0) {
                MO_ASSERT(row->ability_count == 2 && row->arguments.count == 1);
                CHECK(row->abilities[0].parameter == row->arguments.items[0].parameter);
                CHECK(row->abilities[1].parameter == row->arguments.items[0].parameter);
                constructed = true;
            }
            if (row->match != NULL) {
                const struct mo_checked_pattern *pattern = row->match->arms[0].patterns[0];
                MO_ASSERT(pattern->ability_count == 2 && pattern->arguments.count == 1);
                CHECK(pattern->abilities[0].parameter == pattern->arguments.items[0].parameter);
                CHECK(pattern->abilities[1].parameter == pattern->arguments.items[0].parameter);
                CHECK(pattern->abilities[1].implementation->ability == MO_SOURCE_HASH);
                patterned = true;
            }
        }
        CHECK(called && constructed && patterned);
    }
    free_fixture(&f);
    init(&f, "module Main\nstruct Box(T)\nvalue: T\nend\n"
             "fn to_text(_box: Box(T)) : Text where T: Text\n\"box\"\nend\n"
             "fn supported?(_value: T) : Bool where T: Text\ntrue\nend\n"
             "fn main() : Int\nif supported?(Box(value: Box(value: 1))): 1 else: 0\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_body *body = &f.program->functions[2].body;
        bool nested = false;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            if (row->syntax->kind != MO_S_CALL ||
                row->symbol != f.program->functions[1].signature->function)
                continue;
            MO_ASSERT(row->ability_count == 1);
            const struct mo_ability_implementation *outer = row->abilities[0].implementation;
            MO_ASSERT(outer->part_count == 1 && outer->parts[0]->part_count == 1);
            const struct mo_ability_implementation *inner = outer->parts[0];
            CHECK(outer->kind == MO_ABILITY_FUNCTION && inner->kind == MO_ABILITY_FUNCTION);
            CHECK(outer->function == f.program->functions[0].signature->function);
            CHECK(outer->part_parameters[0] == outer->arguments.items[0].parameter);
            CHECK(inner->part_parameters[0] == inner->arguments.items[0].parameter);
            CHECK(inner->parts[0]->ability == MO_SOURCE_TEXT &&
                  strcmp(inner->parts[0]->type->name, "Int") == 0);
            nested = true;
        }
        CHECK(nested);
    }
    free_fixture(&f);
    init(&f, "module Main\nstruct Pair(Z, A)\nfirst: Z\nsecond: A\nend\n"
             "fn to_text(pair: Pair(U, T)) : Text where U: Text, T: Text\n"
             "\"#{pair.first} #{pair.second}\"\nend\n"
             "fn main() : Int\nPair(first: true, second: 7).to_text.byte_count\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_body *body = &f.program->functions[1].body;
        bool customized = false;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            if (row->native == NULL || strcmp(row->native, "to_text") != 0)
                continue;
            MO_ASSERT(row->ability_count == 1);
            const struct mo_ability_implementation *proof = row->abilities[0].implementation;
            MO_ASSERT(proof->kind == MO_ABILITY_FUNCTION && proof->arguments.count == 2 &&
                      proof->part_count == 2);
            CHECK(strcmp(proof->arguments.items[0].parameter->name, "T") == 0);
            CHECK(strcmp(proof->arguments.items[0].type->name, "Int") == 0);
            CHECK(strcmp(proof->arguments.items[1].parameter->name, "U") == 0);
            CHECK(strcmp(proof->arguments.items[1].type->name, "Bool") == 0);
            for (uint32_t j = 0; j < proof->part_count; j++) {
                const char *expected =
                    strcmp(proof->part_parameters[j]->name, "T") == 0 ? "Int" : "Bool";
                CHECK(proof->parts[j]->ability == MO_SOURCE_TEXT);
                CHECK(strcmp(proof->parts[j]->type->name, expected) == 0);
            }
            customized = true;
        }
        CHECK(customized);
    }
    free_fixture(&f);
    init(&f, "module Main\nstruct Tree(T)\nchildren: List(Tree(List(T)))\nend\n"
             "struct Phantom(T)\nvalue: Int\nend\n"
             "fn accepts(_key: Map(Phantom(Float), Int), _tree: Tree(Int))\nend\n"
             "fn main()\nend\n");
    CHECK(f.program == NULL && f.diags.count == 1);
    if (f.diags.count == 1)
        CHECK(f.diags.items[0].code == MO_CODE_DATA_RECURSION);
    free_fixture(&f);
}

static void data_recursion(void) {
    static const struct {
        const char *source;
        bool grows;
    } cases[] = {
        {"struct Tree(T)\nchildren: List(Tree(T))\nend\n", false},
        {"struct Tree(T, U)\nnext: Option(Tree(U, T))\nend\n", false},
        {"struct A(T)\nnext: Option(B(List(T)))\nend\n"
         "struct B(U)\nnext: Option(A(Int))\nend\n",
         false},
        {"struct A(T)\nnext: Option(B(List(T)))\nend\n"
         "struct B(U)\nnext: Option(A(U))\nend\n",
         true},
        {"enum Tree(T)\nLeaf(T)\nBranch(List(Tree(List(T))))\nend\n", true},
        {"struct Tree(T)\nnext: fn() : Tree(List(T))\nend\n", true},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct mo_buffer source;
        mo_buffer_init(&source, 4096);
        MO_ASSERT(mo_buffer_append_text(&source, "module Main\n") &&
                  mo_buffer_append_text(&source, cases[i].source) &&
                  mo_buffer_append_text(&source, "fn main()\nend\n") &&
                  mo_buffer_append_byte(&source, 0));
        struct fixture f;
        init(&f, (const char *)source.bytes);
        CHECK((f.program == NULL) == cases[i].grows);
        if (cases[i].grows) {
            CHECK(f.diags.count == 1);
            if (f.diags.count == 1) {
                CHECK(f.diags.items[0].code == MO_CODE_DATA_RECURSION);
                struct mo_span span = f.diags.items[0].primary.span;
                CHECK(span.start > 12 && span.end > span.start &&
                      span.end < strlen((const char *)source.bytes));
            }
        } else
            CHECK(!mo_diags_any(&f.diags));
        free_fixture(&f);
        mo_buffer_free(&source);
    }
}

static void generic_lambdas(void) {
    struct fixture f;
    init(&f, "module Main\nfn twice(x: T) : T where T: Number\n"
             "f = fn(c) c + c end\nf(x)\nend\n"
             "fn main() : Int\ntwice(3)\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    free_fixture(&f);
    init(&f, "module Main\nstruct Box(T)\nvalue: T\nend\n"
             "fn twice(x: Box(T)) : T where T: Number\n"
             "f = fn(c) c.value + c.value end\nf(x)\nend\n"
             "fn negative(x: T) : T where T: Number\nf = fn(c) -c end\nf(x)\nend\n"
             "fn main() : Int\nnegative(twice(Box(value: 3)))\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    free_fixture(&f);
    init(&f, "module Main\nfn twice(x: T) : T\n"
             "f = fn(c) c + c end\nf(x)\nend\nfn main()\nend\n");
    CHECK(f.program == NULL && f.diags.count != 0);
    free_fixture(&f);
}

static void option_or_graph(void) {
    struct fixture f;
    init(&f, "module Main\nfn main() : Int\nSome(17) or 3\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_body *body = &f.program->functions[0].body;
        bool found = false;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            if (row->operation != MO_T_OR)
                continue;
            MO_ASSERT(row->operand_count == 2);
            CHECK(row->or_kind == MO_CHECK_OR_OPTION);
            CHECK(strcmp(row->type->name, "Int") == 0);
            CHECK(strcmp(body->items[row->operands[0]].type->name, "Option") == 0);
            CHECK(body->items[row->operands[1]].syntax->kind == MO_S_INT &&
                  body->items[row->operands[1]].syntax->text[0] == '3');
            CHECK(!row->never && row->ability_count == 0);
            found = true;
        }
        CHECK(found);
    }
    free_fixture(&f);
}

static void deferred_defaults(void) {
    struct fixture f;
    init(&f, "module Main\nfn choose() : Bool\n"
             "Some(Some(false)) or Some(true) or false or true\nend\n"
             "fn main() : Int\nreader = fn(values) values.get(0) or 17 end\n"
             "reader([3]) + (Some(8) or stop \"missing\")\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_body *choose = &f.program->functions[0].body;
        uint32_t options = 0, bools = 0;
        for (uint32_t i = 0; i < choose->count; i++) {
            const struct mo_checked_expression *row = &choose->items[i];
            if (row->operation != MO_T_OR)
                continue;
            if (row->or_kind == MO_CHECK_OR_OPTION) {
                CHECK(strcmp(row->type->name, options == 0 ? "Option" : "Bool") == 0);
                options++;
            } else {
                CHECK(row->or_kind == MO_CHECK_OR_BOOL && strcmp(row->type->name, "Bool") == 0);
                bools++;
            }
            CHECK(!row->never);
        }
        CHECK(options == 2 && bools == 1);
        const struct mo_checked_body *main = &f.program->functions[1].body;
        uint32_t defaults = 0;
        bool lazy = false;
        for (uint32_t i = 0; i < main->count; i++) {
            const struct mo_checked_expression *row = &main->items[i];
            if (row->operation != MO_T_OR)
                continue;
            MO_ASSERT(row->operand_count == 2);
            CHECK(row->or_kind == MO_CHECK_OR_OPTION && strcmp(row->type->name, "Int") == 0);
            CHECK(!row->never);
            lazy = lazy || main->items[row->operands[1]].never;
            defaults++;
        }
        CHECK(defaults == 2 && lazy);
    }
    free_fixture(&f);
}

static void try_graph(void) {
    struct fixture f;
    init(&f, "module Main\nfn next(value: Option(Int)) : Option(Int)\n"
             "Some((try value) + 1)\nend\n"
             "fn main() : Int\nnext(Some(4)) or 9\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_function *fn = &f.program->functions[0];
        bool found = false;
        for (uint32_t i = 0; i < fn->body.count; i++) {
            const struct mo_checked_expression *row = &fn->body.items[i];
            if (row->try_ == NULL)
                continue;
            MO_ASSERT(row->operand_count == 1);
            CHECK(strcmp(row->type->name, "Int") == 0 && !row->never);
            CHECK(strcmp(fn->body.items[row->operands[0]].type->name, "Option") == 0);
            CHECK(strcmp(row->try_->result->name, "Option") == 0 && row->try_->result->count == 1 &&
                  strcmp(row->try_->result->arguments[0]->name, "Int") == 0);
            CHECK(row->try_->failure.builtin != NULL &&
                  strcmp(row->try_->failure.builtin, "None") == 0);
            CHECK(row->try_->wrapper.symbol == NULL && row->try_->wrapper.builtin == NULL);
            CHECK(row->try_->arguments.count == 0);
            found = true;
        }
        CHECK(found);
    }
    free_fixture(&f);
    init(&f, "module Main\nenum Inner(T)\nBadInner(T)\nend\n"
             "enum Outer(U)\nWrapped(Inner(U))\nDetail(name: Text)\nend\n"
             "fn lift(value: Result(Int, Inner(T))) : Result(Text, Outer(T))\n"
             "number = try value\nOk(number.to_text)\nend\n"
             "fn main() : Int\ncase lift(Error(BadInner(true)))\n"
             "Ok(text): text.byte_count\nError(Wrapped(BadInner(_))): 2\n"
             "Error(Detail(name: _)): 3\nend\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_function *fn = &f.program->functions[0];
        const struct mo_source_type *parameter =
            fn->signature->type->arguments[0]->arguments[1]->arguments[0];
        bool wrapped = false;
        for (uint32_t i = 0; i < fn->body.count; i++) {
            const struct mo_checked_try *try_ = fn->body.items[i].try_;
            if (try_ == NULL)
                continue;
            CHECK(try_->failure.builtin != NULL && strcmp(try_->failure.builtin, "Error") == 0);
            MO_ASSERT(try_->wrapper.symbol != NULL && try_->arguments.count == 1);
            CHECK(strcmp(try_->wrapper.symbol->name, "Wrapped") == 0);
            CHECK(strcmp(try_->arguments.items[0].parameter->declaration->name, "Outer") == 0);
            CHECK(try_->arguments.items[0].type == parameter);
            CHECK(strcmp(try_->result->arguments[0]->name, "Text") == 0);
            wrapped = true;
        }
        CHECK(wrapped);
    }
    free_fixture(&f);
    init(&f, "module Main\nenum Outer(T)\nWrapped(Set(T))\nend\n"
             "fn lift(value: Result(Int, Set(T))) : Result(Text, Outer(T)) where T: Hash\n"
             "number = try value\nOk(number.to_text)\nend\nfn main()\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_function *fn = &f.program->functions[0];
        const struct mo_source_type *parameter =
            fn->signature->type->arguments[0]->arguments[1]->arguments[0];
        bool wrapped = false;
        for (uint32_t i = 0; i < fn->body.count; i++) {
            const struct mo_checked_try *try_ = fn->body.items[i].try_;
            if (try_ == NULL)
                continue;
            MO_ASSERT(try_->wrapper.symbol != NULL && try_->arguments.count == 1);
            CHECK(strcmp(try_->wrapper.symbol->name, "Wrapped") == 0);
            CHECK(try_->arguments.items[0].type == parameter);
            CHECK(try_->ability_count == 2);
            for (uint32_t j = 0; j < try_->ability_count; j++) {
                const struct mo_checked_ability_use *use = &try_->abilities[j];
                CHECK(use->operand == UINT32_MAX &&
                      use->parameter == try_->arguments.items[0].parameter);
                CHECK(use->implementation->kind == MO_ABILITY_PARAMETER &&
                      use->implementation->type == parameter);
                CHECK(use->implementation->ability == MO_SOURCE_EQUAL ||
                      use->implementation->ability == MO_SOURCE_HASH);
            }
            wrapped = true;
        }
        CHECK(wrapped);
    }
    free_fixture(&f);
    init(&f, "module Main\nfn negate(value: Option(T)) : Option(T) where T: Number\n"
             "Some(- try value)\nend\nfn main()\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_body *body = &f.program->functions[0].body;
        bool negated = false;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            if (row->operation != MO_T_MINUS)
                continue;
            MO_ASSERT(row->operand_count == 1 && row->ability_count == 1);
            CHECK(row->type->kind == MO_SOURCE_PARAMETER);
            CHECK(row->abilities[0].implementation->kind == MO_ABILITY_PARAMETER &&
                  row->abilities[0].implementation->ability == MO_SOURCE_NUMBER);
            CHECK(body->items[row->operands[0]].try_ != NULL);
            negated = true;
        }
        CHECK(negated);
    }
    free_fixture(&f);
}

static void inferred_wrappers(void) {
    struct fixture f;
    init(&f, "module Main\nenum Outer\nWrong((Option(Int), Option(Bool)))\n"
             "Wrapped((Option(Int), Option(Int)))\nScalar(Int)\nend\n"
             "fn helper() : Result(Int, Outer)\nempty = None\nOk(try Error((empty, empty)))\n"
             "end\nfn scalar() : Result(Int, Outer)\nOk(try Error(17))\nend\n"
             "fn main()\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_body *body = &f.program->functions[0].body;
        bool wrapped = false, inferred = false;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            if (row->native != NULL && strcmp(row->native, "None") == 0) {
                CHECK(strcmp(row->type->arguments[0]->name, "Int") == 0);
                inferred = true;
            }
            if (row->try_ != NULL) {
                MO_ASSERT(row->try_->wrapper.symbol != NULL);
                CHECK(strcmp(row->try_->wrapper.symbol->name, "Wrapped") == 0);
                wrapped = true;
            }
        }
        CHECK(wrapped && inferred);
        const struct mo_checked_body *scalar = &f.program->functions[1].body;
        bool numbered = false;
        for (uint32_t i = 0; i < scalar->count; i++) {
            if (scalar->items[i].try_ == NULL)
                continue;
            MO_ASSERT(scalar->items[i].try_->wrapper.symbol != NULL);
            CHECK(strcmp(scalar->items[i].try_->wrapper.symbol->name, "Scalar") == 0);
            numbered = true;
        }
        CHECK(numbered);
    }
    free_fixture(&f);
}

static void try_contexts(void) {
    struct fixture f;
    init(&f, "module Main\nfn direct(value: Result(Int, Text)) : Result(Text, Text)\n"
             "n = try value\nOk(n.to_text)\nend\n"
             "fn builtin(value: Result(Int, Int)) : Result(Text, Option(Int))\n"
             "n = try value\nOk(n.to_text)\nend\n"
             "fn nested(value: Option(Option(Int))) : Option(Int)\nSome(try try value)\nend\n"
             "fn stopped() : Option(Int)\nSome(try stop \"unreachable\")\nend\n"
             "fn main() : Int\nf = fn(value) Some((try value).to_text) end\n"
             "(f(Some(12)) or \"\").byte_count\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        uint32_t tries = 0;
        for (uint32_t j = 0; j < f.program->function_count; j++) {
            const struct mo_checked_body *body = &f.program->functions[j].body;
            for (uint32_t i = 0; i < body->count; i++) {
                const struct mo_checked_expression *row = &body->items[i];
                if (row->try_ == NULL)
                    continue;
                const struct mo_checked_try *try_ = row->try_;
                MO_ASSERT(row->operand_count == 1 && row->operands[0] < i);
                CHECK(row->never == body->items[row->operands[0]].never);
                if (j == 0) {
                    CHECK(strcmp(row->type->name, "Int") == 0);
                    CHECK(strcmp(try_->result->arguments[0]->name, "Text") == 0);
                    CHECK(try_->wrapper.symbol == NULL && try_->wrapper.builtin == NULL);
                } else if (j == 1) {
                    MO_ASSERT(try_->wrapper.builtin != NULL && try_->arguments.count == 1);
                    CHECK(strcmp(try_->wrapper.builtin, "Some") == 0);
                    CHECK(strcmp(try_->arguments.items[0].parameter->declaration->name, "Option") ==
                          0);
                    CHECK(strcmp(try_->arguments.items[0].type->name, "Int") == 0);
                } else {
                    CHECK(strcmp(try_->failure.builtin, "None") == 0);
                    CHECK(strcmp(try_->result->name, "Option") == 0);
                    CHECK(try_->wrapper.symbol == NULL && try_->wrapper.builtin == NULL);
                    CHECK(strcmp(try_->result->arguments[0]->name, j == 4 ? "Text" : "Int") == 0);
                    CHECK(row->never == (j == 3));
                }
                tries++;
            }
        }
        CHECK(tries == 6);
    }
    free_fixture(&f);
}

static void lambda_graph(void) {
    struct fixture f;
    init(&f, "module Main\n"
             "fn compose(f: fn(Int) : Int, g: fn(Int) : Int, value: Int) : Int\n"
             "f(g(value))\nend\n"
             "fn main() : Int\ncompose(fn(x) x + 1 end, fn(y) y * 2 end, 20)\nend\n");
    const struct mo_program *program = f.program;
    CHECK(program != NULL && !mo_diags_any(&f.diags));
    if (program != NULL) {
        const struct mo_checked_body *body = &program->functions[program->entry.function].body;
        uint32_t count = 0;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            if (row->lambda == NULL)
                continue;
            const struct mo_checked_lambda *lambda = row->lambda;
            count++;
            CHECK(lambda->number == count && lambda->capture_count == 0);
            MO_ASSERT(lambda->parameter_count == 1 && lambda->body < i);
            CHECK(row->operand_count == 1 && row->operands[0] == lambda->body);
            CHECK(strcmp(lambda->parameters[0]->name, count == 1 ? "x" : "y") == 0);
            CHECK(strcmp(lambda->parameters[0]->type->name, "Int") == 0);
            CHECK(row->type->kind == MO_SOURCE_FUNCTION && row->type->count == 2);
            CHECK(strcmp(row->type->arguments[1]->name, "Int") == 0);
            bool read = false;
            for (uint32_t j = 0; j < lambda->body; j++)
                read = read || body->items[j].binding == lambda->parameters[0];
            CHECK(read);
        }
        CHECK(count == 2);
    }
    free_fixture(&f);
}

static MO_MUST_USE const struct mo_checked_lambda *
checked_lambda(const struct mo_checked_body *body, uint32_t number) {
    for (uint32_t i = 0; i < body->count; i++)
        if (body->items[i].lambda != NULL && body->items[i].lambda->number == number)
            return body->items[i].lambda;
    MO_ASSERT(false);
    return NULL;
}

static void capture_graph(void) {
    struct fixture f;
    init(&f, "module Main\n"
             "fn invoke(f: fn() : Int, extra: Int) : Int\nf() + extra\nend\n"
             "fn make(left: Int, right: Int) : fn() : Int\n"
             "fn() invoke(fn() right + left + right end, left) end\nend\n"
             "fn main() : Int\nmake(7, 23)()\nend\n");
    CHECK(f.program != NULL && f.diags.count == 0);
    if (f.program != NULL) {
        const struct mo_checked_function *make = &f.program->functions[1];
        for (uint32_t i = 1; i <= 2; i++) {
            const struct mo_checked_lambda *lambda = checked_lambda(&make->body, i);
            CHECK(lambda->capture_count == 2);
            if (lambda->capture_count == 2) {
                CHECK(lambda->captures[0] == make->parameters[1]);
                CHECK(lambda->captures[1] == make->parameters[0]);
            }
        }
    }
    free_fixture(&f);

    init(&f, "module Main\n"
             "fn make(left: Int, right: Int) : fn(Int) : fn() : Int\n"
             "fn(shift)\ntemp = left * 7\n"
             "fn() right + temp + shift + left + right end\nend\nend\n"
             "fn main() : Int\nmake(3, 29)(5)()\nend\n");
    CHECK(f.program != NULL && f.diags.count == 0);
    if (f.program != NULL) {
        const struct mo_checked_function *make = &f.program->functions[0];
        const struct mo_checked_lambda *outer = checked_lambda(&make->body, 1);
        const struct mo_checked_lambda *inner = checked_lambda(&make->body, 2);
        CHECK(outer->capture_count == 2 && inner->capture_count == 4);
        if (outer->capture_count == 2 && inner->capture_count == 4) {
            CHECK(outer->captures[0] == make->parameters[0]);
            CHECK(outer->captures[1] == make->parameters[1]);
            CHECK(inner->captures[0] == make->parameters[1]);
            CHECK(strcmp(inner->captures[1]->name, "temp") == 0);
            CHECK(inner->captures[2] == outer->parameters[0]);
            CHECK(inner->captures[3] == make->parameters[0]);
        }
    }
    free_fixture(&f);

    init(&f, "module Main\nfn select(flag: Bool) : fn() : Int\n"
             "if flag\nitem = 13\nfn() item end\n"
             "else\nitem = 29\nfn() item end\nend\nend\n"
             "fn main() : Int\nselect(true)() - select(false)()\nend\n");
    CHECK(f.program != NULL && f.diags.count == 0);
    if (f.program != NULL) {
        const struct mo_checked_body *body = &f.program->functions[0].body;
        const struct mo_checked_lambda *first = checked_lambda(body, 1);
        const struct mo_checked_lambda *second = checked_lambda(body, 2);
        MO_ASSERT(first->capture_count == 1 && second->capture_count == 1);
        CHECK(first->captures[0] != second->captures[0]);
        CHECK(strcmp(first->captures[0]->name, "item") == 0 &&
              strcmp(second->captures[0]->name, "item") == 0);
        CHECK(first->captures[0]->syntax->span.start < second->captures[0]->syntax->span.start);
    }
    free_fixture(&f);

    init(&f, "module Main\nfn main() : Int\nvar count = 7\n"
             "current = count\nf = fn() current end\ncount += 19\n"
             "g = fn() count end\nf() * 100 + g()\nend\n");
    CHECK(f.program != NULL && f.diags.count == 0);
    if (f.program != NULL) {
        const struct mo_checked_body *body = &f.program->functions[f.program->entry.function].body;
        const struct mo_checked_lambda *copied = checked_lambda(body, 1);
        const struct mo_checked_lambda *stable = checked_lambda(body, 2);
        MO_ASSERT(copied->capture_count == 1 && stable->capture_count == 1);
        CHECK(strcmp(copied->captures[0]->name, "current") == 0 && !copied->captures[0]->variable);
        CHECK(strcmp(stable->captures[0]->name, "count") == 0 && stable->captures[0]->variable);
    }
    free_fixture(&f);
}

static void capture_work(void) {
    static const uint32_t counts[] = {6000, 9000};
    for (uint32_t kind = 0; kind < 2; kind++) {
        struct mo_buffer source;
        mo_buffer_init(&source, MO_LIMIT_FILE_SIZE);
        MO_ASSERT(mo_buffer_append_text(&source, "module Main\nfn main()\nvar count = 0\n"));
        for (uint32_t i = 0; i < 2000; i++)
            MO_ASSERT(mo_buffer_append_text(&source, "count += 1\n"));
        for (uint32_t i = 0; i < counts[kind]; i++)
            MO_ASSERT(mo_buffer_append_text(&source, "_f") && mo_buffer_append_uint(&source, i) &&
                      mo_buffer_append_text(&source, " = fn() count end\n"));
        MO_ASSERT(mo_buffer_append_text(&source, "end\n") && mo_buffer_append_byte(&source, 0));
        struct fixture f;
        init(&f, (const char *)source.bytes);
        if (kind == 0)
            CHECK(f.program != NULL && f.diags.count == 0);
        else
            CHECK(f.program == NULL && f.diags.count == 1 &&
                  f.diags.items[0].code == MO_CODE_CHECK_WORK);
        free_fixture(&f);
        mo_buffer_free(&source);
    }
}

static void branch_graph(void) {
    struct fixture f;
    init(&f, "module Main\nfn choose(flag: Bool) : Int\n"
             "if flag\nreturn 7\nelse if not flag\nreturn 9\nelse\nstop \"unreachable\"\nend\n"
             "end\nfn main() : Int\nchoose(false)\nend\n");
    const struct mo_program *program = f.program;
    CHECK(program != NULL && !mo_diags_any(&f.diags));
    if (program != NULL) {
        const struct mo_checked_body *body = &program->functions[0].body;
        bool branched = false;
        uint32_t returns = 0, stops = 0;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            for (uint32_t j = 0; j < row->operand_count; j++)
                CHECK(row->operands[j] < i);
            if (row->syntax->kind == MO_S_RETURN) {
                MO_ASSERT(row->operand_count == 1);
                CHECK(row->never);
                const struct mo_syntax *value = body->items[row->operands[0]].syntax;
                CHECK(value->kind == MO_S_INT && value->text[0] == (returns == 0 ? '7' : '9'));
                returns++;
            } else if (row->syntax->kind == MO_S_STOP) {
                MO_ASSERT(row->operand_count == 1);
                CHECK(row->never && body->items[row->operands[0]].syntax->kind == MO_S_TEXT);
                stops++;
            } else if (row->syntax->kind == MO_S_IF) {
                MO_ASSERT(row->operand_count == 5);
                CHECK(row->never && strcmp(row->type->name, "Int") == 0);
                CHECK(strcmp(body->items[row->operands[0]].type->name, "Bool") == 0);
                CHECK(strcmp(body->items[row->operands[2]].type->name, "Bool") == 0);
                CHECK(body->items[row->operands[4]].never);
                branched = true;
            }
        }
        CHECK(branched && returns == 2 && stops == 1);
    }
    free_fixture(&f);
}

static void record_graph(void) {
    struct fixture f;
    init(&f, "module Main\nstruct Pair(T)\nleft: T\nright: T\nend\n"
             "fn main() : Int\nvar pair = Pair(right: 9, left: 4)\npair.left = 7\n"
             "pair.left + pair.right\nend\n");
    const struct mo_program *program = f.program;
    CHECK(program != NULL && !mo_diags_any(&f.diags));
    if (program != NULL) {
        const struct mo_checked_body *body = &program->functions[program->entry.function].body;
        bool constructed = false, assigned = false;
        uint32_t reads = 0;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            if (row->symbol != NULL && row->symbol->declaration->kind == MO_S_STRUCT) {
                CHECK(strcmp(row->symbol->name, "Pair") == 0);
                MO_ASSERT(row->operand_count == 2 && row->operands[0] < i && row->operands[1] < i);
                CHECK(row->arguments.count == 1);
                if (row->arguments.count == 1) {
                    CHECK(strcmp(row->arguments.items[0].parameter->name, "T") == 0);
                    CHECK(row->arguments.items[0].parameter->declaration == row->symbol);
                    CHECK(strcmp(row->arguments.items[0].type->name, "Int") == 0);
                }
                CHECK(body->items[row->operands[0]].field == 1);
                CHECK(body->items[row->operands[1]].field == 0);
                const struct mo_checked_expression *right = &body->items[row->operands[0]];
                MO_ASSERT(right->operand_count == 1);
                CHECK(body->items[right->operands[0]].syntax->text[0] == '9');
                constructed = true;
            } else if (row->syntax->kind == MO_S_FIELD_ACCESS) {
                MO_ASSERT(row->operand_count == 1 && row->operands[0] < i);
                CHECK(strcmp(row->type->name, "Int") == 0);
                CHECK(row->field == (reads == 2 ? 1u : 0u));
                reads++;
            } else if (row->assignment) {
                MO_ASSERT(row->binding != NULL && row->operand_count == 2);
                CHECK(strcmp(row->binding->name, "pair") == 0 && row->binding->variable);
                CHECK(body->items[row->operands[0]].field == 0);
                CHECK(body->items[row->operands[1]].syntax->text[0] == '7');
                assigned = true;
            }
        }
        CHECK(constructed && assigned && reads == 3);
    }
    free_fixture(&f);
}

static void match_graph(void) {
    struct fixture f;
    init(&f, "module Main\nstruct Pair\nleft: Int\nright: Int\nend\n"
             "enum Choice\nValue(Pair)\nEmpty\nend\n"
             "fn choose(value: Choice) : Int\ncase value\n"
             "Value(Pair(left: n)): n\nEmpty: 0\nend\nend\n"
             "fn main() : Int\nchoose(Value(Pair(right: 9, left: 4)))\nend\n");
    const struct mo_program *program = f.program;
    CHECK(program != NULL && !mo_diags_any(&f.diags));
    if (program != NULL) {
        const struct mo_checked_body *body = &program->functions[0].body;
        bool matched = false;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_match *match = body->items[i].match;
            if (match == NULL)
                continue;
            CHECK(match->scrutinee < i && match->arm_count == 2);
            CHECK(body->items[match->scrutinee].binding == program->functions[0].parameters[0]);
            MO_ASSERT(match->arm_count == 2 && match->arms[0].pattern_count == 1);
            const struct mo_checked_pattern *value = match->arms[0].patterns[0];
            CHECK(value->kind == MO_PATTERN_CONSTRUCTOR);
            CHECK(strcmp(value->constructor.symbol->name, "Value") == 0);
            MO_ASSERT(value->part_count == 1);
            const struct mo_checked_pattern *pair = value->parts[0].pattern;
            CHECK(pair->kind == MO_PATTERN_CONSTRUCTOR);
            CHECK(strcmp(pair->constructor.symbol->name, "Pair") == 0);
            MO_ASSERT(pair->part_count == 2);
            CHECK(pair->parts[0].index == 0 && pair->parts[1].index == 1);
            CHECK(pair->parts[1].pattern->kind == MO_PATTERN_ANY);
            const struct mo_checked_pattern *name = pair->parts[0].pattern;
            MO_ASSERT(name->kind == MO_PATTERN_BIND && name->binding != NULL);
            CHECK(strcmp(name->binding->name, "n") == 0 && strcmp(name->type->name, "Int") == 0);
            const struct mo_checked_arm *arm = &match->arms[0];
            CHECK(arm->guard == UINT32_MAX && arm->body < i);
            bool read = false;
            for (uint32_t j = match->scrutinee + 1; j < arm->body; j++)
                read = read || body->items[j].binding == name->binding;
            CHECK(read);
            CHECK(strcmp(match->arms[1].patterns[0]->constructor.symbol->name, "Empty") == 0);
            matched = true;
        }
        CHECK(matched);
    }
    free_fixture(&f);
}

static void loop_graph(void) {
    struct fixture f;
    init(&f, "module Main\nfn main() : Int\nvar total = 0\n"
             "for value in 2..5\ncontinue if value == 3\ntotal += value\nend\n"
             "while total < 20\nbreak if total == 18\ntotal = total + 2\nend\ntotal\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_body *body = &f.program->functions[0].body;
        uint32_t loops = 0, exits = 0;
        bool compound = false;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            if (row->syntax->kind == MO_S_ADD_ASSIGN && row->assignment) {
                MO_ASSERT(row->binding != NULL && row->operand_count == 1);
                const struct mo_checked_expression *sum = &body->items[row->operands[0]];
                MO_ASSERT(sum->operand_count == 2);
                CHECK(sum->operation == MO_T_PLUS);
                CHECK(body->items[sum->operands[0]].binding == row->binding);
                CHECK(strcmp(row->binding->name, "total") == 0);
                compound = true;
            } else if (row->syntax->kind == MO_S_FOR || row->syntax->kind == MO_S_WHILE) {
                loops++;
                MO_ASSERT(row->operand_count == 2 && row->operands[0] < i && row->operands[1] < i);
                CHECK(strcmp(row->type->name, "Unit") == 0 && !row->never);
                CHECK(body->items[row->operands[1]].syntax->kind == MO_S_SEQUENCE);
                if (row->syntax->kind == MO_S_FOR) {
                    CHECK(strcmp(body->items[row->operands[0]].type->name, "Range") == 0);
                    MO_ASSERT(row->binding != NULL && row->pattern != NULL);
                    CHECK(strcmp(row->binding->name, "value") == 0);
                    CHECK(strcmp(row->binding->type->name, "Int") == 0);
                    CHECK(row->pattern->kind == MO_PATTERN_BIND &&
                          row->pattern->binding == row->binding);
                    bool read = false;
                    for (uint32_t j = row->operands[0] + 1; j < row->operands[1]; j++)
                        read = read || body->items[j].binding == row->binding;
                    CHECK(read);
                } else {
                    CHECK(strcmp(body->items[row->operands[0]].type->name, "Bool") == 0);
                    CHECK(row->binding == NULL);
                }
            } else if (row->syntax->kind == MO_S_BREAK || row->syntax->kind == MO_S_CONTINUE) {
                exits++;
                MO_ASSERT(row->operand_count == 1 && row->operands[0] < i);
                CHECK(!row->never && strcmp(body->items[row->operands[0]].type->name, "Bool") == 0);
            }
        }
        CHECK(loops == 2 && exits == 2 && compound);
    }
    free_fixture(&f);
}

static void builtin_graph(void) {
    struct fixture f;
    init(&f, "module Main\nfn choose(value: Option(Int)) : Int\ncase value\n"
             "Some(n) | Some(n): n\nNone: 0\nend\nend\n"
             "fn main() : Int\nchoose(Some(42))\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_body *body = &f.program->functions[0].body;
        bool matched = false;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_match *match = body->items[i].match;
            if (match == NULL)
                continue;
            MO_ASSERT(match->arm_count == 2 && match->arms[0].pattern_count == 2);
            const struct mo_checked_pattern *a = match->arms[0].patterns[0];
            const struct mo_checked_pattern *b = match->arms[0].patterns[1];
            CHECK(strcmp(a->constructor.builtin, "Some") == 0);
            MO_ASSERT(a->part_count == 1 && b->part_count == 1 && a->arguments.count == 1);
            CHECK(a->parts[0].pattern->binding == b->parts[0].pattern->binding);
            CHECK(strcmp(a->parts[0].pattern->binding->name, "n") == 0);
            CHECK(strcmp(a->arguments.items[0].type->name, "Int") == 0);
            CHECK(strcmp(match->arms[1].patterns[0]->constructor.builtin, "None") == 0);
            const struct mo_checked_body *main = &f.program->functions[1].body;
            bool constructed = false;
            for (uint32_t j = 0; j < main->count; j++) {
                const struct mo_checked_expression *row = &main->items[j];
                if (row->native == NULL || strcmp(row->native, "Some") != 0)
                    continue;
                MO_ASSERT(row->operand_count == 1 && row->arguments.count == 1);
                CHECK(row->arguments.items[0].parameter == a->arguments.items[0].parameter);
                CHECK(strcmp(row->arguments.items[0].type->name, "Int") == 0);
                CHECK(main->items[row->operands[0]].syntax->kind == MO_S_INT);
                constructed = true;
            }
            CHECK(constructed);
            matched = true;
        }
        CHECK(matched);
    }
    free_fixture(&f);
}

static void collection_graph(void) {
    struct fixture f;
    init(&f, "module Main\nfn main() : Int\nvar total = 0\n"
             "for (number, include?) in [(4, true), (9, false)]\n"
             "if include?\ntotal = total + number\nend\nend\n"
             "pair = (total, 99)\n(first, _) = pair\nfirst + pair.1\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_body *body = &f.program->functions[0].body;
        bool list = false, loop = false, destructured = false, projected = false;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            if (row->syntax->kind == MO_S_LIST) {
                MO_ASSERT(row->operand_count == 2);
                CHECK(body->items[row->operands[0]].syntax->kind == MO_S_TUPLE);
                const struct mo_checked_expression *a = &body->items[row->operands[0]];
                const struct mo_checked_expression *b = &body->items[row->operands[1]];
                CHECK(body->items[a->operands[0]].syntax->text[0] == '4');
                CHECK(body->items[b->operands[0]].syntax->text[0] == '9');
                list = true;
            } else if (row->syntax->kind == MO_S_FOR) {
                MO_ASSERT(row->pattern != NULL && row->pattern->part_count == 2);
                CHECK(row->binding == NULL && row->pattern->kind == MO_PATTERN_TUPLE);
                CHECK(strcmp(row->pattern->parts[0].pattern->binding->name, "number") == 0);
                CHECK(strcmp(row->pattern->parts[1].pattern->binding->name, "include?") == 0);
                loop = true;
            } else if (row->syntax->kind == MO_S_BIND && row->pattern != NULL) {
                MO_ASSERT(row->pattern->part_count == 2 && row->operand_count == 1);
                CHECK(row->binding == NULL && !row->assignment);
                CHECK(strcmp(row->pattern->parts[0].pattern->binding->name, "first") == 0);
                CHECK(row->pattern->parts[1].pattern->kind == MO_PATTERN_ANY);
                destructured = true;
            } else if (row->syntax->kind == MO_S_POSITION && row->field == 1) {
                CHECK(row->operand_count == 1 && strcmp(row->type->name, "Int") == 0);
                projected = true;
            }
        }
        CHECK(list && loop && destructured && projected);
    }
    free_fixture(&f);
}

static void literal_patterns(void) {
    struct fixture f;
    init(&f, "module Main\nfn choose(x: Int) : Int\ncase x\n"
             "1 | 2: 10\n-3: 20\n_: 30\nend\nend\nfn main() : Int\nchoose(1)\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_body *body = &f.program->functions[0].body;
        uint32_t literals = 0;
        bool matched = false;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            CHECK(row->operation == MO_T_EOF);
            if (row->syntax->kind == MO_S_INT) {
                CHECK(row->syntax->span.end - row->syntax->span.start == 2);
                literals++;
            }
            if (row->match != NULL) {
                const struct mo_checked_match *match = row->match;
                MO_ASSERT(match->arm_count == 3 && match->arms[0].pattern_count == 2);
                const struct mo_checked_pattern *a = match->arms[0].patterns[0];
                const struct mo_checked_pattern *b = match->arms[0].patterns[1];
                const struct mo_checked_pattern *c = match->arms[1].patterns[0];
                CHECK(a->kind == MO_PATTERN_LITERAL && a->syntax->text[0] == '1');
                CHECK(b->kind == MO_PATTERN_LITERAL && b->syntax->text[0] == '2');
                CHECK(c->kind == MO_PATTERN_LITERAL && c->syntax->kind == MO_S_NEGATE);
                CHECK(strcmp(c->type->name, "Int") == 0);
                matched = true;
            }
        }
        CHECK(literals == 3 && matched);
    }
    free_fixture(&f);
}

static void ignored_binders(void) {
    struct fixture f;
    init(&f, "module Main\nfn main() : Int\n(first, _) = (1, 2)\n"
             "for _ in 0..3\nassert true\nend\nf = fn(_) 7 end\nf(first)\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_body *body = &f.program->functions[0].body;
        CHECK(body->binding_count == 2);
        for (uint32_t i = 0; i < body->binding_count; i++)
            CHECK(strcmp(body->bindings[i].name, "_") != 0);
        bool loop = false, lambda = false;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            if (row->syntax->kind == MO_S_FOR) {
                CHECK(row->binding == NULL && row->pattern->kind == MO_PATTERN_ANY);
                loop = true;
            }
            if (row->lambda != NULL) {
                MO_ASSERT(row->lambda->parameter_count == 1);
                CHECK(row->lambda->parameters[0] == NULL);
                CHECK(strcmp(row->type->arguments[0]->name, "Int") == 0);
                lambda = true;
            }
        }
        CHECK(loop && lambda);
    }
    free_fixture(&f);
}

static void constructor_exits(void) {
    struct fixture f;
    init(&f, "module Main\nstruct P\nx: Int\ny: Int\nend\n"
             "fn make() : P\nP(y: stop \"a\", x: 1)\nend\n"
             "fn failure() : FileError\nFailed(reason: \"oops\", path: stop \"b\")\nend\n"
             "fn main()\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        uint32_t exits = 0, values = 0;
        for (uint32_t fidx = 0; fidx < 2; fidx++) {
            const struct mo_checked_body *body = &f.program->functions[fidx].body;
            for (uint32_t i = 0; i < body->count; i++) {
                const struct mo_checked_expression *row = &body->items[i];
                if (row->syntax->kind != MO_S_NAMED_ARG)
                    continue;
                MO_ASSERT(row->operand_count == 1);
                bool never = body->items[row->operands[0]].never;
                CHECK(row->never == never);
                if (never)
                    exits++;
                else
                    values++;
            }
        }
        CHECK(exits == 2 && values == 2);
    }
    free_fixture(&f);
}

static void wide_patterns(void) {
    struct mo_buffer source;
    mo_buffer_init(&source, 65536);
    MO_ASSERT(mo_buffer_append_text(&source, "module Main\nstruct Wide\n"));
    for (uint32_t i = 0; i < 1000; i++) {
        MO_ASSERT(mo_buffer_append_text(&source, "field"));
        MO_ASSERT(mo_buffer_append_uint(&source, i));
        MO_ASSERT(mo_buffer_append_text(&source, i == 999 ? ": Text\n" : ": Int\n"));
    }
    MO_ASSERT(mo_buffer_append_text(&source, "end\nfn choose(value: Wide) : Int\ncase value\n"));
    for (uint32_t i = 0; i < 150; i++) {
        MO_ASSERT(mo_buffer_append_text(&source, "Wide(field0: "));
        MO_ASSERT(mo_buffer_append_uint(&source, i));
        MO_ASSERT(mo_buffer_append_text(&source, "): 10\n"));
    }
    MO_ASSERT(mo_buffer_append_text(&source, "_: 20\nend\nend\nfn main()\nend\n"));
    MO_ASSERT(mo_buffer_append_byte(&source, 0));
    struct fixture f;
    init(&f, (const char *)source.bytes);
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_body *body = &f.program->functions[0].body;
        bool matched = false;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_match *match = body->items[i].match;
            if (match == NULL)
                continue;
            MO_ASSERT(match->arm_count == 151);
            for (uint32_t j = 0; j < 150; j++) {
                const struct mo_checked_pattern *value = match->arms[j].patterns[0];
                MO_ASSERT(value->part_count == 1000);
                CHECK(value->parts[0].pattern->kind == MO_PATTERN_LITERAL);
                CHECK(value->parts[1].pattern->kind == MO_PATTERN_ANY);
                CHECK(strcmp(value->parts[1].pattern->type->name, "Int") == 0);
                CHECK(strcmp(value->parts[999].pattern->type->name, "Text") == 0);
            }
            matched = true;
        }
        CHECK(matched);
    }
    free_fixture(&f);
    mo_buffer_free(&source);
}

static void rejected_stages(void) {
    static const struct {
        const char *source;
        uint16_t code;
    } cases[] = {
        {"fn helper(x: Int) : Int where T: Bogus\nx\nend\nfn main()\nend\n",
         MO_CODE_ABILITY_REQUIREMENT},
        {"fn main() : Int where Q: Equal\n0\nend\n", MO_CODE_ABILITY_REQUIREMENT},
        {"fn main() : Int where Int: Number\n0\nend\n", MO_CODE_ABILITY_REQUIREMENT},
        {"fn helper(c: Console) : Text\n\"#{c}\"\nend\nfn main()\nend\n", MO_CODE_ABILITY_MISSING},
        {"fn helper(bytes: Bytes) : Text\n\"#{bytes}\"\nend\nfn main()\nend\n",
         MO_CODE_ABILITY_MISSING},
        {"fn helper(_x: Set(Float))\nend\nfn main()\nend\n", MO_CODE_ABILITY_MISSING},
        {"fn helper(_x: Map(Float, Int))\nend\nfn main()\nend\n", MO_CODE_ABILITY_MISSING},
        {"fn helper(_x: Set(fn(Int) : Int))\nend\nfn main()\nend\n", MO_CODE_ABILITY_MISSING},
        {"fn helper(_x: Map(Console, Int))\nend\nfn main()\nend\n", MO_CODE_ABILITY_MISSING},
        {"struct P\nx: Float\nend\nfn helper(_x: Map(P, Int))\nend\nfn main()\nend\n",
         MO_CODE_ABILITY_MISSING},
        {"struct Bag(T)\nvalues: Set(T)\nend\nfn helper(_x: Bag(Float))\nend\nfn main()\nend\n",
         MO_CODE_ABILITY_MISSING},
        {"struct Bag(T)\nvalues: Set(T)\nend\nfn helper(_x: Bag(T)) : Bool where T: Text\n"
         "true\nend\nfn main() : Int\nif helper(Bag(values: Set.empty.add(\"a\".bytes))): "
         "1 else: 0\nend\n",
         MO_CODE_ABILITY_MISSING},
        {"struct P\nm: Map(Float, Int)\nend\nfn main()\nend\n", MO_CODE_ABILITY_MISSING},
        {"enum E\nValue(Map(Float, Int))\nend\nfn main()\nend\n", MO_CODE_ABILITY_MISSING},
        {"fn helper() : Set(Float)\nstop \"no set\"\nend\nfn main()\nend\n",
         MO_CODE_ABILITY_MISSING},
        {"fn helper(_x: List((Int, Set(Float))))\nend\nfn main()\nend\n", MO_CODE_ABILITY_MISSING},
        {"fn helper(_f: fn(Map(Float, Int)) : Int)\nend\nfn main()\nend\n",
         MO_CODE_ABILITY_MISSING},
        {"struct P\nx: Int\nend\nfn compare(_a: P, _b: P) : Int\n0\nend\nfn main()\nend\n",
         MO_CODE_ABILITY_CUSTOMIZER},
        {"struct P\nx: Int\nend\nfn to_text(_p: P) : Int\n0\nend\nfn main()\nend\n",
         MO_CODE_ABILITY_CUSTOMIZER},
        {"struct P\nx: Int\nend\nfn to_text(_p: P, c: Text) : Text\nc\nend\n"
         "fn main()\nend\n",
         MO_CODE_ABILITY_CUSTOMIZER},
        {"struct P\nx: Int\nend\nfn compare(_a: P) : Ordering\nEqual\nend\n"
         "fn main()\nend\n",
         MO_CODE_ABILITY_CUSTOMIZER},
        {"fn main() : Text\n\"not an entry result\"\nend\n", MO_CODE_MAIN_CONTRACT},
        {"fn main(value: Int) : Int\nvalue\nend\n", MO_CODE_MAIN_CONTRACT},
        {"fn main(a: Console, b: Console)\na.print(\"a\")\nb.print(\"b\")\nend\n",
         MO_CODE_MAIN_CONTRACT},
        {"fn main() : Result(Unit, Bytes)\nstop \"no text\"\nend\n", MO_CODE_ABILITY_MISSING},
        {"fn main() : Result(Unit, T)\nstop \"generic result\"\nend\n", MO_CODE_MAIN_CONTRACT},
        {"fn main() : Result(Int, FileError)\nstop \"bad result\"\nend\n", MO_CODE_MAIN_CONTRACT},
        {"fn main() : Int\n0\nend\nfn main(console: Console)\nconsole.print(\"x\")\nend\n",
         MO_CODE_OVERLOAD},
        {"fn grow(value: T)\ngrow([value])\nend\nfn main()\nend\n", MO_CODE_POLYMORPHIC_RECURSION},
        {"fn a(value: T)\nb([value])\nend\nfn b(value: U)\na(value)\nend\nfn main()\nend\n",
         MO_CODE_POLYMORPHIC_RECURSION},
        {"fn main() : Int\ntry Some(1)\nend\n", MO_CODE_TRY_RESULT},
        {"fn helper() : Option(Int)\nSome(try 1)\nend\nfn main()\nend\n", MO_CODE_TRY_INPUT},
        {"fn helper() : Option(Int)\nSome(try true)\nend\nfn main()\nend\n", MO_CODE_TRY_INPUT},
        {"fn helper(value: Option(Int)) : Result(Int, Text)\nOk(try value)\nend\n"
         "fn main()\nend\n",
         MO_CODE_TRY_RESULT},
        {"fn helper(value: Result(Int, Text)) : Option(Int)\nSome(try value)\nend\n"
         "fn main()\nend\n",
         MO_CODE_TRY_RESULT},
        {"fn main() : Int\nf = fn(value) try value end\nf(Some(1)) + 1\nend\n", MO_CODE_TRY_RESULT},
        {"enum Outer\nNamed(value: Text)\nend\n"
         "fn helper(value: Result(Int, Text)) : Result(Int, Outer)\nOk(try value)\nend\n"
         "fn main()\nend\n",
         MO_CODE_TRY_WRAPPER},
        {"enum Outer\nFirst(Text)\nSecond(Text)\nend\n"
         "fn helper(value: Result(Int, Text)) : Result(Int, Outer)\nOk(try value)\nend\n"
         "fn main()\nend\n",
         MO_CODE_TRY_WRAPPER},
        {"fn helper(value: Result(Int, T)) : Result(Int, U)\nOk(try value)\nend\n"
         "fn main()\nend\n",
         MO_CODE_TRY_WRAPPER},
        {"enum Inner(T)\nBad(T)\nend\nenum Outer\nWrap(Inner(Bool))\nend\n"
         "fn helper(value: Result(Int, Inner(Int))) : Result(Int, Outer)\nOk(try value)\nend\n"
         "fn main()\nend\n",
         MO_CODE_TRY_WRAPPER},
        {"fn main(console: Console)\nconsole.printed\nend\n", MO_CODE_CHECK_UNFINISHED},
        {"value = 1\nfn main() : Int\nmissing\nend\n", MO_CODE_NAME_UNKNOWN},
        {"fn main() : Int\nexamples\n1\nend\n1\nend\n", MO_CODE_CONDITION_TYPE},
        {"fn helper(a: Int) : Int\nexamples\na == a\nend\na\nend\nfn main()\nend\n",
         MO_CODE_NAME_UNKNOWN},
        {"fn grow(value: T) : Bool\npromise \"stays finite\"\ngrow([value])\n"
         "allows value: 1, result: true\nforbids value: 2, result: false\nend\n"
         "_used = value\ntrue\nend\nfn main()\nend\n",
         MO_CODE_POLYMORPHIC_RECURSION},
        {"fn helper() : Int\nmissing\nend\nfn main() : Int\n0\nend\n", MO_CODE_NAME_UNKNOWN},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct mo_buffer source;
        mo_buffer_init(&source, 4096);
        MO_ASSERT(mo_buffer_append_text(&source, "module Main\n"));
        MO_ASSERT(mo_buffer_append_text(&source, cases[i].source));
        MO_ASSERT(mo_buffer_append_byte(&source, 0));
        struct fixture f;
        init(&f, (const char *)source.bytes);
        CHECK(f.program == NULL);
        CHECK(f.diags.count == 1 && f.diags.items[0].code == cases[i].code);
        free_fixture(&f);
        mo_buffer_free(&source);
    }
}

static void depth_graph(void) {
    struct mo_buffer source;
    mo_buffer_init(&source, 32768);
    MO_ASSERT(mo_buffer_append_text(
        &source, "module Main\nfn run(a: Bool, b: Bool, c: Bool, d: Range, e: Int, "
                 "f: Int, g: Int, h: fn(Bool) : Int) : Bool\n"));
    for (uint32_t i = 0; i < MO_LIMIT_NESTING_DEPTH - 1; i++)
        MO_ASSERT(mo_buffer_append_text(&source, "a implies b or c and not d == e .. f + g * -h("));
    MO_ASSERT(mo_buffer_append_text(&source, "a"));
    for (uint32_t i = 0; i < MO_LIMIT_NESTING_DEPTH - 1; i++)
        MO_ASSERT(mo_buffer_append_text(&source, ")"));
    MO_ASSERT(mo_buffer_append_text(&source, "\nend\nfn main()\nend\n"));
    MO_ASSERT(mo_buffer_append_byte(&source, 0));
    struct fixture f;
    init(&f, (const char *)source.bytes);
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_body *body = &f.program->functions[0].body;
        CHECK(strcmp(body->items[body->root].type->name, "Bool") == 0);
        for (uint32_t i = 0; i < body->count; i++)
            for (uint32_t j = 0; j < body->items[i].operand_count; j++)
                CHECK(body->items[i].operands[j] < i);
    }
    free_fixture(&f);
    mo_buffer_free(&source);
}

static void diagnostic_order(void) {
    static const char *const later[] = {
        "fn later(_x: Unknown)\nend\n",
        "fn later(_x: Set(Float))\nend\n",
        "struct Later\nfield: Unknown\nend\n",
        "struct Later(T)\nfield: Later(List(T))\nend\n",
    };
    for (size_t i = 0; i < sizeof(later) / sizeof(later[0]); i++) {
        struct mo_buffer source;
        mo_buffer_init(&source, 4096);
        MO_ASSERT(mo_buffer_append_text(&source, "module Main\nfn main() : Int\nmissing\nend\n") &&
                  mo_buffer_append_text(&source, later[i]) && mo_buffer_append_byte(&source, 0));
        struct fixture f;
        init(&f, (const char *)source.bytes);
        CHECK(f.program == NULL && f.diags.count == 2);
        if (f.diags.count != 0) {
            CHECK(f.diags.items[0].code == MO_CODE_NAME_UNKNOWN);
            CHECK(f.diags.items[0].primary.span.start == 28);
        }
        free_fixture(&f);
        mo_buffer_free(&source);
    }
    struct fixture f;
    init(&f, "module Main\nfn zebra() : Int\nunknown\nend\n"
             "fn alpha() : Int\nfalse\nend\n");
    CHECK(f.program == NULL && f.diags.count == 2);
    if (f.diags.count == 2) {
        CHECK(f.diags.items[0].code == MO_CODE_NAME_UNKNOWN &&
              f.diags.items[1].code == MO_CODE_RESULT_TYPE);
        CHECK(mo_sources_position(&f.sources, 0, f.diags.items[0].primary.span.start).line == 3 &&
              mo_sources_position(&f.sources, 0, f.diags.items[1].primary.span.start).line == 6);
    }
    free_fixture(&f);
    init(&f, "module Main\nfn main() : Int\nlater(1)\nend\n"
             "fn later(_x: Unknown) : Int\n0\nend\n");
    CHECK(f.program == NULL && f.diags.count == 1);
    if (f.diags.count == 1)
        CHECK(f.diags.items[0].code == MO_CODE_TYPE_NAME);
    free_fixture(&f);
    init(&f, "module Main\nstruct A\nfield: B\nend\nstruct B\nfield: C\nend\n"
             "struct C\nfield: Unknown\nend\nfn read(a: A) : Int\na.field.field.field\nend\n"
             "fn main() : Int\nmissing\nend\n");
    CHECK(f.program == NULL && f.diags.count == 2);
    if (f.diags.count == 2) {
        CHECK(f.diags.items[0].code == MO_CODE_TYPE_NAME);
        CHECK(f.diags.items[1].code == MO_CODE_NAME_UNKNOWN);
    }
    free_fixture(&f);
    const char *source = "module Main\nfn main() : Int\ns = Set.empty.add(Some(1))\n"
                         "if s < s: 1 else: 0\nend\n";
    init(&f, source);
    CHECK(f.program == NULL && f.diags.count == 1);
    if (f.diags.count == 1) {
        CHECK(f.diags.items[0].code == MO_CODE_ABILITY_MISSING);
        CHECK(strcmp(f.diags.items[0].message, "type Set(Option(Int)) doesn't have Order") == 0);
        struct mo_span span = f.diags.items[0].primary.span;
        CHECK(span.end - span.start == 5 && memcmp(source + span.start, "s < s", 5) == 0);
    }
    free_fixture(&f);
}

static void diagnostic_cap(void) {
    struct mo_buffer source;
    mo_buffer_init(&source, MO_LIMIT_FILE_SIZE);
    MO_ASSERT(mo_buffer_append_text(&source, "module Main\n"));
    for (uint32_t i = 0; i < 1500; i++) {
        MO_ASSERT(mo_buffer_append_text(&source, "fn f") && mo_buffer_append_uint(&source, i) &&
                  mo_buffer_append_text(&source, "()\nunknown\nend\n"));
    }
    MO_ASSERT(mo_buffer_append_byte(&source, 0));
    struct fixture f;
    init(&f, (const char *)source.bytes);
    CHECK(f.program == NULL && f.diags.count == 1000 && f.diags.dropped == 500);
    for (uint32_t i = 0; i < f.diags.count; i++)
        CHECK(f.diags.items[i].code == MO_CODE_NAME_UNKNOWN);
    size_t used = f.arena.used;
    bool capped_resource = false;
    for (size_t i = 32; i < 64; i++) {
        mo_arena_free(&f.arena);
        mo_arena_init(&f.arena, used * i / 64);
        mo_diags_free(&f.diags);
        MO_ASSERT(mo_diags_init(&f.diags));
        f.program = mo_check(&f.arena, &f.set, &f.diags);
        CHECK(f.program == NULL && mo_diags_any(&f.diags));
        if (f.diags.count == MO_LIMIT_DIAGNOSTICS) {
            /* A smaller arena can still finish by allocating smaller chunks.
             * Exhaustion after the cap must stop before all remaining bodies. */
            CHECK(f.diags.dropped > 0 && f.diags.dropped <= 500);
            capped_resource = capped_resource || f.diags.dropped < 500;
        } else if (f.diags.count != 0)
            CHECK(f.diags.items[f.diags.count - 1].code == MO_CODE_CHECK_NO_MEMORY);
    }
    CHECK(capped_resource);
    free_fixture(&f);
    mo_buffer_clear(&source);
    MO_ASSERT(mo_buffer_append_text(&source, "module Main\n"));
    for (uint32_t i = 0; i < 1500; i++)
        MO_ASSERT(mo_buffer_append_text(&source, "c") && mo_buffer_append_uint(&source, i) &&
                  mo_buffer_append_text(&source, " = unknown\n"));
    MO_ASSERT(mo_buffer_append_text(&source, "fn main()\nend\n") &&
              mo_buffer_append_byte(&source, 0));
    init(&f, (const char *)source.bytes);
    CHECK(f.program == NULL && f.diags.count == 1000 && f.diags.dropped == 500);
    for (uint32_t i = 0; i < f.diags.count; i++)
        CHECK(f.diags.items[i].code == MO_CODE_NAME_UNKNOWN);
    free_fixture(&f);
    mo_buffer_free(&source);
}

static void ability_scaling(void) {
    static const uint32_t counts[] = {4100, 6700, 4500, 3000, 600, 250};
    for (uint32_t kind = 0; kind < sizeof(counts) / sizeof(counts[0]); kind++) {
        struct mo_buffer source;
        mo_buffer_init(&source, 4 << 20);
        MO_ASSERT(mo_buffer_append_text(&source, "module Main\n"));
        if (kind == 0)
            MO_ASSERT(mo_buffer_append_text(&source, "enum E\n"));
        if (kind == 5)
            MO_ASSERT(mo_buffer_append_text(&source, "fn duplicate(x: T) : (T, T)\n(x, x)\nend\n"
                                                     "fn main()\nv0 = 1\n"));
        for (uint32_t i = 0; i < counts[kind]; i++) {
            char line[512];
            int n;
            if (kind == 0)
                n = snprintf(line, sizeof(line), "C%u(value: Int)\n", i);
            else if (kind == 1)
                n = snprintf(line, sizeof(line), "fn f%u(x: T) : T\nx\nend\n", i);
            else if (kind == 2 || kind == 3)
                n = snprintf(line, sizeof(line),
                             "struct R%u\nx: Int\nend\nfn show%u(x: R%u) : Text\n\"#{x}\"\nend\n",
                             i, i, i);
            else if (kind == 4)
                n = i + 1 < counts[kind]
                        ? snprintf(line, sizeof(line), "struct R%u(T)\nnext: R%u(T)\nend\n", i,
                                   i + 1)
                        : snprintf(line, sizeof(line), "struct R%u(T)\nnext: Set(T)\nend\n", i);
            else
                n = snprintf(line, sizeof(line), "v%u = duplicate(v%u)\n", i + 1, i);
            MO_ASSERT(n > 0 && (size_t)n < sizeof(line) && mo_buffer_append_text(&source, line));
            if (kind == 3) {
                n = snprintf(line, sizeof(line), "fn to_text(_x: R%u) : Text\n\"r\"\nend\n", i);
                MO_ASSERT(n > 0 && (size_t)n < sizeof(line) &&
                          mo_buffer_append_text(&source, line));
            }
        }
        if (kind == 0)
            MO_ASSERT(mo_buffer_append_text(&source, "end\nfn same(e: E) : Bool\ne == e\nend\n"));
        if (kind == 4)
            MO_ASSERT(mo_buffer_append_text(&source, "fn accepts(_x: R0(T))\nend\n"));
        if (kind == 5)
            MO_ASSERT(mo_buffer_append_text(&source, "_set = Set.empty.add(v250)\nend\n"));
        else
            MO_ASSERT(mo_buffer_append_text(&source, "fn main()\nend\n"));
        MO_ASSERT(mo_buffer_append_byte(&source, 0));
        struct fixture f;
        init(&f, (const char *)source.bytes);
        CHECK(f.program != NULL && !mo_diags_any(&f.diags));
        if (kind == 3 && f.program != NULL) {
            for (uint32_t i = 0; i < f.program->function_count; i++) {
                const struct mo_checked_body *body = &f.program->functions[i].body;
                for (uint32_t j = 0; j < body->count; j++) {
                    const struct mo_checked_expression *row = &body->items[j];
                    if (row->syntax->kind != MO_S_TEXT_INTERPOLATED)
                        continue;
                    MO_ASSERT(row->ability_count == 1);
                    const struct mo_ability_implementation *proof =
                        row->abilities[0].implementation;
                    CHECK(proof->kind == MO_ABILITY_FUNCTION && proof->function != NULL);
                    if (proof->function != NULL) {
                        const struct mo_module_scope *scope =
                            &f.program->interfaces->scopes[proof->function->module];
                        CHECK(proof->function->receiver < scope->count);
                        if (proof->function->receiver < scope->count)
                            CHECK(&scope->symbols[proof->function->receiver] ==
                                  proof->type->declaration);
                    }
                }
            }
        }
        free_fixture(&f);
        mo_buffer_free(&source);
    }
}

static void ability_depth(void) {
    MO_ASSERT(MO_LIMIT_SOURCE_ABILITY_DEPTH % 2 == 0);
    for (uint32_t extra = 0; extra < 2; extra++) {
        struct mo_buffer source;
        mo_buffer_init(&source, 1 << 20);
        MO_ASSERT(mo_buffer_append_text(&source, "module Main\n"));
        uint32_t count = MO_LIMIT_SOURCE_ABILITY_DEPTH / 2;
        for (uint32_t i = 0; i < count; i++) {
            char line[128];
            int n =
                i + 1 < count
                    ? snprintf(line, sizeof(line), "struct R%u\nnext: Option(R%u)\nend\n", i, i + 1)
                    : snprintf(line, sizeof(line), "struct R%u\nnext: Option(Int)\nend\n", i);
            MO_ASSERT(n > 0 && (size_t)n < sizeof(line) && mo_buffer_append_text(&source, line));
        }
        MO_ASSERT(mo_buffer_append_text(
            &source, extra == 0 ? "fn same(a: R0, b: R0) : Bool\na == b\nend\nfn main()\nend\n"
                                : "fn same(a: Option(R0), b: Option(R0)) : Bool\na == b\nend\n"
                                  "fn main()\nend\n"));
        MO_ASSERT(mo_buffer_append_byte(&source, 0));
        struct fixture f;
        init(&f, (const char *)source.bytes);
        if (extra == 0)
            CHECK(f.program != NULL && !mo_diags_any(&f.diags));
        else {
            CHECK(f.program == NULL && f.diags.count == 1);
            if (f.diags.count == 1) {
                CHECK(f.diags.items[0].code == MO_CODE_CHECK_ABILITY_DEPTH);
                struct mo_span span = f.diags.items[0].primary.span;
                CHECK(span.end - span.start == 6 &&
                      memcmp(source.bytes + span.start, "a == b", 6) == 0);
            }
        }
        free_fixture(&f);
        mo_buffer_free(&source);
    }
}

static void allocation_failures(void) {
    static const char *const sources[] = {
        "module Main\nfn main()\nend\n",
        "module Main\nstruct Box(T)\nvalue: T\nend\n"
        "fn to_text(box: Box(T)) : Text where T: Text\n\"#{box.value}\"\nend\n"
        "fn main() : Int\nvar total = 0\nfor i in 0..3\ntotal += i\nend\n"
        "reader = fn(value) value.value end\ntotal = reader(Box(value: total))\n"
        "box = Some(Box(value: total))\n_rendered = \"#{box}\"\n"
        "case box\nSome(Box(value: n)): n\nNone: 0\nend\nend\n",
        "module Main\nenum Outer(T)\nWrapped(Set(T))\nend\n"
        "fn lift(value: Result(Int, Set(T))) : Result(Text, Outer(T)) where T: Hash\n"
        "n = try value\nOk(n.to_text)\nend\n"
        "fn main() : Int\nreader = fn(values) values.get(0) or 17 end\nreader([3])\nend\n",
        "module Main\nfn twice(value: T) : T where T: Number\nexamples\n"
        "twice(3) == 6\nend\nvalue + value\nend\n"
        "fn main() : Int\ntwice(4)\nend\n",
        "module Main\nfn twice(value: T) : T where T: Number, T: Equal\n"
        "promise \"doubles\"\nresult == value + value\n"
        "allows result: 6, value: 3\nforbids value: 2, result: 5\nend\n"
        "value + value\nend\ntest \"private \\u{3bb}\"\nassert twice(4) == 8\nend\n"
        "fn main()\nend\n",
        "module Main\nanswer = twice(3)\nlabel = \"#{Label(value: answer)}\"\n"
        "prefix = \"value\"\nstruct Label\nvalue: Int\nend\n"
        "fn to_text(label: Label) : Text\n\"#{prefix}#{label.value}\"\nend\n"
        "fn twice(value: T) : T where T: Number\nvalue + value\nend\n"
        "fn main() : Int\nanswer\nend\n"};
    for (size_t s = 0; s < sizeof(sources) / sizeof(sources[0]); s++) {
        struct fixture f;
        init(&f, sources[s]);
        MO_ASSERT(f.program != NULL && !mo_diags_any(&f.diags));
        size_t used = f.arena.used;
        uint32_t failures = 0, successes = 0;
        for (size_t limit = 0; limit <= used + 128; limit += 128) {
            mo_arena_free(&f.arena);
            mo_arena_init(&f.arena, limit);
            mo_diags_free(&f.diags);
            MO_ASSERT(mo_diags_init(&f.diags));
            f.program = mo_check(&f.arena, &f.set, &f.diags);
            if (f.program == NULL) {
                failures++;
                CHECK(mo_diags_any(&f.diags));
                for (uint32_t i = 0; i < f.diags.count; i++) {
                    CHECK(f.diags.items[i].code == MO_CODE_CHECK_NO_MEMORY);
                    struct mo_span span = f.diags.items[i].primary.span;
                    CHECK(span.file == f.module->span.file || span.file == UINT32_MAX);
                    CHECK(span.start <= span.end && span.end <= strlen(sources[s]));
                }
            } else {
                successes++;
                CHECK(!mo_diags_any(&f.diags));
            }
        }
        CHECK(failures != 0 && successes != 0);
        free_fixture(&f);
    }
}

static void corpus(void) {
    static const char *const paths[] = {
        "fuzz/corpus/fuzz_program/loops.mo",    "fuzz/corpus/fuzz_program/patterns.mo",
        "fuzz/corpus/fuzz_program/lambdas.mo",  "fuzz/corpus/fuzz_program/failures.mo",
        "fuzz/corpus/fuzz_program/examples.mo", "fuzz/corpus/fuzz_program/specs.mo",
        "fuzz/corpus/fuzz_program/constants.mo"};
    struct mo_sources sources;
    MO_ASSERT(mo_sources_init(&sources));
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        uint32_t file;
        struct mo_source_error error;
        MO_ASSERT(mo_sources_add(&sources, paths[i], &file, &error));
        struct fixture f;
        init(&f, (const char *)sources.files[file].bytes);
        CHECK(f.program != NULL && !mo_diags_any(&f.diags));
        free_fixture(&f);
    }
    mo_sources_free(&sources);
}

static void time_arithmetic_graph(void) {
    struct fixture f;
    init(&f, "module Main\n"
             "fn scale(x: Decimal) : Decimal\n"
             "x * 2\n"
             "end\n"
             "fn compute(a: Time, b: Time, d: Duration, n: Int) : (Duration, Time, Time, Duration, "
             "Decimal)\n"
             "diff = a - b\n"
             "var shifted = a + d\n"
             "shifted -= d\n"
             "var span = (diff + d - d) * n\n"
             "span += -d\n"
             "(diff, a + d, shifted - d, span, 1.5.scale + 1)\n"
             "end\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    free_fixture(&f);

    static const struct {
        const char *source;
        uint16_t code;
        const char *label;
    } invalid[] = {
        {"module Main\nfn bad(a: Time, b: Time) : Time\na + b\nend\n", MO_CODE_TYPE_MISMATCH,
         "expected Duration, found Time"},
        {"module Main\nfn bad(n: Int, d: Duration) : Int\nn * d\nend\n", MO_CODE_TYPE_MISMATCH,
         "expected Int, found Duration"},
        {"module Main\nfn bad(t: Time, n: Int) : Time\nt * n\nend\n", MO_CODE_NUMBER_ABILITY,
         "expected Int or BigInt or Decimal or Float, found Time"},
        {"module Main\nfn bad(t: Time) : Time\n-t\nend\n", MO_CODE_NUMBER_ABILITY,
         "expected Int or BigInt or Decimal or Float, found Time"},
        {"module Main\nfn bad(t: Time) : Text\n-(t - 1.seconds)\nend\n", MO_CODE_NUMBER_ABILITY,
         "expected Int or BigInt or Decimal or Float, found Time"},
        {"module Main\nfn bad() : Int\n-1.seconds\nend\n", MO_CODE_RESULT_TYPE,
         "expected Int, found Duration"},
        {"module Main\nfn bad(a: Time, b: Time) : Int\na - (b - 1.seconds)\nend\n",
         MO_CODE_RESULT_TYPE, "expected Int, found Duration"},
        {"module Main\nfn bad(t: Time) : Unit\n_f = fn(x) t - x end\nUnit\nend\n",
         MO_CODE_CANNOT_INFER, "no use decides the type"},
        {"module Main\nfn bad(a: Time, b: Time) : Duration\na / b\nend\n", MO_CODE_NUMBER_ABILITY,
         "expected Int or BigInt or Decimal or Float, found Time"},
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        init(&f, invalid[i].source);
        CHECK(f.program == NULL && f.diags.count == 1);
        if (f.diags.count == 1) {
            CHECK(f.diags.items[0].code == invalid[i].code);
            CHECK(strcmp(f.diags.items[0].primary.text, invalid[i].label) == 0);
        }
        free_fixture(&f);
    }
}

static void run(void *argument) {
    (void)argument;
    map_graph();
    time_arithmetic_graph();
    constant_graph();
    constant_names();
    constant_context();
    constant_dependencies();
    constant_refusals();
    constant_chain();
    example_graph();
    promise_graph();
    generic_spec_graph();
    fake_specs();
    spec_refusals();
    entry_graph();
    console_graph();
    native_graph();
    interpolation_graph();
    ability_dot_graph();
    customizer_helpers();
    method_graph();
    overload_graph();
    delayed_dot_graph();
    library_graph();
    library_scope();
    entry_record();
    ability_graph();
    native_choice_abilities();
    generic_graph();
    number_literal_bound();
    requirement_keys();
    data_recursion();
    generic_lambdas();
    option_or_graph();
    deferred_defaults();
    try_graph();
    inferred_wrappers();
    try_contexts();
    lambda_graph();
    capture_graph();
    capture_work();
    branch_graph();
    record_graph();
    match_graph();
    loop_graph();
    builtin_graph();
    collection_graph();
    literal_patterns();
    ignored_binders();
    constructor_exits();
    wide_patterns();
    rejected_stages();
    depth_graph();
    diagnostic_order();
    diagnostic_cap();
    ability_scaling();
    ability_depth();
    allocation_failures();
    corpus();
}

int main(void) {
    CHECK(mo_with_stack(MO_SOURCE_STACK_SIZE, run, NULL) == 0);
    return mo_test_report("program");
}
