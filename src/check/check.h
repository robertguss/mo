/* Loading, module interfaces, signatures and staged local body inference.
 * These results are not a checked program and cannot be lowered. Arrays and names belong to
 * arena; interface declaration and origin pointers borrow the loaded
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
MO_MUST_USE struct mo_module_set *mo_load_program(struct mo_arena *arena,
                                                  struct mo_sources *sources, const char *entry,
                                                  const char *library, struct mo_diags *diags);

struct mo_symbol {
    const char *name;
    const struct mo_syntax *declaration;
    uint32_t owner;    /* owning choice's symbol index, otherwise UINT32_MAX */
    uint32_t receiver; /* first parameter's local nominal type, otherwise UINT32_MAX */
    uint32_t case_count;
    uint32_t overload_count; /* name-group size on its first member; one on later members */
    uint32_t name_length;
    const uint32_t *cases; /* choice's symbol indices in declaration order; shared by imports */
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

/* Looks up a declaration without copying or requiring a terminating NUL. The
 * first overload is returned; later overloads are adjacent in symbols. */
MO_MUST_USE const struct mo_symbol *mo_find_symbol(const struct mo_module_scope *scope,
                                                   const uint8_t *name, size_t length);

/* Indexes declarations, validates overload ownership, exposes public interfaces
 * and resolves explicit imports and their choice cases. NULL on error. Does not
 * check body names, types, uses of hidden insides or unused imports; those need
 * body checking. set must be a successful mo_load_program result, or its equivalent:
 * complete syntax, valid target indices and an acyclic dependency order. */
MO_MUST_USE struct mo_module_interfaces *
mo_resolve_modules(struct mo_arena *arena, const struct mo_module_set *set, struct mo_diags *diags);

enum mo_source_type_kind {
    MO_SOURCE_NAMED,
    MO_SOURCE_PARAMETER,
    MO_SOURCE_FUNCTION,
    MO_SOURCE_TUPLE,
};

/* Canonical annotation types: equal types have the same pointer within one
 * preparation. Named types use module UINT32_MAX for built-ins; type parameters
 * belong to their declaring function. Function arguments include the result in
 * the last slot. Declaration pointers borrow the module interfaces. */
struct mo_source_type {
    enum mo_source_type_kind kind;
    const char *name;
    uint32_t module;
    const struct mo_symbol *declaration;
    uint32_t count;
    const struct mo_source_type *const *arguments;
};

struct mo_function_signature {
    const struct mo_symbol *function;
    const struct mo_source_type *type;
};

struct mo_signatures {
    uint32_t count;
    struct mo_function_signature *items; /* module order, then interface symbol order */
};

/* Prepares all named function signatures, including abstract generic parameters
 * and default Unit results. NULL with diagnostics on failure. This does not
 * check function bodies, abilities or fields; it is not a checked program.
 * Result types belong to arena and borrow interfaces, which must outlive them. */
MO_MUST_USE struct mo_signatures *
mo_prepare_signatures(struct mo_arena *arena, const struct mo_module_set *set,
                      const struct mo_module_interfaces *interfaces, struct mo_diags *diags);

struct mo_inferred_expression {
    const struct mo_syntax *syntax;
    const struct mo_source_type *type;
    const struct mo_syntax *binding; /* local binder or parameter, otherwise NULL */
    const struct mo_symbol *symbol;  /* resolved module function, otherwise NULL */
    bool never;                      /* every path leaves the current function or loop, or stops */
};

struct mo_inferred_body {
    const struct mo_function_signature *function;
    uint32_t count;
    struct mo_inferred_expression *items; /* visited expressions/statements, in postorder */
};

/* Infers one named function's body against its prepared signature. Local names,
 * positional calls, lambdas, scalar/collection expressions, variables and
 * control flow are supported. E1103 explicitly refuses unfinished forms.
 * This is not mo_check: fields/constructors/patterns, dot calls, abilities,
 * capability/capture flow, specs, constants, unused imports and main's contract
 * still need their checking stages. A success is not suitable for lowering.
 * NULL with a diagnostic on failure, stopping at the first error in this body.
 * Output belongs to arena and borrows syntax, interfaces and signatures. Equal
 * inferred types are structurally equal; pointer identity is not required.
 * Types are shared DAGs: consumers must memoize walks, not expand shared paths.
 * E1104 bounds inference work per function; E1105 bounds inferred type paths
 * independently of source nesting. Both limits are in base/limits.h. All
 * temporary and result storage uses the caller arena, with E1058 on exhaustion.
 * Syntax and type walks use depth-bounded C recursion; allow several MiB of
 * stack under sanitizers. No scratch allocation bypasses the caller arena. */
MO_MUST_USE struct mo_inferred_body *
mo_infer_function(struct mo_arena *arena, const struct mo_module_interfaces *interfaces,
                  const struct mo_signatures *signatures, uint32_t function,
                  struct mo_diags *diags);

#endif
