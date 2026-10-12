/* A sorted view for name lookup; sorting never changes the strings table. */
#ifndef MO_BYTECODE_NAMES_H
#define MO_BYTECODE_NAMES_H

#include "bytecode/io.h"

struct mo_bytecode_name {
    const struct mo_bytecode_string *string;
    uint32_t index;
};

struct mo_bytecode_names {
    struct mo_bytecode_name *items;
    uint32_t count;
};

/* Strings must already have bounded lengths and readable storage. */
MO_MUST_USE bool mo_bytecode_names_init(struct mo_bytecode_names *names, struct mo_arena *arena,
                                        const struct mo_bytecode *file);
/* Lowest matching file index, or NO_INDEX. Comparison uses bytes, including NUL. */
MO_MUST_USE uint32_t mo_bytecode_name_find(const struct mo_bytecode_names *names,
                                           const uint8_t *bytes, size_t length);

#endif
