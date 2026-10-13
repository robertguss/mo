/* Whole-program initialization dependencies through direct calls. */
#ifndef MO_BYTECODE_VERIFY_PROGRAM_H
#define MO_BYTECODE_VERIFY_PROGRAM_H

#include "bytecode/io.h"

struct mo_verify_program {
    uint32_t *global_rank;
    uint32_t *init_rank;
    uint32_t *required; /* maximum global rank read through direct calls, by function */
};
MO_MUST_USE bool mo_verify_program_init(struct mo_arena *arena, const struct mo_bytecode *file,
                                        const uint32_t *code_offsets, struct mo_verify_program *out,
                                        struct mo_bytecode_error_site *site,
                                        struct mo_load_error *error);

#endif
