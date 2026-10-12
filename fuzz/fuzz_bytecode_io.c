#include "bytecode/io.h"

#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static MO_MUST_USE void *allocate(struct mo_arena *arena, size_t n, size_t size, size_t align) {
    void *p = mo_arena_allocate_array(arena, n, size, align);
    MO_ASSERT(p != NULL);
    return p;
}

/* An independent byte-range UTF-8 check, rather than the decoder's scalar accumulator. */
static MO_MUST_USE bool name_bytes(const struct mo_bytecode_string *s) {
    bool named = s->length != 0;
    for (uint32_t i = 0; i < s->length;) {
        uint8_t lead = s->bytes[i];
        named &= lead != 0;
        uint32_t n = lead < 0x80 ? 1U : lead < 0xe0 ? 2U : lead < 0xf0 ? 3U : 4U;
        MO_ASSERT(n <= s->length - i);
        if (n != 1) {
            MO_ASSERT(lead >= 0xc2 && lead <= 0xf4);
            for (uint32_t j = 1; j < n; j++) {
                MO_ASSERT(s->bytes[i + j] >= 0x80 && s->bytes[i + j] <= 0xbf);
            }
            uint8_t second = s->bytes[i + 1];
            MO_ASSERT((lead != 0xe0 || second >= 0xa0) && (lead != 0xed || second < 0xa0) &&
                      (lead != 0xf0 || second >= 0x90) && (lead != 0xf4 || second < 0x90));
        }
        i += n;
    }
    return named;
}

struct address {
    uintptr_t pointer;
    uint32_t index;
};

static MO_MUST_USE int addresses(const void *a, const void *b) {
    uintptr_t x = ((const struct address *)a)->pointer;
    uintptr_t y = ((const struct address *)b)->pointer;
    return (x > y) - (x < y);
}

static MO_MUST_USE uint32_t named_pointer(const struct address *names, uint32_t count,
                                          const bool *flags, const char *name) {
    uint32_t low = 0, high = count;
    uintptr_t pointer = (uintptr_t)name;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        if (names[mid].pointer < pointer) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return low < count && names[low].pointer == pointer && flags[names[low].index]
               ? names[low].index
               : MO_BYTECODE_NO_INDEX;
}

static MO_MUST_USE bool nominal(const struct mo_type *t) {
    return t->kind == MO_TYPE_RECORD || t->kind == MO_TYPE_CHOICE;
}

struct edge {
    uint32_t parent;
    uint32_t next;
};

/* Compare serialized active keys, not the validator's numeric comparison or heap hashes. */
struct key {
    uint32_t words[3];
    uint32_t count;
    const uint32_t *parameters;
};

static MO_MUST_USE int keys(const void *a, const void *b) {
    const struct key *x = a, *y = b;
    int order = memcmp(x->words, y->words, sizeof x->words);
    if (order == 0) {
        order = (x->count > y->count) - (x->count < y->count);
    }
    if (order == 0 && x->count != 0) {
        order = memcmp(x->parameters, y->parameters, (size_t)x->count * sizeof(uint32_t));
    }
    return order;
}

static void distinct(struct key *items, uint32_t count) {
    qsort(items, count, sizeof *items, keys);
    for (uint32_t i = 1; i < count; i++) {
        MO_ASSERT(keys(&items[i - 1], &items[i]) != 0);
    }
}

static void source(const struct mo_bytecode *f, const bool *flags, uint32_t file, uint32_t start,
                   uint32_t end) {
    MO_ASSERT(file == MO_BYTECODE_NO_INDEX ? start == 0 && end == 0
                                           : file < f->string_count && flags[file] &&
                                                 start <= end && end <= MO_LIMIT_FILE_SIZE);
}

static void accepted_tables(const struct mo_bytecode *f, size_t size) {
    MO_ASSERT(f->header.major == 0 && f->header.minor == 1 && f->header.flags == 0);
    MO_ASSERT(f->string_count <= MO_LIMIT_STRINGS && f->type_count <= MO_LIMIT_TYPES &&
              f->layout_count <= MO_LIMIT_LAYOUTS && f->constant_count <= MO_LIMIT_CONSTANTS &&
              f->global_count <= MO_LIMIT_GLOBALS && f->function_count <= MO_LIMIT_FUNCTIONS &&
              f->module_count <= MO_LIMIT_MODULES && f->debug_count == f->function_count &&
              f->init_count == f->global_count);
    struct mo_arena arena;
    mo_arena_init(&arena, mo_bytecode_work_budget(size));
    bool *flags = allocate(&arena, f->string_count, sizeof *flags, _Alignof(bool));
    struct address *names =
        allocate(&arena, f->string_count, sizeof *names, _Alignof(struct address));
    for (uint32_t i = 0; i < f->string_count; i++) {
        MO_ASSERT(f->strings[i].length <= MO_LIMIT_STRING_SIZE);
        flags[i] = name_bytes(&f->strings[i]);
        names[i] = (struct address){(uintptr_t)f->strings[i].bytes, i};
    }
    qsort(names, f->string_count, sizeof *names, addresses);
    uint32_t *fields_seen =
        allocate(&arena, f->string_count, sizeof *fields_seen, _Alignof(uint32_t));
    uint32_t *cases_seen =
        allocate(&arena, f->string_count, sizeof *cases_seen, _Alignof(uint32_t));
    memset(fields_seen, 0, (size_t)f->string_count * sizeof *fields_seen);
    memset(cases_seen, 0, (size_t)f->string_count * sizeof *cases_seen);
    uint32_t group = 0;
    struct key *type_keys =
        allocate(&arena, f->type_count, sizeof *type_keys, _Alignof(struct key));
    uint32_t key_count = 0;
    uint64_t references = 0;
    for (uint32_t i = 0; i < f->type_count; i++) {
        const struct mo_type *t = &f->types[i];
        MO_ASSERT(t->kind >= MO_TYPE_UNIT && t->kind <= MO_TYPE_CAPABILITY && t->defined);
        uint32_t n = t->parameter_count;
        if (t->kind == MO_TYPE_TUPLE) {
            MO_ASSERT(n >= 2 && n <= MO_LIMIT_FIELDS);
        } else if (t->kind == MO_TYPE_LIST || t->kind == MO_TYPE_SET) {
            MO_ASSERT(n == 1);
        } else if (t->kind == MO_TYPE_MAP) {
            MO_ASSERT(n == 2);
        } else if (t->kind == MO_TYPE_FUNCTION) {
            MO_ASSERT(n <= MO_LIMIT_BYTECODE_PARAMETERS && t->result < f->type_count);
        } else {
            MO_ASSERT(n == 0);
        }
        for (uint32_t j = 0; j < n; j++) {
            MO_ASSERT(t->parameters[j] < f->type_count);
        }
        references += n + (t->kind == MO_TYPE_FUNCTION ? 1U : 0U);
        if (nominal(t)) {
            MO_ASSERT(named_pointer(names, f->string_count, flags, t->name) !=
                      MO_BYTECODE_NO_INDEX);
            MO_ASSERT(t->field_count <= MO_LIMIT_FIELDS);
            group++;
            for (uint32_t j = 0; j < t->field_count; j++) {
                uint32_t id = named_pointer(names, f->string_count, flags, t->fields[j].name);
                MO_ASSERT(t->fields[j].type < f->type_count && id != MO_BYTECODE_NO_INDEX);
                MO_ASSERT(fields_seen[id] != group);
                fields_seen[id] = group;
            }
            if (t->kind == MO_TYPE_CHOICE) {
                MO_ASSERT(t->case_count != 0 && t->case_count <= MO_LIMIT_CHOICE_CASES);
                bool has_fields = false;
                for (uint32_t j = 0; j < t->case_count; j++) {
                    const struct mo_type_case *c = &t->cases[j];
                    uint32_t id = named_pointer(names, f->string_count, flags, c->name);
                    MO_ASSERT(id != MO_BYTECODE_NO_INDEX && cases_seen[id] != i + 1);
                    cases_seen[id] = i + 1;
                    MO_ASSERT(c->field_count <= MO_LIMIT_FIELDS);
                    has_fields |= c->field_count != 0;
                    MO_ASSERT(c->unnamed == (c->field_count == 1 && c->fields[0].name == NULL));
                    group++;
                    for (uint32_t k = 0; k < c->field_count; k++) {
                        MO_ASSERT(c->fields[k].type < f->type_count);
                        if (!c->unnamed) {
                            id = named_pointer(names, f->string_count, flags, c->fields[k].name);
                            MO_ASSERT(id != MO_BYTECODE_NO_INDEX && fields_seen[id] != group);
                            fields_seen[id] = group;
                        }
                    }
                }
                MO_ASSERT(t->has_fields == has_fields);
            }
        } else if (t->kind == MO_TYPE_CAPABILITY) {
            MO_ASSERT(t->capability >= MO_CAP_ARGS && t->capability <= MO_CAP_RANDOM);
        } else if (t->kind == MO_TYPE_ENV) {
            MO_ASSERT(t->function < f->function_count &&
                      f->functions[t->function].kind == MO_FUNCTION_CLOSURE);
        }
        if (!nominal(t)) {
            uint32_t discriminator = t->kind == MO_TYPE_ENV          ? t->function
                                     : t->kind == MO_TYPE_CAPABILITY ? (uint32_t)t->capability
                                                                     : 0;
            type_keys[key_count++] = (struct key){
                {(uint32_t)t->kind, discriminator, t->kind == MO_TYPE_FUNCTION ? t->result : 0},
                n,
                t->parameters};
        }
    }
    distinct(type_keys, key_count);
    /* Kahn's leaf removal checks the same acyclicity contract by a different algorithm.
     * Ignore edges through nominal declarations, which have distinct recursive identities. */
    MO_ASSERT(references <= MO_LIMIT_BYTECODE_SIZE / 4);
    struct edge *edges = allocate(&arena, (size_t)references, sizeof *edges, _Alignof(struct edge));
    uint32_t *heads = allocate(&arena, f->type_count, sizeof *heads, _Alignof(uint32_t));
    uint32_t *pending = allocate(&arena, f->type_count, sizeof *pending, _Alignof(uint32_t));
    uint32_t *queue = allocate(&arena, f->type_count, sizeof *queue, _Alignof(uint32_t));
    for (uint32_t i = 0; i < f->type_count; i++) {
        heads[i] = MO_BYTECODE_NO_INDEX;
        pending[i] = 0;
    }
    uint32_t edge_count = 0, end = 0, structural = 0;
    for (uint32_t i = 0; i < f->type_count; i++) {
        const struct mo_type *t = &f->types[i];
        if (nominal(t)) {
            continue;
        }
        structural++;
        uint32_t n = t->parameter_count + (t->kind == MO_TYPE_FUNCTION ? 1U : 0U);
        for (uint32_t j = 0; j < n; j++) {
            uint32_t child = j < t->parameter_count ? t->parameters[j] : t->result;
            if (!nominal(&f->types[child])) {
                MO_ASSERT(edge_count < references);
                edges[edge_count] = (struct edge){i, heads[child]};
                heads[child] = edge_count++;
                pending[i]++;
            }
        }
        if (pending[i] == 0) {
            queue[end++] = i;
        }
    }
    for (uint32_t start = 0; start < end; start++) {
        for (uint32_t e = heads[queue[start]]; e != MO_BYTECODE_NO_INDEX; e = edges[e].next) {
            uint32_t parent = edges[e].parent;
            MO_ASSERT(pending[parent] != 0);
            if (--pending[parent] == 0) {
                MO_ASSERT(end < f->type_count);
                queue[end++] = parent;
            }
        }
    }
    MO_ASSERT(end == structural);
    struct key *layout_keys =
        allocate(&arena, f->layout_count, sizeof *layout_keys, _Alignof(struct key));
    for (uint32_t i = 0; i < f->layout_count; i++) {
        const struct mo_layout *l = &f->layouts[i];
        MO_ASSERT(l->kind >= MO_LAYOUT_RECORD && l->kind <= MO_LAYOUT_CLOSURE &&
                  l->type < f->type_count && l->field_count <= MO_LIMIT_FIELDS);
        const struct mo_type *t = &f->types[l->type];
        layout_keys[i] = (struct key){{(uint32_t)l->kind, l->type,
                                       l->kind == MO_LAYOUT_CASE      ? l->case_index
                                       : l->kind == MO_LAYOUT_CLOSURE ? l->function
                                                                      : 0},
                                      0,
                                      NULL};
        if (l->kind == MO_LAYOUT_RECORD || l->kind == MO_LAYOUT_CASE) {
            const struct mo_type_field *fields = t->fields;
            uint32_t count = t->field_count;
            if (l->kind == MO_LAYOUT_CASE) {
                MO_ASSERT(t->kind == MO_TYPE_CHOICE && l->case_index < t->case_count);
                const struct mo_type_case *c = &t->cases[l->case_index];
                fields = c->fields;
                count = c->field_count;
                MO_ASSERT(count != 0);
                if (count == 1) {
                    const struct mo_type *field = &f->types[fields[0].type];
                    MO_ASSERT(field->kind == MO_TYPE_DECIMAL || field->kind == MO_TYPE_RANGE ||
                              (field->kind == MO_TYPE_CHOICE && field->has_fields));
                }
            } else {
                MO_ASSERT(t->kind == MO_TYPE_RECORD);
            }
            MO_ASSERT(l->field_count == count);
            for (uint32_t j = 0; j < count; j++) {
                MO_ASSERT(l->fields[j] == fields[j].type);
            }
        } else if (l->kind == MO_LAYOUT_TUPLE) {
            MO_ASSERT(t->kind == MO_TYPE_TUPLE && l->field_count == t->parameter_count);
            for (uint32_t j = 0; j < l->field_count; j++) {
                MO_ASSERT(l->fields[j] == t->parameters[j]);
            }
        } else {
            MO_ASSERT(t->kind == MO_TYPE_FUNCTION && l->function < f->function_count);
            const struct mo_bytecode_function *fn = &f->functions[l->function];
            MO_ASSERT(fn->kind == MO_FUNCTION_CLOSURE && fn->result_type == t->result &&
                      fn->parameter_count == t->parameter_count &&
                      l->field_count == fn->capture_count);
            for (uint32_t j = 0; j < fn->parameter_count; j++) {
                MO_ASSERT(fn->parameters[j].type == t->parameters[j]);
            }
            for (uint32_t j = 0; j < l->field_count; j++) {
                MO_ASSERT(l->fields[j] == fn->capture_types[j] && l->fields[j] < f->type_count);
            }
        }
    }
    distinct(layout_keys, f->layout_count);
    for (uint32_t i = 0; i < f->constant_count; i++) {
        const struct mo_bytecode_constant *k = &f->constants[i];
        MO_ASSERT(k->type < f->type_count);
        enum mo_type_kind kind = f->types[k->type].kind;
        if (kind <= MO_TYPE_DURATION) {
            MO_ASSERT(k->encoding == MO_CONSTANT_IMMEDIATE64);
            if (kind == MO_TYPE_UNIT || kind == MO_TYPE_BOOL || kind == MO_TYPE_BYTE) {
                for (unsigned j = 1; j < 8; j++) {
                    MO_ASSERT(k->value.immediate[j] == 0);
                }
                MO_ASSERT(kind != MO_TYPE_UNIT || k->value.immediate[0] == 0);
                MO_ASSERT(kind != MO_TYPE_BOOL || k->value.immediate[0] <= 1);
            }
            if (kind == MO_TYPE_CHAR) {
                for (unsigned j = 3; j < 8; j++) {
                    MO_ASSERT(k->value.immediate[j] == 0);
                }
                uint32_t scalar = (uint32_t)k->value.immediate[0] |
                                  ((uint32_t)k->value.immediate[1] << 8) |
                                  ((uint32_t)k->value.immediate[2] << 16);
                MO_ASSERT(scalar <= 0x10ffff && (scalar < 0xd800 || scalar > 0xdfff));
            }
            if (kind == MO_TYPE_TIME) {
                uint64_t value = 0;
                for (unsigned j = 0; j < 8; j++) {
                    value |= (uint64_t)k->value.immediate[j] << (8 * j);
                }
                MO_ASSERT(value >= UINT64_C(0xff23400100d44000) ||
                          value < UINT64_C(253402300800000000));
            }
        } else if (kind == MO_TYPE_DECIMAL) {
            MO_ASSERT(k->encoding == MO_CONSTANT_DECIMAL128);
            bool minimum = k->value.immediate[15] == 0x80;
            for (unsigned j = 0; j < 15; j++) {
                minimum &= k->value.immediate[j] == 0;
            }
            MO_ASSERT(!minimum);
        } else if (kind == MO_TYPE_TEXT) {
            MO_ASSERT(k->encoding == MO_CONSTANT_TEXT && k->value.string < f->string_count);
        } else if (kind == MO_TYPE_BYTES) {
            MO_ASSERT(k->encoding == MO_CONSTANT_BYTES &&
                      k->value.bytes.length <= MO_LIMIT_STRING_SIZE);
        } else if (kind == MO_TYPE_BIGINT) {
            const struct mo_bytecode_bigint *b = &k->value.bigint;
            MO_ASSERT(k->encoding == MO_CONSTANT_BIGINT && b->sign >= MO_BIGINT_ZERO &&
                      b->sign <= MO_BIGINT_NEGATIVE && b->magnitude.length <= MO_LIMIT_STRING_SIZE);
            MO_ASSERT(b->sign == MO_BIGINT_ZERO
                          ? b->magnitude.length == 0
                          : b->magnitude.length != 0 &&
                                b->magnitude.bytes[b->magnitude.length - 1] != 0);
        } else {
            MO_ASSERT(kind == MO_TYPE_FUNCTION && k->encoding == MO_CONSTANT_CLOSURE &&
                      k->value.function < f->function_count);
            const struct mo_bytecode_function *fn = &f->functions[k->value.function];
            MO_ASSERT(fn->kind == MO_FUNCTION_CLOSURE && fn->capture_count == 0 &&
                      fn->result_type == f->types[k->type].result &&
                      fn->parameter_count == f->types[k->type].parameter_count);
            for (uint32_t j = 0; j < fn->parameter_count; j++) {
                MO_ASSERT(fn->parameters[j].type == f->types[k->type].parameters[j]);
            }
        }
    }
    for (uint32_t i = 0; i < f->function_count; i++) {
        const struct mo_bytecode_function *fn = &f->functions[i];
        const struct mo_bytecode_debug *d = &f->debug[i];
        MO_ASSERT(fn->kind >= MO_FUNCTION_FUNCTION && fn->kind <= MO_FUNCTION_ANCHOR &&
                  fn->name < f->string_count && flags[fn->name] && fn->module < f->module_count &&
                  fn->result_type < f->type_count &&
                  fn->parameter_count <= MO_LIMIT_BYTECODE_PARAMETERS &&
                  fn->register_count <= MO_LIMIT_FUNCTION_REGISTERS &&
                  fn->capture_count <= MO_LIMIT_FIELDS &&
                  fn->code_count <= MO_LIMIT_FUNCTION_CODE_WORDS);
        uint32_t slots = fn->parameter_count + (fn->kind == MO_FUNCTION_CLOSURE ? 1U : 0U);
        MO_ASSERT(slots <= MO_LIMIT_BYTECODE_PARAMETERS && slots <= fn->register_count);
        MO_ASSERT(fn->kind == MO_FUNCTION_CLOSURE || fn->capture_count == 0);
        MO_ASSERT(fn->kind == MO_FUNCTION_CLOSURE ||
                  fn->spec_code == (fn->kind >= MO_FUNCTION_TEST));
        enum mo_type_kind result = f->types[fn->result_type].kind;
        if (fn->kind >= MO_FUNCTION_INIT && fn->kind <= MO_FUNCTION_EXAMPLE) {
            MO_ASSERT(fn->parameter_count == 0 && result == MO_TYPE_UNIT);
        } else if (fn->kind == MO_FUNCTION_PROMISE) {
            MO_ASSERT(fn->parameter_count != 0 && result == MO_TYPE_INT);
        } else if (fn->kind == MO_FUNCTION_ANCHOR) {
            MO_ASSERT(fn->parameter_count == 0 && result == MO_TYPE_INT);
        }
        for (uint32_t j = 0; j < fn->register_count; j++) {
            MO_ASSERT(fn->register_types[j] < f->type_count);
        }
        for (uint32_t j = 0; j < fn->parameter_count; j++) {
            MO_ASSERT(fn->parameters[j].type == fn->register_types[j] &&
                      fn->parameters[j].ownership >= MO_PARAMETER_OWNED &&
                      fn->parameters[j].ownership <= MO_PARAMETER_BORROWED);
            MO_ASSERT(fn->kind == MO_FUNCTION_FUNCTION ||
                      fn->parameters[j].ownership == MO_PARAMETER_OWNED);
        }
        for (uint32_t j = 0; j < fn->capture_count; j++) {
            MO_ASSERT(fn->capture_types[j] < f->type_count);
        }
        if (fn->kind == MO_FUNCTION_CLOSURE) {
            const struct mo_type *env = &f->types[fn->register_types[fn->parameter_count]];
            MO_ASSERT(env->kind == MO_TYPE_ENV && env->function == i);
        }
        MO_ASSERT(d->span_count <= MO_LIMIT_DEBUG_ENTRIES &&
                  d->name_count <= MO_LIMIT_DEBUG_ENTRIES &&
                  d->line_count <= MO_LIMIT_DEBUG_ENTRIES);
        source(f, flags, d->file, d->start, d->end);
        MO_ASSERT(d->text == MO_BYTECODE_NO_INDEX ||
                  (fn->kind >= MO_FUNCTION_TEST && d->text < f->string_count));
        MO_ASSERT(d->expectation >= MO_BYTECODE_NOT_ANCHOR &&
                  d->expectation <= MO_BYTECODE_FORBIDS);
        MO_ASSERT(d->expectation == MO_BYTECODE_NOT_ANCHOR || fn->kind == MO_FUNCTION_ANCHOR);
        MO_ASSERT(d->line_count == 0 || fn->kind == MO_FUNCTION_PROMISE);
        if (d->subject != MO_BYTECODE_NO_INDEX) {
            MO_ASSERT(d->subject < f->function_count);
            MO_ASSERT(fn->kind == MO_FUNCTION_PROMISE
                          ? f->functions[d->subject].kind == MO_FUNCTION_FUNCTION &&
                                d->line_count != 0
                          : fn->kind == MO_FUNCTION_ANCHOR &&
                                f->functions[d->subject].kind == MO_FUNCTION_PROMISE &&
                                d->expectation != MO_BYTECODE_NOT_ANCHOR);
        }
        for (uint32_t j = 0; j < d->span_count; j++) {
            const struct mo_bytecode_span *s = &d->spans[j];
            MO_ASSERT(s->code_offset < fn->code_count &&
                      (j == 0 || d->spans[j - 1].code_offset < s->code_offset));
            source(f, flags, s->file, s->start, s->end);
        }
        for (uint32_t j = 0; j < d->name_count; j++) {
            const struct mo_bytecode_register_name *r = &d->names[j];
            MO_ASSERT(r->reg < fn->register_count && r->name < f->string_count && flags[r->name] &&
                      r->code_start < r->code_end && r->code_end <= fn->code_count);
        }
        for (uint32_t j = 0; j < d->line_count; j++) {
            const struct mo_bytecode_spec_line *line = &d->lines[j];
            source(f, flags, line->file, line->start, line->end);
            MO_ASSERT(line->text < f->string_count);
        }
    }
    for (uint32_t i = 0; i < f->global_count; i++) {
        const struct mo_bytecode_global *g = &f->globals[i];
        MO_ASSERT(g->type < f->type_count && g->module < f->module_count &&
                  g->name < f->string_count && flags[g->name]);
        MO_ASSERT(f->init[i] < f->function_count &&
                  f->functions[f->init[i]].kind == MO_FUNCTION_INIT);
    }
    uint32_t *exports = allocate(&arena, f->type_count, sizeof *exports, _Alignof(uint32_t));
    memset(exports, 0, (size_t)f->type_count * sizeof *exports);
    for (uint32_t i = 0; i < f->module_count; i++) {
        const struct mo_bytecode_module *m = &f->modules[i];
        MO_ASSERT(
            m->name < f->string_count && flags[m->name] && m->type_count <= MO_LIMIT_MODULE_NAMES &&
            m->function_count <= MO_LIMIT_MODULE_NAMES && m->global_count <= MO_LIMIT_MODULE_NAMES);
        for (uint32_t j = 0; j < m->type_count; j++) {
            MO_ASSERT(m->types[j].type < f->type_count);
            MO_ASSERT(exports[m->types[j].type] != i + 1);
            exports[m->types[j].type] = i + 1;
        }
        for (uint32_t j = 0; j < m->function_count; j++) {
            MO_ASSERT(m->functions[j] < f->function_count &&
                      f->functions[m->functions[j]].module == i);
        }
        for (uint32_t j = 0; j < m->global_count; j++) {
            MO_ASSERT(m->globals[j] < f->global_count && f->globals[m->globals[j]].module == i);
        }
    }
    mo_arena_free(&arena);
}

static void round_trip(const uint8_t *data, size_t size) {
    struct mo_arena arena;
    struct mo_load_error load_error;
    mo_arena_init(&arena, size <= MO_LIMIT_BYTECODE_SIZE ? mo_bytecode_work_budget(size) : 0);
    const struct mo_bytecode *file = mo_bytecode_decode(&arena, data, size, &load_error);
    if (file != NULL) {
        accepted_tables(file, size);
        struct mo_buffer out;
        struct mo_write_error write_error;
        mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
        MO_ASSERT(mo_bytecode_write(file, &out, &write_error));
        MO_ASSERT(size == out.length && memcmp(data, out.bytes, size) == 0);
        mo_buffer_free(&out);
    } else {
        MO_ASSERT(load_error.reason != NULL && load_error.offset <= size);
    }
    mo_arena_free(&arena);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    round_trip(data, size);
    /* Repair the checksum too, so mutations reach the tables rather than stopping at FNV.
     * Still test the untouched input above: a bad checksum must never be accepted. */
    if (size >= MO_BYTECODE_HEADER_SIZE && size <= MO_LIMIT_BYTECODE_SIZE) {
        struct mo_buffer copy;
        mo_buffer_init(&copy, MO_LIMIT_BYTECODE_SIZE);
        if (mo_buffer_append(&copy, data, size)) {
            uint64_t hash = MO_BYTECODE_FNV_OFFSET;
            for (size_t i = MO_BYTECODE_HEADER_SIZE; i < size; i++) {
                hash = (hash ^ copy.bytes[i]) * MO_BYTECODE_FNV_PRIME;
            }
            for (unsigned i = 0; i < 8; i++) {
                copy.bytes[12 + i] = (uint8_t)(hash >> (i * 8));
            }
            round_trip(copy.bytes, size);
        }
        mo_buffer_free(&copy);
    }
    return 0;
}
