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
 * interchange formats, which the reference doesn't state, so they cite no section: NULL. */
#ifndef MO_CODES_H
#define MO_CODES_H

#include <stddef.h>
#include <stdint.h>

#include "base/base.h"

/* X(number, identifier, section) */
#define MO_CODES(X)                                                                                \
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
    X(1026, CORE_CHARACTER, NULL)

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
