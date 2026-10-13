/* The registry of error codes (architecture section 3.5). Every diagnostic has one of these codes,
 * shown as E and four digits, such as E0412.
 *
 * A code never changes its meaning (reference appendix A): once a code is here, its meaning is
 * fixed, and a code that is no longer used stays here, so that it is never reused for something
 * else. Add a code with the next free number of its range.
 *
 * Each row gives the code's number, a short identifier, and the section of the reference it cites,
 * as a string such as "8.3". Codes up to E0999 are the language's errors, and each cites the rule
 * it enforces. Codes from E1000 are implementation errors: limits (src/base/limits.h) and
 * interchange formats, which the reference doesn't state, so they cite no section: NULL.
 *
 * Codes up to E1060 were registered as each part arrived. New implementation codes take the
 * next free number in their part's block, so parts built at the same time never collide: the
 * front end (check, lower) E1100 to E1199, code generation E1200 to E1299, bytecode, heap and
 * VM E1300 to E1399, and the tools E1400 to E1499. */
#ifndef MO_CODES_H
#define MO_CODES_H

#include <stddef.h>
#include <stdint.h>

#include "base/base.h"

/* X(number, identifier, section) */
#define MO_CODES(X)                                                                                \
    /* Source text, tokens and syntax (the executable grammar checks these too). */                \
    X(1, SOURCE_UTF8, "2.1")                                                                       \
    X(2, SOURCE_INVISIBLE, "2.1")                                                                  \
    X(3, SOURCE_NAME, "2.3")                                                                       \
    X(4, SOURCE_NUMBER, "3.2")                                                                     \
    X(5, SOURCE_TEXT, "3.4")                                                                       \
    X(6, SOURCE_TOKEN, "2.4")                                                                      \
    X(7, SOURCE_SYNTAX, "2.4")                                                                     \
    X(8, SOURCE_HEADER, "10.1")                                                                    \
    X(9, SOURCE_PATTERN, "7.3")                                                                    \
    X(10, SOURCE_TARGET, "6.2")                                                                    \
    X(11, SOURCE_CONSTRUCTION, "4.2")                                                              \
    X(12, SOURCE_ARGUMENTS, "5.2")                                                                 \
    X(13, SOURCE_STRUCT, "4.1")                                                                    \
    X(14, SOURCE_ENUM, "4.2")                                                                      \
    X(15, SOURCE_PROMISE, "12.2")                                                                  \
    X(16, SOURCE_CASE, "7.2")                                                                      \
    X(17, SOURCE_FUNCTION, "5.1")                                                                  \
    X(18, SOURCE_EXAMPLES, "12.1")                                                                 \
    X(19, SOURCE_LITERAL, "3.6")                                                                   \
    X(20, SOURCE_TYPE, "9.1")                                                                      \
    X(21, SOURCE_CALL, "5.2")                                                                      \
    X(22, SOURCE_IF, "7.1")                                                                        \
    X(23, SOURCE_LOOP, "7.5")                                                                      \
    X(24, SOURCE_INDEX, "3.7")                                                                     \
    X(25, SOURCE_BOOLEAN, "3.1")                                                                   \
    X(26, SOURCE_ASSIGNMENT, "6.2")                                                                \
    X(27, SOURCE_QUALIFICATION, "10.1")                                                            \
    X(28, SOURCE_COMMENT, "2.2")                                                                   \
    X(29, SOURCE_OPERATOR, "3.2")                                                                  \
    X(30, SOURCE_TEST, "12.3")                                                                     \
    X(31, SOURCE_BINDING, "6.1")                                                                   \
    X(32, SOURCE_LIST, "3.7")                                                                      \
    X(33, SOURCE_MAP, "3.8")                                                                       \
    /* Program loading and module interfaces. */                                                   \
    X(34, MODULE_MISSING, "2.1")                                                                   \
    X(35, MODULE_NAME, "2.1")                                                                      \
    X(36, MODULE_CYCLE, "10.3")                                                                    \
    X(37, MODULE_RESERVED, "10.3")                                                                 \
    X(38, NAME_CLASH, "10.3")                                                                      \
    X(39, EXPOSE_NAME, "10.1")                                                                     \
    X(40, EXPOSE_INSIDES, "10.2")                                                                  \
    X(41, IMPORT_PRIVATE, "10.1")                                                                  \
    X(42, OVERLOAD, "5.1")                                                                         \
    X(43, CASE_CLASH, "4.2")                                                                       \
    X(44, IMPORT_CASE, "10.3")                                                                     \
    X(45, ALWAYS_IN_SCOPE, "10.3")                                                                 \
    X(46, IMPORT_MISSING, "10.3")                                                                  \
    X(47, IMPORT_HIDDEN_CASE, "10.2")                                                              \
    X(48, TYPE_NAME, "10.3")                                                                       \
    X(49, TYPE_ARGUMENTS, "9.1")                                                                   \
    X(50, NAME_UNKNOWN, "6.1")                                                                     \
    X(51, BIND_REPEATED, "6.1")                                                                    \
    X(52, UNUSED_NAME, "6.1")                                                                      \
    X(53, TYPE_MISMATCH, "3")                                                                      \
    X(54, CANNOT_INFER, "5.1")                                                                     \
    X(55, CALL_ARGUMENTS, "5.2")                                                                   \
    X(56, UNUSED_VALUE, "6.1")                                                                     \
    X(57, NOT_VARIABLE, "6.2")                                                                     \
    X(58, EXIT_OUTSIDE_LOOP, "7.5")                                                                \
    X(59, BOOL_NAME, "2.3")                                                                        \
    X(60, LITERAL_RANGE, "3.3")                                                                    \
    X(61, TUPLE_POSITION, "3.6")                                                                   \
    X(62, FIELD_REPEATED, "4")                                                                     \
    X(63, DATA_ARGUMENTS, "4")                                                                     \
    X(64, DATA_PRIVATE, "10.2")                                                                    \
    X(65, FIELD_MISSING, "4.1")                                                                    \
    X(66, DATA_PARAMETERS, "9.1")                                                                  \
    X(72, NOT_FUNCTION, "5.2")                                                                     \
    X(73, NOT_ITERABLE, "7.5")                                                                     \
    X(74, CONDITION_TYPE, "3.1")                                                                   \
    X(75, NUMBER_ABILITY, "9.2")                                                                   \
    X(76, BYTE_ARITHMETIC, "3.2")                                                                  \
    X(77, ARGUMENT_TYPE, "5.2")                                                                    \
    X(78, RESULT_TYPE, "5.1")                                                                      \
    /* A Result is dropped without being handled or passed upward. */                              \
    X(412, FAILURE_IGNORED, "8.3")                                                                 \
    /* A source file is longer than MO_LIMIT_FILE_SIZE bytes. */                                   \
    X(1001, FILE_TOO_BIG, NULL)                                                                    \
    /* The program's source files would be more than MO_LIMIT_PROGRAM_SIZE bytes. */               \
    X(1002, PROGRAM_TOO_BIG, NULL)                                                                 \
    /* The program would have more than MO_LIMIT_FILES source files. */                            \
    X(1003, TOO_MANY_FILES, NULL)                                                                  \
    /* A source file's path is longer than MO_LIMIT_PATH_SIZE bytes. */                            \
    X(1004, PATH_TOO_LONG, NULL)                                                                   \
    /* A line is longer than MO_LIMIT_LINE_LENGTH characters. */                                   \
    X(1005, LINE_TOO_LONG, NULL)                                                                   \
    /* Code nests deeper than MO_LIMIT_NESTING_DEPTH. */                                           \
    X(1006, NESTED_TOO_DEEPLY, NULL)                                                               \
    /* Interpolations nest deeper than MO_LIMIT_INTERPOLATION_DEPTH. */                            \
    X(1007, INTERPOLATIONS_TOO_DEEP, NULL)                                                         \
    /* A module declares more than MO_LIMIT_MODULE_NAMES names. */                                 \
    X(1008, TOO_MANY_NAMES, NULL)                                                                  \
    /* A function has more than MO_LIMIT_PARAMETERS parameters, or a call more arguments. */       \
    X(1009, TOO_MANY_PARAMETERS, NULL)                                                             \
    /* A record or case has more than MO_LIMIT_FIELDS fields, a tuple more elements, or an         \
     * anonymous function captures more values. */                                                 \
    X(1010, TOO_MANY_FIELDS, NULL)                                                                 \
    /* A choice type has more than MO_LIMIT_CHOICE_CASES cases. */                                 \
    X(1011, TOO_MANY_CASES, NULL)                                                                  \
    /* Instantiations of generic functions nest deeper than MO_LIMIT_INSTANTIATION_DEPTH. */       \
    X(1012, INSTANTIATIONS_TOO_DEEP, NULL)                                                         \
    /* A function needs more than MO_LIMIT_FUNCTION_REGISTERS registers. */                        \
    X(1013, TOO_MANY_REGISTERS, NULL)                                                              \
    /* A function compiles to more than MO_LIMIT_FUNCTION_INSTRUCTIONS instructions, or more than  \
     * MO_LIMIT_FUNCTION_CODE_WORDS words. */                                                      \
    X(1014, FUNCTION_TOO_LONG, NULL)                                                               \
    /* The compiled program passes a limit of the bytecode format: a section's count, or the       \
     * file's size (src/base/limits.h). */                                                         \
    X(1015, BYTECODE_TOO_BIG, NULL)                                                                \
    /* Core text exceeds its byte, node or scratch memory limit. */                                \
    X(1016, CORE_TEXT_LIMIT, NULL)                                                                 \
    /* The supplied Core arena, or the system, has no room. */                                     \
    X(1017, CORE_NO_MEMORY, NULL)                                                                  \
    /* Core text errors, specified by formal/TEXT.md rather than the language reference. */        \
    X(1018, CORE_UTF8, NULL)                                                                       \
    X(1019, CORE_SYNTAX, NULL)                                                                     \
    X(1020, CORE_ESCAPE, NULL)                                                                     \
    X(1021, CORE_VERSION, NULL)                                                                    \
    X(1022, CORE_CONSTRUCTOR, NULL)                                                                \
    X(1023, CORE_ARITY, NULL)                                                                      \
    X(1024, CORE_FIELD, NULL)                                                                      \
    X(1025, CORE_NUMBER, NULL)                                                                     \
    X(1026, CORE_CHARACTER, NULL)                                                                  \
    /* Invalid compiler-produced Core, not mistakes in Mo source. */                               \
    X(1027, CORE_CHECK_METADATA, NULL)                                                             \
    X(1028, CORE_CHECK_CONSTANT_NAME, NULL)                                                        \
    X(1029, CORE_CHECK_ENTRY, NULL)                                                                \
    X(1030, CORE_CHECK_ENTRY_KIND, NULL)                                                           \
    X(1031, CORE_CHECK_KIND, NULL)                                                                 \
    X(1032, CORE_CHECK_PREFIX, NULL)                                                               \
    X(1033, CORE_CHECK_LITERAL, NULL)                                                              \
    X(1034, CORE_CHECK_VARIABLE, NULL)                                                             \
    X(1035, CORE_CHECK_GLOBAL, NULL)                                                               \
    X(1036, CORE_CHECK_TYPE, NULL)                                                                 \
    X(1037, CORE_CHECK_ARGUMENTS, NULL)                                                            \
    X(1038, CORE_CHECK_LAMBDA, NULL)                                                               \
    X(1039, CORE_CHECK_APPLICATION, NULL)                                                          \
    X(1040, CORE_CHECK_FUNCTION, NULL)                                                             \
    X(1041, CORE_CHECK_CALL_PREFIX, NULL)                                                          \
    X(1042, CORE_CHECK_SPEC_CALL, NULL)                                                            \
    X(1043, CORE_CHECK_PROJECTION, NULL)                                                           \
    X(1044, CORE_CHECK_CONSTRUCTORS, NULL)                                                         \
    X(1045, CORE_CHECK_CONSTRUCTOR, NULL)                                                          \
    X(1046, CORE_CHECK_ARMS, NULL)                                                                 \
    X(1047, CORE_CHECK_ARM, NULL)                                                                  \
    X(1048, CORE_CHECK_SPEC_PRIMITIVE, NULL)                                                       \
    X(1049, CORE_CHECK_INFERENCE, NULL)                                                            \
    X(1050, CORE_CHECK_FAIL, NULL)                                                                 \
    X(1051, CORE_CHECK_JUMP, NULL)                                                                 \
    X(1052, CORE_CHECK_LIMIT, NULL)                                                                \
    X(1053, CORE_CHECK_MEMORY, NULL)                                                               \
    /* The supplied syntax arena, or the system, has no room. */                                   \
    X(1054, SYNTAX_NO_MEMORY, NULL)                                                                \
    /* A valid Core form not yet implemented by the staged bytecode compiler. */                   \
    X(1055, CODEGEN_UNSUPPORTED, NULL)                                                             \
    /* Code generation's scratch limit or system allocation failure. */                            \
    X(1056, CODEGEN_MEMORY, NULL)                                                                  \
    /* The bytecode writer/verifier refused compiler-produced data: a compiler bug. */             \
    X(1057, CODEGEN_INVALID, NULL)                                                                 \
    /* The supplied surface checking arena, or source storage, has no room. */                     \
    X(1058, CHECK_NO_MEMORY, NULL)                                                                 \
    /* Library discovery exceeds MO_LIMIT_LIBRARY_ENTRIES. */                                      \
    X(1059, LIBRARY_TOO_BIG, NULL)                                                                 \
    /* One module's imported names exceed MO_LIMIT_MODULE_IMPORTS. */                              \
    X(1060, TOO_MANY_IMPORTS, NULL)                                                                \
    /* All expanded imports exceed MO_LIMIT_PROGRAM_IMPORTS. */                                    \
    X(1100, TOO_MANY_PROGRAM_IMPORTS, NULL)                                                        \
    /* A dependency's resolved path escapes its module root. */                                    \
    X(1101, MODULE_OUTSIDE_ROOT, NULL)                                                             \
    /* The toolchain's library folder cannot be inspected. */                                      \
    X(1102, LIBRARY_UNREADABLE, NULL)                                                              \
    /* A surface form whose body rules are not implemented by the staged checker. */               \
    X(1103, CHECK_UNFINISHED, NULL)                                                                \
    /* Inference exceeds MO_LIMIT_SOURCE_CHECK_WORK for one function. */                           \
    X(1104, CHECK_WORK, NULL)                                                                      \
    /* An inferred surface type exceeds MO_LIMIT_SOURCE_TYPE_DEPTH. */                             \
    X(1105, CHECK_TYPE_DEPTH, NULL)                                                                \
    /* A source inference walk exceeds MO_LIMIT_SOURCE_INFERENCE_DEPTH. */                         \
    X(1106, CHECK_SYNTAX_DEPTH, NULL)                                                              \
    /* Inference across bodies exceeds MO_LIMIT_PROGRAM_CHECK_WORK. */                             \
    X(1107, CHECK_PROGRAM_WORK, NULL)                                                              \
    /* The ordinary entry's full runtime name is not unique in the image. */                       \
    X(1200, CODEGEN_ENTRY, NULL)                                                                   \
    /* Core uses a representation that source lowering never produces: a lowering bug. */          \
    X(1201, CODEGEN_REPRESENTATION, NULL)

/* Each code by its identifier: MO_CODE_FAILURE_IGNORED is 412, for E0412. */
enum mo_code {
#define MO_CODE_ENUMERATOR(number, identifier, section) MO_CODE_##identifier = number,
    MO_CODES(MO_CODE_ENUMERATOR)
#undef MO_CODE_ENUMERATOR
};

/* A code's row of the registry. */
struct mo_code_info {
    uint16_t number;
    const char *identifier; /* such as "FAILURE_IGNORED" */
    const char *section;    /* such as "8.3"; NULL for a limit */
};

/* The row of a code, or NULL if no code has that number. */
MO_MUST_USE const struct mo_code_info *mo_code_find(uint32_t number);

#endif
