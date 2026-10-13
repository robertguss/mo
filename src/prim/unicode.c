#include "prim/internal.h"
#include "prim/unicode16-data.h"

#include <string.h>

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define SCALAR_MASK UINT32_C(0x1fffff)

/* Raw spans may be malformed. Refuse incomplete or non-scalar encodings
 * before width estimation, without reading beyond the span. */
static MO_MUST_USE bool checked_scalar(const uint8_t *bytes, size_t length, size_t *at,
                                       uint32_t *out) {
    MO_ASSERT(*at < length);
    size_t i = *at;
    uint32_t c = bytes[i++];
    if (c >= 128) {
        if (c < 0xc2 || c > 0xf4)
            return false;
        unsigned following = c < 0xe0 ? 1U : c < 0xf0 ? 2U : 3U;
        c &= following == 1 ? 31U : following == 2 ? 15U : 7U;
        if (following > length - i)
            return false;
        for (unsigned j = 0; j < following; j++) {
            uint32_t b = bytes[i++];
            if ((b & 0xc0) != 0x80)
                return false;
            c = c * 64 + (b & 63U);
        }
        uint32_t minimum = following == 1 ? 0x80U : following == 2 ? 0x800U : 0x10000U;
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
            return false;
    }
    *at = i;
    *out = c;
    return true;
}

/* Input cells already contain valid UTF-8. Unlike Bytes.decode_utf8 this is
 * not a validation path. Each call advances by exactly one complete scalar. */
static MO_MUST_USE uint32_t scalar(const uint8_t *bytes, size_t length, size_t *at) {
    MO_ASSERT(*at < length);
    uint32_t c = bytes[(*at)++];
    if (c < 128)
        return c;
    unsigned following = c < 0xe0 ? 1U : c < 0xf0 ? 2U : 3U;
    c &= following == 1 ? 31U : following == 2 ? 15U : 7U;
    MO_ASSERT(following <= length - *at);
    for (unsigned i = 0; i < following; i++)
        c = c * 64 + (bytes[(*at)++] & 63U);
    return c;
}

/* NULL counts the bytes before reserving exact, heap-charged output storage. */
static MO_MUST_USE unsigned encode(uint32_t c, uint8_t *out) {
    unsigned n = c < 0x80 ? 1U : c < 0x800 ? 2U : c < 0x10000 ? 3U : 4U;
    if (out != NULL) {
        for (unsigned i = n; i > 1; i--) {
            out[i - 1] = (uint8_t)(0x80 | (c & 63));
            c >>= 6;
        }
        out[0] = (uint8_t)(c | (n == 1 ? 0 : n == 2 ? 0xc0 : n == 3 ? 0xe0 : 0xf0));
    }
    return n;
}

static MO_MUST_USE uint32_t properties(uint32_t c) {
    if (c < 128)
        return mo_unicode_ascii[c];
    size_t lo = 0, hi = COUNT(mo_unicode_ranges);
    uint32_t result = 0;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const struct mo_unicode_range *r = &mo_unicode_ranges[mid];
        if (c < r->first)
            hi = mid;
        else if (c > r->last)
            lo = mid + 1;
        else {
            result = r->properties;
            break;
        }
    }
    if (c >= 0xac00 && c <= 0xd7a3)
        result = (result & ~15U) | ((c - 0xac00) % 28 == 0 ? MO_GCB_LV : MO_GCB_LVT);
    return result;
}

static MO_MUST_USE const struct mo_unicode_mapping *
mapping(uint32_t c, const struct mo_unicode_mapping *table, size_t count) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (table[mid].scalar < c)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < count && table[lo].scalar == c ? &table[lo] : NULL;
}

struct boundary {
    uint32_t previous;
    unsigned indic;
    bool started, odd_ri, pictographic_extend, pictographic_zwj;
};

/* UAX #29 revision 45, in rule order. These suffix states avoid rescanning
 * preceding Extend, regional indicators or Indic linkers at every boundary. */
static MO_MUST_USE bool boundary(struct boundary *s, uint32_t p) {
    uint32_t a = s->previous & 15U, b = p & 15U, indic = (p >> 4) & 3U;
    bool control_a = a == MO_GCB_CR || a == MO_GCB_LF || a == MO_GCB_CONTROL;
    bool control_b = b == MO_GCB_CR || b == MO_GCB_LF || b == MO_GCB_CONTROL;
    bool split;
    if (!s->started)
        split = true;
    else if (a == MO_GCB_CR && b == MO_GCB_LF)
        split = false;
    else if (control_a || control_b)
        split = true;
    else if (a == MO_GCB_L && (b == MO_GCB_L || b == MO_GCB_V || b == MO_GCB_LV || b == MO_GCB_LVT))
        split = false;
    else if ((a == MO_GCB_LV || a == MO_GCB_V) && (b == MO_GCB_V || b == MO_GCB_T))
        split = false;
    else if ((a == MO_GCB_LVT || a == MO_GCB_T) && b == MO_GCB_T)
        split = false;
    else if (b == MO_GCB_EXTEND || b == MO_GCB_ZWJ || b == MO_GCB_SPACINGMARK ||
             a == MO_GCB_PREPEND)
        split = false;
    else if (s->indic == 2 && indic == 1)
        split = false;
    else if (a == MO_GCB_ZWJ && s->pictographic_zwj && (p & MO_UCD_PICTOGRAPHIC) != 0)
        split = false;
    else if (a == MO_GCB_REGIONAL_INDICATOR && b == MO_GCB_REGIONAL_INDICATOR && s->odd_ri)
        split = false;
    else
        split = true;
    s->odd_ri = b == MO_GCB_REGIONAL_INDICATOR && !s->odd_ri;
    s->pictographic_zwj = b == MO_GCB_ZWJ && s->pictographic_extend;
    if ((p & MO_UCD_PICTOGRAPHIC) != 0)
        s->pictographic_extend = true;
    else if (b != MO_GCB_EXTEND)
        s->pictographic_extend = false;
    if (indic == 1)
        s->indic = 1;
    else if (indic == 3)
        s->indic = s->indic != 0 ? 2U : 0U;
    else if (indic != 2)
        s->indic = 0;
    s->previous = p;
    s->started = true;
    return split;
}

bool mo_unicode16_next_grapheme(const uint8_t *bytes, size_t length, size_t *length_out) {
    MO_ASSERT(length_out != NULL && (bytes != NULL || length == 0));
    if (length == 0)
        return false;
    struct boundary state = {0};
    size_t at = 0;
    while (at < length) {
        size_t here = at;
        uint32_t cp;
        if (!checked_scalar(bytes, length, &at, &cp))
            return false;
        if (boundary(&state, properties(cp)) && here != 0) {
            *length_out = here;
            return true;
        }
    }
    *length_out = length;
    return true;
}

static MO_MUST_USE bool piece(struct mo_prim_call *c, struct mo_cell *list, const uint8_t *bytes,
                              size_t length) {
    struct mo_cell text;
    if (!mo_value_text(c->heap, (const char *)bytes, length, &text))
        return false;
    bool ok = mo_list_append(c->heap, c->result, list, text);
    mo_value_drop(c->heap, c->params[0], text);
    return ok;
}

static MO_MUST_USE bool keycap(uint32_t c) {
    return c == '#' || c == '*' || (c >= '0' && c <= '9');
}

bool mo_unicode16_display_width(const uint8_t *bytes, size_t length, uint64_t *width_out) {
    MO_ASSERT(width_out != NULL && (bytes != NULL || length == 0));
    struct boundary state = {0};
    size_t at = 0;
    uint64_t total = 0;
    unsigned cluster_width = 0;
    uint32_t previous = 0, before_previous = 0;
    while (at < length) {
        uint32_t cp;
        if (!checked_scalar(bytes, length, &at, &cp))
            return false;
        uint32_t p = properties(cp);
        if (boundary(&state, p)) {
            if (cluster_width > UINT64_MAX - total)
                return false;
            total += cluster_width;
            cluster_width = 0;
            before_previous = previous = 0;
        }
        unsigned w = (p & MO_UCD_ZERO) != 0 ? 0U : (p & MO_UCD_WIDE) != 0 ? 2U : 1U;
        size_t peek = at;
        uint32_t next = 0;
        if (at < length && !checked_scalar(bytes, length, &peek, &next))
            return false;
        bool emoji = (p & MO_UCD_EMOJI) != 0;
        emoji =
            emoji || (next == 0xfe0f && (p & MO_UCD_VARIANT) != 0) ||
            (cp == 0x20e3 && (keycap(previous) || (previous == 0xfe0f && keycap(before_previous))));
        if (emoji)
            w = 2;
        if (w > cluster_width)
            cluster_width = w;
        before_previous = previous;
        previous = cp;
    }
    if (cluster_width > UINT64_MAX - total)
        return false;
    *width_out = total + cluster_width;
    return true;
}

static MO_MUST_USE enum mo_prim_status graphemes(struct mo_prim_call *c, const uint8_t *bytes,
                                                 size_t length, struct mo_cell *out) {
    struct mo_cell list;
    if (!mo_value_list(c->heap, c->result, NULL, 0, &list))
        return MO_PRIM_OUT_OF_MEMORY;
    struct boundary state = {0};
    size_t at = 0, start = 0;
    while (at < length) {
        size_t here = at;
        if (boundary(&state, properties(scalar(bytes, length, &at)))) {
            if (here != 0 && !piece(c, &list, bytes + start, here - start))
                goto no_memory;
            start = here;
        }
    }
    if (length != 0 && !piece(c, &list, bytes + start, length - start))
        goto no_memory;
    *out = list;
    return MO_PRIM_RETURNED;
no_memory:
    mo_value_drop(c->heap, c->result, list);
    return MO_PRIM_OUT_OF_MEMORY;
}

/* Sigma lookahead visits the following case-ignorable run once per sigma;
 * such runs cannot overlap. Preceding context is maintained in the forward pass. */
static MO_MUST_USE bool following_cased(const uint8_t *bytes, size_t length, size_t at) {
    while (at < length) {
        uint32_t p = properties(scalar(bytes, length, &at));
        if ((p & MO_UCD_IGNORABLE_CASE) == 0)
            return (p & MO_UCD_CASED) != 0;
    }
    return false;
}

static MO_MUST_USE enum mo_prim_status casing(struct mo_prim_call *c, const uint8_t *bytes,
                                              size_t length, struct mo_cell *out) {
    bool lower = c->id == MO_PRIM_UNICODE16_LOWERCASE;
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, c->heap);
    uint8_t *result = NULL;
    size_t needed = 0;
    enum mo_prim_status status = MO_PRIM_OUT_OF_MEMORY;
    for (unsigned pass = 0; pass < 2; pass++) {
        size_t at = 0, wrote = 0;
        bool preceding = false;
        while (at < length) {
            uint32_t cp = scalar(bytes, length, &at), p = properties(cp), one = cp;
            const struct mo_unicode_mapping *m =
                lower ? mapping(cp, mo_unicode_lower, COUNT(mo_unicode_lower))
                      : mapping(cp, mo_unicode_upper, COUNT(mo_unicode_upper));
            if (lower && cp == 0x3a3 && preceding && !following_cased(bytes, length, at)) {
                m = NULL;
                one = 0x3c2;
            }
            if ((p & MO_UCD_IGNORABLE_CASE) == 0)
                preceding = (p & MO_UCD_CASED) != 0;
            uint32_t count = m == NULL ? 1U : m->count;
            const uint32_t *values = m == NULL ? &one : &mo_unicode_scalars[m->offset];
            for (uint32_t i = 0; i < count; i++) {
                unsigned n = encode(values[i], NULL);
                if (n > SIZE_MAX - wrote || !mo_prim_bytes_fit(c, wrote + n))
                    goto done;
                if (pass != 0)
                    MO_ASSERT(encode(values[i], result + wrote) == n);
                wrote += n;
            }
        }
        if (pass == 0) {
            needed = wrote;
            result = mo_scratch_resize(&scratch, NULL, 0, needed);
            if (result == NULL)
                goto done;
        } else
            MO_ASSERT(wrote == needed);
    }
    if (mo_value_text(c->heap, (const char *)result, needed, out))
        status = MO_PRIM_RETURNED;
done:
    mo_scratch_free(&scratch);
    return status;
}

static MO_MUST_USE uint32_t decompose(uint32_t cp, uint32_t *out) {
    uint32_t hangul = cp - 0xac00;
    if (hangul < 11172) {
        out[0] = 0x1100 + hangul / 588;
        out[1] = 0x1161 + hangul % 588 / 28;
        out[2] = 0x11a7 + hangul % 28;
        return hangul % 28 == 0 ? 2U : 3U;
    }
    const struct mo_unicode_mapping *m =
        mapping(cp, mo_unicode_decompose, COUNT(mo_unicode_decompose));
    if (m == NULL) {
        out[0] = cp;
        return 1;
    }
    MO_ASSERT(m->count <= 4);
    memcpy(out, &mo_unicode_scalars[m->offset], m->count * sizeof *out);
    return m->count;
}

/* A packed scalar stores its combining class above bit 20. Short runs use at
 * most 31 shifts per scalar; long runs use stable counting sort, not insertion
 * sort. No quadratic scan of adversarially ordered combining marks. */
static MO_MUST_USE bool order(struct mo_scratch *scratch, uint32_t *values, size_t count) {
    uint32_t *temporary = NULL;
    size_t capacity = 0, at = 0;
    while (at < count) {
        if ((values[at] >> 21) == 0) {
            at++;
            continue;
        }
        size_t start = at++;
        bool sorted = true;
        while (at < count && (values[at] >> 21) != 0) {
            sorted = sorted && (values[at - 1] >> 21) <= (values[at] >> 21);
            at++;
        }
        size_t n = at - start;
        if (sorted)
            continue;
        if (n <= 32) {
            for (size_t i = start + 1; i < at; i++) {
                uint32_t v = values[i];
                size_t j = i;
                while (j > start && (values[j - 1] >> 21) > (v >> 21)) {
                    values[j] = values[j - 1];
                    j--;
                }
                values[j] = v;
            }
            continue;
        }
        if (n > capacity) {
            temporary = mo_scratch_resize(scratch, temporary, capacity * sizeof *temporary,
                                          n * sizeof *temporary);
            if (temporary == NULL)
                return false;
            capacity = n;
        }
        size_t counts[256] = {0}, positions[256], sum = 0;
        for (size_t i = start; i < at; i++)
            counts[values[i] >> 21]++;
        for (unsigned i = 0; i < 256; i++) {
            positions[i] = sum;
            sum += counts[i];
        }
        for (size_t i = start; i < at; i++)
            temporary[positions[values[i] >> 21]++] = values[i];
        memcpy(values + start, temporary, n * sizeof *values);
    }
    return true;
}

static MO_MUST_USE uint32_t compose(uint32_t a, uint32_t b) {
    if (a >= 0x1100 && a < 0x1113 && b >= 0x1161 && b < 0x1176)
        return 0xac00 + (a - 0x1100) * 588 + (b - 0x1161) * 28;
    if (a >= 0xac00 && a <= 0xd7a3 && (a - 0xac00) % 28 == 0 && b > 0x11a7 && b < 0x11c3)
        return a + b - 0x11a7;
    size_t lo = 0, hi = COUNT(mo_unicode_compose);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const struct mo_unicode_composition *p = &mo_unicode_compose[mid];
        if (p->left < a || (p->left == a && p->right < b))
            lo = mid + 1;
        else if (p->left > a || (p->left == a && p->right > b))
            hi = mid;
        else
            return p->result;
    }
    return 0;
}

static MO_MUST_USE enum mo_prim_status normalize(struct mo_prim_call *c, const uint8_t *bytes,
                                                 size_t length, struct mo_cell *out) {
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, c->heap);
    enum mo_prim_status status = MO_PRIM_OUT_OF_MEMORY;
    size_t at = 0, count = 0;
    uint32_t ds[4];
    while (at < length)
        count += decompose(scalar(bytes, length, &at), ds);
    MO_ASSERT(count <= 4 * length);
    uint32_t *values = mo_scratch_resize(&scratch, NULL, 0, count * sizeof *values);
    if (values == NULL)
        goto done;
    at = 0;
    size_t wrote = 0;
    while (at < length) {
        uint32_t n = decompose(scalar(bytes, length, &at), ds);
        for (uint32_t i = 0; i < n; i++)
            values[wrote++] = ds[i] | (((properties(ds[i]) >> 16) & 255U) << 21);
    }
    MO_ASSERT(wrote == count);
    if (!order(&scratch, values, count))
        goto done;
    size_t starter = SIZE_MAX;
    unsigned last_class = 0;
    wrote = 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t cp = values[i] & SCALAR_MASK, cls = values[i] >> 21;
        uint32_t combined = starter != SIZE_MAX && (last_class == 0 || last_class < cls)
                                ? compose(values[starter] & SCALAR_MASK, cp)
                                : 0;
        if (combined != 0)
            values[starter] = combined;
        else {
            if (cls == 0)
                starter = wrote;
            values[wrote++] = values[i];
            last_class = cls;
        }
    }
    size_t needed = 0;
    for (size_t i = 0; i < wrote; i++) {
        unsigned n = encode(values[i] & SCALAR_MASK, NULL);
        if (n > SIZE_MAX - needed || !mo_prim_bytes_fit(c, needed + n))
            goto done;
        needed += n;
    }
    uint8_t *result = mo_scratch_resize(&scratch, NULL, 0, needed);
    if (result == NULL)
        goto done;
    at = 0;
    for (size_t i = 0; i < wrote; i++)
        at += encode(values[i] & SCALAR_MASK, result + at);
    MO_ASSERT(at == needed);
    if (mo_value_text(c->heap, (const char *)result, needed, out))
        status = MO_PRIM_RETURNED;
done:
    mo_scratch_free(&scratch);
    return status;
}

enum mo_prim_status mo_prim_unicode(struct mo_prim_call *c, struct mo_cell *out) {
    size_t length;
    const uint8_t *bytes = mo_value_data(c->args[0], &length);
    switch ((unsigned)c->id) {
    case MO_PRIM_UNICODE16_GRAPHEMES:
        return graphemes(c, bytes, length, out);
    case MO_PRIM_UNICODE16_DISPLAY_WIDTH: {
        uint64_t width;
        MO_ASSERT(mo_unicode16_display_width(bytes, length, &width));
        MO_ASSERT(width <= INT64_MAX);
        *out = mo_cell_int((int64_t)width);
        return MO_PRIM_RETURNED;
    }
    case MO_PRIM_UNICODE16_NORMALIZE_NFC:
        return normalize(c, bytes, length, out);
    case MO_PRIM_UNICODE16_LOWERCASE:
    case MO_PRIM_UNICODE16_UPPERCASE:
        return casing(c, bytes, length, out);
    default:
        MO_UNREACHABLE();
    }
}
