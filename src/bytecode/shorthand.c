#include "bytecode/shorthand.h"

#include <string.h>

bool mo_short_index(struct mo_bytecode_string name) {
    if (name.length < 2 || strchr("stLkgifmdrph", name.bytes[0]) == NULL)
        return false;
    for (uint32_t i = 1; i < name.length; i++)
        if (name.bytes[i] < '0' || name.bytes[i] > '9')
            return false;
    return true;
}

bool mo_short_name(struct mo_shorthand *s, bool header, struct mo_bytecode_string *out) {
    struct mo_text_reader *r = s->r;
    uint32_t start = r->token.start, end = start + r->token.length;
    if (r->token.kind != 'w' || r->token.length == 0) {
        mo_text_refuse(r, start, "expected qualified name");
        return false;
    }
    mo_text_next(r);
    while (mo_text_is(r, "(")) {
        struct mo_text_token before = r->token;
        uint32_t cursor = r->cursor, depth = 0;
        do {
            if (mo_text_is(r, "(")) {
                if (++depth > MO_LIMIT_NESTING_DEPTH) {
                    mo_text_refuse(r, r->token.start, "expected name within nesting limit");
                    return false;
                }
            } else if (mo_text_is(r, ")"))
                depth--;
            else if (r->token.kind == 0 || r->token.kind == 'n') {
                mo_text_refuse(r, r->token.start, "expected ) in qualified name");
                return false;
            }
            end = r->token.start + r->token.length;
            mo_text_next(r);
        } while (depth != 0 && !r->failed);
        if (header && r->token.kind != '(' &&
            !(r->token.kind == 'w' && r->text[r->token.start] == '.')) {
            r->token = before;
            r->cursor = cursor;
            end = before.start;
            while (end > start && (r->text[end - 1] == ' ' || r->text[end - 1] == '\t'))
                end--;
            break;
        }
        if (r->token.kind == 'w' && r->text[r->token.start] == '.') {
            end = r->token.start + r->token.length;
            mo_text_next(r);
        }
    }
    size_t capacity = 3 * (size_t)(end - start);
    if (capacity > MO_LIMIT_STRING_SIZE)
        capacity = MO_LIMIT_STRING_SIZE;
    uint8_t *bytes = mo_arena_allocate(&r->arena, capacity + 1, 1);
    if (bytes == NULL) {
        mo_text_refuse(r, 0, "no memory");
        return false;
    }
    uint32_t n = 0;
    for (uint32_t i = start; i < end; i++) {
        uint8_t c = (uint8_t)r->text[i];
        if (c == ' ' || c == '\t')
            continue;
        unsigned added = c == ':' ? 3U : c == ',' ? 2U : 1U;
        if (added > capacity - n) {
            mo_text_refuse(r, start, "expected bounded UTF-8 qualified name");
            return false;
        }
        if (c == ':')
            bytes[n++] = ' ';
        bytes[n++] = c;
        if (c == ',' || c == ':')
            bytes[n++] = ' ';
    }
    bytes[n] = 0;
    if (n == 0 || !mo_bytecode_utf8(bytes, n)) {
        mo_text_refuse(r, start, "expected bounded UTF-8 qualified name");
        return false;
    }
    *out = (struct mo_bytecode_string){n, bytes};
    return !r->failed;
}

bool mo_short_lookup(struct mo_shorthand *s, enum mo_bytecode_section section,
                     struct mo_bytecode_string name, uint32_t source, uint32_t *out) {
    struct mo_text_index *found = mo_text_find(s->symbols[section], name);
    if (found == NULL) {
        static const char *const expected[] = {"expected string",
                                               "expected declared nominal type",
                                               "expected layout",
                                               "expected constant",
                                               "expected declared global",
                                               "expected initializer",
                                               "expected declared function",
                                               "expected declared module",
                                               "expected debug entry"};
        mo_text_refuse(s->r, source, expected[section]);
        return false;
    }
    *out = found->value;
    return true;
}

bool mo_short_reference(struct mo_shorthand *s, enum mo_bytecode_section section, char prefix,
                        uint32_t *out) {
    struct mo_text_reader *r = s->r;
    uint32_t source = r->token.start;
    struct mo_bytecode_string key = {r->token.length, (const uint8_t *)r->text + source};
    if (mo_short_index(key))
        return mo_text_ref(r, prefix, false, out);
    return mo_short_name(s, false, &key) && mo_short_lookup(s, section, key, source, out);
}

bool mo_short_pool(struct mo_shorthand *s, enum mo_bytecode_section section, uint32_t source,
                   uint32_t *out) {
    struct mo_text_reader *r = s->r;
    MO_ASSERT(section <= MO_SECTION_CONSTANTS);
    struct mo_bytecode_string key = {(uint32_t)r->raw.length, r->raw.bytes};
    if (section == MO_SECTION_STRINGS) {
        key.length -= 4;
        key.bytes += 4;
    }
    struct mo_text_index *found = mo_text_find(s->pools[section], key);
    if (found != NULL) {
        *out = found->value;
        mo_buffer_clear(&r->raw);
        mo_buffer_clear(&r->positions);
        return true;
    }
    *out = r->tables[section].count;
    if (!mo_text_save(r, section, *out, source))
        return false;
    const struct mo_text_entry *e = &r->tables[section].entries[*out];
    key = (struct mo_bytecode_string){e->length, e->bytes};
    if (section == MO_SECTION_STRINGS) {
        key.length -= 4;
        key.bytes += 4;
    }
    return mo_text_insert(r, &s->pools[section], key, *out) != NULL;
}

bool mo_short_string(struct mo_shorthand *s, struct mo_bytecode_string bytes, uint32_t source,
                     uint32_t *out) {
    struct mo_text_reader *r = s->r;
    struct mo_text_index *found = mo_text_find(s->pools[MO_SECTION_STRINGS], bytes);
    if (found != NULL) {
        *out = found->value;
        return true;
    }
    struct mo_buffer saved = r->raw, positions = r->positions;
    mo_buffer_init(&r->raw, MO_LIMIT_BYTECODE_SIZE);
    mo_buffer_init(&r->positions, 2 * (size_t)MO_LIMIT_BYTECODE_SIZE);
    bool ok = mo_text_word(r, bytes.length, source) &&
              mo_text_bytes(r, bytes.bytes, bytes.length, source) &&
              mo_short_pool(s, MO_SECTION_STRINGS, source, out);
    mo_buffer_free(&r->raw);
    mo_buffer_free(&r->positions);
    r->raw = saved;
    r->positions = positions;
    return ok;
}

bool mo_short_request(struct mo_shorthand *s, const struct mo_text_request *request) {
    struct mo_text_request *q =
        mo_arena_copy(&s->r->arena, request, sizeof *q, _Alignof(struct mo_text_request));
    if (q == NULL) {
        mo_text_refuse(s->r, 0, "no memory");
        return false;
    }
    q->next = NULL;
    *s->last_request = q;
    s->last_request = &q->next;
    return true;
}

static MO_MUST_USE bool declare(struct mo_shorthand *s, enum mo_bytecode_section section,
                                uint32_t kind, uint32_t source) {
    struct mo_text_reader *r = s->r;
    struct mo_bytecode_string name;
    uint32_t name_source = r->token.start;
    if (!mo_short_name(s, section == MO_SECTION_FUNCTIONS, &name))
        return false;
    if (mo_short_index(name)) {
        mo_text_refuse(r, name_source, "expected symbolic name, not reserved index spelling");
        return false;
    }
    if (mo_text_find(s->symbols[section], name) != NULL) {
        mo_text_refuse(r, source, "expected unique symbolic declaration");
        return false;
    }
    struct mo_text_decl *decl =
        mo_arena_allocate(&r->arena, sizeof *decl, _Alignof(struct mo_text_decl));
    if (decl == NULL) {
        mo_text_refuse(r, 0, "no memory");
        return false;
    }
    uint32_t index = r->tables[section].count, string;
    if (!mo_short_string(s, name, source, &string) || !mo_text_word(r, kind, source) ||
        (section == MO_SECTION_TYPES && !mo_text_word(r, string, source)) ||
        !mo_text_save(r, section, index, source))
        return false;
    *decl = (struct mo_text_decl){section, source, index, name, NULL};
    *s->last = decl;
    s->last = &decl->next;
    return mo_text_insert(r, &s->symbols[section], name, index) != NULL;
}

/* String encounters are independent of type interning. Recording them during reservation
 * also lets a seed refer to a name string supplied by a later declaration. */
static MO_MUST_USE bool reserve(struct mo_shorthand *s) {
    struct mo_text_reader *r = s->r;
    enum mo_bytecode_section body = MO_SECTION_COUNT;
    while (!r->failed && r->token.kind != 0) {
        uint32_t source = r->token.start;
        if (body == MO_SECTION_COUNT) {
            enum mo_bytecode_section section = mo_text_is(r, "record") || mo_text_is(r, "choice")
                                                   ? MO_SECTION_TYPES
                                               : mo_text_is(r, "function") ? MO_SECTION_FUNCTIONS
                                               : mo_text_is(r, "global")   ? MO_SECTION_GLOBALS
                                               : mo_text_is(r, "module")   ? MO_SECTION_MODULES
                                                                           : MO_SECTION_INIT;
            if (section == MO_SECTION_INIT) {
                if (!mo_text_is(r, "init")) {
                    mo_text_refuse(r, source,
                                   "expected record, choice, global, init, function or module");
                    return false;
                }
                mo_text_next(r);
            } else {
                uint32_t kind = mo_text_is(r, "choice") ? MO_TYPE_CHOICE : MO_TYPE_RECORD;
                mo_text_next(r);
                if (!declare(s, section, kind, source))
                    return false;
                if (section == MO_SECTION_TYPES || section == MO_SECTION_FUNCTIONS)
                    body = section;
            }
        } else if (mo_text_is(r, "end")) {
            body = MO_SECTION_COUNT;
            mo_text_next(r);
        } else if (body == MO_SECTION_TYPES && mo_text_is(r, "case")) {
            struct mo_bytecode_string name;
            uint32_t unused;
            mo_text_next(r);
            if (!mo_short_name(s, true, &name) || !mo_short_string(s, name, source, &unused))
                return false;
        }
        bool registers = body == MO_SECTION_FUNCTIONS && mo_text_is(r, "registers");
        struct mo_text_token previous = {0};
        while (r->token.kind != 'n' && r->token.kind != 0 && !r->failed) {
            struct mo_text_token token = r->token;
            uint32_t unused;
            if (token.kind == 'q') {
                struct mo_bytecode_string bytes;
                if (!mo_text_quote(r, &bytes) || !mo_short_string(s, bytes, token.start, &unused))
                    return false;
            } else {
                if (!registers && token.kind == ':' && previous.kind == 'w' &&
                    r->text[previous.start] != '@') {
                    struct mo_bytecode_string bytes = {previous.length,
                                                       (const uint8_t *)r->text + previous.start};
                    if (!mo_short_string(s, bytes, previous.start, &unused))
                        return false;
                }
                mo_text_next(r);
            }
            previous = token;
        }
        if (!mo_text_end_line(r))
            return false;
    }
    if (body != MO_SECTION_COUNT) {
        mo_text_refuse(r, r->length, "expected declaration end");
        return false;
    }
    return !r->failed;
}

static MO_MUST_USE bool field(struct mo_shorthand *s) {
    struct mo_text_reader *r = s->r;
    uint32_t source = r->token.start, string, type;
    struct mo_bytecode_string name;
    return mo_short_name(s, true, &name) && mo_short_string(s, name, source, &string) &&
           mo_text_take(r, ":", "expected : after field name") && mo_short_type(s, 0, &type) &&
           mo_text_word(r, string, source) && mo_text_word(r, type, source);
}

static MO_MUST_USE bool nominal(struct mo_shorthand *s, const struct mo_text_decl *decl) {
    struct mo_text_reader *r = s->r;
    uint32_t kind = mo_text_get(&r->tables[MO_SECTION_TYPES].entries[decl->index], 0), string;
    struct mo_bytecode_string ignored;
    mo_text_next(r);
    if (!mo_short_name(s, false, &ignored) || !mo_text_end_line(r) ||
        !mo_short_string(s, decl->name, decl->source, &string) ||
        !mo_text_word(r, kind, decl->source) || !mo_text_word(r, string, decl->source))
        return false;
    uint32_t offset = (uint32_t)r->raw.length, count = 0;
    if (!mo_text_word(r, 0, decl->source))
        return false;
    while (!r->failed && !mo_text_is(r, "end")) {
        uint32_t source = r->token.start;
        if (++count > (kind == MO_TYPE_RECORD ? MO_LIMIT_FIELDS : MO_LIMIT_CHOICE_CASES)) {
            mo_text_refuse(r, source, "expected nominal within field or case limit");
            return false;
        }
        if (kind == MO_TYPE_RECORD) {
            if (!field(s))
                return false;
        } else {
            struct mo_bytecode_string case_name;
            if (!mo_text_take(r, "case", "expected case") || !mo_short_name(s, true, &case_name) ||
                !mo_short_string(s, case_name, source, &string) || !mo_text_word(r, string, source))
                return false;
            uint32_t fields_offset = (uint32_t)r->raw.length, fields = 0;
            if (!mo_text_word(r, 0, source))
                return false;
            if (mo_text_is(r, "(")) {
                mo_text_next(r);
                if (!mo_text_is(r, ")")) {
                    do {
                        if (++fields > MO_LIMIT_FIELDS) {
                            mo_text_refuse(r, r->token.start, "expected case within field limit");
                            return false;
                        }
                        struct mo_text_token token = r->token;
                        uint32_t cursor = r->cursor;
                        mo_text_next(r);
                        bool named = r->token.kind == ':';
                        r->token = token;
                        r->cursor = cursor;
                        if (named) {
                            if (!field(s))
                                return false;
                        } else {
                            uint32_t type;
                            if (!mo_short_type(s, 0, &type) ||
                                !mo_text_word(r, MO_BYTECODE_NO_INDEX, token.start) ||
                                !mo_text_word(r, type, token.start))
                                return false;
                        }
                        if (mo_text_is(r, ")"))
                            break;
                    } while (mo_text_take(r, ",", "expected , between case fields"));
                }
                if (!mo_text_take(r, ")", "expected ) after case fields"))
                    return false;
            }
            mo_text_patch(r, fields_offset, fields);
        }
        if (!mo_text_end_line(r))
            return false;
    }
    mo_text_patch(r, offset, count);
    return mo_text_take(r, "end", "expected nominal end") && mo_text_end_line(r) &&
           mo_text_save(r, MO_SECTION_TYPES, decl->index, decl->source);
}

static MO_MUST_USE bool global(struct mo_shorthand *s, const struct mo_text_decl *decl) {
    struct mo_text_reader *r = s->r;
    struct mo_bytecode_string ignored;
    uint32_t type, name, module;
    mo_text_next(r);
    return mo_short_name(s, true, &ignored) &&
           mo_text_take(r, ":", "expected : after global name") && mo_short_type(s, 0, &type) &&
           mo_text_take(r, "in", "expected in module") &&
           mo_short_reference(s, MO_SECTION_MODULES, 'm', &module) && mo_text_end_line(r) &&
           mo_short_string(s, decl->name, decl->source, &name) &&
           mo_text_word(r, type, decl->source) && mo_text_word(r, name, decl->source) &&
           mo_text_word(r, module, decl->source) &&
           mo_text_save(r, MO_SECTION_GLOBALS, decl->index, decl->source);
}

static MO_MUST_USE bool module(struct mo_shorthand *s, const struct mo_text_decl *decl) {
    struct mo_text_reader *r = s->r;
    struct mo_bytecode_string ignored;
    struct mo_buffer types, functions;
    mo_buffer_init(&types, MO_LIMIT_MODULE_NAMES * 8);
    mo_buffer_init(&functions, MO_LIMIT_MODULE_NAMES * 4);
    uint32_t name;
    mo_text_next(r);
    bool ok = mo_short_name(s, false, &ignored) && mo_text_take(r, "exposes", "expected exposes") &&
              mo_text_take(r, "[", "expected exposure list [") &&
              mo_short_string(s, decl->name, decl->source, &name);
    while (ok && !mo_text_is(r, "]")) {
        uint32_t source = r->token.start;
        struct mo_bytecode_string key;
        ok = mo_short_name(s, false, &key);
        bool exposed = ok && key.length >= 4 && memcmp(key.bytes + key.length - 4, "(..)", 4) == 0;
        if (exposed)
            key.length -= 4;
        struct mo_text_index *type = ok ? mo_text_find(s->symbols[MO_SECTION_TYPES], key) : NULL;
        struct mo_text_index *fn = ok ? mo_text_find(s->symbols[MO_SECTION_FUNCTIONS], key) : NULL;
        if (ok && type != NULL && fn != NULL) {
            mo_text_refuse(r, source, "expected unambiguous exposed type or function");
            ok = false;
        }
        if (ok && type != NULL)
            ok = mo_buffer_append_le32(&types, type->value) &&
                 mo_buffer_append_le32(&types, exposed);
        else if (ok && fn != NULL && !exposed)
            ok = mo_buffer_append_le32(&functions, fn->value);
        else if (ok) {
            mo_text_refuse(r, source, "expected exposed type or function (contents only on type)");
            ok = false;
        }
        if (!ok && !r->failed)
            mo_text_refuse(r, source, "expected exposures within count limit or available memory");
        if (mo_text_is(r, "]"))
            break;
        if (ok)
            ok = mo_text_take(r, ",", "expected , between exposures");
    }
    if (ok)
        ok = mo_text_take(r, "]", "expected ] after exposures") && mo_text_end_line(r) &&
             mo_text_word(r, name, decl->source) &&
             mo_text_word(r, (uint32_t)(types.length / 8), decl->source) &&
             mo_text_bytes(r, types.bytes, types.length, decl->source) &&
             mo_text_word(r, (uint32_t)(functions.length / 4), decl->source) &&
             mo_text_bytes(r, functions.bytes, functions.length, decl->source) &&
             mo_text_word(r, 0, decl->source) &&
             mo_text_save(r, MO_SECTION_MODULES, decl->index, decl->source);
    mo_buffer_free(&functions);
    mo_buffer_free(&types);
    return ok;
}

bool mo_text_shorthand(struct mo_text_reader *r) {
    static const char *const seeds[] = {"strings", "types", "layouts", "constants"};
    struct mo_shorthand s = {.r = r};
    s.last = &s.first;
    s.last_request = &s.requests;
    for (unsigned i = 0; i < 4; i++) {
        if (mo_text_is(r, seeds[i]) && !mo_text_indexed_section(r, (enum mo_bytecode_section)i))
            return false;
        if (i == MO_SECTION_CONSTANTS)
            continue;
        for (uint32_t j = 0; j < r->tables[i].count; j++) {
            const struct mo_text_entry *e = &r->tables[i].entries[j];
            if (i == MO_SECTION_TYPES &&
                (mo_text_get(e, 0) == MO_TYPE_RECORD || mo_text_get(e, 0) == MO_TYPE_CHOICE))
                continue;
            struct mo_bytecode_string key = {e->length, e->bytes};
            if (i == MO_SECTION_STRINGS) {
                key.length -= 4;
                key.bytes += 4;
            }
            if (mo_text_insert(r, &s.pools[i], key, j) == NULL)
                return false;
        }
    }
    uint32_t seed_types = r->tables[MO_SECTION_TYPES].count;
    struct mo_text_token first = r->token;
    uint32_t cursor = r->cursor;
    if (!reserve(&s))
        return false;
    for (uint32_t i = 0; i < seed_types; i++) {
        const struct mo_text_entry *e = &r->tables[MO_SECTION_TYPES].entries[i];
        uint32_t kind = mo_text_get(e, 0);
        if (kind != MO_TYPE_RECORD && kind != MO_TYPE_CHOICE)
            continue;
        uint32_t name = mo_text_get(e, 4);
        if (name >= r->tables[MO_SECTION_STRINGS].count) {
            MO_ASSERT(e->position_count >= 2);
            mo_text_refuse(r, e->positions[1].text, "expected string index in final pool");
            return false;
        }
        const struct mo_text_entry *entry = &r->tables[MO_SECTION_STRINGS].entries[name];
        struct mo_bytecode_string key = {entry->length - 4, entry->bytes + 4};
        if (mo_text_find(s.symbols[MO_SECTION_TYPES], key) != NULL) {
            mo_text_refuse(r, e->source, "expected unique symbolic declaration");
            return false;
        }
        if (mo_text_insert(r, &s.symbols[MO_SECTION_TYPES], key, i) == NULL)
            return false;
    }
    uint32_t n = r->tables[MO_SECTION_FUNCTIONS].count;
    s.function_types = mo_arena_allocate_array(&r->arena, (size_t)n + 1, sizeof *s.function_types,
                                               _Alignof(uint32_t));
    if (s.function_types == NULL) {
        mo_text_refuse(r, 0, "no memory");
        return false;
    }
    for (uint32_t i = 0; i < n; i++)
        s.function_types[i] = MO_BYTECODE_NO_INDEX;
    r->token = first;
    r->cursor = cursor;
    const struct mo_text_decl *decl = s.first;
    bool had_init = false;
    while (r->token.kind != 0 && !r->failed) {
        if (mo_text_is(r, "init")) {
            if (had_init) {
                mo_text_refuse(r, r->token.start, "expected at most one init declaration");
                return false;
            }
            had_init = true;
            mo_text_next(r);
            if (!mo_text_take(r, "[", "expected init list ["))
                return false;
            while (!mo_text_is(r, "]") && !r->failed) {
                uint32_t function, source = r->token.start;
                if (!mo_short_reference(&s, MO_SECTION_FUNCTIONS, 'f', &function) ||
                    !mo_text_word(r, function, source) ||
                    !mo_text_save(r, MO_SECTION_INIT, r->tables[MO_SECTION_INIT].count, source))
                    return false;
                if (mo_text_is(r, "]"))
                    break;
                if (!mo_text_take(r, ",", "expected , between init functions"))
                    return false;
            }
            if (!mo_text_take(r, "]", "expected ] after init functions") || !mo_text_end_line(r))
                return false;
            continue;
        }
        MO_ASSERT(decl != NULL && decl->source == r->token.start);
        bool ok = decl->section == MO_SECTION_TYPES       ? nominal(&s, decl)
                  : decl->section == MO_SECTION_GLOBALS   ? global(&s, decl)
                  : decl->section == MO_SECTION_FUNCTIONS ? mo_short_function(&s, decl)
                                                          : module(&s, decl);
        if (!ok)
            return false;
        decl = decl->next;
    }
    return !r->failed && mo_short_finish(&s);
}
