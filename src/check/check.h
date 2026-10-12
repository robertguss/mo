/* Loading and module interfaces, before body checking. These results are not a
 * checked program and cannot be lowered. Result arrays and names belong to
 * arena; interface declaration, owner and origin pointers borrow the loaded
 * syntax, which must outlive the interfaces. Syntax owns its token text
 * independently of sources. */
#ifndef MO_CHECK_H
#define MO_CHECK_H

#include "syntax/syntax.h"

struct mo_module_set {
    const char *root;
    uint32_t count;
    struct mo_module_syntax **modules; /* entry first, then discovery order */
    uint32_t *order;                   /* dependencies before their users; indices into modules */
    const char **names;                /* dotted module names */
    bool *library;
    uint32_t **uses; /* one target index per use line, in header order */
};

/* Loads the entry and its transitive uses. sources must be initialized and empty.
 * A NULL library means no library folder; a non-NULL folder must exist. Every
 * .mo library file is included, recursively, without entering linked folders.
 * Errors return NULL, retaining sources and diagnostics. An unreadable entry
 * has an unavailable primary file (UINT32_MAX).
 * The entry's spelling must end in its reference 2.1 module path. */
MO_MUST_USE struct mo_module_set *mo_load(struct mo_arena *arena, struct mo_sources *sources,
                                          const char *entry, const char *library,
                                          struct mo_diags *diags);

struct mo_symbol {
    const char *name;
    const struct mo_syntax *declaration;
    const struct mo_syntax *owner; /* choice declaration for a case, otherwise NULL */
    uint32_t module;
    bool exported;
    bool insides; /* type fields/cases are exposed */
};

struct mo_imported_name {
    const char *name;
    const struct mo_symbol *symbols; /* all overloads travel together */
    uint32_t count;
    const struct mo_syntax *origin;
};

struct mo_module_alias {
    const char *name;
    uint32_t module;
    const struct mo_syntax *origin;
};

struct mo_module_scope {
    uint32_t count;
    struct mo_symbol *symbols; /* sorted by name, then source position */
    uint32_t imported_count;
    struct mo_imported_name *imported; /* sorted by name */
    uint32_t alias_count;
    struct mo_module_alias *aliases; /* sorted by name; separate from type names */
};

struct mo_module_interfaces {
    uint32_t count;
    struct mo_module_scope *scopes; /* same indices as the loaded set */
};

/* Indexes declarations, validates overload ownership, exposes public interfaces
 * and resolves explicit imports and their choice cases. NULL on error. Does not
 * check body names, types, uses of hidden insides or unused imports; those need
 * body checking. set must be a successful mo_load result, or its equivalent:
 * complete syntax, valid target indices and an acyclic dependency order. */
MO_MUST_USE struct mo_module_interfaces *
mo_resolve_modules(struct mo_arena *arena, const struct mo_module_set *set, struct mo_diags *diags);

#endif
