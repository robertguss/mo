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

struct mo_image;
struct mo_load_error {
    uint32_t offset;    /* byte offset in the input */
    const char *reason; /* static text */
};

/* Loading always verifies. The image copies what it needs; bytes may be freed after this call.
 * NULL on refusal or allocation failure, with error_out filled. The caller owns the image. */
MO_MUST_USE struct mo_image *mo_load(const uint8_t *bytes, size_t length,
                                     struct mo_load_error *error_out);
void mo_image_free(struct mo_image *image);

#endif
