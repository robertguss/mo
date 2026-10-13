#define _GNU_SOURCE
#include "lower/scalar.h"

#include <fenv.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__)
#include <xlocale.h>
#endif

static unsigned digit(uint8_t c) {
    return c >= 'a'   ? (unsigned)(c - 'a') + 10
           : c >= 'A' ? (unsigned)(c - 'A') + 10
                      : (unsigned)(c - '0');
}

static mo_core_integer number(struct lowering *p, const struct mo_syntax *s, mo_core_ty type,
                              bool negative) {
    size_t length = s->span.end - s->span.start;
    unsigned base = length > 2 && s->text[1] == 'x' ? 16 : length > 2 && s->text[1] == 'b' ? 2 : 10;
    /* Decimal magnitude, little-endian digits. Hex needs at most two decimal
     * digits per source digit. All multiplication work is charged explicitly. */
    uint8_t *digits = LOWER_NEW(p, uint8_t, 2 * length + 20);
    size_t used = 1, places = 0;
    bool fraction = false;
    for (size_t i = base == 10 ? 0 : 2; i < length; i++) {
        uint8_t c = s->text[i];
        if (c == '_')
            continue;
        if (c == '.') {
            fraction = true;
            continue;
        }
        unsigned carry = digit(c);
        MO_ASSERT(carry < base);
        lower_work(p, used);
        for (size_t j = 0; j < used; j++) {
            unsigned n = digits[j] * base + carry;
            digits[j] = (uint8_t)(n % 10);
            carry = n / 10;
        }
        while (carry != 0) {
            MO_ASSERT(used < 2 * length + 20);
            digits[used++] = (uint8_t)(carry % 10);
            carry /= 10;
        }
        if (fraction)
            places++;
    }
    size_t zeros = type->tag == MO_CORE_TY_DECIMAL ? 18 - places : 0;
    if (used == 1 && digits[0] == 0) {
        negative = false;
        zeros = 0;
    }
    char *text = LOWER_NEW(p, char, used + zeros + 1);
    for (size_t i = 0; i < used; i++)
        text[i] = (char)('0' + digits[used - i - 1]);
    for (size_t i = 0; i < zeros; i++)
        text[used + i] = '0';
    return (mo_core_integer){.digits = {text, used + zeros}, .negative = negative};
}

static size_t encode(uint32_t c, char *out) {
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xc0u | (c >> 6));
        out[1] = (char)(0x80u | (c & 63u));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (char)(0xe0u | (c >> 12));
        out[1] = (char)(0x80u | ((c >> 6) & 63u));
        out[2] = (char)(0x80u | (c & 63u));
        return 3;
    }
    out[0] = (char)(0xf0u | (c >> 18));
    out[1] = (char)(0x80u | ((c >> 12) & 63u));
    out[2] = (char)(0x80u | ((c >> 6) & 63u));
    out[3] = (char)(0x80u | (c & 63u));
    return 4;
}
static mo_core_string decoded(struct lowering *p, const struct mo_syntax *s) {
    size_t length = s->span.end - s->span.start;
    size_t quote =
        length >= 6 && s->text[0] == '"' && s->text[1] == '"' && s->text[2] == '"' ? 3 : 1;
    MO_ASSERT(length >= 2 * quote);
    char *out = LOWER_NEW(p, char, length + 1);
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
    size_t at = 0;
    bool line_start = quote == 3;
    for (size_t i = begin; i < end; i++) {
        lower_work(p, 1);
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
            out[at++] = (char)c;
            continue;
        }
        MO_ASSERT(++i < end);
        c = s->text[i];
        if (c == 'u') {
            MO_ASSERT(s->text[++i] == '{');
            uint32_t scalar = 0;
            while (s->text[++i] != '}')
                scalar = scalar * 16 + digit(s->text[i]);
            at += encode(scalar, out + at);
        } else
            out[at++] = c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : (char)c;
    }
    return (mo_core_string){out, at};
}

mo_core_expr lower_literal(struct lowering *p, const struct mo_syntax *s, mo_core_ty type,
                           bool negative) {
    if (s->kind == MO_S_NEGATE) {
        MO_ASSERT(s->count == 1);
        negative = !negative;
        s = s->children[0];
    }
    mo_core_expr e = lower_unit(p);
    if (type->tag == MO_CORE_TY_BOOL) {
        e->as.lit->tag = MO_CORE_LIT_BOOL;
        e->as.lit->as.bool_ = s->kind == MO_S_TRUE;
    } else if (type->tag == MO_CORE_TY_TEXT) {
        e->as.lit->tag = MO_CORE_LIT_TEXT;
        e->as.lit->as.text = decoded(p, s);
    } else if (type->tag == MO_CORE_TY_CHAR) {
        mo_core_string text = decoded(p, s);
        const uint8_t *bytes = (const uint8_t *)text.bytes;
        uint32_t c = bytes[0];
        if (text.length > 1) {
            c &= text.length == 2 ? 31u : text.length == 3 ? 15u : 7u;
            for (size_t i = 1; i < text.length; i++)
                c = c * 64u + (bytes[i] & 63u);
        }
        e->as.lit->tag = MO_CORE_LIT_CHAR;
        e->as.lit->as.char_ = c;
    } else if (type->tag == MO_CORE_TY_UNIT) {
        /* Unit is the sole source literal with no spelling to decode. */
    } else if (type->tag == MO_CORE_TY_FLOAT) {
        size_t length = s->span.end - s->span.start;
        char *text = LOWER_NEW(p, char, length * 2 + 24);
        size_t at = 0;
        if (negative)
            text[at++] = '-';
        if (length > 2 && s->text[1] == 'b') {
            mo_core_integer n = number(p, s, type, false);
            memcpy(text + at, n.digits.bytes, n.digits.length);
            at += n.digits.length;
        } else {
            for (size_t i = 0; i < length; i++)
                if (s->text[i] != '_')
                    text[at++] = (char)s->text[i];
        }
        text[at] = 0;
        locale_t locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
        if (locale == (locale_t)0)
            lower_refuse(p, MO_CODE_LOWER_NO_MEMORY, "cannot allocate the numeric locale");
        int rounding = fegetround();
        int changed = fesetround(FE_TONEAREST);
        char *end = NULL;
        double value = strtod_l(text, &end, locale);
        int restored = fesetround(rounding);
        freelocale(locale);
        MO_ASSERT(changed == 0 && restored == 0 && end == text + at);
        e->as.lit->tag = MO_CORE_LIT_FLOAT;
        memcpy(&e->as.lit->as.float_, &value, sizeof value);
    } else {
        mo_core_integer n = number(p, s, type, negative);
        if (type->tag == MO_CORE_TY_INT) {
            e->as.lit->tag = MO_CORE_LIT_INT;
            e->as.lit->as.int_ = n;
        } else if (type->tag == MO_CORE_TY_BIG_INT) {
            e->as.lit->tag = MO_CORE_LIT_BIG_INT;
            e->as.lit->as.big_int = n;
        } else if (type->tag == MO_CORE_TY_BYTE) {
            e->as.lit->tag = MO_CORE_LIT_BYTE;
            e->as.lit->as.byte = n;
        } else if (type->tag == MO_CORE_TY_DECIMAL) {
            e->as.lit->tag = MO_CORE_LIT_DECIMAL;
            e->as.lit->as.decimal = n;
        } else
            MO_UNREACHABLE();
    }
    return e;
}

struct mo_core_prim lower_operator(struct lowering *p, enum mo_syntax_token operation,
                                   mo_core_ty left, mo_core_ty right) {
    struct mo_core_prim prim = {0};
    if (operation == MO_T_NOT) {
        prim.tag = MO_CORE_PRIM_NOT;
        return prim;
    }
    if (operation == MO_T_EQ || operation == MO_T_NE) {
        prim.tag = MO_CORE_PRIM_EQ;
        prim.as.eq = left;
        return prim;
    }
    if (operation == MO_T_LT || operation == MO_T_LE || operation == MO_T_GT ||
        operation == MO_T_GE) {
        prim.tag = MO_CORE_PRIM_CMP;
        prim.as.cmp.op = operation == MO_T_LT   ? MO_CORE_CMP_OP_LT
                         : operation == MO_T_LE ? MO_CORE_CMP_OP_LE
                         : operation == MO_T_GT ? MO_CORE_CMP_OP_GT
                                                : MO_CORE_CMP_OP_GE;
        static const struct {
            enum mo_core_ty_tag ty;
            enum mo_core_base base;
        } bases[] = {
            {MO_CORE_TY_INT, MO_CORE_BASE_INT},          {MO_CORE_TY_BIG_INT, MO_CORE_BASE_BIG_INT},
            {MO_CORE_TY_BYTE, MO_CORE_BASE_BYTE},        {MO_CORE_TY_DECIMAL, MO_CORE_BASE_DECIMAL},
            {MO_CORE_TY_FLOAT, MO_CORE_BASE_FLOAT},      {MO_CORE_TY_BOOL, MO_CORE_BASE_BOOL},
            {MO_CORE_TY_CHAR, MO_CORE_BASE_CHAR},        {MO_CORE_TY_TEXT, MO_CORE_BASE_TEXT},
            {MO_CORE_TY_BYTES, MO_CORE_BASE_BYTES},      {MO_CORE_TY_TIME, MO_CORE_BASE_TIME},
            {MO_CORE_TY_DURATION, MO_CORE_BASE_DURATION}};
        for (size_t i = 0; i < sizeof bases / sizeof bases[0]; i++)
            if (left->tag == bases[i].ty) {
                prim.as.cmp.base = bases[i].base;
                return prim;
            }
        lower_refuse(p, MO_CODE_LOWER_UNFINISHED, "derived ordering is not lowered yet");
    }
    if (operation == MO_T_DOTDOT) {
        prim.tag = MO_CORE_PRIM_RANGE;
        prim.as.range = MO_CORE_RANGE_OP_MAKE;
        return prim;
    }
    unsigned index = operation == MO_T_PLUS || operation == MO_T_PLUSEQ ? 0u
                     : operation == MO_T_MINUS || operation == MO_T_MINUSEQ
                         ? (right == NULL ? 5u : 1u)
                     : operation == MO_T_STAR  ? 2u
                     : operation == MO_T_SLASH ? 3u
                                               : 4u;
    if (left->tag == MO_CORE_TY_INT) {
        prim.tag = MO_CORE_PRIM_INT;
        prim.as.int_ = (enum mo_core_int_op)index;
    } else if (left->tag == MO_CORE_TY_BIG_INT) {
        prim.tag = MO_CORE_PRIM_BIG;
        prim.as.big = (enum mo_core_big_op)index;
    } else if (left->tag == MO_CORE_TY_DECIMAL) {
        prim.tag = MO_CORE_PRIM_DEC;
        prim.as.dec = (enum mo_core_dec_op)index;
    } else if (left->tag == MO_CORE_TY_FLOAT) {
        prim.tag = MO_CORE_PRIM_FLOAT;
        prim.as.float_ = (enum mo_core_float_op)index;
    } else if (left->tag == MO_CORE_TY_TIME || left->tag == MO_CORE_TY_DURATION) {
        prim.tag = MO_CORE_PRIM_TIME;
        if (left->tag == MO_CORE_TY_TIME)
            prim.as.time = index == 0                      ? MO_CORE_TIME_OP_ADD
                           : right->tag == MO_CORE_TY_TIME ? MO_CORE_TIME_OP_SUB
                                                           : MO_CORE_TIME_OP_SUB_DUR;
        else
            prim.as.time = index == 0   ? MO_CORE_TIME_OP_DUR_ADD
                           : index == 1 ? MO_CORE_TIME_OP_DUR_SUB
                           : index == 2 ? MO_CORE_TIME_OP_DUR_MUL
                                        : MO_CORE_TIME_OP_DUR_NEG;
    } else
        lower_refuse(p, MO_CODE_LOWER_UNFINISHED, "this numeric operation is not lowered yet");
    return prim;
}

mo_core_expr lower_error_text(struct lowering *p, mo_core_ty type, mo_core_expr value) {
    if (type->tag == MO_CORE_TY_TEXT)
        return value;
    if (type->tag == MO_CORE_TY_UNIT) {
        mo_core_expr e = lower_unit(p);
        e->as.lit->tag = MO_CORE_LIT_TEXT;
        e->as.lit->as.text = lower_text(p, "Unit");
        return e;
    }
    if (type->tag == MO_CORE_TY_BOOL) {
        mo_core_expr e = lower_expr(p, MO_CORE_EXPR_IF);
        e->as.if_.cond = value;
        e->as.if_.then_ = lower_unit(p);
        e->as.if_.else_ = lower_unit(p);
        e->as.if_.then_->as.lit->tag = e->as.if_.else_->as.lit->tag = MO_CORE_LIT_TEXT;
        e->as.if_.then_->as.lit->as.text = lower_text(p, "true");
        e->as.if_.else_->as.lit->as.text = lower_text(p, "false");
        return e;
    }
    struct mo_core_prim prim = {0};
    if (type->tag == MO_CORE_TY_INT) {
        prim.tag = MO_CORE_PRIM_INT;
        prim.as.int_ = MO_CORE_INT_OP_TO_TEXT;
    } else if (type->tag == MO_CORE_TY_BIG_INT) {
        prim.tag = MO_CORE_PRIM_BIG;
        prim.as.big = MO_CORE_BIG_OP_TO_TEXT;
    } else if (type->tag == MO_CORE_TY_BYTE) {
        prim.tag = MO_CORE_PRIM_BYTE;
        prim.as.byte = MO_CORE_BYTE_OP_TO_TEXT;
    } else if (type->tag == MO_CORE_TY_DECIMAL) {
        prim.tag = MO_CORE_PRIM_DEC;
        prim.as.dec = MO_CORE_DEC_OP_TO_TEXT;
    } else if (type->tag == MO_CORE_TY_FLOAT) {
        prim.tag = MO_CORE_PRIM_FLOAT;
        prim.as.float_ = MO_CORE_FLOAT_OP_TO_TEXT;
    } else if (type->tag == MO_CORE_TY_CHAR) {
        prim.tag = MO_CORE_PRIM_CHAR;
        prim.as.char_ = MO_CORE_CHAR_OP_TO_TEXT;
    } else if (type->tag == MO_CORE_TY_RANGE) {
        prim.tag = MO_CORE_PRIM_RANGE;
        prim.as.range = MO_CORE_RANGE_OP_TO_TEXT;
    } else if (type->tag == MO_CORE_TY_TIME || type->tag == MO_CORE_TY_DURATION) {
        prim.tag = MO_CORE_PRIM_TIME;
        prim.as.time =
            type->tag == MO_CORE_TY_TIME ? MO_CORE_TIME_OP_TO_TEXT : MO_CORE_TIME_OP_DUR_TO_TEXT;
    } else
        lower_refuse(p, MO_CODE_LOWER_UNFINISHED,
                     "this error text implementation is not lowered yet");
    mo_core_expr_list args = {.count = 1, .items = LOWER_NEW(p, mo_core_expr, 1)};
    args.items[0] = value;
    return lower_prim(p, prim, args);
}
