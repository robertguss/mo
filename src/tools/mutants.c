#include "tools/mutants.h"

#include "base/buffer.h"
#include "base/limits.h"

#include <stdlib.h>
#include <string.h>

struct swap {
    enum mo_syntax_token from;
    const char *to;
};

static const struct row {
    const char *kind;
    struct swap swaps[6];
} rows[] = {
    {"boundary", {{MO_T_LT, "<="}, {MO_T_LE, "<"}, {MO_T_GT, ">="}, {MO_T_GE, ">"}}},
    {"negate",
     {{MO_T_EQ, "!="},
      {MO_T_NE, "=="},
      {MO_T_LT, ">="},
      {MO_T_LE, ">"},
      {MO_T_GT, "<="},
      {MO_T_GE, "<"}}},
    {"logic", {{MO_T_AND, "or"}, {MO_T_OR, "and"}}},
    {"not", {{MO_T_NOT, ""}}},
    {"arithmetic", {{MO_T_PLUS, "-"}, {MO_T_MINUS, "+"}, {MO_T_STAR, "/"}, {MO_T_SLASH, "*"}}},
    {"bool", {{MO_T_TRUE, "false"}, {MO_T_FALSE, "true"}}},
    {"int", {{MO_T_INT, NULL}}},
};

static const struct {
    enum mo_syntax_token token;
    const char *text;
} spellings[] = {
    {MO_T_EQ, "=="},  {MO_T_NE, "!="},   {MO_T_LT, "<"},    {MO_T_LE, "<="},
    {MO_T_GT, ">"},   {MO_T_GE, ">="},   {MO_T_AND, "and"}, {MO_T_OR, "or"},
    {MO_T_PLUS, "+"}, {MO_T_MINUS, "-"}, {MO_T_STAR, "*"},  {MO_T_SLASH, "/"},
};

struct walk {
    struct mo_arena *arena;
    const struct mo_sources *sources;
    const struct mo_source *source;
    uint32_t function, module;
    uint64_t count;
    struct mo_mutant *items;
    bool failed;
};

static char *copy(struct mo_arena *arena, const uint8_t *bytes, size_t length) {
    char *text = mo_arena_allocate(arena, length + 1, 1);
    if (text != NULL) {
        memcpy(text, bytes, length);
        text[length] = '\0';
    }
    return text;
}

static int value_of(uint8_t c) {
    return c <= '9' ? c - '0' : c >= 'a' ? c - 'a' + 10 : c - 'A' + 10;
}

static char *raise(struct mo_arena *arena, const uint8_t *text, uint32_t length) {
    uint32_t prefix = length > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'b') ? 2 : 0;
    int base = prefix == 0 ? 10 : text[1] == 'x' ? 16 : 2;
    bool lower = false;
    for (uint32_t i = prefix; i < length; i++)
        lower = lower || (text[i] >= 'a' && text[i] <= 'f');
    char *out = mo_arena_allocate(arena, length + 2, 1);
    if (out == NULL)
        return NULL;
    memcpy(out + 1, text, length);
    out[length + 1] = '\0';
    bool carry = true;
    for (uint32_t i = length; carry && i > prefix; i--) {
        if (out[i] == '_')
            continue;
        int value = value_of((uint8_t)out[i]) + 1;
        carry = value == base;
        value = carry ? 0 : value;
        out[i] = (char)(value < 10 ? '0' + value : (lower ? 'a' : 'A') + value - 10);
    }
    if (!carry)
        return out + 1;
    memmove(out, out + 1, prefix);
    out[prefix] = '1';
    return out;
}

static void site(struct walk *w, enum mo_syntax_token token, struct mo_span span) {
    for (size_t r = 0; r < sizeof rows / sizeof *rows; r++)
        for (size_t s = 0;
             s < sizeof rows[r].swaps / sizeof *rows[r].swaps && rows[r].swaps[s].from != MO_T_EOF;
             s++) {
            if (rows[r].swaps[s].from != token)
                continue;
            if (w->items != NULL) {
                const uint8_t *at = w->source->bytes + span.start;
                uint32_t length = span.end - span.start;
                const char *to = rows[r].swaps[s].to;
                struct mo_mutant *m = &w->items[w->count];
                *m = (struct mo_mutant){w->function,
                                        w->module,
                                        span,
                                        mo_sources_position(w->sources, span.file, span.start),
                                        rows[r].kind,
                                        copy(w->arena, at, length),
                                        to != NULL ? to : raise(w->arena, at, length)};
                w->failed = w->failed || m->original == NULL || m->replacement == NULL;
            }
            w->count++;
        }
}

static bool listed(enum mo_syntax_token token) {
    for (size_t i = 0; i < sizeof spellings / sizeof *spellings; i++)
        if (spellings[i].token == token)
            return true;
    return false;
}

static struct mo_span operator(const struct walk *w, const struct mo_syntax *n) {
    const uint8_t *bytes = w->source->bytes;
    uint32_t at = n->children[0]->span.end, end = n->children[1]->span.start;
    while (at < end && (bytes[at] == ' ' || bytes[at] == '\t' || bytes[at] == '\n' ||
                        bytes[at] == '\r' || bytes[at] == '#'))
        if (bytes[at++] == '#')
            while (at < end && bytes[at] != '\n' && bytes[at] != '\r')
                at++;
    size_t i = 0;
    while (spellings[i].token != n->token)
        i++;
    size_t length = strlen(spellings[i].text);
    MO_ASSERT(at + length <= end && memcmp(bytes + at, spellings[i].text, length) == 0);
    return (struct mo_span){n->span.file, at, at + (uint32_t)length};
}

static void walk(struct walk *w, const struct mo_syntax *n) {
    if (n == NULL)
        return;
    enum mo_syntax_kind kind = n->kind;
    if (kind == MO_S_TRUE || kind == MO_S_FALSE || kind == MO_S_INT) {
        site(w, n->token, n->span);
        return;
    }
    if (kind == MO_S_BINARY) {
        walk(w, n->children[0]);
        if (listed(n->token))
            site(w, n->token, operator(w, n));
        walk(w, n->children[1]);
        return;
    }
    if (kind == MO_S_BINARY_CHAIN) {
        for (uint32_t i = 0; i < n->count; i++)
            if (i % 2 == 0)
                walk(w, n->children[i]);
            else
                site(w, n->children[i]->token, n->children[i]->span);
        return;
    }
    uint32_t from = kind == MO_S_ARM ? 1 : 0;
    uint32_t to = kind == MO_S_IS || kind == MO_S_POSITION ? n->count - 1 : n->count;
    if (kind == MO_S_NOT) {
        MO_ASSERT(memcmp(w->source->bytes + n->span.start, "not", 3) == 0);
        site(w, MO_T_NOT, (struct mo_span){n->span.file, n->span.start, n->span.start + 3});
    }
    for (uint32_t i = from; i < to; i++)
        walk(w, n->children[i]);
}

struct place {
    uint32_t function, module, start;
};

static int by_place(const void *a, const void *b) {
    const struct place *x = a, *y = b;
    if (x->module != y->module)
        return (x->module > y->module) - (x->module < y->module);
    return (x->start > y->start) - (x->start < y->start);
}

static void walk_all(struct walk *w, const struct mo_program *program, const struct place *places,
                     uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        const struct mo_syntax *fn =
            program->functions[places[i].function].signature->function->declaration;
        w->function = places[i].function;
        w->module = places[i].module;
        w->source = &w->sources->files[fn->span.file];
        walk(w, fn->children[5]);
    }
}

bool mo_module_tested(const struct mo_program *program, uint32_t module) {
    return module == 0 || (!program->set->library[module] &&
                           (program->set->dependency == NULL || !program->set->dependency[module]));
}

const char *mo_list_mutants(struct mo_arena *arena, const struct mo_program *program,
                            const struct mo_sources *sources, struct mo_mutants *out) {
    *out = (struct mo_mutants){0};
    const char *resources = "mutants exceed tool resources";
    struct place *places =
        mo_arena_allocate_array(arena, program->function_count == 0 ? 1 : program->function_count,
                                sizeof *places, _Alignof(struct place));
    if (places == NULL)
        return resources;
    uint32_t count = 0;
    for (uint32_t i = 0; i < program->function_count; i++) {
        const struct mo_symbol *symbol = program->functions[i].signature->function;
        if (mo_module_tested(program, symbol->module) && symbol->declaration != NULL &&
            symbol->declaration->kind == MO_S_FN)
            places[count++] = (struct place){i, symbol->module, symbol->declaration->span.start};
    }
    qsort(places, count, sizeof *places, by_place);
    struct walk w = {.arena = arena, .sources = sources};
    walk_all(&w, program, places, count);
    if (w.count > MO_LIMIT_MUTANTS)
        return "the program has more mutants than mo test makes";
    w.items = mo_arena_allocate_array(arena, w.count == 0 ? 1 : (size_t)w.count, sizeof *w.items,
                                      _Alignof(struct mo_mutant));
    if (w.items == NULL)
        return resources;
    uint64_t total = w.count;
    w.count = 0;
    walk_all(&w, program, places, count);
    MO_ASSERT(w.count == total);
    if (w.failed)
        return resources;
    *out = (struct mo_mutants){(uint32_t)w.count, w.items};
    return NULL;
}

bool mo_mutant_sources(const struct mo_program *program, const struct mo_sources *sources,
                       const struct mo_mutant *mutant, struct mo_sources *out) {
    MO_ASSERT(out->count == 0);
    const struct mo_module_set *set = program->set;
    const struct mo_source *changed = &sources->files[mutant->span.file];
    struct mo_buffer bytes;
    mo_buffer_init(&bytes, MO_LIMIT_FILE_SIZE);
    bool ok = mo_buffer_append(&bytes, changed->bytes, mutant->span.start) &&
              mo_buffer_append_text(&bytes, mutant->replacement) &&
              mo_buffer_append(&bytes, changed->bytes + mutant->span.end,
                               changed->length - mutant->span.end);
    for (uint32_t k = 0; ok && k < set->count; k++) {
        const struct mo_source *f = &sources->files[set->modules[k]->span.file];
        uint32_t index;
        struct mo_source_error error;
        ok = k == mutant->module
                 ? mo_sources_add_bytes(out, f->path, bytes.bytes, bytes.length, &index, &error)
                 : mo_sources_add_bytes(out, f->path, f->bytes, f->length, &index, &error);
        MO_ASSERT(!ok || index == k);
    }
    mo_buffer_free(&bytes);
    return ok;
}
