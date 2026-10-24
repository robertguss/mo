#include "base/buffer.h"
#include "base/codes.h"
#include "base/limits.h"
#include "syntax/syntax.h"
#include "test.h"

#include <string.h>
#include <time.h>

struct fixture {
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_arena arena;
    struct mo_module_syntax *module;
};

static void init_bytes(struct fixture *f, const uint8_t *bytes, size_t length, bool fragment) {
    MO_ASSERT(mo_sources_init(&f->sources));
    MO_ASSERT(mo_diags_init(&f->diags));
    mo_arena_init(&f->arena, 1 << 30);
    uint32_t file;
    struct mo_source_error problem;
    MO_ASSERT(mo_sources_add_bytes(&f->sources, "test.mo", bytes, length, &file, &problem));
    f->module = fragment ? mo_parse_fragment(&f->arena, &f->sources, file, &f->diags)
                         : mo_parse(&f->arena, &f->sources, file, &f->diags);
}

static void init(struct fixture *f, const char *text, bool fragment) {
    init_bytes(f, (const uint8_t *)text, strlen(text), fragment);
}

static void free_fixture(struct fixture *f) {
    mo_arena_free(&f->arena);
    mo_diags_free(&f->diags);
    mo_sources_free(&f->sources);
}

static MO_MUST_USE struct mo_syntax *item(struct fixture *f, uint32_t index) {
    MO_ASSERT(f->module != NULL && index < f->module->tree->children[1]->count);
    return f->module->tree->children[1]->children[index];
}

static void spans(const struct fixture *f, const struct mo_syntax *n) {
    if (n == NULL)
        return;
    CHECK(n->span.file == 0 && n->span.start <= n->span.end &&
          n->span.end <= f->sources.files[0].length);
    if (n->text != NULL)
        CHECK(memcmp(n->text, f->sources.files[0].bytes + n->span.start,
                     n->span.end - n->span.start) == 0);
    for (uint32_t i = 0; i < n->count; i++) {
        if (n->children[i] != NULL) {
            CHECK(n->children[i]->span.start >= n->span.start);
            if (n->children[i]->span.end > n->span.end)
                (void)fprintf(stderr, "span kind %u [%u,%u) child %u [%u,%u)\n", (unsigned)n->kind,
                              n->span.start, n->span.end, (unsigned)n->children[i]->kind,
                              n->children[i]->span.start, n->children[i]->span.end);
            CHECK(n->children[i]->span.end <= n->span.end);
        }
        spans(f, n->children[i]);
    }
}

static void shape(const struct mo_syntax *n, const enum mo_syntax_kind *kinds, size_t count,
                  size_t *at) {
    if (n == NULL)
        return;
    CHECK(*at < count);
    if (*at < count)
        CHECK(n->kind == kinds[*at]);
    (*at)++;
    for (uint32_t i = 0; i < n->count; i++)
        shape(n->children[i], kinds, count, at);
}

#define SHAPE(text, ...)                                                                           \
    {                                                                                              \
        text, (enum mo_syntax_kind[]){__VA_ARGS__},                                                \
            sizeof((enum mo_syntax_kind[]){__VA_ARGS__}) / sizeof(enum mo_syntax_kind)             \
    }

static void shapes(void) {
    const struct {
        const char *text;
        const enum mo_syntax_kind *kinds;
        size_t count;
    } cases[] = {
        SHAPE("1 + 2 * 3", MO_S_BINARY, MO_S_INT, MO_S_BINARY, MO_S_INT, MO_S_INT),
        SHAPE("not a == b and c or d implies e", MO_S_BINARY, MO_S_BINARY, MO_S_BINARY, MO_S_NOT,
              MO_S_BINARY, MO_S_NAME, MO_S_NAME, MO_S_NAME, MO_S_NAME, MO_S_NAME),
        SHAPE("-try f(3)", MO_S_PREFIX, MO_S_TOKEN, MO_S_TOKEN, MO_S_CALL, MO_S_NAME, MO_S_SEQUENCE,
              MO_S_INT),
        SHAPE("a..b", MO_S_RANGE, MO_S_NAME, MO_S_NAME),
        SHAPE("(a, b,)", MO_S_TUPLE, MO_S_NAME, MO_S_NAME),
        SHAPE("(3)", MO_S_GROUP, MO_S_INT),
        SHAPE("[true, false, 'é', 0xFF, 0b10, 2.5]", MO_S_LIST, MO_S_TRUE, MO_S_FALSE, MO_S_CHAR,
              MO_S_INT, MO_S_INT, MO_S_DECIMAL),
        SHAPE("{a: 3, b: 5,}", MO_S_MAP, MO_S_MAP_ENTRY, MO_S_NAME, MO_S_INT, MO_S_MAP_ENTRY,
              MO_S_NAME, MO_S_INT),
        SHAPE("[]", MO_S_LIST),
        SHAPE("{}", MO_S_MAP),
        SHAPE("Mod.Case(field: value)", MO_S_POSTFIX, MO_S_CAPNAME, MO_S_QUALIFIED, MO_S_CAPNAME,
              MO_S_CALL, MO_S_SEQUENCE, MO_S_NAMED_ARG, MO_S_NAME, MO_S_NAME),
        SHAPE("pair.0.field", MO_S_POSTFIX, MO_S_NAME, MO_S_POSITION, MO_S_INT, MO_S_FIELD_ACCESS,
              MO_S_NAME),
        SHAPE("if c: a else if d: b else: e", MO_S_LINE_IF, MO_S_NAME, MO_S_NAME, MO_S_SEQUENCE,
              MO_S_IF_PART, MO_S_NAME, MO_S_NAME, MO_S_ELSE, MO_S_NAME),
        SHAPE("value is [head, ..rest]", MO_S_IS, MO_S_NAME, MO_S_LIST, MO_S_NAME, MO_S_REST,
              MO_S_NAME),
        SHAPE("\"hi #{name}: #{2}\"", MO_S_TEXT_INTERPOLATED, MO_S_TEXT, MO_S_NAME, MO_S_TEXT,
              MO_S_INT, MO_S_TEXT),
        SHAPE("\"\"\"\n  a\n  b\n  \"\"\"", MO_S_TEXT),
        SHAPE("fn(x, _) x end", MO_S_LAMBDA, MO_S_SEQUENCE, MO_S_NAME, MO_S_UNDERSCORE, MO_S_NAME),
        SHAPE("fn()\n  return 3\nend", MO_S_LAMBDA, MO_S_SEQUENCE, MO_S_SEQUENCE, MO_S_RETURN,
              MO_S_INT),
        SHAPE("stop \"why\"", MO_S_STOP, MO_S_TEXT),
        SHAPE("case value\nSome(x) if x > 2: x\nNone:\n  stop \"absent\"\nend", MO_S_CASE,
              MO_S_NAME, MO_S_SEQUENCE, MO_S_ARM, MO_S_SEQUENCE, MO_S_CALL, MO_S_CAPNAME,
              MO_S_SEQUENCE, MO_S_NAME, MO_S_BINARY, MO_S_NAME, MO_S_INT, MO_S_SEQUENCE,
              MO_S_EXPR_STMT, MO_S_NAME, MO_S_ARM, MO_S_SEQUENCE, MO_S_CAPNAME, MO_S_SEQUENCE,
              MO_S_EXPR_STMT, MO_S_STOP, MO_S_TEXT),
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        struct mo_buffer source;
        mo_buffer_init(&source, MO_LIMIT_FILE_SIZE);
        MO_ASSERT(mo_buffer_append_text(&source, "x = "));
        MO_ASSERT(mo_buffer_append_text(&source, cases[i].text));
        MO_ASSERT(mo_buffer_append_byte(&source, '\n'));
        struct fixture f;
        init_bytes(&f, source.bytes, source.length, true);
        if (mo_diags_any(&f.diags))
            (void)fprintf(stderr, "shape refused: %s\n", cases[i].text);
        CHECK(f.module != NULL && !mo_diags_any(&f.diags));
        if (f.module != NULL && !mo_diags_any(&f.diags)) {
            size_t at = 0;
            shape(item(&f, 0)->children[1], cases[i].kinds, cases[i].count, &at);
            CHECK(at == cases[i].count);
            spans(&f, f.module->tree);
        }
        free_fixture(&f);
        mo_buffer_free(&source);
    }
}

static void declarations_and_statements(void) {
    struct fixture f;
    init(&f,
         "module All\n"
         "struct Pair(T)\nleft: T\nright: Mod.Value\nend\n"
         "enum Choice\nEmpty\nOne(Int)\nNamed(x: Int, y: Text,)\nend\n"
         "fn f(a: fn(Int, Text): Bool, b: (Int, Text),): List(T) where T: Order\n"
         "promise \"ordered\"\nresult == a\nallows a: 1, result: 1\nforbids a: 1, result: 2\nend\n"
         "examples\nf(1) == 2\nend\n"
         "var v = 1\nv += 2\nv.field -= 3\n(low, _) = (1, 2)\n"
         "if a\nbreak if b\nelse if b\ncontinue\nelse\nreturn v if a\nend\n"
         "for (x, (y, _)) in pairs\nassert x == y\nend\n"
         "while a\na = b\nend\nreturn\nend\n"
         "test \"works\"\nassert true\nend\nn = 42\n",
         false);
    CHECK(f.module != NULL && !mo_diags_any(&f.diags));
    if (f.module != NULL && !mo_diags_any(&f.diags)) {
        spans(&f, f.module->tree);
        CHECK(f.module->tree->children[1]->count == 5);
        struct mo_syntax *record = item(&f, 0), *choice = item(&f, 1), *fn = item(&f, 2);
        CHECK(record->kind == MO_S_STRUCT && record->children[1]->count == 1 &&
              record->children[2]->count == 2);
        CHECK(record->children[2]->children[1]->children[1]->children[0]->kind == MO_S_QUALIFIED);
        CHECK(choice->kind == MO_S_ENUM && choice->children[2]->count == 3);
        CHECK(choice->children[2]->children[0]->children[1] == NULL);
        CHECK(choice->children[2]->children[1]->children[1]->kind == MO_S_TYPE);
        CHECK(choice->children[2]->children[2]->children[1]->count == 2);
        CHECK(fn->count == 6 && fn->children[1]->count == 2 && fn->children[3]->kind == MO_S_WHERE);
        CHECK(fn->children[1]->children[0]->children[1]->token == MO_T_FN);
        CHECK(fn->children[1]->children[1]->children[1]->token == MO_T_LPAREN);
        CHECK(fn->children[4]->count == 2 && fn->children[4]->children[0]->kind == MO_S_PROMISE);
        CHECK(fn->children[4]->children[0]->children[2]->children[0]->token == MO_T_ALLOWS);
        const enum mo_syntax_kind statements[] = {MO_S_VAR,   MO_S_ADD_ASSIGN, MO_S_SUB_ASSIGN,
                                                  MO_S_BIND,  MO_S_EXPR_STMT,  MO_S_FOR,
                                                  MO_S_WHILE, MO_S_RETURN};
        CHECK(fn->children[5]->count == sizeof statements / sizeof statements[0]);
        for (uint32_t i = 0; i < fn->children[5]->count; i++)
            CHECK(fn->children[5]->children[i]->kind == statements[i]);
        struct mo_syntax *conditional = fn->children[5]->children[4]->children[0];
        CHECK(conditional->kind == MO_S_IF && conditional->children[2]->count == 2);
        CHECK(conditional->children[2]->children[0]->kind == MO_S_IF_PART);
        CHECK(conditional->children[2]->children[1]->kind == MO_S_ELSE);
        CHECK(item(&f, 3)->kind == MO_S_TEST && item(&f, 4)->kind == MO_S_CONST);
    }
    free_fixture(&f);
}

static void headers(void) {
    struct fixture f;
    const char *text =
        "module Shop.Orders\nexpose Order(..), total\nuse Shop.Money{Money}\nuse Json\nn = 42\n";
    init(&f, text, false);
    struct mo_header header;
    CHECK(mo_read_header(&f.arena, &f.sources, 0, &header, &f.diags));
    CHECK(header.path->count == 2);
    CHECK(header.expose->count == 2);
    CHECK(header.uses->count == 2);
    CHECK(header.expose->children[0]->kind == MO_S_EXPOSE_ALL);
    CHECK(header.uses->children[0]->children[1]->count == 1);
    CHECK(header.uses->children[1]->children[1] == NULL);
    spans(&f, f.module->tree);
    free_fixture(&f);
    init(&f, "module M\n", false);
    struct mo_header unchanged = {.span = {123, 456, 789}};
    uint32_t bad_file;
    struct mo_source_error problem;
    MO_ASSERT(mo_sources_add_bytes(&f.sources, "bad.mo", (const uint8_t *)"not a header\n", 13,
                                   &bad_file, &problem));
    CHECK(!mo_read_header(&f.arena, &f.sources, bad_file, &unchanged, &f.diags));
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_SOURCE_HEADER);
    CHECK(unchanged.span.file == 123 && unchanged.span.start == 456 && unchanged.span.end == 789);
    free_fixture(&f);
    init(&f, "module M\nuse A\n\xff broken body\n", false);
    mo_diags_free(&f.diags);
    MO_ASSERT(mo_diags_init(&f.diags));
    CHECK(mo_read_header(&f.arena, &f.sources, 0, &header, &f.diags));
    CHECK(!mo_diags_any(&f.diags) && header.uses->count == 1);
    free_fixture(&f);
}

static void diagnostics(void) {
    const struct {
        const char *text;
        uint16_t code;
        bool fragment;
    } cases[] = {
        {"\"\xff\"\n", MO_CODE_SOURCE_UTF8, true},
        {"# invisible \xe2\x80\x8b\n", MO_CODE_SOURCE_INVISIBLE, true},
        {"camelCase = 1\n", MO_CODE_SOURCE_NAME, true},
        {"x = 1_\n", MO_CODE_SOURCE_NUMBER, true},
        {"x = \"\\q\"\n", MO_CODE_SOURCE_TEXT, true},
        {"x = 1;\n", MO_CODE_SOURCE_TOKEN, true},
        {"x = (1,)\n", MO_CODE_SOURCE_LITERAL, true},
        {"x = )\n", MO_CODE_SOURCE_SYNTAX, true},
        {"n = 1\n", MO_CODE_SOURCE_HEADER, false},
        {"x is f(a)\n", MO_CODE_SOURCE_PATTERN, true},
        {"42 = 1\n", MO_CODE_SOURCE_TARGET, true},
        {"Some()\n", MO_CODE_SOURCE_CONSTRUCTION, true},
        {"f(x: 1)\n", MO_CODE_SOURCE_ARGUMENTS, true},
        {"struct Empty\nend\n", MO_CODE_SOURCE_STRUCT, true},
        {"enum Empty\nend\n", MO_CODE_SOURCE_ENUM, true},
        {"fn f()\npromise \"p\"\ntrue\nallows result: true\nend\nend\n", MO_CODE_SOURCE_PROMISE,
         true},
        {"case x\n42\nend\n", MO_CODE_SOURCE_CASE, true},
        {"fn f(x Int)\nend\n", MO_CODE_SOURCE_FUNCTION, true},
        {"fn f()\nexamples\nend\nend\n", MO_CODE_SOURCE_EXAMPLES, true},
        {"struct R\nx: List(Int, )\nend\n", MO_CODE_SOURCE_TYPE, true},
        {"f(1 2)\n", MO_CODE_SOURCE_CALL, true},
        {"if x = 1\nend\n", MO_CODE_SOURCE_IF, true},
        {"for x xs\nend\n", MO_CODE_SOURCE_LOOP, true},
        {"x[1]\n", MO_CODE_SOURCE_INDEX, true},
        {"a && b\n", MO_CODE_SOURCE_BOOLEAN, true},
        {"x *= 2\n", MO_CODE_SOURCE_ASSIGNMENT, true},
        {"M::x\n", MO_CODE_SOURCE_QUALIFICATION, true},
        {"// comment\n", MO_CODE_SOURCE_COMMENT, true},
        {"x ** 2\n", MO_CODE_SOURCE_OPERATOR, true},
        {"test wrong\nend\n", MO_CODE_SOURCE_TEST, true},
        {"module M\nx 1\n", MO_CODE_SOURCE_BINDING, false},
        {"[1 2]\n", MO_CODE_SOURCE_LIST, true},
        {"{1 2}\n", MO_CODE_SOURCE_MAP, true},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        struct fixture f;
        init(&f, cases[i].text, cases[i].fragment);
        CHECK(f.module != NULL);
        if (f.diags.count != 1 || f.diags.items[0].code != cases[i].code)
            (void)fprintf(stderr, "diagnostic mismatch: %s (%u diagnostics)\n", cases[i].text,
                          f.diags.count);
        CHECK(f.diags.count == 1);
        if (f.diags.count > 0) {
            CHECK(f.diags.items[0].code == cases[i].code);
            CHECK(f.diags.items[0].message != NULL && f.diags.items[0].reason != NULL);
            CHECK(strcmp(f.diags.items[0].section, mo_code_find(cases[i].code)->section) == 0);
            CHECK(strstr(f.diags.items[0].reason, f.diags.items[0].section) != NULL);
            CHECK(f.diags.items[0].primary.span.end <= strlen(cases[i].text));
        }
        free_fixture(&f);
    }
}

static void recovery(void) {
    const char *mistakes[] = {"bad = (1 + )\n",  "bad = None()\n", "badName = 1\n",
                              "bad = \"\\q\"\n", "bad = (\n",      "bad = [\n",
                              "# \xff\n"};
    for (size_t i = 0; i < sizeof mistakes / sizeof mistakes[0]; i++) {
        struct mo_buffer source;
        mo_buffer_init(&source, 4096);
        MO_ASSERT(mo_buffer_append_text(&source, "module M\nfn f()\n  before = 1\n"));
        MO_ASSERT(mo_buffer_append_text(&source, mistakes[i]));
        MO_ASSERT(
            mo_buffer_append_text(&source, "  after = 2\n  return after\nend\nfn g()\n  3\nend\n"));
        struct fixture f;
        init_bytes(&f, source.bytes, source.length, false);
        CHECK(f.module != NULL && f.diags.count == 1);
        if (f.module != NULL) {
            CHECK(f.module->tree->children[1]->count == 2);
            CHECK(item(&f, 0)->incomplete);
            CHECK(item(&f, 0)->children[5]->count == 3);
            CHECK(item(&f, 1)->kind == MO_S_FN && !item(&f, 1)->incomplete);
        }
        free_fixture(&f);
        mo_buffer_free(&source);
    }
    struct fixture f;
    init(&f,
         "module M\nstruct R\na: Int\nbad Int\nc: Text\nend\n"
         "enum E\nA\nBad()\nC\nend\nfn f()\nexamples\n1 + )\ntrue\nend\n"
         "promise \"p\"\ntrue\nallows x: (\nforbids x: 0\nend\nreturn 2\nend\n",
         false);
    CHECK(f.module != NULL && f.diags.count == 4);
    if (f.module != NULL) {
        CHECK(f.module->tree->children[1]->count == 3);
        CHECK(item(&f, 0)->children[2]->count == 2 && item(&f, 0)->incomplete);
        CHECK(item(&f, 1)->children[2]->count == 2 && item(&f, 1)->incomplete);
        CHECK(item(&f, 2)->children[4]->count == 2 && item(&f, 2)->children[5]->count == 1);
    }
    free_fixture(&f);
    init(&f,
         "module M\nfn f(x Int)\nreturn 1\nend\n"
         "struct wrong\nx: Int\nend\n"
         "test wrong\nassert true\nend\n"
         "fn g()\npromise wrong\ntrue\nallows result: 1\nforbids result: 0\nend\n2\nend\n",
         false);
    CHECK(f.module != NULL && f.diags.count == 4);
    if (f.module != NULL) {
        CHECK(f.module->tree->children[1]->count == 4);
        CHECK(item(&f, 0)->incomplete && item(&f, 0)->children[5]->count == 1);
        CHECK(item(&f, 1)->incomplete && item(&f, 1)->children[2]->count == 1);
        CHECK(item(&f, 2)->incomplete && item(&f, 2)->children[1]->count == 1);
        CHECK(item(&f, 3)->incomplete && item(&f, 3)->children[5]->count == 1);
        spans(&f, f.module->tree);
    }
    free_fixture(&f);
    init(&f, "module M\nfn f()\n1\nfn g()\n2\nend\n", false);
    CHECK(f.module != NULL && f.diags.count == 1);
    if (f.module != NULL) {
        CHECK(f.module->tree->children[1]->count == 1);
        CHECK(memcmp(item(&f, 0)->children[0]->text, "g", 1) == 0);
    }
    free_fixture(&f);
}

static void control_flow(void) {
    struct fixture f;
    init(&f,
         "return if c: 1 else if d: 2 else: 3 if e\nreturn if c\nreturn 4 if d\n"
         "10 - 3 - 2\n10 / 3 % 2\n1 < 2\n2 <= 3\n3 > 2\n3 >= 2\n1 != 2\n"
         "x = 1\n  + 2\ny = 3\n-4\nz = f(\n  1, # first\n  2\n)\n",
         true);
    CHECK(f.module != NULL && !mo_diags_any(&f.diags));
    if (f.module != NULL && !mo_diags_any(&f.diags)) {
        spans(&f, f.module->tree);
        CHECK(item(&f, 0)->children[0]->kind == MO_S_LINE_IF &&
              item(&f, 0)->children[1]->kind == MO_S_NAME);
        CHECK(item(&f, 1)->children[0] == NULL && item(&f, 1)->children[1]->kind == MO_S_NAME);
        CHECK(item(&f, 3)->children[0]->kind == MO_S_BINARY_CHAIN);
        CHECK(item(&f, 3)->children[0]->token == MO_T_MINUS);
        CHECK(item(&f, 4)->children[0]->children[3]->token == MO_T_PERCENT);
        CHECK(item(&f, 12)->children[0]->kind == MO_S_NEGATE);
        const struct mo_syntax_comment *comma = item(&f, 13)->children[1]->trailing;
        CHECK(comma != NULL && comma->anchor == MO_COMMENT_TOKEN && comma->token == MO_T_COMMA &&
              comma->token_index == 0);
    }
    free_fixture(&f);
    const char *bad[] = {
        "1 < 2 < 3\n",        "1..2..3\n",       "true implies false implies true\n",
        "1 is [..rest, x]\n", "f(1) = 2\n",      "(a).field = 3\n",
        "Some(x, y)\n",       "Some(x: 1, 2)\n", "case x\nNone: while true\nend\nend\n"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        init(&f, bad[i], true);
        CHECK(f.module != NULL && mo_diags_any(&f.diags));
        free_fixture(&f);
    }
}

static MO_MUST_USE uint32_t comment_spans(const struct fixture *f, const struct mo_syntax *n) {
    if (n == NULL)
        return 0;
    uint32_t count = 0;
    const struct mo_syntax_comment *lists[] = {n->leading, n->trailing, n->ending};
    for (size_t i = 0; i < sizeof lists / sizeof lists[0]; i++) {
        for (const struct mo_syntax_comment *c = lists[i]; c != NULL; c = c->next) {
            CHECK(c->span.file == 0 && c->span.start < c->span.end &&
                  c->span.end <= f->sources.files[0].length);
            CHECK(memcmp(c->text, f->sources.files[0].bytes + c->span.start,
                         c->span.end - c->span.start) == 0);
            count++;
        }
    }
    for (uint32_t i = 0; i < n->count; i++)
        count += comment_spans(f, n->children[i]);
    return count;
}

static void inline_anchors(void) {
    struct fixture f;
    init(&f, "not # prefix\ntrue\nfn( # open\nx, # comma\ny) # parameters\nx\nend # close\n", true);
    CHECK(f.module != NULL && !mo_diags_any(&f.diags));
    if (f.module != NULL && !mo_diags_any(&f.diags)) {
        const struct mo_syntax *prefix = item(&f, 0)->children[0];
        const struct mo_syntax *lambda = item(&f, 1)->children[0];
        CHECK(prefix->trailing != NULL);
        if (prefix->trailing != NULL) {
            CHECK(prefix->trailing->token == MO_T_NOT);
            CHECK(prefix->trailing->token_index == 0);
        }
        const enum mo_syntax_token tokens[] = {MO_T_LPAREN, MO_T_COMMA, MO_T_RPAREN, MO_T_END};
        const struct mo_syntax_comment *c = lambda->trailing;
        for (size_t i = 0; i < sizeof tokens / sizeof tokens[0]; i++) {
            CHECK(c != NULL);
            if (c != NULL) {
                CHECK(c->anchor == MO_COMMENT_TOKEN && c->token == tokens[i] &&
                      c->token_index == 0);
                c = c->next;
            }
        }
        CHECK(c == NULL);
        CHECK(lambda->children[0]->trailing == NULL);
    }
    free_fixture(&f);
}

static const struct mo_syntax_comment *named_comment(const struct mo_syntax *n, const char *text,
                                                     const struct mo_syntax **owner_out) {
    if (n == NULL)
        return NULL;
    const struct mo_syntax_comment *lists[] = {n->leading, n->trailing, n->ending};
    for (size_t i = 0; i < sizeof lists / sizeof lists[0]; i++)
        for (const struct mo_syntax_comment *c = lists[i]; c != NULL; c = c->next)
            if (c->span.end - c->span.start == strlen(text) &&
                memcmp(c->text, text, strlen(text)) == 0) {
                *owner_out = n;
                return c;
            }
    for (uint32_t i = 0; i < n->count; i++) {
        const struct mo_syntax_comment *c = named_comment(n->children[i], text, owner_out);
        if (c != NULL)
            return c;
    }
    return NULL;
}

static void token_positions(void) {
    struct fixture f;
    init(&f,
         "module A. # dot first\nB. # dot second\nC\n"
         "fn f(a: Int, b: Bool) where # where\nA: Copy, B: Copy\n"
         "  examples # examples\n    true\n  end # examples close\n"
         "  text = \"#{Pair( # interpolated open\nx: 1)}\"\n"
         "  x = # assign\n    not # not\n    true\n"
         "  y = a + # binary\n    b\n"
         "  z = a + # chain first\n    b + # chain second\n    c\n"
         "  xs. # field dot\n    first. # chain dot\n    other( # call open\n    1, 2) # call "
         "close\n"
         "  ( # group open\n    1) # group close\n"
         "  [ # list open\n    1, 2] # list close\n"
         "  { # map open\n    1: 2} # map close\n"
         "  map_text = \"#{{ # interpolated map open\n1: 2}}\"\n"
         "  if true\n    1\n  else if false\n    2\n  else # else\n    3\n  end # conditional "
         "close\n"
         "end # function close\n",
         false);
    for (uint32_t i = 0; i < f.diags.count; i++) {
        struct mo_position p =
            mo_sources_position(&f.sources, 0, f.diags.items[i].primary.span.start);
        (void)fprintf(stderr, "token fixture %u:%u E%04u: %s\n", p.line, p.column,
                      f.diags.items[i].code, f.diags.items[i].message);
    }
    CHECK(f.module != NULL && !mo_diags_any(&f.diags));
    const struct {
        const char *comment;
        enum mo_syntax_kind owner;
        enum mo_syntax_token token;
        uint32_t index;
    } cases[] = {
        {"# dot first", MO_S_MODULE, MO_T_DOT, 0},
        {"# dot second", MO_S_MODULE, MO_T_DOT, 1},
        {"# interpolated open", MO_S_CALL, MO_T_LPAREN, 0},
        {"# where", MO_S_WHERE, MO_T_WHERE, 0},
        {"# examples", MO_S_EXAMPLES, MO_T_EXAMPLES, 0},
        {"# examples close", MO_S_EXAMPLES, MO_T_END, 0},
        {"# assign", MO_S_BIND, MO_T_ASSIGN, 0},
        {"# not", MO_S_NOT, MO_T_NOT, 0},
        {"# binary", MO_S_BINARY, MO_T_PLUS, 0},
        {"# chain first", MO_S_TOKEN, MO_T_PLUS, 0},
        {"# chain second", MO_S_TOKEN, MO_T_PLUS, 0},
        {"# field dot", MO_S_FIELD_ACCESS, MO_T_DOT, 0},
        {"# chain dot", MO_S_FIELD_ACCESS, MO_T_DOT, 0},
        {"# call open", MO_S_CALL, MO_T_LPAREN, 0},
        {"# call close", MO_S_CALL, MO_T_RPAREN, 0},
        {"# group open", MO_S_GROUP, MO_T_LPAREN, 0},
        {"# group close", MO_S_GROUP, MO_T_RPAREN, 0},
        {"# list open", MO_S_LIST, MO_T_LBRACK, 0},
        {"# list close", MO_S_LIST, MO_T_RBRACK, 0},
        {"# map open", MO_S_MAP, MO_T_LBRACE, 0},
        {"# interpolated map open", MO_S_MAP, MO_T_LBRACE, 0},
        {"# map close", MO_S_MAP, MO_T_RBRACE, 0},
        {"# else", MO_S_ELSE, MO_T_ELSE, 0},
        {"# conditional close", MO_S_IF, MO_T_END, 0},
        {"# function close", MO_S_FN, MO_T_END, 0},
    };
    if (f.module != NULL && !mo_diags_any(&f.diags)) {
        CHECK(comment_spans(&f, f.module->tree) == sizeof cases / sizeof cases[0]);
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            const struct mo_syntax *owner_node = NULL;
            const struct mo_syntax_comment *c =
                named_comment(f.module->tree, cases[i].comment, &owner_node);
            CHECK(c != NULL);
            if (c != NULL)
                CHECK(owner_node->kind == cases[i].owner && c->anchor == MO_COMMENT_TOKEN &&
                      c->token == cases[i].token && c->token_index == cases[i].index);
        }
    }
    free_fixture(&f);
}

static bool same_comments(const struct mo_syntax_comment *a, const struct mo_syntax_comment *b) {
    while (a != NULL && b != NULL) {
        uint32_t size = a->span.end - a->span.start;
        if (size != b->span.end - b->span.start || a->documentation != b->documentation ||
            a->anchor != b->anchor || a->token != b->token || a->token_index != b->token_index ||
            memcmp(a->text, b->text, size) != 0)
            return false;
        a = a->next;
        b = b->next;
    }
    return a == NULL && b == NULL;
}

static bool same_tree(const struct mo_syntax *a, const struct mo_syntax *b) {
    if (a == NULL || b == NULL)
        return a == b;
    if (a->kind != b->kind || a->count != b->count || a->token != b->token ||
        a->blank_before != b->blank_before || (a->text == NULL) != (b->text == NULL) ||
        !same_comments(a->leading, b->leading) || !same_comments(a->trailing, b->trailing) ||
        !same_comments(a->ending, b->ending))
        return false;
    if (a->text != NULL && (a->span.end - a->span.start != b->span.end - b->span.start ||
                            memcmp(a->text, b->text, a->span.end - a->span.start) != 0))
        return false;
    for (uint32_t i = 0; i < a->count; i++)
        if (!same_tree(a->children[i], b->children[i]))
            return false;
    return true;
}

static void optional_separators(void) {
    const struct {
        const char *with, *without;
    } cases[] = {
        {"[1, # final\n]\n", "[1 # final\n]\n"},
        {"f(1, # final\n)\n", "f(1 # final\n)\n"},
        {"Pair(a: 1, b: 2, # final\n)\n", "Pair(a: 1, b: 2 # final\n)\n"},
        {"(1, 2, # final\n)\n", "(1, 2 # final\n)\n"},
        {"{1: 2, # final\n}\n", "{1: 2 # final\n}\n"},
        {"fn f(x: Int, # final\n)\nend\n", "fn f(x: Int # final\n)\nend\n"},
        {"enum E\nCase(x: Int, # final\n)\nend\n", "enum E\nCase(x: Int # final\n)\nend\n"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        struct fixture a, b;
        init(&a, cases[i].with, true);
        init(&b, cases[i].without, true);
        CHECK(a.module != NULL && b.module != NULL && !mo_diags_any(&a.diags) &&
              !mo_diags_any(&b.diags));
        if (a.module != NULL && b.module != NULL && !mo_diags_any(&a.diags) &&
            !mo_diags_any(&b.diags)) {
            CHECK(same_tree(a.module->tree, b.module->tree));
            const struct mo_syntax *owner_node;
            const struct mo_syntax_comment *c =
                named_comment(a.module->tree, "# final", &owner_node);
            CHECK(c != NULL && c->anchor == MO_COMMENT_ITEM);
        }
        free_fixture(&a);
        free_fixture(&b);
    }
}

static void blank_lines(void) {
    struct fixture f;
    init(&f,
         "\nmodule M\r\n\r\nfn f()\r\n\r\n  x = 1\r\n  # consecutive\r\n"
         "  y = 2\r\n \t\r\n\r\n  # paragraph\r\n  z = 3\r\n\r\nend\r\n"
         "fn g()\nend\n",
         false);
    CHECK(f.module != NULL && !mo_diags_any(&f.diags));
    if (f.module != NULL && !mo_diags_any(&f.diags)) {
        const struct mo_syntax *fn = item(&f, 0), *body = fn->children[5];
        CHECK(fn->blank_before && !item(&f, 1)->blank_before);
        CHECK(body->count == 3 && !body->children[0]->blank_before &&
              !body->children[1]->blank_before && body->children[2]->blank_before);
    }
    free_fixture(&f);
}

static void comments(void) {
    struct fixture f;
    init(&f,
         "## module docs\nmodule M\n# function docs\nfn f()\n  # first\n  n = 1 # one\n  # "
         "last\nend # close\n# eof\n",
         false);
    CHECK(f.module != NULL && !mo_diags_any(&f.diags));
    struct mo_syntax *fn = item(&f, 0), *binding = fn->children[5]->children[0];
    CHECK(f.module->tree->children[0]->leading != NULL &&
          f.module->tree->children[0]->leading->documentation);
    CHECK(fn->leading != NULL && binding->leading != NULL);
    CHECK(binding->children[1]->trailing != NULL);
    CHECK(fn->ending != NULL && fn->trailing != NULL && f.module->tree->ending != NULL);
    CHECK(comment_spans(&f, f.module->tree) == 7);
    mo_sources_free(&f.sources);
    CHECK(memcmp(fn->children[0]->text, "f", 1) == 0);
    CHECK(memcmp(binding->children[1]->trailing->text, "# one", 5) == 0);
    free_fixture(&f);
    init(&f,
         "module M\n# one\n# two\nfn f()\nexamples # header\ntrue\nend\n"
         " if true\n  # nested\n  values = [\n    # item\n    1, # first\n    2\n  ] # close\n  # "
         "body end\n"
         " else # alternative\n  3\n end # conditional end\n"
         " case x\n  A:\n   4\n   # arm end\n  B: 5\n end\n # function end\nend\n",
         false);
    CHECK(f.module != NULL && !mo_diags_any(&f.diags));
    if (f.module != NULL && !mo_diags_any(&f.diags)) {
        spans(&f, f.module->tree);
        CHECK(comment_spans(&f, f.module->tree) == 12);
        fn = item(&f, 0);
        CHECK(fn->leading != NULL && fn->leading->next != NULL &&
              fn->leading->span.start < fn->leading->next->span.start);
        CHECK(fn->children[4]->children[0]->trailing != NULL);
        struct mo_syntax *conditional = fn->children[5]->children[0]->children[0];
        CHECK(conditional->ending != NULL);
        CHECK(conditional->children[2]->children[0]->trailing != NULL &&
              memcmp(conditional->children[2]->children[0]->trailing->text, "# alternative", 13) ==
                  0);
        struct mo_syntax *arms = fn->children[5]->children[1]->children[0]->children[1];
        CHECK(arms->children[0]->ending != NULL);
        CHECK(fn->ending != NULL);
    }
    free_fixture(&f);
}

static void boundary(const struct mo_buffer *source, uint16_t code, bool fragment) {
    struct fixture f;
    init_bytes(&f, source->bytes, source->length, fragment);
    if ((code == 0 && (f.module == NULL || mo_diags_any(&f.diags))) ||
        (code != 0 && (f.diags.count != 1 || f.diags.items[0].code != code))) {
        (void)fprintf(stderr, "boundary expected %u: %u diagnostics\n", code, f.diags.count);
        for (uint32_t i = 0; i < f.diags.count; i++)
            (void)fprintf(stderr, "  %u: %s\n", f.diags.items[i].code, f.diags.items[i].message);
    }
    if (code == 0)
        CHECK(f.module != NULL && !mo_diags_any(&f.diags));
    else {
        CHECK(f.diags.count == 1);
        if (f.diags.count > 0)
            CHECK(f.diags.items[0].code == code);
    }
    free_fixture(&f);
}

static void repeat(struct mo_buffer *source, const char *text, uint32_t count) {
    for (uint32_t i = 0; i < count; i++)
        MO_ASSERT(mo_buffer_append_text(source, text));
}

static void limits(void) {
    struct mo_buffer source;
    mo_buffer_init(&source, MO_LIMIT_FILE_SIZE);
    for (uint32_t extra = 0; extra < 2; extra++) {
        MO_ASSERT(mo_buffer_append_byte(&source, '#'));
        repeat(&source, "é", MO_LIMIT_LINE_LENGTH - 1u + extra);
        MO_ASSERT(mo_buffer_append_text(&source, "\r\n"));
        boundary(&source, extra == 0 ? 0 : MO_CODE_LINE_TOO_LONG, true);
        mo_buffer_clear(&source);
        repeat(&source, "[", MO_LIMIT_NESTING_DEPTH + extra);
        MO_ASSERT(mo_buffer_append_byte(&source, '0'));
        repeat(&source, "]", MO_LIMIT_NESTING_DEPTH + extra);
        boundary(&source, extra == 0 ? 0 : MO_CODE_NESTED_TOO_DEEPLY, true);
        mo_buffer_clear(&source);
        repeat(&source, "1 + ", MO_LIMIT_NESTING_DEPTH + extra);
        MO_ASSERT(mo_buffer_append_text(&source, "1\n"));
        boundary(&source, 0, true);
        mo_buffer_clear(&source);
        repeat(&source, "\"#{", MO_LIMIT_INTERPOLATION_DEPTH + extra);
        MO_ASSERT(mo_buffer_append_byte(&source, '0'));
        repeat(&source, "}\"", MO_LIMIT_INTERPOLATION_DEPTH + extra);
        boundary(&source, extra == 0 ? 0 : MO_CODE_INTERPOLATIONS_TOO_DEEP, true);
        mo_buffer_clear(&source);
        MO_ASSERT(mo_buffer_append_text(&source, "module M\nfn f("));
        repeat(&source, "p: Int,\n", MO_LIMIT_PARAMETERS + extra);
        MO_ASSERT(mo_buffer_append_text(&source, ")\nend\n"));
        boundary(&source, extra == 0 ? 0 : MO_CODE_TOO_MANY_PARAMETERS, false);
        mo_buffer_clear(&source);
        MO_ASSERT(mo_buffer_append_text(&source, "f("));
        repeat(&source, "1,\n", MO_LIMIT_PARAMETERS + extra);
        MO_ASSERT(mo_buffer_append_text(&source, ")\n"));
        boundary(&source, extra == 0 ? 0 : MO_CODE_TOO_MANY_PARAMETERS, true);
        mo_buffer_clear(&source);
        MO_ASSERT(mo_buffer_append_text(&source, "fn("));
        repeat(&source, "p,\n", MO_LIMIT_PARAMETERS - 1u + extra);
        MO_ASSERT(mo_buffer_append_text(&source, "p) 0 end\n"));
        boundary(&source, extra == 0 ? 0 : MO_CODE_TOO_MANY_PARAMETERS, true);
        mo_buffer_clear(&source);
        MO_ASSERT(mo_buffer_append_text(&source, "struct R\nf: fn("));
        repeat(&source, "Int,\n", MO_LIMIT_PARAMETERS - 1u + extra);
        MO_ASSERT(mo_buffer_append_text(&source, "Int)\nend\n"));
        boundary(&source, extra == 0 ? 0 : MO_CODE_TOO_MANY_PARAMETERS, true);
        mo_buffer_clear(&source);
        MO_ASSERT(mo_buffer_append_text(&source, "R("));
        repeat(&source, "f: 1,\n", MO_LIMIT_FIELDS + extra);
        MO_ASSERT(mo_buffer_append_text(&source, ")\n"));
        boundary(&source, extra == 0 ? 0 : MO_CODE_TOO_MANY_FIELDS, true);
        mo_buffer_clear(&source);
        MO_ASSERT(mo_buffer_append_text(&source, "struct R\nf: ("));
        repeat(&source, "Int,\n", MO_LIMIT_FIELDS - 1u + extra);
        MO_ASSERT(mo_buffer_append_text(&source, "Int)\nend\n"));
        boundary(&source, extra == 0 ? 0 : MO_CODE_TOO_MANY_FIELDS, true);
        mo_buffer_clear(&source);
        MO_ASSERT(mo_buffer_append_text(&source, "for ("));
        repeat(&source, "a,\n", MO_LIMIT_FIELDS - 1u + extra);
        MO_ASSERT(mo_buffer_append_text(&source, "a) in []\nend\n"));
        boundary(&source, extra == 0 ? 0 : MO_CODE_TOO_MANY_FIELDS, true);
        mo_buffer_clear(&source);
        MO_ASSERT(mo_buffer_append_byte(&source, '('));
        repeat(&source, "1,\n", MO_LIMIT_FIELDS + extra);
        MO_ASSERT(mo_buffer_append_text(&source, ")\n"));
        boundary(&source, extra == 0 ? 0 : MO_CODE_TOO_MANY_FIELDS, true);
        mo_buffer_clear(&source);
        MO_ASSERT(mo_buffer_append_text(&source, "struct R\n"));
        repeat(&source, "f: Int\n", MO_LIMIT_FIELDS + extra);
        MO_ASSERT(mo_buffer_append_text(&source, "end\n"));
        boundary(&source, extra == 0 ? 0 : MO_CODE_TOO_MANY_FIELDS, true);
        mo_buffer_clear(&source);
        MO_ASSERT(mo_buffer_append_text(&source, "enum E\n"));
        repeat(&source, "C\n", MO_LIMIT_CHOICE_CASES + extra);
        MO_ASSERT(mo_buffer_append_text(&source, "end\n"));
        boundary(&source, extra == 0 ? 0 : MO_CODE_TOO_MANY_CASES, true);
        mo_buffer_clear(&source);
        MO_ASSERT(mo_buffer_append_text(&source, "module M\n"));
        repeat(&source, "n = 1\n", MO_LIMIT_MODULE_NAMES + extra);
        boundary(&source, extra == 0 ? 0 : MO_CODE_TOO_MANY_NAMES, false);
        mo_buffer_clear(&source);
    }
    mo_buffer_free(&source);
    struct fixture f;
    init(&f, "module M\n", false);
    mo_arena_free(&f.arena);
    mo_arena_init(&f.arena, 0);
    CHECK(mo_parse(&f.arena, &f.sources, 0, &f.diags) == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_SYNTAX_NO_MEMORY);
    free_fixture(&f);
    init(&f, "module M\n", false);
    mo_diags_free(&f.diags);
    MO_ASSERT(mo_diags_init(&f.diags));
    struct mo_diag diagnostic = {.code = MO_CODE_SOURCE_SYNTAX,
                                 .primary = {{0, 0, 0}, NULL},
                                 .message = "a value is an expression",
                                 .reason = "A value is an expression (reference 2.4)."};
    for (uint32_t i = 0; i < MO_LIMIT_DIAGNOSTICS; i++)
        MO_ASSERT(mo_diags_add(&f.diags, &diagnostic));
    CHECK(mo_read_header(&f.arena, &f.sources, 0, &f.module->header, &f.diags));
    uint32_t bad_file;
    struct mo_source_error problem;
    MO_ASSERT(mo_sources_add_bytes(&f.sources, "bad.mo", (const uint8_t *)"module M\nn = )\n", 15,
                                   &bad_file, &problem));
    CHECK(mo_parse(&f.arena, &f.sources, bad_file, &f.diags) == NULL && f.diags.dropped == 1);
    free_fixture(&f);
}

static void review_recovery(void) {
    const struct {
        const char *source;
        uint32_t errors;
        uint32_t items;
        bool fragment;
    } cases[] = {
        {"valid and 0 < x < 10\n", 1, 0, true},
        {"not a == b == c\n", 1, 0, true},
        {"a or b .. c .. d\n", 1, 0, true},
        {"ready and x is Some(y) == z\n", 1, 0, true},
        {"f = g(x)[x]\n", 1, 0, true},
        {"module M\nfn f()\nx = \"a #{ g(1\nend\nfn h()\n2\nend\nfn k()\n3\nend\n", 1, 3, false},
        {"module M\nfn f()\nif x = 1\n2\nend\n3\nend\nfn g()\n4\nend\n", 1, 2, false},
        {"module M\nfn f()\nfor x xs\n2\nend\n3\nend\nfn g()\n4\nend\n", 1, 2, false},
        {"module M\nfn f()\ncase x:\nA: 2\nend\n3\nend\nfn g()\n4\nend\n", 1, 2, false},
        {"module M\nfn f()\nif a && b\n2\nend\n3\nend\nfn g()\n4\nend\n", 1, 2, false},
        {"module M\nfn f()\nif a\n1\nelse if y = 2\n3\nelse\n4\nend\n5\nend\nfn g()\n6\nend\n", 1,
         2, false},
        {"module M\nfn f()\nx = [\n1,\n2 3\n]\nend\nfn g()\n4\nend\n", 1, 2, false},
        {"module M\nfn f()\n1\nfn g()\n2\nend\n", 1, 1, false},
        {"module M\nfn f()\nx = \"\"\"\n  \\q\n  this is text\n  \"\"\"\nreturn 2\nend\n", 1, 1,
         false},
        {"fn f()\n1\nend\nfn g()\n2\nend\n", 1, 2, false},
        {"while x is f(y)\nbad = )\nend\n", 2, 1, true},
        {"for p in f(n: 2)\nbad = )\nend\n", 2, 1, true},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        struct fixture f;
        init(&f, cases[i].source, cases[i].fragment);
        if (f.module == NULL || f.diags.count != cases[i].errors ||
            f.module->tree->children[1]->count != cases[i].items) {
            (void)fprintf(stderr, "review recovery: %s", cases[i].source);
            for (uint32_t j = 0; j < f.diags.count; j++)
                (void)fprintf(stderr, "  %u at %u: %s\n", f.diags.items[j].code,
                              f.diags.items[j].primary.span.start, f.diags.items[j].message);
        }
        CHECK(f.module != NULL && f.diags.count == cases[i].errors);
        if (f.module != NULL) {
            CHECK(f.module->tree->children[1]->count == cases[i].items);
            CHECK(f.module->tree->incomplete);
            spans(&f, f.module->tree);
        }
        free_fixture(&f);
    }
    struct fixture f;
    const char *arm_errors[] = {
        "case value\nOk(value): value\nError(problem: ):\ndescribe(problem)\nend\n",
        "case value\nOk(value): value\nError(\n# header\nproblem:\n):\ndescribe(problem)\nend\n",
        "case value\nOk(value): value\nError(problem)\tif y = 2:\ndescribe(problem)\nend\n",
    };
    for (size_t i = 0; i < sizeof arm_errors / sizeof arm_errors[0]; i++) {
        init(&f, arm_errors[i], true);
        CHECK(f.module != NULL && f.diags.count == 1);
        if (f.module != NULL) {
            struct mo_syntax *arms = item(&f, 0)->children[0]->children[1];
            CHECK(arms->count == 2 && arms->incomplete);
            CHECK(arms->children[0]->children[2]->count == 1);
            CHECK(arms->children[1]->incomplete && arms->children[1]->children[2]->count == 1);
            spans(&f, f.module->tree);
        }
        free_fixture(&f);
    }
    const char *body_errors[] = {
        "case x\nSome(v):\nbad = )\nv\nend\n",
        "case x\nSome(v):\nbad(\":\", ,)\nv\nend\n",
        "case x\nSome(v):\nbad({1:})\nv\nend\n",
        "case x\nSome(v):\nbad(n:)\nv\nend\n",
        "case x\nSome(v):\nbad = if true: ) else: 2\nv\nend\n",
    };
    for (size_t i = 0; i < sizeof body_errors / sizeof body_errors[0]; i++) {
        init(&f, body_errors[i], true);
        CHECK(f.module != NULL && f.diags.count == 1);
        if (f.module != NULL) {
            struct mo_syntax *arms = item(&f, 0)->children[0]->children[1];
            CHECK(arms->count == 1 && arms->incomplete);
            if (arms->count == 1) {
                CHECK(arms->children[0]->incomplete);
                struct mo_syntax *body = arms->children[0]->children[2];
                CHECK(body->count == 1);
                if (body->count == 1) {
                    struct mo_syntax *value = body->children[0]->children[0];
                    CHECK(value->kind == MO_S_NAME && value->span.end - value->span.start == 1 &&
                          value->text[0] == 'v');
                }
            }
        }
        free_fixture(&f);
    }
    init(&f, "case x\nA:\nif y\nbad = )\nend\nend\n", true);
    CHECK(f.module != NULL && f.diags.count == 1);
    if (f.module != NULL)
        CHECK(item(&f, 0)->children[0]->children[1]->children[0]->incomplete);
    free_fixture(&f);
    init(&f, "x = [\n1,\n# last\n]\nif y\nz = 1\nend\nf(\n1,\n# call end\n)\n", true);
    CHECK(f.module != NULL && !mo_diags_any(&f.diags));
    if (f.module != NULL) {
        CHECK(item(&f, 0)->children[1]->ending != NULL);
        CHECK(item(&f, 2)->children[0]->ending != NULL);
        CHECK(item(&f, 1)->children[0]->children[1]->children[0]->leading == NULL);
        CHECK(comment_spans(&f, f.module->tree) == 2);
    }
    free_fixture(&f);
    init(&f, "x = (f(\n1,\n# inner\n))(\n2,\n# outer\n).trim\n", true);
    CHECK(f.module != NULL && !mo_diags_any(&f.diags));
    if (f.module != NULL) {
        struct mo_syntax *chain = item(&f, 0)->children[1];
        CHECK(chain->kind == MO_S_POSTFIX);
        CHECK(chain->children[0]->children[0]->ending != NULL);
        CHECK(chain->children[1]->ending != NULL);
        CHECK(comment_spans(&f, f.module->tree) == 2);
        spans(&f, f.module->tree);
    }
    free_fixture(&f);
}

static void arena_sweeps(void) {
    const char *cases[] = {
        "module M\nuse A\nn = 1\n",
        "module M\nfn f()\nx = \"\"\"\n  \\q\n  \"\"\"\n2\nend\n",
        "module M\n# comment\nfn f()\nif true\n1\nend\nend\n",
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        struct fixture f;
        init(&f, cases[i], false);
        for (size_t size = 0; size < 8192; size += 8) {
            for (unsigned header = 0; header < 2; header++) {
                struct mo_diags diags;
                MO_ASSERT(mo_diags_init(&diags));
                struct mo_arena arena;
                mo_arena_init(&arena, size);
                struct mo_header out = {.span = {123, 456, 789}};
                bool success;
                if (header != 0) {
                    success = mo_read_header(&arena, &f.sources, 0, &out, &diags);
                    if (!success)
                        CHECK(out.span.file == 123 && out.span.start == 456 && out.span.end == 789);
                } else
                    success = mo_parse(&arena, &f.sources, 0, &diags) != NULL;
                if (!success) {
                    bool allocation = false;
                    for (uint32_t j = 0; j < diags.count; j++)
                        allocation = allocation || diags.items[j].code == MO_CODE_SYNTAX_NO_MEMORY;
                    CHECK(allocation && !diags.dropped);
                }
                mo_arena_free(&arena);
                mo_diags_free(&diags);
            }
        }
        free_fixture(&f);
    }
}

static void nesting_and_chains(void) {
    struct mo_buffer source;
    mo_buffer_init(&source, MO_LIMIT_FILE_SIZE);
    for (uint32_t extra = 0; extra < 2; extra++) {
        MO_ASSERT(mo_buffer_append_text(&source, "module M\nfn f()\nx = "));
        repeat(&source, "[", MO_LIMIT_NESTING_DEPTH - 1u + extra);
        MO_ASSERT(mo_buffer_append_text(&source, "0"));
        repeat(&source, "]", MO_LIMIT_NESTING_DEPTH - 1u + extra);
        MO_ASSERT(mo_buffer_append_text(&source, "\nend\n"));
        boundary(&source, extra == 0 ? 0 : MO_CODE_NESTED_TOO_DEEPLY, false);
        mo_buffer_clear(&source);
        MO_ASSERT(mo_buffer_append_text(&source, "module M\nfn f()\n"));
        repeat(&source, "if true\n", MO_LIMIT_NESTING_DEPTH - 1u + extra);
        MO_ASSERT(mo_buffer_append_text(&source, "0\n"));
        repeat(&source, "end\n", MO_LIMIT_NESTING_DEPTH + extra);
        boundary(&source, extra == 0 ? 0 : MO_CODE_NESTED_TOO_DEEPLY, false);
        mo_buffer_clear(&source);
        MO_ASSERT(mo_buffer_append_text(&source, "struct R\nx: "));
        repeat(&source, "List(", MO_LIMIT_NESTING_DEPTH - 1u + extra);
        MO_ASSERT(mo_buffer_append_text(&source, "Int"));
        repeat(&source, ")", MO_LIMIT_NESTING_DEPTH - 1u + extra);
        MO_ASSERT(mo_buffer_append_text(&source, "\nend\n"));
        boundary(&source, extra == 0 ? 0 : MO_CODE_NESTED_TOO_DEEPLY, true);
        mo_buffer_clear(&source);
        MO_ASSERT(mo_buffer_append_text(&source, "struct R\nx: "));
        repeat(&source, "fn(): ", MO_LIMIT_NESTING_DEPTH - 1u + extra);
        MO_ASSERT(mo_buffer_append_text(&source, "Int\nend\n"));
        boundary(&source, extra == 0 ? 0 : MO_CODE_NESTED_TOO_DEEPLY, true);
        mo_buffer_clear(&source);
        MO_ASSERT(mo_buffer_append_text(&source, "enum E\nC("));
        repeat(&source, "x: Int,\n", MO_LIMIT_FIELDS + extra);
        MO_ASSERT(mo_buffer_append_text(&source, ")\nend\n"));
        boundary(&source, extra == 0 ? 0 : MO_CODE_TOO_MANY_FIELDS, true);
        mo_buffer_clear(&source);
    }
    MO_ASSERT(mo_buffer_append_text(&source, "module M\n"));
    repeat(&source, "n = 1\n", MO_LIMIT_MODULE_NAMES + 2u);
    boundary(&source, MO_CODE_TOO_MANY_NAMES, false);
    mo_buffer_clear(&source);
    MO_ASSERT(mo_buffer_append_text(&source, "module M\n"));
    repeat(&source, "n = 1\n", MO_LIMIT_MODULE_NAMES + 1000u);
    boundary(&source, MO_CODE_TOO_MANY_NAMES, false);
    mo_buffer_clear(&source);
    const char *prefix = "module M\nfn f()\nx = ";
    MO_ASSERT(mo_buffer_append_text(&source, prefix));
    repeat(&source, "[", MO_LIMIT_NESTING_DEPTH);
    MO_ASSERT(mo_buffer_append_text(&source, "0"));
    repeat(&source, "]", MO_LIMIT_NESTING_DEPTH);
    MO_ASSERT(mo_buffer_append_text(&source, "\nend\n"));
    struct fixture f;
    init_bytes(&f, source.bytes, source.length, false);
    CHECK(f.module != NULL && f.diags.count == 1);
    if (f.diags.count != 0)
        CHECK(f.diags.items[0].primary.span.start == strlen(prefix) + MO_LIMIT_NESTING_DEPTH - 1u);
    free_fixture(&f);
    mo_buffer_clear(&source);
    MO_ASSERT(mo_buffer_append_text(&source, "x = "));
    repeat(&source, "1 + ", 300);
    MO_ASSERT(mo_buffer_append_text(&source, "2\ny = text"));
    repeat(&source, ".trim", 300);
    MO_ASSERT(mo_buffer_append_text(&source, "\nif true\n0\n"));
    repeat(&source, "else if false\n1\n", 300);
    MO_ASSERT(mo_buffer_append_text(&source, "end\n"));
    boundary(&source, 0, true);
    mo_buffer_clear(&source);
    repeat(&source, "fn()\n", MO_LIMIT_NESTING_DEPTH + 1u);
    MO_ASSERT(mo_buffer_append_text(&source, "0\n"));
    repeat(&source, "end\n", MO_LIMIT_NESTING_DEPTH + 1u);
    boundary(&source, MO_CODE_NESTED_TOO_DEEPLY, true);
    mo_buffer_clear(&source);
    for (uint32_t extra = 0; extra < 2; extra++) {
        MO_ASSERT(mo_buffer_append_text(&source, "module M\nfn f(x: "));
        repeat(&source, "List(", MO_LIMIT_NESTING_DEPTH - 2u + extra);
        MO_ASSERT(mo_buffer_append_text(&source, "Int"));
        repeat(&source, ")", MO_LIMIT_NESTING_DEPTH - 2u + extra);
        MO_ASSERT(mo_buffer_append_text(&source, ")\nend\n"));
        boundary(&source, extra == 0 ? 0 : MO_CODE_NESTED_TOO_DEEPLY, false);
        mo_buffer_clear(&source);
        MO_ASSERT(mo_buffer_append_text(&source, "enum E\nC("));
        repeat(&source, "List(", MO_LIMIT_NESTING_DEPTH - 2u + extra);
        MO_ASSERT(mo_buffer_append_text(&source, "Int"));
        repeat(&source, ")", MO_LIMIT_NESTING_DEPTH - 2u + extra);
        MO_ASSERT(mo_buffer_append_text(&source, ")\nend\n"));
        boundary(&source, extra == 0 ? 0 : MO_CODE_NESTED_TOO_DEEPLY, true);
        mo_buffer_clear(&source);
        MO_ASSERT(mo_buffer_append_text(&source, "module M\nfn f()\nexamples\n"));
        repeat(&source, "fn()\n", MO_LIMIT_NESTING_DEPTH - 2u + extra);
        MO_ASSERT(mo_buffer_append_text(&source, "0\n"));
        repeat(&source, "end\n", MO_LIMIT_NESTING_DEPTH + extra);
        boundary(&source, extra == 0 ? 0 : MO_CODE_NESTED_TOO_DEEPLY, false);
        mo_buffer_clear(&source);
        MO_ASSERT(mo_buffer_append_text(&source, "x = "));
        repeat(&source, "if true: 0 else: ", MO_LIMIT_NESTING_DEPTH + extra);
        MO_ASSERT(mo_buffer_append_text(&source, "0\n"));
        boundary(&source, extra == 0 ? 0 : MO_CODE_NESTED_TOO_DEEPLY, true);
        mo_buffer_clear(&source);
    }
    mo_buffer_free(&source);
}

static void performance(void) {
    struct mo_buffer source;
    mo_buffer_init(&source, MO_LIMIT_FILE_SIZE);
    for (unsigned test = 0; test < 3; test++) {
        if (test == 0) {
            MO_ASSERT(mo_buffer_append_text(&source, ")\n"));
            repeat(&source, "\t\"\"\"\n", 400000);
        } else if (test == 1) {
            MO_ASSERT(mo_buffer_append_text(&source, "module M\nfn f()\n"));
            repeat(&source, "if true\n", 250);
            repeat(&source, "a = 0\n", 1300000);
            repeat(&source, "end\n", 251);
        } else {
            MO_ASSERT(mo_buffer_append_text(&source, "module M\n"));
            repeat(&source, "#\n", 1000000);
            repeat(&source, "x = [\n0 1\n]\n", 500);
        }
        clock_t start = clock();
        MO_ASSERT(start != (clock_t)-1);
        struct fixture f;
        init_bytes(&f, source.bytes, source.length, test == 0);
        clock_t end = clock();
        MO_ASSERT(end != (clock_t)-1);
        double seconds = (double)(end - start) / (double)CLOCKS_PER_SEC;
        CHECK(f.module != NULL);
        CHECK(test == 1 ? !mo_diags_any(&f.diags) : mo_diags_any(&f.diags));
        CHECK(seconds < 20);
        if (printf("syntax performance %u: %zu bytes, %.3f CPU seconds\n", test, source.length,
                   seconds) < 0)
            mo_test_failures++;
        free_fixture(&f);
        mo_buffer_clear(&source);
    }
    mo_buffer_free(&source);
}

static void lambda_lists(void) {
    const char *good[] = {
        "f = fn(x) [x, x] end\n",
        "f = fn() [] end\n",
        "xs.map(fn(x) [x, x] end)\n",
        "f = fn(\n x,\n y\n) [x, y] end\n",
        "f = fn(x) [fn(y) [x, y] end, x] end\n",
        "f = fn(x) [g(x), (x)] end\n",
        "f = fn(x)\n# a multi-line body\n[x]\nend\n",
        "f = fn(x) \"#{[x].map(fn(y) [y] end)}\" end\n",
        "fn visit(f: fn(List(Int)) : List(Int))\nend\n",
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        struct fixture f;
        init(&f, good[i], true);
        CHECK(f.module != NULL && !mo_diags_any(&f.diags));
        if (f.module != NULL && !mo_diags_any(&f.diags))
            spans(&f, f.module->tree);
        if (i == 0 && f.module != NULL && !mo_diags_any(&f.diags)) {
            const struct mo_syntax *lambda = item(&f, 0)->children[1];
            CHECK(lambda->kind == MO_S_LAMBDA);
            CHECK(lambda->children[0]->count == 1);
            CHECK(lambda->children[1]->kind == MO_S_LIST && lambda->children[1]->count == 2);
        }
        free_fixture(&f);
    }
    const char *bad[] = {"g(x)[x]\n",
                         "(x)[x]\n",
                         "(fn(x) [x] end)[0]\n",
                         "f = fn(x) g(x)[x] end\n",
                         "f = fn(x) [(x)[0]] end\n",
                         "f = fn(x) [g(x)[0]] end\n"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        struct fixture f;
        init(&f, bad[i], true);
        CHECK(f.module != NULL && f.diags.count == 1);
        if (f.diags.count == 1)
            CHECK(f.diags.items[0].code == MO_CODE_SOURCE_INDEX);
        free_fixture(&f);
    }
}

int main(void) {
    shapes();
    declarations_and_statements();
    headers();
    diagnostics();
    recovery();
    control_flow();
    comments();
    inline_anchors();
    token_positions();
    optional_separators();
    blank_lines();
    limits();
    review_recovery();
    arena_sweeps();
    nesting_and_chains();
    lambda_lists();
    performance();
    return mo_test_report("test_syntax");
}
