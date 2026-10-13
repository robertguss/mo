#include "prim/prim.h"

static MO_MUST_USE bool multiply(int64_t a, int64_t b, int64_t *out) {
    mo_i128 wide = (mo_i128)a * b;
    if (wide < INT64_MIN || wide > INT64_MAX)
        return false;
    *out = (int64_t)wide;
    return true;
}

static MO_MUST_USE enum mo_prim_status power(const struct mo_cell *args, struct mo_cell *out,
                                             const char **reason) {
    int64_t a = mo_cell_get_int(args[0]), n = mo_cell_get_int(args[1]), result = 1;
    if (n < 0) {
        *reason = "Int.pow with a negative exponent";
        return MO_PRIM_OUTSIDE;
    }
    /* Squaring only when another bit remains accepts (-2)^63 and INT64_MIN^1. */
    while (n != 0) {
        if ((n & 1) != 0 && !multiply(result, a, &result)) {
            *reason = "Int";
            return MO_PRIM_OVERFLOW;
        }
        n >>= 1;
        if (n != 0 && !multiply(a, a, &a)) {
            *reason = "Int";
            return MO_PRIM_OVERFLOW;
        }
    }
    *out = mo_cell_int(result);
    return MO_PRIM_RETURNED;
}

enum mo_prim_status mo_prim_call(struct mo_heap *heap, const struct mo_types *types,
                                 enum mo_primitive_id id, uint32_t instantiation,
                                 const struct mo_cell *arguments, struct mo_cell *out,
                                 const char **reason_out) {
    MO_ASSERT(heap != NULL && types != NULL && types->finished && instantiation < types->count &&
              id < MO_PRIM_COUNT && out != NULL && reason_out != NULL);
    *reason_out = NULL;
    if (id == MO_PRIM_INT_POW) {
        MO_ASSERT(arguments != NULL);
        return power(arguments, out, reason_out);
    }
    *reason_out = "primitive not implemented";
    return MO_PRIM_NOT_IMPLEMENTED;
}
