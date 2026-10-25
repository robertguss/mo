/* Private spellings shared by the bytecode text reader and printer. */
#ifndef MO_BYTECODE_TEXT_H
#define MO_BYTECODE_TEXT_H

#include "base/capabilities.h"
#include "bytecode/io.h"

extern const char *const mo_bytecode_type_names[MO_TYPE_CAPABILITY + 1];
extern const char *const mo_bytecode_function_names[MO_FUNCTION_ANCHOR + 1];
extern const char *const mo_bytecode_expectation_names[MO_BYTECODE_FORBIDS + 1];
extern const char *const mo_bytecode_operand_prefixes[MO_OPERAND_INDEX + 1];

#endif
