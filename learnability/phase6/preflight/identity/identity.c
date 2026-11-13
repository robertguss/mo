/* Throwaway prototype: content digests for promises and example rows. */
#define _POSIX_C_SOURCE 200809L

#include "base/sha256.h"
#include "base/stack.h"
#include "check/check.h"
#include "prim/prim.h"
#include "syntax/format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum mode { RAW, FORMATTED, CANONICAL, MODES };
static const char *const mode_names[MODES] = {"raw", "formatted", "canonical"};

struct decl {
    const char *module;
    const char *name;
    const struct mo_syntax *syntax;
    char *text[MODES];
};

struct walk {
    const struct mo_program *program;
    const struct mo_syntax *subject;
    const struct mo_syntax *seen[512];
    uint32_t count;
};

static struct mo_arena arena;
static struct mo_sources sources;
static struct mo_diags diags;
static const char *entry, *library;
static struct mo_module_syntax *fresh[64];

static void *need(void *p) {
    if (p == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }
    return p;
}

static uint32_t display_width(const uint8_t *bytes, size_t length) {
    uint64_t width = 0;
    MO_ASSERT(mo_unicode16_display_width(bytes, length, &width));
    return (uint32_t)width;
}

static const struct mo_syntax *find(const struct mo_syntax *n, struct mo_span span,
                                    enum mo_syntax_kind kind) {
    if (n == NULL)
        return NULL;
    if (n->kind == kind && n->span.start == span.start && n->span.end == span.end)
        return n;
    for (uint32_t i = 0; i < n->count; i++) {
        const struct mo_syntax *hit = find(n->children[i], span, kind);
        if (hit != NULL)
            return hit;
    }
    return NULL;
}

/* The same node in an unchecked parse of its file, so checking cannot have touched it. */
static const struct mo_syntax *parsed(const struct mo_syntax *n) {
    uint32_t file = n->span.file;
    MO_ASSERT(file < 64);
    if (fresh[file] == NULL)
        fresh[file] = need(mo_parse(&arena, &sources, file, &diags));
    return need((void *)find(fresh[file]->tree, n->span, n->kind));
}

static struct mo_syntax *copy(const struct mo_syntax *n, bool strip) {
    if (n == NULL)
        return NULL;
    struct mo_syntax *out = need(mo_arena_allocate(&arena, sizeof *out, _Alignof(struct mo_syntax)));
    *out = *n;
    if (n->count != 0)
        out->children = need(mo_arena_allocate_array(&arena, n->count, sizeof *out->children,
                                                     _Alignof(struct mo_syntax *)));
    for (uint32_t i = 0; i < n->count; i++)
        out->children[i] = copy(n->children[i], strip);
    if (strip) {
        out->leading = out->trailing = out->ending = NULL;
        out->blank_before = false;
    }
    return out;
}

static struct mo_syntax *empty_sequence(struct mo_span span) {
    struct mo_syntax *out = need(mo_arena_allocate(&arena, sizeof *out, _Alignof(struct mo_syntax)));
    *out = (struct mo_syntax){.kind = MO_S_SEQUENCE, .span = span};
    return out;
}

static void append(struct mo_buffer *b, const uint8_t *bytes, uint32_t start, uint32_t end) {
    need((void *)(uintptr_t)mo_buffer_append(b, bytes + start, end - start));
}

static char *finish(struct mo_buffer *b) {
    need((void *)(uintptr_t)mo_buffer_append_byte(b, 0));
    char *out = need(mo_arena_copy(&arena, b->bytes, b->length, 1));
    mo_buffer_free(b);
    return out;
}

/* A declaration's text without its promises and examples; signature keeps only the header. */
static char *text(const struct mo_syntax *original, enum mode mode, bool signature) {
    const struct mo_syntax *n = parsed(original);
    struct mo_buffer b;
    mo_buffer_init(&b, 1 << 20);
    if (mode == RAW) {
        const uint8_t *bytes = sources.files[n->span.file].bytes;
        if (n->kind != MO_S_FN) {
            append(&b, bytes, n->span.start, n->span.end);
        } else if (signature) {
            uint32_t end = 0;
            for (uint32_t i = 0; i < 4; i++)
                if (n->children[i] != NULL && n->children[i]->span.end > end)
                    end = n->children[i]->span.end;
            append(&b, bytes, n->span.start, end);
        } else {
            uint32_t at = n->span.start;
            const struct mo_syntax *spec = n->children[4];
            for (uint32_t i = 0; i < spec->count; i++) {
                append(&b, bytes, at, spec->children[i]->span.start);
                at = spec->children[i]->span.end;
            }
            append(&b, bytes, at, n->span.end);
        }
        return finish(&b);
    }
    struct mo_syntax *item = copy(n, mode == CANONICAL);
    if (item->kind == MO_S_FN) {
        item->children[4] = empty_sequence(n->span);
        if (signature)
            item->children[5] = empty_sequence(n->span);
    }
    struct mo_syntax *items = empty_sequence(n->span);
    items->count = 1;
    items->children = &item;
    struct mo_syntax *pair[2] = {NULL, items};
    struct mo_syntax file = {.kind = MO_S_FILE, .span = n->span, .count = 2, .children = pair};
    struct mo_module_syntax module = {.span = n->span, .tree = &file};
    struct mo_buffer out;
    mo_buffer_init(&out, 1 << 20);
    if (!mo_format(&module, (struct mo_format_options){100, display_width}, &out)) {
        fprintf(stderr, "the formatter refused a declaration\n");
        exit(2);
    }
    mo_buffer_free(&b);
    return finish(&out);
}

static bool library_module(const struct mo_program *p, uint32_t module) {
    return module == UINT32_MAX || (p->set->library != NULL && p->set->library[module]);
}

static void walk_body(struct walk *w, const struct mo_checked_body *body);
static void walk_type(struct walk *w, const struct mo_source_type *type);

static void visit(struct walk *w, const struct mo_symbol *symbol) {
    const struct mo_program *p = w->program;
    if (symbol == NULL || library_module(p, symbol->module))
        return;
    if (symbol->owner != UINT32_MAX)
        symbol = &p->interfaces->scopes[symbol->module].symbols[symbol->owner];
    const struct mo_syntax *d = symbol->declaration;
    if (d == NULL || d == w->subject)
        return;
    for (uint32_t i = 0; i < w->count; i++)
        if (w->seen[i] == d)
            return;
    MO_ASSERT(w->count < 512);
    w->seen[w->count++] = d;
    for (uint32_t i = 0; i < p->function_count; i++)
        if (p->functions[i].signature->function->declaration == d) {
            walk_type(w, p->functions[i].signature->type);
            walk_body(w, &p->functions[i].body);
        }
    for (uint32_t i = 0; i < p->constant_count; i++)
        if (p->constants[i].symbol->declaration == d) {
            walk_type(w, p->constants[i].type);
            walk_body(w, &p->constants[i].initializer);
        }
    for (uint32_t i = 0; i < p->signatures->data_count; i++) {
        const struct mo_data_signature *data = &p->signatures->data[i];
        if (data->type->declaration == NULL || data->type->declaration->declaration != d)
            continue;
        for (uint32_t j = 0; j < data->count; j++)
            walk_type(w, data->fields[j].type);
    }
}

static void walk_type(struct walk *w, const struct mo_source_type *type) {
    if (type == NULL)
        return;
    if (type->kind == MO_SOURCE_NAMED)
        visit(w, type->declaration);
    for (uint32_t i = 0; i < type->count; i++)
        walk_type(w, type->arguments[i]);
}

static void walk_proof(struct walk *w, const struct mo_ability_implementation *proof) {
    if (proof == NULL)
        return;
    if (proof->kind == MO_ABILITY_FUNCTION)
        visit(w, proof->function);
    for (uint32_t i = 0; i < proof->part_count; i++)
        walk_proof(w, proof->parts[i]);
}

static void walk_body(struct walk *w, const struct mo_checked_body *body) {
    for (uint32_t i = 0; i < body->count; i++) {
        const struct mo_checked_expression *e = &body->items[i];
        visit(w, e->symbol);
        walk_type(w, e->type);
        for (uint32_t j = 0; j < e->ability_count; j++)
            walk_proof(w, e->abilities[j].implementation);
    }
    for (uint32_t i = 0; i < body->binding_count; i++)
        walk_type(w, body->bindings[i].type);
}

static const char *module_of(const struct mo_program *p, const struct mo_syntax *d) {
    for (uint32_t m = 0; m < p->interfaces->count; m++) {
        const struct mo_module_scope *scope = &p->interfaces->scopes[m];
        for (uint32_t i = 0; i < scope->count; i++)
            if (scope->symbols[i].declaration == d)
                return p->set->names[m];
    }
    MO_UNREACHABLE();
}

static const char *name_of(const struct mo_program *p, const struct mo_syntax *d) {
    for (uint32_t m = 0; m < p->interfaces->count; m++) {
        const struct mo_module_scope *scope = &p->interfaces->scopes[m];
        for (uint32_t i = 0; i < scope->count; i++)
            if (scope->symbols[i].declaration == d)
                return scope->symbols[i].name;
    }
    MO_UNREACHABLE();
}

static void field(struct mo_sha256 *h, const char *s) {
    uint64_t length = strlen(s);
    uint8_t prefix[8];
    for (int i = 0; i < 8; i++)
        prefix[i] = (uint8_t)(length >> (8 * i));
    mo_sha256_update(h, prefix, 8);
    mo_sha256_update(h, s, length);
}

static void json_string(const char *s) {
    putchar('"');
    for (; *s != 0; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            printf("\\%c", c);
        else if (c == '\n')
            printf("\\n");
        else if (c < 0x20)
            printf("\\u%04x", c);
        else
            putchar(c);
    }
    putchar('"');
}

static int by_name(const void *a, const void *b) {
    const struct decl *x = a, *y = b;
    int c = strcmp(x->module, y->module);
    return c != 0 ? c : strcmp(x->name, y->name);
}

static struct decl all[512];
static uint32_t all_count;

static struct decl *declaration(const struct mo_program *p, const struct mo_syntax *d) {
    for (uint32_t i = 0; i < all_count; i++)
        if (all[i].syntax == d)
            return &all[i];
    MO_ASSERT(all_count < 512);
    struct decl *out = &all[all_count++];
    *out = (struct decl){module_of(p, d), name_of(p, d), d, {NULL}};
    for (int m = 0; m < MODES; m++)
        out->text[m] = text(d, (enum mode)m, false);
    return out;
}

static void run(void *unused) {
    (void)unused;
    struct mo_module_set *set = mo_load_program(&arena, &sources, entry, library, &diags);
    struct mo_program *p = set == NULL || mo_diags_any(&diags) ? NULL : mo_check(&arena, set, &diags);
    if (p == NULL || mo_diags_any(&diags)) {
        fprintf(stderr, "the program does not check (%u diagnostics)\n", diags.count);
        exit(1);
    }
    printf("{\"items\":[");
    bool first = true;
    for (uint32_t s = 0; s < p->spec_count; s++) {
        const struct mo_checked_spec *spec = &p->specs[s];
        if ((spec->kind != MO_SPEC_PROMISE && spec->kind != MO_SPEC_EXAMPLE) ||
            library_module(p, spec->module))
            continue;
        const struct mo_function_signature *subject = p->functions[spec->subject].signature;
        struct walk w = {.program = p, .subject = subject->function->declaration};
        walk_body(&w, &spec->body);
        for (uint32_t a = 0; a < p->spec_count; a++)
            if (p->specs[a].kind == MO_SPEC_ANCHOR && p->specs[a].promise == s)
                walk_body(&w, &p->specs[a].body);
        struct decl reach[512];
        for (uint32_t i = 0; i < w.count; i++)
            reach[i] = *declaration(p, w.seen[i]);
        qsort(reach, w.count, sizeof *reach, by_name);
        const char *kind = spec->kind == MO_SPEC_PROMISE ? "promise" : "example";
        char owner[256];
        snprintf(owner, sizeof owner, "%s.%s", p->set->names[spec->module],
                 subject->function->name);
        printf("%s\n{\"name\":\"%s.%s.%u\",\"kind\":\"%s\",\"subject\":\"%s\",\"line\":%u,",
               first ? "" : ",", owner, kind, spec->number, kind, owner,
               mo_sources_position(&sources, spec->syntax->span.file, spec->syntax->span.start)
                   .line);
        first = false;
        printf("\"reach\":[");
        for (uint32_t i = 0; i < w.count; i++)
            printf("%s\"%s.%s\"", i == 0 ? "" : ",", reach[i].module, reach[i].name);
        printf("],\"digests\":{");
        for (int m = 0; m < MODES; m++) {
            char *own = text(spec->syntax, (enum mode)m, false);
            char *signature = text(subject->function->declaration, (enum mode)m, true);
            printf("%s\"%s\":{", m == 0 ? "" : ",", mode_names[m]);
            for (int with = 1; with >= 0; with--) {
                struct mo_sha256 h;
                mo_sha256_init(&h);
                field(&h, owner);
                field(&h, kind);
                field(&h, own);
                if (with)
                    field(&h, signature);
                for (uint32_t i = 0; i < w.count; i++) {
                    char full[256];
                    snprintf(full, sizeof full, "%s.%s", reach[i].module, reach[i].name);
                    field(&h, full);
                    field(&h, reach[i].text[m]);
                }
                uint8_t digest[32];
                mo_sha256_finish(&h, digest);
                printf("\"%s\":\"", with ? "with_signature" : "without_signature");
                for (int i = 0; i < 32; i++)
                    printf("%02x", digest[i]);
                printf("\"%s", with ? "," : "");
            }
            printf("}");
        }
        printf("},\"text\":");
        json_string(text(spec->syntax, CANONICAL, false));
        printf(",\"subject_signature\":");
        json_string(text(subject->function->declaration, CANONICAL, true));
        printf("}");
    }
    printf("\n],\"declarations\":{");
    qsort(all, all_count, sizeof *all, by_name);
    for (uint32_t i = 0; i < all_count; i++) {
        printf("%s\n\"%s.%s\":{", i == 0 ? "" : ",", all[i].module, all[i].name);
        for (int m = 0; m < MODES; m++) {
            printf("%s\"%s\":", m == 0 ? "" : ",", mode_names[m]);
            json_string(all[i].text[m]);
        }
        printf("}");
    }
    printf("\n}}\n");
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: identity ENTRY.mo LIBRARY-FOLDER\n");
        return 2;
    }
    entry = argv[1];
    library = argv[2];
    mo_arena_init(&arena, (size_t)512 << 20);
    if (!mo_sources_init(&sources) || !mo_diags_init(&diags))
        return 2;
    return mo_with_stack(MO_SOURCE_STACK_SIZE, run, NULL) == 0 ? 0 : 2;
}
