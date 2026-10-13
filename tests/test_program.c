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
    mo_arena_init(&f->syntax, 1 << 20);
    mo_arena_init(&f->arena, 16 << 20);
    MO_ASSERT(mo_sources_init(&f->sources) && mo_diags_init(&f->diags));
    uint32_t file;
    struct mo_source_error error;
    MO_ASSERT(mo_sources_add_bytes(&f->sources, "main.mo", (const uint8_t *)text, strlen(text),
                                   &file, &error));
    f->module = mo_parse(&f->syntax, &f->sources, file, &f->diags);
    MO_ASSERT(f->module != NULL && !mo_diags_any(&f->diags));
    f->set = (struct mo_module_set){
        .count = 1, .modules = &f->module, .order = &f->order, .uses = &f->uses};
    f->program = mo_check(&f->arena, &f->set, &f->diags);
}

static void free_fixture(struct fixture *f) {
    mo_arena_free(&f->arena);
    mo_arena_free(&f->syntax);
    mo_diags_free(&f->diags);
    mo_sources_free(&f->sources);
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
             "if set.has?(4): [1, 2, 3].sum else: 0\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program != NULL) {
        const struct mo_checked_body *body = &f.program->functions[0].body;
        bool empty = false, sum = false;
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
            }
        }
        CHECK(empty && sum);
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
    init(&f, "module Main\nstruct Tree(T)\nchildren: List(Tree(List(T)))\nend\n"
             "struct Phantom(T)\nvalue: Int\nend\n"
             "fn accepts(_key: Map(Phantom(Float), Int), _tree: Tree(Int))\nend\n"
             "fn main()\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
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
        {"fn helper()\nend\n", MO_CODE_MAIN_CONTRACT},
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
        {"fn main() : Text\n\"not an entry result\"\nend\n", MO_CODE_MAIN_CONTRACT},
        {"fn main(value: Int) : Int\nvalue\nend\n", MO_CODE_MAIN_CONTRACT},
        {"fn main(a: Console, b: Console)\na.print(\"a\")\nb.print(\"b\")\nend\n",
         MO_CODE_MAIN_CONTRACT},
        {"fn main(_args: Args)\nend\n", MO_CODE_CHECK_UNFINISHED},
        {"fn main(_clock: Clock, _console: Console)\nend\n", MO_CODE_CHECK_UNFINISHED},
        {"fn main() : Result(Unit, FileError)\nstop \"no result\"\nend\n",
         MO_CODE_CHECK_UNFINISHED},
        {"fn main() : Result(Int, FileError)\nstop \"bad result\"\nend\n", MO_CODE_MAIN_CONTRACT},
        {"fn main() : Int\n0\nend\nfn main(console: Console)\nconsole.print(\"x\")\nend\n",
         MO_CODE_OVERLOAD},
        {"fn grow(value: T)\ngrow([value])\nend\nfn main()\nend\n", MO_CODE_POLYMORPHIC_RECURSION},
        {"fn a(value: T)\nb([value])\nend\nfn b(value: U)\na(value)\nend\nfn main()\nend\n",
         MO_CODE_POLYMORPHIC_RECURSION},
        {"fn main() : Int\nx = 1\nf = fn() x end\nf()\nend\n", MO_CODE_CHECK_UNFINISHED},
        {"fn main() : Int\nm = {\"a\": 1}\ncase m\n_: 1\nend\nend\n", MO_CODE_CHECK_UNFINISHED},
        {"fn main() : Int\ntry Some(1)\nend\n", MO_CODE_CHECK_UNFINISHED},
        {"fn main() : Int\nSome(1) or 0\nend\n", MO_CODE_CHECK_UNFINISHED},
        {"fn main(console: Console)\nconsole.printed\nend\n", MO_CODE_CHECK_UNFINISHED},
        {"value = 1\nfn main()\nend\n", MO_CODE_CHECK_UNFINISHED},
        {"fn main() : Int\nexamples\n1 == 1\nend\n1\nend\n", MO_CODE_CHECK_UNFINISHED},
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

static void allocation_failures(void) {
    struct fixture f;
    init(&f,
         "module Main\nstruct Box\nvalue: Int\nend\n"
         "fn main() : Int\nvar total = 0\nfor i in 0..3\ntotal += i\nend\n"
         "box = Some(Box(value: total))\ncase box\nSome(Box(value: n)): n\nNone: 0\nend\nend\n");
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
            for (uint32_t i = 0; i < f.diags.count; i++)
                CHECK(f.diags.items[i].code == MO_CODE_CHECK_NO_MEMORY);
        } else {
            successes++;
            CHECK(!mo_diags_any(&f.diags));
        }
    }
    CHECK(failures != 0 && successes != 0);
    free_fixture(&f);
}

static void corpus(void) {
    static const char *const paths[] = {"fuzz/corpus/fuzz_program/loops.mo",
                                        "fuzz/corpus/fuzz_program/patterns.mo",
                                        "fuzz/corpus/fuzz_program/lambdas.mo"};
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

static void run(void *argument) {
    (void)argument;
    entry_graph();
    console_graph();
    native_graph();
    interpolation_graph();
    ability_dot_graph();
    entry_record();
    ability_graph();
    generic_graph();
    requirement_keys();
    lambda_graph();
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
    allocation_failures();
    corpus();
}

int main(void) {
    CHECK(mo_with_stack(MO_SOURCE_STACK_SIZE, run, NULL) == 0);
    return mo_test_report("program");
}
