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
    CHECK(mo_buffer_append_text(&text, "(core 0.6 "));
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
    CHECK(count == 15);
}
static void test_shapes(void) {
    /* Asymmetric fields expose swapped field order; every Ty, Lit and Value constructor appears. */
    canonical(MO_CORE_TEXT_TYPE,
              "(tuple ((int) (bigInt) (byte) (decimal) (float) (bool) (char) (text) (bytes) (unit) "
              "(range) (time) (duration) (ordering) (fileError) (entry) (entryKind) (list (byte)) "
              "(map (text) (int)) "
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
        "(cap (files) (none) (none)) (cap (readFiles) (some \"\") (some 0)) "
        "(cap (readFiles) (some \"notes\") (some 999999999999999999999999999999))))");
    canonical(MO_CORE_TEXT_VALUE, "(cap (console) (none) (some 2))");
    canonical(MO_CORE_TEXT_CAP_STATE,
              "(CapState (\"fake arg\") ((\"K\" \"first\") (\"K\" \"second\")) (\"in\\r\\n\") "
              "(\"out\\n\") (\"err\") ((\"z\" \"last\") (\"a\" \"first\")) "
              "-999999999999999999999999999999 "
              "(RandomState 0x0000000000000000 0x8000000000000000 0xffffffffffffffff "
              "0x0123456789abcdef))");
    canonical(MO_CORE_TEXT_RANDOM_STATE, "(RandomState 0x0000000000000001 0x8000000000000000 "
                                         "0xffffffffffffffff 0x0123456789abcdef)");
    canonical(
        MO_CORE_TEXT_WORLD,
        "(World (\"b\" \"a\") ((\"K\" \"first\") (\"K\" \"second\")) (\"line\\r\\n\") "
        "(\"old out\\n\") (\"old err\") ((\"z\" \"last\") (\"a\" \"first\") (\"z\" \"duplicate\")) "
        "-123456789012345678901234567890 (RandomState 0x0000000000000001 0x0000000000000002 "
        "0x0000000000000003 0x0000000000000004) "
        "((CapState (\"fake\") () (\"input\\r\\n\") (\"fake out\") (\"fake err\") "
        "((\"b\" \"2\") (\"a\" \"1\") (\"b\" \"duplicate\")) -99 (RandomState 0x0000000000000005 "
        "0x0000000000000006 0x0000000000000007 0x0000000000000008)) "
        "(CapState () () () () () () 0 (RandomState 0x0000000000000000 0x0000000000000000 "
        "0x0000000000000000 0x0000000000000000))))");
    canonical(MO_CORE_TEXT_PROGRAM,
              "(Program ((DataDecl \"Point\" ((Ctor \"Point\" ((\"x\" (int)) (\"y\" (text)))))) "
              "(DataDecl \"Choice\" ((Ctor \"A\" ()) (Ctor \"B\" ((\"0\" (byte))))))) "
              "((FunDecl \"main\" ((\"v\" (int))) (text) (global \"C\") \"Main.Mod\" (function) 1) "
              "(FunDecl \"t\" () (unit) (fail (lit (text \"detail\"))) \"Spec\" (test) 0) "
              "(FunDecl \"e\" () (unit) (lit (unit)) \"Examples\" (example) 0) "
              "(FunDecl \"p\" ((\"result\" (text))) (int) (lit (int 1)) \"Spec\" (promise) 1) "
              "(FunDecl \"a\" () (int) (lit (int 2)) \"Spec\" (anchor) 0)) "
              "((ConstDecl \"C\" (text) (lit (text \"constant\")))) \"main\" (noSource))");
    canonical(MO_CORE_TEXT_EXPR,
              "(join \"loop\" ((\"i\" (int))) (if_ (lit (bool true)) (jump \"loop\" ((var \"i\"))) "
              "(stop \"done\")) "
              "(let_ \"f\" (lam ((\"x\" (text))) (var \"x\")) "
              "(case_ (con (data \"Choice\") \"B\" ((lit (byte 255)))) "
              "((\"A\" () (call \"zero\" ())) (\"B\" (\"b\") (app (var \"f\") "
              "((proj (tuple ((prim (not) ((lit (bool false)))) (global \"C\"))) 1))))))))");
    const char *bugs[] = {"(overflow \"add\")", "(divByZero)",   "(stop \"reason\")",
                          "(outside \"path\")", "(repeatedKey)", "(fileFolderConflict \"a/β\")"};
    for (size_t i = 0; i < sizeof bugs / sizeof bugs[0]; i++)
        canonical(MO_CORE_TEXT_BUG, bugs[i]);
    const char *outcomes[] = {
        "(ok (cap (random) (some \"\") (some 0)) (World () () () (\"effect\") () () -7 "
        "(RandomState 0x0000000000000001 0x0000000000000002 0x0000000000000003 0x0000000000000004) "
        "((CapState () () () () () () 17 (RandomState 0x0000000000000005 0x0000000000000006 "
        "0x0000000000000007 0x0000000000000008)))))",
        "(stopped (stop \"bug\") (World () () () () (\"error\") () 0 "
        "(RandomState 0x0000000000000001 0x0000000000000002 0x0000000000000003 0x0000000000000004) "
        "((CapState () () () (\"before stop\") () () 0 (RandomState 0x0000000000000005 "
        "0x0000000000000006 0x0000000000000007 0x0000000000000008)))))",
        "(failed \"detail\" (World () () () () () ((\"a\" \"b\")) 0 "
        "(RandomState 0x0000000000000001 0x0000000000000002 0x0000000000000003 0x0000000000000004) "
        "((CapState () () () () (\"before failure\") () 0 (RandomState 0x0000000000000005 "
        "0x0000000000000006 0x0000000000000007 0x0000000000000008)))))",
        ("(jumped \"loop\" ((int 1) (text \"next\")) (World () () () () () () 0 (RandomState "
         "0x0000000000000001 0x0000000000000002 0x0000000000000003 0x0000000000000004) ()))"),
        "(wrong \"invariant\")",
        "(outOfFuel)"};
    for (size_t i = 0; i < sizeof outcomes / sizeof outcomes[0]; i++)
        canonical(MO_CORE_TEXT_OUTCOME, outcomes[i]);
    const char *reports[] = {
        ("(returned \"[\\\"a\\\", \\\"b\\\\n\\\"]\" (effects (\"a\\nb\") "
         "(\"error\") ((\"b\" \"2\") (\"a\" \"1\")) "
         "((CapState () () () (\"fake out\") () () 0 (RandomState 0x0000000000000001 "
         "0x0000000000000002 0x0000000000000003 0x0000000000000004)))))"),
        "(stopped (divByZero) (effects (\"before\") () () "
        "((CapState () () () () (\"fake err\") () 0 (RandomState 0x0000000000000001 "
        "0x0000000000000002 0x0000000000000003 0x0000000000000004)))))",
        "(failed \"detail\" (effects () () () "
        "((CapState () () () () () ((\"fake\" \"file\")) 0 (RandomState 0x0000000000000001 "
        "0x0000000000000002 0x0000000000000003 0x0000000000000004)))))",
        "(outOfFuel)",
        "(wrong \"internal\")",
        "(parseError \"input\")",
        "(illTyped \"body\")",
        "(invalidRun \"selection\")",
        "(ioError \"unavailable\")"};
    for (size_t i = 0; i < sizeof reports / sizeof reports[0]; i++)
        canonical(MO_CORE_TEXT_REPORT, reports[i]);
    canonical(MO_CORE_TEXT_EFFECTS,
              "(effects (\"out\") (\"err\") ((\"z\" \"1\") (\"a\" \"2\")) ())");
    canonical(MO_CORE_TEXT_SELECTION, "(entry ((int 3) (cap (args) (some \"\") (none))))");
    canonical(MO_CORE_TEXT_SELECTION, "(test \"a named example\")");
    canonical(
        MO_CORE_TEXT_RUN,
        "(Run (Program () () () \"missing\" (noSource)) (World () () () () () () 0 (RandomState "
        "0x0000000000000001 0x0000000000000002 0x0000000000000003 0x0000000000000004) ()) "
        "(test \"named\") 123456789012345678901234567890)");
}

static void test_operations(void) {
    /* These lists are independent of the implementation tables: omissions or changed spelling
     * must fail, including constructors absent from the sample programs. */
    const struct {
        const char *prefix, *names;
    } families[] = {
        {"int",
         "add sub mul div rem neg abs min max pow floorDiv floorMod checkedAdd checkedSub "
         "checkedMul wrappingAdd wrappingSub wrappingMul saturatingAdd saturatingSub saturatingMul "
         "toText toDecimal toBig toFloat toChar toByte seconds milliseconds minutes hours days"},
        {"big", "add sub mul div rem neg abs min max pow floorDiv floorMod toInt toText"},
        {"byte", "toInt toText"},
        {"dec",
         "add sub mul div rem neg abs min max floor ceil round toInt toText toFixed toFloat"},
        {"float",
         "add sub mul div rem neg abs sqrt floor ceil round isNan isInfinite toDecimal toText"},
        {"char", "code isDigit isLetter isWhiteSpace lower upper toText"},
        {"text", "concat byteCount charCount isEmpty chars fromChars lines words split trim "
                 "trimStart trimEnd startsWith endsWith contains dropPrefix dropSuffix replace "
                 "repeat padLeft padRight lower upper toInt toDecimal toFloat join bytes"},
        {"bytes", "size get decodeUtf8"},
        {"unicode16", "graphemes displayWidth normalizeNfc lowercase uppercase"},
        {"range", "make start end toList contains toText"},
        {"time", "sub add subDur checkedAdd toText parse durAdd durSub durMul durNeg "
                 "inMilliseconds inSeconds durToText"},
        {"list (text)", "size isEmpty get first last append prepend concat reverse take drop set "
                        "removeAt uncons enumerate contains unique"},
        {"map (text) (int)", "empty size isEmpty get has set remove keys values entries ofList"},
        {"set (int)",
         "empty size isEmpty has add remove union intersection difference toList ofList"},
        {"io", "argsList envGet print printError readLine printed printedErrors writeText "
               "appendText remove readOnly now randomInt consoleFake consoleFakeInput "
               "filesInMemory clockFixed randomSeeded argsOf envOf"}};
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
                            "(io (entries true))",
                            "(io (entries false))",
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
        "; before\r\n(core\t0.6(text\"\\u{000000}\\u{00000A}\\u{00007F}\\u{01F600}é\")); after";
    const char *expected = "(core 0.6 (text \"\\u{0}\\n\\u{7f}😀é\"))\n";
    round_trip_bytes(MO_CORE_TEXT_LITERAL, input, strlen(input), expected, strlen(expected));
    input = "(core 0.6(bytes(-0000 00000255 -00123456789012345678901234567890)))";
    expected = "(core 0.6 (bytes (0 255 -123456789012345678901234567890)))\n";
    round_trip_bytes(MO_CORE_TEXT_LITERAL, input, strlen(input), expected, strlen(expected));
    input = "(core 0.6(float 0x7FF800000000ABCD))";
    expected = "(core 0.6 (float 0x7ff800000000abcd))\n";
    round_trip_bytes(MO_CORE_TEXT_LITERAL, input, strlen(input), expected, strlen(expected));
    input = "(core 0.6 (proj(var\"v\") -000))";
    expected = "(core 0.6 (proj (var \"v\") 0))\n";
    round_trip_bytes(MO_CORE_TEXT_EXPR, input, strlen(input), expected, strlen(expected));
    input = "(core 0.6(cap(files)(some\"notes\")(some -000)))";
    expected = "(core 0.6 (cap (files) (some \"notes\") (some 0)))\n";
    round_trip_bytes(MO_CORE_TEXT_VALUE, input, strlen(input), expected, strlen(expected));
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
        {MO_CORE_TEXT_LITERAL, "(core 0.6 (unit)", MO_CODE_CORE_SYNTAX},
        {MO_CORE_TEXT_LITERAL, "(core 0.6 (unit)))", MO_CODE_CORE_SYNTAX},
        {MO_CORE_TEXT_LITERAL, "(core 0.6 (unit)) (unit)", MO_CODE_CORE_SYNTAX},
        {MO_CORE_TEXT_LITERAL, "(core 0.6 (text \"a", MO_CODE_CORE_SYNTAX},
        {MO_CORE_TEXT_LITERAL, "(core 0.6 (text \"\\q\"))", MO_CODE_CORE_ESCAPE},
        {MO_CORE_TEXT_LITERAL, "(core 0.6 (text \"\\u{}\"))", MO_CODE_CORE_ESCAPE},
        {MO_CORE_TEXT_LITERAL, "(core 0.6 (text \"\\u{0000000}\"))", MO_CODE_CORE_ESCAPE},
        {MO_CORE_TEXT_LITERAL, "(core 0.6 (text \"\\u{xyz}\"))", MO_CODE_CORE_ESCAPE},
        {MO_CORE_TEXT_LITERAL, "(core 0.6 (text \"\\u{D800}\"))", MO_CODE_CORE_ESCAPE},
        {MO_CORE_TEXT_LITERAL, "(core 0.6 (text \"\\u{110000}\"))", MO_CODE_CORE_ESCAPE},
        {MO_CORE_TEXT_LITERAL, "(core 0.6 (text \"raw\ncontrol\"))", MO_CODE_CORE_ESCAPE},
        {MO_CORE_TEXT_LITERAL, "(core 0.6 (text \"raw\177control\"))", MO_CODE_CORE_ESCAPE},
        {MO_CORE_TEXT_LITERAL, "(core 0.1 (unit))", MO_CODE_CORE_VERSION},
        {MO_CORE_TEXT_LITERAL, "(core 0.2 (unit))", MO_CODE_CORE_VERSION},
        {MO_CORE_TEXT_LITERAL, "(core 0.3 (unit))", MO_CODE_CORE_VERSION},
        {MO_CORE_TEXT_LITERAL, "(core 0.4 (unit))", MO_CODE_CORE_VERSION},
        {MO_CORE_TEXT_LITERAL, "(core 0.5 (unit))", MO_CODE_CORE_VERSION},
        {MO_CORE_TEXT_LITERAL, "(core 0.7 (unit))", MO_CODE_CORE_VERSION},
        {MO_CORE_TEXT_LITERAL, "(core 9.9 (unit))", MO_CODE_CORE_VERSION},
        {MO_CORE_TEXT_LITERAL, "(core 0.6 (Unit))", MO_CODE_CORE_CONSTRUCTOR},
        {MO_CORE_TEXT_LITERAL, "(core 0.6 (unit extra))", MO_CODE_CORE_ARITY},
        {MO_CORE_TEXT_LITERAL, "(core 0.6 (text name))", MO_CODE_CORE_FIELD},
        {MO_CORE_TEXT_LITERAL, "(core 0.6 (bool True))", MO_CODE_CORE_FIELD},
        {MO_CORE_TEXT_LITERAL, "(core 0.6 (int +1))", MO_CODE_CORE_NUMBER},
        {MO_CORE_TEXT_LITERAL, "(core 0.6 (int -))", MO_CODE_CORE_NUMBER},
        {MO_CORE_TEXT_LITERAL, "(core 0.6 (int 1_000))", MO_CODE_CORE_NUMBER},
        {MO_CORE_TEXT_LITERAL, "(core 0.6 (int １２))", MO_CODE_CORE_NUMBER},
        {MO_CORE_TEXT_LITERAL, "(core 0.6 (float 0X8000000000000000))", MO_CODE_CORE_NUMBER},
        {MO_CORE_TEXT_LITERAL, "(core 0.6 (float 0x800000000000000))", MO_CODE_CORE_NUMBER},
        {MO_CORE_TEXT_LITERAL, "(core 0.6 (float 0x800000000000000g))", MO_CODE_CORE_NUMBER},
        {MO_CORE_TEXT_LITERAL, "(core 0.6 (char \"\"))", MO_CODE_CORE_CHARACTER},
        {MO_CORE_TEXT_LITERAL, "(core 0.6 (char \"ab\"))", MO_CODE_CORE_CHARACTER},
        {MO_CORE_TEXT_LITERAL, "(core 0.6 (bytes 0))", MO_CODE_CORE_FIELD},
        {MO_CORE_TEXT_EXPR, "(core 0.6 (proj (var \"x\") -1))", MO_CODE_CORE_NUMBER},
        {MO_CORE_TEXT_PRIMITIVE, "(core 0.6 (int add))", MO_CODE_CORE_FIELD},
        {MO_CORE_TEXT_PRIMITIVE, "(core 0.6 (big (checkedAdd)))", MO_CODE_CORE_CONSTRUCTOR},
        {MO_CORE_TEXT_PRIMITIVE, "(core 0.6 (io (consoleFake true)))", MO_CODE_CORE_ARITY},
        {MO_CORE_TEXT_PRIMITIVE, "(core 0.6 (io (consolefake)))", MO_CODE_CORE_CONSTRUCTOR},
        {MO_CORE_TEXT_PRIMITIVE, "(core 0.6 (unicode16 (normalizeNFC)))", MO_CODE_CORE_CONSTRUCTOR},
        {MO_CORE_TEXT_PRIMITIVE, "(core 0.6 (unicode16 (graphemes true)))", MO_CODE_CORE_ARITY},
        {MO_CORE_TEXT_PRIMITIVE, "(core 0.6 (io (entries)))", MO_CODE_CORE_ARITY},
        {MO_CORE_TEXT_VALUE, "(core 0.6 (map (((int 1)))))", MO_CODE_CORE_ARITY},
        {MO_CORE_TEXT_OUTCOME, "(core 0.6 (outOfFuel (World () () () () () () 0 0 ())))",
         MO_CODE_CORE_ARITY},
        {MO_CORE_TEXT_PROGRAM, "(core 0.6 (Program () () \"main\"))", MO_CODE_CORE_ARITY},
        {MO_CORE_TEXT_PROGRAM, "(core 0.6 (Program () () () \"main\"))", MO_CODE_CORE_ARITY},
        {MO_CORE_TEXT_VALUE, "(core 0.6 (cap (files) \"notes\" (none)))", MO_CODE_CORE_FIELD},
        {MO_CORE_TEXT_VALUE, "(core 0.6 (cap (files) (some \"notes\")))", MO_CODE_CORE_ARITY},
        {MO_CORE_TEXT_VALUE, "(core 0.6 (cap (files) (none) (some -1)))", MO_CODE_CORE_NUMBER},
        {MO_CORE_TEXT_VALUE, "(core 0.6 (cap (files) (none) (some \"0\")))", MO_CODE_CORE_NUMBER},
        {MO_CORE_TEXT_VALUE, "(core 0.6 (cap (files) (some 2) (none)))", MO_CODE_CORE_FIELD},
        {MO_CORE_TEXT_VALUE, "(core 0.6 (cap (files) (none) (none 0)))", MO_CODE_CORE_ARITY},
        {MO_CORE_TEXT_WORLD, "(core 0.6 (World () () () () () () 0 0))", MO_CODE_CORE_ARITY},
        {MO_CORE_TEXT_WORLD, "(core 0.6 (World () () () () () () 0 0 ()))", MO_CODE_CORE_FIELD},
        {MO_CORE_TEXT_CAP_STATE, "(core 0.6 (CapState () () () () () () 0))", MO_CODE_CORE_ARITY},
        {MO_CORE_TEXT_CAP_STATE, "(core 0.6 (CapState () () () () () () 0 0 ()))",
         MO_CODE_CORE_ARITY},
        {MO_CORE_TEXT_CAP_STATE, "(core 0.6 (CapState () () () () () () 0 -1))",
         MO_CODE_CORE_FIELD},
        {MO_CORE_TEXT_RANDOM_STATE, "(core 0.6 (RandomState 0x0000000000000001))",
         MO_CODE_CORE_ARITY},
        {MO_CORE_TEXT_RANDOM_STATE, "(core 0.6 (RandomState 0 1 2 3))", MO_CODE_CORE_NUMBER},
        {MO_CORE_TEXT_RANDOM_STATE,
         "(core 0.6 (RandomState 0x0000000000000001 0x0000000000000002 0x0000000000000003 "
         "0x10000000000000000))",
         MO_CODE_CORE_NUMBER},
        {MO_CORE_TEXT_BUG, "(core 0.6 (fileFolderConflict))", MO_CODE_CORE_ARITY},
        {MO_CORE_TEXT_BUG, "(core 0.6 (fileFolderConflict a))", MO_CODE_CORE_FIELD},
        {MO_CORE_TEXT_EFFECTS, "(core 0.6 (effects () () ()))", MO_CODE_CORE_ARITY}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        malformed(cases[i].kind, cases[i].text, strlen(cases[i].text), cases[i].code,
                  MO_LIMIT_CORE_SCRATCH_SIZE);
    const char *invalid_utf8[] = {"\xc0\x80",         "\xe0\x80\x80", "\xed\xa0\x80",
                                  "\xf4\x90\x80\x80", "\xf0\x9f",     ";\xff"};
    for (size_t i = 0; i < sizeof invalid_utf8 / sizeof invalid_utf8[0]; i++)
        malformed(MO_CORE_TEXT_LITERAL, invalid_utf8[i], strlen(invalid_utf8[i]), MO_CODE_CORE_UTF8,
                  MO_LIMIT_CORE_SCRATCH_SIZE);
    const char *unit = "(core 0.6 (unit))";
    malformed(MO_CORE_TEXT_LITERAL, unit, strlen(unit), MO_CODE_CORE_NO_MEMORY, 0);
    malformed(MO_CORE_TEXT_LITERAL, unit, MO_LIMIT_CORE_TEXT_SIZE + 1, MO_CODE_CORE_TEXT_LIMIT,
              MO_LIMIT_CORE_SCRATCH_SIZE);
}
static void test_metadata(void) {
    struct mo_sources sources;
    struct mo_source_error error;
    uint32_t file;
    CHECK(mo_sources_init(&sources));
    bool read = mo_sources_add(&sources, "tests/core/Metadata.core", &file, &error);
    CHECK(read);
    if (!read) {
        mo_sources_free(&sources);
        return;
    }
    const char *text = (const char *)sources.files[file].bytes;
    size_t length = sources.files[file].length;
    struct mo_arena arena;
    struct mo_diags diags;
    struct mo_buffer out;
    mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
    mo_buffer_init(&out, MO_LIMIT_CORE_TEXT_SIZE);
    CHECK(mo_diags_init(&diags));
    struct mo_core_run *run = mo_core_text_parse(&arena, MO_CORE_TEXT_RUN, text, length, &diags);
    CHECK(run != NULL && !mo_diags_any(&diags));
    if (run != NULL) {
        const struct mo_core_metadata *m = &run->program.metadata;
        CHECK(m->source && m->files.count == 2 && m->modules.count == 2);
        CHECK(strcmp(m->files.items[1].bytes, "fixtures/main.mo") == 0);
        CHECK(!m->modules.items[0].exposed_types.items[0].contents_exposed);
        CHECK(m->modules.items[1].exposed_types.items[0].contents_exposed);
        CHECK(m->funs.count == 7 && m->consts.count == 1);
        const struct mo_core_fun_meta *f = &m->funs.items[1];
        CHECK(strcmp(f->name.bytes, "Main.main") == 0);
        CHECK(f->span.value.file == 1 && f->span.value.end == 90);
        CHECK(f->body.spans.count == 17 && f->body.bindings.count == 5);
        CHECK(f->body.bindings.items[4].node == 8 && f->body.bindings.items[4].slot == 0);
        CHECK(strcmp(f->body.bindings.items[4].name.bytes, "q") == 0);
        CHECK(f->body.closures.items[0].node == 3);
        CHECK(strcmp(f->body.closures.items[0].name.bytes, "Main.main.fn.1") == 0);
        CHECK(strcmp(m->funs.items[0].body.params.items[0].value.bytes, "n") == 0);
        CHECK(strcmp(m->funs.items[2].text.value.bytes, "a sentence with β and \"quotes\"") == 0);
        CHECK(m->funs.items[4].subject.value == 0 && m->funs.items[4].lines.count == 2);
        CHECK(m->funs.items[4].lines.items[1].span.value.start == 71);
        CHECK(strcmp(m->funs.items[4].lines.items[1].text.bytes, "n >= 0") == 0);
        CHECK(m->funs.items[5].subject.value == 4 && m->funs.items[5].allows.value);
        CHECK(!m->funs.items[6].allows.value && !m->funs.items[6].body.spans.items[0].some);
        CHECK(strcmp(m->consts.items[0].name.bytes, "Hidden.seed") == 0);
    }
    const struct {
        const char *old, *replacement;
        uint16_t code;
    } changes[] = {{"SourceSpan 0 0 40", "SourceSpan 2 0 40", MO_CODE_CORE_FIELD},
                   {"SourceSpan 0 0 40", "SourceSpan 0 41 40", MO_CODE_CORE_FIELD},
                   {"SourceSpan 0 0 40", "SourceSpan 0 0 4294967296", MO_CODE_CORE_NUMBER},
                   {"(some (SourceSpan 1 13 15))", "", MO_CODE_CORE_FIELD},
                   {"((some \"n\"))", "()", MO_CODE_CORE_FIELD},
                   {"BindingMeta 0 0 \"x\"", "BindingMeta 0 1 \"x\"", MO_CODE_CORE_FIELD},
                   {"BindingMeta 2 0 \"f\"", "BindingMeta 0 0 \"f\"", MO_CODE_CORE_FIELD},
                   {"BindingMeta 2 0 \"f\"", "BindingMeta 1 0 \"f\"", MO_CODE_CORE_FIELD},
                   {"((3 \"Main.main.fn.1\"))", "()", MO_CODE_CORE_FIELD},
                   {"((3 \"Main.main.fn.1\"))", "((4 \"Main.main.fn.1\"))", MO_CODE_CORE_FIELD},
                   {"(0 4 5 6)", "(0 4 5)", MO_CODE_CORE_FIELD},
                   {"((0 false))", "((1 false))", MO_CODE_CORE_FIELD},
                   {"((0 false))", "((0 false) (0 true))", MO_CODE_CORE_FIELD},
                   {"((1 true)) (1)", "((1 true)) (2)", MO_CODE_CORE_FIELD},
                   {"(some 0) (none)", "(some 4) (none)", MO_CODE_CORE_FIELD},
                   {"(some 4) (some true)", "(some 0) (some true)", MO_CODE_CORE_FIELD},
                   {"(some 4) (some false)", "(some 4) (none)", MO_CODE_CORE_FIELD},
                   {"(some \"main() == 12\")", "(none)", MO_CODE_CORE_FIELD},
                   {"\"fixtures/main.mo\"", "\"fixtures/main\\u{0}.mo\"", MO_CODE_CORE_FIELD}};
    for (size_t i = 0; i < sizeof changes / sizeof changes[0]; i++) {
        const char *at = strstr(text, changes[i].old);
        CHECK(at != NULL);
        if (at == NULL)
            continue;
        size_t prefix = (size_t)(at - text), old_length = strlen(changes[i].old);
        mo_buffer_clear(&out);
        CHECK(mo_buffer_append(&out, text, prefix));
        CHECK(mo_buffer_append_text(&out, changes[i].replacement));
        CHECK(mo_buffer_append(&out, at + old_length, length - prefix - old_length));
        malformed(MO_CORE_TEXT_RUN, (const char *)out.bytes, out.length, changes[i].code,
                  MO_LIMIT_CORE_SCRATCH_SIZE);
    }
    mo_sources_free(&sources);
    /* Every decoded string/table belongs to the arena, not the input/source registry. */
    if (run != NULL) {
        mo_buffer_clear(&out);
        CHECK(mo_core_text_print(MO_CORE_TEXT_RUN, run, &out));
        CHECK(out.length == length);
        round_trip_bytes(MO_CORE_TEXT_RUN, (const char *)out.bytes, out.length,
                         (const char *)out.bytes, out.length);
        run->program.metadata.funs.items[1].body.spans.count--;
        mo_buffer_clear(&out);
        CHECK(!mo_core_text_print(MO_CORE_TEXT_RUN, run, &out));
        CHECK(out.length == 0);
    }
    mo_buffer_free(&out);
    mo_diags_free(&diags);
    mo_arena_free(&arena);
}
static void test_typed_fields(void) {
    struct mo_arena arena;
    struct mo_diags diags;
    mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
    CHECK(mo_diags_init(&diags));
    const char *text =
        "(core 0.6 (Run (Program ((DataDecl \"D\" ((Ctor \"C\" ((\"field\" (byte))))))) "
        "((FunDecl \"f\" ((\"x\" (int))) (text) (join \"j\" ((\"y\" (byte))) "
        "(call \"g\" ((var \"y\"))) (jump \"j\" ((lit (byte 7))))) \"Module\" (promise) 123)) "
        "((ConstDecl \"K\" (bool) (lit (bool true)))) \"f\" (noSource)) "
        "(World (\"arg\") ((\"key\" \"env\")) (\"in\") (\"out\") (\"err\") ((\"path\" \"file\")) "
        "-19 (RandomState 0x0000000000000017 0x000000000000001d 0x8000000000000000 "
        "0xffffffffffffffff) "
        "((CapState (\"fake arg\") ((\"fake key\" \"fake env\")) (\"fake in\") "
        "(\"fake out\") (\"fake err\") ((\"fake path\" \"fake file\")) -37 "
        "(RandomState 0x0000000000000029 0x000000000000002b 0x000000000000002f "
        "0x0000000000000035)) "
        "(CapState () () () () () () 43 (RandomState 0x000000000000003b 0x000000000000003d "
        "0x0000000000000043 0x0000000000000047)))) "
        "(entry ((float 0x8000000000000000) (cap (readFiles) (some \"notes\") (some 1)) "
        "(cap (console) (none) (none)))) 31))";
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
        CHECK(run->world.random.s0 == 23 && run->world.random.s1 == 29);
        CHECK(run->world.random.s2 == UINT64_C(0x8000000000000000));
        CHECK(run->world.random.s3 == UINT64_MAX);
        CHECK(run->world.fakes.count == 2);
        struct mo_core_cap_state *state = &run->world.fakes.items[0];
        CHECK(strcmp(state->args.items[0].bytes, "fake arg") == 0);
        CHECK(strcmp(state->env.items[0].key.bytes, "fake key") == 0);
        CHECK(strcmp(state->env.items[0].value.bytes, "fake env") == 0);
        CHECK(strcmp(state->stdin_.items[0].bytes, "fake in") == 0);
        CHECK(strcmp(state->stdout_.items[0].bytes, "fake out") == 0);
        CHECK(strcmp(state->stderr_.items[0].bytes, "fake err") == 0);
        CHECK(strcmp(state->files.items[0].key.bytes, "fake path") == 0);
        CHECK(strcmp(state->files.items[0].value.bytes, "fake file") == 0);
        CHECK(state->now.negative && strcmp(state->now.digits.bytes, "37") == 0);
        CHECK(state->random.s0 == 41 && state->random.s1 == 43);
        CHECK(state->random.s2 == 47 && state->random.s3 == 53);
        CHECK(strcmp(run->world.fakes.items[1].now.digits.bytes, "43") == 0);
        CHECK(run->world.fakes.items[1].random.s0 == 59 &&
              run->world.fakes.items[1].random.s1 == 61);
        CHECK(run->world.fakes.items[1].random.s2 == 67 &&
              run->world.fakes.items[1].random.s3 == 71);
        CHECK(run->selection.tag == MO_CORE_SELECTION_ENTRY && run->selection.as.args.count == 3);
        CHECK(run->selection.as.args.items[0]->tag == MO_CORE_VALUE_FLOAT);
        CHECK(run->selection.as.args.items[0]->as.float_ == UINT64_C(0x8000000000000000));
        mo_core_value fake = run->selection.as.args.items[1];
        CHECK(fake->tag == MO_CORE_VALUE_CAP && fake->as.cap.cap->tag == MO_CORE_CAP_READ_FILES);
        CHECK(fake->as.cap.root.some && strcmp(fake->as.cap.root.text.bytes, "notes") == 0);
        CHECK(fake->as.cap.fake.some && strcmp(fake->as.cap.fake.index.digits.bytes, "1") == 0);
        mo_core_value real = run->selection.as.args.items[2];
        CHECK(real->tag == MO_CORE_VALUE_CAP && !real->as.cap.root.some && !real->as.cap.fake.some);
        CHECK(strcmp(run->fuel.digits.bytes, "31") == 0);
    }
    text = "(core 0.6 (map (text) (int) (lit 27)))";
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
    CHECK(mo_buffer_append_text(&text, "(core 0.6 "));
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
    char input[] = "(core 0.6 (Program () () () \"a\\u{0}b\" (noSource)))";
    struct mo_core *program = mo_core_parse(&arena, input, strlen(input), &diags);
    CHECK(program != NULL);
    memset(input, 'x', sizeof input);
    if (program != NULL) {
        CHECK(program->entry.length == 3 && memcmp(program->entry.bytes, "a\0b", 3) == 0);
        struct mo_buffer out;
        mo_buffer_init(&out, 1000);
        CHECK(mo_core_print(program, &out));
        const char *expected = "(core 0.6 (Program () () () \"a\\u{0}b\" (noSource)))\n";
        CHECK(holds(&out, expected, strlen(expected)));
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
static void test_print_byte_limit(void) {
    /* Caller-built trees can be larger than any accepted input. Include wire overhead,
     * escapes and final LF, but not an earlier document, in the per-document byte count. */
    struct mo_buffer chars, out;
    mo_buffer_init(&chars, MO_LIMIT_CORE_TEXT_SIZE);
    mo_buffer_init(&out, MO_LIMIT_CORE_TEXT_SIZE * 3);
    for (size_t i = 0; i < MO_LIMIT_CORE_TEXT_SIZE; i++)
        CHECK(mo_buffer_append_byte(&chars, 'a'));
    struct mo_core_lit lit = {.tag = MO_CORE_LIT_TEXT};
    lit.as.text = (mo_core_string){(const char *)chars.bytes, chars.length};
    CHECK(mo_buffer_append_text(&out, "prefix"));
    CHECK(!mo_core_text_print(MO_CORE_TEXT_LITERAL, &lit, &out));
    CHECK(holds(&out, "prefix", 6));

    size_t overhead = strlen("(core " MO_CORE_TEXT_VERSION " (text \"\"))\n");
    lit.as.text.length = MO_LIMIT_CORE_TEXT_SIZE - overhead;
    CHECK(mo_core_text_print(MO_CORE_TEXT_LITERAL, &lit, &out));
    CHECK(out.length == 6 + MO_LIMIT_CORE_TEXT_SIZE);
    struct mo_arena arena;
    struct mo_diags diags;
    mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
    CHECK(mo_diags_init(&diags));
    CHECK(mo_core_text_parse(&arena, MO_CORE_TEXT_LITERAL, (const char *)out.bytes + 6,
                             MO_LIMIT_CORE_TEXT_SIZE, &diags) != NULL);
    CHECK(!mo_diags_any(&diags));
    size_t saved = out.length;
    lit.as.text.length++;
    CHECK(!mo_core_text_print(MO_CORE_TEXT_LITERAL, &lit, &out));
    CHECK(out.length == saved && memcmp(out.bytes, "prefix", 6) == 0);
    lit.as.text.length--;
    chars.bytes[0] = '\n'; /* escaped output is one byte longer */
    CHECK(!mo_core_text_print(MO_CORE_TEXT_LITERAL, &lit, &out));
    CHECK(out.length == saved);
    mo_diags_free(&diags);
    mo_arena_free(&arena);
    mo_buffer_free(&chars);
    mo_buffer_free(&out);
}
int main(void) {
    test_fixtures();
    test_shapes();
    test_operations();
    test_normalization();
    test_malformed();
    test_metadata();
    test_typed_fields();
    test_limits_and_ownership();
    test_print_byte_limit();
    return mo_test_report("test_core_text");
}
