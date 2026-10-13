#include "base/buffer.h"
#include "base/codes.h"
#include "base/limits.h"
#include "base/stack.h"
#include "check/check.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

struct pair {
    const struct mo_source_type *a, *b;
};

struct comparison {
    struct mo_arena arena;
    struct pair *pairs;
    size_t capacity, count;
};

static MO_MUST_USE size_t hash(struct pair pair) {
    return ((uintptr_t)pair.a >> 3) ^ ((uintptr_t)pair.b >> 3) * (size_t)UINT32_C(2654435761);
}

static MO_MUST_USE bool seen(struct comparison *p, struct pair pair) {
    if (p->count * 2 >= p->capacity) {
        size_t capacity = p->capacity == 0 ? 256 : p->capacity * 2;
        struct pair *pairs =
            mo_arena_allocate_array(&p->arena, capacity, sizeof(*pairs), _Alignof(struct pair));
        MO_ASSERT(pairs != NULL);
        memset(pairs, 0, capacity * sizeof(*pairs));
        for (size_t i = 0; i < p->capacity; i++) {
            if (p->pairs[i].a == NULL)
                continue;
            size_t at = hash(p->pairs[i]) & (capacity - 1);
            while (pairs[at].a != NULL)
                at = (at + 1) & (capacity - 1);
            pairs[at] = p->pairs[i];
        }
        p->pairs = pairs;
        p->capacity = capacity;
    }
    size_t at = hash(pair) & (p->capacity - 1);
    while (p->pairs[at].a != NULL) {
        if (p->pairs[at].a == pair.a && p->pairs[at].b == pair.b)
            return true;
        at = (at + 1) & (p->capacity - 1);
    }
    p->pairs[at] = pair;
    p->count++;
    return false;
}

static void equal_type(struct comparison *p, const struct mo_source_type *a,
                       const struct mo_source_type *b, uint32_t depth) {
    MO_ASSERT(a != NULL && b != NULL && depth < MO_LIMIT_SOURCE_TYPE_DEPTH);
    if (seen(p, (struct pair){a, b}))
        return;
    MO_ASSERT(a->kind == b->kind && a->module == b->module && a->declaration == b->declaration &&
              a->count == b->count && strcmp(a->name, b->name) == 0);
    for (uint32_t i = 0; i < a->count; i++)
        equal_type(p, a->arguments[i], b->arguments[i], depth + 1);
}

static MO_MUST_USE bool text_equal(const char *a, const char *b) {
    return a == NULL || b == NULL ? a == b : strcmp(a, b) == 0;
}

static void equal_span(struct mo_span a, struct mo_span b) {
    MO_ASSERT(a.file == b.file && a.start == b.start && a.end == b.end);
}

static void equal_diags(const struct mo_diags *a, const struct mo_diags *b) {
    MO_ASSERT(a->count == b->count && a->dropped == b->dropped);
    for (uint32_t i = 0; i < a->count; i++) {
        const struct mo_diag *x = &a->items[i], *y = &b->items[i];
        MO_ASSERT(x->code == y->code && text_equal(x->message, y->message) &&
                  text_equal(x->reason, y->reason) && text_equal(x->section, y->section));
        equal_span(x->primary.span, y->primary.span);
        MO_ASSERT(text_equal(x->primary.text, y->primary.text));
        MO_ASSERT(x->secondary_count == y->secondary_count && x->fix_count == y->fix_count);
        for (uint32_t j = 0; j < x->secondary_count; j++) {
            equal_span(x->secondary[j].span, y->secondary[j].span);
            MO_ASSERT(text_equal(x->secondary[j].text, y->secondary[j].text));
        }
        for (uint32_t j = 0; j < x->fix_count; j++) {
            const struct mo_fix *u = &x->fixes[j], *v = &y->fixes[j];
            MO_ASSERT(text_equal(u->title, v->title) && u->edit_count == v->edit_count);
            for (uint32_t k = 0; k < u->edit_count; k++) {
                equal_span(u->edits[k].span, v->edits[k].span);
                MO_ASSERT(text_equal(u->edits[k].replacement, v->edits[k].replacement));
            }
        }
    }
}

static MO_MUST_USE bool no_memory(const struct mo_diags *diags) {
    for (uint32_t i = 0; i < diags->count; i++)
        if (diags->items[i].code == MO_CODE_CHECK_NO_MEMORY)
            return true;
    return false;
}

static MO_MUST_USE bool same_path(const struct mo_syntax *a, const struct mo_syntax *b) {
    if (a->count != b->count)
        return false;
    for (uint32_t i = 0; i < a->count; i++) {
        const struct mo_syntax *x = a->children[i], *y = b->children[i];
        size_t length = x->span.end - x->span.start;
        if (length != y->span.end - y->span.start || memcmp(x->text, y->text, length) != 0)
            return false;
    }
    return true;
}

static void check(const uint8_t *data, size_t size, size_t limit, int expected) {
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_arena syntax, preparation;
    MO_ASSERT(mo_sources_init(&sources) && mo_diags_init(&diags));
    mo_arena_init(&syntax, 64 << 20);
    mo_arena_init(&preparation, 16 << 20);
    uint32_t file;
    struct mo_source_error problem;
    if (!mo_sources_add_bytes(&sources, "main.mo", data, size, &file, &problem))
        goto done;
    struct mo_module_syntax *module = mo_parse(&syntax, &sources, file, &diags);
    if (module == NULL || mo_diags_any(&diags) || module->header.uses->count > 8)
        goto done;
    /* Resolve small constructed module sets, with real headers for every use.
     * Loader/graph fuzzing has its own target; these dependencies have no uses. */
    struct mo_module_syntax *modules[9] = {module};
    uint32_t targets[8], *uses[9] = {targets}, count = 1;
    for (uint32_t i = 0; i < module->header.uses->count; i++) {
        const struct mo_syntax *path = module->header.uses->children[i]->children[0];
        if (same_path(path, module->header.path))
            goto done;
        uint32_t target = 1;
        while (target < count && !same_path(path, modules[target]->header.path))
            target++;
        targets[i] = target;
        if (target != count)
            continue;
        struct mo_buffer helper;
        mo_buffer_init(&helper, (1 << 16) + 1024);
        MO_ASSERT(mo_buffer_append_text(&helper, "module "));
        for (uint32_t j = 0; j < path->count; j++) {
            if (j != 0)
                MO_ASSERT(mo_buffer_append_byte(&helper, '.'));
            const struct mo_syntax *part = path->children[j];
            MO_ASSERT(mo_buffer_append(&helper, part->text, part->span.end - part->span.start));
        }
        MO_ASSERT(mo_buffer_append_text(
            &helper, "\nexpose identity, add\nfn identity(value: T) : T\nvalue\nend\n"
                     "fn add(a: Int, b: Int) : Int\na + b\nend\n"));
        bool added = mo_sources_add_bytes(&sources, "helper.mo", helper.bytes, helper.length, &file,
                                          &problem);
        mo_buffer_free(&helper);
        if (!added)
            goto done;
        modules[count] = mo_parse(&syntax, &sources, file, &diags);
        if (modules[count] == NULL || mo_diags_any(&diags))
            goto done;
        count++;
    }
    struct mo_module_set set = {.count = count, .modules = modules, .uses = uses};
    struct mo_module_interfaces *interfaces = mo_resolve_modules(&preparation, &set, &diags);
    if (interfaces == NULL)
        goto done;
    struct mo_signatures *signatures =
        mo_prepare_signatures(&preparation, &set, interfaces, &diags);
    if (signatures == NULL)
        goto done;
    if (expected < 0 && size >= 8 && memcmp(data, "# budget", 8) == 0) {
        uint64_t remaining = (data[size - 1] & 1) == 0 ? 0 : (uint64_t)data[size - 1] * 16;
        *signatures->work = MO_LIMIT_PROGRAM_CHECK_WORK - remaining;
    }
    bool valid = true;
    for (uint32_t i = 0; i < signatures->count; i++) {
        struct mo_arena first_arena, second_arena;
        struct mo_diags first_diags, second_diags;
        mo_arena_init(&first_arena, limit);
        mo_arena_init(&second_arena, 64 << 20);
        MO_ASSERT(mo_diags_init(&first_diags) && mo_diags_init(&second_diags));
        /* Both runs start with the same shared program budget, even when the
         * first runs out of arena before consuming all its constraints. */
        uint64_t before = *signatures->work;
        struct mo_inferred_body *a =
            mo_infer_function(&first_arena, interfaces, signatures, i, &first_diags);
        *signatures->work = before;
        struct mo_inferred_body *b =
            mo_infer_function(&second_arena, interfaces, signatures, i, &second_diags);
        MO_ASSERT((a == NULL) == mo_diags_any(&first_diags));
        MO_ASSERT((b == NULL) == mo_diags_any(&second_diags));
        valid = valid && b != NULL;
        if (expected == 1 && b != NULL && signatures->items[i].function->module == 0 &&
            strcmp(signatures->items[i].function->name, "run") == 0)
            MO_ASSERT(b->count != 0 && strcmp(b->items[b->count - 1].type->name, "Int") == 0);
        if (!no_memory(&first_diags) && !no_memory(&second_diags)) {
            MO_ASSERT((a == NULL) == (b == NULL));
            equal_diags(&first_diags, &second_diags);
            if (a != NULL) {
                MO_ASSERT(a->count == b->count && a->function == b->function);
                struct comparison comparison = {0};
                mo_arena_init(&comparison.arena, 128 << 20);
                for (uint32_t j = 0; j < a->count; j++) {
                    const struct mo_inferred_expression *x = &a->items[j], *y = &b->items[j];
                    MO_ASSERT(x->syntax == y->syntax && x->binding == y->binding &&
                              x->symbol == y->symbol && x->never == y->never);
                    equal_type(&comparison, x->type, y->type, 0);
                }
                mo_arena_free(&comparison.arena);
            }
        }
        mo_diags_free(&first_diags);
        mo_diags_free(&second_diags);
        mo_arena_free(&first_arena);
        mo_arena_free(&second_arena);
    }
    if (expected >= 0)
        MO_ASSERT(valid == (expected != 0));
done:
    /* Generated controls must reach inference, never merely fail parsing or interfaces. */
    MO_ASSERT(expected < 0 || !mo_diags_any(&diags));
    mo_arena_free(&preparation);
    mo_arena_free(&syntax);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
}

struct input {
    const uint8_t *data;
    size_t size;
};

static void fuzz_input(void *argument) {
    const struct input *input = argument;
    const uint8_t *data = input->data;
    size_t size = input->size;
    if (size > (1 << 20))
        return;
    uint8_t choice = size == 0 ? 0 : data[0];
    size_t limit = (choice & 1) != 0 ? (size_t)choice * (size == 0 ? 1 : size) : 16 << 20;
    check(data, size, limit, -1);
    static const char *const valid[] = {"identity(1) + 2",
                                        "if true: 1 else: 2",
                                        "apply(fn(x) x + 1 end, 4)",
                                        "(2, \"x\").0",
                                        "if [1, 2] == [2, 1]: 1 else: 2",
                                        "f = fn(x) x + 1 end\nf(2)",
                                        "var total = 0\nfor i in 0..3\ntotal += i\nend\ntotal",
                                        "apply(fn(p) p.0 end, (1, \"a\"))",
                                        "Helpers.add(1, identity(2))"};
    static const char *const invalid[] = {"missing",
                                          "1 + true",
                                          "x = 1\nx = 2\nx",
                                          "x = 1\n2",
                                          "x += 1\n2",
                                          "identity()",
                                          "_xs = []\n1",
                                          "(1, 2).3",
                                          "ready? = 1\nready?",
                                          "1\n2",
                                          "Helpers.add(true, 2)"};
    bool succeeds = (choice & 2) == 0;
    uint8_t item = size < 2 ? 0 : data[1];
    struct mo_buffer source;
    mo_buffer_init(&source, 4096);
    MO_ASSERT(mo_buffer_append_text(
        &source, "module Main\nuse Helpers{identity}\n"
                 "fn apply(f: fn(T) : Int, p: T) : Int\nf(p)\nend\nfn run() : Int\n"));
    const char *expression = succeeds ? valid[item % 9] : invalid[item % 11];
    MO_ASSERT(mo_buffer_append_text(&source, expression));
    MO_ASSERT(mo_buffer_append_text(&source, "\nend\n"));
    check(source.bytes, source.length, limit, succeeds ? 1 : 0);
    mo_buffer_free(&source);

    /* An independently chosen result type and a varying literal exercise
     * inference, not just two executions of the same wrong answer. */
    mo_buffer_init(&source, 4096);
    MO_ASSERT(mo_buffer_append_text(&source, "module Main\nuse Helpers\nfn run() : "));
    MO_ASSERT(mo_buffer_append_text(&source, succeeds ? "Int" : "Text"));
    MO_ASSERT(mo_buffer_append_text(&source, "\nHelpers.identity("));
    MO_ASSERT(mo_buffer_append_uint(&source, item));
    MO_ASSERT(mo_buffer_append_text(&source, ")\nend\n"));
    check(source.bytes, source.length, limit, succeeds ? 1 : 0);
    mo_buffer_free(&source);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    struct input input = {data, size};
    MO_ASSERT(mo_with_stack(MO_SOURCE_STACK_SIZE, fuzz_input, &input) == 0);
    return 0;
}
