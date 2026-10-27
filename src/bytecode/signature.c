#include "bytecode/signature.h"

#include <string.h>

struct edge {
    uint32_t parent, next;
};

uint8_t *mo_bytecode_abilities(struct mo_arena *arena, const struct mo_type *types,
                               uint32_t type_count) {
    size_t count = 0;
    for (uint32_t i = 0; i < type_count; i++) {
        const struct mo_type *t = &types[i];
        count += t->parameter_count + t->field_count;
        for (uint32_t j = 0; j < t->case_count; j++) {
            count += t->cases[j].field_count;
        }
    }
    if (count > UINT32_MAX) {
        return NULL;
    }
    struct edge *edges =
        mo_arena_allocate_array(arena, count, sizeof *edges, _Alignof(struct edge));
    uint32_t *heads = mo_arena_allocate_array(arena, type_count, sizeof *heads, _Alignof(uint32_t));
    uint32_t *queue =
        mo_arena_allocate_array(arena, (size_t)type_count * 2, sizeof *queue, _Alignof(uint32_t));
    uint8_t *flags = mo_arena_allocate_array(arena, type_count, sizeof *flags, _Alignof(uint8_t));
    if (edges == NULL || heads == NULL || queue == NULL || flags == NULL) {
        return NULL;
    }
    uint8_t both = MO_ABILITY_EQUAL | MO_ABILITY_HASH;
    uint32_t end = 0, n = 0;
    for (uint32_t i = 0; i < type_count; i++) {
        heads[i] = MO_NO_INDEX;
    }
    for (uint32_t i = 0; i < type_count; i++) {
        const struct mo_type *t = &types[i];
        flags[i] = both;
        if (t->kind == MO_TYPE_FUNCTION || t->kind == MO_TYPE_ENV ||
            t->kind == MO_TYPE_CAPABILITY) {
            flags[i] = 0;
        } else if (t->kind == MO_TYPE_FLOAT) {
            flags[i] = MO_ABILITY_EQUAL;
        }
        if (flags[i] != both) {
            queue[end++] = i;
        }
        uint32_t total = t->parameter_count + t->field_count;
        for (uint32_t j = 0; j < total; j++) {
            uint32_t child =
                j < t->parameter_count ? t->parameters[j] : t->fields[j - t->parameter_count].type;
            MO_ASSERT(n < count && child < type_count);
            edges[n] = (struct edge){i, heads[child]};
            heads[child] = n++;
        }
        for (uint32_t j = 0; j < t->case_count; j++) {
            for (uint32_t k = 0; k < t->cases[j].field_count; k++) {
                uint32_t child = t->cases[j].fields[k].type;
                MO_ASSERT(n < count && child < type_count);
                edges[n] = (struct edge){i, heads[child]};
                heads[child] = n++;
            }
        }
    }
    for (uint32_t start = 0; start < end; start++) {
        uint32_t child = queue[start];
        for (uint32_t e = heads[child]; e != MO_NO_INDEX; e = edges[e].next) {
            uint32_t parent = edges[e].parent;
            uint8_t after = flags[parent] & flags[child];
            if (after != flags[parent]) {
                flags[parent] = after;
                MO_ASSERT(end < 2U * type_count);
                queue[end++] = parent;
            }
        }
    }
    return flags;
}

static MO_MUST_USE bool cases(const struct mo_type *t, const char *const *names, uint32_t n) {
    if (t->kind != MO_TYPE_CHOICE || t->case_count != n) {
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (strcmp(t->cases[i].name, names[i]) != 0) {
            return false;
        }
    }
    return true;
}

bool mo_bytecode_ordering(const struct mo_type *t) {
    static const char *const names[] = {"Less", "Equal", "Greater"};
    return cases(t, names, 3) && !t->has_fields;
}

struct match {
    const struct mo_bytecode *file;
    const struct mo_verify_function *v;
    const char *cursor;
    uint32_t variables[4]; /* T, U, K, V */
};

static void spaces(struct match *m) {
    while (*m->cursor == ' ') {
        m->cursor++;
    }
}
static MO_MUST_USE bool take(struct match *m, char c) {
    spaces(m);
    if (*m->cursor != c) {
        return false;
    }
    m->cursor++;
    return true;
}
static MO_MUST_USE bool word_is(const char *p, size_t n, const char *word) {
    return strlen(word) == n && memcmp(p, word, n) == 0;
}
static MO_MUST_USE int variable(const char *p, size_t n) {
    if (n != 1) {
        return -1;
    }
    return *p == 'T' ? 0 : *p == 'U' ? 1 : *p == 'K' ? 2 : *p == 'V' ? 3 : -1;
}

static MO_MUST_USE bool layouts(const struct match *m, uint32_t index) {
    const struct mo_type *t = &m->file->types[index];
    if (t->kind == MO_TYPE_TUPLE) {
        return mo_verify_layout_lookup(m->v->layouts, m->file->layout_count, index, MO_NO_INDEX) !=
               MO_NO_INDEX;
    }
    for (uint32_t i = 0; i < t->case_count; i++) {
        if (mo_case_shape(m->v->types, index, i) == MO_CASE_BOXED &&
            mo_verify_layout_lookup(m->v->layouts, m->file->layout_count, index, i) ==
                MO_NO_INDEX) {
            return false;
        }
    }
    return true;
}

/* Recursion follows only the fixed, trusted templates in the .def files, never the type graph. */
static MO_MUST_USE bool type(struct match *m, uint32_t index, unsigned depth) {
    MO_ASSERT(depth < MO_LIMIT_DEF_SIGNATURE_DEPTH && index < m->file->type_count);
    const struct mo_type *t = &m->file->types[index];
    spaces(m);
    if (*m->cursor == '(') {
        m->cursor++;
        if (t->kind != MO_TYPE_TUPLE || !layouts(m, index)) {
            return false;
        }
        for (uint32_t i = 0; i < t->parameter_count; i++) {
            if ((i != 0 && !take(m, ',')) || !type(m, t->parameters[i], depth + 1)) {
                return false;
            }
        }
        return take(m, ')');
    }
    const char *start = m->cursor;
    while ((*m->cursor >= 'A' && *m->cursor <= 'Z') || (*m->cursor >= 'a' && *m->cursor <= 'z')) {
        m->cursor++;
    }
    size_t length = (size_t)(m->cursor - start);
    int v = variable(start, length);
    if (v >= 0) {
        if (m->variables[v] == MO_NO_INDEX) {
            m->variables[v] = index;
        }
        return m->variables[v] == index;
    }
    static const struct {
        const char *name;
        enum mo_type_kind kind;
    } builtins[] = {
        {"Unit", MO_TYPE_UNIT},     {"Bool", MO_TYPE_BOOL},         {"Byte", MO_TYPE_BYTE},
        {"Int", MO_TYPE_INT},       {"Float", MO_TYPE_FLOAT},       {"Char", MO_TYPE_CHAR},
        {"Time", MO_TYPE_TIME},     {"Duration", MO_TYPE_DURATION}, {"Decimal", MO_TYPE_DECIMAL},
        {"Range", MO_TYPE_RANGE},   {"Text", MO_TYPE_TEXT},         {"Bytes", MO_TYPE_BYTES},
        {"BigInt", MO_TYPE_BIGINT}, {"List", MO_TYPE_LIST},         {"Set", MO_TYPE_SET},
        {"Map", MO_TYPE_MAP}};
    for (size_t i = 0; i < sizeof builtins / sizeof *builtins; i++) {
        if (word_is(start, length, builtins[i].name)) {
            if (t->kind != builtins[i].kind) {
                return false;
            }
            if (t->parameter_count == 0) {
                return true;
            }
            if (!take(m, '(')) {
                return false;
            }
            for (uint32_t j = 0; j < t->parameter_count; j++) {
                if ((j != 0 && !take(m, ',')) || !type(m, t->parameters[j], depth + 1)) {
                    return false;
                }
            }
            return take(m, ')');
        }
    }
    static const char *const caps[] = {"Args",      "Env",   "Console", "Files",
                                       "ReadFiles", "Clock", "Random"};
    for (uint32_t i = 0; i < sizeof caps / sizeof *caps; i++) {
        if (word_is(start, length, caps[i])) {
            return t->kind == MO_TYPE_CAPABILITY && (uint32_t)t->capability == i;
        }
    }
    if (word_is(start, length, "Ordering")) {
        return mo_bytecode_ordering(t);
    }
    if (word_is(start, length, "Entry")) {
        static const char *const names[] = {"File", "Folder", "Link", "Other"};
        if (t->kind != MO_TYPE_RECORD || t->field_count != 2 ||
            strcmp(t->fields[0].name, "name") != 0 || strcmp(t->fields[1].name, "kind") != 0 ||
            m->file->types[t->fields[0].type].kind != MO_TYPE_TEXT ||
            mo_verify_layout_lookup(m->v->layouts, m->file->layout_count, index, MO_NO_INDEX) ==
                MO_NO_INDEX) {
            return false;
        }
        const struct mo_type *k = &m->file->types[t->fields[1].type];
        return cases(k, names, 4) && !k->has_fields;
    }
    if (word_is(start, length, "Option") || word_is(start, length, "Result")) {
        bool option = word_is(start, length, "Option");
        static const char *const opts[] = {"None", "Some"}, *const results[] = {"Ok", "Error"};
        if (!cases(t, option ? opts : results, 2) || !layouts(m, index) || !take(m, '(')) {
            return false;
        }
        if (option && t->cases[0].field_count != 0) {
            return false;
        }
        for (uint32_t i = option ? 1U : 0U; i < 2; i++) {
            const struct mo_type_case *c = &t->cases[i];
            if (c->field_count != 1 || !c->unnamed || (i == 1 && !option && !take(m, ',')) ||
                !type(m, c->fields[0].type, depth + 1)) {
                return false;
            }
        }
        return take(m, ')');
    }
    if (word_is(start, length, "FileError")) {
        static const char *const names[] = {"NotFound", "NotAllowed", "NotText", "Failed"};
        if (!cases(t, names, 4) || !layouts(m, index)) {
            return false;
        }
        for (uint32_t i = 0; i < 4; i++) {
            const struct mo_type_case *c = &t->cases[i];
            if (c->field_count != (i == 3 ? 2U : 1U) || c->unnamed ||
                strcmp(c->fields[0].name, "path") != 0 ||
                m->file->types[c->fields[0].type].kind != MO_TYPE_TEXT) {
                return false;
            }
            if (i == 3 && (strcmp(c->fields[1].name, "reason") != 0 ||
                           m->file->types[c->fields[1].type].kind != MO_TYPE_TEXT)) {
                return false;
            }
        }
        return true;
    }
    return false;
}

bool mo_bytecode_signature(const struct mo_verify_function *context, const char *signature,
                           const uint32_t *parameters, uint32_t count, uint32_t result,
                           const char **reason) {
    MO_ASSERT(reason != NULL);
    const struct mo_bytecode *f = context->file;
    struct match m = {.file = f,
                      .v = context,
                      .cursor = signature,
                      .variables = {MO_NO_INDEX, MO_NO_INDEX, MO_NO_INDEX, MO_NO_INDEX}};
    if (strncmp(m.cursor, "fn(", 3) == 0) {
        m.cursor += 3;
        for (uint32_t i = 0; i < count; i++) {
            if ((i != 0 && !take(&m, ',')) || !type(&m, parameters[i], 0)) {
                return false;
            }
        }
        if (!take(&m, ')')) {
            return false;
        }
        if (take(&m, ':')) {
            if (!type(&m, result, 0)) {
                return false;
            }
        } else if (f->types[result].kind != MO_TYPE_UNIT) {
            return false;
        }
    } else if (count != 0 || !type(&m, result, 0)) {
        return false;
    }
    spaces(&m);
    if (strncmp(m.cursor, "where ", 6) == 0) {
        m.cursor += 6;
        int v = variable(m.cursor, 1);
        m.cursor++;
        if (v < 0 || m.variables[v] == MO_NO_INDEX || !take(&m, ':')) {
            return false;
        }
        spaces(&m);
        uint32_t t = m.variables[v];
        if (strcmp(m.cursor, "Equal") == 0) {
            *reason = "library Equal constraint";
            return (m.v->abilities[t] & MO_ABILITY_EQUAL) != 0;
        }
        if (strcmp(m.cursor, "Hash") == 0) {
            *reason = "library Hash constraint";
            return (m.v->abilities[t] & MO_ABILITY_HASH) != 0;
        }
        *reason = "library Number constraint";
        enum mo_type_kind k = f->types[t].kind;
        return strcmp(m.cursor, "Number") == 0 && (k == MO_TYPE_INT || k == MO_TYPE_BIGINT ||
                                                   k == MO_TYPE_DECIMAL || k == MO_TYPE_FLOAT);
    }
    return *m.cursor == '\0';
}
