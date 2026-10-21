#include "heap/heap.h"

#include <float.h>
#include <string.h>

_Static_assert(sizeof(double) == 8 && FLT_RADIX == 2 && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
               "Float is IEEE 754 binary64");
_Static_assert(sizeof(struct mo_range) == 16, "Range is two Ints");

struct mo_cell mo_cell_unit(void) {
    return (struct mo_cell){0};
}

/* memcpy avoids aliasing and alignment assumptions for all immediate kinds. */
#define CELL_ACCESSORS(name, c_type)                                                               \
    struct mo_cell mo_cell_##name(c_type value) {                                                  \
        struct mo_cell cell = {0};                                                                 \
        memcpy(cell.bytes, &value, sizeof value);                                                  \
        return cell;                                                                               \
    }                                                                                              \
    c_type mo_cell_get_##name(struct mo_cell cell) {                                               \
        c_type value;                                                                              \
        memcpy(&value, cell.bytes, sizeof value);                                                  \
        return value;                                                                              \
    }

CELL_ACCESSORS(byte, uint8_t)
CELL_ACCESSORS(int, int64_t)
CELL_ACCESSORS(float, double)
CELL_ACCESSORS(duration, int64_t)
CELL_ACCESSORS(ref, struct mo_object *)
#undef CELL_ACCESSORS

struct mo_cell mo_cell_bool(bool value) {
    return mo_cell_byte(value ? 1 : 0);
}
bool mo_cell_get_bool(struct mo_cell cell) {
    MO_ASSERT(cell.bytes[0] <= 1);
    return cell.bytes[0] != 0;
}

struct mo_cell mo_cell_char(uint32_t value) {
    MO_ASSERT(value < 0x110000 && (value < 0xd800 || value > 0xdfff));
    struct mo_cell cell = {0};
    memcpy(cell.bytes, &value, sizeof value);
    return cell;
}
uint32_t mo_cell_get_char(struct mo_cell cell) {
    uint32_t value;
    memcpy(&value, cell.bytes, sizeof value);
    return value;
}

struct mo_cell mo_cell_time(int64_t value) {
    /* Inclusive first and last microseconds of years 1 through 9999. */
    MO_ASSERT(value >= INT64_C(-62135596800000000) && value <= INT64_C(253402300799999999));
    return mo_cell_int(value);
}
int64_t mo_cell_get_time(struct mo_cell cell) {
    return mo_cell_get_int(cell);
}

struct mo_cell mo_cell_decimal(mo_i128 value) {
    /* Two's complement minimum is the only count outside Decimal's range. */
    MO_ASSERT(value >= -(mo_i128)(((mo_u128)1 << 127) - 1));
    struct mo_cell cell;
    memcpy(cell.bytes, &value, sizeof value);
    return cell;
}
mo_i128 mo_cell_get_decimal(struct mo_cell cell) {
    mo_i128 value;
    memcpy(&value, cell.bytes, sizeof value);
    return value;
}

struct mo_cell mo_cell_range(int64_t start, int64_t end) {
    struct mo_range value = {start, end};
    struct mo_cell cell;
    memcpy(cell.bytes, &value, sizeof value);
    return cell;
}
struct mo_range mo_cell_get_range(struct mo_cell cell) {
    struct mo_range value;
    memcpy(&value, cell.bytes, sizeof value);
    return value;
}

struct mo_cell mo_cell_case(uint32_t tag, struct mo_cell payload) {
    struct mo_cell cell = {0};
    memcpy(cell.bytes, &tag, sizeof tag);
    memcpy(cell.bytes + 8, payload.bytes, 8);
    return cell;
}
uint32_t mo_cell_get_tag(struct mo_cell cell) {
    uint32_t tag;
    memcpy(&tag, cell.bytes, sizeof tag);
    return tag;
}
struct mo_cell mo_cell_get_payload(struct mo_cell cell) {
    struct mo_cell payload = {0};
    memcpy(payload.bytes, cell.bytes + 8, 8);
    return payload;
}
