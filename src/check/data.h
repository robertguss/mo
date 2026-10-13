/* Built-in case signatures, private to check. Reference 8, 9.2 and 14. */
#ifndef MO_CHECK_DATA_H
#define MO_CHECK_DATA_H

#include "check/check.h"

struct mo_builtin_case {
    const char *name, *type;
    uintptr_t key; /* normalized coverage constructor, below every symbol address */
    uint32_t first_key, cases, parameters, count, parameter;
    const char *fields[2]; /* NULL for one unnamed type-parameter payload */
};

MO_MUST_USE const struct mo_builtin_case *mo_builtin_case_by_name(const struct mo_syntax *name);
MO_MUST_USE const struct mo_builtin_case *mo_builtin_case_by_key(uintptr_t key);

#endif
