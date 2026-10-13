#include "codegen/operations.h"

static const struct mo_codegen_operation primitives[] = {
#define PRIM(id, module, name, sig, modes, bugs)                                                   \
    {.opcode = MO_OP_PRIM, .row = MO_PRIM_##id, .signature = sig, .params = modes},
#include "base/prims.def"
};

static const struct mo_codegen_operation hosts[] = {
#define HOSTOP(id, module, name, sig, modes, bugs, spec)                                           \
    {.opcode = MO_OP_HOST, .row = MO_HOST_##id, .signature = sig, .params = modes},
#include "base/hostops.def"
};

struct mo_codegen_operation mo_codegen_primitive(enum mo_primitive_id row) {
    MO_ASSERT((unsigned)row < sizeof primitives / sizeof *primitives);
    return primitives[row];
}
static MO_MUST_USE struct mo_codegen_operation native(enum mo_opcode opcode, const char *sig,
                                                      const char *params) {
    return (struct mo_codegen_operation){.opcode = opcode, .signature = sig, .params = params};
}
/* Core's first six operations in these three families are the native arithmetic opcodes. */
static MO_MUST_USE struct mo_codegen_operation arithmetic(unsigned op, unsigned family) {
    static const enum mo_opcode ops[][3] = {{MO_OP_ADD_INT, MO_OP_ADD_DECIMAL, MO_OP_ADD_FLOAT},
                                            {MO_OP_SUB_INT, MO_OP_SUB_DECIMAL, MO_OP_SUB_FLOAT},
                                            {MO_OP_MUL_INT, MO_OP_MUL_DECIMAL, MO_OP_MUL_FLOAT},
                                            {MO_OP_DIV_INT, MO_OP_DIV_DECIMAL, MO_OP_DIV_FLOAT},
                                            {MO_OP_REM_INT, MO_OP_REM_DECIMAL, MO_OP_REM_FLOAT},
                                            {MO_OP_NEG_INT, MO_OP_NEG_DECIMAL, MO_OP_NEG_FLOAT}};
    static const char *const binary[] = {"fn(Int, Int) : Int", "fn(Decimal, Decimal) : Decimal",
                                         "fn(Float, Float) : Float"};
    static const char *const unary[] = {"fn(Int) : Int", "fn(Decimal) : Decimal",
                                        "fn(Float) : Float"};
    MO_ASSERT(op < 6 && family < 3);
    return native(ops[op][family], op == 5 ? unary[family] : binary[family], op == 5 ? "b" : "bb");
}
static MO_MUST_USE struct mo_codegen_operation comparison(const struct mo_core_prim *p) {
    static const enum mo_opcode lt[] = {MO_OP_LT_INT,     MO_OP_INVALID,    MO_OP_LT_BYTE,
                                        MO_OP_LT_DECIMAL, MO_OP_LT_FLOAT,   MO_OP_LT_BOOL,
                                        MO_OP_LT_CHAR,    MO_OP_INVALID,    MO_OP_INVALID,
                                        MO_OP_LT_TIME,    MO_OP_LT_DURATION};
    static const enum mo_opcode le[] = {MO_OP_LE_INT,     MO_OP_INVALID,    MO_OP_LE_BYTE,
                                        MO_OP_LE_DECIMAL, MO_OP_LE_FLOAT,   MO_OP_LE_BOOL,
                                        MO_OP_LE_CHAR,    MO_OP_INVALID,    MO_OP_INVALID,
                                        MO_OP_LE_TIME,    MO_OP_LE_DURATION};
    static const char *const signatures[] = {"fn(Int, Int) : Bool",
                                             "",
                                             "fn(Byte, Byte) : Bool",
                                             "fn(Decimal, Decimal) : Bool",
                                             "fn(Float, Float) : Bool",
                                             "fn(Bool, Bool) : Bool",
                                             "fn(Char, Char) : Bool",
                                             "",
                                             "",
                                             "fn(Time, Time) : Bool",
                                             "fn(Duration, Duration) : Bool"};
    enum mo_core_base base = p->as.cmp.base;
    bool inclusive = p->as.cmp.op == MO_CORE_CMP_OP_LE || p->as.cmp.op == MO_CORE_CMP_OP_GE;
    struct mo_codegen_operation out;
    if (base == MO_CORE_BASE_BIG_INT || base == MO_CORE_BASE_TEXT || base == MO_CORE_BASE_BYTES) {
        out = primitives[base == MO_CORE_BASE_BIG_INT ? MO_PRIM_BIGINT_COMPARE
                         : base == MO_CORE_BASE_TEXT  ? MO_PRIM_TEXT_COMPARE
                                                      : MO_PRIM_BYTES_COMPARE];
        out.compare = true;
    } else
        out = native(inclusive ? le[base] : lt[base], signatures[base], "bb");
    out.reverse = p->as.cmp.op == MO_CORE_CMP_OP_GT || p->as.cmp.op == MO_CORE_CMP_OP_GE;
    return out;
}

/* These arrays map names, not signatures or ownership: those come only from the .def rows. */
bool mo_codegen_operation(const struct mo_core_prim *p, struct mo_codegen_operation *out) {
    enum mo_primitive_id row = MO_PRIM_COUNT;
    switch (p->tag) {
    case MO_CORE_PRIM_INT: {
        if (p->as.int_ <= MO_CORE_INT_OP_NEG) {
            *out = arithmetic((unsigned)p->as.int_, 0);
            return true;
        }
        static const enum mo_primitive_id rows[] = {MO_PRIM_INT_ABS,
                                                    MO_PRIM_INT_MIN,
                                                    MO_PRIM_INT_MAX,
                                                    MO_PRIM_INT_POW,
                                                    MO_PRIM_INT_FLOOR_DIV,
                                                    MO_PRIM_INT_FLOOR_MOD,
                                                    MO_PRIM_INT_CHECKED_ADD,
                                                    MO_PRIM_INT_CHECKED_SUB,
                                                    MO_PRIM_INT_CHECKED_MUL,
                                                    MO_PRIM_INT_WRAPPING_ADD,
                                                    MO_PRIM_INT_WRAPPING_SUB,
                                                    MO_PRIM_INT_WRAPPING_MUL,
                                                    MO_PRIM_INT_SATURATING_ADD,
                                                    MO_PRIM_INT_SATURATING_SUB,
                                                    MO_PRIM_INT_SATURATING_MUL,
                                                    MO_PRIM_INT_TO_TEXT,
                                                    MO_PRIM_INT_TO_DECIMAL,
                                                    MO_PRIM_INT_TO_BIG,
                                                    MO_PRIM_INT_TO_FLOAT,
                                                    MO_PRIM_INT_TO_CHAR,
                                                    MO_PRIM_INT_TO_BYTE,
                                                    MO_PRIM_INT_SECONDS,
                                                    MO_PRIM_INT_MILLISECONDS,
                                                    MO_PRIM_INT_MINUTES,
                                                    MO_PRIM_INT_HOURS,
                                                    MO_PRIM_INT_DAYS};
        row = rows[p->as.int_ - MO_CORE_INT_OP_ABS];
        break;
    }
    case MO_CORE_PRIM_BIG: {
#define ROW(id, wire) MO_PRIM_BIGINT_##id,
        static const enum mo_primitive_id rows[] = {MO_CORE_BIG_OPS(ROW)};
#undef ROW
        row = rows[p->as.big];
        break;
    }
    case MO_CORE_PRIM_BYTE:
        row = p->as.byte == MO_CORE_BYTE_OP_TO_INT ? MO_PRIM_BYTE_TO_INT : MO_PRIM_BYTE_TO_TEXT;
        break;
    case MO_CORE_PRIM_DEC: {
        if (p->as.dec <= MO_CORE_DEC_OP_NEG) {
            *out = arithmetic((unsigned)p->as.dec, 1);
            return true;
        }
        static const enum mo_primitive_id rows[] = {
            MO_PRIM_DECIMAL_ABS,     MO_PRIM_DECIMAL_MIN,     MO_PRIM_DECIMAL_MAX,
            MO_PRIM_DECIMAL_FLOOR,   MO_PRIM_DECIMAL_CEIL,    MO_PRIM_DECIMAL_ROUND,
            MO_PRIM_DECIMAL_TO_INT,  MO_PRIM_DECIMAL_TO_TEXT, MO_PRIM_DECIMAL_TO_FIXED,
            MO_PRIM_DECIMAL_TO_FLOAT};
        row = rows[p->as.dec - MO_CORE_DEC_OP_ABS];
        break;
    }
    case MO_CORE_PRIM_FLOAT: {
        if (p->as.float_ <= MO_CORE_FLOAT_OP_NEG) {
            *out = arithmetic((unsigned)p->as.float_, 2);
            return true;
        }
        static const enum mo_primitive_id rows[] = {
            MO_PRIM_FLOAT_ABS,         MO_PRIM_FLOAT_SQRT,       MO_PRIM_FLOAT_FLOOR,
            MO_PRIM_FLOAT_CEIL,        MO_PRIM_FLOAT_ROUND,      MO_PRIM_FLOAT_IS_NAN,
            MO_PRIM_FLOAT_IS_INFINITE, MO_PRIM_FLOAT_TO_DECIMAL, MO_PRIM_FLOAT_TO_TEXT};
        row = rows[p->as.float_ - MO_CORE_FLOAT_OP_ABS];
        break;
    }
    case MO_CORE_PRIM_NOT:
        *out = native(MO_OP_NOT, "fn(Bool) : Bool", "b");
        return true;
    case MO_CORE_PRIM_CMP:
        *out = comparison(p);
        return true;
    case MO_CORE_PRIM_CHAR: {
        static const enum mo_primitive_id rows[] = {MO_PRIM_CHAR_CODE,
                                                    MO_PRIM_CHAR_IS_ASCII_DIGIT,
                                                    MO_PRIM_CHAR_IS_ASCII_LETTER,
                                                    MO_PRIM_CHAR_IS_ASCII_WHITE_SPACE,
                                                    MO_PRIM_CHAR_ASCII_LOWERCASE,
                                                    MO_PRIM_CHAR_ASCII_UPPERCASE,
                                                    MO_PRIM_CHAR_TO_TEXT};
        row = rows[p->as.char_];
        break;
    }
    case MO_CORE_PRIM_TEXT: {
        static const enum mo_primitive_id rows[] = {MO_PRIM_TEXT_CONCAT,
                                                    MO_PRIM_TEXT_BYTE_COUNT,
                                                    MO_PRIM_TEXT_CHAR_COUNT,
                                                    MO_PRIM_TEXT_IS_EMPTY,
                                                    MO_PRIM_TEXT_CHARS,
                                                    MO_PRIM_TEXT_FROM_CHARS,
                                                    MO_PRIM_TEXT_LINES,
                                                    MO_PRIM_TEXT_WORDS,
                                                    MO_PRIM_TEXT_SPLIT,
                                                    MO_PRIM_TEXT_TRIM,
                                                    MO_PRIM_TEXT_TRIM_START,
                                                    MO_PRIM_TEXT_TRIM_END,
                                                    MO_PRIM_TEXT_STARTS_WITH,
                                                    MO_PRIM_TEXT_ENDS_WITH,
                                                    MO_PRIM_TEXT_CONTAINS,
                                                    MO_PRIM_TEXT_DROP_PREFIX,
                                                    MO_PRIM_TEXT_DROP_SUFFIX,
                                                    MO_PRIM_TEXT_REPLACE,
                                                    MO_PRIM_TEXT_REPEAT,
                                                    MO_PRIM_TEXT_PAD_LEFT,
                                                    MO_PRIM_TEXT_PAD_RIGHT,
                                                    MO_PRIM_TEXT_ASCII_LOWERCASE,
                                                    MO_PRIM_TEXT_ASCII_UPPERCASE,
                                                    MO_PRIM_TEXT_TO_INT,
                                                    MO_PRIM_TEXT_TO_DECIMAL,
                                                    MO_PRIM_TEXT_TO_FLOAT,
                                                    MO_PRIM_LIST_JOIN,
                                                    MO_PRIM_TEXT_BYTES};
        row = rows[p->as.text];
        break;
    }
    case MO_CORE_PRIM_BYTES: {
#define ROW(id, wire) MO_PRIM_BYTES_##id,
        static const enum mo_primitive_id rows[] = {MO_CORE_BYTES_OPS(ROW)};
#undef ROW
        row = rows[p->as.bytes];
        break;
    }
    case MO_CORE_PRIM_UNICODE16: {
#define ROW(id, wire) MO_PRIM_UNICODE16_##id,
        static const enum mo_primitive_id rows[] = {MO_CORE_UNICODE_OPS(ROW)};
#undef ROW
        row = rows[p->as.unicode16];
        break;
    }
    case MO_CORE_PRIM_RANGE:
        if (p->as.range == MO_CORE_RANGE_OP_MAKE) {
            *out = native(MO_OP_MAKE_RANGE, "fn(Int, Int) : Range", "bb");
            return true;
        }
        if (p->as.range == MO_CORE_RANGE_OP_START || p->as.range == MO_CORE_RANGE_OP_END) {
            *out =
                native(p->as.range == MO_CORE_RANGE_OP_START ? MO_OP_RANGE_START : MO_OP_RANGE_END,
                       "fn(Range) : Int", "b");
            return true;
        }
        if (p->as.range == MO_CORE_RANGE_OP_TO_TEXT) {
            *out = (struct mo_codegen_operation){.signature = "fn(Range) : Text",
                                                 .params = "b",
                                                 .expansion = MO_CODEGEN_EXPAND_RANGE};
            return true;
        }
        row = p->as.range == MO_CORE_RANGE_OP_TO_LIST ? MO_PRIM_RANGE_TO_LIST
                                                      : MO_PRIM_RANGE_CONTAINS;
        break;
    case MO_CORE_PRIM_TIME: {
        static const struct mo_codegen_operation ops[] = {
            {.opcode = MO_OP_DIFF_TIME, .signature = "fn(Time, Time) : Duration", .params = "bb"},
            {.opcode = MO_OP_ADD_TIME, .signature = "fn(Time, Duration) : Time", .params = "bb"},
            {.opcode = MO_OP_SUB_TIME, .signature = "fn(Time, Duration) : Time", .params = "bb"},
            {.opcode = MO_OP_INVALID},
            {.opcode = MO_OP_INVALID},
            {.opcode = MO_OP_INVALID},
            {.opcode = MO_OP_ADD_DURATION,
             .signature = "fn(Duration, Duration) : Duration",
             .params = "bb"},
            {.opcode = MO_OP_SUB_DURATION,
             .signature = "fn(Duration, Duration) : Duration",
             .params = "bb"},
            {.opcode = MO_OP_SCALE_DURATION,
             .signature = "fn(Duration, Int) : Duration",
             .params = "bb"},
            {.opcode = MO_OP_NEG_DURATION, .signature = "fn(Duration) : Duration", .params = "b"}};
        if ((unsigned)p->as.time < sizeof ops / sizeof *ops &&
            ops[p->as.time].opcode != MO_OP_INVALID) {
            *out = ops[p->as.time];
            return true;
        }
        switch (p->as.time) {
        case MO_CORE_TIME_OP_CHECKED_ADD:
            row = MO_PRIM_TIME_CHECKED_ADD;
            break;
        case MO_CORE_TIME_OP_TO_TEXT:
            row = MO_PRIM_TIME_TO_TEXT;
            break;
        case MO_CORE_TIME_OP_PARSE:
            row = MO_PRIM_TIME_PARSE;
            break;
        case MO_CORE_TIME_OP_IN_MILLISECONDS:
            row = MO_PRIM_DURATION_IN_MILLISECONDS;
            break;
        case MO_CORE_TIME_OP_IN_SECONDS:
            row = MO_PRIM_DURATION_IN_SECONDS;
            break;
        case MO_CORE_TIME_OP_DUR_TO_TEXT:
            row = MO_PRIM_DURATION_TO_TEXT;
            break;
        case MO_CORE_TIME_OP_SUB:
        case MO_CORE_TIME_OP_ADD:
        case MO_CORE_TIME_OP_SUB_DUR:
        case MO_CORE_TIME_OP_DUR_ADD:
        case MO_CORE_TIME_OP_DUR_SUB:
        case MO_CORE_TIME_OP_DUR_MUL:
        case MO_CORE_TIME_OP_DUR_NEG:
            MO_UNREACHABLE();
        }
        break;
    }
    case MO_CORE_PRIM_LIST: {
        static const enum mo_primitive_id rows[] = {
            MO_PRIM_LIST_APPEND,    MO_PRIM_LIST_SIZE,      MO_PRIM_LIST_IS_EMPTY,
            MO_PRIM_LIST_GET,       MO_PRIM_LIST_FIRST,     MO_PRIM_LIST_LAST,
            MO_PRIM_LIST_APPEND,    MO_PRIM_LIST_PREPEND,   MO_PRIM_LIST_CONCAT,
            MO_PRIM_LIST_REVERSE,   MO_PRIM_LIST_TAKE,      MO_PRIM_LIST_DROP,
            MO_PRIM_LIST_SET,       MO_PRIM_LIST_REMOVE_AT, MO_PRIM_LIST_EMPTY,
            MO_PRIM_LIST_ENUMERATE, MO_PRIM_LIST_CONTAINS,  MO_PRIM_LIST_UNIQUE,
            MO_PRIM_LIST_ZIP};
        if (p->as.list.op.tag == MO_CORE_LIST_OP_UNCONS) {
            *out = (struct mo_codegen_operation){.signature = "fn(List(T)) : Option((T, List(T)))",
                                                 .params = "b",
                                                 .expansion = MO_CODEGEN_EXPAND_UNCONS};
            return true;
        }
        row = rows[p->as.list.op.tag];
        *out = primitives[row];
        if (p->as.list.op.tag == MO_CORE_LIST_OP_LIT)
            out->literal = MO_CODEGEN_LITERAL_LIST;
        return true;
    }
    case MO_CORE_PRIM_MAP: {
        static const enum mo_primitive_id rows[] = {MO_PRIM_MAP_LITERAL_INSERT,
                                                    MO_PRIM_MAP_EMPTY,
                                                    MO_PRIM_MAP_SIZE,
                                                    MO_PRIM_MAP_IS_EMPTY,
                                                    MO_PRIM_MAP_GET,
                                                    MO_PRIM_MAP_HAS,
                                                    MO_PRIM_MAP_SET,
                                                    MO_PRIM_MAP_REMOVE,
                                                    MO_PRIM_MAP_KEYS,
                                                    MO_PRIM_MAP_VALUES,
                                                    MO_PRIM_MAP_ENTRIES,
                                                    MO_PRIM_LIST_TO_MAP};
        row = rows[p->as.map.op.tag];
        *out = primitives[row];
        if (p->as.map.op.tag == MO_CORE_MAP_OP_LIT)
            out->literal = MO_CODEGEN_LITERAL_MAP;
        return true;
    }
    case MO_CORE_PRIM_SET: {
        static const enum mo_primitive_id rows[] = {
            MO_PRIM_SET_EMPTY,   MO_PRIM_SET_SIZE,         MO_PRIM_SET_IS_EMPTY,
            MO_PRIM_SET_HAS,     MO_PRIM_SET_ADD,          MO_PRIM_SET_REMOVE,
            MO_PRIM_SET_UNION,   MO_PRIM_SET_INTERSECTION, MO_PRIM_SET_DIFFERENCE,
            MO_PRIM_SET_TO_LIST, MO_PRIM_LIST_TO_SET};
        row = rows[p->as.set.op];
        break;
    }
    case MO_CORE_PRIM_EQ:
        *out = native(MO_OP_EQUAL, "fn(T, T) : Bool", "bb");
        return true;
    case MO_CORE_PRIM_STRUCTURAL:
        row = MO_PRIM_TEXT_STRUCTURAL;
        break;
    case MO_CORE_PRIM_IO: {
        static const enum mo_host_id rows[] = {MO_HOST_ARGS_LIST,
                                               MO_HOST_ENV_GET,
                                               MO_HOST_CONSOLE_PRINT,
                                               MO_HOST_CONSOLE_ERROR,
                                               MO_HOST_CONSOLE_READ_LINE,
                                               MO_HOST_CONSOLE_PRINTED,
                                               MO_HOST_CONSOLE_PRINTED_ERRORS,
                                               MO_HOST_FILES_WRITE_TEXT,
                                               MO_HOST_FILES_APPEND_TEXT,
                                               MO_HOST_FILES_REMOVE,
                                               MO_HOST_FILES_MAKE_FOLDER,
                                               MO_HOST_FILES_READ_ONLY,
                                               MO_HOST_CLOCK_NOW,
                                               MO_HOST_RANDOM_INT,
                                               MO_HOST_CONSOLE_FAKE,
                                               MO_HOST_CONSOLE_FAKE_INPUT,
                                               MO_HOST_FILES_IN_MEMORY,
                                               MO_HOST_CLOCK_FIXED,
                                               MO_HOST_RANDOM_SEEDED,
                                               MO_HOST_ARGS_OF,
                                               MO_HOST_ENV_OF};
        static const enum mo_host_id files[] = {MO_HOST_FILES_READ_TEXT, MO_HOST_FILES_READ_BYTES,
                                                MO_HOST_FILES_LIST,      MO_HOST_FILES_ENTRIES,
                                                MO_HOST_FILES_EXISTS,    MO_HOST_FILES_WITHIN};
        static const enum mo_host_id readonly[] = {
            MO_HOST_READFILES_READ_TEXT, MO_HOST_READFILES_READ_BYTES, MO_HOST_READFILES_LIST,
            MO_HOST_READFILES_ENTRIES,   MO_HOST_READFILES_EXISTS,     MO_HOST_READFILES_WITHIN};
        enum mo_host_id h =
            p->as.io.tag < MO_CORE_IO_OP_READ_TEXT
                ? rows[p->as.io.tag]
                : (p->as.io.ro ? readonly : files)[p->as.io.tag - MO_CORE_IO_OP_READ_TEXT];
        *out = hosts[h];
        return true;
    }
    }
    *out = mo_codegen_primitive(row);
    return true;
}
