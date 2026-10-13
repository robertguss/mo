#include "bytecode/assemble.h"

#include <string.h>

static MO_MUST_USE int hex(unsigned char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool mo_text_quote(struct mo_text_reader *r, struct mo_bytecode_string *out) {
    struct mo_text_token token = r->token;
    if (token.kind != 'q' && token.kind != 'x') {
        mo_text_refuse(r, token.start, "expected quoted string or hex bytes");
        return false;
    }
    uint32_t prefix = token.kind == 'q' ? 1 : 4;
    uint32_t start = token.start + prefix, end = token.start + token.length - 1, n = 0;
    if (token.kind == 'x' && (end - start) % 2 != 0) {
        mo_text_refuse(r, end, "expected even number of hex digits");
        return false;
    }
    /* Count first: a long escaped string must not allocate its spelling's size. */
    for (uint32_t i = start; i < end; i++) {
        unsigned char c = (unsigned char)r->text[i];
        if (token.kind == 'x') {
            if (hex(c) < 0 || hex((unsigned char)r->text[i + 1]) < 0) {
                mo_text_refuse(r, i, "expected hex byte");
                return false;
            }
            i++;
        } else if (c == '\\') {
            i++;
            if (i >= end) {
                mo_text_refuse(r, i, "expected string escape");
                return false;
            }
            c = (unsigned char)r->text[i];
            if (c == 'x') {
                if (end - i <= 2 || hex((unsigned char)r->text[i + 1]) < 0 ||
                    hex((unsigned char)r->text[i + 2]) < 0) {
                    mo_text_refuse(r, i, "expected two hex digits after \\x");
                    return false;
                }
                i += 2;
            } else if (c != '\\' && c != '"') {
                mo_text_refuse(r, i, "expected quote, backslash or hex escape");
                return false;
            }
        } else if (c < 0x20 || c == 0x7f) {
            mo_text_refuse(r, i, "expected printable quoted byte or escape");
            return false;
        }
        if (++n > MO_LIMIT_STRING_SIZE) {
            mo_text_refuse(r, i, "expected string within byte limit");
            return false;
        }
    }
    uint8_t *bytes = mo_arena_allocate(&r->arena, (size_t)n + 1, 1);
    if (bytes == NULL) {
        mo_text_refuse(r, 0, "no memory");
        return false;
    }
    n = 0;
    for (uint32_t i = start; i < end; i++) {
        uint8_t c = (uint8_t)r->text[i];
        if (token.kind == 'x') {
            c = (uint8_t)(16 * hex((unsigned char)r->text[i]) + hex((unsigned char)r->text[i + 1]));
            i++;
        } else if (c == '\\') {
            c = (uint8_t)r->text[++i];
            if (c == 'x') {
                c = (uint8_t)(16 * hex((unsigned char)r->text[i + 1]) +
                              hex((unsigned char)r->text[i + 2]));
                i += 2;
            }
        }
        bytes[n++] = c;
    }
    bytes[n] = 0;
    if (token.kind == 'q' && !mo_bytecode_utf8(bytes, n)) {
        mo_text_refuse(r, token.start, "expected valid UTF-8 string");
        return false;
    }
    *out = (struct mo_bytecode_string){n, bytes};
    mo_text_next(r);
    return !r->failed;
}

static MO_MUST_USE bool integer(struct mo_text_reader *r, enum mo_type_kind kind, mo_u128 *out) {
    struct mo_text_token t = r->token;
    const char *s = r->text + t.start;
    bool negative = t.length != 0 && s[0] == '-';
    uint32_t i = negative ? 1 : 0, fraction = 0;
    bool decimal = kind == MO_TYPE_DECIMAL, point = false;
    mo_u128 maximum = decimal                ? ((mo_u128)1 << 127) - 1
                      : kind == MO_TYPE_BYTE ? UINT8_MAX
                                             : (mo_u128)INT64_MAX + (negative ? 1U : 0U);
    mo_u128 value = 0;
    if (t.kind != 'w' || i == t.length || (kind == MO_TYPE_BYTE && negative)) {
        mo_text_refuse(r, t.start, "expected integer literal in type bounds");
        return false;
    }
    for (; i < t.length; i++) {
        char c = s[i];
        if (decimal && c == '.' && !point && i != (negative ? 1U : 0U) && i + 1 < t.length) {
            point = true;
            continue;
        }
        if (c < '0' || c > '9' || (point && ++fraction > 18)) {
            mo_text_refuse(r, t.start + i,
                           decimal ? "expected decimal with at most 18 fraction digits"
                                   : "expected decimal integer digit");
            return false;
        }
        unsigned digit = (unsigned)(c - '0');
        if (value > (maximum - digit) / 10) {
            mo_text_refuse(r, t.start, "expected numeric literal in type bounds");
            return false;
        }
        value = value * 10 + digit;
    }
    if (decimal) {
        for (; fraction < 18; fraction++) {
            if (value > maximum / 10) {
                mo_text_refuse(r, t.start, "expected Decimal coefficient in bounds");
                return false;
            }
            value *= 10;
        }
    }
    *out = negative ? -value : value;
    if (kind == MO_TYPE_TIME) {
        mo_u128 minimum = UINT64_C(62135596800000000), maximum_time = UINT64_C(253402300800000000);
        if (negative ? value > minimum : value >= maximum_time) {
            mo_text_refuse(r, t.start, "expected Time between years 1 and 9999");
            return false;
        }
    }
    mo_text_next(r);
    return !r->failed;
}

bool mo_text_payload(struct mo_text_reader *r, uint32_t type, bool shorthand) {
    (void)shorthand;
    const struct mo_text_table *types = &r->tables[MO_SECTION_TYPES];
    uint32_t source = r->token.start;
    if (type >= types->count) {
        mo_text_refuse(r, source, "expected existing literal type");
        return false;
    }
    uint32_t kind = mo_text_get(&types->entries[type], 0);
    uint8_t bytes[16] = {0};
    mo_u128 value = 0;
    if (kind == MO_TYPE_UNIT) {
        if (!mo_text_take(r, "(", "expected () for Unit") ||
            !mo_text_take(r, ")", "expected () for Unit"))
            return false;
    } else if (kind == MO_TYPE_BOOL) {
        static const char *const booleans[] = {"false", "true"};
        uint32_t boolean;
        if (!mo_text_enum(r, booleans, 2, "expected true or false", &boolean))
            return false;
        value = boolean;
    } else if (kind == MO_TYPE_CHAR) {
        struct mo_text_token t = r->token;
        if (t.kind != 'w' || t.length < 3 || memcmp(r->text + t.start, "U+", 2) != 0) {
            mo_text_refuse(r, source, "expected Char U+hex scalar");
            return false;
        }
        for (uint32_t i = 2; i < t.length; i++) {
            int digit = hex((unsigned char)r->text[t.start + i]);
            if (digit < 0 || value > (UINT32_MAX - (unsigned)digit) / 16) {
                mo_text_refuse(r, t.start + i, "expected Unicode scalar hex digit");
                return false;
            }
            value = value * 16 + (unsigned)digit;
        }
        if (value >= 0x110000 || (value >= 0xd800 && value <= 0xdfff)) {
            mo_text_refuse(r, source, "expected Unicode scalar value");
            return false;
        }
        mo_text_next(r);
    } else if (kind == MO_TYPE_FLOAT) {
        struct mo_bytecode_string bits;
        if (!mo_text_take(r, "bits", "expected bits hex for Float") || r->token.kind != 'x') {
            mo_text_refuse(r, r->token.start, "expected hex Float bits");
            return false;
        }
        if (!mo_text_quote(r, &bits))
            return false;
        if (bits.length != 8) {
            mo_text_refuse(r, source, "expected exactly eight Float bytes");
            return false;
        }
        return mo_text_bytes(r, bits.bytes, 8, source);
    } else if (kind == MO_TYPE_TEXT || kind == MO_TYPE_FUNCTION) {
        uint32_t index;
        return mo_text_take(r, kind == MO_TYPE_TEXT ? "text" : "closure",
                            kind == MO_TYPE_TEXT ? "expected text sN" : "expected closure fN") &&
               mo_text_ref(r, kind == MO_TYPE_TEXT ? 's' : 'f', false, &index) &&
               mo_text_word(r, index, source);
    } else if (kind == MO_TYPE_BYTES || kind == MO_TYPE_BIGINT) {
        struct mo_bytecode_string blob;
        if (!mo_text_take(r, kind == MO_TYPE_BYTES ? "bytes" : "bigint",
                          kind == MO_TYPE_BYTES ? "expected bytes hex"
                                                : "expected bigint sign hex"))
            return false;
        if (kind == MO_TYPE_BIGINT) {
            static const char *const signs[] = {"zero", "positive", "negative"};
            uint32_t sign;
            if (!mo_text_enum(r, signs, 3, "expected zero, positive or negative", &sign) ||
                !mo_text_word(r, sign, source))
                return false;
        }
        if (r->token.kind != 'x') {
            mo_text_refuse(r, r->token.start, "expected hex bytes");
            return false;
        }
        return mo_text_quote(r, &blob) && mo_text_word(r, blob.length, source) &&
               mo_text_bytes(r, blob.bytes, blob.length, source);
    } else if (kind == MO_TYPE_BYTE || kind == MO_TYPE_INT || kind == MO_TYPE_TIME ||
               kind == MO_TYPE_DURATION || kind == MO_TYPE_DECIMAL) {
        if (!integer(r, (enum mo_type_kind)kind, &value))
            return false;
    } else {
        mo_text_refuse(r, source, "expected type with a literal encoding");
        return false;
    }
    uint32_t width = kind == MO_TYPE_DECIMAL ? 16 : 8;
    for (uint32_t i = 0; i < width; i++)
        bytes[i] = (uint8_t)(value >> (8 * i));
    return mo_text_bytes(r, bytes, width, source);
}
