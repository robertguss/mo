/* Loading and module interfaces, before body checking. These results are not a
 * checked program and cannot be lowered. All returned storage belongs to arena;
 * syntax owns its token text independently of sources. */
#ifndef MO_CHECK_H
#define MO_CHECK_H

#include "syntax/syntax.h"

struct mo_module_set {
    const char *root;
    uint32_t count;
    struct mo_module_syntax **modules; /* entry first, then discovery order */
    uint32_t *order;                   /* dependencies before their users; indices into modules */
};

/* Loads the entry and its transitive uses. sources must be initialized and empty.
 * A NULL library means no library folder; a non-NULL folder must exist. Library
 * discovery is added in the next pass. Errors return NULL, retaining sources and
 * diagnostics. An unreadable entry has an unavailable primary file (UINT32_MAX).
 * The entry's spelling must end in its reference 2.1 module path. */
MO_MUST_USE struct mo_module_set *mo_load(struct mo_arena *arena, struct mo_sources *sources,
                                          const char *entry, const char *library,
                                          struct mo_diags *diags);

#endif
