#include "prim/internal.h"

#include <string.h>

struct text {
    const uint8_t *bytes;
    size_t length;
};

static MO_MUST_USE struct text view(struct mo_cell value) {
    struct text t;
    t.bytes = mo_value_data(value, &t.length);
    return t;
}

static MO_MUST_USE bool white(uint32_t c) {
    return c == ' ' || (c >= 9 && c <= 13);
}

static MO_MUST_USE bool scalar(struct text text, size_t *at, uint32_t *out) {
    MO_ASSERT(*at < text.length);
    uint32_t c = text.bytes[(*at)++], minimum = 0;
    unsigned following = 0;
    if (c >= 0xc2 && c <= 0xdf) {
        c &= 31;
        following = 1;
        minimum = 0x80;
    } else if (c >= 0xe0 && c <= 0xef) {
        c &= 15;
        following = 2;
        minimum = 0x800;
    } else if (c >= 0xf0 && c <= 0xf4) {
        c &= 7;
        following = 3;
        minimum = 0x10000;
    } else if (c >= 0x80)
        return false;
    if (following > text.length - *at)
        return false;
    for (unsigned i = 0; i < following; i++) {
        uint32_t byte = text.bytes[(*at)++];
        if (byte < 0x80 || byte > 0xbf)
            return false;
        c = c * 64 + (byte & 63);
    }
    if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
        return false;
    *out = c;
    return true;
}

static MO_MUST_USE unsigned encode(uint32_t c, uint8_t *out) {
    unsigned count = c < 0x80 ? 1U : c < 0x800 ? 2U : c < 0x10000 ? 3U : 4U;
    for (unsigned i = count; i > 1; i--) {
        out[i - 1] = (uint8_t)(0x80 | (c & 63));
        c >>= 6;
    }
    out[0] = (uint8_t)(c | (count == 1 ? 0 : count == 2 ? 0xc0 : count == 3 ? 0xe0 : 0xf0));
    return count;
}

static MO_MUST_USE size_t characters(struct text text) {
    size_t count = 0;
    for (size_t i = 0; i < text.length; i++)
        count += (text.bytes[i] & 0xc0) != 0x80 ? 1U : 0U;
    return count;
}

static MO_MUST_USE enum mo_prim_status make_text(struct mo_prim_call *c, const uint8_t *bytes,
                                                 size_t length, struct mo_cell *out) {
    return mo_value_text(c->heap, (const char *)bytes, length, out) ? MO_PRIM_RETURNED
                                                                    : MO_PRIM_OUT_OF_MEMORY;
}

static MO_MUST_USE enum mo_prim_status some_text(struct mo_prim_call *c, const uint8_t *bytes,
                                                 size_t length, struct mo_cell *out) {
    struct mo_cell text;
    if (!mo_value_text(c->heap, (const char *)bytes, length, &text))
        return MO_PRIM_OUT_OF_MEMORY;
    enum mo_prim_status status = mo_prim_option(c, true, text, out);
    uint32_t type = c->types->items[c->result].cases[1].fields[0].type;
    mo_value_drop(c->heap, type, text);
    return status;
}

static MO_MUST_USE bool piece(struct mo_prim_call *c, struct mo_cell *list, const uint8_t *bytes,
                              size_t length) {
    struct mo_cell text;
    if (!mo_value_text(c->heap, (const char *)bytes, length, &text))
        return false;
    bool ok = mo_list_append(c->heap, c->result, list, text);
    mo_value_drop(c->heap, c->types->items[c->result].parameters[0], text);
    return ok;
}

/* One prefix table per call; all matches are non-overlapping and each byte is
 * visited a constant number of times even for long, almost-equal separators. */
struct matcher {
    struct text pattern;
    uint32_t *failure;
};
static MO_MUST_USE bool matcher_init(struct matcher *m, struct mo_scratch *scratch, struct text p) {
    MO_ASSERT(p.length != 0);
    m->pattern = p;
    m->failure = mo_scratch_resize(scratch, NULL, 0, p.length * sizeof *m->failure);
    if (m->failure == NULL)
        return false;
    m->failure[0] = 0;
    uint32_t matched = 0;
    for (size_t i = 1; i < p.length; i++) {
        while (matched != 0 && p.bytes[i] != p.bytes[matched])
            matched = m->failure[matched - 1];
        if (p.bytes[i] == p.bytes[matched])
            matched++;
        m->failure[i] = matched;
    }
    return true;
}
static MO_MUST_USE bool next(const struct matcher *m, struct text t, size_t *at) {
    uint32_t matched = 0;
    for (; *at < t.length; (*at)++) {
        while (matched != 0 && t.bytes[*at] != m->pattern.bytes[matched])
            matched = m->failure[matched - 1];
        if (t.bytes[*at] == m->pattern.bytes[matched])
            matched++;
        if (matched == m->pattern.length) {
            (*at)++;
            return true;
        }
    }
    return false;
}

static MO_MUST_USE enum mo_prim_status matching(struct mo_prim_call *c, struct text text,
                                                struct text pattern, struct mo_cell *out) {
    bool contains = c->id == MO_PRIM_TEXT_CONTAINS, replace = c->id == MO_PRIM_TEXT_REPLACE;
    if (pattern.length == 0) {
        if (contains) {
            *out = mo_cell_bool(true);
            return MO_PRIM_RETURNED;
        }
        c->reason =
            replace ? "Text.replace of an empty text" : "Text.split with an empty separator";
        return MO_PRIM_OUTSIDE;
    }
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, c->heap);
    enum mo_prim_status status = MO_PRIM_OUT_OF_MEMORY;
    struct matcher m;
    bool searchable = pattern.length <= text.length;
    if (searchable && !matcher_init(&m, &scratch, pattern))
        goto done;
    size_t at = 0;
    if (contains) {
        *out = mo_cell_bool(searchable && next(&m, text, &at));
        status = MO_PRIM_RETURNED;
    } else if (replace) {
        size_t count = 0;
        while (searchable && next(&m, text, &at))
            count++;
        if (count == 0) {
            *out = mo_value_dup(c->heap, c->params[0], c->args[0]);
            status = MO_PRIM_RETURNED;
            goto done;
        }
        struct text replacement = view(c->args[2]);
        size_t length = text.length - count * pattern.length;
        if (!mo_prim_bytes_fit(c, length + count * replacement.length))
            goto done;
        length += count * replacement.length;
        uint8_t *bytes = mo_scratch_resize(&scratch, NULL, 0, length);
        if (bytes == NULL)
            goto done;
        size_t previous = 0, wrote = 0;
        at = 0;
        while (next(&m, text, &at)) {
            size_t start = at - pattern.length, n = start - previous;
            memcpy(bytes + wrote, text.bytes + previous, n);
            wrote += n;
            memcpy(bytes + wrote, replacement.bytes, replacement.length);
            wrote += replacement.length;
            previous = at;
        }
        memcpy(bytes + wrote, text.bytes + previous, text.length - previous);
        MO_ASSERT(wrote + text.length - previous == length);
        status = make_text(c, bytes, length, out);
    } else {
        struct mo_cell list;
        if (!mo_value_list(c->heap, c->result, NULL, 0, &list))
            goto done;
        size_t previous = 0;
        bool ok = true;
        while (ok && searchable && next(&m, text, &at)) {
            ok = piece(c, &list, text.bytes + previous, at - pattern.length - previous);
            previous = at;
        }
        ok = ok && piece(c, &list, text.bytes + previous, text.length - previous);
        if (ok) {
            *out = list;
            status = MO_PRIM_RETURNED;
        } else
            mo_value_drop(c->heap, c->result, list);
    }
done:
    mo_scratch_free(&scratch);
    return status;
}

static MO_MUST_USE enum mo_prim_status parts(struct mo_prim_call *c, struct text text,
                                             struct mo_cell *out) {
    struct mo_cell list;
    if (!mo_value_list(c->heap, c->result, NULL, 0, &list))
        return MO_PRIM_OUT_OF_MEMORY;
    bool words = c->id == MO_PRIM_TEXT_WORDS, ok = true;
    size_t start = 0;
    for (size_t i = 0; ok && i < text.length; i++) {
        if (words ? white(text.bytes[i]) : text.bytes[i] == '\n') {
            size_t end = i;
            if (!words && end > start && text.bytes[end - 1] == '\r')
                end--;
            if (!words || end > start)
                ok = piece(c, &list, text.bytes + start, end - start);
            start = i + 1;
        }
    }
    if (ok && start < text.length)
        ok = piece(c, &list, text.bytes + start, text.length - start);
    if (!ok) {
        mo_value_drop(c->heap, c->result, list);
        return MO_PRIM_OUT_OF_MEMORY;
    }
    *out = list;
    return MO_PRIM_RETURNED;
}

static MO_MUST_USE enum mo_prim_status quoted(struct mo_prim_call *c, struct text text,
                                              uint8_t delimiter, struct mo_cell *out) {
    size_t length = 2;
    for (size_t i = 0; i < text.length; i++) {
        uint8_t b = text.bytes[i];
        size_t extra = b < 32 || b == 127 ? (b < 16 ? 5U : 6U)
                       : b == delimiter || b == '\\' ||
                               (b == '#' && i + 1 < text.length && text.bytes[i + 1] == '{')
                           ? 2U
                           : 1U;
        if (b == '\n' || b == '\r' || b == '\t')
            extra = 2;
        if (!mo_prim_bytes_fit(c, length + extra))
            return MO_PRIM_OUT_OF_MEMORY;
        length += extra;
    }
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, c->heap);
    uint8_t *bytes = mo_scratch_resize(&scratch, NULL, 0, length);
    enum mo_prim_status status = MO_PRIM_OUT_OF_MEMORY;
    if (bytes != NULL) {
        size_t at = 0;
        bytes[at++] = delimiter;
        static const char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < text.length; i++) {
            uint8_t b = text.bytes[i];
            if (b == '\n' || b == '\r' || b == '\t') {
                bytes[at++] = '\\';
                bytes[at++] = b == '\n' ? 'n' : b == '\r' ? 'r' : 't';
            } else if (b < 32 || b == 127) {
                memcpy(bytes + at, "\\u{", 3);
                at += 3;
                if (b >= 16)
                    bytes[at++] = (uint8_t)hex[b >> 4];
                bytes[at++] = (uint8_t)hex[b & 15];
                bytes[at++] = '}';
            } else {
                if (b == delimiter || b == '\\' ||
                    (b == '#' && i + 1 < text.length && text.bytes[i + 1] == '{'))
                    bytes[at++] = '\\';
                bytes[at++] = b;
            }
        }
        bytes[at++] = delimiter;
        MO_ASSERT(at == length);
        status = make_text(c, bytes, length, out);
    }
    mo_scratch_free(&scratch);
    return status;
}

static MO_MUST_USE enum mo_prim_status char_primitive(struct mo_prim_call *c, struct mo_cell *out) {
    uint32_t ch = mo_cell_get_char(c->args[0]);
    bool digit = ch >= '0' && ch <= '9', lower = ch >= 'a' && ch <= 'z',
         upper = ch >= 'A' && ch <= 'Z';
    switch ((unsigned)c->id) {
    case MO_PRIM_CHAR_CODE:
        *out = mo_cell_int(ch);
        return MO_PRIM_RETURNED;
    case MO_PRIM_CHAR_IS_ASCII_DIGIT:
        *out = mo_cell_bool(digit);
        return MO_PRIM_RETURNED;
    case MO_PRIM_CHAR_IS_ASCII_LETTER:
        *out = mo_cell_bool(lower || upper);
        return MO_PRIM_RETURNED;
    case MO_PRIM_CHAR_IS_ASCII_WHITE_SPACE:
        *out = mo_cell_bool(white(ch));
        return MO_PRIM_RETURNED;
    case MO_PRIM_CHAR_ASCII_LOWERCASE:
        *out = mo_cell_char(upper ? ch + 32 : ch);
        return MO_PRIM_RETURNED;
    case MO_PRIM_CHAR_ASCII_UPPERCASE:
        *out = mo_cell_char(lower ? ch - 32 : ch);
        return MO_PRIM_RETURNED;
    case MO_PRIM_CHAR_TO_TEXT:
    case MO_PRIM_CHAR_QUOTED: {
        uint8_t bytes[4];
        unsigned length = encode(ch, bytes);
        return c->id == MO_PRIM_CHAR_QUOTED ? quoted(c, (struct text){bytes, length}, '\'', out)
                                            : make_text(c, bytes, length, out);
    }
    default:
        MO_UNREACHABLE();
    }
}

static MO_MUST_USE enum mo_prim_status byte_primitive(struct mo_prim_call *c, struct mo_cell *out) {
    struct text bytes = view(c->args[0]);
    if (c->id == MO_PRIM_BYTES_SIZE) {
        *out = mo_cell_int((int64_t)bytes.length);
        return MO_PRIM_RETURNED;
    }
    if (c->id == MO_PRIM_BYTES_GET) {
        int64_t index = mo_cell_get_int(c->args[1]);
        bool valid = index >= 0 && (uint64_t)index < bytes.length;
        return mo_prim_option(c, valid, mo_cell_byte(valid ? bytes.bytes[(size_t)index] : 0), out);
    }
    MO_ASSERT(c->id == MO_PRIM_BYTES_DECODE_UTF8);
    for (size_t at = 0; at < bytes.length;) {
        uint32_t ch;
        if (!scalar(bytes, &at, &ch))
            return mo_prim_option(c, false, mo_cell_unit(), out);
    }
    return some_text(c, bytes.bytes, bytes.length, out);
}

static MO_MUST_USE enum mo_prim_status from_chars(struct mo_prim_call *c, struct mo_cell *out) {
    uint32_t count = mo_list_size(c->args[0]);
    size_t length = 0;
    for (uint32_t i = 0; i < count; i++) {
        struct mo_cell ch;
        MO_ASSERT(mo_list_get(c->args[0], i, &ch));
        uint8_t bytes[4];
        unsigned n = encode(mo_cell_get_char(ch), bytes);
        if (!mo_prim_bytes_fit(c, length + n))
            return MO_PRIM_OUT_OF_MEMORY;
        length += n;
    }
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, c->heap);
    uint8_t *bytes = mo_scratch_resize(&scratch, NULL, 0, length);
    enum mo_prim_status status = MO_PRIM_OUT_OF_MEMORY;
    if (bytes != NULL) {
        size_t at = 0;
        for (uint32_t i = 0; i < count; i++) {
            struct mo_cell ch;
            MO_ASSERT(mo_list_get(c->args[0], i, &ch));
            at += encode(mo_cell_get_char(ch), bytes + at);
        }
        status = make_text(c, bytes, length, out);
    }
    mo_scratch_free(&scratch);
    return status;
}

enum mo_prim_status mo_prim_text(struct mo_prim_call *c, struct mo_cell *out) {
    if (c->id == MO_PRIM_TEXT_STRUCTURAL)
        return mo_prim_print(c, c->params[0], c->args[0], out);
    if (c->id == MO_PRIM_TEXT_TO_INT || c->id == MO_PRIM_TEXT_TO_DECIMAL ||
        c->id == MO_PRIM_TEXT_TO_FLOAT)
        return mo_prim_parse(c, out);
    if (c->id == MO_PRIM_TEXT_COMPARE || c->id == MO_PRIM_BYTES_COMPARE) {
        int order;
        if (!mo_value_compare(c->heap, c->params[0], c->args[0], c->args[1], &order))
            return MO_PRIM_OUT_OF_MEMORY;
        *out = mo_cell_case(order < 0 ? 0U : order > 0 ? 2U : 1U, mo_cell_unit());
        return MO_PRIM_RETURNED;
    }
    if (c->id >= MO_PRIM_CHAR_CODE && c->id <= MO_PRIM_CHAR_QUOTED)
        return char_primitive(c, out);
    if (c->id >= MO_PRIM_BYTES_SIZE && c->id <= MO_PRIM_BYTES_DECODE_UTF8)
        return byte_primitive(c, out);
    if (c->id == MO_PRIM_TEXT_FROM_CHARS)
        return from_chars(c, out);
    struct text t = view(c->args[0]);
    switch ((unsigned)c->id) {
    case MO_PRIM_TEXT_BYTE_COUNT:
        *out = mo_cell_int((int64_t)t.length);
        return MO_PRIM_RETURNED;
    case MO_PRIM_TEXT_CHAR_COUNT:
        *out = mo_cell_int((int64_t)characters(t));
        return MO_PRIM_RETURNED;
    case MO_PRIM_TEXT_IS_EMPTY:
        *out = mo_cell_bool(t.length == 0);
        return MO_PRIM_RETURNED;
    case MO_PRIM_TEXT_BYTES:
        return mo_value_bytes(c->heap, t.bytes, t.length, out) ? MO_PRIM_RETURNED
                                                               : MO_PRIM_OUT_OF_MEMORY;
    case MO_PRIM_TEXT_CHARS: {
        struct mo_cell list;
        if (!mo_value_list(c->heap, c->result, NULL, 0, &list))
            return MO_PRIM_OUT_OF_MEMORY;
        for (size_t at = 0; at < t.length;) {
            uint32_t ch;
            MO_ASSERT(scalar(t, &at, &ch));
            if (!mo_list_append(c->heap, c->result, &list, mo_cell_char(ch))) {
                mo_value_drop(c->heap, c->result, list);
                return MO_PRIM_OUT_OF_MEMORY;
            }
        }
        *out = list;
        return MO_PRIM_RETURNED;
    }
    case MO_PRIM_TEXT_LINES:
    case MO_PRIM_TEXT_WORDS:
        return parts(c, t, out);
    case MO_PRIM_TEXT_SPLIT:
    case MO_PRIM_TEXT_CONTAINS:
    case MO_PRIM_TEXT_REPLACE:
        return matching(c, t, view(c->args[1]), out);
    case MO_PRIM_TEXT_TRIM:
    case MO_PRIM_TEXT_TRIM_START:
    case MO_PRIM_TEXT_TRIM_END: {
        size_t start = 0, end = t.length;
        if (c->id != MO_PRIM_TEXT_TRIM_END)
            while (start < end && white(t.bytes[start]))
                start++;
        if (c->id != MO_PRIM_TEXT_TRIM_START)
            while (end > start && white(t.bytes[end - 1]))
                end--;
        struct mo_cell value = c->args[0];
        if (!mo_text_slice(c->heap, &value, start, end - start))
            return MO_PRIM_OUT_OF_MEMORY;
        c->moved = 1;
        *out = value;
        return MO_PRIM_RETURNED;
    }
    case MO_PRIM_TEXT_STARTS_WITH:
    case MO_PRIM_TEXT_ENDS_WITH:
    case MO_PRIM_TEXT_DROP_PREFIX:
    case MO_PRIM_TEXT_DROP_SUFFIX: {
        struct text p = view(c->args[1]);
        bool prefix = c->id == MO_PRIM_TEXT_STARTS_WITH || c->id == MO_PRIM_TEXT_DROP_PREFIX;
        bool found = p.length <= t.length &&
                     memcmp(t.bytes + (prefix ? 0 : t.length - p.length), p.bytes, p.length) == 0;
        if (c->id == MO_PRIM_TEXT_STARTS_WITH || c->id == MO_PRIM_TEXT_ENDS_WITH) {
            *out = mo_cell_bool(found);
            return MO_PRIM_RETURNED;
        }
        return found ? some_text(c, t.bytes + (prefix ? p.length : 0), t.length - p.length, out)
                     : mo_prim_option(c, false, mo_cell_unit(), out);
    }
    case MO_PRIM_TEXT_QUOTED:
        return quoted(c, t, '"', out);
    default:
        break;
    }
    if (c->id == MO_PRIM_TEXT_CONCAT) {
        struct text right = view(c->args[1]);
        if (!mo_prim_bytes_fit(c, t.length + right.length))
            return MO_PRIM_OUT_OF_MEMORY;
        struct mo_cell value = c->args[0];
        if (!mo_text_concat(c->heap, &value, c->args[1]))
            return MO_PRIM_OUT_OF_MEMORY;
        c->moved = 1;
        *out = value;
        return MO_PRIM_RETURNED;
    }
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, c->heap);
    enum mo_prim_status status = MO_PRIM_OUT_OF_MEMORY;
    size_t length = t.length;
    int64_t count = 0;
    if (c->id == MO_PRIM_TEXT_REPEAT) {
        count = mo_cell_get_int(c->args[1]);
        if (count < 0) {
            c->reason = "Text.repeat with a negative count";
            status = MO_PRIM_OUTSIDE;
            goto done;
        }
        if (t.length != 0 && (uint64_t)count > MO_LIMIT_VALUE_BYTES / t.length) {
            c->reason = "result exceeds MO_LIMIT_VALUE_BYTES (u32 byte length)";
            goto done;
        }
        length = t.length == 0 ? 0 : t.length * (size_t)count;
    } else if (c->id == MO_PRIM_TEXT_PAD_LEFT || c->id == MO_PRIM_TEXT_PAD_RIGHT) {
        int64_t width = mo_cell_get_int(c->args[1]);
        size_t chars = characters(t);
        count = width > (int64_t)chars ? width - (int64_t)chars : 0;
        if (!mo_prim_bytes_fit(c, length + (uint64_t)count))
            goto done;
        length += (size_t)count;
    } else
        MO_ASSERT(c->id == MO_PRIM_TEXT_ASCII_LOWERCASE || c->id == MO_PRIM_TEXT_ASCII_UPPERCASE);
    uint8_t *bytes = mo_scratch_resize(&scratch, NULL, 0, length);
    if (bytes == NULL)
        goto done;
    if (c->id == MO_PRIM_TEXT_REPEAT) {
        size_t wrote = length == 0 ? 0 : t.length;
        memcpy(bytes, t.bytes, wrote);
        while (wrote < length) {
            size_t n = wrote < length - wrote ? wrote : length - wrote;
            memcpy(bytes + wrote, bytes, n);
            wrote += n;
        }
    } else if (c->id == MO_PRIM_TEXT_PAD_LEFT || c->id == MO_PRIM_TEXT_PAD_RIGHT) {
        size_t spaces = (size_t)count;
        bool left = c->id == MO_PRIM_TEXT_PAD_LEFT;
        memset(bytes + (left ? 0 : t.length), ' ', spaces);
        memcpy(bytes + (left ? spaces : 0), t.bytes, t.length);
    } else {
        memcpy(bytes, t.bytes, t.length);
        for (size_t i = 0; i < length; i++) {
            if (c->id == MO_PRIM_TEXT_ASCII_LOWERCASE && bytes[i] >= 'A' && bytes[i] <= 'Z')
                bytes[i] += 32;
            if (c->id == MO_PRIM_TEXT_ASCII_UPPERCASE && bytes[i] >= 'a' && bytes[i] <= 'z')
                bytes[i] -= 32;
        }
    }
    status = make_text(c, bytes, length, out);
done:
    mo_scratch_free(&scratch);
    return status;
}
