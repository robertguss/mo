#include "base/codes.h"
#include "bytecode/io.h"
#include "codegen/codegen.h"
#include "heap/types.h"
#include "test.h"

#include <string.h>

static MO_MUST_USE struct mo_core *parse(struct mo_arena *arena, const char *payload,
                                         struct mo_diags *diags) {
    struct mo_buffer input;
    mo_buffer_init(&input, MO_LIMIT_CORE_TEXT_SIZE);
    CHECK(mo_buffer_append_text(&input, "(core " MO_CORE_TEXT_VERSION " "));
    CHECK(mo_buffer_append_text(&input, payload));
    CHECK(mo_buffer_append_text(&input, ")\n"));
    struct mo_core *p = mo_core_parse(arena, (const char *)input.bytes, input.length, diags);
    CHECK(p != NULL);
    mo_buffer_free(&input);
    return p;
}
static MO_MUST_USE struct mo_bytecode *compile(struct mo_arena *decoded, const char *payload) {
    struct mo_arena arena;
    struct mo_diags diags;
    struct mo_buffer first, second;
    mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
    mo_buffer_init(&first, MO_LIMIT_BYTECODE_SIZE);
    mo_buffer_init(&second, MO_LIMIT_BYTECODE_SIZE);
    CHECK(mo_diags_init(&diags));
    struct mo_core *p = parse(&arena, payload, &diags);
    bool ok = p != NULL && mo_compile(p, &first, &diags);
    CHECK(ok);
    if (!ok && diags.count != 0)
        (void)fprintf(stderr, "%s\n",
                      diags.items[0].primary.text != NULL ? diags.items[0].primary.text
                                                          : diags.items[0].message);
    CHECK(p != NULL && mo_compile(p, &second, &diags) == ok);
    CHECK(first.length == second.length);
    if (ok)
        CHECK(memcmp(first.bytes, second.bytes, first.length) == 0 && !mo_diags_any(&diags));
    mo_arena_free(&arena); /* Output and decoded data must not borrow Core's storage. */
    struct mo_load_error error;
    struct mo_bytecode *b =
        ok ? mo_bytecode_decode(decoded, first.bytes, first.length, &error) : NULL;
    CHECK(b != NULL);
    mo_buffer_free(&first);
    mo_buffer_free(&second);
    mo_diags_free(&diags);
    return b;
}
static MO_MUST_USE bool string_is(const struct mo_bytecode *b, uint32_t id, const char *s) {
    return id < b->string_count && b->strings[id].length == strlen(s) &&
           memcmp(b->strings[id].bytes, s, strlen(s)) == 0;
}
static void test_literals(void) {
    const struct {
        const char *type, *literal;
        enum mo_type_kind kind;
        uint8_t bytes[16];
    } cases[] = {
        {"unit", "unit", MO_TYPE_UNIT, {0}},
        {"bool", "bool true", MO_TYPE_BOOL, {1}},
        {"byte", "byte 255", MO_TYPE_BYTE, {255}},
        {"int", "int -9223372036854775808", MO_TYPE_INT, {0, 0, 0, 0, 0, 0, 0, 128}},
        {"float", "float 0x8000000000000000", MO_TYPE_FLOAT, {0, 0, 0, 0, 0, 0, 0, 128}},
        {"char", "char \"🙂\"", MO_TYPE_CHAR, {0x42, 0xf6, 1}},
        {"time", "time 1", MO_TYPE_TIME, {1}},
        {"duration", "duration -1", MO_TYPE_DURATION, {255, 255, 255, 255, 255, 255, 255, 255}},
        {"decimal",
         "decimal -170141183460469231731687303715884105727",
         MO_TYPE_DECIMAL,
         {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128}},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        struct mo_buffer text;
        mo_buffer_init(&text, MO_LIMIT_CORE_TEXT_SIZE);
        CHECK(mo_buffer_append_text(&text, "(Program () ((FunDecl \"main\" () ("));
        CHECK(mo_buffer_append_text(&text, cases[i].type));
        CHECK(mo_buffer_append_text(&text, ") (lit ("));
        CHECK(mo_buffer_append_text(&text, cases[i].literal));
        CHECK(mo_buffer_append_text(&text, ")) \"Other\" (function) 0)) () \"main\" (noSource))"));
        CHECK(mo_buffer_append_byte(&text, 0));
        struct mo_arena arena;
        mo_arena_init(&arena, MO_LIMIT_BYTECODE_SIZE);
        struct mo_bytecode *b = compile(&arena, (const char *)text.bytes);
        if (b != NULL) {
            CHECK(b->function_count == 1 && b->global_count == 0 && b->layout_count == 0);
            CHECK(string_is(b, b->functions[0].name, "Other.main"));
            CHECK(string_is(b, b->modules[b->functions[0].module].name, "Other"));
            const struct mo_bytecode_function *f = &b->functions[0];
            CHECK(f->code_count >= 5);
            const uint32_t *end = f->code + f->code_count - 5;
            CHECK(end[0] == MO_OP_CONST && end[3] == MO_OP_RET && end[1] == end[4]);
            CHECK(b->types[f->result_type].kind == cases[i].kind);
            CHECK(end[2] < b->constant_count);
            CHECK(memcmp(b->constants[end[2]].value.immediate, cases[i].bytes, 16) == 0);
            CHECK(b->debug[0].span_count == 0 && b->debug[0].name_count == 0 &&
                  b->debug[0].file == MO_NO_INDEX && b->modules[0].function_count == 0);
        }
        mo_arena_free(&arena);
        mo_buffer_free(&text);
    }
}
static void test_control(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_BYTECODE_SIZE);
    struct mo_bytecode *b =
        compile(&arena, "(Program () ((FunDecl \"main\" ((\"b\" (bool))) (int) "
                        "(if_ (var \"b\") (lit (int 17)) (lit (int 29))) \"Control\" (function) "
                        "0)) () \"main\" (noSource))");
    if (b != NULL) {
        const uint32_t expected[] = {MO_OP_CONST, 1,           2,  MO_OP_CONST, 2,
                                     2,           MO_OP_CONST, 3,  2,           MO_OP_BRANCH,
                                     0,           13,          18, MO_OP_CONST, 1,
                                     0,           MO_OP_RET,   1,  MO_OP_CONST, 2,
                                     1,           MO_OP_RET,   2};
        CHECK(b->functions[0].code_count == sizeof expected / sizeof expected[0]);
        CHECK(memcmp(b->functions[0].code, expected, sizeof expected) == 0);
        CHECK(b->constants[0].value.immediate[0] == 17 && b->constants[1].value.immediate[0] == 29);
    }
    mo_arena_free(&arena);
    mo_arena_init(&arena, MO_LIMIT_BYTECODE_SIZE);
    b = compile(&arena,
                "(Program () ((FunDecl \"main\" ((\"o\" (option (int)))) (int) "
                "(case_ (var \"o\") ((\"None\" () (lit (int -5))) (\"Some\" (\"x\") (var \"x\")))) "
                "\"Cases\" (function) 0)) () \"main\" (noSource))");
    if (b != NULL) {
        const uint32_t expected[] = {MO_OP_CONST,
                                     1,
                                     1,
                                     MO_OP_CONST,
                                     2,
                                     1,
                                     MO_OP_CONST,
                                     3,
                                     1,
                                     MO_OP_SWITCH | (2u << 16),
                                     0,
                                     13,
                                     18,
                                     MO_OP_CONST,
                                     1,
                                     0,
                                     MO_OP_RET,
                                     1,
                                     MO_OP_CASE_FIELD,
                                     2,
                                     0,
                                     1,
                                     0,
                                     MO_OP_RET,
                                     2};
        CHECK(b->functions[0].code_count == sizeof expected / sizeof expected[0]);
        CHECK(memcmp(b->functions[0].code, expected, sizeof expected) == 0);
        CHECK(b->types[1].kind == MO_TYPE_CHOICE && b->types[1].case_count == 2);
        CHECK(strcmp(b->types[1].cases[0].name, "None") == 0 &&
              strcmp(b->types[1].cases[1].name, "Some") == 0 && b->types[1].cases[1].unnamed);
    }
    mo_arena_free(&arena);
}
static void test_calls_and_scopes(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_BYTECODE_SIZE);
    struct mo_bytecode *b = compile(
        &arena,
        "(Program () ((FunDecl \"main\" ((\"flag\" (bool))) (int) "
        "(let_ \"n\" (call \"identity\" ((lit (int 7)))) "
        "(if_ (var \"flag\") (call \"identity\" ((var \"n\"))) "
        "(let_ \"n\" (if_ (var \"flag\") (stop \"no result\") (lit (int 9))) (var "
        "\"n\")))) \"M\" (function) 0) "
        "(FunDecl \"identity\" ((\"n\" (int))) (int) (var \"n\") \"M\" (function) 0) "
        "(FunDecl \"identity\" () (bool) (lit (bool true)) \"M\" (function) 0)) () \"main\" "
        "(noSource))");
    if (b != NULL) {
        unsigned calls = 0, tails = 0, stops = 0;
        const struct mo_bytecode_function *f = &b->functions[0];
        for (uint32_t at = 0; at < f->code_count;) {
            const struct mo_opcode_info *op = mo_opcode_find(f->code[at] & MO_OPCODE_MASK);
            CHECK(op != NULL);
            if (op == NULL)
                break;
            if (op->number == MO_OP_CALL) {
                calls++;
                CHECK(f->code[at + 2] == 1);
            }
            if (op->number == MO_OP_TAIL_CALL) {
                tails++;
                CHECK(f->code[at + 1] == 1);
            }
            if (op->number == MO_OP_STOP)
                stops++;
            at += 1u + op->fixed_count + (f->code[at] >> MO_OPCODE_VARIABLE_SHIFT);
        }
        CHECK(calls == 1 && tails == 1 && stops == 1);
        CHECK(b->functions[1].code_count == 2 && b->functions[1].code[0] == MO_OP_RET &&
              b->functions[1].code[1] == 0);
    }
    mo_arena_free(&arena);
    mo_arena_init(&arena, MO_LIMIT_BYTECODE_SIZE);
    b = compile(&arena, "(Program () ((FunDecl \"main\" ((\"x\" (bool)) (\"x\" (int))) (bool) "
                        "(var \"x\") \"M\" (function) 0)) () \"main\" (noSource))");
    if (b != NULL)
        CHECK(b->functions[0].code[0] == MO_OP_RET && b->functions[0].code[1] == 0);
    mo_arena_free(&arena);
    mo_arena_init(&arena, MO_LIMIT_BYTECODE_SIZE);
    b = compile(
        &arena,
        "(Program () ((FunDecl \"M.main\" () (unit) (lit (unit)) \"X.\" (function) 0) "
        "(FunDecl \"main\" () (unit) (lit (unit)) \"X.M\" (function) 0)) () \"main\" (noSource))");
    if (b != NULL)
        CHECK(string_is(b, b->functions[0].name, "X..M.main") &&
              string_is(b, b->functions[1].name, "X.M.main"));
    mo_arena_free(&arena);
}
static void test_metadata(void) {
    struct mo_arena arena;
    struct mo_diags diags;
    struct mo_buffer out;
    mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
    CHECK(mo_diags_init(&diags));
    mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
    struct mo_core *p = parse(
        &arena,
        "(Program ((DataDecl \"Flag\" ((Ctor \"Off\" ()) (Ctor \"On\" ())))) "
        "((FunDecl \"main\" () (int) (let_ \"tmp\" (lit (int 7)) "
        "(if_ (lit (bool true)) (var \"tmp\") (lit (int 9)))) \"M\" (function) 0) "
        "(FunDecl \"law\" ((\"result\" (int))) (int) (lit (int 0)) \"M\" (promise) 0) "
        "(FunDecl \"anchor\" () (int) (call \"law\" ((lit (int 7)))) \"M\" (anchor) 0) "
        "(FunDecl \"test\" () (unit) (lit (unit)) \"M\" (test) 0) "
        "(FunDecl \"example\" () (unit) (lit (unit)) \"M\" (example) 0)) () \"main\" (noSource))",
        &diags);
    if (p != NULL) {
        mo_core_string files[] = {{"absent-A.mo", 11}, {"absent-B.mo", 11}};
        uint32_t owned[] = {0, 1, 2, 3, 4}, data[] = {0}, exposed[] = {0};
        struct mo_core_exposed_type hidden[] = {{0, false}};
        struct mo_core_module_meta module = {.name = {"M", 1},
                                             .data = {data, 1},
                                             .funs = {owned, 5},
                                             .exposed_types = {hidden, 1},
                                             .exposed_funs = {exposed, 1}};
        struct mo_core_span spans[] = {
            {true, {1, 10, 20}}, {true, {1, 11, 12}}, {0},
            {true, {0, 30, 31}}, {true, {1, 13, 14}}, {true, {0, 32, 33}}};
        struct mo_core_span literal_span[] = {{true, {0, 40, 41}}},
                            anchor_spans[] = {{true, {1, 50, 60}}, {0}};
        struct mo_core_binding_meta binding = {0, 0, {"sourceLocal", 11}};
        struct mo_core_name param = {true, {"sourceResult", 12}};
        struct mo_core_spec_line condition = {{true, {1, 70, 75}}, {"result >= 0", 11}};
        struct mo_core_fun_meta meta[5] = {
            {.name = {"M.main", 6},
             .span = {true, {1, 1, 99}},
             .body = {.spans = {spans, 6}, .bindings = {&binding, 1}}},
            {.name = {"Original.go.promise.1", 21},
             .span = {true, {0, 2, 90}},
             .text = {true, {"result is nonnegative", 21}},
             .body = {.spans = {literal_span, 1}, .params = {&param, 1}},
             .subject = {true, 0},
             .lines = {&condition, 1}},
            {.name = {"Original.go.promise.1.allows.1", 30},
             .span = {true, {1, 3, 80}},
             .text = {true, {"allows 7", 8}},
             .body = {.spans = {anchor_spans, 2}},
             .subject = {true, 1},
             .allows = {true, true}},
            {.name = {"M.test.1", 8},
             .text = {true, {"a test", 6}},
             .body = {.spans = {literal_span, 1}}},
            {.name = {"M.example.1", 11},
             .text = {true, {"an example", 10}},
             .body = {.spans = {literal_span, 1}}},
        };
        /* Let C measure full-name lengths, independently of the supplied literals. */
        for (size_t i = 0; i < 5; i++)
            meta[i].name.length = strlen(meta[i].name.bytes);
        p->metadata = (struct mo_core_metadata){
            .source = true, .files = {files, 2}, .modules = {&module, 1}, .funs = {meta, 5}};
        bool ok = mo_compile(p, &out, &diags);
        CHECK(ok && !mo_diags_any(&diags));
        if (!ok && diags.count != 0)
            (void)fprintf(stderr, "%s\n", diags.items[0].primary.text);
        struct mo_load_error error;
        struct mo_bytecode *b =
            ok ? mo_bytecode_decode(&arena, out.bytes, out.length, &error) : NULL;
        CHECK(b != NULL);
        if (b != NULL) {
            CHECK(string_is(b, b->functions[0].name, "M.main"));
            CHECK(string_is(b, b->debug[0].file, "absent-B.mo") && b->debug[0].start == 1 &&
                  b->debug[0].end == 99);
            CHECK(b->debug[0].name_count == 1 &&
                  string_is(b, b->debug[0].names[0].name, "sourceLocal"));
            CHECK(b->debug[0].names[0].code_start < b->debug[0].names[0].code_end);
            bool cleared = false, remapped = false;
            for (uint32_t i = 0; i < b->debug[0].span_count; i++) {
                const struct mo_bytecode_span *s = &b->debug[0].spans[i];
                if (s->file == MO_NO_INDEX)
                    cleared = true;
                if (s->start == 30)
                    remapped = string_is(b, s->file, "absent-A.mo");
            }
            CHECK(cleared && remapped);
            CHECK(b->debug[1].subject == 0 && b->debug[1].line_count == 1 &&
                  string_is(b, b->debug[1].lines[0].text, "result >= 0") &&
                  b->debug[1].lines[0].start == 70);
            CHECK(b->debug[2].subject == 1 && b->debug[2].expectation == MO_BYTECODE_ALLOWS);
            CHECK(b->functions[1].kind == MO_FUNCTION_PROMISE &&
                  b->functions[2].kind == MO_FUNCTION_ANCHOR &&
                  b->functions[3].kind == MO_FUNCTION_TEST &&
                  b->functions[4].kind == MO_FUNCTION_EXAMPLE);
            CHECK(b->modules[0].type_count == 1 && !b->modules[0].types[0].contents_exposed &&
                  b->modules[0].function_count == 1 && b->modules[0].functions[0] == 0);
        }
        mo_buffer_free(&out);
        mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
        meta[0].name = (mo_core_string){"Other.start", 11};
        CHECK(mo_core_check(p, &diags));
        ok = mo_compile(p, &out, &diags);
        CHECK(ok && !mo_diags_any(&diags));
        b = ok ? mo_bytecode_decode(&arena, out.bytes, out.length, &error) : NULL;
        CHECK(b != NULL && string_is(b, b->functions[0].name, "Other.start"));
    }
    mo_buffer_free(&out);
    mo_diags_free(&diags);
    mo_arena_free(&arena);
}
static void test_refusals(void) {
    struct mo_arena arena;
    struct mo_diags diags;
    struct mo_buffer out;
    mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
    CHECK(mo_diags_init(&diags));
    mo_buffer_init(&out, 1);
    struct mo_core *p = parse(&arena,
                              "(Program () ((FunDecl \"main\" () (unit) (lit (unit)) \"M\" "
                              "(function) 0)) () \"main\" (noSource))",
                              &diags);
    if (p != NULL) {
        CHECK(!mo_compile(p, &out, &diags) && out.length == 0);
        CHECK(diags.count == 1 && diags.items[0].code == MO_CODE_BYTECODE_TOO_BIG);
    }
    mo_buffer_free(&out);
    mo_diags_free(&diags);
    CHECK(mo_diags_init(&diags));
    mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
    p = parse(
        &arena,
        "(Program () ((FunDecl \"main\" () (bigInt) (lit (bigInt 17)) \"M\" (function) 0)) () "
        "\"main\" (noSource))",
        &diags);
    if (p != NULL) {
        CHECK(!mo_compile(p, &out, &diags) && out.length == 0);
        CHECK(diags.count == 1 && diags.items[0].code == MO_CODE_CODEGEN_UNSUPPORTED);
        CHECK(strstr(diags.items[0].primary.text, "main:") != NULL);
    }
    mo_diags_free(&diags);
    CHECK(mo_diags_init(&diags));
    p = parse(&arena,
              "(Program () ((FunDecl \"other\" () (unit) (lit (unit)) \"M\" (function) 0)) () "
              "\"other\" (noSource))",
              &diags);
    if (p != NULL) {
        CHECK(mo_core_check(p, &diags));
        bool ok = mo_compile(p, &out, &diags);
        CHECK(ok && !mo_diags_any(&diags));
        struct mo_load_error error;
        struct mo_bytecode *b =
            ok ? mo_bytecode_decode(&arena, out.bytes, out.length, &error) : NULL;
        CHECK(b != NULL && string_is(b, b->functions[0].name, "M.other"));
    }
    mo_buffer_free(&out);
    mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
    mo_diags_free(&diags);
    CHECK(mo_diags_init(&diags));
    p = parse(
        &arena,
        "(Program () ((FunDecl \"other\" () (unit) (lit (unit)) \"M\" (function) 0) "
        "(FunDecl \"main\" () (unit) (lit (unit)) \"M\" (function) 0)) () \"main\" (noSource))",
        &diags);
    if (p != NULL) {
        uint32_t members[] = {0, 1};
        struct mo_core_span absent = {0};
        mo_core_string file = {"absent.mo", 9};
        struct mo_core_fun_meta meta[2] = {
            {.name = {"M.main", 6}, .span = {true, {0, 1, 2}}, .body = {.spans = {&absent, 1}}},
            {.name = {"M.main", 6}, .span = {true, {0, 7, 9}}, .body = {.spans = {&absent, 1}}},
        };
        struct mo_core_module_meta module = {.name = {"M", 1}, .funs = {members, 2}};
        p->metadata = (struct mo_core_metadata){
            .source = true, .files = {&file, 1}, .modules = {&module, 1}, .funs = {meta, 2}};
        CHECK(mo_core_check(p, &diags));
        CHECK(!mo_compile(p, &out, &diags) && out.length == 0);
        CHECK(diags.count == 1 && diags.items[0].code == MO_CODE_CODEGEN_ENTRY &&
              diags.items[0].primary.span.start == 7 &&
              strstr(diags.items[0].primary.text, "not unique") != NULL);
        mo_diags_free(&diags);
        CHECK(mo_diags_init(&diags));
        p->entry = (mo_core_string){"other", 5};
        CHECK(mo_core_check(p, &diags));
        CHECK(!mo_compile(p, &out, &diags) && out.length == 0);
        CHECK(diags.count == 1 && diags.items[0].code == MO_CODE_CODEGEN_ENTRY &&
              diags.items[0].primary.span.start == 1);
    }
    mo_diags_free(&diags);
    CHECK(mo_diags_init(&diags));
    p = parse(
        &arena,
        "(Program () ((FunDecl \"M.main\" () (unit) (lit (unit)) \"X\" (function) 0) "
        "(FunDecl \"main\" () (unit) (lit (unit)) \"X.M\" (function) 0)) () \"main\" (noSource))",
        &diags);
    if (p != NULL) {
        CHECK(mo_core_check(p, &diags));
        CHECK(!mo_compile(p, &out, &diags) && out.length == 0);
        CHECK(diags.count == 1 && diags.items[0].code == MO_CODE_CODEGEN_ENTRY &&
              strstr(diags.items[0].primary.text, "not unique") != NULL);
        mo_diags_free(&diags);
        CHECK(mo_diags_init(&diags));
        p->entry = (mo_core_string){"M.main", 6};
        CHECK(mo_core_check(p, &diags));
        CHECK(!mo_compile(p, &out, &diags) && out.length == 0);
        CHECK(diags.count == 1 && diags.items[0].code == MO_CODE_CODEGEN_ENTRY);
    }
    mo_buffer_free(&out);
    mo_diags_free(&diags);
    mo_arena_free(&arena);
}

static void test_references(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_BYTECODE_SIZE);
    struct mo_bytecode *b =
        compile(&arena, "(Program () ((FunDecl \"main\" ((\"x\" (text))) (text) (var \"x\") "
                        "\"M\" (function) 0)) () \"main\" (noSource))");
    if (b != NULL) {
        const uint32_t expected[] = {MO_OP_DUP, 1, 0, MO_OP_DROP, 0, MO_OP_RET, 1};
        CHECK(b->functions[0].parameters[0].ownership == MO_PARAMETER_OWNED);
        CHECK(b->functions[0].code_count == sizeof expected / sizeof expected[0]);
        CHECK(memcmp(b->functions[0].code, expected, sizeof expected) == 0);
    }
    mo_arena_free(&arena);
}

static MO_MUST_USE const uint32_t *instruction(const struct mo_bytecode_function *f,
                                               enum mo_opcode wanted, uint32_t occurrence) {
    for (uint32_t pc = 0; pc < f->code_count;) {
        const struct mo_opcode_info *op = mo_opcode_find(f->code[pc] & MO_OPCODE_MASK);
        CHECK(op != NULL);
        if (op == NULL)
            return NULL;
        if (op->number == (uint32_t)wanted && occurrence-- == 0)
            return &f->code[pc];
        pc += 1u + op->fixed_count + (f->code[pc] >> MO_OPCODE_VARIABLE_SHIFT);
    }
    CHECK(false);
    return NULL;
}
static void test_globals(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_BYTECODE_SIZE);
    struct mo_bytecode *b = compile(
        &arena, "(Program () ((FunDecl \"start\" () (int) (global \"second\") \"M\" (function) 2) "
                "(FunDecl \"read\" () (int) (global \"first\") \"M\" (function) 1)) "
                "((ConstDecl \"first\" (int) (lit (int 17))) "
                "(ConstDecl \"second\" (int) (call \"read\" ()))) \"start\" (noSource))");
    if (b != NULL) {
        CHECK(b->global_count == 2 && b->init_count == 2 && b->function_count == 6);
        CHECK(string_is(b, b->modules[0].name, "%Core") && b->modules[0].global_count == 2);
        CHECK(b->modules[0].globals[0] == 0 && b->modules[0].globals[1] == 1);
        CHECK(string_is(b, b->globals[0].name, "first") &&
              string_is(b, b->globals[1].name, "second"));
        for (uint32_t i = 0; i < 2; i++) {
            CHECK(b->globals[i].module == 0 && b->types[b->globals[i].type].kind == MO_TYPE_INT);
            uint32_t init = b->init[i], value = init - 1;
            const struct mo_bytecode_function *f = &b->functions[init];
            CHECK(f->code_count == 11);
            if (f->code_count != 11)
                continue;
            const uint32_t expected[] = {MO_OP_CALL,  0, value,      MO_OP_SET_GLOBAL, i, 0,
                                         MO_OP_CONST, 1, f->code[8], MO_OP_RET,        1};
            CHECK(f->kind == MO_FUNCTION_INIT && !f->spec_code && f->parameter_count == 0);
            CHECK(b->types[f->result_type].kind == MO_TYPE_UNIT);
            CHECK(f->code_count == sizeof expected / sizeof expected[0] &&
                  memcmp(f->code, expected, sizeof expected) == 0);
            CHECK(b->types[b->constants[f->code[8]].type].kind == MO_TYPE_UNIT);
        }
        CHECK(string_is(b, b->functions[b->init[0]].name, "first.init"));
        CHECK(string_is(b, b->functions[b->init[0] - 1].name, "first.init.%value"));
        const uint32_t *read = instruction(&b->functions[0], MO_OP_GLOBAL, 0),
                       *call = instruction(&b->functions[b->init[1] - 1], MO_OP_TAIL_CALL, 0);
        if (read != NULL && call != NULL)
            CHECK(read[2] == 1 && call[1] == 1);
    }
    mo_arena_free(&arena);
}
static void test_global_references_and_stop(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_BYTECODE_SIZE);
    struct mo_bytecode *b = compile(
        &arena,
        "(Program () ((FunDecl \"start\" () (text) "
        "(if_ (lit (bool true)) (global \"alias\") (global \"text\")) \"M\" (function) 2) "
        "(FunDecl \"sink\" ((\"x\" (text))) (unit) (lit (unit)) \"M\" (function) 0) "
        "(FunDecl \"borrow\" () (unit) (call \"sink\" ((global \"alias\"))) \"M\" (function) 2)) "
        "((ConstDecl \"text\" (text) (lit (text \"A\\u{0}🙂\"))) "
        "(ConstDecl \"alias\" (text) (global \"text\"))) \"start\" (noSource))");
    if (b != NULL) {
        const struct mo_bytecode_function *f = &b->functions[b->init[1] - 1];
        const uint32_t *global = instruction(f, MO_OP_GLOBAL, 0),
                       *dup = instruction(f, MO_OP_DUP, 0), *ret = instruction(f, MO_OP_RET, 0);
        if (global != NULL && dup != NULL && ret != NULL)
            CHECK(global[2] == 0 && dup[2] == global[1] && ret[1] == dup[1]);
        CHECK(b->functions[1].parameters[0].ownership == MO_PARAMETER_BORROWED);
        const uint32_t *tail = instruction(&b->functions[2], MO_OP_TAIL_CALL, 0),
                       *read = instruction(&b->functions[2], MO_OP_GLOBAL, 0);
        if (tail != NULL && read != NULL)
            CHECK(tail[1] == 1 && tail[3] == read[1]);
        for (uint32_t i = 0; i < b->function_count; i++) {
            const struct mo_bytecode_function *fn = &b->functions[i];
            for (uint32_t pc = 0; pc < fn->code_count;) {
                const struct mo_opcode_info *op = mo_opcode_find(fn->code[pc] & MO_OPCODE_MASK);
                CHECK(op != NULL && op->number != MO_OP_DROP);
                if (op == NULL)
                    break;
                pc += 1u + op->fixed_count + (fn->code[pc] >> MO_OPCODE_VARIABLE_SHIFT);
            }
        }
    }
    mo_arena_free(&arena);
    mo_arena_init(&arena, MO_LIMIT_BYTECODE_SIZE);
    b = compile(&arena,
                "(Program () ((FunDecl \"start\" () (tuple ((text) (int))) (global \"pair\") \"M\" "
                "(function) 2)) ((ConstDecl \"text\" (text) (stop \"do not publish\")) "
                "(ConstDecl \"pair\" (tuple ((text) (int))) "
                "(if_ (lit (bool true)) (tuple ((global \"text\") (lit (int 31)))) "
                "(tuple ((lit (text \"other\")) (lit (int -9))))))) \"start\" (noSource))");
    if (b != NULL) {
        CHECK(b->global_count == 2 && b->init_count == 2);
        const struct mo_bytecode_function *f = &b->functions[b->init[0] - 1];
        const uint32_t *stop = instruction(f, MO_OP_STOP, 0);
        if (stop != NULL)
            CHECK(stop + 2 == f->code + f->code_count);
        CHECK(b->functions[b->init[0]].kind == MO_FUNCTION_INIT);
        const uint32_t *store = instruction(&b->functions[b->init[0]], MO_OP_SET_GLOBAL, 0);
        if (store != NULL)
            CHECK(store[1] == 0);
        const uint32_t *read = instruction(&b->functions[0], MO_OP_GLOBAL, 0),
                       *dup = instruction(&b->functions[0], MO_OP_DUP, 0);
        if (read != NULL && dup != NULL)
            CHECK(read[2] == 1 && dup[2] == read[1]);
    }
    mo_arena_free(&arena);
}
static void test_global_metadata(void) {
    struct mo_arena arena;
    struct mo_diags diags;
    struct mo_buffer out;
    mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
    CHECK(mo_diags_init(&diags));
    mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
    struct mo_core *p =
        parse(&arena,
              "(Program () ((FunDecl \"entry\" () (tuple ((text) (text))) (global \"third\") "
              "\"Main\" (function) 3) "
              "(FunDecl \"relay\" () (text) (global \"first\") \"Hidden\" (function) 1)) "
              "((ConstDecl \"first\" (text) (let_ \"tmp\" (lit (text \"seed\")) (var \"tmp\"))) "
              "(ConstDecl \"second\" (text) (call \"relay\" ())) "
              "(ConstDecl \"third\" (tuple ((text) (text))) "
              "(tuple ((global \"first\") (global \"second\"))))) \"entry\" (noSource))",
              &diags);
    if (p != NULL) {
        mo_core_string files[] = {{"hidden.mo", 9}, {"main.mo", 7}};
        uint32_t hidden_funs[] = {1}, main_funs[] = {0}, hidden_consts[] = {2, 0},
                 main_consts[] = {1};
        struct mo_core_module_meta modules[] = {
            {.name = {"Hidden", 6}, .funs = {hidden_funs, 1}, .consts = {hidden_consts, 2}},
            {.name = {"Main", 4},
             .funs = {main_funs, 1},
             .consts = {main_consts, 1},
             .exposed_funs = {main_funs, 1}}};
        struct mo_core_span read_span = {true, {1, 110, 120}};
        struct mo_core_fun_meta funs[] = {
            {.name = {"Main.start", 10}, .body = {.spans = {&read_span, 1}}},
            {.name = {"Hidden.relay", 12}, .body = {.spans = {&read_span, 1}}}};
        struct mo_core_span first_spans[] = {{true, {0, 20, 24}}, {0}, {true, {1, 30, 34}}},
                            third_spans[] = {{true, {0, 80, 90}}, {0}, {true, {1, 90, 100}}};
        struct mo_core_binding_meta binding = {0, 0, {"sourceName", 10}};
        struct mo_core_const_meta consts[] = {
            {.name = {"Hidden.first", 12},
             .span = {true, {1, 5, 10}},
             .body = {.spans = {first_spans, 3}, .bindings = {&binding, 1}}},
            {.name = {"Main.second", 11},
             .span = {true, {0, 40, 60}},
             .body = {.spans = {&read_span, 1}}},
            {.name = {"Hidden.third", 12}, .body = {.spans = {third_spans, 3}}}};
        p->metadata = (struct mo_core_metadata){.source = true,
                                                .files = {files, 2},
                                                .modules = {modules, 2},
                                                .funs = {funs, 2},
                                                .consts = {consts, 3}};
        bool ok = mo_compile(p, &out, &diags);
        CHECK(ok && !mo_diags_any(&diags));
        if (!ok && diags.count != 0)
            (void)fprintf(stderr, "%s\n", diags.items[0].primary.text);
        struct mo_load_error error;
        struct mo_bytecode *b =
            ok ? mo_bytecode_decode(&arena, out.bytes, out.length, &error) : NULL;
        CHECK(b != NULL);
        if (b != NULL) {
            CHECK(b->global_count == 3 && b->init_count == 3 && b->function_count == 8);
            CHECK(b->modules[0].global_count == 2 && b->modules[0].globals[0] == 2 &&
                  b->modules[0].globals[1] == 0);
            CHECK(b->modules[1].global_count == 1 && b->modules[1].globals[0] == 1 &&
                  b->modules[1].function_count == 1 && b->modules[1].functions[0] == 0);
            CHECK(b->globals[0].module == 0 && b->globals[1].module == 1 &&
                  b->globals[2].module == 0 && string_is(b, b->globals[0].name, "Hidden.first"));
            for (uint32_t i = 0; i < 3; i++) {
                const uint32_t *store = instruction(&b->functions[b->init[i]], MO_OP_SET_GLOBAL, 0);
                if (store != NULL)
                    CHECK(store[1] == i);
            }
            uint32_t init = b->init[0], value = init - 1;
            CHECK(string_is(b, b->functions[init].name, "Hidden.first.init"));
            CHECK(string_is(b, b->functions[value].name, "Hidden.first.init.%value"));
            const struct mo_bytecode_debug *d = &b->debug[value], *wrapper = &b->debug[init];
            CHECK(string_is(b, d->file, "main.mo") && d->start == 5 && d->end == 10);
            CHECK(wrapper->file == d->file && wrapper->start == 5 && wrapper->end == 10);
            CHECK(d->name_count == 1 && string_is(b, d->names[0].name, "sourceName") &&
                  d->names[0].code_start < d->names[0].code_end && wrapper->name_count == 0);
            CHECK(wrapper->span_count == 2 && wrapper->spans[0].code_offset == 0 &&
                  wrapper->spans[0].start == 20 &&
                  string_is(b, wrapper->spans[0].file, "hidden.mo") &&
                  wrapper->spans[1].code_offset == 3 && wrapper->spans[1].start == 5 &&
                  string_is(b, wrapper->spans[1].file, "main.mo"));
            bool cleared = false, remapped = false;
            for (uint32_t i = 0; i < d->span_count; i++) {
                cleared |= d->spans[i].file == MO_NO_INDEX;
                remapped |= d->spans[i].start == 30 && string_is(b, d->spans[i].file, "main.mo");
            }
            CHECK(cleared && remapped);
        }
    }
    mo_buffer_free(&out);
    mo_diags_free(&diags);
    mo_arena_free(&arena);
}
static void test_borrow_calls(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_BYTECODE_SIZE);
    struct mo_bytecode *b = compile(
        &arena,
        "(Program () ((FunDecl \"main\" ((\"x\" (text))) (text) "
        "(let_ \"y\" (call \"pick\" ((var \"x\") (var \"x\"))) (var \"y\")) \"M\" (function) 0) "
        "(FunDecl \"pick\" ((\"x\" (text)) (\"ignored\" (text))) (text) "
        "(var \"x\") \"M\" (function) 0)) () \"main\" (noSource))");
    if (b != NULL) {
        CHECK(b->functions[0].parameters[0].ownership == MO_PARAMETER_OWNED);
        CHECK(b->functions[1].parameters[0].ownership == MO_PARAMETER_OWNED &&
              b->functions[1].parameters[1].ownership == MO_PARAMETER_BORROWED);
        const uint32_t *call = instruction(&b->functions[0], MO_OP_CALL, 0),
                       *dup = instruction(&b->functions[0], MO_OP_DUP, 0);
        if (call != NULL && dup != NULL)
            CHECK(call[2] == 1 && call[3] == dup[1] && call[4] == 0 && dup[2] == 0);
    }
    mo_arena_free(&arena);
    mo_arena_init(&arena, MO_LIMIT_BYTECODE_SIZE);
    b = compile(
        &arena,
        "(Program () ((FunDecl \"main\" ((\"x\" (text))) (unit) "
        "(call \"relay\" ((proj (tuple ((var \"x\") (lit (int 8)))) 0))) \"M\" (function) 0) "
        "(FunDecl \"relay\" ((\"x\" (text))) (unit) (call \"sink\" ((var \"x\"))) \"M\" (function) "
        "0) "
        "(FunDecl \"sink\" ((\"x\" (text))) (unit) (lit (unit)) \"M\" (function) 0) "
        "(FunDecl \"unused\" ((\"x\" (text))) (unit) (lit (unit)) \"M\" (function) 0)) "
        "() \"main\" (noSource))");
    if (b != NULL) {
        for (uint32_t i = 0; i < 3; i++)
            CHECK(b->functions[i].parameters[0].ownership == MO_PARAMETER_OWNED);
        CHECK(b->functions[3].parameters[0].ownership == MO_PARAMETER_BORROWED);
        const uint32_t *tail = instruction(&b->functions[0], MO_OP_TAIL_CALL, 0);
        if (tail != NULL)
            CHECK(tail[1] == 1);
        tail = instruction(&b->functions[1], MO_OP_TAIL_CALL, 0);
        if (tail != NULL)
            CHECK(tail[1] == 2);
        CHECK(b->layout_count == 1 && b->layouts[0].kind == MO_LAYOUT_TUPLE);
    }
    mo_arena_free(&arena);
}

static void test_aggregate_values(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_BYTECODE_SIZE);
    struct mo_bytecode *b = compile(
        &arena,
        "(Program ((DataDecl \"Pair\" ((Ctor \"Pair\" ((\"left\" (text)) (\"right\" (text))))))) "
        "((FunDecl \"main\" ((\"pair\" (data \"Pair\")) (\"b\" (bool))) (text) "
        "(case_ (var \"pair\") ((\"Pair\" (\"x\" \"y\") (if_ (var \"b\") (var \"x\") (var "
        "\"y\"))))) "
        "\"M\" (function) 0) (FunDecl \"build\" ((\"x\" (text))) (data \"Pair\") "
        "(con (data \"Pair\") \"Pair\" ((var \"x\") (var \"x\"))) \"M\" (function) 0) "
        "(FunDecl \"bytes\" () (tuple ((bytes) (bytes))) "
        "(tuple ((lit (bytes (0 128 255))) (lit (bytes (0 128 255))))) \"M\" (function) 0) "
        "(FunDecl \"literal\" () (text) (lit (text \"A\\u{0}🙂\")) \"M\" (function) 0)) "
        "() \"main\" (noSource))");
    if (b != NULL) {
        CHECK(b->functions[0].parameters[0].ownership == MO_PARAMETER_BORROWED);
        CHECK(b->functions[1].parameters[0].ownership == MO_PARAMETER_OWNED);
        CHECK(b->layout_count == 2 && b->layouts[0].kind == MO_LAYOUT_CASE &&
              b->layouts[0].field_count == 2 && b->layouts[1].kind == MO_LAYOUT_TUPLE);
        const uint32_t *con = instruction(&b->functions[1], MO_OP_MAKE_CASE, 0);
        if (con != NULL)
            CHECK(con[2] == b->functions[1].result_type && con[3] == 0 && con[4] != con[5]);
        const uint8_t bytes[] = {0, 128, 255};
        uint32_t found = 0;
        for (uint32_t i = 0; i < b->constant_count; i++) {
            const struct mo_bytecode_constant *k = &b->constants[i];
            if (b->types[k->type].kind == MO_TYPE_BYTES && k->value.bytes.length != 0) {
                CHECK(k->value.bytes.length == sizeof bytes &&
                      memcmp(k->value.bytes.bytes, bytes, sizeof bytes) == 0);
                found++;
            }
        }
        CHECK(found == 1);
        const uint32_t *lit = instruction(&b->functions[3], MO_OP_CONST, 1);
        if (lit != NULL) {
            uint32_t id = b->constants[lit[2]].value.string;
            CHECK(b->strings[id].length == 6 && memcmp(b->strings[id].bytes, "A\0🙂", 6) == 0);
        }
    }
    mo_arena_free(&arena);
    mo_arena_init(&arena, MO_LIMIT_BYTECODE_SIZE);
    b = compile(
        &arena,
        "(Program () ((FunDecl \"main\" ((\"b\" (bool)) (\"x\" (text))) (text) "
        "(let_ \"chosen\" (if_ (var \"b\") (var \"x\") (stop \"stopped\")) (var \"chosen\")) "
        "\"M\" (function) 0) (FunDecl \"spec\" ((\"unused\" (text))) (int) "
        "(lit (int 0)) \"M\" (promise) 0)) () \"main\" (noSource))");
    if (b != NULL) {
        CHECK(b->functions[0].parameters[1].ownership == MO_PARAMETER_OWNED);
        CHECK(b->functions[1].parameters[0].ownership == MO_PARAMETER_OWNED);
        const uint32_t *drop = instruction(&b->functions[1], MO_OP_DROP, 0);
        if (drop != NULL)
            CHECK(drop[1] == 0);
    }
    mo_arena_free(&arena);
    mo_arena_init(&arena, MO_LIMIT_BYTECODE_SIZE);
    b = compile(&arena, "(Program () ((FunDecl \"main\" () (result (tuple ((text) (option (int)))) "
                        "(tuple ((bool) (text)))) (con (result (tuple ((text) (option (int)))) "
                        "(tuple ((bool) (text)))) \"Ok\" ((tuple ((lit (text \"a\")) "
                        "(con (option (int)) \"Some\" ((lit (int 7)))))))) \"M\" (function) 0)) "
                        "() \"main\" (noSource))");
    if (b != NULL) {
        const struct mo_type *result = &b->types[b->functions[0].result_type];
        CHECK(strcmp(result->name, "Result((Text, Option(Int)), (Bool, Text))") == 0);
        uint32_t ok = result->cases[0].fields[0].type, error = result->cases[1].fields[0].type;
        CHECK(ok != error && b->types[ok].kind == MO_TYPE_TUPLE &&
              b->types[error].kind == MO_TYPE_TUPLE);
    }
    mo_arena_free(&arena);
}

static void test_range_and_fail(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_BYTECODE_SIZE);
    struct mo_bytecode *b = compile(
        &arena,
        "(Program () ((FunDecl \"main\" ((\"start\" (int)) (\"end\" (int))) (tuple ((int) (int))) "
        "(let_ \"r\" (prim (range (make)) ((var \"start\") (var \"end\"))) "
        "(tuple ((prim (range (start)) ((var \"r\"))) (prim (range (end)) ((var \"r\")))))) "
        "\"M\" (function) 0) (FunDecl \"failed\" ((\"b\" (bool)) (\"why\" (text))) (int) "
        "(if_ (var \"b\") (fail (var \"why\")) (lit (int 23))) \"M\" (promise) 0)) "
        "() \"main\" (noSource))");
    if (b != NULL) {
        const uint32_t *make = instruction(&b->functions[0], MO_OP_MAKE_RANGE, 2),
                       *move = instruction(&b->functions[0], MO_OP_MOVE, 0),
                       *start = instruction(&b->functions[0], MO_OP_RANGE_START, 0),
                       *end = instruction(&b->functions[0], MO_OP_RANGE_END, 0);
        if (make != NULL && move != NULL && start != NULL && end != NULL)
            CHECK(make[2] == 0 && make[3] == 1 && move[2] == make[1] && start[2] == move[1] &&
                  end[2] == move[1] && start[1] != end[1]);
        CHECK(b->functions[1].parameters[1].ownership == MO_PARAMETER_OWNED);
        const uint32_t *fail = instruction(&b->functions[1], MO_OP_FAIL, 0);
        if (fail != NULL)
            CHECK(fail[1] == 1);
    }
    mo_arena_free(&arena);
}

static void test_representation_refusals(void) {
    const struct {
        const char *data, *params, *result, *body, *reason;
        uint16_t code;
    } cases[] = {
        {"((DataDecl \"Empty\" ()))", "()", "(unit)", "(lit (unit))", "Empty data",
         MO_CODE_CODEGEN_REPRESENTATION},
        {"((DataDecl \"D\" ((Ctor \"Same\" ()) (Ctor \"Same\" ()))))", "()", "(unit)",
         "(lit (unit))", "Duplicate Core constructor", MO_CODE_CODEGEN_REPRESENTATION},
        {"()", "((\"x\" (data \"Missing\")))", "(unit)", "(lit (unit))", "Undeclared data",
         MO_CODE_CODEGEN_REPRESENTATION},
        {"()", "()", "(tuple ())", "(tuple ())", "Zero/one-element tuples",
         MO_CODE_CODEGEN_REPRESENTATION},
        {"()", "()", "(tuple ((int)))", "(tuple ((lit (int 2))))", "Zero/one-element tuples",
         MO_CODE_CODEGEN_REPRESENTATION},
        {"()", "()", "(unit)", "(let_ \"unused\" (tuple ()) (lit (unit)))",
         "Zero/one-element tuples", MO_CODE_CODEGEN_REPRESENTATION},
        {"((DataDecl \"D\" ((Ctor \"\\u{0}\" ()))))", "()", "(unit)", "(lit (unit))",
         "NUL-containing", MO_CODE_CODEGEN_REPRESENTATION},
        {"((DataDecl \"\" ((Ctor \"C\" ()))))", "()", "(unit)", "(lit (unit))", "Empty or NUL",
         MO_CODE_CODEGEN_REPRESENTATION},
        {"()", "((\"\" (int)))", "(int)", "(var \"\")", "Empty or NUL",
         MO_CODE_CODEGEN_REPRESENTATION},
        {"()", "()", "(int)", "(let_ \"x\\u{0}\" (lit (int 2)) (var \"x\\u{0}\"))", "Empty or NUL",
         MO_CODE_CODEGEN_REPRESENTATION},
        {"()", "((\"o\" (option (int))))", "(int)",
         "(case_ (var \"o\") ((\"None\" () (lit (int 1))) (\"Some\" (\"\") (var \"\"))))",
         "Empty or NUL", MO_CODE_CODEGEN_REPRESENTATION},
        {"((DataDecl \"D\" ((Ctor \"C\" ((\"x\" (int)) (\"x\" (text)))))))", "()", "(unit)",
         "(lit (unit))", "duplicate named fields", MO_CODE_CODEGEN_REPRESENTATION},
        {"((DataDecl \"D\" ((Ctor \"C\" ((\"\" (int)) (\"x\" (text)))))))", "()", "(unit)",
         "(lit (unit))", "Empty or duplicate", MO_CODE_CODEGEN_REPRESENTATION},
        {"()", "((\"b\" (bool)) (\"x\" (text)))", "(text)",
         "(let_ \"y\" (if_ (var \"b\") (proj (tuple ((var \"x\") (lit (int 1)))) 0) (var \"x\")) "
         "(var \"y\"))",
         "Reconverging branches", MO_CODE_CODEGEN_UNSUPPORTED},
        {"()", "((\"b\" (bool)) (\"x\" (text)))", "(text)",
         "(let_ \"y\" (if_ (var \"b\") (if_ (var \"b\") (var \"x\") (lit (text \"z\"))) (var "
         "\"x\")) (var \"y\"))",
         "Reconverging branches", MO_CODE_CODEGEN_UNSUPPORTED},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        struct mo_arena arena;
        struct mo_diags diags;
        struct mo_buffer payload, out;
        mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
        CHECK(mo_diags_init(&diags));
        mo_buffer_init(&payload, MO_LIMIT_CORE_TEXT_SIZE);
        mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
        CHECK(mo_buffer_append_text(&payload, "(Program "));
        CHECK(mo_buffer_append_text(&payload, cases[i].data));
        CHECK(mo_buffer_append_text(&payload, " ((FunDecl \"main\" "));
        CHECK(mo_buffer_append_text(&payload, cases[i].params));
        CHECK(mo_buffer_append_byte(&payload, ' '));
        CHECK(mo_buffer_append_text(&payload, cases[i].result));
        CHECK(mo_buffer_append_byte(&payload, ' '));
        CHECK(mo_buffer_append_text(&payload, cases[i].body));
        CHECK(mo_buffer_append_text(&payload, " \"M\" (function) 0)) () \"main\" (noSource))"));
        CHECK(mo_buffer_append_byte(&payload, 0));
        struct mo_core *p = parse(&arena, (const char *)payload.bytes, &diags);
        if (p != NULL) {
            CHECK(mo_core_check(p, &diags));
            CHECK(!mo_compile(p, &out, &diags) && out.length == 0);
            CHECK(diags.count == 1 && diags.items[0].code == cases[i].code &&
                  strstr(diags.items[0].primary.text, cases[i].reason) != NULL);
        }
        mo_buffer_free(&out);
        mo_buffer_free(&payload);
        mo_diags_free(&diags);
        mo_arena_free(&arena);
    }
}

static void test_lowered_names(void) {
    const mo_core_string invalid[] = {{"", 0}, {"bad\0name", 8}};
    for (size_t i = 0; i < 8; i++) {
        struct mo_arena arena;
        struct mo_diags diags;
        struct mo_buffer out;
        mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
        CHECK(mo_diags_init(&diags));
        mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
        struct mo_core *p =
            parse(&arena,
                  "(Program () ((FunDecl \"entry\" () (unit) (lit (unit)) \"M\" (function) 0)) () "
                  "\"entry\" (noSource))",
                  &diags);
        if (p != NULL) {
            struct mo_core_const constant = {
                .name = {"M.seed", 6}, .ty = p->funs.items[0].ret, .body = p->funs.items[0].body};
            if (i < 2)
                p->entry = p->funs.items[0].name = invalid[i];
            else if (i < 4)
                p->funs.items[0].module = invalid[i - 2];
            else {
                p->consts = (mo_core_consts){&constant, 1};
                if (i < 6)
                    constant.name = invalid[i - 4];
                else {
                    const char *entry = i == 6 ? "seed.init" : "seed.init.%value";
                    p->entry = p->funs.items[0].name = (mo_core_string){entry, strlen(entry)};
                }
            }
            CHECK(mo_core_check(p, &diags));
            CHECK(!mo_compile(p, &out, &diags) && out.length == 0);
            uint16_t code = i < 6 ? MO_CODE_CODEGEN_REPRESENTATION : MO_CODE_CODEGEN_ENTRY;
            CHECK(diags.count == 1 && diags.items[0].code == code &&
                  strstr(diags.items[0].reason, i < 6 ? "Empty or NUL" : "not unique") != NULL);
        }
        mo_buffer_free(&out);
        mo_diags_free(&diags);
        mo_arena_free(&arena);
    }
    /* The empty label of a single field means unnamed, not an invalid named field. */
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_BYTECODE_SIZE);
    struct mo_bytecode *b = compile(
        &arena,
        "(Program ((DataDecl \"Box\" ((Ctor \"Box\" ((\"\" (int))))))) "
        "((FunDecl \"entry\" () (data \"Box\") (con (data \"Box\") \"Box\" ((lit (int 19)))) "
        "\"M\" (function) 0)) () \"entry\" (noSource))");
    if (b != NULL) {
        const struct mo_type *t = &b->types[b->functions[0].result_type];
        CHECK(t->kind == MO_TYPE_CHOICE && t->case_count == 1 && t->cases[0].unnamed &&
              t->cases[0].field_count == 1 && t->cases[0].fields[0].name == NULL);
    }
    mo_arena_free(&arena);
}

static void test_register_limit(void) {
    for (uint32_t variant = 0; variant < 4; variant++) {
        uint32_t extra = variant % 2;
        bool global = variant >= 2;
        /* One basic block keeps this register boundary independent of verification's
         * registers × blocks limit. Tuple fields, result and return duplicate fill the frame. */
        uint32_t count = MO_LIMIT_FUNCTION_REGISTERS - 2u + extra;
        struct mo_buffer text, out;
        struct mo_arena arena;
        struct mo_diags diags;
        mo_buffer_init(&text, MO_LIMIT_CORE_TEXT_SIZE);
        mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
        mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
        CHECK(mo_diags_init(&diags));
        CHECK(mo_buffer_append_text(&text, "(core " MO_CORE_TEXT_VERSION
                                           " (Program () ((FunDecl \"main\" () (tuple ("));
        for (uint32_t i = 0; i < count; i++)
            CHECK(mo_buffer_append_text(&text, "(int)"));
        CHECK(mo_buffer_append_text(&text, ")) (tuple ("));
        for (uint32_t i = 0; i < count; i++)
            CHECK(mo_buffer_append_text(&text, "(lit (int 1))"));
        CHECK(mo_buffer_append_text(&text, ")) \"M\" (function) 0)) () \"main\" (noSource)))"));
        struct mo_core *p = mo_core_parse(&arena, (const char *)text.bytes, text.length, &diags);
        CHECK(p != NULL);
        if (p != NULL) {
            struct mo_core_const constant = {
                .name = {"large", 5}, .ty = p->funs.items[0].ret, .body = p->funs.items[0].body};
            struct mo_core_expr read = {.tag = MO_CORE_EXPR_GLOBAL, .as.global = {"large", 5}};
            if (global) {
                p->consts = (mo_core_consts){&constant, 1};
                p->funs.items[0].body = &read;
                p->funs.items[0].const_count = (mo_core_natural){.digits = {"1", 1}};
            }
            bool ok = mo_compile(p, &out, &diags);
            CHECK(ok == (extra == 0));
            if (ok) {
                struct mo_load_error error;
                struct mo_image *image = mo_load(out.bytes, out.length, &error);
                CHECK(image != NULL);
                if (image != NULL) {
                    CHECK(mo_image_bytecode(image)->functions[global ? 1 : 0].register_count ==
                          MO_LIMIT_FUNCTION_REGISTERS);
                    mo_image_free(image);
                }
            } else {
                CHECK(out.length == 0 && diags.count == 1 &&
                      diags.items[0].code == MO_CODE_TOO_MANY_REGISTERS);
            }
        }
        mo_diags_free(&diags);
        mo_arena_free(&arena);
        mo_buffer_free(&out);
        mo_buffer_free(&text);
    }
}

static void test_register_block_limit(void) {
    /* This allocator needs count+3 registers and count+1 blocks. Wide tail-call operands
     * give the verifier enough code words for its separate work budget at the product bound. */
    uint32_t boundary = 1;
    while ((uint64_t)(boundary + 4u) * (boundary + 2u) <= MO_LIMIT_FUNCTION_REGISTER_BLOCKS)
        boundary++;
    for (uint32_t extra = 0; extra < 3; extra++) {
        bool late_register = extra == 2;
        uint32_t count = boundary + extra;
        if (late_register) {
            /* The last tail argument's dup crosses the bound within an existing block. */
            count = 1;
            while (UINT64_C(2) * (count + 2u) * (count + 2u) <= MO_LIMIT_FUNCTION_REGISTER_BLOCKS)
                count++;
        }
        const char *value_type = late_register ? "(text)" : "(unit)";
        uint32_t arity = late_register ? 1u : 100u;
        struct mo_buffer text, out;
        struct mo_arena arena;
        struct mo_diags diags;
        mo_buffer_init(&text, MO_LIMIT_CORE_TEXT_SIZE);
        mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
        mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
        CHECK(mo_diags_init(&diags));
        CHECK(mo_buffer_append_text(&text,
                                    "(core " MO_CORE_TEXT_VERSION " (Program ((DataDecl \"D\" ("));
        for (uint32_t i = 0; i < count; i++) {
            CHECK(mo_buffer_append_text(&text, "(Ctor \"C"));
            CHECK(mo_buffer_append_uint(&text, i));
            CHECK(mo_buffer_append_text(&text, "\" ())"));
        }
        CHECK(mo_buffer_append_text(&text, ")))((FunDecl \"main\" ((\"d\" (data \"D\")) "
                                           "(\"u\" "));
        CHECK(mo_buffer_append_text(&text, value_type));
        CHECK(mo_buffer_append_text(&text, ")) "));
        CHECK(mo_buffer_append_text(&text, value_type));
        CHECK(mo_buffer_append_text(&text, " (case_ (var \"d\") ("));
        for (uint32_t i = 0; i < count; i++) {
            CHECK(mo_buffer_append_text(&text, "(\"C"));
            CHECK(mo_buffer_append_uint(&text, i));
            CHECK(mo_buffer_append_text(&text, "\" () (call \"sink\" ("));
            for (uint32_t j = 0; j < arity; j++)
                CHECK(mo_buffer_append_text(&text, "(var \"u\")"));
            CHECK(mo_buffer_append_text(&text, ")))"));
        }
        CHECK(mo_buffer_append_text(&text, ")) \"M\" (function) 0)(FunDecl \"sink\" ("));
        for (uint32_t j = 0; j < arity; j++) {
            CHECK(mo_buffer_append_text(&text, "(\"p\" "));
            CHECK(mo_buffer_append_text(&text, value_type));
            CHECK(mo_buffer_append_byte(&text, ')'));
        }
        CHECK(mo_buffer_append_text(&text, ") "));
        CHECK(mo_buffer_append_text(&text, value_type));
        CHECK(mo_buffer_append_text(&text, late_register ? " (var \"p\")" : " (lit (unit))"));
        CHECK(mo_buffer_append_text(&text, " \"M\" (function) 0)) () \"main\" (noSource)))"));
        struct mo_core *p = mo_core_parse(&arena, (const char *)text.bytes, text.length, &diags);
        CHECK(p != NULL);
        if (p != NULL) {
            CHECK(mo_core_check(p, &diags));
            bool ok = mo_compile(p, &out, &diags);
            CHECK(ok || extra != 0);
            if (ok) {
                struct mo_load_error error;
                struct mo_image *image = mo_load(out.bytes, out.length, &error);
                CHECK(image != NULL && diags.count == 0);
                if (image != NULL) {
                    CHECK((uint64_t)mo_image_bytecode(image)->functions[0].register_count *
                              (count + 1u) <=
                          MO_LIMIT_FUNCTION_REGISTER_BLOCKS);
                    mo_image_free(image);
                }
            } else {
                CHECK(out.length == 0 && diags.count == 1 &&
                      diags.items[0].code == MO_CODE_BYTECODE_TOO_BIG);
                if (diags.count == 1)
                    CHECK(strstr(diags.items[0].primary.text,
                                 "MO_LIMIT_FUNCTION_REGISTER_BLOCKS") != NULL);
            }
        }
        mo_diags_free(&diags);
        mo_arena_free(&arena);
        mo_buffer_free(&out);
        mo_buffer_free(&text);
    }
}

/* Batch corpus driver: malformed documents and ill-typed programs are not compiler inputs.
 * Lowering-impossible forms are excluded; unfinished forms are not passed off as compilation. */
static MO_MUST_USE int corpus(int argc, char **argv) {
    unsigned compiled = 0, unsupported = 0, excluded = 0, entry = 0, limited = 0, rejected = 0;
    for (int i = 1; i < argc; i++) {
        struct mo_sources sources;
        struct mo_source_error source_error;
        uint32_t file;
        CHECK(mo_sources_init(&sources));
        bool read = mo_sources_add(&sources, argv[i], &file, &source_error);
        CHECK(read);
        if (!read) {
            mo_sources_free(&sources);
            continue;
        }
        bool seen = false;
        const enum mo_core_text_kind kinds[] = {MO_CORE_TEXT_PROGRAM, MO_CORE_TEXT_RUN};
        for (size_t j = 0; !seen && j < 2; j++) {
            struct mo_arena arena;
            struct mo_diags diags;
            mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
            CHECK(mo_diags_init(&diags));
            const struct mo_source *s = &sources.files[file];
            void *parsed =
                mo_core_text_parse(&arena, kinds[j], (const char *)s->bytes, s->length, &diags);
            if (parsed != NULL) {
                seen = true;
                const struct mo_core *p = kinds[j] == MO_CORE_TEXT_PROGRAM
                                              ? parsed
                                              : &((struct mo_core_run *)parsed)->program;
                if (mo_core_check(p, &diags)) {
                    struct mo_buffer first, second;
                    mo_buffer_init(&first, MO_LIMIT_BYTECODE_SIZE);
                    mo_buffer_init(&second, MO_LIMIT_BYTECODE_SIZE);
                    bool ok = mo_compile(p, &first, &diags);
                    CHECK(mo_compile(p, &second, &diags) == ok);
                    CHECK(first.length == second.length);
                    if (ok) {
                        compiled++;
                        CHECK(memcmp(first.bytes, second.bytes, first.length) == 0);
                    } else {
                        uint16_t code = diags.count == 0 ? 0 : diags.items[0].code;
                        bool limit = code == MO_CODE_TOO_MANY_REGISTERS ||
                                     code == MO_CODE_FUNCTION_TOO_LONG ||
                                     code == MO_CODE_BYTECODE_TOO_BIG ||
                                     code == MO_CODE_CODEGEN_MEMORY;
                        if (code == MO_CODE_CODEGEN_ENTRY)
                            entry++;
                        else if (code == MO_CODE_CODEGEN_REPRESENTATION)
                            excluded++;
                        else if (limit)
                            limited++;
                        else
                            unsupported++;
                        CHECK(first.length == 0 && diags.count == 2 &&
                              diags.items[1].code == code &&
                              (limit || code == MO_CODE_CODEGEN_UNSUPPORTED ||
                               code == MO_CODE_CODEGEN_ENTRY ||
                               code == MO_CODE_CODEGEN_REPRESENTATION));
                        if (diags.count != 0 && !limit && code != MO_CODE_CODEGEN_UNSUPPORTED &&
                            code != MO_CODE_CODEGEN_ENTRY && code != MO_CODE_CODEGEN_REPRESENTATION)
                            (void)fprintf(stderr, "%s: %s\n", argv[i], diags.items[0].primary.text);
                    }
                    mo_buffer_free(&first);
                    mo_buffer_free(&second);
                } else
                    rejected++;
            }
            mo_diags_free(&diags);
            mo_arena_free(&arena);
        }
        if (!seen)
            rejected++;
        mo_sources_free(&sources);
    }
    (void)printf(
        "codegen corpus: %u compiled deterministically; %u explicitly unsupported; %u "
        "lowering-impossible inputs skipped; %u entry-name collisions; %u resource limits; %u not "
        "checked programs\n",
        compiled, unsupported, excluded, entry, limited, rejected);
    return mo_test_report("test_codegen corpus");
}
static void test_closure(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_BYTECODE_SIZE);
    struct mo_bytecode *b =
        compile(&arena, "(Program () ((FunDecl \"main\" () (text) "
                        "(app (lam ((\"x\" (text))) (var \"x\")) ((lit (text \"value\")))) "
                        "\"M\" (function) 0)) () \"main\" (noSource))");
    if (b != NULL) {
        CHECK(b->function_count == 2);
        CHECK(b->functions[1].kind == MO_FUNCTION_CLOSURE);
        CHECK(b->functions[1].parameter_count == 1 && b->functions[1].capture_count == 0);
        CHECK(b->functions[1].parameters[0].ownership == MO_PARAMETER_OWNED);
        CHECK(b->types[b->functions[1].register_types[1]].kind == MO_TYPE_ENV);
        const struct mo_bytecode_function *f = &b->functions[0];
        CHECK(f->code_count >= 3 &&
              (f->code[f->code_count - 3] & MO_OPCODE_MASK) == MO_OP_TAIL_CALL_CLOSURE);
    }
    b = compile(&arena, "(Program () ((FunDecl \"start\" () (fn () (int)) "
                        "(lam () (stop \"stopped lambda\")) \"M\" (function) 0)) "
                        "() \"start\" (noSource))");
    if (b != NULL)
        CHECK(b->functions[1].result_type == b->types[b->functions[0].result_type].result);
    b = compile(&arena, "(Program () ((FunDecl \"start\" () (text) "
                        "(app (lam ((\"x\" (text)) (\"x\" (text))) (var \"x\")) "
                        "((lit (text \"first\")) (lit (text \"second\")))) "
                        "\"M\" (function) 0)) () \"start\" (noSource))");
    if (b != NULL) {
        const uint32_t *dup = instruction(&b->functions[1], MO_OP_DUP, 0);
        if (dup != NULL)
            CHECK(dup[2] == 0);
    }
    mo_arena_free(&arena);
}
static void test_captures(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_BYTECODE_SIZE);
    struct mo_bytecode *b = compile(
        &arena, "(Program () ((FunDecl \"main\" ((\"a\" (text)) (\"b\" (int))) "
                "(fn ((int)) (fn () (tuple ((text) (int) (int))))) "
                "(lam ((\"x\" (int))) (lam () (tuple ((var \"a\") (var \"x\") (var \"b\"))))) "
                "\"M\" (function) 0)) () \"main\" (noSource))");
    if (b != NULL) {
        CHECK(b->function_count == 3 && b->functions[1].capture_count == 2 &&
              b->functions[2].capture_count == 3);
        CHECK(string_is(b, b->functions[1].name, "main.fn.1") &&
              string_is(b, b->functions[2].name, "main.fn.2"));
        CHECK(b->functions[0].parameters[0].ownership == MO_PARAMETER_OWNED);
        const uint32_t *a = instruction(&b->functions[1], MO_OP_FIELD, 0),
                       *b_field = instruction(&b->functions[1], MO_OP_FIELD, 1),
                       *dup = instruction(&b->functions[1], MO_OP_DUP, 0),
                       *make = instruction(&b->functions[1], MO_OP_MAKE_CLOSURE, 0);
        if (a != NULL && b_field != NULL && dup != NULL && make != NULL)
            CHECK(a[2] == 1 && a[3] == 0 && b_field[2] == 1 && b_field[3] == 1 && dup[2] == a[1] &&
                  make[2] == 2 && make[3] == dup[1] && make[4] == 0 && make[5] == b_field[1]);
    }
    b = compile(&arena, "(Program () ((FunDecl \"main\" ((\"x\" (text)) (\"x\" (text))) "
                        "(fn () (tuple ((text) (text)))) "
                        "(let_ \"x\" (var \"x\") (lam () (tuple ((var \"x\") (var \"x\"))))) "
                        "\"M\" (function) 0)) () \"main\" (noSource))");
    if (b != NULL) {
        CHECK(b->functions[1].capture_count == 1);
        const uint32_t *dup = instruction(&b->functions[0], MO_OP_DUP, 0),
                       *fields = instruction(&b->functions[1], MO_OP_FIELD, 0),
                       *first = instruction(&b->functions[1], MO_OP_DUP, 0),
                       *second = instruction(&b->functions[1], MO_OP_DUP, 1);
        if (dup != NULL && fields != NULL && first != NULL && second != NULL)
            CHECK(dup[2] == 0 && first[2] == fields[1] && second[2] == fields[1] &&
                  first[1] != second[1]);
    }
    mo_arena_free(&arena);
}
static void test_closure_contexts(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_BYTECODE_SIZE);
    const char *const programs[] = {
        /* Inference falls back to a later case arm, including a nested stopping lambda. */
        "(Program () ((FunDecl \"start\" () (fn () (fn () (int))) "
        "(let_ \"f\" (case_ (con (option (int)) \"Some\" ((lit (int 3)))) "
        "((\"None\" () (lam () (let_ \"x\" (lit (int 1)) (lam () (stop \"stop\"))))) "
        "(\"Some\" (\"x\") (lam () (lam () (lit (int 7))))))) (var \"f\")) "
        "\"M\" (function) 0)) () \"start\" (noSource))",
        "(Program () ((FunDecl \"start\" () (unit) (lit (unit)) \"M\" (function) 0) "
        "(FunDecl \"test\" () (unit) (app (lam ((\"why\" (text))) "
        "(if_ (lit (bool true)) (fail (var \"why\")) (lit (unit)))) "
        "((lit (text \"failed\")))) \"M\" (test) 0)) () \"start\" (noSource))",
        "(Program () ((FunDecl \"start\" () (text) (app (global \"closure\") ()) "
        "\"M\" (function) 2)) ((ConstDecl \"seed\" (text) (lit (text \"seed\"))) "
        "(ConstDecl \"closure\" (fn () (text)) (let_ \"x\" (global \"seed\") "
        "(lam () (var \"x\"))))) \"start\" (noSource))",
        /* A function argument has the signature needed for a stopping lambda. */
        "(Program () ((FunDecl \"start\" () (int) (call \"apply\" "
        "((lam () (stop \"stopped\")))) \"M\" (function) 0) "
        "(FunDecl \"apply\" ((\"f\" (fn () (int)))) (int) (app (var \"f\") ()) "
        "\"M\" (function) 0)) () \"start\" (noSource))",
        /* Applications in value position precede a direct tail call. */
        "(Program () ((FunDecl \"start\" ((\"f\" (fn ((text) (text)) (text))) "
        "(\"x\" (text))) (text) (let_ \"y\" (app (var \"f\") ((var \"x\") (var \"x\"))) "
        "(call \"identity\" ((var \"y\")))) \"M\" (function) 0) "
        "(FunDecl \"identity\" ((\"x\" (text))) (text) (var \"x\") \"M\" (function) 0)) "
        "() \"start\" (noSource))",
    };
    for (size_t i = 0; i < sizeof programs / sizeof programs[0]; i++) {
        struct mo_bytecode *b = compile(&arena, programs[i]);
        if (b != NULL && i == 1) {
            CHECK(b->functions[2].kind == MO_FUNCTION_CLOSURE && b->functions[2].spec_code);
            CHECK(b->debug[2].text == MO_NO_INDEX && b->debug[2].subject == MO_NO_INDEX);
        }
        if (b != NULL && i == 4) {
            const uint32_t *call = instruction(&b->functions[0], MO_OP_CALL_CLOSURE, 0);
            if (call != NULL)
                CHECK(call[2] != call[3] && call[3] != call[4]);
            CHECK(b->functions[0].parameters[0].ownership == MO_PARAMETER_OWNED &&
                  b->functions[0].parameters[1].ownership == MO_PARAMETER_OWNED);
        }
        mo_arena_free(&arena);
        mo_arena_init(&arena, MO_LIMIT_BYTECODE_SIZE);
    }
    mo_arena_free(&arena);
}
static void test_closure_metadata(void) {
    struct mo_arena arena;
    struct mo_diags diags;
    struct mo_buffer out;
    mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
    mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
    CHECK(mo_diags_init(&diags));
    struct mo_core *p =
        parse(&arena,
              "(Program () ((FunDecl \"start\" ((\"a\" (text))) "
              "(fn ((text)) (tuple ((text) (text)))) (let_ \"f\" "
              "(lam ((\"x\" (text))) (tuple ((var \"x\") (var \"a\")))) (var \"f\")) "
              "\"M\" (function) 0)) () \"start\" (noSource))",
              &diags);
    if (p != NULL) {
        mo_core_string files[] = {{"outer.mo", 8}, {"inner.mo", 8}};
        uint32_t functions[] = {0};
        struct mo_core_module_meta module = {.name = {"M", 1}, .funs = {functions, 1}};
        struct mo_core_span spans[] = {{true, {0, 10, 40}},   {true, {1, 100, 130}},
                                       {true, {1, 108, 128}}, {0},
                                       {true, {0, 17, 19}},   {true, {0, 35, 36}}};
        struct mo_core_name param = {true, {"alpha", 5}};
        struct mo_core_binding_meta bindings[] = {{0, 0, {"chosen", 6}}, {1, 0, {"arg", 3}}};
        struct mo_core_closure_meta closure = {1, {"M.start.fn.7", 12}};
        struct mo_core_fun_meta meta = {.name = {"M.start", 7},
                                        .span = {true, {0, 1, 50}},
                                        .body = {.spans = {spans, 6},
                                                 .params = {&param, 1},
                                                 .bindings = {bindings, 2},
                                                 .closures = {&closure, 1}}};
        p->metadata = (struct mo_core_metadata){
            .source = true, .files = {files, 2}, .modules = {&module, 1}, .funs = {&meta, 1}};
        bool ok = mo_compile(p, &out, &diags);
        CHECK(ok && !mo_diags_any(&diags));
        struct mo_load_error error;
        struct mo_bytecode *b =
            ok ? mo_bytecode_decode(&arena, out.bytes, out.length, &error) : NULL;
        CHECK(b != NULL);
        if (b != NULL) {
            CHECK(string_is(b, b->functions[1].name, "M.start.fn.7"));
            const struct mo_bytecode_debug *debug = &b->debug[1];
            CHECK(string_is(b, debug->file, "inner.mo") && debug->start == 100 &&
                  debug->end == 130);
            const uint32_t *field = instruction(&b->functions[1], MO_OP_FIELD, 0);
            bool argument_name = false, capture_name = false;
            for (uint32_t i = 0; i < debug->name_count; i++) {
                const struct mo_bytecode_register_name *n = &debug->names[i];
                argument_name |= string_is(b, n->name, "arg") && n->reg == 0 && n->code_start == 0;
                capture_name |= field != NULL && string_is(b, n->name, "alpha") &&
                                n->reg == field[1] &&
                                n->code_start == (uint32_t)(field - b->functions[1].code) &&
                                n->code_end == b->functions[1].code_count;
            }
            CHECK(argument_name && capture_name);
            bool body_span = false;
            for (uint32_t i = 0; i < debug->span_count; i++)
                body_span |=
                    debug->spans[i].start == 108 && string_is(b, debug->spans[i].file, "inner.mo");
            CHECK(body_span);
        }
        mo_buffer_free(&out);
        mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
        closure.name = meta.name;
        CHECK(!mo_compile(p, &out, &diags) && out.length == 0);
        CHECK(diags.count == 1 && diags.items[0].code == MO_CODE_CODEGEN_ENTRY);
    }
    mo_buffer_free(&out);
    mo_diags_free(&diags);
    mo_arena_free(&arena);
}
static void test_spec_closure_metadata(void) {
    struct mo_arena arena;
    struct mo_diags diags;
    struct mo_buffer out;
    mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
    mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
    CHECK(mo_diags_init(&diags));
    struct mo_core *p =
        parse(&arena,
              "(Program () ((FunDecl \"start\" () (unit) (lit (unit)) \"M\" (function) 0) "
              "(FunDecl \"spec\" () (unit) (let_ \"f\" (lam ((\"x\" (text))) (var \"x\")) "
              "(let_ \"ignored\" (app (var \"f\") ((lit (text \"value\")))) (lit (unit)))) "
              "\"M\" (test) 0)) () \"start\" (noSource))",
              &diags);
    if (p != NULL) {
        mo_core_string file = {"spec.mo", 7};
        uint32_t functions[] = {0, 1};
        struct mo_core_module_meta module = {.name = {"M", 1}, .funs = {functions, 2}};
        struct mo_core_span main_span = {0}, spec_spans[8] = {{true, {0, 10, 100}},
                                                              {true, {0, 17, 27}},
                                                              {true, {0, 20, 21}}};
        struct mo_core_binding_meta binding = {1, 0, {"callbackArg", 11}};
        struct mo_core_closure_meta closure = {1, {"M.test.1.fn.42", 14}};
        struct mo_core_fun_meta meta[] = {
            {.name = {"M.start", 7}, .body = {.spans = {&main_span, 1}}},
            {.name = {"M.test.1", 8},
             .span = {true, {0, 10, 100}},
             .text = {true, {"call closure", 12}},
             .body = {
                 .spans = {spec_spans, 8}, .bindings = {&binding, 1}, .closures = {&closure, 1}}}};
        p->metadata = (struct mo_core_metadata){
            .source = true, .files = {&file, 1}, .modules = {&module, 1}, .funs = {meta, 2}};
        bool ok = mo_compile(p, &out, &diags);
        CHECK(ok && !mo_diags_any(&diags));
        struct mo_load_error error;
        struct mo_bytecode *b =
            ok ? mo_bytecode_decode(&arena, out.bytes, out.length, &error) : NULL;
        CHECK(b != NULL);
        if (b != NULL) {
            CHECK(string_is(b, b->debug[1].text, "call closure"));
            CHECK(b->functions[2].spec_code &&
                  string_is(b, b->functions[2].name, "M.test.1.fn.42"));
            CHECK(b->debug[2].text == MO_NO_INDEX && b->debug[2].subject == MO_NO_INDEX &&
                  b->debug[2].line_count == 0 && b->debug[2].expectation == MO_BYTECODE_NOT_ANCHOR);
            CHECK(b->debug[2].start == 17 && b->debug[2].end == 27);
        }
    }
    mo_buffer_free(&out);
    mo_diags_free(&diags);
    mo_arena_free(&arena);
}
static void test_closure_parameter_limit(void) {
    for (uint32_t n = MO_LIMIT_PARAMETERS; n <= MO_LIMIT_PARAMETERS + 1u; n++) {
        struct mo_buffer text, out;
        struct mo_arena arena;
        struct mo_diags diags;
        mo_buffer_init(&text, MO_LIMIT_CORE_TEXT_SIZE);
        mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
        mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
        CHECK(mo_diags_init(&diags));
        CHECK(mo_buffer_append_text(
            &text, "(Program () ((FunDecl \"start\" () (unit) (let_ \"f\" (lam ("));
        for (uint32_t i = 0; i < n; i++)
            CHECK(mo_buffer_append_text(&text, "(\"arg\" (int)) "));
        CHECK(mo_buffer_append_text(
            &text,
            ") (lit (int 42))) (lit (unit))) \"M\" (function) 0)) () \"start\" (noSource))"));
        CHECK(mo_buffer_append_byte(&text, 0));
        struct mo_core *p = parse(&arena, (const char *)text.bytes, &diags);
        if (p != NULL) {
            CHECK(mo_core_check(p, &diags));
            bool ok = mo_compile(p, &out, &diags);
            CHECK(ok == (n == MO_LIMIT_PARAMETERS));
            if (!ok)
                CHECK(out.length == 0 && diags.count == 1 &&
                      diags.items[0].code == MO_CODE_BYTECODE_TOO_BIG &&
                      strstr(diags.items[0].primary.text, "MO_LIMIT_PARAMETERS") != NULL);
        }
        mo_buffer_free(&text);
        mo_buffer_free(&out);
        mo_diags_free(&diags);
        mo_arena_free(&arena);
    }
}
static void test_shared_function_types(void) {
    struct mo_arena arena;
    struct mo_diags diags;
    struct mo_buffer first, second;
    mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
    mo_buffer_init(&first, MO_LIMIT_BYTECODE_SIZE);
    mo_buffer_init(&second, MO_LIMIT_BYTECODE_SIZE);
    CHECK(mo_diags_init(&diags));
    struct mo_core *p = parse(&arena,
                              "(Program () ((FunDecl \"start\" ((\"x\" (int))) (int) (var \"x\") "
                              "\"M\" (function) 0)) () \"start\" (noSource))",
                              &diags);
    if (p != NULL) {
        /* Annotations can share immutable subtrees without expanding their text form. */
        struct mo_core_ty types[45] = {{.tag = MO_CORE_TY_INT}};
        mo_core_ty arguments[45][2] = {{NULL}};
        for (size_t i = 1; i < 45; i++) {
            arguments[i][0] = arguments[i][1] = &types[i - 1];
            types[i] = (struct mo_core_ty){
                .tag = MO_CORE_TY_FN, .as.fn = {.params = {arguments[i], 2}, .ret = &types[0]}};
        }
        p->funs.items[0].params.items[0].ty = p->funs.items[0].ret = &types[44];
        CHECK(mo_core_check(p, &diags));
        bool ok = mo_compile(p, &first, &diags);
        CHECK(ok && !mo_diags_any(&diags));
        CHECK(mo_compile(p, &second, &diags) == ok && first.length == second.length);
        if (ok) {
            CHECK(memcmp(first.bytes, second.bytes, first.length) == 0);
            struct mo_load_error error;
            struct mo_bytecode *b = mo_bytecode_decode(&arena, first.bytes, first.length, &error);
            CHECK(b != NULL);
            if (b != NULL) {
                uint32_t id = b->functions[0].result_type;
                for (size_t i = 44; i != 0; i--) {
                    const struct mo_type *t = &b->types[id];
                    CHECK(t->kind == MO_TYPE_FUNCTION && t->parameter_count == 2);
                    if (t->kind != MO_TYPE_FUNCTION || t->parameter_count != 2)
                        break;
                    CHECK(t->parameters[0] == t->parameters[1] &&
                          b->types[t->result].kind == MO_TYPE_INT);
                    id = t->parameters[0];
                }
                CHECK(b->types[id].kind == MO_TYPE_INT);
            }
        }
    }
    mo_buffer_free(&first);
    mo_buffer_free(&second);
    mo_diags_free(&diags);
    mo_arena_free(&arena);
}
int main(int argc, char **argv) {
    if (argc > 1)
        return corpus(argc, argv);
    test_literals();
    test_control();
    test_calls_and_scopes();
    test_metadata();
    test_refusals();
    test_references();
    test_globals();
    test_global_references_and_stop();
    test_global_metadata();
    test_borrow_calls();
    test_aggregate_values();
    test_range_and_fail();
    test_representation_refusals();
    test_lowered_names();
    test_register_limit();
    test_register_block_limit();
    test_closure();
    test_captures();
    test_closure_contexts();
    test_closure_metadata();
    test_spec_closure_metadata();
    test_closure_parameter_limit();
    test_shared_function_types();
    return mo_test_report("test_codegen");
}
