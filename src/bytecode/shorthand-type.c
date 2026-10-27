#include "bytecode/shorthand.h"

#include <string.h>

static const char *const simple[] = {"Unit", "Bool",  "Byte",     "Int",     "Float",
                                     "Char", "Time",  "Duration", "Decimal", "Range",
                                     "Text", "Bytes", "BigInt"};

static MO_MUST_USE bool body(struct mo_shorthand *s, unsigned depth, uint32_t *out) {
    struct mo_text_reader *r = s->r;
    uint32_t source = r->token.start;
    bool tuple = mo_text_is(r, "("), function = mo_text_is(r, "fn");
    uint32_t kind = tuple ? MO_TYPE_TUPLE : function ? MO_TYPE_FUNCTION : MO_BYTECODE_NO_INDEX;
    for (uint32_t i = 0; i <= MO_TYPE_BIGINT; i++) {
        if (mo_text_is(r, simple[i])) {
            mo_text_next(r);
            return mo_text_word(r, i, source) && mo_short_pool(s, MO_SECTION_TYPES, source, out);
        }
    }
    for (uint32_t i = 0; i <= MO_CAP_SIMULATION; i++) {
        if (mo_text_is(r, mo_bytecode_capability_names[i])) {
            mo_text_next(r);
            return mo_text_word(r, MO_TYPE_CAPABILITY, source) && mo_text_word(r, i, source) &&
                   mo_short_pool(s, MO_SECTION_TYPES, source, out);
        }
    }
    if (mo_text_is(r, "env")) {
        mo_text_next(r);
        uint32_t function_index;
        return mo_text_take(r, "(", "expected ( after env") &&
               mo_short_reference(s, MO_SECTION_FUNCTIONS, 'f', &function_index) &&
               mo_text_take(r, ")", "expected ) after env function") &&
               mo_text_word(r, MO_TYPE_ENV, source) && mo_text_word(r, function_index, source) &&
               mo_short_pool(s, MO_SECTION_TYPES, source, out);
    }
    if (mo_text_is(r, "List"))
        kind = MO_TYPE_LIST;
    if (mo_text_is(r, "Set"))
        kind = MO_TYPE_SET;
    if (mo_text_is(r, "Map"))
        kind = MO_TYPE_MAP;
    if (kind == MO_BYTECODE_NO_INDEX) {
        struct mo_bytecode_string nominal;
        /* Type arguments are part of a nominal identity, not runtime dependencies. */
        return mo_short_name(s, false, &nominal) &&
               mo_short_lookup(s, MO_SECTION_TYPES, nominal, source, out);
    }
    if (kind != MO_BYTECODE_NO_INDEX && !mo_text_word(r, kind, source))
        return false;
    if (!tuple)
        mo_text_next(r);
    uint32_t count = 0, count_offset = (uint32_t)r->raw.length;
    if (mo_text_is(r, "(")) {
        mo_text_next(r);
        if (kind != MO_BYTECODE_NO_INDEX && !mo_text_word(r, 0, source))
            return false;
        if (!mo_text_is(r, ")")) {
            do {
                uint32_t child;
                uint32_t limit = function ? MO_LIMIT_BYTECODE_PARAMETERS : MO_LIMIT_FIELDS;
                if (++count > limit) {
                    mo_text_refuse(r, r->token.start, "expected type within parameter limit");
                    return false;
                }
                if (!mo_short_type(s, depth + 1, &child) ||
                    (kind != MO_BYTECODE_NO_INDEX && !mo_text_word(r, child, r->token.start)))
                    return false;
                if (mo_text_is(r, ")"))
                    break;
            } while (mo_text_take(r, ",", "expected , between type parameters"));
        }
        if (!mo_text_take(r, ")", "expected ) after type parameters"))
            return false;
        if (kind != MO_BYTECODE_NO_INDEX)
            mo_text_patch(r, count_offset, count);
    } else if (kind != MO_BYTECODE_NO_INDEX) {
        mo_text_refuse(r, r->token.start, "expected type parameters (");
        return false;
    }
    if ((tuple && count < 2) || ((kind == MO_TYPE_LIST || kind == MO_TYPE_SET) && count != 1) ||
        (kind == MO_TYPE_MAP && count != 2)) {
        mo_text_refuse(
            r, source,
            "expected tuple with at least two types, List/Set with one, or Map with two");
        return false;
    }
    if (function) {
        uint32_t result;
        if (!mo_text_take(r, ":", "expected : before function result") ||
            !mo_short_type(s, depth + 1, &result) || !mo_text_word(r, result, source))
            return false;
    }
    return mo_short_pool(s, MO_SECTION_TYPES, source, out);
}

bool mo_short_type(struct mo_shorthand *s, unsigned depth, uint32_t *out) {
    struct mo_text_reader *r = s->r;
    if (depth >= MO_LIMIT_NESTING_DEPTH) {
        mo_text_refuse(r, r->token.start, "expected type within nesting limit");
        return false;
    }
    if (r->token.kind == 'w' && r->text[r->token.start] == 't' &&
        mo_short_index((struct mo_bytecode_string){r->token.length,
                                                   (const uint8_t *)r->text + r->token.start})) {
        return mo_text_ref(r, 't', false, out);
    }
    struct mo_buffer raw = r->raw, positions = r->positions;
    mo_buffer_init(&r->raw, MO_LIMIT_BYTECODE_SIZE);
    mo_buffer_init(&r->positions, 2 * (size_t)MO_LIMIT_BYTECODE_SIZE);
    bool ok = body(s, depth, out);
    mo_buffer_free(&r->raw);
    mo_buffer_free(&r->positions);
    r->raw = raw;
    r->positions = positions;
    return ok;
}
