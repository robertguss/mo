/* Ordered byte keys: deterministic logarithmic lookup, including hostile names. */
#ifndef MO_CODEGEN_INDEX_H
#define MO_CODEGEN_INDEX_H

#include "base/arena.h"
#include "core/core.h"

struct mo_codegen_index {
    mo_core_string key;
    uint32_t value;
    unsigned height;
    struct mo_codegen_index *left, *right;
};

MO_MUST_USE struct mo_codegen_index *mo_codegen_find(struct mo_codegen_index *root,
                                                     mo_core_string key);
/* Borrow keys until arena_free. Insert only if absent; retain the first value otherwise.
 * Callers may change the returned entry's value for a lexical push/pop. */
MO_MUST_USE struct mo_codegen_index *mo_codegen_insert(struct mo_arena *arena,
                                                       struct mo_codegen_index **root,
                                                       mo_core_string key, uint32_t value);

#endif
