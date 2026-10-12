/* The bytecode contract (docs/design/bytecode.md). Stream 3 implements loading and verification.
 * These are decoded structures, never structs to copy directly to or from a file. */
#ifndef MO_BYTECODE_H
#define MO_BYTECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/base.h"
#include "base/limits.h"

enum mo_operand_kind {
    MO_OPERAND_NONE,
    MO_OPERAND_REGISTER,
    MO_OPERAND_TYPE,
    MO_OPERAND_LAYOUT,
    MO_OPERAND_CONSTANT,
    MO_OPERAND_GLOBAL,
    MO_OPERAND_FUNCTION,
    MO_OPERAND_PRIMITIVE,
    MO_OPERAND_HOST,
    MO_OPERAND_TARGET,
    MO_OPERAND_FIELD,
    MO_OPERAND_CASE,
};

/* MOVE consumes only an owned source, and preserves borrowing otherwise. The three parameter
 * accesses resolve ownership from the callee, prims.def or hostops.def, respectively. Token
 * accesses use token(L), not the ordinary readable/owned register states of section 4.2. */
enum mo_operand_access {
    MO_ACCESS_NONE,
    MO_ACCESS_WRITE,
    MO_ACCESS_READ,
    MO_ACCESS_CONSUME,
    MO_ACCESS_MOVE,
    MO_ACCESS_CALL_PARAMETER,
    MO_ACCESS_PRIM_PARAMETER,
    MO_ACCESS_HOST_PARAMETER,
    MO_ACCESS_TOKEN_WRITE,
    MO_ACCESS_TOKEN_CONSUME,
};

struct mo_operand_info {
    enum mo_operand_kind kind;
    enum mo_operand_access access;
};

enum { MO_OPCODE_FIXED_OPERANDS = 4 };

enum mo_opcode {
    MO_OP_INVALID = 0,
#define OPCODE(id, number, name, count, limit, tail, a, b, c, d) MO_OP_##id = number,
#include "bytecode/opcodes.def"
};

enum {
    MO_OPCODE_COUNT = 0
#define OPCODE(id, number, name, count, limit, tail, a, b, c, d) +1
#include "bytecode/opcodes.def"
};

struct mo_opcode_info {
    uint16_t number;
    const char *name;
    uint8_t fixed_count;
    uint16_t variable_limit; /* zero for fixed instructions; high head bits must then be zero */
    struct mo_operand_info variable;
    struct mo_operand_info fixed[MO_OPCODE_FIXED_OPERANDS];
};

/* These short names are only used while expanding the operand table. */
#define MO_DESC_NONE                                                                               \
    { MO_OPERAND_NONE, MO_ACCESS_NONE }
#define MO_DESC_W                                                                                  \
    { MO_OPERAND_REGISTER, MO_ACCESS_WRITE }
#define MO_DESC_R                                                                                  \
    { MO_OPERAND_REGISTER, MO_ACCESS_READ }
#define MO_DESC_C                                                                                  \
    { MO_OPERAND_REGISTER, MO_ACCESS_CONSUME }
#define MO_DESC_M                                                                                  \
    { MO_OPERAND_REGISTER, MO_ACCESS_MOVE }
#define MO_DESC_CALL                                                                               \
    { MO_OPERAND_REGISTER, MO_ACCESS_CALL_PARAMETER }
#define MO_DESC_PRIM                                                                               \
    { MO_OPERAND_REGISTER, MO_ACCESS_PRIM_PARAMETER }
#define MO_DESC_HOST                                                                               \
    { MO_OPERAND_REGISTER, MO_ACCESS_HOST_PARAMETER }
#define MO_DESC_TW                                                                                 \
    { MO_OPERAND_REGISTER, MO_ACCESS_TOKEN_WRITE }
#define MO_DESC_TC                                                                                 \
    { MO_OPERAND_REGISTER, MO_ACCESS_TOKEN_CONSUME }
#define MO_DESC_TYPE                                                                               \
    { MO_OPERAND_TYPE, MO_ACCESS_NONE }
#define MO_DESC_LAYOUT                                                                             \
    { MO_OPERAND_LAYOUT, MO_ACCESS_NONE }
#define MO_DESC_CONSTANT                                                                           \
    { MO_OPERAND_CONSTANT, MO_ACCESS_NONE }
#define MO_DESC_GLOBAL                                                                             \
    { MO_OPERAND_GLOBAL, MO_ACCESS_NONE }
#define MO_DESC_FUNCTION                                                                           \
    { MO_OPERAND_FUNCTION, MO_ACCESS_NONE }
#define MO_DESC_PRIMITIVE                                                                          \
    { MO_OPERAND_PRIMITIVE, MO_ACCESS_NONE }
#define MO_DESC_HOSTOP                                                                             \
    { MO_OPERAND_HOST, MO_ACCESS_NONE }
#define MO_DESC_TARGET                                                                             \
    { MO_OPERAND_TARGET, MO_ACCESS_NONE }
#define MO_DESC_FIELD                                                                              \
    { MO_OPERAND_FIELD, MO_ACCESS_NONE }
#define MO_DESC_CASE                                                                               \
    { MO_OPERAND_CASE, MO_ACCESS_NONE }

static const struct mo_opcode_info mo_opcodes[MO_OPCODE_COUNT] = {
#define OPCODE(id, number, name, count, limit, tail, a, b, c, d)                                   \
    {number, name,           count,                                                                \
     limit,  MO_DESC_##tail, {MO_DESC_##a, MO_DESC_##b, MO_DESC_##c, MO_DESC_##d}},
#include "bytecode/opcodes.def"
};

#undef MO_DESC_NONE
#undef MO_DESC_W
#undef MO_DESC_R
#undef MO_DESC_C
#undef MO_DESC_M
#undef MO_DESC_CALL
#undef MO_DESC_PRIM
#undef MO_DESC_HOST
#undef MO_DESC_TW
#undef MO_DESC_TC
#undef MO_DESC_TYPE
#undef MO_DESC_LAYOUT
#undef MO_DESC_CONSTANT
#undef MO_DESC_GLOBAL
#undef MO_DESC_FUNCTION
#undef MO_DESC_PRIMITIVE
#undef MO_DESC_HOSTOP
#undef MO_DESC_TARGET
#undef MO_DESC_FIELD
#undef MO_DESC_CASE

/* A table lookup also accepts the untrusted low 16 bits of an instruction head. */
MO_MUST_USE static inline const struct mo_opcode_info *mo_opcode_find(uint32_t number) {
    if (number == 0 || number > MO_OPCODE_COUNT) {
        return NULL;
    }
    return &mo_opcodes[number - 1];
}

/* An instruction head is opcode | (variable_count << 16). Fixed operands are not counted;
 * fixed instructions require a zero high half. Targets count words from the function's start,
 * including head words. Verification checks boundaries, counts and the table's variable_limit. */
#define MO_OPCODE_MASK UINT32_C(0xffff)
enum { MO_OPCODE_VARIABLE_SHIFT = 16 };

/* p and h are zero-based row indices in the base tables. The spec gives symbolic ids but no
 * numbers; deriving these enums from table order keeps every consumer on the same numbering.
 * Phase 2 reads and writes with the same build, so the numbers need not survive table edits. */
enum mo_primitive_id {
#define PRIM(id, module, name, signature, params, bugs) MO_PRIM_##id,
#include "base/prims.def"
    MO_PRIM_COUNT,
};

enum mo_host_id {
#define HOSTOP(id, module, name, signature, params, bugs, spec_only) MO_HOST_##id,
#include "base/hostops.def"
    MO_HOST_COUNT,
};

/* Only FUNCTION permits borrowed parameters. INIT/TEST/EXAMPLE take no arguments and return
 * Unit. PROMISE takes the subject's arguments and result, and returns Int; ANCHOR takes no
 * arguments and returns Int. TEST/EXAMPLE/PROMISE/ANCHOR must be spec code; closures written
 * inside spec code are marked spec too. FUNCTION and INIT are not spec code. */
enum mo_function_kind {
    MO_FUNCTION_FUNCTION = 0,
    MO_FUNCTION_CLOSURE = 1,
    MO_FUNCTION_INIT = 2,
    MO_FUNCTION_TEST = 3,
    MO_FUNCTION_EXAMPLE = 4,
    MO_FUNCTION_PROMISE = 5,
    MO_FUNCTION_ANCHOR = 6,
};

enum mo_parameter_ownership { MO_PARAMETER_OWNED = 0, MO_PARAMETER_BORROWED = 1 };

struct mo_parameter {
    uint32_t type;
    enum mo_parameter_ownership ownership;
};

/* The header is exactly 20 bytes on disk: magic at 0, major at 4, minor at 6, flags at 8,
 * checksum at 12. No padding. Integers are little-endian throughout the file. */
#define MO_BYTECODE_MAGIC "MOBC"
enum { MO_BYTECODE_MAJOR = 0, MO_BYTECODE_MINOR = 1, MO_BYTECODE_HEADER_SIZE = 20 };
#define MO_BYTECODE_FLAGS UINT32_C(0)
#define MO_BYTECODE_FNV_OFFSET UINT64_C(14695981039346656037)
#define MO_BYTECODE_FNV_PRIME UINT64_C(1099511628211)

struct mo_bytecode_header {
    uint8_t magic[4];
    uint16_t major;
    uint16_t minor;
    uint32_t flags;
    uint64_t checksum; /* FNV-1a of every byte after the 20-byte header */
};

enum mo_bytecode_section {
    MO_SECTION_STRINGS = 0,
    MO_SECTION_TYPES = 1,
    MO_SECTION_LAYOUTS = 2,
    MO_SECTION_CONSTANTS = 3,
    MO_SECTION_GLOBALS = 4,
    MO_SECTION_INIT = 5,
    MO_SECTION_FUNCTIONS = 6,
    MO_SECTION_MODULES = 7,
    MO_SECTION_DEBUG = 8,
    MO_SECTION_COUNT = 9,
};

/* The format leaves entry framing open. Use u32 for every count, index, enum and boolean after
 * the header, except literal payloads and code words. Booleans are 0 or 1. There is no alignment
 * padding, trailing data or section directory; each section starts with its entry count.
 * Arrays below are encoded as count then entries; structs follow their declared field order,
 * except the explicitly decoded-only fields. Each count is checked against base/limits.h and
 * the bytes remaining before allocation. All indices start at zero.
 *
 * The types and layouts sections belong to heap/types.h. Its definitions supply their entry
 * encodings, not a second type or layout definition here. Forward declarations let this header
 * stand alone while that contract is built. Every type reference below is a u32 table index. */
struct mo_type;
struct mo_layout;

/* UINT32_MAX cannot be a valid index within any table limit. It marks optional strings and
 * debug text that is absent, rather than conflating absence with a valid empty string. */
#define MO_BYTECODE_NO_INDEX UINT32_MAX

/* On disk: u32 length then exactly length UTF-8 bytes, without a terminator. An embedded NUL is
 * allowed in text literals; use length, not strlen. The loader keeps its own copy of the bytes. */
struct mo_bytecode_string {
    uint32_t length; /* at most MO_LIMIT_STRING_SIZE */
    const uint8_t *bytes;
};

/* A constant's encoding is inferred from its type; this enum is decoded-only, not an extra tag
 * in the file. Range and choice literals are not in section 3.1: code builds them instead. */
enum mo_constant_encoding {
    MO_CONSTANT_IMMEDIATE64,
    MO_CONSTANT_DECIMAL128,
    MO_CONSTANT_TEXT,
    MO_CONSTANT_BYTES,
    MO_CONSTANT_BIGINT,
    MO_CONSTANT_CLOSURE,
};

/* The spec leaves the sign's encoding and the magnitude's byte order open. Use a u32 sign and
 * little-endian magnitude, like the other integers. Zero has ZERO and an empty magnitude;
 * nonzero values have POSITIVE/NEGATIVE and a nonzero final (most significant) byte. This gives
 * each integer exactly one encoding, and keeps sign independent of magnitude size. */
enum mo_bigint_sign { MO_BIGINT_ZERO = 0, MO_BIGINT_POSITIVE = 1, MO_BIGINT_NEGATIVE = 2 };

struct mo_bytecode_bytes {
    uint32_t length; /* at most MO_LIMIT_STRING_SIZE */
    const uint8_t *bytes;
};

struct mo_bytecode_bigint {
    enum mo_bigint_sign sign;
    struct mo_bytecode_bytes magnitude; /* u32 length then magnitude bytes */
};

/* On disk: type, then only the payload selected by that type:
 * - unit/bool/byte/char/int/float/time/duration: 8 bytes. Signed integers are two's complement;
 *   float is its IEEE binary64 bits. Unit is zero; bool/byte/char have zero upper bytes.
 * - decimal: 16 bytes, two's complement, rejecting -2^127; no host __int128 is required here.
 * - text: u32 string index; bytes: u32 length followed by raw bytes.
 * - bigint: sign, length, magnitude; closure: u32 function index.
 * Unused bytes of decoded immediate[] are zero. The loader validates scalar values, UTF-8,
 * time bounds and closure signatures before making reference constants immortal in the image.
 * These are literal descriptions, not runtime cells or heap pointers. */
struct mo_bytecode_constant {
    uint32_t type;
    enum mo_constant_encoding encoding; /* decoded-only */
    union {
        uint8_t immediate[16];
        uint32_t string;
        struct mo_bytecode_bytes bytes;
        struct mo_bytecode_bigint bigint;
        uint32_t function;
    } value;
};

struct mo_bytecode_global {
    uint32_t type;
    uint32_t name;   /* string index */
    uint32_t module; /* module index */
};

/* On disk in field order: name, module, kind, spec_code, parameter array (type, ownership),
 * result_type, capture type array, register type array, code word array. Only CLOSURE has
 * captures. The parameters array counts only explicit arguments: section 3.3 says the env is
 * one additional register, while 5.3 calls it a parameter. Use register parameter_count for
 * that implicit, owned env, and do not count it in a call's variable operands or function type.
 * register_types includes it, so a closure needs at least parameter_count + 1 registers.
 * Non-closures need at least parameter_count. Each parameter's type matches its register.
 *
 * capture_count <= MO_LIMIT_FIELDS; parameter_count <= MO_LIMIT_PARAMETERS;
 * register_count <= MO_LIMIT_FUNCTION_REGISTERS; code_count <= MO_LIMIT_FUNCTION_CODE_WORDS,
 * with at most MO_LIMIT_FUNCTION_INSTRUCTIONS decoded instructions. */
struct mo_bytecode_function {
    uint32_t name; /* full name, string index (section 3.5) */
    uint32_t module;
    enum mo_function_kind kind;
    bool spec_code;
    uint32_t parameter_count;
    const struct mo_parameter *parameters;
    uint32_t result_type;
    uint32_t capture_count;
    const uint32_t *capture_types;
    uint32_t register_count;
    const uint32_t *register_types;
    uint32_t code_count;
    const uint32_t *code;
};

/* The module table's exposed-type entry needs more than an index: architecture 10.1 generates
 * values differently for a type exposed without its insides. The bytecode spec leaves that
 * distinction unencoded. Keep it here as a u32 boolean, since exposure is a module property,
 * not a runtime layout property; the verifier and VM still see the complete type definition. */
struct mo_bytecode_type_export {
    uint32_t type;
    bool contents_exposed;
};

/* Exposed types and functions, and ALL module globals, in declaration order. On disk: name,
 * exposed type array, exposed function array, global array. Each array has at most
 * MO_LIMIT_MODULE_NAMES entries, and each index must refer to the appropriate table. */
struct mo_bytecode_module {
    uint32_t name; /* string index */
    uint32_t type_count;
    const struct mo_bytecode_type_export *types;
    uint32_t function_count;
    const uint32_t *functions;
    uint32_t global_count;
    const uint32_t *globals;
};

/* File is a string index, not a source-file index (the loader has no source files). Spans use
 * half-open byte ranges, like base/diag.h. Entries are in strictly increasing code-offset order,
 * at instruction boundaries; a span applies until the next entry or the code's end. */
struct mo_bytecode_span {
    uint32_t code_offset;
    uint32_t file;
    uint32_t start;
    uint32_t end;
};

/* A register's source name is live in [code_start, code_end), in word offsets. Both ends are
 * instruction boundaries, or code_end is the end of the function. Ranges for one register do
 * not overlap; no name implies no printable source variable, not an undefined register. */
struct mo_bytecode_register_name {
    uint32_t reg;
    uint32_t name; /* string index */
    uint32_t code_start;
    uint32_t code_end;
};

/* The debug section has one entry per function, in function-table order; it doesn't encode a
 * redundant function index. text is its test sentence/spec line, or NO_INDEX for ordinary
 * functions. For anchors, section 3.5's full name identifies allows versus forbids.
 * On disk: span array, register-name array, text. Each array and their total across the section
 * are bounded by MO_LIMIT_DEBUG_ENTRIES. Debug is mandatory, including for files without specs. */
struct mo_bytecode_debug {
    uint32_t span_count;
    const struct mo_bytecode_span *spans;
    uint32_t name_count;
    const struct mo_bytecode_register_name *names;
    uint32_t text;
};

/* The loader fills these tables after decoding, then verifies them before publishing an image.
 * Pointers and everything they reach belong to the image; none points into the input bytes.
 * Each section count has its MO_LIMIT_* bound in base/limits.h. init_count == global_count and
 * debug_count == function_count. init holds function indices in initialization order.
 *
 * This is the file's data, not the image's mutable runtime state. Stream 3 defines mo_image,
 * which also owns the immortal constants/globals, their heap and initialization state. */
struct mo_bytecode {
    struct mo_bytecode_header header;
    uint32_t string_count;
    const struct mo_bytecode_string *strings;
    uint32_t type_count;
    const struct mo_type *types;
    uint32_t layout_count;
    const struct mo_layout *layouts;
    uint32_t constant_count;
    const struct mo_bytecode_constant *constants;
    uint32_t global_count;
    const struct mo_bytecode_global *globals;
    uint32_t init_count;
    const uint32_t *init;
    uint32_t function_count;
    const struct mo_bytecode_function *functions;
    uint32_t module_count;
    const struct mo_bytecode_module *modules;
    uint32_t debug_count;
    const struct mo_bytecode_debug *debug;
};

struct mo_image;
struct mo_load_error {
    uint32_t offset;    /* byte offset in the input */
    const char *reason; /* static text */
};

/* Loading always verifies. The image copies what it needs; bytes may be freed after this call.
 * NULL on refusal or allocation failure, with error_out filled (offset zero for a failure not
 * tied to an input byte, such as no memory). error_out is required; its reason is static text.
 * Inputs larger than MO_LIMIT_BYTECODE_SIZE are refused before narrowing any offsets.
 * The caller owns the image; processes must be freed before mo_image_free. */
MO_MUST_USE struct mo_image *mo_load(const uint8_t *bytes, size_t length,
                                     struct mo_load_error *error_out);
void mo_image_free(struct mo_image *image);

#endif
