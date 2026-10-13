#include "base/buffer.h"
#include "base/codes.h"
#include "base/limits.h"
#include "check/check.h"
#include "test.h"

#include <string.h>

struct fixture {
    struct mo_arena syntax, arena;
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_module_interfaces *interfaces;
    struct mo_signatures *signatures;
};

static void init(struct fixture *f, const char *first, const char *second) {
    mo_arena_init(&f->syntax, 16 << 20);
    mo_arena_init(&f->arena, 16 << 20);
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
        {"fn invoke(f: fn(Int) : Int) : Int\nf(4)\nend\n"
         "fn run() : Int\ninvoke(fn(x) x + 2 end)\nend\n",
         "Int"},
        {"fn run(f: fn() : fn() : Int) : Int\nf()()\nend\n", "Int"},
        {"fn run(pair: ((Int, Text), Bool)) : Text\npair.0.1\nend\n", "Text"},
        {"fn run(flag: Bool) : Int\nif flag\nx = 1\nx\nelse\nx = 2\nx\nend\nend\n", "Int"},
        {"fn run(flag: Bool) : Int\nreturn 1 if flag\n2\nend\n", "Int"},
        {"fn run(flag: Bool) : Int\nif flag\nreturn 1\nelse\nstop \"impossible\"\nend\nend\n",
         "Int"},
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
        if (out != NULL)
            CHECK(strcmp(out->items[out->count - 1].type->name, cases[i].result) == 0);
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
        {"fn run()\n_xs = []\nend\n", MO_CODE_CANNOT_INFER},
        {"fn run() : Int\nf = fn(x) x(x) end\nf(1)\nend\n", MO_CODE_TYPE_MISMATCH},
        {"fn callee(x: Int) : Int\nx\nend\nfn run() : Int\ncallee()\nend\n",
         MO_CODE_CALL_ARGUMENTS},
        {"fn run()\n1\nUnit\nend\n", MO_CODE_UNUSED_VALUE},
        {"fn run()\nx += 1\nend\n", MO_CODE_NOT_VARIABLE},
        {"fn run()\nbreak\nend\n", MO_CODE_EXIT_OUTSIDE_LOOP},
        {"fn run() : Int\n9223372036854775808\nend\n", MO_CODE_LITERAL_RANGE},
        {"fn run(pair: (Int, Text)) : Int\npair.2\nend\n", MO_CODE_TUPLE_POSITION},
        {"fn run(pair: (Int, Text)) : Int\npair.99999999999999999999\nend\n",
         MO_CODE_TUPLE_POSITION},
        {"fn run(flag: Bool) : Bool\nif flag\ntrue\nend\nend\n", MO_CODE_TYPE_MISMATCH},
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
    number("Decimal", "-170141183460469231731.687303715884105728", true);
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

int main(void) {
    question_name();
    tuple_position();
    inferred_uses();
    scope_errors();
    literal_ranges();
    module_calls();
    allocation_limits();
    shared_types();
    return mo_test_report("check bodies");
}
