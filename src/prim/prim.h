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
};

/* types is sealed and belongs to heap's image. id and instantiation are verified: a
 * function type, including fn() : T for a zero-argument constant. arguments
 * holds the signature's valid cells and may be NULL only for no arguments. out is not
 * an argument or an owned reference. On RETURNED, out owns the result and the call
 * consumes exactly prims.def's owned parameters; borrowed parameters stay readable.
 * All other outcomes leave operands, their objects/ownership and out unchanged.
 * reason_out is required, gives static detail on refusal and is NULL on success.
 * Entry sets FE_TONEAREST; the VM's arithmetic and subsequent primitive calls use it. */
MO_MUST_USE enum mo_prim_status mo_prim_call(struct mo_heap *heap, const struct mo_types *types,
                                             enum mo_primitive_id id, uint32_t instantiation,
                                             const struct mo_cell *arguments, struct mo_cell *out,
                                             const char **reason_out);

/* The same grapheme-aware column estimate as Unicode16.display_width, over a
 * readable UTF-8 span. bytes may be NULL only when length is zero; width_out is
 * required. No allocation or heap, constant stack space and linear work.
 * On success writes the width; on invalid UTF-8 or uint64_t overflow returns
 * false and leaves width_out unchanged. No source-file or runtime value cap. */
MO_MUST_USE bool mo_unicode16_display_width(const uint8_t *bytes, size_t length,
                                            uint64_t *width_out);

/* First Unicode16.graphemes cluster of a valid UTF-8 span (a precondition),
 * starting at a cluster boundary: text start or the previous cluster's end.
 * Inspects/validates only that cluster and one lookahead scalar, not the suffix.
 * No allocation, constant stack and work linear in the inspected prefix;
 * repeated calls split a text in linear work. length_out is required. Returns
 * false on empty input or malformed bytes encountered in the inspected prefix,
 * leaving *length_out unchanged. bytes may be NULL only for empty input. */
MO_MUST_USE bool mo_unicode16_next_grapheme(const uint8_t *bytes, size_t length,
                                            size_t *length_out);

#endif
