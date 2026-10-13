/* VM primitives over verified heap cells. */
#ifndef MO_PRIM_H
#define MO_PRIM_H

#include "base/prims.h"
#include "heap/heap.h"

enum mo_prim_status {
    MO_PRIM_RETURNED,
    MO_PRIM_OVERFLOW,
    MO_PRIM_DIV_BY_ZERO,
    MO_PRIM_OUTSIDE,
    MO_PRIM_REPEATED_KEY,
    MO_PRIM_OUT_OF_MEMORY,
    MO_PRIM_NOT_IMPLEMENTED,
};

/* types is sealed and belongs to heap's image. id and instantiation are verified: a
 * function type for a call, or the result type for a zero-argument constant. arguments
 * holds the signature's valid cells and may be NULL only for no arguments. out is not
 * an argument or an owned reference. On RETURNED, out owns the result and the call
 * consumes exactly prims.def's owned parameters; borrowed parameters stay readable.
 * All other outcomes leave operands, their objects/ownership and out unchanged.
 * reason_out is required, gives static detail on refusal and is NULL on success.
 * NOT_IMPLEMENTED is the explicit staged-library refusal, never a language failure. */
MO_MUST_USE enum mo_prim_status mo_prim_call(struct mo_heap *heap, const struct mo_types *types,
                                             enum mo_primitive_id id, uint32_t instantiation,
                                             const struct mo_cell *arguments, struct mo_cell *out,
                                             const char **reason_out);

#endif
