#include "check/map_keys.h"

static unsigned digit(uint8_t c) {
    return c >= 'a'   ? (unsigned)(c - 'a') + 10
           : c >= 'A' ? (unsigned)(c - 'A') + 10
                      : (unsigned)(c - '0');
}

static MO_MUST_USE void *allocate(const struct mo_map_key_context *p, size_t bytes) {
    if (!p->work(p->context, bytes))
        return NULL;
    return p->allocate(p->context, bytes);
}

static MO_MUST_USE bool number(const struct mo_map_key_context *p, const struct mo_syntax *s,
                               bool negative, struct mo_map_literal_key *out) {
    size_t length = s->span.end - s->span.start;
    unsigned base = length > 2 && s->text[1] == 'x' ? 16 : length > 2 && s->text[1] == 'b' ? 2 : 10;
    /* Little-endian decimal digits preserve arbitrary BigInt magnitudes.
     * Two decimal digits per source digit suffice even for hexadecimal. */
    uint8_t *digits = allocate(p, 2 * length + 1);
    if (digits == NULL || !p->work(p->context, length))
        return false;
    size_t used = 0, places = 0;
    if (base == 10) {
        bool fraction = s->kind == MO_S_DECIMAL;
        for (size_t i = length; i != 0; i--) {
            uint8_t c = s->text[i - 1];
            if (c == '_')
                continue;
            if (c == '.') {
                fraction = false;
                continue;
            }
            digits[used++] = (uint8_t)digit(c);
            if (fraction)
                places++;
        }
    } else {
        used = 1;
        digits[0] = 0;
        for (size_t i = 2; i < length; i++) {
            if (s->text[i] == '_')
                continue;
            unsigned carry = digit(s->text[i]);
            MO_ASSERT(carry < base);
            if (!p->work(p->context, used + 2))
                return false;
            for (size_t j = 0; j < used; j++) {
                unsigned value = digits[j] * base + carry;
                digits[j] = (uint8_t)(value % 10);
                carry = value / 10;
            }
            while (carry != 0) {
                MO_ASSERT(used < 2 * length + 1);
                digits[used++] = (uint8_t)(carry % 10);
                carry /= 10;
            }
        }
    }
    MO_ASSERT(used != 0);
    /* Remove leading magnitude zeroes, then insignificant fractional zeroes. */
    if (!p->work(p->context, 2 * used))
        return false;
    while (used > 1 && digits[used - 1] == 0)
        used--;
    size_t first = 0;
    if (used == 1 && digits[0] == 0) {
        negative = false;
        places = 0;
    } else {
        while (places != 0 && digits[first] == 0) {
            first++;
            used--;
            places--;
        }
    }
    size_t size = (used > places ? used : places) + 2;
    uint8_t *bytes = allocate(p, size);
    if (bytes == NULL || !p->work(p->context, size))
        return false;
    size_t at = 0;
    if (used <= places) {
        bytes[at++] = '0';
        bytes[at++] = '.';
        for (size_t i = used; i < places; i++)
            bytes[at++] = '0';
    }
    for (size_t i = used; i != 0; i--) {
        bytes[at++] = (uint8_t)('0' + digits[first + i - 1]);
        if (places != 0 && i - 1 == places)
            bytes[at++] = '.';
    }
    MO_ASSERT(at <= size);
    *out = (struct mo_map_literal_key){bytes, at, MO_MAP_KEY_NUMBER, negative};
    return true;
}

static size_t encode(uint32_t c, uint8_t *out) {
    if (c < 0x80) {
        out[0] = (uint8_t)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (uint8_t)(0xc0u | (c >> 6));
        out[1] = (uint8_t)(0x80u | (c & 63u));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (uint8_t)(0xe0u | (c >> 12));
        out[1] = (uint8_t)(0x80u | ((c >> 6) & 63u));
        out[2] = (uint8_t)(0x80u | (c & 63u));
        return 3;
    }
    out[0] = (uint8_t)(0xf0u | (c >> 18));
    out[1] = (uint8_t)(0x80u | ((c >> 12) & 63u));
    out[2] = (uint8_t)(0x80u | ((c >> 6) & 63u));
    out[3] = (uint8_t)(0x80u | (c & 63u));
    return 4;
}

static MO_MUST_USE bool decoded(const struct mo_map_key_context *p, const struct mo_syntax *s,
                                struct mo_map_literal_key *out) {
    size_t length = s->span.end - s->span.start;
    /* Keep these source text rules aligned with lower/scalar.c: decoded.
     * Both ordinary escapes and triple-text indentation affect equality. */
    size_t quote =
        length >= 6 && s->text[0] == '"' && s->text[1] == '"' && s->text[2] == '"' ? 3 : 1;
    MO_ASSERT(length >= 2 * quote);
    if (!p->work(p->context, 3 * length))
        return false;
    size_t begin = quote, end = length - quote, indent = 0;
    if (quote == 3) {
        while (begin < end && s->text[begin] != '\n')
            begin++;
        MO_ASSERT(begin < end);
        begin++;
        size_t closing = end;
        while (closing > begin && s->text[closing - 1] == ' ')
            closing--;
        indent = end - closing;
        end = closing > begin ? closing - 1 : begin;
        if (end > begin && s->text[end - 1] == '\r')
            end--;
    }
    uint8_t *bytes = allocate(p, end - begin + 1);
    if (bytes == NULL)
        return false;
    size_t at = 0;
    bool line_start = quote == 3;
    for (size_t i = begin; i < end; i++) {
        if (line_start) {
            size_t spaces = 0;
            while (i < end && spaces < indent && s->text[i] == ' ') {
                i++;
                spaces++;
            }
            line_start = false;
            if (i == end)
                break;
        }
        uint8_t c = s->text[i];
        if (quote == 3 && c == '\r') {
            MO_ASSERT(i + 1 < end && s->text[i + 1] == '\n');
            i++;
            c = '\n';
        }
        if (quote == 3 && c == '\n')
            line_start = true;
        if (c != '\\') {
            bytes[at++] = c;
            continue;
        }
        MO_ASSERT(++i < end);
        c = s->text[i];
        if (c == 'u') {
            MO_ASSERT(s->text[++i] == '{');
            uint32_t scalar = 0;
            while (s->text[++i] != '}')
                scalar = scalar * 16 + digit(s->text[i]);
            at += encode(scalar, bytes + at);
        } else
            bytes[at++] = c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c;
    }
    *out = (struct mo_map_literal_key){.bytes = bytes,
                                       .length = at,
                                       .category = s->kind == MO_S_CHAR ? MO_MAP_KEY_CHAR
                                                                        : MO_MAP_KEY_TEXT};
    return true;
}

bool mo_map_literal_key(const struct mo_map_key_context *p, const struct mo_syntax *s,
                        struct mo_map_literal_key *out) {
    MO_ASSERT(p != NULL && p->work != NULL && p->allocate != NULL && s != NULL && out != NULL);
    *out = (struct mo_map_literal_key){0};
    bool negative = false, numeric = false;
    while (s->kind == MO_S_GROUP || s->kind == MO_S_NEGATE || s->kind == MO_S_PREFIX) {
        if (!p->work(p->context, 1))
            return false;
        if (s->kind == MO_S_PREFIX) {
            MO_ASSERT(s->count >= 3);
            if (!p->work(p->context, s->count - 1))
                return false;
            for (uint32_t i = 0; i + 1 < s->count; i++) {
                if (s->children[i]->token != MO_T_MINUS)
                    return true;
                negative = !negative;
            }
            numeric = true;
        } else if (s->kind == MO_S_NEGATE) {
            MO_ASSERT(s->count == 1);
            negative = !negative;
            numeric = true;
        } else
            MO_ASSERT(s->count == 1);
        s = s->children[s->count - 1];
    }
    if (s->kind == MO_S_INT || s->kind == MO_S_DECIMAL)
        return number(p, s, negative, out);
    if (numeric)
        return true;
    if (s->kind == MO_S_TEXT || s->kind == MO_S_CHAR)
        return decoded(p, s, out);
    if (s->kind == MO_S_TRUE || s->kind == MO_S_FALSE)
        *out = (struct mo_map_literal_key){.bytes =
                                               (const uint8_t *)(s->kind == MO_S_TRUE ? "1" : "0"),
                                           .length = 1,
                                           .category = MO_MAP_KEY_BOOL};
    return true;
}
