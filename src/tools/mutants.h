#ifndef MO_TOOLS_MUTANTS_H
#define MO_TOOLS_MUTANTS_H

#include "check/check.h"

struct mo_mutant {
    uint32_t function;
    uint32_t module;
    struct mo_span span;
    struct mo_position position;
    const char *kind;
    const char *original;
    const char *replacement;
};

struct mo_mutants {
    uint32_t count;
    struct mo_mutant *items;
};

MO_MUST_USE bool mo_module_tested(const struct mo_program *program, uint32_t module);

MO_MUST_USE const char *mo_list_mutants(struct mo_arena *arena, const struct mo_program *program,
                                        const struct mo_sources *sources, struct mo_mutants *out);

MO_MUST_USE bool mo_mutant_sources(const struct mo_program *program,
                                   const struct mo_sources *sources, const struct mo_mutant *mutant,
                                   struct mo_sources *out);

#endif
