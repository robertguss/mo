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
    MO_CORE_TY_ENTRY,
    MO_CORE_TY_ENTRY_KIND,
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

/* Each operation family is distinct; equal spellings in two families are not interchangeable. */
#include "core/ops.def"
#define MO_CORE_OP(tag, text) MO_CORE_INT_OP_##tag,
enum mo_core_int_op { MO_CORE_INT_OPS(MO_CORE_OP) };
#undef MO_CORE_OP
#define MO_CORE_OP(tag, text) MO_CORE_BIG_OP_##tag,
enum mo_core_big_op { MO_CORE_BIG_OPS(MO_CORE_OP) };
#undef MO_CORE_OP
#define MO_CORE_OP(tag, text) MO_CORE_BYTE_OP_##tag,
enum mo_core_byte_op { MO_CORE_BYTE_OPS(MO_CORE_OP) };
#undef MO_CORE_OP
#define MO_CORE_OP(tag, text) MO_CORE_DEC_OP_##tag,
enum mo_core_dec_op { MO_CORE_DEC_OPS(MO_CORE_OP) };
#undef MO_CORE_OP
#define MO_CORE_OP(tag, text) MO_CORE_FLOAT_OP_##tag,
enum mo_core_float_op { MO_CORE_FLOAT_OPS(MO_CORE_OP) };
#undef MO_CORE_OP
#define MO_CORE_OP(tag, text) MO_CORE_BASE_##tag,
enum mo_core_base { MO_CORE_BASES(MO_CORE_OP) };
#undef MO_CORE_OP
#define MO_CORE_OP(tag, text) MO_CORE_CMP_OP_##tag,
enum mo_core_cmp_op { MO_CORE_CMP_OPS(MO_CORE_OP) };
#undef MO_CORE_OP
#define MO_CORE_OP(tag, text) MO_CORE_CHAR_OP_##tag,
enum mo_core_char_op { MO_CORE_CHAR_OPS(MO_CORE_OP) };
#undef MO_CORE_OP
#define MO_CORE_OP(tag, text) MO_CORE_TEXT_OP_##tag,
enum mo_core_text_op { MO_CORE_TEXT_OPS(MO_CORE_OP) };
#undef MO_CORE_OP
#define MO_CORE_OP(tag, text) MO_CORE_BYTES_OP_##tag,
enum mo_core_bytes_op { MO_CORE_BYTES_OPS(MO_CORE_OP) };
#undef MO_CORE_OP
#define MO_CORE_OP(tag, text) MO_CORE_UNICODE_OP_##tag,
enum mo_core_unicode_op { MO_CORE_UNICODE_OPS(MO_CORE_OP) };
#undef MO_CORE_OP
#define MO_CORE_OP(tag, text) MO_CORE_RANGE_OP_##tag,
enum mo_core_range_op { MO_CORE_RANGE_OPS(MO_CORE_OP) };
#undef MO_CORE_OP
#define MO_CORE_OP(tag, text) MO_CORE_TIME_OP_##tag,
enum mo_core_time_op { MO_CORE_TIME_OPS(MO_CORE_OP) };
#undef MO_CORE_OP
#define MO_CORE_OP(tag, text) MO_CORE_LIST_OP_##tag,
enum mo_core_list_op_tag { MO_CORE_LIST_OP_LIT, MO_CORE_LIST_OPS(MO_CORE_OP) MO_CORE_LIST_OP_ZIP };
#undef MO_CORE_OP
struct mo_core_list_op {
    enum mo_core_list_op_tag tag;
    union {
        mo_core_natural lit;
        mo_core_ty zip;
    } as;
};
#define MO_CORE_OP(tag, text) MO_CORE_MAP_OP_##tag,
enum mo_core_map_op_tag { MO_CORE_MAP_OP_LIT, MO_CORE_MAP_OPS(MO_CORE_OP) };
#undef MO_CORE_OP
struct mo_core_map_op {
    enum mo_core_map_op_tag tag;
    mo_core_natural lit;
};
#define MO_CORE_OP(tag, text) MO_CORE_SET_OP_##tag,
enum mo_core_set_op { MO_CORE_SET_OPS(MO_CORE_OP) };
#undef MO_CORE_OP
#define MO_CORE_OP(tag, text) MO_CORE_IO_OP_##tag,
enum mo_core_io_op_tag {
    MO_CORE_IO_OPS(MO_CORE_OP) MO_CORE_IO_OP_READ_TEXT,
    MO_CORE_IO_OP_READ_BYTES,
    MO_CORE_IO_OP_LIST,
    MO_CORE_IO_OP_ENTRIES,
    MO_CORE_IO_OP_EXISTS,
    MO_CORE_IO_OP_WITHIN
};
#undef MO_CORE_OP
struct mo_core_io_op {
    enum mo_core_io_op_tag tag;
    bool ro;
};

enum mo_core_prim_tag {
    MO_CORE_PRIM_INT,
    MO_CORE_PRIM_BIG,
    MO_CORE_PRIM_BYTE,
    MO_CORE_PRIM_DEC,
    MO_CORE_PRIM_FLOAT,
    MO_CORE_PRIM_NOT,
    MO_CORE_PRIM_CMP,
    MO_CORE_PRIM_CHAR,
    MO_CORE_PRIM_TEXT,
    MO_CORE_PRIM_BYTES,
    MO_CORE_PRIM_UNICODE16,
    MO_CORE_PRIM_RANGE,
    MO_CORE_PRIM_TIME,
    MO_CORE_PRIM_LIST,
    MO_CORE_PRIM_MAP,
    MO_CORE_PRIM_SET,
    MO_CORE_PRIM_EQ,
    MO_CORE_PRIM_IO
};
struct mo_core_prim {
    enum mo_core_prim_tag tag;
    union {
        enum mo_core_int_op int_;
        enum mo_core_big_op big;
        enum mo_core_byte_op byte;
        enum mo_core_dec_op dec;
        enum mo_core_float_op float_;
        struct {
            enum mo_core_base base;
            enum mo_core_cmp_op op;
        } cmp;
        enum mo_core_char_op char_;
        enum mo_core_text_op text;
        enum mo_core_bytes_op bytes;
        enum mo_core_unicode_op unicode16;
        enum mo_core_range_op range;
        enum mo_core_time_op time;
        struct {
            mo_core_ty t;
            struct mo_core_list_op op;
        } list;
        struct {
            mo_core_ty k, v;
            struct mo_core_map_op op;
        } map;
        struct {
            mo_core_ty t;
            enum mo_core_set_op op;
        } set;
        mo_core_ty eq;
        struct mo_core_io_op io;
    } as;
};

typedef struct mo_core_expr *mo_core_expr;
typedef struct {
    mo_core_expr *items;
    size_t count;
} mo_core_expr_list;
typedef struct {
    mo_core_string *items;
    size_t count;
} mo_core_string_list;
struct mo_core_param {
    mo_core_string name;
    mo_core_ty ty;
};
typedef struct {
    struct mo_core_param *items;
    size_t count;
} mo_core_params;
struct mo_core_arm {
    mo_core_string ctor;
    mo_core_string_list names;
    mo_core_expr body;
};
typedef struct {
    struct mo_core_arm *items;
    size_t count;
} mo_core_arms;

enum mo_core_expr_tag {
    MO_CORE_EXPR_LIT,
    MO_CORE_EXPR_VAR,
    MO_CORE_EXPR_GLOBAL,
    MO_CORE_EXPR_LET,
    MO_CORE_EXPR_LAM,
    MO_CORE_EXPR_APP,
    MO_CORE_EXPR_CALL,
    MO_CORE_EXPR_TUPLE,
    MO_CORE_EXPR_PROJ,
    MO_CORE_EXPR_CON,
    MO_CORE_EXPR_CASE,
    MO_CORE_EXPR_IF,
    MO_CORE_EXPR_PRIM,
    MO_CORE_EXPR_STOP,
    MO_CORE_EXPR_FAIL,
    MO_CORE_EXPR_JOIN,
    MO_CORE_EXPR_JUMP
};
struct mo_core_expr {
    enum mo_core_expr_tag tag;
    union {
        mo_core_lit lit;
        mo_core_string var, global, stop;
        struct {
            mo_core_string name;
            mo_core_expr bound, body;
        } let_;
        struct {
            mo_core_params params;
            mo_core_expr body;
        } lam;
        struct {
            mo_core_expr f;
            mo_core_expr_list args;
        } app;
        struct {
            mo_core_string name;
            mo_core_expr_list args;
        } call;
        mo_core_expr_list tuple;
        struct {
            mo_core_expr expr;
            mo_core_natural index;
        } proj;
        struct {
            mo_core_ty ty;
            mo_core_string ctor;
            mo_core_expr_list args;
        } con;
        struct {
            mo_core_expr expr;
            mo_core_arms arms;
        } case_;
        struct {
            mo_core_expr cond, then_, else_;
        } if_;
        struct {
            struct mo_core_prim prim;
            mo_core_expr_list args;
        } prim;
        mo_core_expr fail;
        struct {
            mo_core_string name;
            mo_core_params params;
            mo_core_expr body, rest;
        } join;
        struct {
            mo_core_string name;
            mo_core_expr_list args;
        } jump;
    } as;
};

struct mo_core_ctor {
    mo_core_string name;
    mo_core_params fields;
};
typedef struct {
    struct mo_core_ctor *items;
    size_t count;
} mo_core_ctors;
struct mo_core_data {
    mo_core_string name;
    mo_core_ctors ctors;
};
typedef struct {
    struct mo_core_data *items;
    size_t count;
} mo_core_data_list;
enum mo_core_fun_kind {
    MO_CORE_FUNCTION,
    MO_CORE_TEST,
    MO_CORE_EXAMPLE,
    MO_CORE_PROMISE,
    MO_CORE_ANCHOR
};
struct mo_core_fun {
    mo_core_string name;
    mo_core_params params;
    mo_core_ty ret;
    mo_core_expr body;
    mo_core_string module;
    enum mo_core_fun_kind kind;
    mo_core_natural const_count;
};
typedef struct {
    struct mo_core_fun *items;
    size_t count;
} mo_core_funs;
struct mo_core_const {
    mo_core_string name;
    mo_core_ty ty;
    mo_core_expr body;
};
typedef struct {
    struct mo_core_const *items;
    size_t count;
} mo_core_consts;

/* Compiler-only side tables; see formal/TEXT.md for preorder and binder slot rules.
 * Everything is arena-owned, including filenames in mo_sources index order. */
struct mo_core_span {
    bool some;
    struct mo_span value;
};
struct mo_core_name {
    bool some;
    mo_core_string value;
};
struct mo_core_index {
    bool some;
    uint32_t value;
};
struct mo_core_expectation {
    bool some;
    bool value;
};
typedef struct {
    uint32_t *items;
    size_t count;
} mo_core_indices;
typedef struct {
    struct mo_core_span *items;
    size_t count;
} mo_core_spans;
typedef struct {
    struct mo_core_name *items;
    size_t count;
} mo_core_names;
struct mo_core_binding_meta {
    uint32_t node, slot;
    mo_core_string name;
};
typedef struct {
    struct mo_core_binding_meta *items;
    size_t count;
} mo_core_binding_metas;
struct mo_core_closure_meta {
    uint32_t node;
    mo_core_string name;
};
typedef struct {
    struct mo_core_closure_meta *items;
    size_t count;
} mo_core_closure_metas;
struct mo_core_body_meta {
    mo_core_spans spans;
    mo_core_names params;
    mo_core_binding_metas bindings;
    mo_core_closure_metas closures;
};
struct mo_core_spec_line {
    struct mo_core_span span;
    mo_core_string text;
};
typedef struct {
    struct mo_core_spec_line *items;
    size_t count;
} mo_core_spec_lines;
struct mo_core_fun_meta {
    mo_core_string name;
    struct mo_core_span span;
    struct mo_core_name text;
    struct mo_core_body_meta body;
    struct mo_core_index subject;
    struct mo_core_expectation allows;
    mo_core_spec_lines lines;
};
typedef struct {
    struct mo_core_fun_meta *items;
    size_t count;
} mo_core_fun_metas;
struct mo_core_const_meta {
    mo_core_string name;
    struct mo_core_span span;
    struct mo_core_body_meta body;
};
typedef struct {
    struct mo_core_const_meta *items;
    size_t count;
} mo_core_const_metas;
struct mo_core_exposed_type {
    uint32_t index;
    bool contents_exposed;
};
typedef struct {
    struct mo_core_exposed_type *items;
    size_t count;
} mo_core_exposed_types;
struct mo_core_module_meta {
    mo_core_string name;
    mo_core_indices data, funs, consts;
    mo_core_exposed_types exposed_types;
    mo_core_indices exposed_funs;
};
typedef struct {
    struct mo_core_module_meta *items;
    size_t count;
} mo_core_module_metas;
struct mo_core_metadata {
    bool source; /* false is the explicit noSource form, never fabricated source information */
    mo_core_string_list files;
    mo_core_module_metas modules;
    mo_core_fun_metas funs;
    mo_core_const_metas consts;
};
/* Program in Lean. Constants are in dependency order; entry is a resolved lookup key.
 * Metadata has no part in evaluation, typing or safety proofs. */
struct mo_core {
    mo_core_data_list data;
    mo_core_funs funs;
    mo_core_consts consts;
    mo_core_string entry;
    struct mo_core_metadata metadata;
};

/* Lossless transport is separate from VM values and structural observations. */
typedef struct mo_core_value *mo_core_value;
typedef struct {
    mo_core_value *items;
    size_t count;
} mo_core_values;
struct mo_core_value_pair {
    mo_core_value key, value;
};
typedef struct {
    struct mo_core_value_pair *items;
    size_t count;
} mo_core_value_pairs;
struct mo_core_binding {
    mo_core_string name;
    mo_core_value value;
};
typedef struct {
    struct mo_core_binding *items;
    size_t count;
} mo_core_env;
struct mo_core_root {
    bool some;
    mo_core_string text;
};
/* Independent of the folder root: none is real, some selects World.fakes. */
struct mo_core_fake {
    bool some;
    mo_core_natural index;
};
enum mo_core_value_tag {
    MO_CORE_VALUE_INT,
    MO_CORE_VALUE_BIG_INT,
    MO_CORE_VALUE_BYTE,
    MO_CORE_VALUE_DECIMAL,
    MO_CORE_VALUE_FLOAT,
    MO_CORE_VALUE_BOOL,
    MO_CORE_VALUE_CHAR,
    MO_CORE_VALUE_TEXT,
    MO_CORE_VALUE_BYTES,
    MO_CORE_VALUE_UNIT,
    MO_CORE_VALUE_RANGE,
    MO_CORE_VALUE_TIME,
    MO_CORE_VALUE_DURATION,
    MO_CORE_VALUE_TUPLE,
    MO_CORE_VALUE_LIST,
    MO_CORE_VALUE_MAP,
    MO_CORE_VALUE_SET,
    MO_CORE_VALUE_CON,
    MO_CORE_VALUE_CLOSURE,
    MO_CORE_VALUE_CAP
};
struct mo_core_value {
    enum mo_core_value_tag tag;
    union {
        mo_core_integer int_, big_int, byte, decimal, time, duration;
        mo_core_bits float_;
        bool bool_;
        mo_core_character char_;
        mo_core_string text;
        mo_core_integer_list bytes;
        struct {
            mo_core_integer start, end;
        } range;
        mo_core_values tuple, list, set;
        mo_core_value_pairs map;
        struct {
            mo_core_string ctor;
            mo_core_values fields;
        } con;
        struct {
            mo_core_env env;
            mo_core_params params;
            mo_core_expr body;
            mo_core_env consts;
        } closure;
        struct {
            mo_core_cap cap;
            struct mo_core_root root;
            struct mo_core_fake fake;
        } cap;
    } as;
};
struct mo_core_string_pair {
    mo_core_string key, value;
};
typedef struct {
    struct mo_core_string_pair *items;
    size_t count;
} mo_core_string_pairs;
/* xoshiro256** words in reference s[0..3] order, not a seed or byte array. */
struct mo_core_random_state {
    mo_core_bits s0, s1, s2, s3;
};
/* A fake state has no kind or nested store. List and pair order is retained. */
struct mo_core_cap_state {
    mo_core_string_list args;
    mo_core_string_pairs env;
    mo_core_string_list stdin_, stdout_, stderr_;
    mo_core_string_pairs files;
    mo_core_integer now;
    struct mo_core_random_state random;
};
typedef struct {
    struct mo_core_cap_state *items;
    size_t count;
} mo_core_cap_states;
struct mo_core_world {
    mo_core_string_list args;
    mo_core_string_pairs env;
    mo_core_string_list stdin_, stdout_, stderr_;
    mo_core_string_pairs files;
    mo_core_integer now;
    struct mo_core_random_state random;
    mo_core_cap_states fakes;
};
enum mo_core_bug_tag {
    MO_CORE_BUG_OVERFLOW,
    MO_CORE_BUG_DIV_BY_ZERO,
    MO_CORE_BUG_STOP,
    MO_CORE_BUG_OUTSIDE,
    MO_CORE_BUG_REPEATED_KEY,
    MO_CORE_BUG_FILE_FOLDER_CONFLICT
};
struct mo_core_bug {
    enum mo_core_bug_tag tag;
    mo_core_string detail;
};
enum mo_core_outcome_tag {
    MO_CORE_OUTCOME_OK,
    MO_CORE_OUTCOME_STOPPED,
    MO_CORE_OUTCOME_FAILED,
    MO_CORE_OUTCOME_JUMPED,
    MO_CORE_OUTCOME_WRONG,
    MO_CORE_OUTCOME_OUT_OF_FUEL
};
struct mo_core_outcome {
    enum mo_core_outcome_tag tag;
    union {
        struct {
            mo_core_value value;
            struct mo_core_world world;
        } ok;
        struct {
            struct mo_core_bug bug;
            struct mo_core_world world;
        } stopped;
        struct {
            mo_core_string detail;
            struct mo_core_world world;
        } failed;
        struct {
            mo_core_string name;
            mo_core_values values;
            struct mo_core_world world;
        } jumped;
        mo_core_string wrong;
    } as;
};
struct mo_core_effects {
    mo_core_string_list stdout_, stderr_;
    mo_core_string_pairs files;
    mo_core_cap_states fakes;
};
enum mo_core_report_tag {
    MO_CORE_REPORT_RETURNED,
    MO_CORE_REPORT_STOPPED,
    MO_CORE_REPORT_FAILED,
    MO_CORE_REPORT_OUT_OF_FUEL,
    MO_CORE_REPORT_WRONG,
    MO_CORE_REPORT_PARSE_ERROR,
    MO_CORE_REPORT_ILL_TYPED,
    MO_CORE_REPORT_INVALID_RUN,
    MO_CORE_REPORT_IO_ERROR
};
struct mo_core_report {
    enum mo_core_report_tag tag;
    union {
        struct {
            mo_core_string value;
            struct mo_core_effects effects;
        } returned;
        struct {
            struct mo_core_bug bug;
            struct mo_core_effects effects;
        } stopped;
        struct {
            mo_core_string detail;
            struct mo_core_effects effects;
        } failed;
        mo_core_string detail;
    } as;
};
enum mo_core_selection_tag { MO_CORE_SELECTION_ENTRY, MO_CORE_SELECTION_TEST };
struct mo_core_selection {
    enum mo_core_selection_tag tag;
    union {
        mo_core_values args;
        mo_core_string name;
    } as;
};
struct mo_core_run {
    struct mo_core program;
    struct mo_core_world world;
    struct mo_core_selection selection;
    mo_core_natural fuel;
};

/* A document's category is supplied by the caller, never guessed from its tag. Parse copies all
 * input into arena, returns NULL on error and adds one diagnostic (or increments dropped). Its
 * diagnostic spans use file 0 and byte offsets in text. Failure may leave allocations in arena.
 * Print appends a complete document including LF; on failure it restores out's length. */
enum mo_core_text_kind {
    MO_CORE_TEXT_TYPE,
    MO_CORE_TEXT_CAP,
    MO_CORE_TEXT_LITERAL,
    MO_CORE_TEXT_PRIMITIVE,
    MO_CORE_TEXT_EXPR,
    MO_CORE_TEXT_PROGRAM,
    MO_CORE_TEXT_VALUE,
    MO_CORE_TEXT_WORLD,
    MO_CORE_TEXT_BUG,
    MO_CORE_TEXT_OUTCOME,
    MO_CORE_TEXT_EFFECTS,
    MO_CORE_TEXT_REPORT,
    MO_CORE_TEXT_SELECTION,
    MO_CORE_TEXT_RUN,
    MO_CORE_TEXT_CAP_STATE,
    MO_CORE_TEXT_RANDOM_STATE
};
#define MO_CORE_TEXT_VERSION "0.6"
MO_MUST_USE void *mo_core_text_parse(struct mo_arena *arena, enum mo_core_text_kind kind,
                                     const char *text, size_t length, struct mo_diags *diags);
MO_MUST_USE bool mo_core_text_print(enum mo_core_text_kind kind, const void *value,
                                    struct mo_buffer *out);

/* The program-only interfaces used by lowering and code generation (architecture 12).
 * Stream 2 supplies mo_core_check; parsing deliberately does not type-check. */
/* Shape only, not source truth or Core typing; false also on scratch allocation failure.
 * Inputs must be complete arena trees with readable arrays, as for printing. */
MO_MUST_USE bool mo_core_metadata_valid(const struct mo_core *core);
MO_MUST_USE bool mo_core_check(const struct mo_core *core, struct mo_diags *diags);
MO_MUST_USE bool mo_core_print(const struct mo_core *core, struct mo_buffer *out);
MO_MUST_USE struct mo_core *mo_core_parse(struct mo_arena *arena, const char *text, size_t length,
                                          struct mo_diags *diags);

#endif
