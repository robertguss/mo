#include "check/patterns.h"
#include "check/data.h"

#include "base/codes.h"
#include "base/limits.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cell {
    struct mo_match_pattern *pattern;
    struct cell *next;
};

struct row {
    struct cell *cells;
    struct row *next;
};

struct head {
    struct mo_match_pattern *pattern;
    struct cell *rest;
};

struct frame {
    struct frame *parent;
    struct row *defaults;
    struct head *heads;
    uint32_t count, at, end, width;
    struct mo_match_pattern *chosen;
    bool complete;
};

struct coverage {
    struct mo_arena *arena;
    const struct mo_module_interfaces *interfaces;
    struct mo_diags *diags;
    struct mo_span span;
    uint32_t *work;
    uint64_t *program_work;
    bool failed;
};

static MO_MUST_USE bool problem(struct coverage *p, uint16_t code, const char *message) {
    if (!p->failed) {
        bool kept = mo_diags_add(
            p->diags,
            &(struct mo_diag){.code = code, .primary = {p->span, message}, .message = message});
        (void)kept;
    }
    p->failed = true;
    return false;
}

static MO_MUST_USE bool work(struct coverage *p, uint32_t amount) {
    if (p->failed)
        return false;
    if (amount > MO_LIMIT_SOURCE_CHECK_WORK - *p->work)
        return problem(p, MO_CODE_CHECK_WORK, "the function needs too much pattern-checking work");
    if (amount > MO_LIMIT_PROGRAM_CHECK_WORK - *p->program_work)
        return problem(p, MO_CODE_CHECK_PROGRAM_WORK,
                       "the program needs too much pattern-checking work");
    *p->work += amount;
    *p->program_work += amount;
    return true;
}

static MO_MUST_USE void *allocate(struct coverage *p, size_t count, size_t size, size_t alignment) {
    if (p->failed)
        return NULL;
    void *out = mo_arena_allocate_array(p->arena, count, size, alignment);
    if (out == NULL) {
        bool reported = problem(p, MO_CODE_CHECK_NO_MEMORY, "the checking arena has no room");
        (void)reported;
    }
    return out;
}

static MO_MUST_USE struct cell *prepend(struct coverage *p, struct mo_match_pattern *pattern,
                                        struct cell *rest) {
    if (!work(p, 1))
        return NULL;
    struct cell *out = allocate(p, 1, sizeof(*out), _Alignof(struct cell));
    if (out != NULL)
        *out = (struct cell){pattern, rest};
    return out;
}

static MO_MUST_USE bool add_row(struct coverage *p, struct row **rows, struct cell *cells) {
    if (!work(p, 1))
        return false;
    struct row *out = allocate(p, 1, sizeof(*out), _Alignof(struct row));
    if (out == NULL)
        return false;
    *out = (struct row){cells, *rows};
    *rows = out;
    return true;
}

static MO_MUST_USE int compare_heads(const void *a, const void *b) {
    const struct head *x = a, *y = b;
    return x->pattern->key < y->pattern->key ? -1 : x->pattern->key > y->pattern->key ? 1 : 0;
}

static MO_MUST_USE struct mo_match_pattern *missing_head(struct coverage *p, struct frame *f) {
    struct mo_match_pattern *first = f->heads[0].pattern;
    const char *name = "<another value>";
    uintptr_t key = 0;
    uint32_t count = 0;
    if (first->choice != NULL) {
        const struct mo_symbol *owner = first->choice;
        const struct mo_module_scope *scope = &p->interfaces->scopes[owner->module];
        /* Mark cases once, rather than scanning the matrix for each case. */
        bool *seen = allocate(p, scope->count, sizeof(*seen), _Alignof(bool));
        if (seen == NULL || !work(p, scope->count))
            return NULL;
        memset(seen, 0, scope->count * sizeof(*seen));
        for (uint32_t i = 0; i < f->count; i++) {
            const struct mo_symbol *symbol = (const struct mo_symbol *)f->heads[i].pattern->key;
            MO_ASSERT(symbol >= scope->symbols && symbol < scope->symbols + scope->count);
            seen[(size_t)(symbol - scope->symbols)] = true;
        }
        for (uint32_t i = 0; i < owner->case_count; i++)
            if (!seen[owner->cases[i]]) {
                const struct mo_symbol *symbol = &scope->symbols[owner->cases[i]];
                name = symbol->name;
                if (symbol->declaration->children[1] != NULL) {
                    size_t length = strlen(name);
                    char *payload = allocate(p, length + 5, 1, 1);
                    if (payload == NULL)
                        return NULL;
                    memcpy(payload, name, length);
                    memcpy(payload + length, "(..)", 5);
                    name = payload;
                }
                break;
            }
    } else if (mo_builtin_case_by_key(first->key) != NULL) {
        const struct mo_builtin_case *domain = mo_builtin_case_by_key(first->key);
        for (uint32_t i = 0; i < domain->cases; i++) {
            uintptr_t candidate = domain->first_key + i;
            bool found = false;
            for (uint32_t j = 0; j < f->count; j++)
                found = found || f->heads[j].pattern->key == candidate;
            if (!found) {
                const struct mo_builtin_case *missing = mo_builtin_case_by_key(candidate);
                MO_ASSERT(missing != NULL);
                name = missing->name;
                key = missing->key;
                count = missing->count;
                break;
            }
        }
    } else if (first->signature == 256) {
        bool seen[256] = {false};
        for (uint32_t i = 0; i < f->count; i++) {
            MO_ASSERT(f->heads[i].pattern->key >= 256 && f->heads[i].pattern->key < 512);
            seen[f->heads[i].pattern->key - 256] = true;
        }
        for (uint32_t i = 0; i < 256; i++)
            if (!seen[i]) {
                char *number = allocate(p, 16, 1, 1);
                if (number == NULL)
                    return NULL;
                int n = snprintf(number, 16, "%u", i);
                MO_ASSERT(n > 0 && n < 16);
                name = number;
                break;
            }
    } else if (first->signature == 2) {
        if (first->key == 1 || first->key == 2)
            name = first->key == 1 ? "false" : "true";
        else if (first->key == 3 || first->key == 4) {
            name = first->key == 3 ? "[_, .._]" : "[]";
            key = first->key == 3 ? 4u : 3u;
            count = first->key == 3 ? 2u : 0u;
        }
    }
    struct mo_match_pattern *out = allocate(p, 1, sizeof(*out), _Alignof(struct mo_match_pattern));
    struct mo_match_pattern **items =
        allocate(p, count, sizeof(*items), _Alignof(struct mo_match_pattern *));
    if (p->failed)
        return NULL;
    memset(items, 0, count * sizeof(*items));
    *out = (struct mo_match_pattern){.name = name, .key = key, .count = count, .items = items};
    return out;
}

static MO_MUST_USE struct frame *partition(struct coverage *p, struct row *rows, uint32_t width,
                                           struct frame *parent) {
    uint32_t count = 0;
    for (struct row *r = rows; r != NULL; r = r->next) {
        if (!work(p, 1))
            return NULL;
        count += r->cells->pattern != NULL ? 1u : 0u;
    }
    struct frame *f = allocate(p, 1, sizeof(*f), _Alignof(struct frame));
    struct head *heads = allocate(p, count, sizeof(*heads), _Alignof(struct head));
    if (p->failed)
        return NULL;
    *f = (struct frame){.parent = parent, .heads = heads, .count = count, .width = width};
    uint32_t at = 0;
    for (struct row *r = rows; r != NULL; r = r->next) {
        if (r->cells->pattern == NULL) {
            if (!add_row(p, &f->defaults, r->cells->next))
                return NULL;
        } else
            heads[at++] = (struct head){r->cells->pattern, r->cells->next};
    }
    if (count == 0)
        return f;
    for (uint32_t n = count; n != 0; n >>= 1)
        if (!work(p, count))
            return NULL;
    qsort(heads, count, sizeof(*heads), compare_heads);
    uint32_t distinct = 1;
    for (uint32_t i = 1; i < count; i++)
        distinct += heads[i].pattern->key != heads[i - 1].pattern->key ? 1u : 0u;
    f->complete = distinct == heads[0].pattern->signature;
    if (!f->complete)
        f->chosen = missing_head(p, f);
    return p->failed ? NULL : f;
}

static MO_MUST_USE struct cell *specialize_cells(struct coverage *p,
                                                 const struct mo_match_pattern *pattern,
                                                 uint32_t arity, struct cell *rest) {
    for (uint32_t i = arity; i != 0; i--) {
        rest = prepend(p, pattern == NULL ? NULL : pattern->items[i - 1], rest);
        if (rest == NULL)
            return NULL;
    }
    return rest;
}

static MO_MUST_USE struct row *specialize(struct coverage *p, struct frame *f) {
    f->chosen = f->heads[f->at].pattern;
    f->end = f->at + 1;
    while (f->end < f->count && f->heads[f->end].pattern->key == f->chosen->key)
        f->end++;
    struct row *out = NULL;
    for (uint32_t i = f->at; i < f->end; i++) {
        struct cell *cells =
            specialize_cells(p, f->heads[i].pattern, f->chosen->count, f->heads[i].rest);
        if (p->failed || !add_row(p, &out, cells))
            return NULL;
    }
    for (struct row *r = f->defaults; r != NULL; r = r->next) {
        struct cell *cells = specialize_cells(p, NULL, f->chosen->count, r->cells);
        if (p->failed || !add_row(p, &out, cells))
            return NULL;
    }
    return out;
}

struct rendering {
    struct mo_match_pattern *pattern;
    uint32_t at;
    bool opened;
    struct rendering *parent;
};

static void append(char *text, size_t *length, const char *bytes, size_t count) {
    size_t room = *length < 4000 ? 4000 - *length : 0;
    if (count > room)
        count = room;
    memcpy(text + *length, bytes, count);
    *length += count;
}

/* The preview is bounded and is never a proposed edit. Scalar domains use a
 * schematic <another value>; constructor and product holes retain their path. */
static MO_MUST_USE const char *render(struct coverage *p, struct cell *witness) {
    char *text = allocate(p, 4096, 1, 1);
    struct rendering *stack = allocate(p, 1, sizeof(*stack), _Alignof(struct rendering));
    if (p->failed)
        return NULL;
    MO_ASSERT(witness != NULL && witness->next == NULL);
    *stack = (struct rendering){.pattern = witness->pattern};
    size_t length = 0;
    while (stack != NULL && length < 4000) {
        if (!work(p, 1))
            return NULL;
        struct mo_match_pattern *pattern = stack->pattern;
        if (pattern == NULL || pattern->count == 0) {
            const char *name = pattern == NULL ? "_" : pattern->name;
            append(text, &length, name, strlen(name));
            stack = stack->parent;
            continue;
        }
        if (!stack->opened) {
            if (pattern->key != 4 && pattern->key != 5)
                append(text, &length, pattern->name, strlen(pattern->name));
            append(text, &length, pattern->key == 4 ? "[" : "(", 1);
            stack->opened = true;
        }
        if (stack->at == pattern->count) {
            append(text, &length, pattern->key == 4 ? "]" : ")", 1);
            stack = stack->parent;
            continue;
        }
        if (pattern->key == 4 && stack->at == 1) {
            struct mo_match_pattern *tail = pattern->items[1];
            if (tail == NULL || tail->key == 3) {
                if (tail == NULL)
                    append(text, &length, ", .._", 5);
                stack->at = 2;
                continue;
            }
            MO_ASSERT(tail->key == 4);
            append(text, &length, ", ", 2);
            stack->pattern = tail;
            stack->at = 0;
            continue;
        }
        if (stack->at != 0)
            append(text, &length, ", ", 2);
        if (pattern->data != NULL && !pattern->data->unnamed) {
            const struct mo_syntax *field = pattern->data->by_index[stack->at]->name;
            append(text, &length, (const char *)field->text, field->span.end - field->span.start);
            append(text, &length, ": ", 2);
        }
        const struct mo_builtin_case *builtin = mo_builtin_case_by_key(pattern->key);
        if (builtin != NULL && builtin->fields[0] != NULL) {
            append(text, &length, builtin->fields[stack->at], strlen(builtin->fields[stack->at]));
            append(text, &length, ": ", 2);
        }
        struct rendering *child = allocate(p, 1, sizeof(*child), _Alignof(struct rendering));
        if (child == NULL)
            return NULL;
        *child = (struct rendering){.pattern = pattern->items[stack->at++], .parent = stack};
        stack = child;
    }
    if (stack != NULL) {
        memcpy(text + length, "...", 3);
        length += 3;
    }
    text[length] = '\0';
    return text;
}

bool mo_patterns_cover(struct mo_arena *arena, const struct mo_module_interfaces *interfaces,
                       struct mo_match_pattern *const *patterns, uint32_t count, uint32_t *steps,
                       uint64_t *program_work, struct mo_diags *diags, struct mo_span span,
                       const char **missing) {
    struct coverage p = {.arena = arena,
                         .interfaces = interfaces,
                         .diags = diags,
                         .span = span,
                         .work = steps,
                         .program_work = program_work};
    struct row *rows = NULL;
    *missing = NULL;
    for (uint32_t i = 0; i < count; i++) {
        struct cell *cells = prepend(&p, patterns[i], NULL);
        if (cells == NULL || !add_row(&p, &rows, cells))
            return false;
    }
    uint32_t width = 1;
    struct frame *stack = NULL;
    for (;;) {
        if (!work(&p, 1))
            return false;
        if (rows == NULL) {
            struct cell *witness = NULL;
            for (uint32_t i = 0; i < width; i++) {
                witness = prepend(&p, NULL, witness);
                if (witness == NULL)
                    return false;
            }
            for (struct frame *f = stack; f != NULL; f = f->parent) {
                struct mo_match_pattern *chosen = f->chosen;
                if (f->complete) {
                    struct mo_match_pattern *node =
                        allocate(&p, 1, sizeof(*node), _Alignof(struct mo_match_pattern));
                    struct mo_match_pattern **items = allocate(&p, chosen->count, sizeof(*items),
                                                               _Alignof(struct mo_match_pattern *));
                    if (p.failed || !work(&p, chosen->count))
                        return false;
                    *node = *chosen;
                    node->items = items;
                    for (uint32_t i = 0; i < chosen->count; i++) {
                        MO_ASSERT(witness != NULL);
                        items[i] = witness->pattern;
                        witness = witness->next;
                    }
                    chosen = node;
                }
                witness = prepend(&p, chosen, witness);
                if (witness == NULL)
                    return false;
            }
            *missing = render(&p, witness);
            return !p.failed;
        }
        if (width != 0) {
            struct frame *f = partition(&p, rows, width, stack);
            if (f == NULL)
                return false;
            stack = f;
            if (!f->complete) {
                rows = f->defaults;
                width--;
                continue;
            }
            rows = specialize(&p, f);
            width = f->width - 1 + f->chosen->count;
            continue;
        }
        while (stack != NULL) {
            struct frame *f = stack;
            if (f->complete && f->end < f->count) {
                f->at = f->end;
                rows = specialize(&p, f);
                width = f->width - 1 + f->chosen->count;
                break;
            }
            stack = f->parent;
        }
        if (stack == NULL)
            return true;
    }
}
