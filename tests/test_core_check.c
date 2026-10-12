#include "base/codes.h"
#include "base/limits.h"
#include "core/core.h"
#include "test.h"

#include <string.h>

static void check_program(const char *payload, uint16_t code) {
    struct mo_arena arena;
    struct mo_diags diags;
    struct mo_buffer input;
    mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
    mo_buffer_init(&input, MO_LIMIT_CORE_TEXT_SIZE);
    CHECK(mo_diags_init(&diags));
    CHECK(mo_buffer_append_text(&input, "(core 0.5 "));
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
    mo_diags_free(&d);
}

static void test_source_span(void) {
    const char *text =
        "(core 0.5 (Program () ((FunDecl \"main\" () (tuple ((int) (int))) (tuple ((lit (int "
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
        }
    }
    mo_diags_free(&diags);
    mo_arena_free(&arena);
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
    return mo_test_report("test_core_check");
}
