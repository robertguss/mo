/* Runtime types and fixed layouts (bytecode sections 2.1, 2.3 and 2.4). This
 * header uses only base: the compiler and loader need no heap implementation. */
#ifndef MO_TYPES_H
#define MO_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#include "base/arena.h"
#include "base/base.h"

enum mo_type_kind {
    MO_TYPE_UNIT,
    MO_TYPE_BOOL,
    MO_TYPE_BYTE,
    MO_TYPE_INT,
    MO_TYPE_FLOAT,
    MO_TYPE_CHAR,
    MO_TYPE_TIME,
    MO_TYPE_DURATION,
    MO_TYPE_DECIMAL,
    MO_TYPE_RANGE,
    MO_TYPE_TEXT,
    MO_TYPE_BYTES,
    MO_TYPE_BIGINT,
    MO_TYPE_TUPLE,
    MO_TYPE_LIST,
    MO_TYPE_SET,
    MO_TYPE_MAP,
    MO_TYPE_RECORD,
    MO_TYPE_CHOICE,
    MO_TYPE_FUNCTION,
    MO_TYPE_ENV,
    MO_TYPE_CAPABILITY,
};

enum mo_capability {
    MO_CAP_ARGS,
    MO_CAP_ENV,
    MO_CAP_CONSOLE,
    MO_CAP_FILES,
    MO_CAP_READ_FILES,
    MO_CAP_CLOCK,
    MO_CAP_RANDOM,
};

enum mo_case_shape { MO_CASE_EMPTY, MO_CASE_INLINE, MO_CASE_REF, MO_CASE_BOXED };
enum mo_layout_kind { MO_LAYOUT_RECORD, MO_LAYOUT_TUPLE, MO_LAYOUT_CASE, MO_LAYOUT_CLOSURE };

/* Not an index. Also used for a layout's case or function when not applicable.
 * Built-in object layouts belong to stream 3 and use indices outside the fixed
 * layout table; this contract does not assign their numbers. */
#define MO_NO_INDEX UINT32_MAX

enum { MO_ABILITY_EQUAL = 1, MO_ABILITY_HASH = 2, MO_ABILITY_ORDER = 4 };

struct mo_type_field {
    const char *name; /* NULL only for an unnamed case field */
    uint32_t type;
};

struct mo_type_case {
    const char *name;
    const struct mo_type_field *fields;
    uint32_t field_count;
    bool unnamed; /* true exactly when its one field has no name */
};

/* Only the member selected by kind is used. Names are UTF-8, NUL-terminated.
 * parameter_count is zero for all but tuple, list, set, map and function.
 * Function parameters do not include env; result is its result type. Runtime
 * function types allow MO_LIMIT_BYTECODE_PARAMETERS, including generated promises;
 * the source checker separately enforces MO_LIMIT_PARAMETERS.
 * These are decoded structures, not a wire format. bytecode.md section 3 defines
 * each kind's encoding: names are string indices, and defined, reference,
 * has_fields and a case's unnamed are derived and never written. */
struct mo_type {
    enum mo_type_kind kind;
    const uint32_t *parameters;
    uint32_t parameter_count;
    uint32_t result;
    uint32_t function; /* env: index of its closure code */
    enum mo_capability capability;
    const char *name;                   /* record or choice: instantiation's printable name */
    const struct mo_type_field *fields; /* record */
    uint32_t field_count;
    const struct mo_type_case *cases; /* choice, in declaration order */
    uint32_t case_count;
    bool defined;    /* nominal declarations may be incomplete while building */
    bool reference;  /* computed by mo_types_finish, not supplied by callers */
    bool has_fields; /* choice: cached by define, so case shapes take constant time */
};

struct mo_layout {
    enum mo_layout_kind kind;
    uint32_t type;          /* record, tuple, choice, or the closure's function type */
    uint32_t case_index;    /* MO_LAYOUT_CASE only */
    uint32_t function;      /* MO_LAYOUT_CLOSURE only */
    const uint32_t *fields; /* capture types for a closure, never its function index */
    uint32_t field_count;
};

/* Read-only to consumers. Pointers into items/layouts may move on additions;
 * keep indices until finish. All storage belongs to arena, which must outlive
 * the table. No implicit built-ins: indices follow first use deterministically. */
struct mo_types {
    struct mo_arena *arena;
    struct mo_type *items;
    uint32_t count;
    uint32_t capacity;
    struct mo_layout *layouts;
    uint32_t layout_count;
    uint32_t layout_capacity;
    uint32_t *type_slots; /* private interning indices, zero means unused */
    uint32_t type_slot_count;
    uint32_t *layout_slots;
    uint32_t layout_slot_count;
    bool finished;
};

enum mo_type_problem {
    MO_TYPES_OK,
    MO_TYPES_INVALID,
    MO_TYPES_TYPE_LIMIT,
    MO_TYPES_LAYOUT_LIMIT,
    MO_TYPES_FIELD_LIMIT,
    MO_TYPES_CASE_LIMIT,
    MO_TYPES_PARAMETER_LIMIT,
    MO_TYPES_STRING_LIMIT,
    MO_TYPES_NO_MEMORY,
};

void mo_types_init(struct mo_types *types, struct mo_arena *arena);

/* Interns all non-nominal kinds, including built-ins, env and capabilities.
 * Type indices must already exist; incomplete nominal entries are allowed.
 * The loader may validate through a scratch builder, declaring nominal entries
 * before structural dependencies. Any remapping is private to that validation;
 * the image keeps file indices in its decoded and runtime tables. The loader
 * rejects cycles made only of structural types and duplicate interned types.
 * Copies active parameters. On failure, out and table entries are unchanged;
 * arena storage used by a failed allocation sequence lasts until arena_free. */
MO_MUST_USE enum mo_type_problem mo_types_intern(struct mo_types *types, const struct mo_type *type,
                                                 uint32_t *out);

/* Reserves a fresh nominal identity, even for the same printable name. The
 * compiler/loader keeps its declaration-to-index map. Define each exactly once,
 * after reserving everything it references, to allow mutual recursion. Copies
 * names, fields and cases; inputs may go away after each successful call. */
MO_MUST_USE enum mo_type_problem mo_types_declare(struct mo_types *types, enum mo_type_kind kind,
                                                  const char *name, uint32_t *out);
MO_MUST_USE enum mo_type_problem mo_types_define_record(struct mo_types *types, uint32_t type,
                                                        const struct mo_type_field *fields,
                                                        uint32_t count);
MO_MUST_USE enum mo_type_problem mo_types_define_choice(struct mo_types *types, uint32_t type,
                                                        const struct mo_type_case *cases,
                                                        uint32_t count);

/* Interns a fixed layout, checking its fields against its type. Case layouts
 * are only for boxed cases. Closure capture types and function linkage are
 * checked here for bounds, and against the function table by the verifier.
 * Call after finish; layouts may still be added after the types are sealed. */
MO_MUST_USE enum mo_type_problem mo_types_layout(struct mo_types *types,
                                                 const struct mo_layout *layout, uint32_t *out);

/* Refuses incomplete declarations and computes choice reference classifications
 * without C recursion. Seals types on success; repeat calls are harmless.
 * A nested choice with fields is always boxed, even when it is a reference
 * type: its tag and payload cannot fit together in eight bytes (2.3). */
MO_MUST_USE enum mo_type_problem mo_types_finish(struct mo_types *types);

/* Only on a successfully finished table, with valid type/case indices. */
MO_MUST_USE bool mo_type_is_reference(const struct mo_types *types, uint32_t type);
MO_MUST_USE enum mo_case_shape mo_case_shape(const struct mo_types *types, uint32_t type,
                                             uint32_t case_index);

#endif
