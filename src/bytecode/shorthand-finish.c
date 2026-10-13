#include "bytecode/shorthand.h"

#include <string.h>

struct type_info {
    uint32_t *cases;
    bool has_fields;
};

static MO_MUST_USE bool types(struct mo_shorthand *s, struct type_info **out,
                              struct mo_text_index **names) {
    struct mo_text_reader *r = s->r;
    struct mo_text_table *table = &r->tables[MO_SECTION_TYPES];
    struct type_info *info = mo_arena_allocate_array(&r->arena, (size_t)table->count + 1,
                                                     sizeof *info, _Alignof(struct type_info));
    if (info == NULL) {
        mo_text_refuse(r, 0, "no memory");
        return false;
    }
    memset(info, 0, ((size_t)table->count + 1) * sizeof *info);
    for (uint32_t i = 0; i < table->count; i++) {
        const struct mo_text_entry *e = &table->entries[i];
        if (mo_text_get(e, 0) != MO_TYPE_CHOICE)
            continue;
        uint32_t count = mo_text_get(e, 8), offset = 12;
        info[i].cases = mo_arena_allocate_array(&r->arena, (size_t)count + 1, sizeof(uint32_t),
                                                _Alignof(uint32_t));
        if (info[i].cases == NULL) {
            mo_text_refuse(r, 0, "no memory");
            return false;
        }
        for (uint32_t j = 0; j < count; j++) {
            info[i].cases[j] = offset;
            uint32_t name = mo_text_get(e, offset), fields = mo_text_get(e, offset + 4);
            info[i].has_fields |= fields != 0;
            uint8_t *key = mo_arena_allocate(&r->arena, 8, 1);
            if (key == NULL) {
                mo_text_refuse(r, 0, "no memory");
                return false;
            }
            for (unsigned b = 0; b < 4; b++) {
                key[b] = (uint8_t)(i >> (8 * b));
                key[b + 4] = (uint8_t)(name >> (8 * b));
            }
            if (mo_text_insert(r, names, (struct mo_bytecode_string){8, key}, j) == NULL)
                return false;
            offset += 8 + fields * 8;
        }
    }
    *out = info;
    return true;
}

bool mo_short_literal(struct mo_shorthand *s, uint32_t type, bool shorthand) {
    struct mo_text_reader *r = s->r;
    uint32_t source = r->token.start;
    if (type >= r->tables[MO_SECTION_TYPES].count) {
        mo_text_refuse(r, source, "expected constant type index in final pool");
        return false;
    }
    uint32_t kind = mo_text_get(&r->tables[MO_SECTION_TYPES].entries[type], 0);
    if (shorthand && kind == MO_TYPE_TEXT) {
        struct mo_bytecode_string bytes;
        uint32_t string;
        if (r->token.kind != 'q') {
            mo_text_refuse(r, source, "expected quoted Text literal");
            return false;
        }
        return mo_text_quote(r, &bytes) && mo_short_string(s, bytes, source, &string) &&
               mo_text_word(r, string, source);
    }
    if (shorthand && kind == MO_TYPE_FUNCTION) {
        uint32_t function;
        return mo_text_take(r, "closure", "expected closure(NAME) literal") &&
               mo_text_take(r, "(", "expected ( after closure") &&
               mo_short_reference(s, MO_SECTION_FUNCTIONS, 'f', &function) &&
               mo_text_take(r, ")", "expected ) after closure function") &&
               mo_text_word(r, function, source);
    }
    return mo_text_payload(r, type, shorthand);
}

static MO_MUST_USE bool layout(struct mo_shorthand *s, const struct mo_text_request *q,
                               const struct type_info *info, struct mo_text_index *names,
                               struct mo_text_index **cache, uint32_t *out) {
    struct mo_text_reader *r = s->r;
    uint32_t kind = q->kind, type = q->type, tag = q->tag;
    bool automatic = q->automatic;
    if (kind == MO_LAYOUT_CLOSURE) {
        if (tag >= r->tables[MO_SECTION_FUNCTIONS].count ||
            s->function_types[tag] == MO_BYTECODE_NO_INDEX) {
            mo_text_refuse(r, q->source, "expected closure function for layout");
            return false;
        }
        type = s->function_types[tag];
    }
    if (type >= r->tables[MO_SECTION_TYPES].count) {
        mo_text_refuse(r, q->source, "expected layout type in final pool");
        return false;
    }
    const struct mo_text_entry *e = &r->tables[MO_SECTION_TYPES].entries[type];
    uint32_t type_kind = mo_text_get(e, 0), count, offset, stride;
    if (kind == MO_LAYOUT_CLOSURE) {
        e = &r->tables[MO_SECTION_FUNCTIONS].entries[tag];
        offset = 24 + mo_text_get(e, 16) * 8;
        count = mo_text_get(e, offset);
        offset += 4;
        stride = 4;
    } else if (kind == MO_LAYOUT_CASE) {
        if (type_kind != MO_TYPE_CHOICE) {
            mo_text_refuse(r, q->source, "expected choice type for case layout");
            return false;
        }
        if (!automatic) {
            uint8_t key[8];
            for (unsigned b = 0; b < 4; b++) {
                key[b] = (uint8_t)(type >> (8 * b));
                key[b + 4] = (uint8_t)(tag >> (8 * b));
            }
            struct mo_text_index *found = mo_text_find(names, (struct mo_bytecode_string){8, key});
            if (found == NULL) {
                mo_text_refuse(r, q->source, "expected case name declared by layout type");
                return false;
            }
            tag = found->value;
        }
        if (tag >= mo_text_get(e, 8)) {
            mo_text_refuse(r, q->source, "expected case index in range");
            return false;
        }
        offset = info[type].cases[tag] + 4;
        count = mo_text_get(e, offset);
        offset += 8;
        stride = 8;
        bool boxed = count > 1;
        if (count == 1) {
            uint32_t field = mo_text_get(e, offset);
            if (field >= r->tables[MO_SECTION_TYPES].count) {
                mo_text_refuse(r, q->source, "expected case field type in range");
                return false;
            }
            uint32_t field_kind = mo_text_get(&r->tables[MO_SECTION_TYPES].entries[field], 0);
            boxed = field_kind == MO_TYPE_DECIMAL || field_kind == MO_TYPE_RANGE ||
                    (field_kind == MO_TYPE_CHOICE && info[field].has_fields);
        }
        if (!boxed) {
            if (automatic) {
                *out = MO_BYTECODE_NO_INDEX;
                return true;
            }
            mo_text_refuse(r, q->source, "expected boxed choice case for layout");
            return false;
        }
    } else if (type_kind == MO_TYPE_RECORD) {
        kind = MO_LAYOUT_RECORD;
        count = mo_text_get(e, 8);
        offset = 16;
        stride = 8;
    } else if (type_kind == MO_TYPE_TUPLE) {
        kind = MO_LAYOUT_TUPLE;
        count = mo_text_get(e, 4);
        offset = 8;
        stride = 4;
    } else {
        mo_text_refuse(r, q->source, "expected record, tuple, boxed case or closure layout");
        return false;
    }
    uint8_t key[12];
    const uint32_t parts[] = {kind, type, tag};
    for (unsigned i = 0; i < 3; i++)
        for (unsigned b = 0; b < 4; b++)
            key[i * 4 + b] = (uint8_t)(parts[i] >> (8 * b));
    struct mo_text_index *found =
        mo_text_find(*cache, (struct mo_bytecode_string){sizeof key, key});
    if (found != NULL) {
        *out = found->value;
        return true;
    }
    if (!mo_text_word(r, kind, q->source) || !mo_text_word(r, type, q->source) ||
        ((kind == MO_LAYOUT_CASE || kind == MO_LAYOUT_CLOSURE) &&
         !mo_text_word(r, tag, q->source)) ||
        !mo_text_word(r, count, q->source))
        return false;
    for (uint32_t i = 0; i < count; i++)
        if (!mo_text_word(r, mo_text_get(e, offset + i * stride), q->source))
            return false;
    if (!mo_short_pool(s, MO_SECTION_LAYOUTS, q->source, out))
        return false;
    uint8_t *saved = mo_arena_copy(&r->arena, key, sizeof key, 1);
    if (saved == NULL) {
        mo_text_refuse(r, 0, "no memory");
        return false;
    }
    return mo_text_insert(r, cache, (struct mo_bytecode_string){sizeof key, saved}, *out) != NULL;
}

static MO_MUST_USE bool globals(struct mo_shorthand *s) {
    struct mo_text_reader *r = s->r;
    struct mo_text_table *modules = &r->tables[MO_SECTION_MODULES],
                         *globals = &r->tables[MO_SECTION_GLOBALS];
    uint32_t *counts = mo_arena_allocate_array(&r->arena, (size_t)modules->count + 1,
                                               sizeof(uint32_t), _Alignof(uint32_t));
    uint32_t *starts = mo_arena_allocate_array(&r->arena, (size_t)modules->count + 1,
                                               sizeof(uint32_t), _Alignof(uint32_t));
    uint32_t *items = mo_arena_allocate_array(&r->arena, (size_t)globals->count + 1,
                                              sizeof(uint32_t), _Alignof(uint32_t));
    if (counts == NULL || starts == NULL || items == NULL) {
        mo_text_refuse(r, 0, "no memory");
        return false;
    }
    memset(counts, 0, ((size_t)modules->count + 1) * sizeof *counts);
    for (uint32_t i = 0; i < globals->count; i++) {
        uint32_t module = mo_text_get(&globals->entries[i], 8);
        if (module >= modules->count) {
            mo_text_refuse(r, globals->entries[i].source, "expected declared module index");
            return false;
        }
        counts[module]++;
    }
    uint32_t start = 0;
    for (uint32_t i = 0; i < modules->count; i++) {
        starts[i] = start;
        start += counts[i];
        counts[i] = 0;
    }
    for (uint32_t i = 0; i < globals->count; i++) {
        uint32_t module = mo_text_get(&globals->entries[i], 8);
        items[starts[module] + counts[module]++] = i;
    }
    for (uint32_t i = 0; i < modules->count; i++) {
        const struct mo_text_entry *e = &modules->entries[i];
        uint32_t source = e->source;
        if (!mo_text_bytes(r, e->bytes, e->length - 4, source) ||
            !mo_text_word(r, counts[i], source))
            return false;
        for (uint32_t j = 0; j < counts[i]; j++)
            if (!mo_text_word(r, items[starts[i] + j], source))
                return false;
        if (!mo_text_save(r, MO_SECTION_MODULES, i, source))
            return false;
    }
    return true;
}

bool mo_short_finish(struct mo_shorthand *s) {
    struct mo_text_reader *r = s->r;
    struct mo_text_token end = r->token;
    uint32_t cursor = r->cursor;
    /* Refuse at the original reference before a deferred operand needs its target. */
    if (!mo_text_check_references(r, false))
        return false;
    struct type_info *info;
    struct mo_text_index *names = NULL, *cache = NULL;
    if (!types(s, &info, &names))
        return false;
    struct mo_text_table *constants = &r->tables[MO_SECTION_CONSTANTS];
    for (uint32_t i = 0; i < constants->count; i++) {
        const struct mo_text_entry *e = &constants->entries[i];
        uint32_t type = mo_text_get(e, 0), source = e->source;
        r->cursor = mo_text_get(e, 4);
        mo_text_next(r);
        if (!mo_text_word(r, type, source) || !mo_short_literal(s, type, false) ||
            !mo_text_end_line(r) || !mo_text_save(r, MO_SECTION_CONSTANTS, i, source))
            return false;
        e = &constants->entries[i];
        if (mo_text_insert(r, &s->pools[MO_SECTION_CONSTANTS],
                           (struct mo_bytecode_string){e->length, e->bytes}, i) == NULL)
            return false;
    }
    for (const struct mo_text_request *q = s->requests; q != NULL; q = q->next) {
        uint32_t index;
        if (q->constant) {
            if (q->payload.length != 0) {
                if (!mo_text_bytes(r, q->payload.bytes, q->payload.length, q->source))
                    return false;
            } else {
                r->cursor = q->source;
                mo_text_next(r);
                if (!mo_text_word(r, q->type, q->source) || !mo_short_literal(s, q->type, true))
                    return false;
                if (r->token.kind != 'n' && r->token.kind != 0 && r->token.kind != ',') {
                    mo_text_refuse(r, r->token.start, "expected end of literal operand");
                    return false;
                }
            }
            if (!mo_short_pool(s, MO_SECTION_CONSTANTS, q->source, &index))
                return false;
        } else if (!layout(s, q, info, names, &cache, &index))
            return false;
        if (q->offset != MO_BYTECODE_NO_INDEX) {
            struct mo_text_entry *fn = &r->tables[MO_SECTION_FUNCTIONS].entries[q->function];
            MO_ASSERT(q->offset <= fn->length && fn->length - q->offset >= 4);
            for (unsigned i = 0; i < 4; i++)
                fn->bytes[q->offset + i] = (uint8_t)(index >> (8 * i));
        }
    }
    r->token = end;
    r->cursor = cursor;
    return globals(s);
}
