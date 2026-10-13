#include "base/buffer.h"
#include "base/codes.h"
#include "base/limits.h"
#include "check/check.h"
#include "test.h"

#include <locale.h>
#include <stdlib.h>
#include <string.h>

struct fixture {
    struct mo_arena syntax, arena;
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_module_interfaces *interfaces;
    struct mo_signatures *signatures;
};

static void init_limit(struct fixture *f, const char *first, const char *second, size_t limit) {
    mo_arena_init(&f->syntax, limit);
    mo_arena_init(&f->arena, limit);
    MO_ASSERT(mo_sources_init(&f->sources) && mo_diags_init(&f->diags));
    struct mo_module_syntax *modules[2];
    const char *texts[] = {first, second};
    uint32_t count = second == NULL ? 1u : 2u;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t file;
        struct mo_source_error problem;
        MO_ASSERT(mo_sources_add_bytes(&f->sources, i == 0 ? "main.mo" : "other.mo",
                                       (const uint8_t *)texts[i], strlen(texts[i]), &file,
                                       &problem));
        modules[i] = mo_parse(&f->syntax, &f->sources, file, &f->diags);
        if (mo_diags_any(&f->diags))
            MO_ASSERT(fprintf(stderr, "test source doesn't parse:\n%s\n%s\n", texts[i],
                              f->diags.items[0].message) >= 0);
        MO_ASSERT(modules[i] != NULL && !mo_diags_any(&f->diags));
    }
    uint32_t target[] = {1, 1}, *uses[] = {target, NULL};
    struct mo_module_set set = {.count = count, .modules = modules, .uses = uses};
    MO_ASSERT(modules[0]->header.uses->count <= 2);
    f->interfaces = mo_resolve_modules(&f->arena, &set, &f->diags);
    MO_ASSERT(f->interfaces != NULL);
    f->signatures = mo_prepare_signatures(&f->arena, &set, f->interfaces, &f->diags);
}

static void init(struct fixture *f, const char *first, const char *second) {
    init_limit(f, first, second, 16 << 20);
}

static void finish(struct fixture *f) {
    mo_arena_free(&f->arena);
    mo_arena_free(&f->syntax);
    mo_diags_free(&f->diags);
    mo_sources_free(&f->sources);
}

static MO_MUST_USE struct mo_inferred_body *body(struct fixture *f, const char *name) {
    MO_ASSERT(f->signatures != NULL);
    for (uint32_t i = 0; i < f->signatures->count; i++)
        if (strcmp(f->signatures->items[i].function->name, name) == 0)
            return mo_infer_function(&f->arena, f->interfaces, f->signatures, i, &f->diags);
    MO_ASSERT(false);
    return NULL;
}

static void question_name(void) {
    struct fixture f;
    init(&f, "module Main\nfn answer?() : Int\n1\nend\n", NULL);
    CHECK(f.signatures == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_BOOL_NAME);
    finish(&f);
    init(&f, "module Main\nfn run() : Int\nanswer? = 1\nanswer?\nend\n", NULL);
    CHECK(body(&f, "run") == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_BOOL_NAME);
    finish(&f);
}

static void tuple_position(void) {
    struct fixture f;
    init(&f, "module Main\nfn run(pair: (Int, Text)) : Text\npair.1\nend\n", NULL);
    struct mo_inferred_body *out = body(&f, "run");
    CHECK(out != NULL && f.diags.count == 0);
    if (out != NULL)
        CHECK(strcmp(out->items[out->count - 1].type->name, "Text") == 0);
    finish(&f);
}

static void inferred_uses(void) {
    const struct {
        const char *source, *result;
    } cases[] = {
        {"fn run() : Decimal\nvalue = 1\nvalue\nend\n", "Decimal"},
        {"fn run() : Decimal\nf = fn(x) x + 1 end\nf(0.5)\nend\n", "Decimal"},
        {"fn run() : List(Text)\nvar xs = []\nxs = [\"late\"]\nxs\nend\n", "List"},
        {"fn run() : Map(Text, Int)\nvar xs = {}\nxs = {\"late\": 2}\nxs\nend\n", "Map"},
        {"fn run(_xs: List(Int)) : Int\n0\nend\n", "Int"},
        {"fn identity(value: T) : T\nvalue\nend\nfn run() : Text\nidentity(\"text\")\nend\n",
         "Text"},
        {"fn identity(value: T) : T\nvalue\nend\nfn run() : Int\n"
         "assert identity(\"text\") == \"text\"\nidentity(2)\nend\n",
         "Int"},
        {"fn invoke(f: fn(Int) : Int) : Int\nf(4)\nend\n"
         "fn run() : Int\ninvoke(fn(x) x + 2 end)\nend\n",
         "Int"},
        {"fn run(f: fn() : fn() : Int) : Int\nf()()\nend\n", "Int"},
        {"fn run(pair: ((Int, Text), Bool)) : Text\npair.0.1\nend\n", "Text"},
        {"fn run(flag: Bool) : Int\nif flag\nx = 1\nx\nelse\nx = 2\nx\nend\nend\n", "Int"},
        {"fn run(flag: Bool) : Int\nreturn 1 if flag\n2\nend\n", "Int"},
        {"fn run(flag: Bool) : Int\nif flag\nreturn 1\nelse\nstop \"impossible\"\nend\nend\n",
         "Int"},
        {"fn run(flag: Bool) : Int\nif flag\nreturn 1\nelse\nreturn 2\nend\n"
         "assert true\nend\n",
         "Unit"},
        {"fn run() : Int\nvar total = 0\nfor i in 0..10\ncontinue if i % 2 == 0\n"
         "total += i\nend\ntotal\nend\n",
         "Int"},
        {"fn run() : Int\nvar total = 0\nfor (key, value) in {\"one\": 1}\n"
         "if key == \"one\"\ntotal += value\nend\nend\ntotal\nend\n",
         "Int"},
        {"fn run(flag: Bool)\nvar total = 0\nwhile flag\ntotal += 1\nbreak if total > "
         "3\nend\nend\n",
         "Unit"},
        {"fn run() : Int\nquestion? = fn() true end\nif question?(): 1 else: 0\nend\n", "Int"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char source[1024];
        int n = snprintf(source, sizeof(source), "module Main\n%s", cases[i].source);
        MO_ASSERT(n > 0 && (size_t)n < sizeof(source));
        struct fixture f;
        init(&f, source, NULL);
        struct mo_inferred_body *out = body(&f, "run");
        if (out == NULL)
            MO_ASSERT(fprintf(stderr, "inferred_uses[%zu]: E%04u %s\n", i, f.diags.items[0].code,
                              f.diags.items[0].message) >= 0);
        CHECK(out != NULL && f.diags.count == 0);
        if (out != NULL) {
            CHECK(strcmp(out->items[out->count - 1].type->name, cases[i].result) == 0);
            if (i == 1) {
                bool found = false;
                for (uint32_t j = 0; j < out->count; j++)
                    if (out->items[j].syntax->kind == MO_S_LAMBDA) {
                        const struct mo_source_type *type = out->items[j].type;
                        CHECK(type->count == 2);
                        CHECK(strcmp(type->arguments[0]->name, "Decimal") == 0);
                        CHECK(strcmp(type->arguments[1]->name, "Decimal") == 0);
                        found = true;
                    }
                CHECK(found);
            }
        }
        finish(&f);
    }
}

static void scope_errors(void) {
    const struct {
        const char *source;
        uint16_t code;
    } cases[] = {
        {"fn run() : Int\nx\nend\n", MO_CODE_NAME_UNKNOWN},
        {"fn run() : Int\nx = y\ny = 1\nx\nend\n", MO_CODE_NAME_UNKNOWN},
        {"fn run(flag: Bool) : Int\nif flag\nx = 1\nassert x == 1\nend\nx\nend\n",
         MO_CODE_NAME_UNKNOWN},
        {"fn run(x: Int) : Int\nx = x + 1\nx\nend\n", MO_CODE_BIND_REPEATED},
        {"fn run(x: Int) : Int\nf = fn(x) x end\nf(x)\nend\n", MO_CODE_BIND_REPEATED},
        {"fn run(x: Int) : Int\n1\nend\n", MO_CODE_UNUSED_NAME},
        {"fn run() : Int\n1 + true\nend\n", MO_CODE_TYPE_MISMATCH},
        {"fn run(value: T) : T\n1\nend\n", MO_CODE_RESULT_TYPE},
        {"fn run()\n_xs = []\nend\n", MO_CODE_CANNOT_INFER},
        {"fn run() : Int\nf = fn(x) x(x) end\nf(1)\nend\n", MO_CODE_ARGUMENT_TYPE},
        {"fn callee(x: Int) : Int\nx\nend\nfn run() : Int\ncallee()\nend\n",
         MO_CODE_CALL_ARGUMENTS},
        {"fn run()\n1\nUnit\nend\n", MO_CODE_UNUSED_VALUE},
        {"fn run()\nx += 1\nend\n", MO_CODE_NOT_VARIABLE},
        {"fn run()\nbreak\nend\n", MO_CODE_EXIT_OUTSIDE_LOOP},
        {"fn run() : Int\n9223372036854775808\nend\n", MO_CODE_LITERAL_RANGE},
        {"fn run(pair: (Int, Text)) : Int\npair.2\nend\n", MO_CODE_TUPLE_POSITION},
        {"fn run(pair: (Int, Text)) : Int\npair.99999999999999999999\nend\n",
         MO_CODE_TUPLE_POSITION},
        {"fn run(flag: Bool) : Bool\nif flag\ntrue\nend\nend\n", MO_CODE_RESULT_TYPE},
        {"fn run()\nwhile true\nf = fn()\nbreak\nend\nf()\nend\nend\n", MO_CODE_EXIT_OUTSIDE_LOOP},
        {"fn run()\nvar x = 0\nf = fn()\nx = 1\nend\nf()\nassert x == 1\nend\n",
         MO_CODE_NOT_VARIABLE},
        {"fn own()\nend\nfn run(flag: Bool)\nif flag\nown = 1\nassert own == 1\nend\nown()\nend\n",
         MO_CODE_BIND_REPEATED},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char source[1024];
        int n = snprintf(source, sizeof(source), "module Main\n%s", cases[i].source);
        MO_ASSERT(n > 0 && (size_t)n < sizeof(source));
        struct fixture f;
        init(&f, source, NULL);
        CHECK(body(&f, "run") == NULL);
        if (f.diags.count != 1 || f.diags.items[0].code != cases[i].code)
            MO_ASSERT(fprintf(stderr, "scope_errors[%zu]: expected E%04u, got %u diagnostics%s\n",
                              i, cases[i].code, f.diags.count,
                              f.diags.count == 0 ? "" : f.diags.items[0].message) >= 0);
        CHECK(f.diags.count == 1 && f.diags.items[0].code == cases[i].code);
        finish(&f);
    }
}

static void number(const char *type, const char *value, bool valid) {
    struct mo_buffer source;
    mo_buffer_init(&source, 8192);
    MO_ASSERT(mo_buffer_append_text(&source, "module Main\nfn run() : "));
    MO_ASSERT(mo_buffer_append_text(&source, type));
    MO_ASSERT(mo_buffer_append_text(&source, "\n"));
    MO_ASSERT(mo_buffer_append_text(&source, value));
    MO_ASSERT(mo_buffer_append_text(&source, "\nend\n"));
    MO_ASSERT(mo_buffer_append_byte(&source, 0));
    struct fixture f;
    init(&f, (const char *)source.bytes, NULL);
    struct mo_inferred_body *out = body(&f, "run");
    CHECK((out != NULL) == valid);
    if (valid && out != NULL) {
        CHECK(f.diags.count == 0);
        CHECK(strcmp(out->items[out->count - 1].type->name, type) == 0);
    } else if (!valid)
        CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_LITERAL_RANGE);
    finish(&f);
    mo_buffer_free(&source);
}

static void literal_ranges(void) {
    number("Int", "9223372036854775807", true);
    number("Int", "-9223372036854775808", true);
    number("Int", "-9223372036854775809", false);
    number("Byte", "255", true);
    number("Byte", "256", false);
    number("Decimal", "170141183460469231731.687303715884105727", true);
    number("Decimal", "170141183460469231731.687303715884105728", false);
    number("Decimal", "-170141183460469231731.687303715884105727", true);
    number("Decimal", "-170141183460469231731.687303715884105728", false);
    number("Decimal", "0.000000000000000001", true);
    number("Decimal", "0.0000000000000000001", false);
    char binary[1027];
    memcpy(binary, "0b", 2);
    memset(binary + 2, '1', 53);
    memset(binary + 55, '0', 971);
    binary[1026] = '\0';
    number("Float", binary, true);
    binary[55] = '1'; /* 2^1024 - 2^970 rounds to infinity, not the largest Float. */
    number("Float", binary, false);
    char hex[260];
    memcpy(hex, "0x", 2);
    memset(hex + 2, 'f', 257);
    hex[259] = '\0';
    number("Float", hex, false);
    number("BigInt", hex, true);
    /* Decimal spelling of 2^1024 - 2^970, computed independently of the checker. */
    char boundary[] =
        "179769313486231580793728971405303415079934132710037826936173778980444968292764750946649"
        "017977587207096330286416692887910946555547851940402630657488671505820681908902000708383"
        "676273854845817711531764475730270069855571366959622842914819860834936475292719074168444"
        "365510704342711559699508093042880177904174497792";
    number("Float", boundary, false);
    boundary[sizeof(boundary) - 2]--;
    number("Float", boundary, true);
    char fraction[512];
    int n = snprintf(fraction, sizeof(fraction), "%s.999999999999999999", boundary);
    MO_ASSERT(n > 0 && (size_t)n < sizeof(fraction));
    number("Float", fraction, true);
}

static void float_locale(void) {
    /* CI needs no installed foreign locale. A caller can supply a comma locale
     * to exercise the host's global state without changing the test inputs. */
    const char *locale = getenv("MO_TEST_DECIMAL_LOCALE");
    if (locale != NULL)
        MO_ASSERT(setlocale(LC_NUMERIC, locale) != NULL);
    number("Float", "0.5", true);
    number("Float", "1_000.25", true);
    if (locale != NULL)
        MO_ASSERT(setlocale(LC_NUMERIC, "C") != NULL);
}

static void assignment_equivalence(void) {
    const char *const signatures[] = {"a: Int, b: Int", "a: Duration, b: Duration",
                                      "a: Time, b: Duration", "a: T, b: T"};
    const char *const types[] = {"Int", "Duration", "Time", "T"};
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        for (unsigned op = 0; op < 2; op++) {
            uint16_t codes[2] = {0};
            for (unsigned form = 0; form < 2; form++) {
                char source[512];
                int n = snprintf(source, sizeof(source),
                                 "module Main\nfn run(%s) : %s\nvar x = a\nx %s %sb\nx\nend\n",
                                 signatures[i], types[i], form == 0 ? (op == 0 ? "+=" : "-=") : "=",
                                 form == 0 ? "" : (op == 0 ? "x + " : "x - "));
                MO_ASSERT(n > 0 && (size_t)n < sizeof(source));
                struct fixture f;
                init(&f, source, NULL);
                struct mo_inferred_body *out = body(&f, "run");
                if (f.diags.count != 0)
                    codes[form] = f.diags.items[0].code;
                CHECK(i == 0 ? out != NULL : out == NULL);
                finish(&f);
            }
            CHECK(codes[0] == codes[1]);
            CHECK(codes[0] == (i == 0 ? 0 : MO_CODE_CHECK_UNFINISHED));
        }
    }
}

static void discarded_tails(void) {
    const struct {
        const char *body;
        uint16_t code;
    } cases[] = {
        {"fn run()\n1\nend\n", MO_CODE_UNUSED_VALUE},
        {"fn run(flag: Bool)\nif flag\n1\nend\nend\n", MO_CODE_UNUSED_VALUE},
        {"fn run(f: fn(Int) : Int)\nfor i in 0..3\nf(i)\nend\nend\n", MO_CODE_UNUSED_VALUE},
        {"fn run(f: fn() : Result(Int, Text))\nf()\nend\n", MO_CODE_FAILURE_IGNORED},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char source[512];
        int n = snprintf(source, sizeof(source), "module Main\n%s", cases[i].body);
        MO_ASSERT(n > 0 && (size_t)n < sizeof(source));
        struct fixture f;
        init(&f, source, NULL);
        CHECK(body(&f, "run") == NULL);
        CHECK(f.diags.count == 1 && f.diags.items[0].code == cases[i].code);
        finish(&f);
    }
}

static void module_calls(void) {
    const char other[] = "module Helpers\nexpose identity\n"
                         "fn identity(value: T) : T\nvalue\nend\n"
                         "fn hidden(value: Int) : Int\nvalue\nend\n";
    struct fixture f;
    init(&f, "module Main\nuse Helpers{identity}\nfn run() : Int\nidentity(3)\nend\n", other);
    CHECK(body(&f, "run") != NULL && f.diags.count == 0);
    finish(&f);
    init(&f, "module Main\nuse Helpers\nfn run() : Text\nHelpers.identity(\"text\")\nend\n", other);
    CHECK(body(&f, "run") != NULL && f.diags.count == 0);
    finish(&f);
    init(&f, "module Main\nuse Helpers\nfn run() : Int\nHelpers.hidden(3)\nend\n", other);
    CHECK(body(&f, "run") == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_IMPORT_PRIVATE);
    CHECK(f.diags.items[0].secondary_count == 1);
    finish(&f);
}

static void imported_meaning(void) {
    const char helper[] = "module Helpers\nexpose add\nfn add(x: Int, y: Int) : Int\nx + y\nend\n";
    const char *const orders[] = {"if true\nadd = 3\nassert add == 3\nend\nadd(1, 2)",
                                  "_first = add(1, 2)\nif true\nadd = 3\nassert add == 3\nend\n0"};
    for (size_t i = 0; i < sizeof(orders) / sizeof(orders[0]); i++) {
        char source[512];
        int n = snprintf(source, sizeof(source),
                         "module Main\nuse Helpers{add}\nfn run() : Int\n%s\nend\n", orders[i]);
        MO_ASSERT(n > 0 && (size_t)n < sizeof(source));
        struct fixture f;
        init(&f, source, helper);
        CHECK(body(&f, "run") == NULL);
        CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_BIND_REPEATED);
        finish(&f);
    }
}

static void contextual_arguments(void) {
    const char *const functions[] = {"fn apply(f: fn(T) : U, value: T) : U\nf(value)\nend\n",
                                     "fn apply(value: T, f: fn(T) : U) : U\nf(value)\nend\n"};
    const char *const calls[] = {"apply(fn(p) p.0 end, (1, \"a\"))",
                                 "apply((1, \"a\"), fn(p) p.0 end)"};
    for (size_t i = 0; i < 2; i++) {
        char source[512];
        int n = snprintf(source, sizeof(source), "module Main\n%sfn run() : Int\n%s\nend\n",
                         functions[i], calls[i]);
        MO_ASSERT(n > 0 && (size_t)n < sizeof(source));
        struct fixture f;
        init(&f, source, NULL);
        struct mo_inferred_body *out = body(&f, "run");
        CHECK(out != NULL && f.diags.count == 0);
        if (out != NULL)
            CHECK(strcmp(out->items[out->count - 1].type->name, "Int") == 0);
        finish(&f);
    }
}

static void source_order(void) {
    const struct {
        const char *body, *first;
        uint16_t code;
    } cases[] = {
        {"a = 1\nb = 2\n3", "a =", MO_CODE_UNUSED_NAME},
        {"1\nmissing", "1\n", MO_CODE_UNUSED_VALUE},
        {"_x = 9223372036854775808\nmissing", "922337", MO_CODE_LITERAL_RANGE},
        {"ready? = 1\n_keep = ready?\nmissing", "ready? =", MO_CODE_BOOL_NAME},
        {"if true\na = 1\nend\nmissing", "a =", MO_CODE_UNUSED_NAME},
        {"_xs = []\nmissing", "missing", MO_CODE_NAME_UNKNOWN},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char source[512];
        int n = snprintf(source, sizeof(source), "module Main\nfn run() : Int\n%s\nend\n",
                         cases[i].body);
        MO_ASSERT(n > 0 && (size_t)n < sizeof(source));
        struct fixture f;
        init(&f, source, NULL);
        CHECK(body(&f, "run") == NULL);
        CHECK(f.diags.count == 1 && f.diags.items[0].code == cases[i].code);
        CHECK(f.diags.items[0].primary.span.start ==
              (uint32_t)(strstr(source, cases[i].first) - source));
        finish(&f);
    }
}

static void diagnostic_contexts(void) {
    const struct {
        const char *source, *label;
        uint16_t code;
    } cases[] = {
        {"fn run(count: Int)\nif count\nUnit\nend\nend\n", "expected Bool, found Int",
         MO_CODE_CONDITION_TYPE},
        {"fn run() : Text\n\"a\" + \"b\"\nend\n", "found Text", MO_CODE_NUMBER_ABILITY},
        {"fn run(a: Byte, b: Byte) : Byte\na + b\nend\n", "found Byte", MO_CODE_BYTE_ARITHMETIC},
        {"fn run(x: Text) : Int\nx()\nend\n", "found Text", MO_CODE_NOT_FUNCTION},
        {"fn owner(_x: Text) : Int\n0\nend\nfn run(owner: Text) : Int\nowner(owner)\nend\n", NULL,
         MO_CODE_BIND_REPEATED},
        {"fn run()\nfor _ in 5\nend\nend\n", NULL, MO_CODE_NOT_ITERABLE},
        {"fn run()\nfor _c in \"abc\"\nend\nend\n", NULL, MO_CODE_NOT_ITERABLE},
        {"fn run()\nUnknown.f(1)\nend\n", NULL, MO_CODE_IMPORT_MISSING},
        {"fn f(_x: Int)\nend\nfn run()\nf(\"text\")\nend\n", "expected Int, found Text",
         MO_CODE_ARGUMENT_TYPE},
        {"fn run() : Int\n\"text\"\nend\n", "expected Int, found Text", MO_CODE_RESULT_TYPE},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char source[512];
        int n = snprintf(source, sizeof(source), "module Main\n%s", cases[i].source);
        MO_ASSERT(n > 0 && (size_t)n < sizeof(source));
        struct fixture f;
        init(&f, source, NULL);
        CHECK(body(&f, "run") == NULL);
        CHECK(f.diags.count == 1 && f.diags.items[0].code == cases[i].code);
        CHECK(f.diags.items[0].primary.span.start < f.diags.items[0].primary.span.end);
        if (cases[i].label != NULL)
            CHECK(strstr(f.diags.items[0].primary.text, cases[i].label) != NULL);
        finish(&f);
    }
}

static void specifications(void) {
    const char *const specifications[] = {
        "examples\nrun(1) == \"two\"\nend\n",
        "promise \"twice the input\"\nresult == x + x\nallows x: 1, result: 2\n"
        "forbids x: 1, result: 3\nend\n"};
    for (size_t i = 0; i < sizeof(specifications) / sizeof(specifications[0]); i++) {
        char source[512];
        int n = snprintf(source, sizeof(source),
                         "module Main\nfn run(x: Int) : Int\n%s\nx + x\nend\n", specifications[i]);
        MO_ASSERT(n > 0 && (size_t)n < sizeof(source));
        struct fixture f;
        init(&f, source, NULL);
        CHECK(body(&f, "run") == NULL);
        CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_CHECK_UNFINISHED);
        if (f.diags.count == 1)
            CHECK(f.diags.items[0].primary.span.start ==
                  (uint32_t)(strstr(source, i == 0 ? "examples" : "promise") - source));
        finish(&f);
    }
}

static void allocation_limits(void) {
    struct fixture f;
    init(&f, "module Main\nfn run() : Decimal\nf = fn(x) x + 1 end\nf(0.5)\nend\n", NULL);
    bool succeeded = false, failed = false;
    for (size_t limit = 0; limit < 32768; limit += 16) {
        struct mo_arena arena;
        mo_arena_init(&arena, limit);
        struct mo_inferred_body *out =
            mo_infer_function(&arena, f.interfaces, f.signatures, 0, &f.diags);
        if (out != NULL) {
            succeeded = true;
            CHECK(f.diags.count == 0);
            CHECK(strcmp(out->items[out->count - 1].type->name, "Decimal") == 0);
        } else {
            failed = true;
            CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_CHECK_NO_MEMORY);
        }
        mo_arena_free(&arena);
        mo_diags_free(&f.diags);
        MO_ASSERT(mo_diags_init(&f.diags));
    }
    CHECK(succeeded && failed);
    finish(&f);
}

static void unused_fix(void) {
    const char source[] = "module Main\nfn run() : Int\nvalue = 1\n2\nend\n";
    struct fixture f;
    init(&f, source, NULL);
    CHECK(body(&f, "run") == NULL);
    MO_ASSERT(f.diags.count == 1 && f.diags.items[0].fix_count == 1);
    const struct mo_edit *edit = f.diags.items[0].fixes[0].edits;
    struct mo_buffer fixed;
    mo_buffer_init(&fixed, 1024);
    MO_ASSERT(mo_buffer_append(&fixed, (const uint8_t *)source, edit->span.start));
    MO_ASSERT(mo_buffer_append_text(&fixed, edit->replacement));
    MO_ASSERT(mo_buffer_append_text(&fixed, source + edit->span.end));
    MO_ASSERT(mo_buffer_append_byte(&fixed, 0));
    finish(&f);
    init(&f, (const char *)fixed.bytes, NULL);
    CHECK(body(&f, "run") != NULL && f.diags.count == 0);
    finish(&f);
    mo_buffer_free(&fixed);
    init(&f, "module Main\nfn run(value: Int, _value: Int) : Int\n2\nend\n", NULL);
    CHECK(body(&f, "run") == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_UNUSED_NAME);
    CHECK(f.diags.items[0].fix_count == 0);
    finish(&f);
}

static void reference_bodies(void) {
    struct mo_sources sources;
    MO_ASSERT(mo_sources_init(&sources));
    struct mo_source_error problem;
    uint32_t file;
    MO_ASSERT(mo_sources_add(&sources, "docs/reference/README.md", &file, &problem));
    const struct mo_source *reference = &sources.files[file];
    struct mo_buffer text;
    mo_buffer_init(&text, MO_LIMIT_FILE_SIZE);
    MO_ASSERT(mo_buffer_append(&text, reference->bytes, reference->length));
    MO_ASSERT(mo_buffer_append_byte(&text, 0));
    const char *const names[] = {"shipping", "discount", "sum_to"};
    const char *const types[] = {"Decimal", "Decimal", "Int"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char marker[64];
        int n = snprintf(marker, sizeof(marker), "```mo\nfn %s(", names[i]);
        MO_ASSERT(n > 0 && (size_t)n < sizeof(marker));
        const char *start = strstr((const char *)text.bytes, marker);
        MO_ASSERT(start != NULL);
        start += 6;
        const char *end = strstr(start, "\n```");
        MO_ASSERT(end != NULL);
        struct mo_buffer source;
        mo_buffer_init(&source, 4096);
        MO_ASSERT(mo_buffer_append_text(&source, "module Main\n"));
        MO_ASSERT(mo_buffer_append(&source, (const uint8_t *)start, (size_t)(end - start)));
        MO_ASSERT(mo_buffer_append_text(&source, "\n"));
        MO_ASSERT(mo_buffer_append_byte(&source, 0));
        struct fixture f;
        init(&f, (const char *)source.bytes, NULL);
        struct mo_inferred_body *out = body(&f, names[i]);
        CHECK(out != NULL && f.diags.count == 0);
        if (out != NULL)
            CHECK(strcmp(out->items[out->count - 1].type->name, types[i]) == 0);
        finish(&f);
        mo_buffer_free(&source);
    }
    mo_buffer_free(&text);
    mo_sources_free(&sources);
}

static void shared_types(void) {
    /* A function value adds one node above the repeated tuple type. */
    for (uint32_t count = MO_LIMIT_SOURCE_TYPE_DEPTH - 2; count <= MO_LIMIT_SOURCE_TYPE_DEPTH - 1;
         count++) {
        struct mo_buffer source;
        mo_buffer_init(&source, 16384);
        MO_ASSERT(mo_buffer_append_text(
            &source, "module Main\n"
                     "fn duplicate(value: T) : (T, T)\n(value, value)\nend\nfn run()\n_v0 = 1\n"));
        for (uint32_t i = 1; i <= count; i++) {
            MO_ASSERT(mo_buffer_append_text(&source, "_v"));
            MO_ASSERT(mo_buffer_append_uint(&source, i));
            MO_ASSERT(mo_buffer_append_text(&source, " = duplicate(_v"));
            MO_ASSERT(mo_buffer_append_uint(&source, i - 1));
            MO_ASSERT(mo_buffer_append_text(&source, ")\n"));
        }
        MO_ASSERT(mo_buffer_append_text(&source, "end\n"));
        MO_ASSERT(mo_buffer_append_byte(&source, 0));
        struct fixture f;
        init(&f, (const char *)source.bytes, NULL);
        struct mo_inferred_body *out = body(&f, "run");
        CHECK((out != NULL) == (count == MO_LIMIT_SOURCE_TYPE_DEPTH - 2));
        if (count == MO_LIMIT_SOURCE_TYPE_DEPTH - 1)
            CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_CHECK_TYPE_DEPTH);
        finish(&f);
        mo_buffer_free(&source);
    }
}

static void assignment_notes(void) {
    struct fixture f;
    init(&f,
         "module Main\nfn run() : Int\nvar count = 0\ncount = 5\ncount += 1\n"
         "total = count\ntotal\nend\n",
         NULL);
    struct mo_inferred_body *out = body(&f, "run");
    CHECK(out != NULL && f.diags.count == 0);
    if (out != NULL) {
        const struct mo_syntax *count = NULL;
        uint32_t assignments = 0, bound = 0;
        for (uint32_t i = 0; i < out->count; i++) {
            const struct mo_inferred_expression *item = &out->items[i];
            if (item->syntax->kind == MO_S_VAR)
                count = item->syntax->children[0];
            if (item->syntax->kind == MO_S_ADD_ASSIGN || item->syntax->kind == MO_S_BIND) {
                const struct mo_syntax *target = item->syntax->children[0];
                bool to_count = target->span.end - target->span.start == 5 &&
                                memcmp(target->text, "count", 5) == 0;
                CHECK(item->binding == (to_count ? count : NULL));
                if (to_count)
                    assignments++;
            }
            if (item->syntax->kind == MO_S_NAME && item->binding == item->syntax) {
                CHECK(strcmp(item->type->name, "Int") == 0);
                bound++;
            }
        }
        CHECK(assignments == 2 && bound == 2);
    }
    finish(&f);
    init(&f, "module Main\nfn run()\nvar count = 0\ncount = 5\nend\n", NULL);
    CHECK(body(&f, "run") == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_UNUSED_NAME);
    CHECK(f.diags.items[0].fix_count == 0);
    finish(&f);
}

static void repeated_signature(void) {
    struct mo_buffer source;
    mo_buffer_init(&source, 1 << 20);
    MO_ASSERT(mo_buffer_append_text(&source, "module Main\nfn big(_x: (Int"));
    for (uint32_t i = 1; i < 60000; i++)
        MO_ASSERT(mo_buffer_append_text(&source, i % 32 == 0 ? ",\nInt" : ", Int"));
    MO_ASSERT(mo_buffer_append_text(&source, "))\nend\nfn run()\n"));
    for (uint32_t i = 0; i < 130; i++) {
        MO_ASSERT(mo_buffer_append_text(&source, "_a"));
        MO_ASSERT(mo_buffer_append_uint(&source, i));
        MO_ASSERT(mo_buffer_append_text(&source, " = big\n"));
    }
    MO_ASSERT(mo_buffer_append_text(&source, "end\n"));
    MO_ASSERT(mo_buffer_append_byte(&source, 0));
    struct fixture f;
    init(&f, (const char *)source.bytes, NULL);
    size_t prepared = f.arena.used;
    struct mo_inferred_body *out = body(&f, "run");
    CHECK(out != NULL && f.diags.count == 0);
    CHECK(f.arena.used - prepared < (1 << 20));
    if (out != NULL) {
        bool found = false;
        for (uint32_t i = 0; i < out->count; i++)
            if (out->items[i].symbol != NULL) {
                CHECK(out->items[i].type->arguments[0]->count == 60000);
                found = true;
            }
        CHECK(found);
    }
    finish(&f);
    mo_buffer_free(&source);
}

static void syntax_depth(void) {
    const uint32_t depths[] = {110, MO_LIMIT_NESTING_DEPTH - 1};
    for (size_t d = 0; d < sizeof(depths) / sizeof(depths[0]); d++) {
        struct mo_buffer source;
        mo_buffer_init(&source, 32768);
        MO_ASSERT(mo_buffer_append_text(
            &source, "module Main\nfn run(a: Bool, b: Bool, c: Bool, d: Range, e: Int, "
                     "f: Int, g: Int, h: fn(Bool) : Int) : Bool\n"));
        for (uint32_t i = 0; i < depths[d]; i++)
            MO_ASSERT(
                mo_buffer_append_text(&source, "a implies b or c and not d == e .. f + g * -h("));
        MO_ASSERT(mo_buffer_append_text(&source, "a"));
        for (uint32_t i = 0; i < depths[d]; i++)
            MO_ASSERT(mo_buffer_append_text(&source, ")"));
        MO_ASSERT(mo_buffer_append_text(&source, "\nend\n"));
        MO_ASSERT(mo_buffer_append_byte(&source, 0));
        struct fixture f;
        init(&f, (const char *)source.bytes, NULL);
        struct mo_inferred_body *out = body(&f, "run");
        CHECK(out != NULL && f.diags.count == 0);
        if (out != NULL)
            CHECK(strcmp(out->items[out->count - 1].type->name, "Bool") == 0);
        finish(&f);
        mo_buffer_free(&source);
    }
}

static void work_limits(void) {
    struct fixture f;
    init(&f, "module Main\nfn run() : Int\n1\nend\n", NULL);
    CHECK(body(&f, "run") != NULL);
    uint64_t cost = *f.signatures->work;
    MO_ASSERT(cost > 0 && cost < MO_LIMIT_PROGRAM_CHECK_WORK);
    *f.signatures->work = MO_LIMIT_PROGRAM_CHECK_WORK - cost;
    CHECK(body(&f, "run") != NULL && f.diags.count == 0);
    CHECK(*f.signatures->work == MO_LIMIT_PROGRAM_CHECK_WORK);
    CHECK(body(&f, "run") == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_CHECK_PROGRAM_WORK);
    finish(&f);

    /* A flat function repeatedly constrains a shared, unresolved type DAG.
     * Its source and type depths fit; inference must stop at the work budget,
     * not exhaust the larger arena or expand the DAG exponentially. */
    struct mo_buffer source;
    mo_buffer_init(&source, MO_LIMIT_FILE_SIZE);
    MO_ASSERT(mo_buffer_append_text(
        &source, "module Main\nfn duplicate(value: T) : (T, T)\n(value, value)\nend\n"
                 "fn ignore(_value: T)\nend\nfn run()\n_v0 = 1\n"));
    for (uint32_t i = 1; i <= 250; i++) {
        MO_ASSERT(mo_buffer_append_text(&source, "_v"));
        MO_ASSERT(mo_buffer_append_uint(&source, i));
        MO_ASSERT(mo_buffer_append_text(&source, " = duplicate(_v"));
        MO_ASSERT(mo_buffer_append_uint(&source, i - 1));
        MO_ASSERT(mo_buffer_append_text(&source, ")\n"));
    }
    for (uint32_t i = 0; i < 40000; i++)
        MO_ASSERT(mo_buffer_append_text(&source, "ignore(_v250)\n"));
    MO_ASSERT(mo_buffer_append_text(&source, "end\n"));
    MO_ASSERT(mo_buffer_append_byte(&source, 0));
    init_limit(&f, (const char *)source.bytes, NULL, 256 << 20);
    CHECK(body(&f, "run") == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_CHECK_WORK);
    CHECK(*f.signatures->work == MO_LIMIT_SOURCE_CHECK_WORK);
    finish(&f);
    mo_buffer_free(&source);
}

int main(void) {
    question_name();
    tuple_position();
    inferred_uses();
    scope_errors();
    literal_ranges();
    float_locale();
    assignment_equivalence();
    discarded_tails();
    module_calls();
    imported_meaning();
    contextual_arguments();
    source_order();
    diagnostic_contexts();
    specifications();
    allocation_limits();
    unused_fix();
    reference_bodies();
    shared_types();
    assignment_notes();
    repeated_signature();
    syntax_depth();
    work_limits();
    return mo_test_report("check bodies");
}
