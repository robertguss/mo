#include "base/codes.h"
#include "base/limits.h"
#include "core/core.h"
#include "test.h"

#include <string.h>
#include <time.h>

static void check_program(const char *payload, uint16_t code) {
    struct mo_arena arena;
    struct mo_diags diags;
    struct mo_buffer input;
    mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
    mo_buffer_init(&input, MO_LIMIT_CORE_TEXT_SIZE);
    CHECK(mo_diags_init(&diags));
    CHECK(mo_buffer_append_text(&input, "(core " MO_CORE_TEXT_VERSION " "));
    CHECK(mo_buffer_append_text(&input, payload));
    CHECK(mo_buffer_append_text(&input, ")\n"));
    struct mo_core *p = mo_core_parse(&arena, (const char *)input.bytes, input.length, &diags);
    CHECK(p != NULL && !mo_diags_any(&diags));
    if (p != NULL) {
        CHECK(mo_core_check(p, &diags) == (code == 0));
        CHECK(code == 0 ? !mo_diags_any(&diags) : diags.count == 1);
        if (diags.count == 1) {
            CHECK(diags.items[0].code == code);
            CHECK(diags.items[0].section == NULL && diags.items[0].reason != NULL);
            CHECK(strstr(diags.items[0].message, "bug in Mo") != NULL);
            CHECK(diags.items[0].primary.span.file == UINT32_MAX);
            CHECK(diags.items[0].primary.span.start == 0 && diags.items[0].primary.span.end == 0);
            if (code == MO_CODE_CORE_CHECK_TYPE) {
                CHECK(strstr(diags.items[0].primary.text, "expected ") != NULL);
                CHECK(strstr(diags.items[0].primary.text, "found ") != NULL);
            }
        }
    }
    mo_buffer_free(&input);
    mo_diags_free(&diags);
    mo_arena_free(&arena);
}

static void expression(const char *ret, const char *body, uint16_t code) {
    struct mo_buffer p;
    mo_buffer_init(&p, MO_LIMIT_CORE_TEXT_SIZE);
    CHECK(mo_buffer_append_text(&p, "(Program () ((FunDecl \"main\" () "));
    CHECK(mo_buffer_append_text(&p, ret));
    CHECK(mo_buffer_append_text(&p, " "));
    CHECK(mo_buffer_append_text(&p, body));
    CHECK(mo_buffer_append_text(&p, " \"Main\" (function) 0)) () \"main\" (noSource))"));
    CHECK(mo_buffer_append_byte(&p, 0));
    check_program((const char *)p.bytes, code);
    mo_buffer_free(&p);
}

static void test_tail_and_inference(void) {
    expression("(int)", "(join \"j\" ((\"n\" (int))) (var \"n\") (jump \"j\" ((lit (int 7)))))", 0);
    expression("(int)", "(join \"j\" () (lit (int 7)) (let_ \"x\" (jump \"j\" ()) (var \"x\")))",
               MO_CODE_CORE_CHECK_JUMP);
    expression("(fn () (int))", "(join \"j\" () (stop \"x\") (lam () (jump \"j\" ())))",
               MO_CODE_CORE_CHECK_JUMP);
    /* Expected tuple types do not flow into parts in Lean's checker. The typing relation does
     * allow this tuple, so accepting it would silently substitute a different checker. */
    expression("(tuple ((int)))", "(tuple ((stop \"x\")))", MO_CODE_CORE_CHECK_INFERENCE);
    expression("(int)", "(let_ \"x\" (stop \"x\") (lit (int 1)))", MO_CODE_CORE_CHECK_INFERENCE);
    expression("(int)",
               "(let_ \"x\" (if_ (lit (bool true)) (stop \"x\") (lit (int 7))) (var \"x\"))", 0);
    expression("(int)",
               "(let_ \"x\" (if_ (lit (bool true)) (lit (int 7)) (stop \"x\")) (var \"x\"))", 0);
    expression("(int)", "(let_ \"x\" (join \"j\" () (lit (int 7)) (jump \"j\" ())) (var \"x\"))",
               0);
    expression("(int)", "(let_ \"x\" (join \"j\" () (jump \"j\" ()) (lit (int 7))) (var \"x\"))",
               0);
    expression("(fn ((text)) (int))", "(lam ((\"x\" (text))) (stop \"x\"))", 0);
    expression("(int)",
               "(let_ \"x\" (case_ (con (option (int)) \"None\" ()) ((\"None\" () (stop \"\")) "
               "(\"Some\" (\"n\") (var \"n\")))) (var \"x\"))",
               0);
    /* Inference first fails inside this lambda; checking later supplies the result type. */
    expression("(int)",
               "(let_ \"f\" (if_ (lit (bool true)) (lam () (stop \"\")) (lam () (lit (int 3)))) "
               "(app (var \"f\") ()))",
               0);
    expression("(int)",
               "(let_ \"f\" (if_ (lit (bool true)) (lam () (if_ (lit (bool true)) (stop \"\") "
               "(stop \"\"))) (lam () (lit (text \"right\")))) (app (var \"f\") ()))",
               MO_CODE_CORE_CHECK_TYPE);
    expression(
        "(text)",
        "(let_ \"x\" (if_ (lit (bool true)) (if_ (lit (bool true)) (if_ (lit (bool true)) (stop "
        "\"\") (lit (int 3))) (lit (text \"middle\"))) (lit (text \"outer\"))) (var \"x\"))",
        MO_CODE_CORE_CHECK_TYPE);
    check_program("(Program () ((FunDecl \"main\" ((\"x\" (int)) (\"x\" (text))) (int) (var \"x\") "
                  "\"Main\" (function) 0) (FunDecl \"main\" () (text) (lit (text \"later\")) "
                  "\"Main\" (function) 0)) () \"main\" (noSource))",
                  0);
    check_program(
        "(Program ((DataDecl \"D\" ((Ctor \"A\" ((\"x\" (int)))) (Ctor \"A\" ((\"x\" (text)))))) "
        "(DataDecl \"D\" ((Ctor \"B\" ())))) ((FunDecl \"main\" () (data \"D\") (con (data \"D\") "
        "\"A\" ((lit (int 1)))) \"Main\" (function) 0)) () \"main\" (noSource))",
        0);
    check_program("(Program () ((FunDecl \"main\" () (unit) (lit (unit)) \"Main\" (function) 0) "
                  "(FunDecl \"spec\" () (unit) (let_ \"f\" (lam () (prim (io (consoleFake)) ())) "
                  "(lit (unit))) \"Main\" (test) 0)) () \"main\" (noSource))",
                  0);
    check_program("(Program () ((FunDecl \"main\" () (unit) (lit (unit)) \"Main\" (function) 1)) "
                  "((ConstDecl \"fake\" (fn () (cap (console))) (lam () (prim (io (consoleFake)) "
                  "())))) \"main\" (noSource))",
                  MO_CODE_CORE_CHECK_SPEC_PRIMITIVE);
}

static void test_refusals(void) {
    const struct {
        const char *ret, *body;
        uint16_t code;
    } cases[] = {
        {"(int)", "(var \"absent\")", MO_CODE_CORE_CHECK_VARIABLE},
        {"(int)", "(global \"absent\")", MO_CODE_CORE_CHECK_GLOBAL},
        {"(int)", "(lit (bool true))", MO_CODE_CORE_CHECK_TYPE},
        {"(int)", "(prim (int (add)) ((lit (int 1))))", MO_CODE_CORE_CHECK_ARGUMENTS},
        {"(fn ((int)) (int))", "(lam ((\"x\" (text))) (lit (int 1)))", MO_CODE_CORE_CHECK_LAMBDA},
        {"(int)", "(app (lit (int 1)) ())", MO_CODE_CORE_CHECK_APPLICATION},
        {"(int)", "(call \"absent\" ())", MO_CODE_CORE_CHECK_FUNCTION},
        {"(int)", "(proj (tuple ((lit (text \"a\")) (lit (int 7)))) 2)",
         MO_CODE_CORE_CHECK_PROJECTION},
        {"(int)", "(con (int) \"X\" ())", MO_CODE_CORE_CHECK_CONSTRUCTORS},
        {"(option (int))", "(con (option (int)) \"Error\" ())", MO_CODE_CORE_CHECK_CONSTRUCTOR},
        {"(int)", "(case_ (con (option (int)) \"None\" ()) ())", MO_CODE_CORE_CHECK_ARMS},
        {"(int)",
         "(case_ (con (option (int)) \"None\" ()) ((\"Some\" (\"x\") (var \"x\")) (\"None\" () "
         "(lit (int 7)))))",
         MO_CODE_CORE_CHECK_ARM},
        {"(cap (console))", "(prim (io (consoleFake)) ())", MO_CODE_CORE_CHECK_SPEC_PRIMITIVE},
        {"(int)", "(fail (lit (text \"detail\")))", MO_CODE_CORE_CHECK_FAIL},
        {"(int)", "(jump \"absent\" ())", MO_CODE_CORE_CHECK_JUMP},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        expression(cases[i].ret, cases[i].body, cases[i].code);
    check_program("(Program () () () \"absent\" (noSource))", MO_CODE_CORE_CHECK_ENTRY);
    check_program("(Program () ((FunDecl \"main\" () (unit) (lit (unit)) \"Main\" (test) 0)) () "
                  "\"main\" (noSource))",
                  MO_CODE_CORE_CHECK_ENTRY_KIND);
    check_program(
        "(Program () ((FunDecl \"main\" () (int) (lit (int 1)) \"Main\" (function) 0) (FunDecl "
        "\"spec\" () (int) (lit (int 1)) \"Main\" (test) 0)) () \"main\" (noSource))",
        MO_CODE_CORE_CHECK_KIND);
    check_program("(Program () ((FunDecl \"main\" () (int) (lit (int 1)) \"Main\" (function) 1)) "
                  "() \"main\" (noSource))",
                  MO_CODE_CORE_CHECK_PREFIX);
    check_program(
        "(Program () ((FunDecl \"main\" () (int) (lit (int 1)) \"Main\" (function) 0)) ((ConstDecl "
        "\"c\" (int) (lit (int 1))) (ConstDecl \"c\" (int) (lit (int 2)))) \"main\" (noSource))",
        MO_CODE_CORE_CHECK_CONSTANT_NAME);
    check_program("(Program () ((FunDecl \"main\" () (int) (call \"f\" ()) \"Main\" (function) 0) "
                  "(FunDecl \"f\" () (int) (lit (int 1)) \"Main\" (function) 1)) ((ConstDecl \"c\" "
                  "(int) (lit (int 2)))) \"main\" (noSource))",
                  MO_CODE_CORE_CHECK_CALL_PREFIX);
    check_program(
        "(Program () ((FunDecl \"main\" () (int) (call \"spec\" ()) \"Main\" (function) 0) "
        "(FunDecl \"spec\" () (int) (lit (int 1)) \"Main\" (anchor) 0)) () \"main\" (noSource))",
        MO_CODE_CORE_CHECK_SPEC_CALL);
}

static void test_literal_boundaries(void) {
    const struct {
        const char *ty, *valid, *invalid;
    } cases[] = {
        {"int", "-9223372036854775808", "-9223372036854775809"},
        {"int", "9223372036854775807", "9223372036854775808"},
        {"byte", "255", "256"},
        {"byte", "0", "-1"},
        {"decimal", "170141183460469231731687303715884105727",
         "170141183460469231731687303715884105728"},
        {"decimal", "-170141183460469231731687303715884105727",
         "-170141183460469231731687303715884105728"},
        {"time", "-62135596800000000", "-62135596800000001"},
        {"time", "253402300799999999", "253402300800000000"},
        {"duration", "-9223372036854775808", "9223372036854775808"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char ret[32], body[128];
        int n = snprintf(ret, sizeof ret, "(%s)", cases[i].ty);
        CHECK(n > 0 && (size_t)n < sizeof ret);
        for (size_t j = 0; j < 2; j++) {
            n = snprintf(body, sizeof body, "(lit (%s %s))", cases[i].ty,
                         j == 0 ? cases[i].valid : cases[i].invalid);
            CHECK(n > 0 && (size_t)n < sizeof body);
            expression(ret, body, j == 0 ? 0 : MO_CODE_CORE_CHECK_LITERAL);
        }
    }
    expression("(bytes)", "(lit (bytes (0 255)))", 0);
    expression("(bytes)", "(lit (bytes (0 256)))", MO_CODE_CORE_CHECK_LITERAL);
    expression("(bytes)", "(lit (bytes (-1 255)))", MO_CODE_CORE_CHECK_LITERAL);
    expression("(float)", "(lit (float 0x7ff8000000000001))", 0);
    /* Lean constructs List.replicate at this count before checking arity. C must not. */
    expression("(list (int))", "(prim (list (int) (lit 999999999999999999999999999)) ())",
               MO_CODE_CORE_CHECK_ARGUMENTS);
    expression("(map (text) (int))",
               "(prim (map (text) (int) (lit 999999999999999999999999999)) ())",
               MO_CODE_CORE_CHECK_ARGUMENTS);
}

static void test_limits_and_metadata(void) {
    struct mo_core_ty ty = {.tag = MO_CORE_TY_INT};
    struct mo_core_expr leaf = {.tag = MO_CORE_EXPR_STOP};
    struct mo_core_expr nodes[MO_LIMIT_CORE_DEPTH];
    mo_core_expr body = &leaf;
    for (size_t i = 0; i < MO_LIMIT_CORE_DEPTH; i++) {
        nodes[i] = (struct mo_core_expr){.tag = MO_CORE_EXPR_JOIN};
        nodes[i].as.join.body = &leaf;
        nodes[i].as.join.rest = body;
        body = &nodes[i];
    }
    struct mo_core_fun f = {
        .name = {"main", 4}, .ret = &ty, .body = body, .const_count = {.digits = {"0", 1}}};
    struct mo_core p = {.funs = {&f, 1}, .entry = {"main", 4}};
    struct mo_diags d;
    CHECK(mo_diags_init(&d));
    CHECK(!mo_core_check(&p, &d));
    CHECK(d.count == 1 && d.items[0].code == MO_CODE_CORE_CHECK_LIMIT);
    CHECK(strstr(d.items[0].message, "resource limit") != NULL);
    CHECK(strstr(d.items[0].message, "bug in Mo") == NULL);
    /* At the permitted boundary, every branch still checks and no join recursion executes. */
    f.body = &nodes[MO_LIMIT_CORE_DEPTH - 2];
    CHECK(mo_core_check(&p, &d));
    CHECK(d.count == 1);
    p.metadata.source = true;
    CHECK(!mo_core_check(&p, &d));
    CHECK(d.count == 2 && d.items[1].code == MO_CODE_CORE_CHECK_METADATA);
    /* A name's bytes are not nodes. This fits the interchange and scratch byte limits. */
    struct mo_arena names;
    mo_arena_init(&names, MO_LIMIT_CORE_TEXT_SIZE);
    size_t length = MO_LIMIT_CORE_NODES + 1;
    char *name = mo_arena_allocate(&names, length, 1);
    CHECK(name != NULL);
    if (name != NULL) {
        memset(name, 'x', length);
        ty = (struct mo_core_ty){.tag = MO_CORE_TY_DATA, .as.data = {name, length}};
        p.metadata.source = false;
        CHECK(mo_core_check(&p, &d));
        CHECK(d.count == 2);
    }
    mo_arena_free(&names);
    ty = (struct mo_core_ty){.tag = MO_CORE_TY_INT};
    f.module = (mo_core_string){"Main", 4};
    struct mo_core_span spans[2 * MO_LIMIT_CORE_DEPTH + 1] = {0};
    mo_core_string file = {"main.mo", 7};
    uint32_t index = 0;
    struct mo_core_module_meta mods[3] = {
        {.name = {"Main", 4}, .funs = {&index, 1}}, {.name = {"Other", 5}}, {.name = {"Main", 4}}};
    struct mo_core_fun_meta meta = {.name = {"Main.main", 9},
                                    .body.spans = {spans, 2 * MO_LIMIT_CORE_DEPTH - 1}};
    p.metadata = (struct mo_core_metadata){
        .source = true, .files = {&file, 1}, .modules = {mods, 1}, .funs = {&meta, 1}};
    CHECK(mo_core_metadata_valid(&p));
    f.body = &nodes[MO_LIMIT_CORE_DEPTH - 1];
    meta.body.spans.count += 2;
    CHECK(!mo_core_metadata_valid(&p));
    f.body = &leaf;
    meta.body.spans.count = 1;
    p.metadata.modules.count = 3;
    CHECK(!mo_core_metadata_valid(&p));
    mods[2].name = (mo_core_string){"Third", 5};
    CHECK(mo_core_metadata_valid(&p));
    mo_diags_free(&d);
}

static void test_source_span(void) {
    const char *text =
        "(core " MO_CORE_TEXT_VERSION
        " (Program () ((FunDecl \"main\" () (tuple ((int) (int))) (tuple ((lit (int "
        "7)) (var \"missing\"))) \"Main\" (function) 0)) () \"main\" (Metadata (\"main.mo\") "
        "((ModuleMeta \"Main\" () (0) () () ())) ((FunMeta \"Main.main\" (none) (none) "
        "(BodyMeta ((none) (none) (some (SourceSpan 0 29 40))) () () ()) (none) (none) ())) "
        "())))";
    struct mo_arena arena;
    struct mo_diags diags;
    mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
    CHECK(mo_diags_init(&diags));
    struct mo_core *p = mo_core_parse(&arena, text, strlen(text), &diags);
    CHECK(p != NULL && !mo_diags_any(&diags));
    if (p != NULL) {
        CHECK(!mo_core_check(p, &diags));
        CHECK(diags.count == 1 && diags.items[0].code == MO_CODE_CORE_CHECK_VARIABLE);
        if (diags.count == 1) {
            struct mo_span span = diags.items[0].primary.span;
            CHECK(span.file == 0 && span.start == 29 && span.end == 40);
            CHECK(strstr(diags.items[0].primary.text, "main") != NULL);
            CHECK(strstr(diags.items[0].primary.text, "missing") != NULL);
        }
        struct mo_core_fun_meta *m = &p->metadata.funs.items[0];
        m->span = (struct mo_core_span){.some = true, .value = {0, 10, 20}};
        m->body.spans.items[2].some = false;
        CHECK(!mo_core_check(p, &diags));
        CHECK(diags.count == 2 && diags.items[1].primary.span.start == 10 &&
              diags.items[1].primary.span.end == 20);
        p->funs.items[0].kind = MO_CORE_TEST;
        m->text.some = true;
        m->text.value = (mo_core_string){"spec", 4};
        CHECK(!mo_core_check(p, &diags));
        CHECK(diags.count == 3 && diags.items[2].code == MO_CODE_CORE_CHECK_ENTRY_KIND &&
              diags.items[2].primary.span.start == 10);
        p->funs.items[0].kind = MO_CORE_FUNCTION;
        m->text.some = false;
        p->funs.items[0].const_count.digits = (mo_core_string){"1", 1};
        CHECK(!mo_core_check(p, &diags));
        CHECK(diags.count == 4 && diags.items[3].code == MO_CODE_CORE_CHECK_PREFIX &&
              diags.items[3].primary.span.start == 10);
        p->funs.items[0].const_count.digits = (mo_core_string){"0", 1};
        p->entry = (mo_core_string){"absent", 6};
        CHECK(!mo_core_check(p, &diags));
        CHECK(diags.count == 5 && diags.items[4].code == MO_CODE_CORE_CHECK_ENTRY);
        CHECK(strstr(diags.items[4].primary.text, "absent") != NULL);
        struct mo_core_fun fs[2] = {p->funs.items[0], p->funs.items[0]};
        struct mo_core_fun_meta ms[2] = {*m, *m};
        uint32_t indices[] = {0, 1};
        fs[0].kind = MO_CORE_TEST;
        ms[0].text.some = true;
        fs[1].name = (mo_core_string){"another", 7};
        p->funs.items = fs;
        p->funs.count = 2;
        p->metadata.funs.items = ms;
        p->metadata.funs.count = 2;
        p->metadata.modules.items[0].funs = (mo_core_indices){indices, 2};
        p->entry = fs[1].name;
        CHECK(!mo_core_check(p, &diags));
        CHECK(diags.count == 6 && diags.items[5].code == MO_CODE_CORE_CHECK_KIND &&
              diags.items[5].primary.span.start == 10);
        struct mo_core_const cs[2] = {{.name = {"c", 1}, .ty = fs[1].ret, .body = fs[1].body}};
        cs[1] = cs[0];
        struct mo_core_const_meta cm[2] = {{.name = {"Main.c", 6},
                                            .span = {.some = true, .value = {0, 100, 110}},
                                            .body = ms[1].body}};
        cm[1] = cm[0];
        cm[1].span.value = (struct mo_span){0, 120, 130};
        p->consts = (mo_core_consts){cs, 2};
        p->metadata.consts = (mo_core_const_metas){cm, 2};
        p->metadata.modules.items[0].consts = (mo_core_indices){indices, 2};
        CHECK(!mo_core_check(p, &diags));
        CHECK(diags.count == 7 && diags.items[6].code == MO_CODE_CORE_CHECK_CONSTANT_NAME &&
              diags.items[6].primary.span.start == 120);
    }
    mo_diags_free(&diags);
    mo_arena_free(&arena);
}

static void test_nested_spans(void) {
    struct mo_core_ty integer = {.tag = MO_CORE_TY_INT};
    struct mo_core_ty callable = {.tag = MO_CORE_TY_FN, .as.fn.ret = &integer};
    struct mo_core_ty option = {.tag = MO_CORE_TY_OPTION, .as.option = &integer};
    struct mo_core_expr missing = {.tag = MO_CORE_EXPR_VAR, .as.var = {"missing", 7}};
    struct mo_core_expr stop = {.tag = MO_CORE_EXPR_STOP};
    struct mo_core_expr con = {.tag = MO_CORE_EXPR_CON,
                               .as.con = {.ty = &option, .ctor = {"None", 4}}};
    struct mo_core_arm arms[] = {{.ctor = {"None", 4}, .body = &missing},
                                 {.ctor = {"Some", 4}, .body = &stop}};
    mo_core_string bound = {"n", 1};
    arms[1].names = (mo_core_string_list){&bound, 1};
    struct mo_core_expr roots[] = {
        {.tag = MO_CORE_EXPR_LAM, .as.lam.body = &missing},
        {.tag = MO_CORE_EXPR_JOIN, .as.join = {.name = {"j", 1}, .body = &missing, .rest = &stop}},
        {.tag = MO_CORE_EXPR_CASE, .as.case_ = {.expr = &con, .arms = {arms, 2}}}};
    for (size_t i = 0; i < 4; i++) {
        bool constant = i == 3;
        struct mo_core_fun f = {.name = {"main", 4},
                                .module = {"Main", 4},
                                .ret = i == 0 ? &callable : &integer,
                                .body = constant ? &stop : &roots[i],
                                .const_count.digits = {"0", 1}};
        struct mo_core_const k = {.name = {"c", 1}, .ty = &integer, .body = &roots[1]};
        struct mo_core_span spans[4] = {0};
        spans[i == 2 ? 2 : 1] = (struct mo_core_span){.some = true, .value = {0, 29, 40}};
        struct mo_core_closure_meta closure = {.node = 0, .name = {"Main.lambda", 11}};
        struct mo_core_body_meta body = {.spans = {spans, i == 0 ? 2 : (i == 2 ? 4 : 3)},
                                         .closures = {&closure, i == 0 ? 1 : 0}};
        struct mo_core_span unavailable = {0};
        struct mo_core_fun_meta fm = {.name = {"Main.main", 9}, .body = body};
        if (constant)
            fm.body = (struct mo_core_body_meta){.spans = {&unavailable, 1}};
        struct mo_core_const_meta cm = {.name = {"Main.c", 6}, .body = body};
        uint32_t index = 0;
        struct mo_core_module_meta module = {
            .name = {"Main", 4}, .funs = {&index, 1}, .consts = {&index, constant ? 1 : 0}};
        mo_core_string file = {"main.mo", 7};
        struct mo_core p = {.funs = {&f, 1},
                            .consts = {&k, constant ? 1 : 0},
                            .entry = f.name,
                            .metadata = {.source = true,
                                         .files = {&file, 1},
                                         .modules = {&module, 1},
                                         .funs = {&fm, 1},
                                         .consts = {&cm, constant ? 1 : 0}}};
        struct mo_diags diags;
        CHECK(mo_diags_init(&diags));
        CHECK(mo_core_metadata_valid(&p));
        CHECK(!mo_core_check(&p, &diags));
        CHECK(diags.count == 1 && diags.items[0].code == MO_CODE_CORE_CHECK_VARIABLE);
        CHECK(diags.items[0].primary.span.file == 0 && diags.items[0].primary.span.start == 29 &&
              diags.items[0].primary.span.end == 40);
        CHECK(strstr(diags.items[0].primary.text, constant ? "\"c\"" : "\"main\"") != NULL);
        CHECK(strstr(diags.items[0].primary.text, "missing") != NULL);
        mo_diags_free(&diags);
    }
}

static void test_large_programs(void) {
    struct mo_arena names;
    mo_arena_init(&names, MO_LIMIT_CORE_TEXT_SIZE);
    size_t name_length = 2000000;
    char *name = mo_arena_allocate(&names, name_length, 1);
    CHECK(name != NULL);
    if (name == NULL) {
        mo_arena_free(&names);
        return;
    }
    memset(name, 'd', name_length);
    for (size_t variant = 0; variant < 4; variant++) {
        struct mo_buffer b;
        mo_buffer_init(&b, MO_LIMIT_CORE_TEXT_SIZE);
        CHECK(mo_buffer_append_text(&b, "(core " MO_CORE_TEXT_VERSION " (Program ("));
        if (variant == 2) {
            CHECK(mo_buffer_append_text(&b, "(DataDecl \"D\" ((Ctor \"A\" ("));
            for (size_t i = 0; i < 1000; i++) {
                CHECK(mo_buffer_append_text(&b, "(\"a"));
                CHECK(mo_buffer_append_uint(&b, i));
                CHECK(mo_buffer_append_text(&b, "\" (unit)) "));
            }
            CHECK(mo_buffer_append_text(&b, "))))"));
        } else if (variant == 3) {
            CHECK(mo_buffer_append_text(&b, "(DataDecl \""));
            CHECK(mo_buffer_append(&b, name, name_length));
            CHECK(mo_buffer_append_text(&b, "\" ())"));
        }
        CHECK(mo_buffer_append_text(&b, ") ((FunDecl \"main\" ("));
        if (variant == 0)
            CHECK(mo_buffer_append_text(&b, "(\"b\" (bool)) (\"u\" (unit))"));
        else if (variant == 1) {
            CHECK(mo_buffer_append_text(&b, "(\"p\" (tuple ("));
            for (size_t i = 0; i < 330000; i++) {
                CHECK(mo_buffer_append_text(&b, "(data \""));
                CHECK(mo_buffer_append_uint(&b, i));
                CHECK(mo_buffer_append_text(&b, "\") "));
            }
            CHECK(mo_buffer_append_text(&b, ")))"));
        } else {
            CHECK(mo_buffer_append_text(&b, "(\"d\" (data \""));
            CHECK(variant == 2 ? mo_buffer_append_text(&b, "D")
                               : mo_buffer_append(&b, name, name_length));
            CHECK(mo_buffer_append_text(&b, "\"))"));
        }
        if (variant == 1)
            CHECK(mo_buffer_append_text(&b, ") (unit) (lit (unit))"));
        else {
            size_t count = variant == 0 ? 55000 : (variant == 2 ? 960 : 160000);
            CHECK(mo_buffer_append_text(&b, ") (list (unit)) (prim (list (unit) (lit "));
            CHECK(mo_buffer_append_uint(&b, count));
            CHECK(mo_buffer_append_text(&b, ")) ("));
            for (size_t i = 0; i < count; i++) {
                if (variant == 0)
                    CHECK(mo_buffer_append_text(
                        &b, "(let_ \"x\" (if_ (var \"b\") (stop \"\") (var \"u\")) (var \"x\")) "));
                else if (variant == 2) {
                    CHECK(mo_buffer_append_text(&b, "(let_ \"x\" (case_ (var \"d\") ((\"A\" ("));
                    for (size_t j = 0; j < 1000; j++) {
                        CHECK(mo_buffer_append_text(&b, "\"a"));
                        CHECK(mo_buffer_append_uint(&b, j));
                        CHECK(mo_buffer_append_text(&b, "\" "));
                    }
                    CHECK(mo_buffer_append_text(&b, ") (lit (unit))))) (var \"x\")) "));
                } else
                    CHECK(mo_buffer_append_text(&b, "(case_ (var \"d\") ()) "));
            }
            CHECK(mo_buffer_append_text(&b, "))"));
        }
        CHECK(mo_buffer_append_text(&b, " \"Main\" (function) 0)) () \"main\" (noSource)))"));
        if (variant == 3) {
            CHECK(mo_buffer_append_text(&b, "\n;"));
            size_t padding = MO_LIMIT_CORE_TEXT_SIZE - b.length;
            char *pad = mo_arena_allocate(&names, padding, 1);
            CHECK(pad != NULL);
            if (pad != NULL) {
                memset(pad, '.', padding);
                CHECK(mo_buffer_append(&b, pad, padding));
                CHECK(b.length == MO_LIMIT_CORE_TEXT_SIZE);
            }
        }
        struct mo_arena arena;
        struct mo_diags diags;
        mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
        CHECK(mo_diags_init(&diags));
        struct mo_core *p = mo_core_parse(&arena, (const char *)b.bytes, b.length, &diags);
        CHECK(p != NULL && !mo_diags_any(&diags));
        if (p != NULL) {
            clock_t start = clock();
            CHECK(mo_core_check(p, &diags));
            clock_t end = clock();
            CHECK(!mo_diags_any(&diags));
            if (variant == 3) {
                CHECK(start != (clock_t)-1 && end != (clock_t)-1 && end >= start);
                if (start != (clock_t)-1 && end >= start) {
                    double seconds = (double)(end - start) / (double)CLOCKS_PER_SEC;
                    /* The repeated-name scan took 15 seconds; leave sanitizer/CI margin. */
                    CHECK(seconds < 4.0);
                    CHECK(printf("Core long-name checking: %.3f CPU seconds\n", seconds) >= 0);
                }
            }
        }
        mo_diags_free(&diags);
        mo_arena_free(&arena);
        mo_buffer_free(&b);
    }
    mo_arena_free(&names);
}

static void test_parser_node_limit(void) {
    for (size_t extra = 0; extra < 2; extra++) {
        struct mo_buffer b;
        mo_buffer_init(&b, MO_LIMIT_CORE_TEXT_SIZE);
        CHECK(mo_buffer_append_text(&b,
                                    "(core " MO_CORE_TEXT_VERSION
                                    " (Program () ((FunDecl \"main\" () (bytes) (lit (bytes ("));
        /* This document has 26 wire nodes besides the byte atoms. */
        for (size_t i = 0; i < MO_LIMIT_CORE_NODES - 26 + extra; i++)
            CHECK(mo_buffer_append_text(&b, "0 "));
        CHECK(mo_buffer_append_text(&b, "))) \"Main\" (function) 0)) () \"main\" (noSource)))"));
        struct mo_arena arena;
        struct mo_diags diags;
        mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
        CHECK(mo_diags_init(&diags));
        struct mo_core *p = mo_core_parse(&arena, (const char *)b.bytes, b.length, &diags);
        if (extra == 0) {
            CHECK(p != NULL && !mo_diags_any(&diags));
            if (p != NULL) {
                CHECK(mo_core_check(p, &diags));
                CHECK(!mo_diags_any(&diags));
            }
        } else {
            CHECK(p == NULL && diags.count == 1);
            CHECK(diags.items[0].code == MO_CODE_CORE_TEXT_LIMIT);
        }
        mo_diags_free(&diags);
        mo_arena_free(&arena);
        mo_buffer_free(&b);
    }
}

/* The differential harness uses this same public parse/check path, not a private checker hook.
 * Each argument is a Run file. Fuel and host arguments never influence its typing verdict. */
static MO_MUST_USE int verdict(const char *path) {
    struct mo_sources sources;
    struct mo_source_error error;
    struct mo_arena arena;
    struct mo_diags diags;
    uint32_t file;
    if (!mo_sources_init(&sources))
        return 8;
    if (!mo_diags_init(&diags)) {
        mo_sources_free(&sources);
        return 8;
    }
    mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
    int status = 8;
    if (mo_sources_add(&sources, path, &file, &error)) {
        const struct mo_source *s = &sources.files[file];
        struct mo_core_run *run =
            mo_core_text_parse(&arena, MO_CORE_TEXT_RUN, (const char *)s->bytes, s->length, &diags);
        status = run == NULL ? 4 : (mo_core_check(&run->program, &diags) ? 0 : 5);
    }
    if (printf("%d", status) < 0)
        status = 8;
    if (diags.count != 0 &&
        printf(" E%04u %s", (unsigned)diags.items[0].code,
               diags.items[0].reason == NULL ? diags.items[0].message : diags.items[0].reason) < 0)
        status = 8;
    if (putchar('\n') == EOF)
        status = 8;
    mo_arena_free(&arena);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
    return status == 8 ? 8 : 0;
}
int main(int argc, char **argv) {
    if (argc > 1) {
        for (int i = 1; i < argc; i++)
            if (verdict(argv[i]) != 0)
                return 8;
        return 0;
    }
    check_program("(Program () ((FunDecl \"main\" () (int) (lit (int 9223372036854775808)) "
                  "\"Main\" (function) 0)) () \"main\" (noSource))",
                  MO_CODE_CORE_CHECK_LITERAL);
    test_tail_and_inference();
    test_refusals();
    test_literal_boundaries();
    test_limits_and_metadata();
    test_source_span();
    test_nested_spans();
    test_large_programs();
    test_parser_node_limit();
    return mo_test_report("test_core_check");
}
