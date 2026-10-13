/* Typed pattern coverage, private to check. NULL denotes a wildcard. */
#ifndef MO_CHECK_PATTERNS_H
#define MO_CHECK_PATTERNS_H

#include "check/check.h"

struct mo_match_pattern {
    uintptr_t key;
    uint32_t signature; /* UINT32_MAX for scalar literal domains */
    uint32_t count;
    const char *name;
    const struct mo_symbol *choice;
    const struct mo_data_signature *data;
    struct mo_match_pattern **items;
};

/* Maranget's exhaustiveness matrix. Guarded rows are excluded by the caller.
 * Charges matrix steps and allocated bytes to the body and program work budgets and all
 * storage to its arena. An explicit frame stack also handles wide tuples without C
 * recursion. False means a diagnostic; true with *missing == NULL means total. */
MO_MUST_USE bool mo_patterns_cover(struct mo_arena *arena,
                                   const struct mo_module_interfaces *interfaces,
                                   struct mo_match_pattern *const *patterns, uint32_t count,
                                   const struct mo_source_type *type, uint32_t *work,
                                   uint64_t *program_work, struct mo_diags *diags,
                                   struct mo_span span, const char **missing);

#endif
