#define _POSIX_C_SOURCE 200809L
#include "bytecode/assemble.h"

#include <locale.h>
#include <stdlib.h>
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

static MO_MUST_USE bool integer(struct mo_text_reader *r, enum mo_type_kind kind, bool shorthand,
                                mo_u128 *out) {
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
    unsigned radix = 10;
    if (shorthand && !decimal && t.length - i > 2 && s[i] == '0' &&
        (s[i + 1] == 'x' || s[i + 1] == 'b')) {
        radix = s[i + 1] == 'x' ? 16 : 2;
        i += 2;
    }
    bool previous_digit = false;
    for (; i < t.length; i++) {
        char c = s[i];
        if (shorthand && !decimal && c == '_' && previous_digit && i + 1 < t.length) {
            previous_digit = false;
            continue;
        }
        if (decimal && c == '.' && !point && i != (negative ? 1U : 0U) && i + 1 < t.length) {
            point = true;
            continue;
        }
        int parsed = hex((unsigned char)c);
        if (parsed < 0 || (unsigned)parsed >= radix) {
            mo_text_refuse(r, t.start + i,
                           decimal ? "expected decimal digit"
                                   : "expected integer digit in literal radix");
            return false;
        }
        if (point && ++fraction > 18) {
            mo_text_refuse(r, t.start + i, "expected decimal with at most 18 fraction digits");
            return false;
        }
        unsigned digit = (unsigned)parsed;
        if (value > (maximum - digit) / radix) {
            mo_text_refuse(r, t.start, "expected numeric literal in type bounds");
            return false;
        }
        value = value * radix + digit;
        previous_digit = true;
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

static MO_MUST_USE bool character(struct mo_text_reader *r, mo_u128 *out) {
    struct mo_text_token t = r->token;
    const uint8_t *p = (const uint8_t *)r->text + t.start + 1;
    uint32_t n = t.length - 2, value = 0;
    if (n >= 2 && p[0] == '\\') {
        if (n == 2) {
            switch (p[1]) {
            case 'n':
                value = '\n';
                break;
            case 'r':
                value = '\r';
                break;
            case 't':
                value = '\t';
                break;
            case '0':
                value = 0;
                break;
            case '\\':
                value = '\\';
                break;
            case '\'':
                value = '\'';
                break;
            case '"':
                value = '"';
                break;
            default:
                goto invalid;
            }
        } else if (n >= 5 && n <= 10 && p[1] == 'u' && p[2] == '{' && p[n - 1] == '}') {
            for (uint32_t i = 3; i + 1 < n; i++) {
                int digit = hex(p[i]);
                if (digit < 0)
                    goto invalid;
                value = value * 16 + (unsigned)digit;
            }
        } else {
            goto invalid;
        }
    } else {
        if (n == 0 || n > 4 || !mo_bytecode_utf8(p, n))
            goto invalid;
        uint32_t width = p[0] < 0x80 ? 1 : p[0] < 0xe0 ? 2 : p[0] < 0xf0 ? 3 : 4;
        if (n != width)
            goto invalid;
        value = p[0] & (width == 1 ? 0x7fU : width == 2 ? 0x1fU : width == 3 ? 0xfU : 7U);
        for (uint32_t i = 1; i < width; i++)
            value = (value << 6) | (p[i] & 0x3fU);
    }
    if (value >= 0x110000 || (value >= 0xd800 && value <= 0xdfff))
        goto invalid;
    *out = value;
    mo_text_next(r);
    return !r->failed;
invalid:
    mo_text_refuse(r, t.start, "expected one Unicode scalar or Char escape");
    return false;
}

static MO_MUST_USE bool floating(struct mo_text_reader *r) {
    struct mo_text_token t = r->token;
    uint64_t bits = UINT64_C(0x7ff8000000000000);
    if (!mo_text_is(r, "nan")) {
        uint32_t i = 0;
        if (t.length != 0 && r->text[t.start] == '-')
            i++;
        bool point = false, exponent = false, digit = false;
        for (; i < t.length; i++) {
            char c = r->text[t.start + i];
            if (c >= '0' && c <= '9') {
                digit = true;
            } else if (c == '.' && !point && !exponent && digit && i + 1 < t.length) {
                point = true;
                digit = false;
            } else if ((c == 'e' || c == 'E') && !exponent && digit) {
                exponent = true;
                digit = false;
                if (i + 1 < t.length &&
                    (r->text[t.start + i + 1] == '+' || r->text[t.start + i + 1] == '-'))
                    i++;
            } else {
                digit = false;
                break;
            }
        }
        if (t.kind != 'w' || !digit || i != t.length) {
            mo_text_refuse(r, t.start, "expected Float number or nan");
            return false;
        }
        char *spelling = mo_arena_allocate(&r->arena, (size_t)t.length + 1, 1);
        if (spelling == NULL) {
            mo_text_refuse(r, 0, "no memory");
            return false;
        }
        memcpy(spelling, r->text + t.start, t.length);
        spelling[t.length] = 0;
        locale_t locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
        if (locale == (locale_t)0) {
            mo_text_refuse(r, 0, "no memory for Float locale");
            return false;
        }
        locale_t old = uselocale(locale);
        if (old == (locale_t)0) {
            freelocale(locale);
            mo_text_refuse(r, 0, "cannot select Float locale");
            return false;
        }
        char *end;
        double value = strtod(spelling, &end);
        bool restored = uselocale(old) != (locale_t)0;
        /* A valid locale returned by uselocale remains valid until restored. */
        MO_ASSERT(restored && end == spelling + t.length && sizeof value == sizeof bits);
        freelocale(locale);
        memcpy(&bits, &value, sizeof bits);
    }
    uint8_t bytes[8];
    for (unsigned i = 0; i < 8; i++)
        bytes[i] = (uint8_t)(bits >> (8 * i));
    mo_text_next(r);
    return mo_text_bytes(r, bytes, sizeof bytes, t.start);
}

static MO_MUST_USE bool bigint(struct mo_text_reader *r) {
    struct mo_text_token t = r->token;
    const char *text = r->text + t.start;
    bool negative = t.length != 0 && text[0] == '-';
    uint32_t start = negative ? 1U : 0U;
    if (t.kind != 'w' || start == t.length) {
        mo_text_refuse(r, t.start, "expected BigInt decimal integer or bigint sign hex");
        return false;
    }
    if (t.length - start > MO_LIMIT_BYTECODE_BIGINT_DIGITS) {
        mo_text_refuse(r, t.start,
                       "expected BigInt within decimal digit limit (use bigint sign hex)");
        return false;
    }
    /* Nine decimal digits per step and base-2^32 limbs keep the bounded
     * conversion's work small even when a file is filled with large literals. */
    uint32_t capacity = (t.length - start) / 8 + 1, length = 0;
    uint32_t *limbs =
        mo_arena_allocate_array(&r->arena, capacity, sizeof *limbs, _Alignof(uint32_t));
    if (limbs == NULL) {
        mo_text_refuse(r, 0, "no memory");
        return false;
    }
    for (uint32_t i = start; i < t.length;) {
        uint32_t digits = t.length - i < 9 ? t.length - i : 9;
        uint64_t carry = 0, scale = 1;
        for (uint32_t j = 0; j < digits; j++, i++) {
            if (text[i] < '0' || text[i] > '9') {
                mo_text_refuse(r, t.start + i, "expected BigInt decimal digit");
                return false;
            }
            carry = carry * 10 + (unsigned)(text[i] - '0');
            scale *= 10;
        }
        for (uint32_t j = 0; j < length; j++) {
            carry += scale * limbs[j];
            limbs[j] = (uint32_t)carry;
            carry >>= 32;
        }
        if (carry != 0) {
            MO_ASSERT(length < capacity);
            limbs[length++] = (uint32_t)carry;
        }
    }
    uint32_t byte_length = length * 4;
    uint8_t *bytes = mo_arena_allocate(&r->arena, (size_t)byte_length + 1, 1);
    if (bytes == NULL) {
        mo_text_refuse(r, 0, "no memory");
        return false;
    }
    for (uint32_t i = 0; i < byte_length; i++)
        bytes[i] = (uint8_t)(limbs[i / 4] >> (8 * (i % 4)));
    while (byte_length != 0 && bytes[byte_length - 1] == 0)
        byte_length--;
    mo_text_next(r);
    return mo_text_word(r,
                        byte_length == 0 ? MO_BIGINT_ZERO
                        : negative       ? MO_BIGINT_NEGATIVE
                                         : MO_BIGINT_POSITIVE,
                        t.start) &&
           mo_text_word(r, byte_length, t.start) && mo_text_bytes(r, bytes, byte_length, t.start);
}

bool mo_text_payload(struct mo_text_reader *r, uint32_t type, bool shorthand) {
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
        if (shorthand && r->token.kind == 'c') {
            if (!character(r, &value))
                return false;
            goto scalar;
        }
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
        if (shorthand)
            return floating(r);
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
        if (shorthand && kind == MO_TYPE_BIGINT && !mo_text_is(r, "bigint"))
            return bigint(r);
        struct mo_bytecode_string blob;
        uint32_t sign = MO_BIGINT_ZERO;
        if (!mo_text_take(r, kind == MO_TYPE_BYTES ? "bytes" : "bigint",
                          kind == MO_TYPE_BYTES ? "expected bytes hex"
                                                : "expected bigint sign hex"))
            return false;
        if (kind == MO_TYPE_BIGINT) {
            static const char *const signs[] = {"zero", "positive", "negative"};
            if (!mo_text_enum(r, signs, 3, "expected zero, positive or negative", &sign) ||
                !mo_text_word(r, sign, source))
                return false;
        }
        if (r->token.kind != 'x') {
            mo_text_refuse(r, r->token.start, "expected hex bytes");
            return false;
        }
        if (!mo_text_quote(r, &blob))
            return false;
        if (kind == MO_TYPE_BIGINT && ((sign == MO_BIGINT_ZERO) != (blob.length == 0) ||
                                       (blob.length != 0 && blob.bytes[blob.length - 1] == 0))) {
            mo_text_refuse(r, source, "noncanonical bigint literal");
            return false;
        }
        return mo_text_word(r, blob.length, source) &&
               mo_text_bytes(r, blob.bytes, blob.length, source);
    } else if (kind == MO_TYPE_BYTE || kind == MO_TYPE_INT || kind == MO_TYPE_TIME ||
               kind == MO_TYPE_DURATION || kind == MO_TYPE_DECIMAL) {
        if (!integer(r, (enum mo_type_kind)kind, shorthand, &value))
            return false;
    } else {
        mo_text_refuse(r, source, "expected type with a literal encoding");
        return false;
    }
scalar:;
    uint32_t width = kind == MO_TYPE_DECIMAL ? 16 : 8;
    for (uint32_t i = 0; i < width; i++)
        bytes[i] = (uint8_t)(value >> (8 * i));
    return mo_text_bytes(r, bytes, width, source);
}
