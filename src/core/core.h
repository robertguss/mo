/* Mo Core mirrors formal/MoCore/Syntax.lean, not the surface language or the VM's cells.
 * All pointers and lists belong to the caller's arena. Text may contain NUL. */
#ifndef MO_CORE_H
#define MO_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/arena.h"
#include "base/buffer.h"
#include "base/diag.h"

typedef struct {
    const char *bytes;
    size_t length;
} mo_core_string;

/* Arbitrary precision, including unchecked Int and Byte literals. Digits are ASCII, shortest
 * decimal, and never empty; zero is not negative. A natural uses the same representation. */
typedef struct {
    mo_core_string digits;
    bool negative;
} mo_core_integer;
typedef mo_core_integer mo_core_natural;
typedef uint64_t mo_core_bits;
typedef bool mo_core_boolean;
typedef uint32_t mo_core_character;

typedef struct mo_core_ty *mo_core_ty;
typedef struct mo_core_lit *mo_core_lit;
typedef struct mo_core_cap *mo_core_cap;
typedef struct {
    mo_core_ty *items;
    size_t count;
} mo_core_ty_list;
typedef struct {
    mo_core_integer *items;
    size_t count;
} mo_core_integer_list;

enum mo_core_cap_tag {
    MO_CORE_CAP_CONSOLE,
    MO_CORE_CAP_FILES,
    MO_CORE_CAP_READ_FILES,
    MO_CORE_CAP_CLOCK,
    MO_CORE_CAP_RANDOM,
    MO_CORE_CAP_ARGS,
    MO_CORE_CAP_ENV
};
struct mo_core_cap {
    enum mo_core_cap_tag tag;
};

enum mo_core_ty_tag {
    MO_CORE_TY_INT,
    MO_CORE_TY_BIG_INT,
    MO_CORE_TY_BYTE,
    MO_CORE_TY_DECIMAL,
    MO_CORE_TY_FLOAT,
    MO_CORE_TY_BOOL,
    MO_CORE_TY_CHAR,
    MO_CORE_TY_TEXT,
    MO_CORE_TY_BYTES,
    MO_CORE_TY_UNIT,
    MO_CORE_TY_RANGE,
    MO_CORE_TY_TIME,
    MO_CORE_TY_DURATION,
    MO_CORE_TY_TUPLE,
    MO_CORE_TY_LIST,
    MO_CORE_TY_MAP,
    MO_CORE_TY_SET,
    MO_CORE_TY_DATA,
    MO_CORE_TY_OPTION,
    MO_CORE_TY_RESULT,
    MO_CORE_TY_ORDERING,
    MO_CORE_TY_FILE_ERROR,
    MO_CORE_TY_FN,
    MO_CORE_TY_CAP
};
struct mo_core_ty {
    enum mo_core_ty_tag tag;
    union {
        mo_core_ty_list tuple;
        mo_core_ty list;
        struct {
            mo_core_ty k, v;
        } map;
        mo_core_ty set;
        mo_core_string data;
        mo_core_ty option;
        struct {
            mo_core_ty t, e;
        } result;
        struct {
            mo_core_ty_list params;
            mo_core_ty ret;
        } fn;
        mo_core_cap cap;
    } as;
};

enum mo_core_lit_tag {
    MO_CORE_LIT_INT,
    MO_CORE_LIT_BIG_INT,
    MO_CORE_LIT_BYTE,
    MO_CORE_LIT_DECIMAL,
    MO_CORE_LIT_FLOAT,
    MO_CORE_LIT_BOOL,
    MO_CORE_LIT_CHAR,
    MO_CORE_LIT_TEXT,
    MO_CORE_LIT_BYTES,
    MO_CORE_LIT_UNIT,
    MO_CORE_LIT_TIME,
    MO_CORE_LIT_DURATION
};
struct mo_core_lit {
    enum mo_core_lit_tag tag;
    union {
        mo_core_integer int_, big_int, byte, decimal, time, duration;
        mo_core_bits float_;
        mo_core_boolean bool_;
        mo_core_character char_;
        mo_core_string text;
        mo_core_integer_list bytes;
    } as;
};

struct mo_core;
MO_MUST_USE bool mo_core_check(const struct mo_core *core, struct mo_diags *diags);
MO_MUST_USE bool mo_core_print(const struct mo_core *core, struct mo_buffer *out);
MO_MUST_USE struct mo_core *mo_core_parse(struct mo_arena *arena, const char *text, size_t length,
                                          struct mo_diags *diags);

#endif
