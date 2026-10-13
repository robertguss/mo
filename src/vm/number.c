#include "base/decimal.h"
#include "vm/internal.h"

#include <math.h>

static MO_MUST_USE bool decimal(unsigned family, mo_i128 a, mo_i128 b, mo_i128 *out) {
    const mo_i128 maximum = (mo_i128)(((mo_u128)1 << 127) - 1);
    if (family <= 1) {
        if (family == 1)
            b = -b;
        if ((b > 0 && a > maximum - b) || (b < 0 && a < -maximum - b))
            return false;
        *out = a + b;
        return true;
    }
    if (family == 4) {
        *out = a % b;
        return true;
    }
    return family == 2 ? mo_decimal_mul(a, b, out) : mo_decimal_div(a, b, out);
}

static MO_MUST_USE bool integer(unsigned family, int64_t a, int64_t b, int64_t *out) {
    mo_i128 value;
    switch (family) {
    case 0:
        value = (mo_i128)a + b;
        break;
    case 1:
        value = (mo_i128)a - b;
        break;
    case 2:
        value = (mo_i128)a * b;
        break;
    case 3:
    case 4:
        if (a == INT64_MIN && b == -1) {
            if (family == 3)
                return false;
            *out = 0;
            return true;
        }
        value = family == 3 ? a / b : a % b;
        break;
    default:
        MO_UNREACHABLE();
    }
    if (value < INT64_MIN || value > INT64_MAX)
        return false;
    *out = (int64_t)value;
    return true;
}

bool mo_vm_number(uint32_t opcode, struct mo_cell a, struct mo_cell b, struct mo_cell *out,
                  const char **reason) {
    *reason = "overflow";
    if (opcode <= MO_OP_REM_FLOAT) {
        unsigned family = (opcode - MO_OP_ADD_INT) / 3;
        unsigned kind = (opcode - MO_OP_ADD_INT) % 3;
        if (kind == 2) {
            double x = mo_cell_get_float(a), y = mo_cell_get_float(b), value;
            switch (family) {
            case 0:
                value = x + y;
                break;
            case 1:
                value = x - y;
                break;
            case 2:
                value = x * y;
                break;
            case 3:
                value = x / y;
                break;
            case 4:
                value = fmod(x, y);
                break;
            default:
                MO_UNREACHABLE();
            }
            *out = mo_cell_float(value);
            return true;
        }
        if (family >= 3 && (kind == 0 ? mo_cell_get_int(b) == 0 : mo_cell_get_decimal(b) == 0)) {
            *reason = "dividing by zero";
            return false;
        }
        if (kind == 0) {
            int64_t value;
            if (!integer(family, mo_cell_get_int(a), mo_cell_get_int(b), &value))
                return false;
            *out = mo_cell_int(value);
        } else {
            mo_i128 value;
            if (!decimal(family, mo_cell_get_decimal(a), mo_cell_get_decimal(b), &value))
                return false;
            *out = mo_cell_decimal(value);
        }
        return true;
    }
    if (opcode >= MO_OP_NEG_INT && opcode <= MO_OP_NEG_DURATION) {
        if (opcode == MO_OP_NEG_FLOAT)
            *out = mo_cell_float(-mo_cell_get_float(a));
        else if (opcode == MO_OP_NEG_DECIMAL)
            *out = mo_cell_decimal(-mo_cell_get_decimal(a));
        else {
            int64_t x = mo_cell_get_int(a);
            if (x == INT64_MIN)
                return false;
            *out = mo_cell_int(-x);
        }
        return true;
    }
    if (opcode >= MO_OP_ADD_DURATION && opcode <= MO_OP_DIFF_TIME) {
        mo_i128 x = mo_cell_get_int(a), y = mo_cell_get_int(b), value;
        if (opcode == MO_OP_SCALE_DURATION)
            value = x * y;
        else if (opcode == MO_OP_ADD_DURATION || opcode == MO_OP_ADD_TIME)
            value = x + y;
        else
            value = x - y;
        if (opcode == MO_OP_ADD_TIME || opcode == MO_OP_SUB_TIME) {
            if (value < INT64_C(-62135596800000000) || value > INT64_C(253402300799999999)) {
                *reason = "time outside years 1 to 9999";
                return false;
            }
            *out = mo_cell_time((int64_t)value);
        } else {
            if (value < INT64_MIN || value > INT64_MAX)
                return false;
            *out = mo_cell_duration((int64_t)value);
        }
        return true;
    }
    if (opcode >= MO_OP_EQ_INT && opcode <= MO_OP_LE_CHOICE) {
        unsigned family = (opcode - MO_OP_EQ_INT) / 9;
        unsigned kind = (opcode - MO_OP_EQ_INT) % 9;
        bool value;
        if (kind == 2) {
            double x = mo_cell_get_float(a), y = mo_cell_get_float(b);
            value = family == 0 ? x == y : family == 1 ? x < y : x <= y;
        } else {
            mo_i128 x, y;
            if (kind == 1) {
                x = mo_cell_get_decimal(a);
                y = mo_cell_get_decimal(b);
            } else if (kind == 3 || kind == 4) {
                x = mo_cell_get_byte(a);
                y = mo_cell_get_byte(b);
            } else if (kind == 5) {
                x = mo_cell_get_char(a);
                y = mo_cell_get_char(b);
            } else if (kind == 8) {
                x = mo_cell_get_tag(a);
                y = mo_cell_get_tag(b);
            } else {
                x = mo_cell_get_int(a);
                y = mo_cell_get_int(b);
            }
            value = family == 0 ? x == y : family == 1 ? x < y : x <= y;
        }
        *out = mo_cell_bool(value);
        return true;
    }
    switch (opcode) {
    case MO_OP_CMP_FLOAT: {
        double x = mo_cell_get_float(a), y = mo_cell_get_float(b);
        int order = isnan(x) ? (isnan(y) ? 0 : 1) : isnan(y) ? -1 : (x > y) - (x < y);
        *out = mo_cell_case(order < 0 ? 0 : order == 0 ? 1 : 2, mo_cell_unit());
        break;
    }
    case MO_OP_NOT:
        *out = mo_cell_bool(!mo_cell_get_bool(a));
        break;
    case MO_OP_MAKE_RANGE:
        *out = mo_cell_range(mo_cell_get_int(a), mo_cell_get_int(b));
        break;
    case MO_OP_RANGE_START:
        *out = mo_cell_int(mo_cell_get_range(a).start);
        break;
    case MO_OP_RANGE_END:
        *out = mo_cell_int(mo_cell_get_range(a).end);
        break;
    default:
        MO_UNREACHABLE();
    }
    return true;
}
