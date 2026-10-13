#ifndef MO_PRIM_INTERNAL_H
#define MO_PRIM_INTERNAL_H

#include "prim/prim.h"

#define MO_DECIMAL_SCALE INT64_C(1000000000000000000)
#define MO_DECIMAL_MAX ((mo_i128)(((mo_u128)1 << 127) - 1))

struct mo_prim_call {
    struct mo_heap *heap;
    const struct mo_types *types;
    enum mo_primitive_id id;
    const struct mo_cell *args;
    const uint32_t *params;
    uint32_t result;
    unsigned moved;
    const char *reason;
};

MO_MUST_USE enum mo_prim_status mo_prim_option(struct mo_prim_call *call, bool some,
                                               struct mo_cell value, struct mo_cell *out);
MO_MUST_USE enum mo_prim_status mo_prim_number(struct mo_prim_call *call, struct mo_cell *out);
MO_MUST_USE enum mo_prim_status mo_prim_print(struct mo_prim_call *call, uint32_t type,
                                              struct mo_cell value, struct mo_cell *out);
MO_MUST_USE double mo_prim_ratio_float(bool negative, mo_u128 n, mo_u128 d);

#endif
