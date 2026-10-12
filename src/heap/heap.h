/* Cells and the C value-building interface (architecture section 12).
 * Stream 3 implements the heap and builders; cells need no process or heap. */
#ifndef MO_HEAP_H
#define MO_HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/base.h"
#include "heap/types.h"

/* A cell is native-endian runtime storage, not a serialized constant. Alignment
 * is 8, so fields can follow the 8-byte object header without padding. Decimal
 * uses memcpy, not an aligned mo_i128 lvalue. Unused bytes are always zero. */
struct mo_cell {
    _Alignas(uint64_t) uint8_t bytes[16];
};
_Static_assert(sizeof(struct mo_cell) == 16, "every value fits in one cell");
_Static_assert(_Alignof(struct mo_cell) == 8, "cells follow the heap header");

struct mo_object {
    uint32_t count;
    uint32_t layout;
};
_Static_assert(sizeof(struct mo_object) == 8, "heap headers are eight bytes");
_Static_assert(offsetof(struct mo_object, count) == 0 && offsetof(struct mo_object, layout) == 4,
               "the header stores the count before the layout index");
#define MO_IMMORTAL_COUNT UINT32_MAX

struct mo_range {
    int64_t start;
    int64_t end;
};

/* Constructors assert valid Char, Time and Decimal values. The loader checks
 * untrusted constants before calling them. Unit has no payload to read. */
MO_MUST_USE struct mo_cell mo_cell_unit(void);
MO_MUST_USE struct mo_cell mo_cell_bool(bool value);
MO_MUST_USE bool mo_cell_get_bool(struct mo_cell cell);
MO_MUST_USE struct mo_cell mo_cell_byte(uint8_t value);
MO_MUST_USE uint8_t mo_cell_get_byte(struct mo_cell cell);
MO_MUST_USE struct mo_cell mo_cell_int(int64_t value);
MO_MUST_USE int64_t mo_cell_get_int(struct mo_cell cell);
MO_MUST_USE struct mo_cell mo_cell_float(double value);
MO_MUST_USE double mo_cell_get_float(struct mo_cell cell);
MO_MUST_USE struct mo_cell mo_cell_char(uint32_t value);
MO_MUST_USE uint32_t mo_cell_get_char(struct mo_cell cell);
MO_MUST_USE struct mo_cell mo_cell_time(int64_t value);
MO_MUST_USE int64_t mo_cell_get_time(struct mo_cell cell);
MO_MUST_USE struct mo_cell mo_cell_duration(int64_t value);
MO_MUST_USE int64_t mo_cell_get_duration(struct mo_cell cell);
MO_MUST_USE struct mo_cell mo_cell_decimal(mo_i128 value);
MO_MUST_USE mo_i128 mo_cell_get_decimal(struct mo_cell cell);
MO_MUST_USE struct mo_cell mo_cell_range(int64_t start, int64_t end);
MO_MUST_USE struct mo_range mo_cell_get_range(struct mo_cell cell);
MO_MUST_USE struct mo_cell mo_cell_ref(struct mo_object *value);
MO_MUST_USE struct mo_object *mo_cell_get_ref(struct mo_cell cell);

/* The payload is zero for empty, the first eight bytes of a field for inline,
 * or a pointer cell for ref/boxed. Choice references are at byte 8, not byte 0.
 * Callers use mo_case_shape to select a payload; these operations don't count. */
MO_MUST_USE struct mo_cell mo_cell_case(uint32_t tag, struct mo_cell payload);
MO_MUST_USE uint32_t mo_cell_get_tag(struct mo_cell cell);
MO_MUST_USE struct mo_cell mo_cell_get_payload(struct mo_cell cell);

struct mo_process;

/* Stream 3 implements type-directed ownership for C callers. type is an index
 * in the process image's type table; value is a valid cell of that type, from
 * this process or its image, never another process. dup reads an owned or
 * borrowed value and returns another owned cell. drop consumes one owned cell;
 * its C bits remain but must not be read again. Neither operation can fail.
 * Immediates only copy/do nothing. For choices, the tag selects the payload's
 * shape. Counts saturate at MO_IMMORTAL_COUNT; dup and drop leave immortal
 * counts unchanged, and drop frees at zero without C recursion (bytecode 2.4).
 *
 * mo_call consumes owned arguments even on stop/failure, but not borrowed
 * ones. dup before passing an owned argument if C needs it after the call.
 * For a promise, retain the inputs while calling its subject, then pass owned
 * duplicates of those inputs and the owned result to the promise. Drop any
 * retained ownership afterward, before mo_process_free. A cell copy alone
 * does not duplicate ownership. */
MO_MUST_USE struct mo_cell mo_value_dup(struct mo_process *process, uint32_t type,
                                        struct mo_cell value);
void mo_value_drop(struct mo_process *process, uint32_t type, struct mo_cell value);

/* Each successful result is owned by the caller in the process's heap. Input
 * cells are borrowed: builders duplicate references they keep, so generators
 * can reuse inputs. On false, inputs and out are unchanged and no ownership
 * transfers. This differs from the VM's consuming construct instructions.
 * Maps keep the first key's position and last value for duplicates; sets keep
 * the first occurrence, as the library's conversions do.
 *
 * type is the whole result type (List(T), Map(K, V), Set(T) or a choice), not
 * an element type. construct accepts record, tuple and boxed-case layouts;
 * closures use value_closure. C callers supply valid cells of the declared
 * field/element types, with counts matching the layout, case or captures.
 * Arrays may be NULL only when count is zero. out must not overlap input
 * arrays or hold an owned reference. text refuses invalid UTF-8. text, bytes
 * and bigint copy their input bytes; length is at most MO_LIMIT_STRING_SIZE,
 * and a byte pointer may be NULL only when length is zero. bytes accepts every
 * byte sequence, including invalid UTF-8 and NUL. bigint uses base's shared
 * signs and bytecode 3.1's little-endian magnitude: ZERO iff length is zero;
 * POSITIVE/NEGATIVE require a nonzero final byte. Invalid signs, noncanonical
 * magnitudes, a length past the limit, or allocation/heap-limit failure return
 * false, leaving out and inputs unchanged and keeping no input ownership.
 * These are C builders, not bytecode verification. */
MO_MUST_USE bool mo_value_text(struct mo_process *process, const char *utf8, size_t length,
                               struct mo_cell *out);
MO_MUST_USE bool mo_value_bytes(struct mo_process *process, const uint8_t *bytes, size_t length,
                                struct mo_cell *out);
MO_MUST_USE bool mo_value_bigint(struct mo_process *process, enum mo_bigint_sign sign,
                                 const uint8_t *magnitude, size_t length, struct mo_cell *out);
MO_MUST_USE bool mo_value_construct(struct mo_process *process, uint32_t layout,
                                    const struct mo_cell *fields, uint32_t count,
                                    struct mo_cell *out);
MO_MUST_USE bool mo_value_case(struct mo_process *process, uint32_t type, uint32_t tag,
                               const struct mo_cell *fields, uint32_t count, struct mo_cell *out);
MO_MUST_USE bool mo_value_list(struct mo_process *process, uint32_t type,
                               const struct mo_cell *elements, uint32_t count, struct mo_cell *out);
MO_MUST_USE bool mo_value_map(struct mo_process *process, uint32_t type, const struct mo_cell *keys,
                              const struct mo_cell *values, uint32_t count, struct mo_cell *out);
MO_MUST_USE bool mo_value_set(struct mo_process *process, uint32_t type,
                              const struct mo_cell *elements, uint32_t count, struct mo_cell *out);
MO_MUST_USE bool mo_value_closure(struct mo_process *process, uint32_t function,
                                  const struct mo_cell *captures, uint32_t count,
                                  struct mo_cell *out);

#endif
