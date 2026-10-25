#include "bytecode/assemble.h"

#include <string.h>

void mo_text_refuse(struct mo_text_reader *r, uint32_t offset, const char *reason) {
    if (!r->failed) {
        *r->error = (struct mo_assemble_error){offset, reason};
        r->failed = true;
    }
}

void mo_text_next(struct mo_text_reader *r) {
    if (r->failed)
        return;
    while (r->cursor < r->length) {
        char c = r->text[r->cursor];
        if (c == ' ' || c == '\t') {
            r->cursor++;
            continue;
        }
        if (c == ';') {
            while (r->cursor < r->length && r->text[r->cursor] != '\n' &&
                   r->text[r->cursor] != '\r')
                r->cursor++;
            continue;
        }
        break;
    }
    uint32_t start = r->cursor;
    r->token = (struct mo_text_token){.start = start};
    if (start == r->length)
        return;
    char c = r->text[r->cursor++];
    if (c == '\r' || c == '\n') {
        if (c == '\r') {
            if (r->cursor == r->length || r->text[r->cursor++] != '\n') {
                mo_text_refuse(r, start, "expected LF after CR");
                return;
            }
        }
        r->token.kind = 'n';
    } else if (c != 0 && strchr("[](),:", c) != NULL) {
        r->token.kind = c;
    } else if (c == '"' || c == '\'' ||
               (c == 'h' && r->length - start >= 4 && memcmp(r->text + start, "hex\"", 4) == 0)) {
        r->token.kind = c == '"' ? 'q' : c == '\'' ? 'c' : 'x';
        unsigned char quote = c == '\'' ? '\'' : '"';
        if (r->token.kind == 'x')
            r->cursor = start + 4;
        bool closed = false;
        while (r->cursor < r->length) {
            unsigned char b = (unsigned char)r->text[r->cursor++];
            if (b == quote) {
                closed = true;
                break;
            }
            if (b < 0x20 || b == 0x7f) {
                mo_text_refuse(r, r->cursor - 1, "expected printable quoted byte or escape");
                return;
            }
            if (b == '\\' && r->token.kind != 'x' && r->cursor < r->length)
                r->cursor++;
        }
        if (!closed) {
            mo_text_refuse(r, start, "expected closing quote");
            return;
        }
    } else {
        r->token.kind = 'w';
        if ((unsigned char)c < 0x20 || (unsigned char)c == 0x7f) {
            mo_text_refuse(r, start, "expected text token");
            return;
        }
        while (r->cursor < r->length) {
            unsigned char b = (unsigned char)r->text[r->cursor];
            if (b <= 0x20 || b == 0x7f || strchr("[](),:;\"", b) != NULL)
                break;
            r->cursor++;
        }
    }
    r->token.length = r->cursor - start;
}

bool mo_text_is(const struct mo_text_reader *r, const char *s) {
    size_t n = strlen(s);
    return !r->failed && n == r->token.length && memcmp(r->text + r->token.start, s, n) == 0;
}

bool mo_text_take(struct mo_text_reader *r, const char *s, const char *expected) {
    if (!mo_text_is(r, s)) {
        mo_text_refuse(r, r->token.start, expected);
        return false;
    }
    mo_text_next(r);
    return !r->failed;
}

bool mo_text_end_line(struct mo_text_reader *r) {
    if (r->token.kind != 'n' && r->token.kind != 0) {
        mo_text_refuse(r, r->token.start, "expected end of line");
        return false;
    }
    while (r->token.kind == 'n' && !r->failed)
        mo_text_next(r);
    return !r->failed;
}

bool mo_text_ref(struct mo_text_reader *r, char prefix, bool optional, uint32_t *out) {
    static const char *const expected[] = {
        "expected unsigned u32",       "expected string index sN",   "expected type index tN",
        "expected layout index LN",    "expected constant index kN", "expected global index gN",
        "expected init index iN",      "expected function index fN", "expected module index mN",
        "expected debug index dN",     "expected register rN",       "expected word offset @N",
        "expected primitive index pN", "expected host index hN",     "expected filename index PN",
        "expected service index SN"};
    static const char prefixes[] = "\0stLkgifmdr@phPS";
    size_t which = 0;
    while (prefixes[which] != prefix) {
        which++;
        MO_ASSERT(which < sizeof prefixes - 1);
    }
    uint32_t start = r->token.start, n = r->token.length;
    const char *s = r->text + start;
    if (optional && mo_text_is(r, "-")) {
        *out = MO_BYTECODE_NO_INDEX;
        mo_text_next(r);
        return !r->failed;
    }
    uint32_t i = prefix == 0 ? 0 : 1;
    if (r->token.kind != 'w' || n <= i || (prefix != 0 && s[0] != prefix)) {
        mo_text_refuse(r, start, expected[which]);
        return false;
    }
    uint32_t value = 0;
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9' || value > (UINT32_MAX - (uint32_t)(s[i] - '0')) / 10) {
            mo_text_refuse(r, start + i, expected[which]);
            return false;
        }
        value = value * 10 + (uint32_t)(s[i] - '0');
    }
    if (prefix != 0 && value == MO_BYTECODE_NO_INDEX) {
        mo_text_refuse(r, start, "expected index below UINT32_MAX (use - for absence)");
        return false;
    }
    if ((which >= 1 && which <= 9) || which >= 12) {
        struct mo_text_reference *ref =
            mo_arena_allocate(&r->arena, sizeof *ref, _Alignof(struct mo_text_reference));
        if (ref == NULL) {
            mo_text_refuse(r, 0, "no memory");
            return false;
        }
        *ref = (struct mo_text_reference){start, value, (uint32_t)which, NULL};
        *r->last_reference = ref;
        r->last_reference = &ref->next;
    }
    *out = value;
    mo_text_next(r);
    return !r->failed;
}

bool mo_text_enum(struct mo_text_reader *r, const char *const *names, uint32_t count,
                  const char *expected, uint32_t *out) {
    for (uint32_t i = 0; i < count; i++) {
        if (mo_text_is(r, names[i])) {
            *out = i;
            mo_text_next(r);
            return !r->failed;
        }
    }
    mo_text_refuse(r, r->token.start, expected);
    return false;
}

bool mo_text_bytes(struct mo_text_reader *r, const void *bytes, size_t length, uint32_t source) {
    if (r->failed)
        return false;
    struct mo_text_position pos = {(uint32_t)r->raw.length, source};
    if (length > r->raw.limit - r->raw.length) {
        mo_text_refuse(r, source, "expected bytecode within size limit");
        return false;
    }
    if (!mo_buffer_append(&r->positions, &pos, sizeof pos) ||
        !mo_buffer_append(&r->raw, bytes, length)) {
        mo_text_refuse(r, 0, "no memory");
        return false;
    }
    return true;
}

bool mo_text_word(struct mo_text_reader *r, uint32_t value, uint32_t source) {
    uint8_t bytes[4];
    for (unsigned i = 0; i < 4; i++)
        bytes[i] = (uint8_t)(value >> (8 * i));
    return mo_text_bytes(r, bytes, sizeof bytes, source);
}

void mo_text_patch(struct mo_text_reader *r, uint32_t offset, uint32_t word) {
    MO_ASSERT(offset <= r->raw.length && r->raw.length - offset >= 4);
    for (unsigned i = 0; i < 4; i++)
        r->raw.bytes[offset + i] = (uint8_t)(word >> (8 * i));
}

uint32_t mo_text_get(const struct mo_text_entry *e, uint32_t offset) {
    MO_ASSERT(offset <= e->length && e->length - offset >= 4);
    uint32_t word = 0;
    for (unsigned i = 0; i < 4; i++)
        word |= (uint32_t)e->bytes[offset + i] << (8 * i);
    return word;
}

bool mo_text_save(struct mo_text_reader *r, enum mo_bytecode_section section, uint32_t index,
                  uint32_t source) {
    static const uint32_t limits[] = {MO_LIMIT_STRINGS,   MO_LIMIT_TYPES,   MO_LIMIT_LAYOUTS,
                                      MO_LIMIT_CONSTANTS, MO_LIMIT_GLOBALS, MO_LIMIT_INITS,
                                      MO_LIMIT_FUNCTIONS, MO_LIMIT_MODULES, MO_LIMIT_FILES,
                                      MO_LIMIT_FUNCTIONS, MO_LIMIT_SERVICES};
    struct mo_text_table *t = &r->tables[section];
    if (index > t->count || (!r->shorthand && index != t->count) || index >= limits[section]) {
        mo_text_refuse(r, source, "expected consecutive entry index within section limit");
        return false;
    }
    if (t->count == 0 && t->source == 0)
        t->source = source;
    if (index < t->count)
        r->wire_size -= t->entries[index].length;
    if (r->raw.length > MO_LIMIT_BYTECODE_SIZE - r->wire_size) {
        mo_text_refuse(r, source, "expected bytecode within size limit");
        return false;
    }
    if (t->count == t->capacity) {
        uint32_t capacity = t->capacity == 0 ? 8 : t->capacity * 2;
        struct mo_text_entry *entries =
            mo_arena_resize(&r->arena, t->entries, (size_t)t->capacity * sizeof *entries,
                            (size_t)capacity * sizeof *entries, _Alignof(struct mo_text_entry));
        if (entries == NULL) {
            mo_text_refuse(r, 0, "no memory");
            return false;
        }
        t->entries = entries;
        t->capacity = capacity;
    }
    uint8_t *bytes = mo_arena_copy(&r->arena, r->raw.bytes, r->raw.length, 1);
    struct mo_text_position *positions = mo_arena_copy(
        &r->arena, r->positions.bytes, r->positions.length, _Alignof(struct mo_text_position));
    if (bytes == NULL || positions == NULL) {
        mo_text_refuse(r, 0, "no memory");
        return false;
    }
    t->entries[index] =
        (struct mo_text_entry){bytes, (uint32_t)r->raw.length, positions,
                               (uint32_t)(r->positions.length / sizeof *positions), source};
    if (index == t->count)
        t->count++;
    r->wire_size += r->raw.length;
    mo_buffer_clear(&r->raw);
    mo_buffer_clear(&r->positions);
    return true;
}

static MO_MUST_USE uint32_t text_offset(const struct mo_text_reader *r, uint32_t wire) {
    uint32_t offset = MO_BYTECODE_HEADER_SIZE;
    for (unsigned s = 0; s < MO_SECTION_COUNT; s++) {
        const struct mo_text_table *t = &r->tables[s];
        if (wire < offset + 4)
            return t->source;
        offset += 4;
        for (uint32_t i = 0; i < t->count; i++) {
            const struct mo_text_entry *e = &t->entries[i];
            if (wire < offset + e->length) {
                uint32_t local = wire - offset, low = 0, high = e->position_count;
                while (low < high) {
                    uint32_t mid = low + (high - low) / 2;
                    if (e->positions[mid].wire <= local)
                        low = mid + 1;
                    else
                        high = mid;
                }
                return low == 0 ? e->source : e->positions[low - 1].text;
            }
            offset += e->length;
        }
    }
    return r->length;
}

bool mo_text_check_references(struct mo_text_reader *r, bool complete) {
    static const char *const expected[] = {"",
                                           "expected string index in final pool",
                                           "expected type index in final pool",
                                           "expected layout index in final pool",
                                           "expected constant index in final pool",
                                           "expected global index in final pool",
                                           "expected init index in final pool",
                                           "expected function index in final pool",
                                           "expected module index in final pool",
                                           "expected debug index in final pool",
                                           "",
                                           "",
                                           "expected primitive index in range",
                                           "expected host index in range",
                                           "expected filename index in final pool",
                                           "expected service index in final pool"};
    for (const struct mo_text_reference *ref = r->references; ref != NULL; ref = ref->next) {
        /* Shorthand's deferred operands can still append layouts and constants. All other
         * tables are complete before resolving those operands. */
        if (!complete && (ref->space == 3 || ref->space == 4))
            continue;
        uint32_t bound = ref->space == 15   ? r->tables[MO_SECTION_SERVICES].count
                         : ref->space == 14 ? r->tables[MO_SECTION_FILES].count
                         : ref->space == 9  ? r->tables[MO_SECTION_DEBUG].count
                         : ref->space < 9   ? r->tables[ref->space - 1].count
                         : ref->space == 12 ? (uint32_t)MO_PRIM_COUNT
                                            : (uint32_t)MO_HOST_COUNT;
        if (ref->index >= bound) {
            mo_text_refuse(r, ref->source, expected[ref->space]);
            return false;
        }
    }
    return true;
}

static MO_MUST_USE bool output(struct mo_text_reader *r, struct mo_buffer *out) {
    if (!mo_text_check_references(r, true))
        return false;
    uint8_t header[MO_BYTECODE_HEADER_SIZE];
    mo_bytecode_header_bytes(header);
    header[8] = r->tables[MO_SECTION_SERVICES].count != 0 ? MO_BYTECODE_SERVICE_FLAG : 0;
    if (r->wire_size > out->limit) {
        mo_text_refuse(r, 0, "bytecode output limit");
        return false;
    }
    if (!mo_buffer_append(out, header, sizeof header)) {
        mo_text_refuse(r, 0, "no memory");
        return false;
    }
    for (unsigned s = 0; s < MO_SECTION_COUNT; s++) {
        const struct mo_text_table *t = &r->tables[s];
        if (s == MO_SECTION_SERVICES && t->count == 0)
            continue;
        if (!mo_buffer_append_le32(out, t->count)) {
            mo_text_refuse(r, 0, "no memory");
            return false;
        }
        for (uint32_t i = 0; i < t->count; i++) {
            if (!mo_buffer_append(out, t->entries[i].bytes, t->entries[i].length)) {
                mo_text_refuse(r, 0, "no memory");
                return false;
            }
        }
    }
    uint64_t hash = MO_BYTECODE_FNV_OFFSET;
    for (size_t i = sizeof header; i < out->length; i++)
        hash = (hash ^ out->bytes[i]) * MO_BYTECODE_FNV_PRIME;
    for (unsigned i = 0; i < 8; i++)
        out->bytes[12 + i] = (uint8_t)(hash >> (8 * i));
    struct mo_load_error error;
    struct mo_bytecode_error_site site;
    struct mo_image *image = mo_bytecode_load_site(out->bytes, out->length, &site, &error);
    if (image == NULL) {
        uint32_t source = error.kind == MO_BYTECODE_NO_MEMORY ? 0 : text_offset(r, error.offset);
        if (error.offset == MO_BYTECODE_HEADER_SIZE && site.section < MO_SECTION_COUNT) {
            const struct mo_text_table *table = &r->tables[site.section];
            source = site.entry < table->count ? table->entries[site.entry].source : table->source;
        }
        mo_text_refuse(r, source, error.reason);
        return false;
    }
    mo_image_free(image);
    return true;
}

bool mo_bytecode_assemble(const char *text, size_t length, struct mo_buffer *out,
                          struct mo_assemble_error *error_out) {
    MO_ASSERT((text != NULL || length == 0) && out != NULL && out->length == 0 &&
              error_out != NULL);
    if (length > MO_LIMIT_BYTECODE_TEXT_SIZE) {
        *error_out = (struct mo_assemble_error){0, "bytecode text size limit"};
        return false;
    }
    struct mo_text_reader r = {.text = text == NULL ? "" : text,
                               .length = (uint32_t)length,
                               .error = error_out,
                               .wire_size = MO_BYTECODE_HEADER_SIZE + 4 * MO_SECTION_SERVICES};
    r.last_reference = &r.references;
    size_t bounded = length < MO_LIMIT_BYTECODE_SIZE ? length : MO_LIMIT_BYTECODE_SIZE;
    mo_arena_init(&r.arena, mo_bytecode_work_budget(bounded));
    mo_buffer_init(&r.raw, MO_LIMIT_BYTECODE_SIZE);
    mo_buffer_init(&r.positions, 2 * (size_t)MO_LIMIT_BYTECODE_SIZE);
    mo_text_next(&r);
    bool ok = (r.token.kind != 'n' || mo_text_end_line(&r)) &&
              mo_text_take(&r, "bytecode", "expected bytecode header") &&
              mo_text_take(&r, "0.2", "expected bytecode version 0.2");
    if (ok) {
        r.shorthand = mo_text_is(&r, "shorthand");
        ok = mo_text_take(&r, r.shorthand ? "shorthand" : "indexed",
                          "expected indexed or shorthand") &&
             mo_text_end_line(&r);
    }
    if (ok && r.shorthand) {
        ok = mo_text_shorthand(&r);
    } else if (ok) {
        for (unsigned s = 0; s < MO_SECTION_SERVICES && ok; s++) {
            ok = mo_text_indexed_section(&r, (enum mo_bytecode_section)s);
        }
    }
    if (ok && mo_text_is(&r, "services")) {
        r.wire_size += 4;
        ok = mo_text_indexed_section(&r, MO_SECTION_SERVICES);
        if (ok && r.tables[MO_SECTION_SERVICES].count == 0) {
            mo_text_refuse(&r, r.tables[MO_SECTION_SERVICES].source, "empty service extension");
            ok = false;
        }
    }
    if (ok && r.token.kind != 0) {
        mo_text_refuse(&r, r.token.start, "expected end of text");
        ok = false;
    }
    if (ok)
        ok = output(&r, out);
    if (!ok)
        mo_buffer_clear(out);
    mo_buffer_free(&r.positions);
    mo_buffer_free(&r.raw);
    mo_arena_free(&r.arena);
    return ok;
}
