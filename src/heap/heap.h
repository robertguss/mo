/* Cells and the C value-building interface (architecture section 12).
 * Stream 3 implements the heap and builders; cells need no process or heap. */
#ifndef MO_HEAP_H
#define MO_HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/base.h"
#include "base/buffer.h"
#include "base/limits.h"
#include "heap/types.h"

/* A cell is native-endian runtime storage, not a serialized constant. Alignment
 * is 8, so fields can follow the 8-byte object header without padding. Decimal
 * uses memcpy, not an aligned mo_i128 lvalue. Unused bytes are always zero. */
struct mo_cell {
    _Alignas(8) uint8_t bytes[16];
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

struct mo_heap;

struct mo_heap_image;
/* One context per VM run: borrowed sealed types, shared abilities/layout index,
 * derived secret hash/index domains and an owned initialization heap. NULL on
 * randomness or memory failure: errno is EIO for entropy, ENOSPC for the
 * heap limit, ENOMEM for native allocation. The context and types outlive all its heaps.
 * Layouts may be appended during initialization, before parallel execution;
 * refresh updates the index only when its layout count changes. */
MO_MUST_USE struct mo_heap_image *mo_heap_image_new(const struct mo_types *types,
                                                    const struct mo_limits *limits);
void mo_heap_image_free(struct mo_heap_image *image);
MO_MUST_USE bool mo_heap_image_refresh(struct mo_heap_image *image);
MO_MUST_USE struct mo_heap *mo_heap_image_constants(struct mo_heap_image *image);
/* The VM's process owns this separate heap. Creation does no entropy reads,
 * type walks or sorting. Native metadata and scratch count against heap_size.
 * Free reclaims every allocation, including saturated and abandoned values. */
MO_MUST_USE struct mo_heap *mo_heap_new(struct mo_heap_image *image,
                                        const struct mo_limits *limits);
void mo_heap_free(struct mo_heap *process);
MO_MUST_USE size_t mo_heap_used(const struct mo_heap *process);
MO_MUST_USE size_t mo_heap_objects(const struct mo_heap *process);

/* Temporary native operands and walk storage share the heap's remaining
 * budget, including concurrent scratch arenas. Free before ending the heap. */
struct mo_scratch {
    struct mo_arena arena;
    struct mo_heap *process;
};
void mo_scratch_init(struct mo_scratch *scratch, struct mo_heap *process);
void mo_scratch_free(struct mo_scratch *scratch);
MO_MUST_USE void *mo_scratch_resize(struct mo_scratch *scratch, void *bytes, size_t old_size,
                                    size_t new_size);

/* Borrowed views last until the owner is dropped or updated. */
MO_MUST_USE const uint8_t *mo_value_data(struct mo_cell value, size_t *length);
/* BigInt only. Its magnitude is the read-only byte view above. */
MO_MUST_USE enum mo_bigint_sign mo_value_bigint_sign(struct mo_cell value);
/* Append D236's shortest Float spelling without allocating process scratch.
 * Failure preserves the existing output, as mo_value_print does. */
MO_MUST_USE bool mo_float_text(double value, struct mo_buffer *out);
MO_MUST_USE struct mo_cell mo_value_field(struct mo_cell value, uint32_t field);
MO_MUST_USE uint32_t mo_list_size(struct mo_cell list);
MO_MUST_USE bool mo_list_get(struct mo_cell list, uint32_t index, struct mo_cell *out);
/* Updates consume *list only on success; element is borrowed. On failure both
 * are unchanged. Shared and immortal values are copied before modification. */
MO_MUST_USE bool mo_list_append(struct mo_heap *process, uint32_t type, struct mo_cell *list,
                                struct mo_cell element);
MO_MUST_USE bool mo_list_set(struct mo_heap *process, uint32_t type, struct mo_cell *list,
                             uint32_t index, struct mo_cell element);
MO_MUST_USE bool mo_list_prepend(struct mo_heap *process, uint32_t type, struct mo_cell *list,
                                 struct mo_cell element);
MO_MUST_USE bool mo_list_concat(struct mo_heap *process, uint32_t type, struct mo_cell *list,
                                struct mo_cell right);
/* Valid ranges/indices are required. These consume *list only on success and
 * keep unchanged lists even when shared. Unique shrinking cannot fail: below
 * one quarter capacity it tries an exact buffer, retaining storage on refusal.
 * Slicing advances a live window, with compaction past half capacity: repeated
 * unique drop(1) or remove_at(0) has amortized constant storage work, plus
 * destruction of discarded references. Unique reordering does not allocate.
 * concat's right operand may alias *list. */
MO_MUST_USE bool mo_list_slice(struct mo_heap *process, uint32_t type, struct mo_cell *list,
                               uint32_t start, uint32_t count);
MO_MUST_USE bool mo_list_reverse(struct mo_heap *process, uint32_t type, struct mo_cell *list);
MO_MUST_USE bool mo_list_remove_at(struct mo_heap *process, uint32_t type, struct mo_cell *list,
                                   uint32_t index);

/* Consume the left Text on success; the right Text is borrowed and may alias
 * it. Unique storage grows geometrically. slice's byte range must end at UTF-8
 * boundaries; both operations preserve valid UTF-8 without validating again.
 * On false every input, count and output is unchanged. Unchanged slices keep
 * the original object even when shared or immortal. Unique slicing cannot
 * fail; below one quarter capacity it tries best-effort exact contraction. */
MO_MUST_USE bool mo_text_concat(struct mo_heap *process, struct mo_cell *text,
                                struct mo_cell right);
MO_MUST_USE bool mo_text_slice(struct mo_heap *process, struct mo_cell *text, size_t start,
                               size_t length);

/* Generic operations borrow values. false means unavailable ability or out of
 * memory; result is unchanged. No work cap on equality, ordering or hashing.
 * Structural compare ignores custom Mo compare functions. Hashes use the run's
 * hash domain, never its indexing key. No operation uses C recursion. */
MO_MUST_USE bool mo_value_equal(struct mo_heap *process, uint32_t type, struct mo_cell left,
                                struct mo_cell right, bool *out);
MO_MUST_USE bool mo_value_hash(struct mo_heap *process, uint32_t type, struct mo_cell value,
                               uint64_t *out);
MO_MUST_USE bool mo_value_compare(struct mo_heap *process, uint32_t type, struct mo_cell left,
                                  struct mo_cell right, int *out);
/* Append structural text, without executing Mo code. Caller-owned output is
 * outside the process heap; scratch is charged to it. On failure the original
 * output length and contents remain (capacity may grow). A defect renderer
 * must use a fixed fallback rather than stop when printing runs out of room. */
MO_MUST_USE bool mo_value_print(struct mo_heap *process, uint32_t type, struct mo_cell value,
                                struct mo_buffer *out);
/* Stable saved-value format in docs/design/saved-state.md. Encode borrows a
 * valid typed value. Decode treats every byte as untrusted, including its
 * schema, and publishes one owned value only after consuming the entire input.
 * Refusal preserves out and releases all temporary values. Neither format
 * contains native pointers, layouts, capabilities or function values. */
MO_MUST_USE bool mo_value_encode(struct mo_heap *process, uint32_t type, struct mo_cell value,
                                 struct mo_buffer *out);
MO_MUST_USE bool mo_value_decode(struct mo_heap *process, uint32_t type, const uint8_t *bytes,
                                 size_t length, struct mo_cell *out);
MO_MUST_USE bool mo_value_copy_saved(struct mo_heap *from, struct mo_heap *to, uint32_t type,
                                     struct mo_cell value, struct mo_cell *out,
                                     size_t *encoded_length, const char **failure);
/* Lookup borrows its result; found=false leaves out unchanged. Operations
 * return false on limits or allocation failure, leaving every input unchanged.
 * Iteration's position is an entry cursor, not a live-element index. */
MO_MUST_USE uint32_t mo_table_size(struct mo_cell table);
MO_MUST_USE bool mo_table_next(struct mo_cell table, uint32_t *position, struct mo_cell *key,
                               struct mo_cell *value);
MO_MUST_USE bool mo_table_get(struct mo_heap *process, uint32_t type, struct mo_cell table,
                              struct mo_cell key, bool *found, struct mo_cell *out);
MO_MUST_USE bool mo_table_set(struct mo_heap *process, uint32_t type, struct mo_cell *table,
                              struct mo_cell key, struct mo_cell value);
MO_MUST_USE bool mo_table_remove(struct mo_heap *process, uint32_t type, struct mo_cell *table,
                                 struct mo_cell key);
/* Set union consumes *left on success and borrows right (which may alias it).
 * Hash/equality, capacity and index planning precede ownership or mutation, so
 * a refusal preserves both sets. Unique left storage is reused when it fits. */
MO_MUST_USE bool mo_set_union(struct mo_heap *process, uint32_t type, struct mo_cell *left,
                              struct mo_cell right);

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
MO_MUST_USE struct mo_cell mo_value_dup(struct mo_heap *process, uint32_t type,
                                        struct mo_cell value);
void mo_value_drop(struct mo_heap *process, uint32_t type, struct mo_cell value);

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
 * and bigint use ENOSPC/ENOMEM for allocation refusal; invalid UTF-8 is EINVAL.
 * They copy their input bytes; length is at most MO_LIMIT_VALUE_BYTES,
 * and a byte pointer may be NULL only when length is zero. bytes accepts every
 * byte sequence, including invalid UTF-8 and NUL. bigint uses base's shared
 * signs and bytecode 3.1's little-endian magnitude: ZERO iff length is zero;
 * POSITIVE/NEGATIVE require a nonzero final byte. Invalid signs, noncanonical
 * magnitudes, a length past the limit, or allocation/heap-limit failure return
 * false, leaving out and inputs unchanged and keeping no input ownership.
 * These are C builders, not bytecode verification. */
MO_MUST_USE bool mo_value_text(struct mo_heap *process, const char *utf8, size_t length,
                               struct mo_cell *out);
MO_MUST_USE bool mo_value_bytes(struct mo_heap *process, const uint8_t *bytes, size_t length,
                                struct mo_cell *out);
MO_MUST_USE bool mo_value_bigint(struct mo_heap *process, enum mo_bigint_sign sign,
                                 const uint8_t *magnitude, size_t length, struct mo_cell *out);
MO_MUST_USE bool mo_value_construct(struct mo_heap *process, uint32_t layout,
                                    const struct mo_cell *fields, uint32_t count,
                                    struct mo_cell *out);
MO_MUST_USE bool mo_value_case(struct mo_heap *process, uint32_t type, uint32_t tag,
                               const struct mo_cell *fields, uint32_t count, struct mo_cell *out);
MO_MUST_USE bool mo_value_list(struct mo_heap *process, uint32_t type,
                               const struct mo_cell *elements, uint32_t count, struct mo_cell *out);
MO_MUST_USE bool mo_value_map(struct mo_heap *process, uint32_t type, const struct mo_cell *keys,
                              const struct mo_cell *values, uint32_t count, struct mo_cell *out);
MO_MUST_USE bool mo_value_set(struct mo_heap *process, uint32_t type,
                              const struct mo_cell *elements, uint32_t count, struct mo_cell *out);
MO_MUST_USE bool mo_value_closure(struct mo_heap *process, uint32_t function,
                                  const struct mo_cell *captures, uint32_t count,
                                  struct mo_cell *out);

/* VM variants transfer field ownership on success, with no dup/drop pair.
 * Invalid layout/count invariants assert. On memory failure fields and outputs
 * are unchanged, with errno ENOSPC for the heap limit or ENOMEM for native
 * allocation, as for scratch and generic walks with a verified ability. */
MO_MUST_USE bool mo_value_construct_owned(struct mo_heap *process, uint32_t layout,
                                          const struct mo_cell *fields, uint32_t count,
                                          struct mo_cell *out);
MO_MUST_USE bool mo_value_case_owned(struct mo_heap *process, uint32_t type, uint32_t tag,
                                     const struct mo_cell *fields, uint32_t count,
                                     struct mo_cell *out);
MO_MUST_USE bool mo_value_closure_owned(struct mo_heap *process, uint32_t function,
                                        const struct mo_cell *captures, uint32_t count,
                                        struct mo_cell *out);
/* Consume the record/tuple and replacement on success, copying only when
 * shared. On failure both values and out are unchanged. */
MO_MUST_USE bool mo_value_set_field_owned(struct mo_heap *process, uint32_t type,
                                          struct mo_cell value, uint32_t field,
                                          struct mo_cell replacement, struct mo_cell *out);

/* reset consumes a fixed-layout record, tuple or boxed choice. Tokens own
 * cleared storage, not values. reuse borrows fields, consumes the token only
 * on success, and preserves it on failure. The VM uses reuse_owned to transfer
 * its field ownership directly. Never copy a token's ownership. */
struct mo_token {
    struct mo_object *memory;
    uint32_t layout;
};
MO_MUST_USE struct mo_token mo_value_reset(struct mo_heap *process, uint32_t type,
                                           struct mo_cell value);
MO_MUST_USE bool mo_value_reuse(struct mo_heap *process, struct mo_token *token,
                                const struct mo_cell *fields, uint32_t count, struct mo_cell *out);
MO_MUST_USE bool mo_value_reuse_owned(struct mo_heap *process, struct mo_token *token,
                                      const struct mo_cell *fields, uint32_t count,
                                      struct mo_cell *out);
void mo_token_free(struct mo_heap *process, struct mo_token *token);
/* Image initialization only: the whole immutable graph becomes immortal.
 * Functions may capture immutable values; no reachable capability/state is
 * allowed. First compute caches, then freeze them. false means out of memory;
 * counts and immortality remain unchanged. The mark walk itself allocates
 * nothing and is iterative, including shared DAGs. */
MO_MUST_USE bool mo_value_make_immortal(struct mo_heap *process, uint32_t type,
                                        struct mo_cell value);

/* Host storage, not host operations. A state shares typed mutable slots and
 * accepts Equal types, or capability parents of deferred real folder handles.
 * The host must keep capability parent links acyclic. Fakes use Equal slots.
 * Its slot types never change; get borrows, set borrows its input. */
struct mo_state;
MO_MUST_USE bool mo_state_new(struct mo_heap *process, const uint32_t *types,
                              const struct mo_cell *values, uint32_t count, struct mo_state **out);
MO_MUST_USE struct mo_state *mo_state_dup(struct mo_state *state);
void mo_state_drop(struct mo_heap *process, struct mo_state *state);
MO_MUST_USE struct mo_cell mo_state_get(struct mo_state *state, uint32_t index);
MO_MUST_USE uint32_t mo_state_type(struct mo_state *state, uint32_t index);
/* Host-only owned slot access for transactional collection updates. Keep its
 * declared type and one owned cell valid on both success and failure. */
MO_MUST_USE struct mo_cell *mo_state_slot(struct mo_state *state, uint32_t index);
void mo_state_set(struct mo_heap *process, struct mo_state *state, uint32_t index,
                  struct mo_cell value);

/* operations is an opaque, borrowed, static host table. state and optional
 * Text prefix are borrowed. On success the handle transfers to the heap;
 * release(handle) runs once on final drop or teardown, and must neither touch
 * child values nor reenter the heap. On false everything remains with caller.
 * Real folder handle limits belong to host; this layer never opens a handle. */
struct mo_capability_info {
    const void *operations;
    struct mo_state *state;
    struct mo_cell prefix;
    intptr_t handle;
    void (*release)(intptr_t handle);
    bool has_prefix;
    bool read_only;
};
MO_MUST_USE bool mo_value_capability(struct mo_heap *process, uint32_t type,
                                     const struct mo_capability_info *info, struct mo_cell *out);
/* Copy optional native bytes into the same charged allocation. When size is
 * nonzero, release receives their address as handle instead of info->handle.
 * Bytes have pointer alignment and no heap children; release may read them.
 * Size is at most MO_LIMIT_STRING_SIZE. Failure transfers nothing. */
MO_MUST_USE bool mo_value_capability_native(struct mo_heap *process, uint32_t type,
                                            const struct mo_capability_info *info,
                                            const void *native, size_t size, struct mo_cell *out);
MO_MUST_USE const struct mo_capability_info *mo_capability_get(struct mo_cell value);

#endif
