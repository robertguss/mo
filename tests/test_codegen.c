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
        meta[0].name = (mo_core_string){"Other.main", 10};
        CHECK(mo_core_check(p, &diags));
        CHECK(!mo_compile(p, &out, &diags) && out.length == 0);
        CHECK(diags.count == 1 && diags.items[0].code == MO_CODE_CODEGEN_ENTRY &&
              diags.items[0].primary.span.file == 1 && diags.items[0].primary.span.start == 1);
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
    p = parse(&arena,
              "(Program () ((FunDecl \"main\" () (text) (lit (text \"x\")) \"M\" (function) 0)) () "
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
        CHECK(!mo_compile(p, &out, &diags) && out.length == 0);
        CHECK(diags.count == 1 && diags.items[0].code == MO_CODE_CODEGEN_ENTRY);
    }
    mo_buffer_free(&out);
    mo_diags_free(&diags);
    mo_arena_free(&arena);
}

static void test_register_limit(void) {
    /* A case per temporary now crosses the verifier's register-block ceiling before the
     * register ceiling. One more temporary must still diagnose register overflow earlier,
     * during lowering, rather than reaching the writer. */
    for (uint32_t extra = 0; extra < 2; extra++) {
        uint32_t count = MO_LIMIT_FUNCTION_REGISTERS - 2u + extra;
        struct mo_buffer text, long_name, out;
        struct mo_arena arena;
        struct mo_diags diags;
        mo_buffer_init(&text, MO_LIMIT_CORE_TEXT_SIZE);
        mo_buffer_init(&long_name, MO_LIMIT_STRING_SIZE);
        mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
        mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
        CHECK(mo_diags_init(&diags));
        for (uint32_t i = 0; i < 2u * 1000u * 1000u; i++)
            CHECK(mo_buffer_append_byte(&long_name, 'D'));
        CHECK(
            mo_buffer_append_text(&text, "(core " MO_CORE_TEXT_VERSION " (Program ((DataDecl \""));
        CHECK(mo_buffer_append(&text, long_name.bytes, long_name.length));
        CHECK(mo_buffer_append_text(&text, "\" ("));
        for (uint32_t i = 0; i < count; i++) {
            CHECK(mo_buffer_append_text(&text, "(Ctor \"c"));
            CHECK(mo_buffer_append_uint(&text, i));
            CHECK(mo_buffer_append_text(&text, "\" ())"));
        }
        CHECK(mo_buffer_append_text(&text, "))) ((FunDecl \"main\" ((\"t\" (data \""));
        CHECK(mo_buffer_append(&text, long_name.bytes, long_name.length));
        CHECK(mo_buffer_append_text(&text, "\"))) (int) (case_ (var \"t\") ("));
        for (uint32_t i = 0; i < count; i++) {
            CHECK(mo_buffer_append_text(&text, "(\"c"));
            CHECK(mo_buffer_append_uint(&text, i));
            CHECK(mo_buffer_append_text(&text, "\" () (lit (int 1)))"));
        }
        CHECK(mo_buffer_append_text(&text, ")) \"M\" (function) 0)) () \"main\" (noSource)))"));
        struct mo_core *p = mo_core_parse(&arena, (const char *)text.bytes, text.length, &diags);
        CHECK(p != NULL);
        if (p != NULL) {
            bool ok = mo_compile(p, &out, &diags);
            CHECK(!ok && out.length == 0 && diags.count == 1);
            CHECK(diags.count == 1 &&
                  diags.items[0].code ==
                      (extra == 0 ? MO_CODE_BYTECODE_TOO_BIG : MO_CODE_TOO_MANY_REGISTERS));
            if (extra == 0 && diags.count == 1) {
                CHECK(strcmp(diags.items[0].primary.text, "many: register-block limit") == 0);
            }
        }
        mo_diags_free(&diags);
        mo_arena_free(&arena);
        mo_buffer_free(&out);
        mo_buffer_free(&text);
        mo_buffer_free(&long_name);
    }
}

/* Batch corpus driver: malformed documents and ill-typed programs are not compiler inputs.
 * Partial implementation refusals are counted distinctly, never passed off as compilation. */
static MO_MUST_USE int corpus(int argc, char **argv) {
    unsigned compiled = 0, unsupported = 0, rejected = 0;
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
                        unsupported++;
                        CHECK(first.length == 0 && diags.count == 2 &&
                              (diags.items[0].code == MO_CODE_CODEGEN_UNSUPPORTED ||
                               diags.items[0].code == MO_CODE_CODEGEN_ENTRY));
                        if (diags.count != 0 &&
                            diags.items[0].code != MO_CODE_CODEGEN_UNSUPPORTED &&
                            diags.items[0].code != MO_CODE_CODEGEN_ENTRY)
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
    (void)printf("codegen corpus: %u compiled deterministically; %u explicitly unsupported; %u not "
                 "checked programs\n",
                 compiled, unsupported, rejected);
    return mo_test_report("test_codegen corpus");
}
int main(int argc, char **argv) {
    if (argc > 1)
        return corpus(argc, argv);
    test_literals();
    test_control();
    test_calls_and_scopes();
    test_metadata();
    test_refusals();
    test_register_limit();
    return mo_test_report("test_codegen");
}
