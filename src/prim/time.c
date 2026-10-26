#include "prim/internal.h"

/* Inclusive UTC microseconds of years 1 through 9999, as in the cell and model. */
static MO_MUST_USE bool in_time(mo_i128 t) {
    return t >= MO_TIME_MIN_US && t <= MO_TIME_MAX_US;
}

static MO_MUST_USE bool digits(const uint8_t *s, size_t start, unsigned count, int64_t *out) {
    int64_t n = 0;
    for (unsigned i = 0; i < count; i++) {
        uint8_t c = s[start + i];
        if (c < '0' || c > '9')
            return false;
        n = n * 10 + c - '0';
    }
    *out = n;
    return true;
}

/* Hinnant's Gregorian day count, also used by Support/Time.lean. Division of
 * negative years is floored; the wire spelling can have year 0000 before its
 * offset moves the instant into year 1. Check the UTC instant, not local year. */
static MO_MUST_USE int64_t days(int64_t year, int64_t month, int64_t day) {
    year -= month <= 2 ? 1 : 0;
    int64_t era = year >= 0 ? year / 400 : (year - 399) / 400;
    int64_t yoe = year - era * 400;
    int64_t mp = month > 2 ? month - 3 : month + 9;
    int64_t doy = (153 * mp + 2) / 5 + day - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

static MO_MUST_USE bool parse(const uint8_t *s, size_t n, int64_t *out) {
    /* A six-digit fraction and signed offset occupy at most 32 bytes. */
    if (n < 20 || n > 32 || s[4] != '-' || s[7] != '-' || (s[10] != 'T' && s[10] != 't') ||
        s[13] != ':' || s[16] != ':')
        return false;
    int64_t y, m, d, hh, mm, ss;
    if (!digits(s, 0, 4, &y) || !digits(s, 5, 2, &m) || !digits(s, 8, 2, &d) ||
        !digits(s, 11, 2, &hh) || !digits(s, 14, 2, &mm) || !digits(s, 17, 2, &ss))
        return false;
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    int64_t last = m == 2 ? (leap ? 29 : 28) : m == 4 || m == 6 || m == 9 || m == 11 ? 30 : 31;
    if (m < 1 || m > 12 || d < 1 || d > last || hh > 23 || mm > 59 || ss > 59)
        return false;
    size_t at = 19;
    int64_t fraction = 0;
    if (s[at] == '.') {
        at++;
        unsigned places = 0;
        while (at < n && s[at] >= '0' && s[at] <= '9') {
            if (places == 6)
                return false;
            fraction = fraction * 10 + s[at++] - '0';
            places++;
        }
        if (places == 0)
            return false;
        for (; places < 6; places++)
            fraction *= 10;
    }
    int64_t offset = 0;
    if (n - at == 1 && (s[at] == 'Z' || s[at] == 'z')) {
        offset = 0;
    } else if (n - at == 6 && (s[at] == '+' || s[at] == '-') && s[at + 3] == ':') {
        int64_t oh, om;
        if (!digits(s, at + 1, 2, &oh) || !digits(s, at + 4, 2, &om) || oh > 23 || om > 59)
            return false;
        offset = (oh * 3600 + om * 60) * (s[at] == '-' ? -1 : 1);
    } else
        return false;
    int64_t t = (days(y, m, d) * 86400 + hh * 3600 + mm * 60 + ss - offset) * 1000000 + fraction;
    if (!in_time(t))
        return false;
    *out = t;
    return true;
}

enum mo_prim_status mo_prim_time(struct mo_prim_call *c, struct mo_cell *out) {
    switch ((unsigned)c->id) {
    case MO_PRIM_TIME_TO_TEXT:
    case MO_PRIM_DURATION_TO_TEXT:
        return mo_prim_print(c, c->params[0], c->args[0], out);
    case MO_PRIM_TIME_CHECKED_ADD: {
        mo_i128 t = (mo_i128)mo_cell_get_time(c->args[0]) + mo_cell_get_duration(c->args[1]);
        bool valid = in_time(t);
        return mo_prim_option(c, valid, valid ? mo_cell_time((int64_t)t) : mo_cell_unit(), out);
    }
    case MO_PRIM_TIME_PARSE: {
        size_t length;
        const uint8_t *s = mo_value_data(c->args[0], &length);
        int64_t t;
        bool valid = parse(s, length, &t);
        return mo_prim_option(c, valid, valid ? mo_cell_time(t) : mo_cell_unit(), out);
    }
    case MO_PRIM_DURATION_IN_MILLISECONDS:
        *out = mo_cell_int(mo_cell_get_duration(c->args[0]) / 1000);
        return MO_PRIM_RETURNED;
    case MO_PRIM_DURATION_IN_SECONDS:
        *out = mo_cell_decimal((mo_i128)mo_cell_get_duration(c->args[0]) * INT64_C(1000000000000));
        return MO_PRIM_RETURNED;
    default:
        MO_UNREACHABLE();
    }
}
