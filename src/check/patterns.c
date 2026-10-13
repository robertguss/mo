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
    MO_ASSERT(size != 0);
    /* Retained matrix storage must be bounded by work, not merely by the
     * caller's arena. Charge bytes as well as the algorithm's visited cells. */
    if (count > MO_LIMIT_SOURCE_CHECK_WORK / size) {
        bool reported =
            problem(p, MO_CODE_CHECK_WORK, "the function needs too much pattern-checking work");
        (void)reported;
        return NULL;
    }
    if (!work(p, (uint32_t)(count * size)))
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

static void append(char *text, size_t *length, const char *bytes, size_t count);

static MO_MUST_USE bool head_present(struct coverage *p, const struct frame *f, uintptr_t key,
                                     bool *present) {
    if (!work(p, 1))
        return false;
    uint32_t lo = 0, hi = f->count;
    while (lo < hi) {
        if (!work(p, 1))
            return false;
        uint32_t mid = lo + (hi - lo) / 2;
        if (f->heads[mid].pattern->key < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    *present = lo < f->count && f->heads[lo].pattern->key == key;
    return true;
}

static MO_MUST_USE struct mo_match_pattern *missing_head(struct coverage *p, struct frame *f,
                                                         struct mo_match_pattern *first) {
    const char *name = "<another value>";
    uintptr_t key = 0;
    uint32_t count = 0;
    if (first->choice != NULL) {
        const struct mo_symbol *owner = first->choice;
        const struct mo_module_scope *scope = &p->interfaces->scopes[owner->module];
        char *text = allocate(p, 4096, 1, 1);
        if (text == NULL)
            return NULL;
        size_t length = 0;
        uint32_t missing = 0, shown = 0;
        for (uint32_t i = 0; i < owner->case_count; i++) {
            const struct mo_symbol *symbol = &scope->symbols[owner->cases[i]];
            bool present;
            if (!head_present(p, f, (uintptr_t)symbol, &present))
                return NULL;
            if (present)
                continue;
            missing++;
            if (shown < 8 && length < 3800) {
                if (shown != 0)
                    append(text, &length, " | ", 3);
                append(text, &length, symbol->name,
                       symbol->name_length > 128 ? 128 : symbol->name_length);
                if (symbol->name_length > 128)
                    append(text, &length, "...", 3);
                if (symbol->declaration->children[1] != NULL)
                    append(text, &length, "(..)", 4);
                shown++;
            }
        }
        if (missing > shown) {
            int n = snprintf(text + length, 4096 - length, " and %u more", missing - shown);
            MO_ASSERT(n > 0 && (size_t)n < 4096 - length);
            length += (size_t)n;
        }
        text[length] = '\0';
        name = text;
    } else if (mo_builtin_case_by_key(first->key) != NULL) {
        const struct mo_builtin_case *domain = mo_builtin_case_by_key(first->key);
        char *text = allocate(p, 4096, 1, 1);
        if (text == NULL)
            return NULL;
        size_t length = 0;
        for (uint32_t i = 0; i < domain->cases; i++) {
            uintptr_t candidate = domain->first_key + i;
            bool found;
            if (!head_present(p, f, candidate, &found))
                return NULL;
            if (!found) {
                const struct mo_builtin_case *missing = mo_builtin_case_by_key(candidate);
                MO_ASSERT(missing != NULL);
                if (length != 0)
                    append(text, &length, " | ", 3);
                append(text, &length, missing->name, strlen(missing->name));
                if (missing->count != 0)
                    append(text, &length, "(..)", 4);
            }
        }
        text[length] = '\0';
        name = text;
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
            name = f->count == 0 ? "false | true" : first->key == 1 ? "false" : "true";
        else if (first->key == 3 || first->key == 4) {
            name = f->count == 0 ? "[] | [_, .._]" : first->key == 3 ? "[_, .._]" : "[]";
            key = f->count == 0 ? 0 : first->key == 3 ? 4u : 3u;
            count = f->count == 0 ? 0 : first->key == 3 ? 2u : 0u;
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
                       struct mo_match_pattern *const *patterns, uint32_t count,
                       const struct mo_source_type *type, uint32_t *steps, uint64_t *program_work,
                       struct mo_diags *diags, struct mo_span span, const char **missing) {
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
    if (count == 0) {
        struct mo_match_pattern domain = {0};
        if (type->declaration != NULL && type->declaration->declaration->kind == MO_S_ENUM)
            domain.choice = type->declaration;
        else if (type->kind == MO_SOURCE_NAMED && type->module == UINT32_MAX) {
            for (uintptr_t key = 6; key <= 22; key++) {
                const struct mo_builtin_case *builtin = mo_builtin_case_by_key(key);
                if (builtin != NULL && strcmp(builtin->type, type->name) == 0) {
                    domain.key = key;
                    break;
                }
            }
            if (strcmp(type->name, "Bool") == 0)
                domain = (struct mo_match_pattern){.key = 1, .signature = 2};
            else if (strcmp(type->name, "List") == 0)
                domain = (struct mo_match_pattern){.key = 3, .signature = 2};
        }
        if (domain.key != 0 || domain.choice != NULL) {
            struct frame empty = {0};
            struct mo_match_pattern *hole = missing_head(&p, &empty, &domain);
            if (hole == NULL)
                return false;
            struct cell witness = {.pattern = hole};
            *missing = render(&p, &witness);
            return !p.failed;
        }
        *missing = "every value";
        return true;
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
                struct mo_match_pattern *chosen = f->complete || f->count == 0
                                                      ? f->chosen
                                                      : missing_head(&p, f, f->heads[0].pattern);
                if (p.failed)
                    return false;
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
