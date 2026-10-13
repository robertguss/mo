#define _POSIX_C_SOURCE 200809L
#include "bytecode/bytecode.h"
#include "bytecode_text.h"
#include "heap/types.h"
#include "test.h"

#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static const char *corpus;

static void seed(const struct mo_buffer *text) {
    if (corpus == NULL)
        return;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < text->length; i++)
        hash = (hash ^ text->bytes[i]) * UINT64_C(1099511628211);
    char path[512];
    int n = snprintf(path, sizeof path, "%s/text-%016llx.mbt", corpus, (unsigned long long)hash);
    CHECK(n > 0 && (size_t)n < sizeof path);
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f != NULL) {
        CHECK(fwrite(text->bytes, 1, text->length, f) == text->length);
        CHECK(fclose(f) == 0);
    }
}

static MO_MUST_USE struct mo_image *assemble(const struct mo_buffer *text) {
    struct mo_buffer bytes;
    struct mo_assemble_error ae;
    struct mo_load_error le;
    mo_buffer_init(&bytes, MO_LIMIT_BYTECODE_SIZE);
    bool ok = mo_bytecode_assemble((const char *)text->bytes, text->length, &bytes, &ae);
    CHECK(ok);
    struct mo_image *image = NULL;
    if (!ok) {
        CHECK(fprintf(stderr, "assembly refusal at %u: %s\n", ae.offset, ae.reason) >= 0);
    } else {
        seed(text);
        image = mo_load(bytes.bytes, bytes.length, &le);
        CHECK(image != NULL);
        text_roundtrip(&bytes);
    }
    mo_buffer_free(&bytes);
    return image;
}

static void literal_text(struct mo_buffer *text, const char *type, const char *literal) {
    CHECK(mo_buffer_append_text(text, "bytecode 0.1 shorthand\nmodule A exposes [A.value]\n"
                                      "function A.value() : "));
    CHECK(mo_buffer_append_text(text, type));
    CHECK(mo_buffer_append_text(text, " function in A\n  registers [r0: "));
    CHECK(mo_buffer_append_text(text, type));
    CHECK(mo_buffer_append_text(text, "]\n  const r0, "));
    CHECK(mo_buffer_append_text(text, literal));
    if (strcmp(type, "BigInt") == 0 || strcmp(type, "Text") == 0 || strcmp(type, "Bytes") == 0)
        CHECK(mo_buffer_append_text(text, "\n  dup r0, r0"));
    CHECK(mo_buffer_append_text(text, "\n  ret r0\nend\n"));
}

static void scalars(void) {
    const struct {
        const char *type, *literal;
        uint64_t bits;
    } cases[] = {
        {"Unit", "()", 0},
        {"Bool", "true", 1},
        {"Bool", "false", 0},
        {"Byte", "255", 255},
        {"Byte", "0b1010_0101", 165},
        {"Int", "-9223372036854775808", UINT64_C(0x8000000000000000)},
        {"Int", "9223372036854775807", UINT64_C(0x7fffffffffffffff)},
        {"Int", "-0x7f", UINT64_C(0xffffffffffffff81)},
        {"Float", "-0.0", UINT64_C(0x8000000000000000)},
        {"Float", "1.25e+2", UINT64_C(0x405f400000000000)},
        {"Float", "nan", UINT64_C(0x7ff8000000000000)},
        {"Char", "'🙂'", 0x1f642},
        {"Char", "'\\u{10ffff}'", 0x10ffff},
        {"Char", "'\\0'", 0},
        {"Char", "'\\''", '\''},
        {"Char", "U+d7ff", 0xd7ff},
        {"Time", "-62135596800000000", UINT64_C(0xff23400100d44000)},
        {"Time", "253402300799999999", UINT64_C(253402300799999999)},
        {"Duration", "-1", UINT64_MAX},
        {"Decimal", "-0.125", UINT64_C(0xfe43e9298b138000)},
        {"Decimal", "0.000000000000000001", 1},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        struct mo_buffer text;
        mo_buffer_init(&text, MO_LIMIT_BYTECODE_TEXT_SIZE);
        literal_text(&text, cases[i].type, cases[i].literal);
        struct mo_image *image = assemble(&text);
        if (image != NULL) {
            const struct mo_bytecode *b = mo_image_bytecode(image);
            CHECK(b->constant_count == 1);
            uint8_t expected[16] = {0};
            for (unsigned j = 0; j < 8; j++)
                expected[j] = (uint8_t)(cases[i].bits >> (8 * j));
            if (strcmp(cases[i].type, "Decimal") == 0 && cases[i].literal[0] == '-')
                memset(expected + 8, 255, 8);
            CHECK(memcmp(b->constants[0].value.immediate, expected, 16) == 0);
            mo_image_free(image);
        }
        mo_buffer_free(&text);
    }
    struct mo_buffer text;
    mo_buffer_init(&text, MO_LIMIT_BYTECODE_TEXT_SIZE);
    literal_text(&text, "BigInt", "-18446744073709551616");
    struct mo_image *image = assemble(&text);
    if (image != NULL) {
        const struct mo_bytecode_bigint *b = &mo_image_bytecode(image)->constants[0].value.bigint;
        static const uint8_t magnitude[] = {0, 0, 0, 0, 0, 0, 0, 0, 1};
        CHECK(b->sign == MO_BIGINT_NEGATIVE && b->magnitude.length == sizeof magnitude &&
              memcmp(b->magnitude.bytes, magnitude, sizeof magnitude) == 0);
        mo_image_free(image);
    }
    mo_buffer_clear(&text);
    literal_text(&text, "Text", "\"a\\x00🙂\\\"\\\\\"");
    image = assemble(&text);
    if (image != NULL) {
        const struct mo_bytecode *b = mo_image_bytecode(image);
        const struct mo_bytecode_string *s = &b->strings[b->constants[0].value.string];
        static const char expected[] = "a\0🙂\"\\";
        CHECK(s->length == sizeof expected - 1 && memcmp(s->bytes, expected, s->length) == 0);
        mo_image_free(image);
    }
    mo_buffer_free(&text);
}

static void programs(void) {
    static const char *const cases[] = {
        "bytecode 0.1 shorthand\nglobal A.answer: Int in A\ninit [A.init]\n"
        "function A.get() : Int function in A\n registers [r0: Int]\n"
        " global r0, A.answer\n ret r0\nend\n"
        "function A.init() : Unit init in A\n registers [r0: Int, r1: Unit]\n"
        " const r0, 7\n set_global A.answer, r0\n const r1, ()\n ret r1\nend\n"
        "module A exposes [A.get]\n",

        "bytecode 0.1 shorthand\n"
        "function A.apply() : Int function in A\n"
        " registers [r0: Int, r1: fn(Int): Int, r2: Int]\n"
        " const r0, 3\n make_closure r1, A.add, r0\n const r0, 7\n"
        " call_closure r2, r1, r0\n ret r2\nend\n"
        "function A.add(x: Int owned) : Int closure in A captures [Int]\n"
        " registers [r2: Int, r3: Int]\n field r2, r1, 0\n add_int r3, r0, r2\n"
        " drop r1\n ret r3\nend\nmodule A exposes [A.apply]\n",

        "bytecode 0.1 shorthand\nchoice Pair\n case Empty\n"
        " case Both(left: Int, right: Int)\nend\n"
        "function A.choose(n: Int owned) : Int function in A\n"
        " registers [r1: Pair, r2: Int]\n make_case r1, Pair, 1, r0, r0\n"
        " switch r1, @empty, @both\n @empty:\n const r2, 0\n drop r1\n ret r2\n"
        " @both:\n case_field r2, r1, 1, 0\n drop r1\n ret r2\nend\n"
        "module A exposes [Pair(..), A.choose]\n",

        "bytecode 0.1 shorthand\nrecord Box\n value: Int\nend\n"
        "function A.rebuild(n: Int owned) : Box function in A\n"
        " registers [r1: Box, r2: Box]\n construct r1, t0, r0\n reset r2, r1\n"
        " reuse r1, r2, Box, r0\n set_field r1, r1, 0, r0\n ret r1\nend\n"
        "module A exposes [Box(..), A.rebuild]\n",

        "bytecode 0.1 shorthand\nmodule A exposes []\n"
        "function A.test() : Unit test spec in A\n"
        " registers [r0: Console, r1: Text, r2: Unit, r3: Bytes, r4: Int]\n"
        " text \"prints and counts\"\n host r0, Console.fake\n const r1, \"hello\"\n"
        " host r2, Console.print, r0, r1\n const r3, bytes hex\"0012ff\"\n"
        " prim r4, Bytes.size, fn(Bytes): Int, r3\n drop r0\n ret r2\nend\n",

        "bytecode 0.1 shorthand\nstrings\n s0 \"A\"\n s1 \"A\"\nend\n"
        "types\n t0 int\nend\nconstants\n k0 t0 7\n k1 t0 7\nend\n"
        "module A exposes [A.id]\nfunction A.id(n: Int owned) : Int function in A\n"
        " registers [r1: Int]\n const r1, k1\n const r1, 7\n ret r1\nend\n",

        "bytecode 0.1 shorthand\nconstants\n k0 t0 7\nend\n"
        "function A.id(n: Int owned) : Int function in A\n registers []\n ret r0\nend\n"
        "module A exposes [A.id]\n",

        "bytecode 0.1 shorthand\nrecord Wrapper(Int,Text)\n value: Int\nend\n"
        "function A.unwrap(n: Wrapper(Int, Text) borrowed) : Int function in A\n"
        " registers [r1: Int]\n field r1, r0, 0\n ret r1\nend\n"
        "module A exposes [Wrapper(Int, Text)(..), A.unwrap]\n",
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        struct mo_buffer text;
        mo_buffer_init(&text, MO_LIMIT_BYTECODE_TEXT_SIZE);
        CHECK(mo_buffer_append_text(&text, cases[i]));
        struct mo_image *image = assemble(&text);
        if (image != NULL) {
            const struct mo_bytecode *b = mo_image_bytecode(image);
            if (i == 2)
                CHECK(b->layout_count == 1 && b->layouts[0].kind == MO_LAYOUT_CASE &&
                      b->layouts[0].case_index == 1 && b->layouts[0].field_count == 2);
            if (i == 5) {
                CHECK(b->string_count == 4 && b->constants[0].value.immediate[0] == 7 &&
                      b->constants[1].value.immediate[0] == 7 && b->constant_count == 2);
                CHECK(b->modules[0].name == 0 && b->functions[0].code[2] == 1 &&
                      b->functions[0].code[5] == 0);
            }
            mo_image_free(image);
        }
        mo_buffer_free(&text);
    }
}

static void refusal(const char *text, size_t length, uint32_t offset, const char *reason) {
    struct mo_buffer bytes;
    struct mo_assemble_error error = {.offset = UINT32_MAX, .reason = NULL};
    mo_buffer_init(&bytes, MO_LIMIT_BYTECODE_SIZE);
    CHECK(!mo_bytecode_assemble(text, length, &bytes, &error));
    CHECK(bytes.length == 0);
    CHECK(error.offset == offset && error.reason != NULL && strcmp(error.reason, reason) == 0);
    if (error.reason != NULL && (error.offset != offset || strcmp(error.reason, reason) != 0))
        CHECK(fprintf(stderr, "expected %u/%s; got %u/%s\n", offset, reason, error.offset,
                      error.reason) >= 0);
    mo_buffer_free(&bytes);
}

static void refusals(void) {
    static const struct {
        const char *text, *at, *reason;
    } cases[] = {
        {"", "", "expected bytecode header"},
        {"bytecode 0.2 indexed\n", "0.2", "expected bytecode version 0.1"},
        {"bytecode 0.1 mixed\n", "mixed", "expected indexed or shorthand"},
        {"bytecode 0.1 indexed\ntypes\n", "types", "expected strings section"},
        {"bytecode 0.1 shorthand\nstrings\n s1 \"a\"\nend\n", "s1",
         "expected consecutive entry index within section limit"},
        {"bytecode 0.1 shorthand\nstrings\n s0 \"a\"\n s0 \"b\"\nend\n", "s0 \"b\"",
         "expected consecutive entry index within section limit"},
        {"bytecode 0.1 shorthand\nstrings\n s0 hex\"61\"\nend\n", "hex",
         "expected quoted UTF-8 string"},
        {"bytecode 0.1 shorthand\nstrings\n s0 \"\\n\"\nend\n", "n",
         "expected quote, backslash or hex escape"},
        {"bytecode 0.1 shorthand\nstrings\n s0 \"\\xg0\"\nend\n", "xg0",
         "expected two hex digits after \\x"},
        {"bytecode 0.1 shorthand\nstrings\n s0 \"\\xff\"\nend\n", "\"\\xff",
         "expected valid UTF-8 string"},
        {"bytecode 0.1 shorthand\nstrings\n s0 \"open", "\"open", "expected closing quote"},
        {"bytecode 0.1 shorthand\r", "\r", "expected LF after CR"},
        {"bytecode 0.1 shorthand\nmodule A exposes []\nmodule A exposes []\n",
         "module A exposes []\nmodule", "expected unique symbolic declaration"},
        {"bytecode 0.1 shorthand\nmodule m0 exposes []\n", "m0",
         "expected symbolic name, not reserved index spelling"},
        {"bytecode 0.1 shorthand\ninit []\ninit []\n", "init []\ninit",
         "expected at most one init declaration"},
        {"bytecode 0.1 shorthand\nfunctions\n", "functions",
         "expected record, choice, global, init, function or module"},
        {"bytecode 0.1 shorthand\ntypes\n t0 list t1\nend\n", "t1",
         "expected type index in final pool"},
        {"bytecode 0.1 indexed\nstrings\nend\ntypes\n t0 int\nend\nlayouts\nend\n"
         "constants\n k0 t1 7\nend\nglobals\nend\ninit\nend\nfunctions\nend\n"
         "modules\nend\ndebug\nend\n",
         "t1", "expected type index in final pool"},
        {"bytecode 0.1 shorthand\nconstants\n k0 t9 7\nend\n"
         "module A exposes [A.id]\nfunction A.id(n: Int owned) : Int function in A\n"
         "registers []\nret r0\nend\n",
         "t9", "expected type index in final pool"},
        {"bytecode 0.1 shorthand\nmodule A exposes [A.id]\n"
         "function A.id(n: Int owned) : Int function in A\n"
         "registers [r1: t9]\nconst r1, 7\nret r0\nend\n",
         "t9", "expected type index in final pool"},
        {"bytecode 0.1 shorthand\ntypes\n t0 record s9 []\nend\n", "s9",
         "expected string index in final pool"},
        {"bytecode 0.1 shorthand\nglobal A.value: Int in m9\nmodule A exposes []\n", "m9",
         "expected module index in final pool"},
        {"bytecode 0.1 shorthand\nmodule A exposes [A.id]\n"
         "function A.id(n: Int owned) : Int function in A\n"
         "registers [r1: fn(Int): Int]\nconstruct r1, closure(f9)\nret r0\nend\n",
         "f9", "expected function index in final pool"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const char *at = strstr(cases[i].text, cases[i].at);
        MO_ASSERT(at != NULL);
        size_t offset = (size_t)(at - cases[i].text);
        if (i == 12)
            offset += strlen("module A exposes []\n");
        if (i == 14)
            offset += strlen("init []\n");
        if (i == 7)
            at = strstr(cases[i].text, "\\n"), offset = (size_t)(at - cases[i].text) + 1;
        refusal(cases[i].text, strlen(cases[i].text), (uint32_t)offset, cases[i].reason);
    }
    static const struct {
        const char *type, *literal, *reason;
        unsigned delta;
    } bad_literals[] = {
        {"Byte", "256", "expected numeric literal in type bounds", 0},
        {"Byte", "-1", "expected integer literal in type bounds", 0},
        {"Int", "9223372036854775808", "expected numeric literal in type bounds", 0},
        {"Int", "-9223372036854775809", "expected numeric literal in type bounds", 0},
        {"Int", "1__2", "expected integer digit in literal radix", 2},
        {"Int", "0b2", "expected integer digit in literal radix", 2},
        {"Decimal", "0.0000000000000000001", "expected decimal with at most 18 fraction digits",
         20},
        {"Decimal", "170141183460469231732", "expected Decimal coefficient in bounds", 0},
        {"Time", "-62135596800000001", "expected Time between years 1 and 9999", 0},
        {"Time", "253402300800000000", "expected Time between years 1 and 9999", 0},
        {"Float", "1e+", "expected Float number or nan", 0},
        {"Char", "'ab'", "expected one Unicode scalar or Char escape", 0},
        {"Char", "'\\u{d800}'", "expected one Unicode scalar or Char escape", 0},
        {"Char", "U+110000", "expected Unicode scalar value", 0},
        {"Bytes", "bytes hex\"a\"", "expected even number of hex digits", 11},
        {"BigInt", "12.3", "expected BigInt decimal digit", 2},
    };
    for (size_t i = 0; i < sizeof bad_literals / sizeof bad_literals[0]; i++) {
        struct mo_buffer text;
        mo_buffer_init(&text, MO_LIMIT_BYTECODE_TEXT_SIZE);
        literal_text(&text, bad_literals[i].type, bad_literals[i].literal);
        CHECK(mo_buffer_append_byte(&text, 0));
        const char *input = (const char *)text.bytes, *at = strstr(input, "const r0, ");
        MO_ASSERT(at != NULL);
        uint32_t offset = (uint32_t)(at - input) + 10 + bad_literals[i].delta;
        refusal(input, text.length - 1, offset, bad_literals[i].reason);
        mo_buffer_free(&text);
    }
}

static void instruction_refusals(void) {
    static const struct {
        const char *metadata, *body, *at, *reason;
    } cases[] = {
        {"", "move r1, r3\nret r1\n", "r3",
         "instruction index out of range or target not a boundary"},
        {"", "move r1\nret r1\n", "\nret", "expected instruction operand"},
        {"", "unknown r1\nret r0\n", "unknown", "expected opcode, @label: or span"},
        {"", "ret r1\n", "r1", "register is not readable"},
        {"", "ret r2\n", "ret", "return type"},
        {"", "const r1, k9\nret r1\n", "k9", "expected constant index in final pool"},
        {"", "call r1, f9, r0\nret r1\n", "f9", "expected function index in final pool"},
        {"", "call r1, A.missing, r0\nret r1\n", "A.missing", "expected declared function"},
        {"", "prim r1, p999, fn(Int): Int, r0\nret r1\n", "p999",
         "expected primitive index in range"},
        {"", "host r1, h999, r0\nret r1\n", "h999", "expected host index in range"},
        {"", "jump @missing\nret r0\n", "@missing", "expected defined label in this function"},
        {"", "jump @done\n@done:\n", "@done",
         "instruction index out of range or target not a boundary"},
        {"", "span - 1 0\nret r0\n", "span",
         "expected one ordered span per offset, with zero absent bounds"},
        {"", "ret r0\nspan \"file\" 0 1\n", "span", "invalid debug span"},
        {"variables [(r0, \"n\", @start, @start)]\n", "ret r0\n", "(r0",
         "invalid debug register range"},
        {"declaration (-, 0, 1)\n", "ret r0\n", "-, 0",
         "expected ordered location, or zero bounds for absent file"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        struct mo_buffer text;
        mo_buffer_init(&text, MO_LIMIT_BYTECODE_TEXT_SIZE);
        CHECK(mo_buffer_append_text(&text, "bytecode 0.1 shorthand\nmodule A exposes [A.id]\n"
                                           "function A.id(n: Int owned) : Int function in A\n"
                                           "registers [r1: Int, r2: Bool]\n"));
        size_t start = text.length;
        CHECK(mo_buffer_append_text(&text, cases[i].metadata));
        CHECK(mo_buffer_append_text(&text, "@start:\n"));
        CHECK(mo_buffer_append_text(&text, cases[i].body));
        CHECK(mo_buffer_append_text(&text, "end\n"));
        CHECK(mo_buffer_append_byte(&text, 0));
        const char *input = (const char *)text.bytes, *at = strstr(input + start, cases[i].at);
        MO_ASSERT(at != NULL);
        refusal(input, text.length - 1, (uint32_t)(at - input), cases[i].reason);
        mo_buffer_free(&text);
    }
}

static void boundaries(void) {
    struct mo_buffer text, literal;
    mo_buffer_init(&text, MO_LIMIT_BYTECODE_TEXT_SIZE);
    mo_buffer_init(&literal, MO_LIMIT_BYTECODE_TEXT_SIZE);
    for (unsigned i = 0; i < MO_LIMIT_BYTECODE_BIGINT_DIGITS; i++)
        CHECK(mo_buffer_append_byte(&literal, '9'));
    CHECK(mo_buffer_append_byte(&literal, 0));
    literal_text(&text, "BigInt", (const char *)literal.bytes);
    struct mo_image *image = assemble(&text);
    if (image != NULL) {
        const struct mo_bytecode_bigint *b = &mo_image_bytecode(image)->constants[0].value.bigint;
        /* 10^4096 - 1: log2(10)*4096 needs 1701 bytes, and its low 4096 bits are ones. */
        CHECK(b->magnitude.length == 1701 && b->sign == MO_BIGINT_POSITIVE);
        for (unsigned i = 0; i < 512 && i < b->magnitude.length; i++)
            CHECK(b->magnitude.bytes[i] == 255);
        mo_image_free(image);
    }
    mo_buffer_clear(&text);
    literal.length--;
    CHECK(mo_buffer_append_text(&literal, "9"));
    CHECK(mo_buffer_append_byte(&literal, 0));
    literal_text(&text, "BigInt", (const char *)literal.bytes);
    CHECK(mo_buffer_append_byte(&text, 0));
    const char *input = (const char *)text.bytes, *at = strstr(input, "const r0, ");
    MO_ASSERT(at != NULL);
    refusal(input, text.length - 1, (uint32_t)(at - input) + 10,
            "expected BigInt within decimal digit limit (use bigint sign hex)");

    mo_buffer_clear(&text);
    CHECK(mo_buffer_append_text(&text, "bytecode 0.1 shorthand\nstrings\n s0 \""));
    size_t start = text.length;
    for (unsigned i = 0; i < MO_LIMIT_STRING_SIZE; i++)
        CHECK(mo_buffer_append_byte(&text, 'a'));
    CHECK(mo_buffer_append_text(&text, "\"\nend\n"));
    image = assemble(&text);
    if (image != NULL) {
        const struct mo_bytecode *b = mo_image_bytecode(image);
        CHECK(b->string_count == 1 && b->strings[0].length == MO_LIMIT_STRING_SIZE &&
              b->strings[0].bytes[0] == 'a' &&
              b->strings[0].bytes[MO_LIMIT_STRING_SIZE - 1] == 'a');
        mo_image_free(image);
    }
    /* Insert one byte before the closing quote, not a second independent string. */
    text.length = start + MO_LIMIT_STRING_SIZE;
    CHECK(mo_buffer_append_text(&text, "a\"\nend\n"));
    refusal((const char *)text.bytes, text.length, (uint32_t)(start + MO_LIMIT_STRING_SIZE),
            "expected string within byte limit");

    mo_buffer_clear(&text);
    CHECK(mo_buffer_append_text(&text, "bytecode 0.1 shorthand\nmodule A exposes [A.id]\n"
                                       "function A.id(n: Int owned) : Int function in A\n"
                                       "registers [r1: "));
    for (unsigned i = 0; i < MO_LIMIT_NESTING_DEPTH - 1; i++)
        CHECK(mo_buffer_append_text(&text, "List("));
    CHECK(mo_buffer_append_text(&text, "Int"));
    for (unsigned i = 0; i < MO_LIMIT_NESTING_DEPTH - 1; i++)
        CHECK(mo_buffer_append_text(&text, ")"));
    CHECK(mo_buffer_append_text(&text, "]\nret r0\nend\n"));
    image = assemble(&text);
    if (image != NULL)
        mo_image_free(image);
    mo_buffer_clear(&text);
    CHECK(mo_buffer_append_text(&text, "bytecode 0.1 shorthand\nmodule A exposes []\n"
                                       "function A.id(n: Int owned) : Int function in A\n"
                                       "registers [r1: "));
    for (unsigned i = 0; i < MO_LIMIT_NESTING_DEPTH; i++)
        CHECK(mo_buffer_append_text(&text, "List("));
    size_t leaf = text.length;
    CHECK(mo_buffer_append_text(&text, "Int"));
    for (unsigned i = 0; i < MO_LIMIT_NESTING_DEPTH; i++)
        CHECK(mo_buffer_append_text(&text, ")"));
    CHECK(mo_buffer_append_text(&text, "]\nret r0\nend\n"));
    refusal((const char *)text.bytes, text.length, (uint32_t)leaf,
            "expected type within nesting limit");

    /* A sparse file supplies a real, readable 2 GiB input without committing 2 GiB RAM.
     * At the text ceiling parsing reaches the NUL byte; ceiling+1 refuses before parsing. */
    FILE *sparse = tmpfile();
    CHECK(sparse != NULL);
    if (sparse != NULL) {
        int fd = fileno(sparse);
        CHECK(fd >= 0 && ftruncate(fd, (off_t)MO_LIMIT_BYTECODE_TEXT_SIZE + 1) == 0);
        void *mapped =
            mmap(NULL, (size_t)MO_LIMIT_BYTECODE_TEXT_SIZE + 1, PROT_READ, MAP_PRIVATE, fd, 0);
        CHECK(mapped != MAP_FAILED);
        if (mapped != MAP_FAILED) {
            refusal(mapped, (size_t)MO_LIMIT_BYTECODE_TEXT_SIZE, 0, "expected text token");
            refusal(mapped, (size_t)MO_LIMIT_BYTECODE_TEXT_SIZE + 1, 0, "bytecode text size limit");
            CHECK(munmap(mapped, (size_t)MO_LIMIT_BYTECODE_TEXT_SIZE + 1) == 0);
        }
        CHECK(fclose(sparse) == 0);
    }

    static const char minimal[] = "bytecode 0.1 shorthand\n";
    struct mo_buffer small;
    struct mo_assemble_error error;
    mo_buffer_init(&small, MO_BYTECODE_HEADER_SIZE + 4 * MO_SECTION_COUNT - 1);
    CHECK(!mo_bytecode_assemble(minimal, sizeof minimal - 1, &small, &error));
    CHECK(small.length == 0 && error.offset == 0 &&
          strcmp(error.reason, "bytecode output limit") == 0);
    mo_buffer_free(&small);
    mo_buffer_free(&literal);
    mo_buffer_free(&text);
}

static void ordered_names(void) {
    /* Monotone keys would make an unbalanced pool/symbol tree exceed its stack bound.
     * Repeated lookups must also reuse the seed, rather than scanning every earlier name. */
    struct mo_buffer text;
    mo_buffer_init(&text, MO_LIMIT_BYTECODE_TEXT_SIZE);
    CHECK(mo_buffer_append_text(&text, "bytecode 0.1 shorthand\n"));
    for (unsigned i = 0; i < 8192; i++) {
        char declaration[128];
        int n = snprintf(declaration, sizeof declaration,
                         "record RepeatedPrefix.Record%08u\n value: Int\nend\n", i);
        CHECK(n > 0 && (size_t)n < sizeof declaration);
        CHECK(mo_buffer_append_text(&text, declaration));
    }
    struct mo_image *image = assemble(&text);
    if (image != NULL) {
        const struct mo_bytecode *b = mo_image_bytecode(image);
        CHECK(b->type_count == 8193 && b->string_count == 8193);
        CHECK(strcmp(b->types[0].name, "RepeatedPrefix.Record00000000") == 0 &&
              strcmp(b->types[8191].name, "RepeatedPrefix.Record00008191") == 0);
        CHECK(b->types[0].fields[0].type == 8192 && b->types[8191].fields[0].type == 8192);
        mo_image_free(image);
    }
    mo_buffer_free(&text);
}

static void empty(void) {
    static const char expected[] = "bytecode 0.1 indexed\nstrings\nend\ntypes\nend\nlayouts\nend\n"
                                   "constants\nend\nglobals\nend\ninit\nend\nfunctions\nend\n"
                                   "modules\nend\ndebug\nend\n";
    struct mo_bytecode file = {0};
    struct mo_buffer wire, text, small;
    struct mo_write_error we;
    struct mo_load_error le;
    struct mo_assemble_error ae;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    mo_buffer_init(&text, MO_LIMIT_BYTECODE_TEXT_SIZE);
    mo_buffer_init(&small, sizeof expected - 2);
    CHECK(mo_bytecode_write(&file, &wire, &we));
    CHECK(mo_bytecode_disassemble(wire.bytes, wire.length, &text, &le));
    CHECK(text.length == sizeof expected - 1);
    CHECK(memcmp(text.bytes, expected, text.length) == 0);
    struct mo_buffer assembled;
    mo_buffer_init(&assembled, MO_LIMIT_BYTECODE_SIZE);
    CHECK(mo_bytecode_assemble(expected, sizeof expected - 1, &assembled, &ae));
    CHECK(assembled.length == wire.length && memcmp(assembled.bytes, wire.bytes, wire.length) == 0);
    mo_buffer_free(&assembled);
    CHECK(!mo_bytecode_disassemble(wire.bytes, wire.length, &small, &le));
    CHECK(small.length == 0 && le.offset == 0 && le.kind == MO_BYTECODE_LIMIT);
    wire.bytes[0] = 'X';
    mo_buffer_clear(&text);
    CHECK(!mo_bytecode_disassemble(wire.bytes, wire.length, &text, &le));
    CHECK(text.length == 0 && le.kind == MO_BYTECODE_INVALID);
    mo_buffer_free(&small);
    mo_buffer_free(&text);
    mo_buffer_free(&wire);
}

int main(int argc, char **argv) {
    MO_ASSERT(argc == 1 || (argc == 3 && strcmp(argv[1], "--corpus") == 0));
    if (argc == 3)
        corpus = argv[2];
    empty();
    scalars();
    programs();
    refusals();
    instruction_refusals();
    boundaries();
    ordered_names();
    return mo_test_report("bytecode text");
}
