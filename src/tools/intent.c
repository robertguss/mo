#define _POSIX_C_SOURCE 200809L

#include "tools/intent.h"

#include "base/sha256.h"
#include "prim/prim.h"
#include "syntax/format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct text {
    const uint8_t *bytes;
    size_t length;
};

struct entry {
    const char *module;
    const char *name;
    struct text text;
};

struct walker {
    struct mo_arena *arena;
    const struct mo_program *program;
    struct mo_diags diags;
    uint32_t *offsets;
    uint32_t symbol_count;
    uint32_t *functions, *constants, *data;
    uint32_t *marks;
    uint32_t *reach;
    uint32_t reach_count;
    struct text *texts;
    struct mo_module_syntax **parsed;
    const struct mo_syntax *subject;
    uint32_t mark;
    uint64_t work;
    bool ok;
};

static void *array(struct walker *w, size_t count, size_t size, size_t align) {
    void *out = mo_arena_allocate_array(w->arena, count == 0 ? 1 : count, size, align);
    if (out == NULL)
        w->ok = false;
    return out;
}

static bool charge(struct walker *w) {
    if (w->work == 0)
        w->ok = false;
    else
        w->work--;
    return w->ok;
}

static bool library(const struct mo_program *p, uint32_t module) {
    return module == UINT32_MAX || p->set->library[module];
}

static uint32_t symbol_index(const struct walker *w, const struct mo_symbol *symbol) {
    const struct mo_module_scope *scope = &w->program->interfaces->scopes[symbol->module];
    MO_ASSERT(symbol >= scope->symbols && symbol < scope->symbols + scope->count);
    return w->offsets[symbol->module] + (uint32_t)(symbol - scope->symbols);
}

static void visit(struct walker *w, const struct mo_symbol *symbol) {
    const struct mo_program *p = w->program;
    if (symbol == NULL || library(p, symbol->module) || !charge(w))
        return;
    if (symbol->owner != UINT32_MAX)
        symbol = &p->interfaces->scopes[symbol->module].symbols[symbol->owner];
    if (symbol->declaration == NULL || symbol->declaration == w->subject)
        return;
    uint32_t index = symbol_index(w, symbol);
    if (w->marks[index] == w->mark)
        return;
    w->marks[index] = w->mark;
    w->reach[w->reach_count++] = index;
}

static void walk_type(struct walker *w, const struct mo_source_type *type) {
    if (type == NULL || !charge(w))
        return;
    if (type->kind == MO_SOURCE_NAMED)
        visit(w, type->declaration);
    for (uint32_t i = 0; i < type->count; i++)
        walk_type(w, type->arguments[i]);
}

static void walk_proof(struct walker *w, const struct mo_ability_implementation *proof) {
    if (proof == NULL || !charge(w))
        return;
    if (proof->kind == MO_ABILITY_FUNCTION)
        visit(w, proof->function);
    for (uint32_t i = 0; i < proof->part_count; i++)
        walk_proof(w, proof->parts[i]);
}

static void walk_body(struct walker *w, const struct mo_checked_body *body) {
    for (uint32_t i = 0; i < body->count && charge(w); i++) {
        const struct mo_checked_expression *e = &body->items[i];
        visit(w, e->symbol);
        walk_type(w, e->type);
        for (uint32_t j = 0; j < e->ability_count; j++)
            walk_proof(w, e->abilities[j].implementation);
    }
    for (uint32_t i = 0; i < body->binding_count; i++)
        walk_type(w, body->bindings[i].type);
}

static void close_reach(struct walker *w) {
    const struct mo_program *p = w->program;
    for (uint32_t i = 0; i < w->reach_count && w->ok; i++) {
        uint32_t index = w->reach[i];
        if (w->functions[index] != UINT32_MAX) {
            const struct mo_checked_function *f = &p->functions[w->functions[index]];
            walk_type(w, f->signature->type);
            walk_body(w, &f->body);
        }
        if (w->constants[index] != UINT32_MAX) {
            const struct mo_checked_constant *c = &p->constants[w->constants[index]];
            walk_type(w, c->type);
            walk_body(w, &c->initializer);
        }
        if (w->data[index] != UINT32_MAX) {
            const struct mo_data_signature *d = &p->signatures->data[w->data[index]];
            for (uint32_t j = 0; j < d->count; j++)
                walk_type(w, d->fields[j].type);
        }
    }
}

static const struct mo_syntax *find(const struct mo_syntax *n, struct mo_span span,
                                    enum mo_syntax_kind kind) {
    if (n == NULL || n->span.start > span.start || n->span.end < span.end)
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

static struct mo_syntax *node(struct walker *w, struct mo_syntax value) {
    struct mo_syntax *out = array(w, 1, sizeof *out, _Alignof(struct mo_syntax));
    if (out != NULL)
        *out = value;
    return out;
}

static struct mo_syntax *bare(struct walker *w, const struct mo_syntax *n) {
    if (n == NULL || !charge(w))
        return NULL;
    struct mo_syntax *out = node(w, *n);
    if (out == NULL)
        return NULL;
    out->leading = out->trailing = out->ending = NULL;
    out->blank_before = false;
    if (n->count == 0)
        return out;
    out->children = array(w, n->count, sizeof *out->children, _Alignof(struct mo_syntax *));
    for (uint32_t i = 0; out->children != NULL && i < n->count; i++)
        out->children[i] =
            n->kind == MO_S_FN && i == 4
                ? node(w, (struct mo_syntax){.kind = MO_S_SEQUENCE, .span = n->children[4]->span})
                : bare(w, n->children[i]);
    return w->ok ? out : NULL;
}

static uint32_t display_width(const uint8_t *bytes, size_t length) {
    uint64_t width;
    MO_ASSERT(mo_unicode16_display_width(bytes, length, &width));
    MO_ASSERT(width <= UINT32_MAX);
    return (uint32_t)width;
}

static struct text canonical(struct walker *w, const struct mo_syntax *original) {
    struct text out = {NULL, 0};
    uint32_t file = original->span.file;
    if (w->parsed[file] == NULL) {
        w->parsed[file] = mo_parse(w->arena, w->program->set->sources, file, &w->diags);
        if (w->parsed[file] == NULL || mo_diags_any(&w->diags)) {
            w->ok = false;
            return out;
        }
    }
    const struct mo_syntax *found = find(w->parsed[file]->tree, original->span, original->kind);
    MO_ASSERT(found != NULL);
    struct mo_syntax *item = bare(w, found);
    struct mo_syntax *items =
        node(w, (struct mo_syntax){.kind = MO_S_SEQUENCE, .span = item->span});
    struct mo_syntax **pair = array(w, 2, sizeof *pair, _Alignof(struct mo_syntax *));
    if (!w->ok || item == NULL || items == NULL || pair == NULL)
        return out;
    items->count = 1;
    items->children = &item;
    pair[0] = NULL;
    pair[1] = items;
    struct mo_syntax tree = {.kind = MO_S_FILE, .span = item->span, .count = 2, .children = pair};
    struct mo_module_syntax module = {.span = item->span, .tree = &tree};
    struct mo_buffer formatted;
    mo_buffer_init(&formatted, MO_LIMIT_FILE_SIZE);
    if (mo_format(&module, (struct mo_format_options){100, display_width}, &formatted)) {
        out.bytes = mo_arena_copy(w->arena, formatted.bytes, formatted.length, 1);
        out.length = formatted.length;
    }
    if (out.bytes == NULL && formatted.length != 0)
        w->ok = false;
    mo_buffer_free(&formatted);
    if (out.bytes == NULL && formatted.length == 0)
        w->ok = false;
    return out;
}

static int by_name(const void *a, const void *b) {
    const struct entry *x = a, *y = b;
    int c = strcmp(x->module, y->module);
    if (c == 0)
        c = strcmp(x->name, y->name);
    if (c == 0) {
        size_t n = x->text.length < y->text.length ? x->text.length : y->text.length;
        c = memcmp(x->text.bytes, y->text.bytes, n);
    }
    if (c == 0)
        c = (x->text.length > y->text.length) - (x->text.length < y->text.length);
    return c;
}

static void field(struct mo_sha256 *h, const void *bytes, size_t length) {
    uint8_t prefix[8];
    for (uint32_t i = 0; i < 8; i++)
        prefix[i] = (uint8_t)((uint64_t)length >> (8 * i));
    mo_sha256_update(h, prefix, sizeof prefix);
    mo_sha256_update(h, bytes, length);
}

static void text_field(struct mo_sha256 *h, const char *text) {
    field(h, text, strlen(text));
}

static void digest(struct walker *w, const char *owner, const char *kind,
                   const struct mo_syntax *syntax, uint8_t out[32]) {
    close_reach(w);
    struct entry *entries = array(w, w->reach_count, sizeof *entries, _Alignof(struct entry));
    for (uint32_t i = 0; w->ok && i < w->reach_count; i++) {
        uint32_t index = w->reach[i];
        uint32_t m = 0;
        while (m + 1 < w->program->interfaces->count && w->offsets[m + 1] <= index)
            m++;
        const struct mo_symbol *symbol =
            &w->program->interfaces->scopes[m].symbols[index - w->offsets[m]];
        if (w->texts[index].bytes == NULL)
            w->texts[index] = canonical(w, symbol->declaration);
        entries[i] = (struct entry){w->program->set->names[m], symbol->name, w->texts[index]};
    }
    struct text own = w->ok ? canonical(w, syntax) : (struct text){NULL, 0};
    if (!w->ok)
        return;
    qsort(entries, w->reach_count, sizeof *entries, by_name);
    struct mo_sha256 h;
    mo_sha256_init(&h);
    uint8_t version = MO_INTENT_DIGEST_VERSION;
    mo_sha256_update(&h, &version, 1);
    text_field(&h, owner);
    text_field(&h, kind);
    field(&h, own.bytes, own.length);
    for (uint32_t i = 0; i < w->reach_count; i++) {
        text_field(&h, entries[i].module);
        text_field(&h, entries[i].name);
        field(&h, entries[i].text.bytes, entries[i].text.length);
    }
    mo_sha256_finish(&h, out);
}

static void begin(struct walker *w, const struct mo_syntax *subject) {
    w->mark++;
    w->reach_count = 0;
    w->subject = subject;
}

static const char *function_owner(struct walker *w, uint32_t module, const struct mo_symbol *f) {
    const struct mo_program *p = w->program;
    const struct mo_module_scope *scope = &p->interfaces->scopes[f->module];
    const struct mo_symbol *group = mo_find_symbol(scope, (const uint8_t *)f->name, f->name_length);
    MO_ASSERT(group != NULL);
    const char *receiver = f->receiver != UINT32_MAX && group->overload_count > 1
                               ? scope->symbols[f->receiver].name
                               : NULL;
    size_t length = strlen(p->set->names[module]) + 1 + strlen(f->name) +
                    (receiver != NULL ? 1 + strlen(receiver) : 0) + 1;
    char *out = array(w, length, 1, 1);
    if (out == NULL)
        return NULL;
    int n = receiver != NULL
                ? snprintf(out, length, "%s.%s@%s", p->set->names[module], f->name, receiver)
                : snprintf(out, length, "%s.%s", p->set->names[module], f->name);
    MO_ASSERT(n >= 0 && (size_t)n + 1 == length);
    return out;
}

static bool prepare(struct walker *w) {
    const struct mo_program *p = w->program;
    uint32_t modules = p->interfaces->count;
    w->offsets = array(w, modules, sizeof *w->offsets, _Alignof(uint32_t));
    for (uint32_t m = 0; w->ok && m < modules; m++) {
        w->offsets[m] = w->symbol_count;
        w->symbol_count += p->interfaces->scopes[m].count;
    }
    uint32_t n = w->symbol_count;
    w->functions = array(w, n, sizeof(uint32_t), _Alignof(uint32_t));
    w->constants = array(w, n, sizeof(uint32_t), _Alignof(uint32_t));
    w->data = array(w, n, sizeof(uint32_t), _Alignof(uint32_t));
    w->marks = array(w, n, sizeof(uint32_t), _Alignof(uint32_t));
    w->reach = array(w, n, sizeof(uint32_t), _Alignof(uint32_t));
    w->texts = array(w, n, sizeof *w->texts, _Alignof(struct text));
    w->parsed =
        array(w, p->set->sources->count, sizeof *w->parsed, _Alignof(struct mo_module_syntax *));
    if (!w->ok)
        return false;
    for (uint32_t i = 0; i < n; i++) {
        w->functions[i] = w->constants[i] = w->data[i] = UINT32_MAX;
        w->marks[i] = 0;
        w->texts[i] = (struct text){NULL, 0};
    }
    memset(w->parsed, 0, p->set->sources->count * sizeof *w->parsed);
    for (uint32_t i = 0; i < p->function_count; i++) {
        const struct mo_symbol *f = p->functions[i].signature->function;
        if (f != NULL && !library(p, f->module))
            w->functions[symbol_index(w, f)] = i;
    }
    for (uint32_t i = 0; i < p->constant_count; i++)
        if (!library(p, p->constants[i].symbol->module))
            w->constants[symbol_index(w, p->constants[i].symbol)] = i;
    for (uint32_t i = 0; i < p->signatures->data_count; i++) {
        const struct mo_symbol *d = p->signatures->data[i].type->declaration;
        if (d != NULL && !library(p, d->module))
            w->data[symbol_index(w, d)] = i;
    }
    return true;
}

bool mo_intent_items(struct mo_arena *arena, const struct mo_program *program,
                     struct mo_intent *out) {
    MO_ASSERT(program->set->sources != NULL);
    *out = (struct mo_intent){0};
    struct walker walker = {
        .arena = arena, .program = program, .work = MO_LIMIT_INTENT_WORK, .ok = true};
    struct walker *w = &walker;
    if (!mo_diags_init(&w->diags))
        return false;
    uint32_t count = 0;
    for (uint32_t i = 0; i < program->spec_count; i++) {
        const struct mo_checked_spec *spec = &program->specs[i];
        count += (spec->kind == MO_SPEC_PROMISE || spec->kind == MO_SPEC_EXAMPLE) &&
                 !library(program, spec->module);
    }
    for (uint32_t i = 0; i < program->service_count; i++)
        if (!library(program, program->services[i].module))
            count += program->services[i].promise_count;
    uint32_t *anchors = array(w, program->spec_count, sizeof *anchors, _Alignof(uint32_t));
    uint32_t *first = array(w, program->spec_count, sizeof *first, _Alignof(uint32_t));
    out->items = array(w, count, sizeof *out->items, _Alignof(struct mo_intent_item));
    if (!w->ok || !prepare(w))
        goto done;
    for (uint32_t i = 0; i < program->spec_count; i++)
        first[i] = UINT32_MAX;
    for (uint32_t i = program->spec_count; i-- > 0;)
        if (program->specs[i].kind == MO_SPEC_ANCHOR) {
            anchors[i] = first[program->specs[i].promise];
            first[program->specs[i].promise] = i;
        }
    for (uint32_t i = 0; w->ok && i < program->spec_count; i++) {
        const struct mo_checked_spec *spec = &program->specs[i];
        if ((spec->kind != MO_SPEC_PROMISE && spec->kind != MO_SPEC_EXAMPLE) ||
            library(program, spec->module))
            continue;
        const struct mo_symbol *subject = program->functions[spec->subject].signature->function;
        begin(w, subject->declaration);
        walk_body(w, &spec->body);
        for (uint32_t a = first[i]; a != UINT32_MAX; a = anchors[a])
            walk_body(w, &program->specs[a].body);
        const char *owner = function_owner(w, spec->module, subject);
        struct mo_intent_item *item = &out->items[out->count];
        item->spec = spec;
        if (owner != NULL)
            digest(w, owner, spec->kind == MO_SPEC_PROMISE ? "promise" : "example", spec->syntax,
                   item->digest);
        out->count++;
    }
    for (uint32_t i = 0; w->ok && i < program->service_count; i++) {
        const struct mo_checked_service *service = &program->services[i];
        if (library(program, service->module))
            continue;
        const char *module = program->set->names[service->module];
        size_t length = strlen(module) + sizeof ".service";
        char *owner = array(w, length, 1, 1);
        if (owner == NULL)
            break;
        int n = snprintf(owner, length, "%s.service", module);
        MO_ASSERT(n >= 0 && (size_t)n + 1 == length);
        for (uint32_t j = 0; w->ok && j < service->promise_count; j++) {
            const struct mo_checked_service_promise *promise = &service->promises[j];
            begin(w, NULL);
            walk_body(w, &promise->spec.body);
            for (uint32_t a = 0; a < promise->anchor_count; a++)
                walk_body(w, &promise->anchors[a].body);
            struct mo_intent_item *item = &out->items[out->count++];
            item->spec = &promise->spec;
            digest(w, owner, "service promise", promise->spec.syntax, item->digest);
        }
    }
done:
    mo_diags_free(&w->diags);
    if (w->ok)
        MO_ASSERT(out->count == count);
    return w->ok;
}
