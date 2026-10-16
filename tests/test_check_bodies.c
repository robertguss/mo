#include "base/buffer.h"
#include "base/codes.h"
#include "base/limits.h"
#include "base/stack.h"
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

static void record_fields(void) {
    struct fixture f;
    init(&f,
         "module Main\nstruct Point\nx: Decimal\ny: Decimal\nend\n"
         "fn run() : Decimal\npoint = Point(y: 2, x: 1)\npoint.x + point.y\nend\n",
         NULL);
    struct mo_inferred_body *out = body(&f, "run");
    CHECK(out != NULL && f.diags.count == 0);
    if (out != NULL)
        CHECK(strcmp(out->items[out->count - 1].type->name, "Decimal") == 0);
    finish(&f);

    init(&f,
         "module Main\nstruct Box(T)\ntext: T\nindex: Int\nend\n"
         "fn run() : Text\nbox = Box(index: 2, text: \"a\")\nbox.text\nend\n"
         "enum Tree(T)\nLeaf\nNode(left: Tree(T), value: T, right: Tree(T))\nend\n"
         "fn tree() : Tree(Int)\nNode(right: Leaf, value: 1, left: Leaf)\nend\n",
         NULL);
    out = body(&f, "run");
    CHECK(out != NULL && f.diags.count == 0);
    if (out != NULL) {
        uint32_t fields = 0;
        for (uint32_t i = 0; i < out->count; i++) {
            const struct mo_inferred_expression *note = &out->items[i];
            if (note->syntax->kind != MO_S_INT && note->syntax->kind != MO_S_TEXT)
                continue;
            CHECK(note->field != NULL);
            if (note->field != NULL) {
                CHECK(note->field->index == (fields == 0 ? 1u : 0u));
                CHECK(strcmp(note->type->name, fields == 0 ? "Int" : "Text") == 0);
            }
            fields++;
        }
        CHECK(fields == 2);
    }
    out = body(&f, "tree");
    CHECK(out != NULL && f.diags.count == 0);
    if (out != NULL) {
        const struct mo_source_type *type = out->items[out->count - 1].type;
        CHECK(strcmp(type->name, "Tree") == 0 && type->count == 1);
        CHECK(strcmp(type->arguments[0]->name, "Int") == 0);
    }
    finish(&f);

    init(&f,
         "module Main\nstruct Callback\nf: fn(Int) : Text\nend\n"
         "fn run() : Text\nCallback(f: fn(n) if n == 1: \"a\" else: \"b\" end).f(1)\nend\n",
         NULL);
    CHECK(body(&f, "run") != NULL && f.diags.count == 0);
    finish(&f);
}

static void contextual_fields(void) {
    struct fixture f;
    init(&f,
         "module Main\nstruct Apply(T)\nvalue: T\nf: fn(T) : Int\nend\n"
         "fn run() : Int\nrecord = Apply(f: (fn(p) p.0 end), value: (7, \"a\"))\n"
         "record.f(record.value)\nend\n",
         NULL);
    struct mo_inferred_body *out = body(&f, "run");
    CHECK(out != NULL && f.diags.count == 0);
    if (out != NULL) {
        uint32_t fields = 0;
        for (uint32_t i = 0; i < out->count; i++) {
            const struct mo_inferred_expression *note = &out->items[i];
            if (note->field != NULL &&
                (note->syntax->kind == MO_S_GROUP || note->syntax->kind == MO_S_TUPLE)) {
                CHECK(note->field->index == (fields == 0 ? 1u : 0u));
                fields++;
            }
        }
        CHECK(fields == 2);
        CHECK(strcmp(out->items[out->count - 1].type->name, "Int") == 0);
    }
    finish(&f);
}

static void field_assignment(void) {
    struct fixture f;
    init(&f,
         "module Main\nstruct Point\nx: Int\nend\nstruct Box\npoint: Point\nend\n"
         "fn run() : Int\nvar box = Box(point: Point(x: 1))\n"
         "box.point.x += 4\nbox.point.x = 9\nbox.point.x\nend\n",
         NULL);
    struct mo_inferred_body *out = body(&f, "run");
    CHECK(out != NULL && f.diags.count == 0);
    if (out != NULL)
        CHECK(strcmp(out->items[out->count - 1].type->name, "Int") == 0);
    finish(&f);

    const char *assignments[] = {"box.value += 2", "box.value = box.value + 2"};
    for (size_t i = 0; i < sizeof(assignments) / sizeof(assignments[0]); i++) {
        char source[256];
        int n = snprintf(source, sizeof(source),
                         "module Main\nstruct Box\nvalue: Byte\nend\nfn run()\n"
                         "var box = Box(value: 1)\n%s\nassert box.value == 3\nend\n",
                         assignments[i]);
        MO_ASSERT(n > 0 && (size_t)n < sizeof(source));
        init(&f, source, NULL);
        CHECK(body(&f, "run") == NULL);
        CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_BYTE_ARITHMETIC);
        finish(&f);
    }
}

static void choice_patterns(void) {
    struct fixture f;
    init(&f,
         "module Main\nenum Shape\nDot\nCircle(Decimal)\n"
         "Rectangle(width: Decimal, height: Decimal)\nend\n"
         "fn run(shape: Shape) : Decimal\ncase shape\nDot: 0\nCircle(r): 3 * r * r\n"
         "Rectangle(height: h, width: w): w * h\nend\nend\n",
         NULL);
    struct mo_inferred_body *out = body(&f, "run");
    CHECK(out != NULL && f.diags.count == 0);
    if (out != NULL)
        CHECK(strcmp(out->items[out->count - 1].type->name, "Decimal") == 0);
    finish(&f);

    init(&f,
         "module Main\nuse Shapes{Shape}\nfn run(shape: Shape) : Decimal\n"
         "case shape\nShapes.Dot: 0\nShapes.Circle(r): r\n"
         "Shapes.Rectangle(height: h, width: w): w * h\nend\nend\n",
         "module Shapes\nexpose Shape(..)\nenum Shape\nDot\nCircle(Decimal)\n"
         "Rectangle(width: Decimal, height: Decimal)\nend\n");
    out = body(&f, "run");
    CHECK(out != NULL && f.diags.count == 0);
    if (out != NULL) {
        bool bound = false;
        for (uint32_t i = 0; i < out->count; i++)
            if (out->items[i].syntax->kind == MO_S_NAME &&
                out->items[i].syntax->span.end - out->items[i].syntax->span.start == 1 &&
                out->items[i].syntax->text[0] == 'r') {
                CHECK(out->items[i].binding != NULL);
                bound = true;
            }
        CHECK(bound);
    }
    finish(&f);
}

static void qualified_patterns(void) {
    const char *other = "module Shapes\nexpose Shape(..), Box(..)\nenum Shape\nDot\n"
                        "Circle(Decimal)\nRectangle(width: Decimal, height: Decimal)\nend\n"
                        "struct Box\npair: (Int, Bool)\nshapes: List(Shape)\nend\n";
    const char *forms[] = {
        "fn run(shape: Shape) : Int\ncase shape\nShapes.Circle(2.5): 1\nCircle(_): 3\n"
        "Shapes.Dot: 0\nShapes.Rectangle(width: _, height: _): 2\nend\nend\n",
        "fn run(shape: Shape) : Decimal\nif shape is Shapes.Circle(r): r else: 0\nend\n",
        "fn run(xs: List(Shape)) : Int\ncase xs\n[]: 0\n[Shapes.Dot, .._rest]: 1\n"
        "[Shapes.Circle(_), .._rest]: 2\n[Shapes.Rectangle(width: _, height: _), .._rest]: 3\n"
        "end\nend\n",
        "fn run(box: Box) : Int\nif box is Shapes.Box(pair: (x, _), shapes: [Shapes.Dot, .._]): "
        "x else: 0\nend\n",
        "fn run(shape: Shape) : Int\ncase shape\nShapes.Circle(_) | Shapes.Dot: 0\n"
        "Shapes.Rectangle(width: _, height: _): 1\nend\nend\n",
        "fn run(pair: (Shape, Bool)) : Int\ncase pair\n(Shapes.Dot, _): 0\n"
        "(Shapes.Circle(_), _): 1\n(Shapes.Rectangle(width: _, height: _), _): 2\nend\nend\n",
    };
    for (size_t i = 0; i < sizeof(forms) / sizeof(forms[0]); i++) {
        struct mo_buffer source;
        mo_buffer_init(&source, 4096);
        MO_ASSERT(mo_buffer_append_text(&source, "module Main\nuse Shapes{Shape, Box}\n"));
        MO_ASSERT(mo_buffer_append_text(&source, forms[i]));
        MO_ASSERT(mo_buffer_append_byte(&source, 0));
        struct fixture f;
        init(&f, (const char *)source.bytes, other);
        CHECK(body(&f, "run") != NULL && f.diags.count == 0);
        finish(&f);
        mo_buffer_free(&source);
    }
    struct fixture f;
    init(&f,
         "module Main\nuse Shapes{Shape}\nfn run(shape: Shape, radius: Decimal) : Decimal\n"
         "case shape\nShapes.Circle(radius): radius\nShapes.Dot: 0\n"
         "Shapes.Rectangle(width: _, height: _): 0\nend\nend\n",
         other);
    CHECK(body(&f, "run") == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_BIND_REPEATED);
    finish(&f);
}

static void data_errors(void) {
    const struct {
        const char *source;
        uint16_t code;
        bool preparation;
    } cases[] = {
        {"struct Point\nx: Int\nx: Int\nend\n", MO_CODE_FIELD_REPEATED, true},
        {"struct Box(T, T)\nvalue: T\nend\n", MO_CODE_DATA_PARAMETERS, true},
        {"struct Point\nx: Int\nend\nfn run() : Point\nPoint(y: 1)\nend\n", MO_CODE_DATA_ARGUMENTS,
         false},
        {"struct Point\nx: Int\nend\nfn run(point: Point) : Int\npoint.y\nend\n",
         MO_CODE_FIELD_MISSING, false},
        {"enum Reply\nYes(value: Int)\nNo\nend\nfn run(reply: Reply) : Int\n"
         "case reply\nYes(n): n\nNo: 0\nend\nend\n",
         MO_CODE_PATTERN_FIELDS, false},
        {"fn run(reply: Option(Int)) : Int\ncase reply\n_: 0\nend\nend\n", MO_CODE_CASE_CATCH_ALL,
         false},
        {"fn run(reply: Option(Int)) : Int\ncase reply\nSome(n) | None: n\nend\nend\n",
         MO_CODE_PATTERN_ALTERNATIVES, false},
        {"fn run(flag: Bool) : Int\ncase flag\ntrue: 0\nend\nend\n", MO_CODE_CASE_INCOMPLETE,
         false},
        {"fn run(n: Int) : Int\ncase n\nInt: 0\n_: 1\nend\nend\n", MO_CODE_PATTERN_CONSTRUCTOR,
         false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char source[512];
        int n = snprintf(source, sizeof(source), "module Main\n%s", cases[i].source);
        MO_ASSERT(n > 0 && (size_t)n < sizeof(source));
        struct fixture f;
        init(&f, source, NULL);
        if (cases[i].preparation)
            CHECK(f.signatures == NULL);
        else
            CHECK(body(&f, "run") == NULL);
        if (f.diags.count != 1 || f.diags.items[0].code != cases[i].code)
            MO_ASSERT(fprintf(stderr, "data_errors[%zu]: expected E%04u, got %u diagnostics%s\n", i,
                              cases[i].code, f.diags.count,
                              f.diags.count == 0 ? "" : f.diags.items[0].message) >= 0);
        CHECK(f.diags.count == 1 && f.diags.items[0].code == cases[i].code);
        finish(&f);
    }
}

static void hidden_data(void) {
    const char *bodies[] = {"Box(value: 1).value", "box.value", "case box\nBox(value: n): n\nend"};
    for (unsigned exposed = 0; exposed < 2; exposed++) {
        char other[256];
        int n = snprintf(other, sizeof(other),
                         "module Shapes\nexpose Box%s\nstruct Box\nvalue: Int\nend\n",
                         exposed == 0 ? "" : "(..)");
        MO_ASSERT(n > 0 && (size_t)n < sizeof(other));
        for (size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); i++) {
            char source[256];
            n = snprintf(source, sizeof(source),
                         "module Main\nuse Shapes{Box}\nfn run(%s) : Int\n%s\nend\n",
                         i == 0 ? "" : "box: Box", bodies[i]);
            MO_ASSERT(n > 0 && (size_t)n < sizeof(source));
            struct fixture f;
            init(&f, source, other);
            CHECK((body(&f, "run") != NULL) == (exposed != 0));
            if (exposed == 0) {
                CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_DATA_PRIVATE);
                CHECK(f.diags.items[0].secondary_count == 1);
                CHECK(f.diags.items[0].secondary[0].span.file == 1);
            } else
                CHECK(f.diags.count == 0);
            finish(&f);
        }
    }
}

static void product_coverage(void) {
    for (unsigned complete = 0; complete < 2; complete++) {
        char source[512];
        int n = snprintf(source, sizeof(source),
                         "module Main\nfn run(pair: (Bool, Bool)) : Int\ncase pair\n"
                         "(true, true): 0\n(true, false): 1\n(false, true): 2\n%send\nend\n",
                         complete == 0 ? "" : "(false, false): 3\n");
        MO_ASSERT(n > 0 && (size_t)n < sizeof(source));
        struct fixture f;
        init(&f, source, NULL);
        CHECK((body(&f, "run") != NULL) == (complete != 0));
        if (complete == 0) {
            CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_CASE_INCOMPLETE);
            CHECK(strstr(f.diags.items[0].message, "missing: (false, false)") != NULL);
        } else
            CHECK(f.diags.count == 0);
        finish(&f);
    }
}

static void coverage_cost(void) {
    uint64_t small_work = 0;
    for (unsigned large = 0; large < 2; large++) {
        struct mo_buffer source;
        mo_buffer_init(&source, MO_LIMIT_FILE_SIZE);
        MO_ASSERT(mo_buffer_append_text(&source, "module Main\nenum Color\nRed\nBlue\nend\n"));
        for (uint32_t i = 0; large != 0 && i < 20000; i++) {
            MO_ASSERT(mo_buffer_append_text(&source, "fn unrelated"));
            MO_ASSERT(mo_buffer_append_uint(&source, i));
            MO_ASSERT(mo_buffer_append_text(&source, "()\nend\n"));
        }
        MO_ASSERT(mo_buffer_append_text(&source, "fn run(value: (Bool"));
        for (uint32_t i = 1; i < 10; i++)
            MO_ASSERT(mo_buffer_append_text(&source, ", Bool"));
        MO_ASSERT(mo_buffer_append_text(&source, ", Color)) : Int\ncase value\n"));
        for (uint32_t i = 0; i <= 10; i++)
            for (uint32_t color = 0; color < 2; color++) {
                MO_ASSERT(mo_buffer_append_byte(&source, '('));
                for (uint32_t j = 0; j < 10; j++)
                    MO_ASSERT(mo_buffer_append_text(&source, i == 10  ? "false, "
                                                             : i == j ? "true, "
                                                                      : "_, "));
                MO_ASSERT(mo_buffer_append_text(&source, color == 0 ? "Red): 0\n" : "Blue): 0\n"));
            }
        MO_ASSERT(mo_buffer_append_text(&source, "end\nend\n"));
        MO_ASSERT(mo_buffer_append_byte(&source, 0));
        struct fixture f;
        init_limit(&f, (const char *)source.bytes, NULL, 64 << 20);
        uint64_t prepared = *f.signatures->work;
        CHECK(body(&f, "run") != NULL && f.diags.count == 0);
        if (large == 0)
            small_work = *f.signatures->work - prepared;
        else
            CHECK(*f.signatures->work - prepared == small_work);
        finish(&f);
        mo_buffer_free(&source);
    }

    /* Diagonal rows create many specialized matrices. Bytes retained by the
     * coverage walk must hit work refusal long before a large arena is full. */
    struct mo_buffer source;
    mo_buffer_init(&source, 16384);
    MO_ASSERT(mo_buffer_append_text(&source, "module Main\nfn run(value: (Bool"));
    for (uint32_t i = 1; i < 20; i++)
        MO_ASSERT(mo_buffer_append_text(&source, ", Bool"));
    MO_ASSERT(mo_buffer_append_text(&source, ")) : Int\ncase value\n"));
    for (uint32_t i = 0; i < 40; i++) {
        MO_ASSERT(mo_buffer_append_byte(&source, '('));
        for (uint32_t j = 0; j < 20; j++) {
            if (j != 0)
                MO_ASSERT(mo_buffer_append_text(&source, ", "));
            MO_ASSERT(mo_buffer_append_text(&source, i / 2 != j   ? "_"
                                                     : i % 2 == 0 ? "true"
                                                                  : "false"));
        }
        MO_ASSERT(mo_buffer_append_text(&source, "): 0\n"));
    }
    MO_ASSERT(mo_buffer_append_text(&source, "end\nend\n"));
    MO_ASSERT(mo_buffer_append_byte(&source, 0));
    struct fixture f;
    init_limit(&f, (const char *)source.bytes, NULL, 64 << 20);
    CHECK(body(&f, "run") == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_CHECK_WORK);
    CHECK(f.arena.used < 2 * MO_LIMIT_SOURCE_CHECK_WORK);
    finish(&f);
    mo_buffer_free(&source);
}

static void missing_previews(void) {
    struct fixture f;
    init(&f,
         "module Main\nenum Color\nRed\nGreen\nBlue\nBlack\nend\n"
         "fn run(color: Color) : Int\ncase color\nRed: 0\nend\nend\n",
         NULL);
    CHECK(body(&f, "run") == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_CASE_INCOMPLETE);
    if (f.diags.count == 1)
        CHECK(strstr(f.diags.items[0].message, "missing: Green | Blue | Black") != NULL);
    finish(&f);
    for (unsigned guarded = 0; guarded < 2; guarded++) {
        struct mo_buffer source;
        mo_buffer_init(&source, 4096);
        MO_ASSERT(mo_buffer_append_text(&source, "module Main\nenum Color\n"));
        for (uint32_t i = 0; i < 12; i++) {
            MO_ASSERT(mo_buffer_append_text(&source, "Shade"));
            MO_ASSERT(mo_buffer_append_uint(&source, i));
            MO_ASSERT(mo_buffer_append_byte(&source, '\n'));
        }
        MO_ASSERT(mo_buffer_append_text(&source, "end\nfn run(color: Color) : Int\ncase color\n"));
        MO_ASSERT(
            mo_buffer_append_text(&source, guarded == 0 ? "Shade0: 0\n" : "Shade0 if false: 0\n"));
        MO_ASSERT(mo_buffer_append_text(&source, "end\nend\n"));
        MO_ASSERT(mo_buffer_append_byte(&source, 0));
        init(&f, (const char *)source.bytes, NULL);
        CHECK(body(&f, "run") == NULL);
        CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_CASE_INCOMPLETE);
        if (f.diags.count == 1) {
            CHECK(strstr(f.diags.items[0].message, guarded == 0 ? "and 3 more" : "and 4 more") !=
                  NULL);
            CHECK((strstr(f.diags.items[0].message, "guards don't count toward coverage") !=
                   NULL) == (guarded != 0));
        }
        finish(&f);
        mo_buffer_free(&source);
    }
    init(&f, "module Main\nfn run(order: Ordering) : Int\ncase order\nLess: 0\nend\nend\n", NULL);
    CHECK(body(&f, "run") == NULL);
    if (f.diags.count == 1)
        CHECK(strstr(f.diags.items[0].message, "missing: Equal | Greater") != NULL);
    finish(&f);
    init(&f, "module Main\nfn run(entry: Entry) : Bool\nentry is Entry(name: _, kind: File)\nend\n",
         NULL);
    CHECK(body(&f, "run") != NULL);
    CHECK(!mo_diags_any(&f.diags));
    finish(&f);
}

static void pattern_scopes(void) {
    struct fixture f;
    init(&f,
         "module Main\nenum Reply\nYes(Int)\nNo\nend\n"
         "fn run(first: Reply, second: Reply) : Int\nvar total = 0\n"
         "if first is Yes(n) and n > 0\ntotal += n\nend\n"
         "if second is Yes(n)\ntotal += n\nend\n"
         "assert first is Yes(m)\ntotal += m\n"
         "assert second is Yes(k) implies k > 0\ntotal\nend\n",
         NULL);
    CHECK(body(&f, "run") != NULL && f.diags.count == 0);
    finish(&f);
    const char *expired = "module Main\nfn run(x: Option(Int)) : Int\n"
                          "if not (x is Some(v))\nUnit\nend\nv\nend\n";
    init(&f, expired, NULL);
    CHECK(body(&f, "run") == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_NAME_UNKNOWN);
    if (f.diags.count == 1) {
        CHECK(f.diags.items[0].primary.span.start ==
              (uint32_t)(strstr(expired, "\nv\n") + 1 - expired));
        CHECK(f.diags.items[0].fix_count == 0);
    }
    finish(&f);
}

static void list_coverage(void) {
    struct fixture f;
    init(&f, "module Main\nfn run(xs: List(Int)) : Int\ncase xs\n[]: 0\n[x]: x\nend\nend\n", NULL);
    CHECK(body(&f, "run") == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_CASE_INCOMPLETE);
    if (f.diags.count == 1)
        CHECK(strstr(f.diags.items[0].message, "missing: [_, _, .._]") != NULL);
    finish(&f);
}

static void pattern_boundaries(void) {
    struct mo_buffer source;
    mo_buffer_init(&source, 1 << 20);
    MO_ASSERT(mo_buffer_append_text(&source, "module Main\nfn run(value: (Bool"));
    for (uint32_t i = 1; i < 4096; i++)
        MO_ASSERT(mo_buffer_append_text(&source, i % 32 == 0 ? ",\nBool" : ", Bool"));
    MO_ASSERT(mo_buffer_append_text(&source, ")) : Int\ncase value\n(_"));
    for (uint32_t i = 1; i < 4096; i++)
        MO_ASSERT(mo_buffer_append_text(&source, i % 32 == 0 ? ",\n_" : ", _"));
    MO_ASSERT(mo_buffer_append_text(&source, "): 0\nend\nend\n"));
    MO_ASSERT(mo_buffer_append_byte(&source, 0));
    struct fixture f;
    init(&f, (const char *)source.bytes, NULL);
    uint64_t prepared = *f.signatures->work;
    CHECK(body(&f, "run") != NULL && f.diags.count == 0);
    uint64_t cost = *f.signatures->work - prepared;
    MO_ASSERT(cost > 0 && cost < MO_LIMIT_PROGRAM_CHECK_WORK);
    *f.signatures->work = MO_LIMIT_PROGRAM_CHECK_WORK - cost;
    CHECK(body(&f, "run") != NULL && f.diags.count == 0);
    CHECK(*f.signatures->work == MO_LIMIT_PROGRAM_CHECK_WORK);
    *f.signatures->work = MO_LIMIT_PROGRAM_CHECK_WORK - cost + 1;
    CHECK(body(&f, "run") == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_CHECK_PROGRAM_WORK);
    finish(&f);
    mo_buffer_free(&source);

    mo_buffer_init(&source, 32768);
    MO_ASSERT(mo_buffer_append_text(&source, "module Main\nenum Chain\nEnd\nNext(Chain)\nend\n"
                                             "fn run(value: Chain) : Int\ncase value\n"));
    for (uint32_t i = 0; i < MO_LIMIT_NESTING_DEPTH - 3; i++)
        MO_ASSERT(mo_buffer_append_text(&source, "Next("));
    MO_ASSERT(mo_buffer_append_text(&source, "_"));
    for (uint32_t i = 0; i < MO_LIMIT_NESTING_DEPTH - 3; i++)
        MO_ASSERT(mo_buffer_append_text(&source, ")"));
    MO_ASSERT(mo_buffer_append_text(&source, ": 0\nNext(_): 1\nEnd: 2\nend\nend\n"));
    MO_ASSERT(mo_buffer_append_byte(&source, 0));
    init(&f, (const char *)source.bytes, NULL);
    CHECK(body(&f, "run") != NULL && f.diags.count == 0);
    finish(&f);
    mo_buffer_free(&source);

    init(&f,
         "module Main\nfn run(value: Option(Int)) : Int\ncase value\n"
         "Some(n) if n > 0: n\nNone: 0\nend\nend\n",
         NULL);
    CHECK(body(&f, "run") == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_CASE_INCOMPLETE);
    finish(&f);
}

static void builtin_choices(void) {
    struct fixture f;
    init(&f,
         "module Main\nfn run(flag: Bool) : Int\n"
         "value = if flag: Some(3) else: None\ncase value\n"
         "Some(n) if n > 0: n\nSome(_): 0\nNone: -1\nend\nend\n"
         "fn nested() : Result(Option(Int), Text)\nOk(Some(1))\nend\n",
         NULL);
    CHECK(body(&f, "run") != NULL && f.diags.count == 0);
    CHECK(body(&f, "nested") != NULL && f.diags.count == 0);
    finish(&f);
}

static void byte_coverage(void) {
    for (uint32_t count = 255; count <= 256; count++) {
        struct mo_buffer source;
        mo_buffer_init(&source, 16384);
        MO_ASSERT(mo_buffer_append_text(
            &source, "module Main\nfn run(value: Byte) : Int\ncase value\n0x00: 0\n"));
        for (uint32_t i = 0; i < count; i++) {
            if (i == 255)
                MO_ASSERT(mo_buffer_append_text(&source, "0xff"));
            else
                MO_ASSERT(mo_buffer_append_uint(&source, i));
            MO_ASSERT(mo_buffer_append_text(&source, ": 0\n"));
        }
        MO_ASSERT(mo_buffer_append_text(&source, "end\nend\n"));
        MO_ASSERT(mo_buffer_append_byte(&source, 0));
        struct fixture f;
        init(&f, (const char *)source.bytes, NULL);
        CHECK((body(&f, "run") != NULL) == (count == 256));
        if (count == 255) {
            CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_CASE_INCOMPLETE);
            if (f.diags.count == 1)
                CHECK(strstr(f.diags.items[0].message, "missing: 255") != NULL);
        }
        finish(&f);
        mo_buffer_free(&source);
    }
    for (unsigned broken = 0; broken < 2; broken++) {
        struct mo_buffer source;
        mo_buffer_init(&source, 16384);
        MO_ASSERT(
            mo_buffer_append_text(&source, "module Main\nfn run(b: Byte)\nvar x = 0\ncase x\n"));
        for (uint32_t i = 0; i < 256; i++) {
            MO_ASSERT(mo_buffer_append_uint(&source, i));
            MO_ASSERT(mo_buffer_append_text(&source, ": Unit\n"));
        }
        MO_ASSERT(mo_buffer_append_text(&source, "end\n"));
        if (broken != 0)
            MO_ASSERT(mo_buffer_append_text(&source, "_y = missing_name\n"));
        MO_ASSERT(mo_buffer_append_text(&source, "x = b\nend\n"));
        MO_ASSERT(mo_buffer_append_byte(&source, 0));
        struct fixture f;
        init(&f, (const char *)source.bytes, NULL);
        CHECK((body(&f, "run") != NULL) == (broken == 0));
        if (broken != 0)
            CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_NAME_UNKNOWN);
        finish(&f);
        mo_buffer_free(&source);
    }
}

static void named_pattern_fix(void) {
    const char source[] = "module Main\nenum Reply\nYes(value: Int)\nNo\nend\n"
                          "fn run(reply: Reply) : Int\ncase reply\nYes(n): n\nNo: 0\nend\nend\n";
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
    init(&f,
         "module Main\nstruct Point\nx: Int\ny: Int\nend\n"
         "fn run() : Point\nPoint(x: 1)\nend\n",
         NULL);
    CHECK(body(&f, "run") == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_DATA_ARGUMENTS);
    if (f.diags.count == 1)
        CHECK(strstr(f.diags.items[0].message, "missing field 'y'") != NULL);
    finish(&f);
    init(&f, "module Main\nfn run() : FileError\nFailed(path: \"x\")\nend\n", NULL);
    CHECK(body(&f, "run") == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_DATA_ARGUMENTS);
    if (f.diags.count == 1)
        CHECK(strstr(f.diags.items[0].message, "missing field 'reason'") != NULL);
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

static void map_literal_keys(void) {
    const struct {
        const char *keys;
        const char *type;
        bool repeated;
    } cases[] = {
        {"1: 3, 0x1: 5", "Int", true},
        {"0b1010: 3, 1_0: 5", "Int", true},
        {"-7: 3, (-0x7): 5", "Int", true},
        {"-0: 3, 0: 5", "Int", true},
        {"1: 3, 1.00: 5", "Decimal", true},
        {"1.1: 3, 11: 5", "Decimal", false},
        {"18446744073709551616: 3, 0x10000000000000000: 5", "BigInt", true},
        {"\"a\": 3, \"\\u{61}\": 5", "Text", true},
        {"'a': 3, '\\u{61}': 5", "Char", true},
        {"true: 3, (true): 5", "Bool", true},
        {"false: 3, true: 5", "Bool", false},
        {"\"a\\u{0}b\": 3, \"a\\u{0}c\": 5", "Text", false},
        {"1: 3, 2: 5, 1: 7, 2: 9", "Int", true},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char source[512];
        int n =
            snprintf(source, sizeof(source), "module Main\nfn run() : Map(%s, Int)\n{%s}\nend\n",
                     cases[i].type, cases[i].keys);
        MO_ASSERT(n > 0 && (size_t)n < sizeof(source));
        struct fixture f;
        init(&f, source, NULL);
        CHECK((body(&f, "run") == NULL) == cases[i].repeated);
        CHECK(f.diags.count == (cases[i].repeated ? 1u : 0u));
        if (f.diags.count != 0)
            CHECK(f.diags.items[0].code == MO_CODE_MAP_KEY_REPEATED);
        finish(&f);
    }
    struct fixture f;
    init(&f,
         "module Main\nfn run() : Map(Int, Int)\nvar key = 1\nvar other = 1\n"
         "{key: case true\ntrue:\nother = 2\nother + 1\nfalse: 4\nend, key: 5}\nend\n",
         NULL);
    CHECK(body(&f, "run") == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_MAP_KEY_REPEATED);
    finish(&f);
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

/* A Number parameter is checked once for every instantiation, so its literal
 * must fit Int, the narrowest Number type. */
static void number_parameter_range(const char *body_text, bool valid) {
    struct mo_buffer source;
    mo_buffer_init(&source, 8192);
    MO_ASSERT(mo_buffer_append_text(&source, "module Main\nfn run() : T where T: Number\n"));
    MO_ASSERT(mo_buffer_append_text(&source, body_text));
    MO_ASSERT(mo_buffer_append_text(&source, "\nend\n"));
    MO_ASSERT(mo_buffer_append_byte(&source, 0));
    struct fixture f;
    init(&f, (const char *)source.bytes, NULL);
    struct mo_inferred_body *out = body(&f, "run");
    CHECK((out != NULL) == valid);
    if (valid && out != NULL) {
        CHECK(f.diags.count == 0);
        CHECK(strcmp(out->items[out->count - 1].type->name, "T") == 0);
    } else if (!valid)
        CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_LITERAL_RANGE);
    finish(&f);
    mo_buffer_free(&source);
}

static void number_parameter_ranges(void) {
    number_parameter_range("1", true);
    number_parameter_range("9223372036854775807", true);
    number_parameter_range("-9223372036854775808", true);
    number_parameter_range("9223372036854775808", false);
    number_parameter_range("-9223372036854775809", false);
    number_parameter_range("170141183460469231731687303715884105728", false);

    struct fixture f;
    init(&f,
         "module Main\nfn run() : T where T: Number\ng = fn()\n"
         "9223372036854775808\nend\ng()\nend\n",
         NULL);
    CHECK(body(&f, "run") == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_LITERAL_RANGE);
    finish(&f);
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
        {"a = case true\ntrue: 1\nend\nb = case false\nfalse: 2\nend\na + b", "case true",
         MO_CODE_CASE_INCOMPLETE},
        {"a = case true\ntrue: 1\nend\nb = 2\na", "case true", MO_CODE_CASE_INCOMPLETE},
        {"_a = case true\ntrue: 1\nend\nmissing", "case true", MO_CODE_CASE_INCOMPLETE},
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
    init(&f,
         "module Main\nfn run(x: Option(Int)) : Int\ncase x\n"
         "Some(n) | Some(n): 0\nNone: 1\nend\nend\n",
         NULL);
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
    uint64_t prepared = *f.signatures->work;
    CHECK(body(&f, "run") != NULL);
    uint64_t cost = *f.signatures->work - prepared;
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
    prepared = *f.signatures->work;
    CHECK(body(&f, "run") == NULL);
    CHECK(f.diags.count == 1 && f.diags.items[0].code == MO_CODE_CHECK_WORK);
    CHECK(*f.signatures->work - prepared == MO_LIMIT_SOURCE_CHECK_WORK);
    finish(&f);
    mo_buffer_free(&source);
}

static void run_tests(void *argument) {
    (void)argument;
    record_fields();
    contextual_fields();
    field_assignment();
    choice_patterns();
    qualified_patterns();
    data_errors();
    hidden_data();
    product_coverage();
    coverage_cost();
    missing_previews();
    pattern_scopes();
    list_coverage();
    pattern_boundaries();
    builtin_choices();
    byte_coverage();
    named_pattern_fix();
    question_name();
    tuple_position();
    inferred_uses();
    scope_errors();
    map_literal_keys();
    literal_ranges();
    number_parameter_ranges();
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
}

int main(void) {
    CHECK(mo_with_stack(MO_SOURCE_STACK_SIZE, run_tests, NULL) == 0);
    return mo_test_report("check bodies");
}
