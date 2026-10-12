#include "base/codes.h"
#include "base/limits.h"
#include "core/core.h"
#include "test.h"

#include <dirent.h>
#include <string.h>

static MO_MUST_USE bool holds(const struct mo_buffer *out, const char *text, size_t length) {
    return out->length == length && (length == 0 || memcmp(out->bytes, text, length) == 0);
}
static void round_trip_bytes(enum mo_core_text_kind kind, const char *input, size_t length,
                             const char *expected, size_t expected_length) {
    struct mo_arena arena;
    struct mo_diags diags;
    struct mo_buffer out;
    mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
    mo_buffer_init(&out, MO_LIMIT_CORE_TEXT_SIZE * 2);
    CHECK(mo_diags_init(&diags));
    void *value = mo_core_text_parse(&arena, kind, input, length, &diags);
    CHECK(value != NULL && !mo_diags_any(&diags));
    if (value != NULL) {
        CHECK(mo_core_text_print(kind, value, &out));
        CHECK(holds(&out, expected, expected_length));
        void *again = mo_core_text_parse(&arena, kind, (const char *)out.bytes, out.length, &diags);
        CHECK(again != NULL);
        if (again != NULL) {
            mo_buffer_clear(&out);
            CHECK(mo_core_text_print(kind, again, &out));
            CHECK(holds(&out, expected, expected_length));
        }
    }
    mo_buffer_free(&out);
    mo_diags_free(&diags);
    mo_arena_free(&arena);
}
static void canonical(enum mo_core_text_kind kind, const char *payload) {
    struct mo_buffer text;
    mo_buffer_init(&text, MO_LIMIT_CORE_TEXT_SIZE);
    CHECK(mo_buffer_append_text(&text, "(core 0.1 "));
    CHECK(mo_buffer_append_text(&text, payload));
    CHECK(mo_buffer_append_text(&text, ")\n"));
    round_trip_bytes(kind, (const char *)text.bytes, text.length, (const char *)text.bytes,
                     text.length);
    mo_buffer_free(&text);
}
static void test_fixtures(void) {
    DIR *dir = opendir("tests/core");
    CHECK(dir != NULL);
    if (dir == NULL)
        return;
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t n = strlen(entry->d_name);
        if (n < 5 || strcmp(entry->d_name + n - 5, ".core") != 0)
            continue;
        char path[MO_LIMIT_PATH_SIZE];
        int written = snprintf(path, sizeof path, "tests/core/%s", entry->d_name);
        CHECK(written >= 0 && (size_t)written < sizeof path);
        if (written < 0 || (size_t)written >= sizeof path)
            continue;
        struct mo_sources sources;
        struct mo_source_error error;
        uint32_t file;
        CHECK(mo_sources_init(&sources));
        bool read = mo_sources_add(&sources, path, &file, &error);
        CHECK(read);
        if (read) {
            const struct mo_source *src = &sources.files[file];
            round_trip_bytes(MO_CORE_TEXT_RUN, (const char *)src->bytes, src->length,
                             (const char *)src->bytes, src->length);
            count++;
        }
        mo_sources_free(&sources);
    }
    CHECK(closedir(dir) == 0);
    CHECK(count == 12);
}
static void test_shapes(void) {
    /* Asymmetric fields expose swapped field order; every Ty, Lit and Value constructor appears. */
    canonical(MO_CORE_TEXT_TYPE,
              "(tuple ((int) (bigInt) (byte) (decimal) (float) (bool) (char) (text) (bytes) (unit) "
              "(range) (time) (duration) (ordering) (fileError) (list (byte)) (map (text) (int)) "
              "(set (time)) (data \"Point[β]\") (option (char)) (result (unit) (fileError)) "
              "(fn ((int) (text)) (bool)) (cap (console))))");
    const char *caps[] = {"(console)", "(files)", "(readFiles)", "(clock)",
                          "(random)",  "(args)",  "(env)"};
    for (size_t i = 0; i < sizeof caps / sizeof caps[0]; i++)
        canonical(MO_CORE_TEXT_CAP, caps[i]);
    const char *lits[] = {"(int -9223372036854775809)",
                          "(bigInt 12345678901234567890123456789012345678901234567890)",
                          "(byte -1)",
                          "(decimal -170141183460469231731687303715884105728)",
                          "(float 0x8000000000000000)",
                          "(float 0x7ff8000000000042)",
                          "(bool true)",
                          "(bool false)",
                          "(char \"😀\")",
                          "(char \"\\u{0}\")",
                          "(text \"\\u{1}\\u{1f}\\u{7f}\\n\\r\\t\\\"\\\\β\")",
                          "(bytes (-1 0 255 256 999999999999999999999999999999))",
                          "(unit)",
                          "(time -999999999999999999999)",
                          "(duration 3)"};
    for (size_t i = 0; i < sizeof lits / sizeof lits[0]; i++) {
        canonical(MO_CORE_TEXT_LITERAL, lits[i]);
        canonical(MO_CORE_TEXT_VALUE, lits[i]);
    }
    canonical(
        MO_CORE_TEXT_VALUE,
        "(tuple ((range -3 7) (tuple ()) (list ((int 5) (int 1))) "
        "(map (((text \"b\") (int 8)) ((text \"a\") (int 2)))) (set ((int 2) (int 1))) "
        "(con \"Point\" ((int 4) (int 9))) "
        "(closure ((\"x\" (int 3)) (\"x\" (text \"outer\"))) ((\"y\" (int))) (var \"x\") ((\"C\" "
        "(bool true)))) "
        "(cap (files) (none)) (cap (readFiles) (some \"\")) (cap (readFiles) (some \"notes\"))))");
    canonical(
        MO_CORE_TEXT_WORLD,
        "(World (\"b\" \"a\") ((\"K\" \"first\") (\"K\" \"second\")) (\"line\\r\\n\") "
        "(\"old out\\n\") (\"old err\") ((\"z\" \"last\") (\"a\" \"first\") (\"z\" \"duplicate\")) "
        "-123456789012345678901234567890 999999999999999999999999999999)");
    canonical(MO_CORE_TEXT_PROGRAM,
              "(Program ((DataDecl \"Point\" ((Ctor \"Point\" ((\"x\" (int)) (\"y\" (text)))))) "
              "(DataDecl \"Choice\" ((Ctor \"A\" ()) (Ctor \"B\" ((\"0\" (byte))))))) "
              "((FunDecl \"main\" ((\"v\" (int))) (text) (global \"C\") \"Main.Mod\" (function) 1) "
              "(FunDecl \"t\" () (unit) (fail (lit (text \"detail\"))) \"Spec\" (test) 0) "
              "(FunDecl \"e\" () (unit) (lit (unit)) \"Examples\" (example) 0) "
              "(FunDecl \"p\" ((\"result\" (text))) (int) (lit (int 1)) \"Spec\" (promise) 1) "
              "(FunDecl \"a\" () (int) (lit (int 2)) \"Spec\" (anchor) 0)) "
              "((ConstDecl \"C\" (text) (lit (text \"constant\")))) \"main\")");
    canonical(MO_CORE_TEXT_EXPR,
              "(join \"loop\" ((\"i\" (int))) (if_ (lit (bool true)) (jump \"loop\" ((var \"i\"))) "
              "(stop \"done\")) "
              "(let_ \"f\" (lam ((\"x\" (text))) (var \"x\")) "
              "(case_ (con (data \"Choice\") \"B\" ((lit (byte 255)))) "
              "((\"A\" () (call \"zero\" ())) (\"B\" (\"b\") (app (var \"f\") "
              "((proj (tuple ((prim (not) ((lit (bool false)))) (global \"C\"))) 1))))))))");
    const char *bugs[] = {"(overflow \"add\")", "(divByZero)", "(stop \"reason\")",
                          "(outside \"path\")", "(repeatedKey)"};
    for (size_t i = 0; i < sizeof bugs / sizeof bugs[0]; i++)
        canonical(MO_CORE_TEXT_BUG, bugs[i]);
    const char *outcomes[] = {
        "(ok (int 5) (World () () () (\"effect\") () () -7 13))",
        "(stopped (stop \"bug\") (World () () () () (\"error\") () 0 0))",
        "(failed \"detail\" (World () () () () () ((\"a\" \"b\")) 0 0))",
        "(jumped \"loop\" ((int 1) (text \"next\")) (World () () () () () () 0 0))",
        "(wrong \"invariant\")",
        "(outOfFuel)"};
    for (size_t i = 0; i < sizeof outcomes / sizeof outcomes[0]; i++)
        canonical(MO_CORE_TEXT_OUTCOME, outcomes[i]);
    const char *reports[] = {("(returned \"[\\\"a\\\", \\\"b\\\\n\\\"]\" (effects (\"a\\nb\") "
                              "(\"error\") ((\"b\" \"2\") (\"a\" \"1\"))))"),
                             "(stopped (divByZero) (effects (\"before\") () ()))",
                             "(failed \"detail\" (effects () () ()))",
                             "(outOfFuel)",
                             "(wrong \"internal\")",
                             "(parseError \"input\")",
                             "(illTyped \"body\")",
                             "(invalidRun \"selection\")",
                             "(ioError \"unavailable\")"};
    for (size_t i = 0; i < sizeof reports / sizeof reports[0]; i++)
        canonical(MO_CORE_TEXT_REPORT, reports[i]);
    canonical(MO_CORE_TEXT_EFFECTS, "(effects (\"out\") (\"err\") ((\"z\" \"1\") (\"a\" \"2\")))");
    canonical(MO_CORE_TEXT_SELECTION, "(entry ((int 3) (cap (args) (some \"\"))))");
    canonical(MO_CORE_TEXT_SELECTION, "(test \"a named example\")");
    canonical(MO_CORE_TEXT_RUN, "(Run (Program () () () \"missing\") (World () () () () () () 0 0) "
                                "(test \"named\") 123456789012345678901234567890)");
}

static void test_operations(void) {
    /* These lists are independent of the implementation tables: omissions or changed spelling
     * must fail, including constructors absent from the twelve sample programs. */
    const struct {
        const char *prefix, *names;
    } families[] = {
        {"int",
         "add sub mul div rem neg abs min max pow floorDiv floorMod checkedAdd checkedSub "
         "checkedMul wrappingAdd wrappingSub wrappingMul saturatingAdd saturatingSub saturatingMul "
         "toText toDecimal toBig toFloat toChar toByte seconds milliseconds minutes hours days"},
        {"big", "add sub mul div rem neg abs toInt toText"},
        {"byte", "toInt toText"},
        {"dec",
         "add sub mul div rem neg abs min max floor ceil round toInt toText toFixed toFloat"},
        {"float", "add sub mul div neg abs sqrt floor ceil round isNan isInfinite toText"},
        {"char", "code isDigit isLetter isWhiteSpace lower upper toText"},
        {"text", "concat byteCount charCount isEmpty chars fromChars lines words split trim "
                 "trimStart trimEnd startsWith endsWith contains dropPrefix dropSuffix replace "
                 "repeat padLeft padRight lower upper toInt toDecimal join bytes"},
        {"bytes", "size get decodeUtf8"},
        {"range", "make start end toList contains toText"},
        {"time", "sub add subDur checkedAdd toText parse durAdd durSub durMul durNeg "
                 "inMilliseconds inSeconds durToText"},
        {"list (text)", "size isEmpty get first last append prepend concat reverse take drop set "
                        "removeAt uncons enumerate contains unique"},
        {"map (text) (int)", "empty size isEmpty get has set remove keys values entries ofList"},
        {"set (int)",
         "empty size isEmpty has add remove union intersection difference toList ofList"},
        {"io", "argsList envGet print printError readLine printed printedErrors writeText "
               "appendText remove readOnly now randomInt"}};
    struct mo_buffer payload;
    mo_buffer_init(&payload, MO_LIMIT_CORE_TEXT_SIZE);
    for (size_t i = 0; i < sizeof families / sizeof families[0]; i++) {
        const char *name = families[i].names;
        while (*name != '\0') {
            size_t length = strcspn(name, " ");
            mo_buffer_clear(&payload);
            CHECK(mo_buffer_append_text(&payload, "("));
            CHECK(mo_buffer_append_text(&payload, families[i].prefix));
            CHECK(mo_buffer_append_text(&payload, " ("));
            CHECK(mo_buffer_append(&payload, name, length));
            CHECK(mo_buffer_append_text(&payload, "))"));
            CHECK(mo_buffer_append_byte(&payload, 0));
            canonical(MO_CORE_TEXT_PRIMITIVE, (const char *)payload.bytes);
            name += length;
            if (*name == ' ')
                name++;
        }
    }
    mo_buffer_free(&payload);
    const char *extras[] = {"(not)",
                            "(eq (map (text) (list (byte))))",
                            "(list (byte) (lit 0))",
                            "(list (byte) (lit 9999999999999999999999999999))",
                            "(list (byte) (zip (text)))",
                            "(map (text) (int) (lit 2))",
                            "(io (readText true))",
                            "(io (readText false))",
                            "(io (readBytes true))",
                            "(io (readBytes false))",
                            "(io (list true))",
                            "(io (list false))",
                            "(io (exists true))",
                            "(io (exists false))",
                            "(io (within true))",
                            "(io (within false))"};
    for (size_t i = 0; i < sizeof extras / sizeof extras[0]; i++)
        canonical(MO_CORE_TEXT_PRIMITIVE, extras[i]);
    const char *bases[] = {"int",  "bigInt", "byte",  "decimal", "float",   "bool",
                           "char", "text",   "bytes", "time",    "duration"};
    const char *cmps[] = {"lt", "le", "gt", "ge"};
    for (size_t i = 0; i < sizeof bases / sizeof bases[0]; i++) {
        for (size_t j = 0; j < sizeof cmps / sizeof cmps[0]; j++) {
            char text[100];
            int n = snprintf(text, sizeof text, "(cmp (%s) (%s))", bases[i], cmps[j]);
            CHECK(n > 0 && (size_t)n < sizeof text);
            canonical(MO_CORE_TEXT_PRIMITIVE, text);
        }
    }
}
static void test_normalization(void) {
    const char *input =
        "; before\r\n(core\t0.1(text\"\\u{000000}\\u{00000A}\\u{00007F}\\u{01F600}é\")); after";
    const char *expected = "(core 0.1 (text \"\\u{0}\\n\\u{7f}😀é\"))\n";
    round_trip_bytes(MO_CORE_TEXT_LITERAL, input, strlen(input), expected, strlen(expected));
    input = "(core 0.1(bytes(-0000 00000255 -00123456789012345678901234567890)))";
    expected = "(core 0.1 (bytes (0 255 -123456789012345678901234567890)))\n";
    round_trip_bytes(MO_CORE_TEXT_LITERAL, input, strlen(input), expected, strlen(expected));
    input = "(core 0.1(float 0x7FF800000000ABCD))";
    expected = "(core 0.1 (float 0x7ff800000000abcd))\n";
    round_trip_bytes(MO_CORE_TEXT_LITERAL, input, strlen(input), expected, strlen(expected));
    input = "(core 0.1 (proj(var\"v\") -000))";
    expected = "(core 0.1 (proj (var \"v\") 0))\n";
    round_trip_bytes(MO_CORE_TEXT_EXPR, input, strlen(input), expected, strlen(expected));
}
static void malformed(enum mo_core_text_kind kind, const char *input, size_t length, uint16_t code,
                      size_t arena_limit) {
    struct mo_arena arena;
    struct mo_diags diags;
    mo_arena_init(&arena, arena_limit);
    CHECK(mo_diags_init(&diags));
    CHECK(mo_core_text_parse(&arena, kind, input, length, &diags) == NULL);
    CHECK(diags.count == 1 && diags.items[0].code == code);
    if (diags.count != 0)
        CHECK(diags.items[0].primary.span.end <= length);
    mo_diags_free(&diags);
    mo_arena_free(&arena);
}
static void test_malformed(void) {
    const struct {
        enum mo_core_text_kind kind;
        const char *text;
        uint16_t code;
    } cases[] = {
        {MO_CORE_TEXT_LITERAL, "", MO_CODE_CORE_SYNTAX},
        {MO_CORE_TEXT_LITERAL, "(core 0.1 (unit)", MO_CODE_CORE_SYNTAX},
        {MO_CORE_TEXT_LITERAL, "(core 0.1 (unit)))", MO_CODE_CORE_SYNTAX},
        {MO_CORE_TEXT_LITERAL, "(core 0.1 (unit)) (unit)", MO_CODE_CORE_SYNTAX},
        {MO_CORE_TEXT_LITERAL, "(core 0.1 (text \"a", MO_CODE_CORE_SYNTAX},
        {MO_CORE_TEXT_LITERAL, "(core 0.1 (text \"\\q\"))", MO_CODE_CORE_ESCAPE},
        {MO_CORE_TEXT_LITERAL, "(core 0.1 (text \"\\u{}\"))", MO_CODE_CORE_ESCAPE},
        {MO_CORE_TEXT_LITERAL, "(core 0.1 (text \"\\u{0000000}\"))", MO_CODE_CORE_ESCAPE},
        {MO_CORE_TEXT_LITERAL, "(core 0.1 (text \"\\u{xyz}\"))", MO_CODE_CORE_ESCAPE},
        {MO_CORE_TEXT_LITERAL, "(core 0.1 (text \"\\u{D800}\"))", MO_CODE_CORE_ESCAPE},
        {MO_CORE_TEXT_LITERAL, "(core 0.1 (text \"\\u{110000}\"))", MO_CODE_CORE_ESCAPE},
        {MO_CORE_TEXT_LITERAL, "(core 0.1 (text \"raw\ncontrol\"))", MO_CODE_CORE_ESCAPE},
        {MO_CORE_TEXT_LITERAL, "(core 0.1 (text \"raw\177control\"))", MO_CODE_CORE_ESCAPE},
        {MO_CORE_TEXT_LITERAL, "(core 0.2 (unit))", MO_CODE_CORE_VERSION},
        {MO_CORE_TEXT_LITERAL, "(core 9.9 (unit))", MO_CODE_CORE_VERSION},
        {MO_CORE_TEXT_LITERAL, "(core 0.1 (Unit))", MO_CODE_CORE_CONSTRUCTOR},
        {MO_CORE_TEXT_LITERAL, "(core 0.1 (unit extra))", MO_CODE_CORE_ARITY},
        {MO_CORE_TEXT_LITERAL, "(core 0.1 (text name))", MO_CODE_CORE_FIELD},
        {MO_CORE_TEXT_LITERAL, "(core 0.1 (bool True))", MO_CODE_CORE_FIELD},
        {MO_CORE_TEXT_LITERAL, "(core 0.1 (int +1))", MO_CODE_CORE_NUMBER},
        {MO_CORE_TEXT_LITERAL, "(core 0.1 (int -))", MO_CODE_CORE_NUMBER},
        {MO_CORE_TEXT_LITERAL, "(core 0.1 (int 1_000))", MO_CODE_CORE_NUMBER},
        {MO_CORE_TEXT_LITERAL, "(core 0.1 (int １２))", MO_CODE_CORE_NUMBER},
        {MO_CORE_TEXT_LITERAL, "(core 0.1 (float 0X8000000000000000))", MO_CODE_CORE_NUMBER},
        {MO_CORE_TEXT_LITERAL, "(core 0.1 (float 0x800000000000000))", MO_CODE_CORE_NUMBER},
        {MO_CORE_TEXT_LITERAL, "(core 0.1 (float 0x800000000000000g))", MO_CODE_CORE_NUMBER},
        {MO_CORE_TEXT_LITERAL, "(core 0.1 (char \"\"))", MO_CODE_CORE_CHARACTER},
        {MO_CORE_TEXT_LITERAL, "(core 0.1 (char \"ab\"))", MO_CODE_CORE_CHARACTER},
        {MO_CORE_TEXT_LITERAL, "(core 0.1 (bytes 0))", MO_CODE_CORE_FIELD},
        {MO_CORE_TEXT_EXPR, "(core 0.1 (proj (var \"x\") -1))", MO_CODE_CORE_NUMBER},
        {MO_CORE_TEXT_PRIMITIVE, "(core 0.1 (int add))", MO_CODE_CORE_FIELD},
        {MO_CORE_TEXT_PRIMITIVE, "(core 0.1 (big (checkedAdd)))", MO_CODE_CORE_CONSTRUCTOR},
        {MO_CORE_TEXT_VALUE, "(core 0.1 (map (((int 1)))))", MO_CODE_CORE_ARITY},
        {MO_CORE_TEXT_OUTCOME, "(core 0.1 (outOfFuel (World () () () () () () 0 0)))",
         MO_CODE_CORE_ARITY},
        {MO_CORE_TEXT_PROGRAM, "(core 0.1 (Program () () \"main\"))", MO_CODE_CORE_ARITY},
        {MO_CORE_TEXT_VALUE, "(core 0.1 (cap (files) \"notes\"))", MO_CODE_CORE_FIELD}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        malformed(cases[i].kind, cases[i].text, strlen(cases[i].text), cases[i].code,
                  MO_LIMIT_CORE_SCRATCH_SIZE);
    const char *invalid_utf8[] = {"\xc0\x80",         "\xe0\x80\x80", "\xed\xa0\x80",
                                  "\xf4\x90\x80\x80", "\xf0\x9f",     ";\xff"};
    for (size_t i = 0; i < sizeof invalid_utf8 / sizeof invalid_utf8[0]; i++)
        malformed(MO_CORE_TEXT_LITERAL, invalid_utf8[i], strlen(invalid_utf8[i]), MO_CODE_CORE_UTF8,
                  MO_LIMIT_CORE_SCRATCH_SIZE);
    const char *unit = "(core 0.1 (unit))";
    malformed(MO_CORE_TEXT_LITERAL, unit, strlen(unit), MO_CODE_CORE_NO_MEMORY, 0);
    malformed(MO_CORE_TEXT_LITERAL, unit, MO_LIMIT_CORE_TEXT_SIZE + 1, MO_CODE_CORE_TEXT_LIMIT,
              MO_LIMIT_CORE_SCRATCH_SIZE);
}
static void test_typed_fields(void) {
    struct mo_arena arena;
    struct mo_diags diags;
    mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
    CHECK(mo_diags_init(&diags));
    const char *text =
        "(core 0.1 (Run (Program ((DataDecl \"D\" ((Ctor \"C\" ((\"field\" (byte))))))) "
        "((FunDecl \"f\" ((\"x\" (int))) (text) (join \"j\" ((\"y\" (byte))) "
        "(call \"g\" ((var \"y\"))) (jump \"j\" ((lit (byte 7))))) \"Module\" (promise) 123)) "
        "((ConstDecl \"K\" (bool) (lit (bool true)))) \"f\") "
        "(World (\"arg\") ((\"key\" \"env\")) (\"in\") (\"out\") (\"err\") ((\"path\" \"file\")) "
        "-19 23) "
        "(entry ((float 0x8000000000000000))) 31))";
    struct mo_core_run *run =
        mo_core_text_parse(&arena, MO_CORE_TEXT_RUN, text, strlen(text), &diags);
    CHECK(run != NULL && !mo_diags_any(&diags));
    if (run != NULL) {
        struct mo_core *p = &run->program;
        CHECK(p->data.count == 1 && p->funs.count == 1 && p->consts.count == 1);
        CHECK(strcmp(p->entry.bytes, "f") == 0);
        struct mo_core_ctor *ctor = &p->data.items[0].ctors.items[0];
        CHECK(strcmp(ctor->name.bytes, "C") == 0 && ctor->fields.count == 1);
        CHECK(strcmp(ctor->fields.items[0].name.bytes, "field") == 0);
        CHECK(ctor->fields.items[0].ty->tag == MO_CORE_TY_BYTE);
        struct mo_core_fun *f = &p->funs.items[0];
        CHECK(strcmp(f->name.bytes, "f") == 0 && strcmp(f->module.bytes, "Module") == 0);
        CHECK(f->params.count == 1 && f->params.items[0].ty->tag == MO_CORE_TY_INT);
        CHECK(f->ret->tag == MO_CORE_TY_TEXT && f->kind == MO_CORE_PROMISE);
        CHECK(!f->const_count.negative && strcmp(f->const_count.digits.bytes, "123") == 0);
        CHECK(f->body->tag == MO_CORE_EXPR_JOIN && strcmp(f->body->as.join.name.bytes, "j") == 0);
        CHECK(f->body->as.join.body->tag == MO_CORE_EXPR_CALL);
        CHECK(strcmp(f->body->as.join.body->as.call.name.bytes, "g") == 0);
        CHECK(f->body->as.join.rest->tag == MO_CORE_EXPR_JUMP);
        CHECK(strcmp(f->body->as.join.rest->as.jump.args.items[0]->as.lit->as.byte.digits.bytes,
                     "7") == 0);
        CHECK(p->consts.items[0].ty->tag == MO_CORE_TY_BOOL &&
              p->consts.items[0].body->as.lit->as.bool_);
        CHECK(strcmp(run->world.env.items[0].value.bytes, "env") == 0);
        CHECK(strcmp(run->world.stdin_.items[0].bytes, "in") == 0);
        CHECK(strcmp(run->world.stdout_.items[0].bytes, "out") == 0);
        CHECK(strcmp(run->world.stderr_.items[0].bytes, "err") == 0);
        CHECK(strcmp(run->world.files.items[0].value.bytes, "file") == 0);
        CHECK(run->world.now.negative && strcmp(run->world.now.digits.bytes, "19") == 0);
        CHECK(!run->world.seed.negative && strcmp(run->world.seed.digits.bytes, "23") == 0);
        CHECK(run->selection.tag == MO_CORE_SELECTION_ENTRY && run->selection.as.args.count == 1);
        CHECK(run->selection.as.args.items[0]->tag == MO_CORE_VALUE_FLOAT);
        CHECK(run->selection.as.args.items[0]->as.float_ == UINT64_C(0x8000000000000000));
        CHECK(strcmp(run->fuel.digits.bytes, "31") == 0);
    }
    text = "(core 0.1 (map (text) (int) (lit 27)))";
    struct mo_core_prim *prim =
        mo_core_text_parse(&arena, MO_CORE_TEXT_PRIMITIVE, text, strlen(text), &diags);
    CHECK(prim != NULL);
    if (prim != NULL) {
        CHECK(prim->tag == MO_CORE_PRIM_MAP && prim->as.map.k->tag == MO_CORE_TY_TEXT);
        CHECK(prim->as.map.v->tag == MO_CORE_TY_INT && prim->as.map.op.tag == MO_CORE_MAP_OP_LIT);
        CHECK(strcmp(prim->as.map.op.lit.digits.bytes, "27") == 0);
    }
    mo_diags_free(&diags);
    mo_arena_free(&arena);
}
static void test_limits_and_ownership(void) {
    struct mo_buffer text;
    mo_buffer_init(&text, MO_LIMIT_CORE_TEXT_SIZE);
    CHECK(mo_buffer_append_text(&text, "(core 0.1 "));
    for (size_t i = 0; i < MO_LIMIT_CORE_DEPTH - 2; i++)
        CHECK(mo_buffer_append_text(&text, "(list "));
    CHECK(mo_buffer_append_text(&text, "(int)"));
    for (size_t i = 0; i < MO_LIMIT_CORE_DEPTH - 1; i++)
        CHECK(mo_buffer_append_byte(&text, ')'));
    CHECK(mo_buffer_append_byte(&text, '\n'));
    round_trip_bytes(MO_CORE_TEXT_TYPE, (const char *)text.bytes, text.length,
                     (const char *)text.bytes, text.length);
    mo_buffer_clear(&text);
    for (size_t i = 0; i <= MO_LIMIT_CORE_DEPTH; i++)
        CHECK(mo_buffer_append_byte(&text, '('));
    malformed(MO_CORE_TEXT_TYPE, (const char *)text.bytes, text.length, MO_CODE_NESTED_TOO_DEEPLY,
              MO_LIMIT_CORE_SCRATCH_SIZE);
    mo_buffer_clear(&text);
    CHECK(mo_buffer_append_byte(&text, '('));
    for (size_t i = 0; i < MO_LIMIT_CORE_NODES; i++)
        CHECK(mo_buffer_append_text(&text, "x "));
    malformed(MO_CORE_TEXT_TYPE, (const char *)text.bytes, text.length, MO_CODE_CORE_TEXT_LIMIT,
              MO_LIMIT_CORE_SCRATCH_SIZE);
    mo_buffer_free(&text);

    struct mo_arena arena;
    struct mo_diags diags;
    mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
    CHECK(mo_diags_init(&diags));
    char input[] = "(core 0.1 (Program () () () \"a\\u{0}b\"))";
    struct mo_core *program = mo_core_parse(&arena, input, strlen(input), &diags);
    CHECK(program != NULL);
    memset(input, 'x', sizeof input);
    if (program != NULL) {
        CHECK(program->entry.length == 3 && memcmp(program->entry.bytes, "a\0b", 3) == 0);
        struct mo_buffer out;
        mo_buffer_init(&out, 1000);
        CHECK(mo_core_print(program, &out));
        CHECK(holds(&out, "(core 0.1 (Program () () () \"a\\u{0}b\"))\n", 40));
        mo_buffer_free(&out);
        mo_buffer_init(&out, 20);
        CHECK(mo_buffer_append_text(&out, "prefix"));
        CHECK(!mo_core_print(program, &out));
        CHECK(holds(&out, "prefix", 6));
        mo_buffer_free(&out);
    }
    struct mo_core_ty cyclic = {.tag = MO_CORE_TY_LIST};
    cyclic.as.list = &cyclic;
    struct mo_buffer out;
    mo_buffer_init(&out, 10000);
    CHECK(!mo_core_text_print(MO_CORE_TEXT_TYPE, &cyclic, &out));
    CHECK(out.length == 0);
    mo_buffer_free(&out);
    mo_diags_free(&diags);
    mo_arena_free(&arena);
}
int main(void) {
    test_fixtures();
    test_shapes();
    test_operations();
    test_normalization();
    test_malformed();
    test_typed_fields();
    test_limits_and_ownership();
    return mo_test_report("test_core_text");
}
