#define _POSIX_C_SOURCE 200809L

#include "base/buffer.h"
#include "base/codes.h"
#include "check/check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static char root[] = "/tmp/mo-fuzz-check-XXXXXX";
static char files[3][128], library[128], library_file[160];
static bool ready;
static const char money[] = "module Money\nexpose Money, Choice(..), total\n"
                            "struct Money\n amount: Int\nend\nstruct Line\n amount: Int\nend\n"
                            "enum Choice\n Ready\n Done\nend\nsecret = 1\n"
                            "fn total(money: Money) : Int\n money.amount\nend\n"
                            "fn total(line: Line) : Int\n line.amount\nend\n";
static const char another[] =
    "module Another\nexpose Status(..)\nenum Status\n Ready\n Quit\nend\n";

static void cleanup(void) {
    for (size_t i = 0; i < 3; i++)
        MO_ASSERT(unlink(files[i]) == 0);
    MO_ASSERT(unlink(library_file) == 0);
    MO_ASSERT(rmdir(library) == 0);
    MO_ASSERT(rmdir(root) == 0);
}

static void write_file(const char *path, const uint8_t *data, size_t size) {
    FILE *file = fopen(path, "wb");
    MO_ASSERT(file != NULL);
    if (size != 0)
        MO_ASSERT(fwrite(data, 1, size, file) == size);
    MO_ASSERT(fclose(file) == 0);
}

static void setup(void) {
    if (ready)
        return;
    MO_ASSERT(mkdtemp(root) != NULL);
    const char *names[] = {"main", "money", "another"};
    for (size_t i = 0; i < 3; i++) {
        int n = snprintf(files[i], sizeof(files[i]), "%s/%s.mo", root, names[i]);
        MO_ASSERT(n > 0 && (size_t)n < sizeof(files[i]));
    }
    int b = snprintf(library, sizeof(library), "%s/lib", root);
    int c = snprintf(library_file, sizeof(library_file), "%s/library.mo", library);
    MO_ASSERT(b > 0 && (size_t)b < sizeof(library));
    MO_ASSERT(c > 0 && (size_t)c < sizeof(library_file));
    MO_ASSERT(mkdir(library, 0700) == 0);
    const char text[] = "module Library\nexpose Item(..), Choice(..), read\n"
                        "struct Item\n value: Int\nend\nenum Choice\n Fresh\n Old\nend\n"
                        "fn read(item: Item) : Int\n item.value\nend\n";
    write_file(library_file, (const uint8_t *)text, sizeof(text) - 1);
    MO_ASSERT(atexit(cleanup) == 0);
    ready = true;
}

static MO_MUST_USE bool equal_text(const char *a, const char *b) {
    return a == NULL || b == NULL ? a == b : strcmp(a, b) == 0;
}

static void equal_span(struct mo_span a, struct mo_span b) {
    MO_ASSERT(a.file == b.file && a.start == b.start && a.end == b.end);
}

static void equal_diags(const struct mo_diags *a, const struct mo_diags *b) {
    MO_ASSERT(a->count == b->count && a->dropped == b->dropped);
    for (uint32_t i = 0; i < a->count; i++) {
        const struct mo_diag *x = &a->items[i], *y = &b->items[i];
        MO_ASSERT(x->code == y->code && equal_text(x->message, y->message) &&
                  equal_text(x->reason, y->reason) && equal_text(x->section, y->section));
        equal_span(x->primary.span, y->primary.span);
        MO_ASSERT(equal_text(x->primary.text, y->primary.text));
        MO_ASSERT(x->secondary_count == y->secondary_count && x->fix_count == y->fix_count);
        for (uint32_t j = 0; j < x->secondary_count; j++) {
            equal_span(x->secondary[j].span, y->secondary[j].span);
            MO_ASSERT(equal_text(x->secondary[j].text, y->secondary[j].text));
        }
        for (uint32_t j = 0; j < x->fix_count; j++) {
            const struct mo_fix *u = &x->fixes[j], *v = &y->fixes[j];
            MO_ASSERT(equal_text(u->title, v->title) && u->edit_count == v->edit_count);
            for (uint32_t k = 0; k < u->edit_count; k++) {
                equal_span(u->edits[k].span, v->edits[k].span);
                MO_ASSERT(equal_text(u->edits[k].replacement, v->edits[k].replacement));
            }
        }
    }
}

static void equal_symbol(const struct mo_symbol *a, const struct mo_symbol *b) {
    MO_ASSERT(equal_text(a->name, b->name) && a->name_length == b->name_length);
    MO_ASSERT(a->declaration == b->declaration && a->module == b->module);
    MO_ASSERT(a->owner == b->owner && a->receiver == b->receiver);
    MO_ASSERT(a->case_count == b->case_count && a->overload_count == b->overload_count);
    MO_ASSERT(a->exported == b->exported && a->insides == b->insides);
    for (uint32_t i = 0; i < a->case_count; i++)
        MO_ASSERT(a->cases[i] == b->cases[i]);
}

static MO_MUST_USE bool allocation_error(const struct mo_diags *diags) {
    for (uint32_t i = 0; i < diags->count; i++)
        if (diags->items[i].code == MO_CODE_CHECK_NO_MEMORY)
            return true;
    return false;
}

/* Compare complete results in fresh arenas, including failed allocations after
 * language errors. Loading never decides whether the direct resolver is run. */
static void resolve(const struct mo_module_set *set, size_t limit) {
    struct mo_arena first_arena, second_arena;
    struct mo_diags first_diags, second_diags;
    mo_arena_init(&first_arena, limit);
    mo_arena_init(&second_arena, 16 << 20);
    MO_ASSERT(mo_diags_init(&first_diags) && mo_diags_init(&second_diags));
    struct mo_module_interfaces *first = mo_resolve_modules(&first_arena, set, &first_diags);
    struct mo_module_interfaces *second = mo_resolve_modules(&second_arena, set, &second_diags);
    MO_ASSERT((first == NULL) == mo_diags_any(&first_diags));
    MO_ASSERT((second == NULL) == mo_diags_any(&second_diags));
    if (!allocation_error(&first_diags) && !allocation_error(&second_diags)) {
        MO_ASSERT((first == NULL) == (second == NULL));
        equal_diags(&first_diags, &second_diags);
        if (first != NULL) {
            MO_ASSERT(first->count == second->count);
            for (uint32_t i = 0; i < first->count; i++) {
                const struct mo_module_scope *a = &first->scopes[i], *b = &second->scopes[i];
                MO_ASSERT(a->count == b->count && a->imported_count == b->imported_count &&
                          a->alias_count == b->alias_count);
                for (uint32_t j = 0; j < a->count; j++)
                    equal_symbol(&a->symbols[j], &b->symbols[j]);
                for (uint32_t j = 0; j < a->imported_count; j++) {
                    MO_ASSERT(equal_text(a->imported[j].name, b->imported[j].name));
                    MO_ASSERT(a->imported[j].count == b->imported[j].count);
                    MO_ASSERT(a->imported[j].origin == b->imported[j].origin);
                    for (uint32_t k = 0; k < a->imported[j].count; k++)
                        equal_symbol(&a->imported[j].symbols[k], &b->imported[j].symbols[k]);
                }
                for (uint32_t j = 0; j < a->alias_count; j++) {
                    MO_ASSERT(equal_text(a->aliases[j].name, b->aliases[j].name));
                    MO_ASSERT(a->aliases[j].module == b->aliases[j].module);
                    MO_ASSERT(a->aliases[j].origin == b->aliases[j].origin);
                }
            }
        }
    }
    mo_diags_free(&first_diags);
    mo_diags_free(&second_diags);
    mo_arena_free(&first_arena);
    mo_arena_free(&second_arena);
}

static MO_MUST_USE uint8_t byte(const uint8_t *data, size_t size, size_t index) {
    return size == 0 ? 0 : data[index % size];
}

static void append(struct mo_buffer *b, const char *text) {
    MO_ASSERT(mo_buffer_append_text(b, text));
}

/* Valid syntax chosen by the bytes reaches clashes, visibility, receivers,
 * case expansion, repeated imports and missing names on every fuzz run. */
static void generated(const uint8_t *data, size_t size, size_t limit) {
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_arena syntax;
    MO_ASSERT(mo_sources_init(&sources) && mo_diags_init(&diags));
    mo_arena_init(&syntax, 1 << 20);
    struct mo_module_syntax *modules[3];
    struct mo_buffer b;
    mo_buffer_init(&b, 4096);
    const char *imports[] = {"Choice", "Money, total", "Choice, Ready",  "Ready",
                             "secret", "missing",      "Choice, Choice", "Text"};
    append(&b, "module Main\nuse Money{");
    append(&b, imports[byte(data, size, 0) % 8]);
    append(&b, "}\nuse Another");
    if ((byte(data, size, 1) & 1) != 0)
        append(&b, "{Status}");
    append(&b, "\n");
    if ((byte(data, size, 2) & 1) != 0)
        append(&b, "use Money{Choice}\n");
    if ((byte(data, size, 3) & 1) != 0)
        append(&b, "zeta = 1\nzeta = 2\nalpha = 1\nalpha = 2\n");
    if ((byte(data, size, 3) & 2) != 0)
        append(&b, "struct Text\n value: Int\nend\n");
    for (uint32_t i = 0; i < 3; i++) {
        if (i == 1) {
            b.length = 0;
            append(&b, "module Money\nexpose Money, total, Choice");
            if ((byte(data, size, 4) & 1) == 0)
                append(&b, "(..)");
            if ((byte(data, size, 4) & 2) != 0)
                append(&b, ", missing, Ready");
            append(&b, "\n");
            append(&b, "struct Money(T)\n amount: T\nend\nstruct Line\n amount: Int\nend\n"
                       "enum Choice\n Ready\n Done\nend\nsecret = 1\n"
                       "fn total(m: Money(Int)) : Int\n 1\nend\nfn total(m: ");
            const char *receivers[] = {"Line", "Money(Text)", "Int", "Money(Int)"};
            append(&b, receivers[byte(data, size, 5) % 4]);
            append(&b, ") : Int\n 2\nend\n");
        } else if (i == 2) {
            b.length = 0;
            append(&b, another);
            if ((byte(data, size, 6) & 1) != 0)
                append(&b, "enum Trouble\n Ready\nend\n");
        }
        uint32_t file;
        struct mo_source_error problem;
        MO_ASSERT(mo_sources_add_bytes(&sources, files[i], b.bytes, b.length, &file, &problem));
        modules[i] = mo_parse(&syntax, &sources, file, &diags);
        MO_ASSERT(modules[i] != NULL && !mo_diags_any(&diags));
    }
    uint32_t targets[] = {1, 2, 1}, *uses[] = {targets, NULL, NULL}, order[] = {1, 2, 0};
    struct mo_module_set set = {.count = 3, .modules = modules, .uses = uses, .order = order};
    resolve(&set, limit);
    mo_buffer_free(&b);
    mo_arena_free(&syntax);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > (1 << 16))
        return 0;
    setup();
    /* Three NUL-separated documents mutate a real graph. Ordinary .mo seeds
     * get Money and Another defaults so their imports reach resolution too. */
    size_t offset = 0;
    for (size_t i = 0; i < 3; i++) {
        if (i != 0 && offset > size) {
            const char *fallback = i == 1 ? money : another;
            write_file(files[i], (const uint8_t *)fallback, strlen(fallback));
        } else {
            const uint8_t *end = memchr(data + offset, 0, size - offset);
            size_t length = end == NULL ? size - offset : (size_t)(end - data) - offset;
            write_file(files[i], data + offset, length);
            offset += length + 1;
        }
    }
    size_t limit = size != 0 && (data[size - 1] & 7) == 0 ? size * (1u + data[0]) : 16 << 20;
    generated(data, size, limit);
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_arena arena;
    MO_ASSERT(mo_sources_init(&sources) && mo_diags_init(&diags));
    mo_arena_init(&arena, limit);
    struct mo_module_set *set = mo_load(&arena, &sources, files[0], library, &diags);
    MO_ASSERT((set == NULL) == mo_diags_any(&diags));
    if (set != NULL)
        resolve(set, limit);
    mo_arena_free(&arena);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
    return 0;
}
