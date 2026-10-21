/* Private verification of structurally decoded tables. */
#ifndef MO_BYTECODE_VERIFY_H
#define MO_BYTECODE_VERIFY_H

#include "bytecode/io.h"

struct layout_key {
    uint32_t type, tag, index;
};
MO_MUST_USE uint32_t mo_verify_layout_lookup(const struct layout_key *keys, uint32_t count,
                                             uint32_t type, uint32_t tag);

struct mo_verify_function {
    const struct mo_bytecode *file;
    const struct mo_bytecode_function *fn;
    const uint8_t *abilities;
    uint32_t base;
    struct mo_load_error *error;
    const struct mo_types *types;
    const struct layout_key *layouts;
};
/* All local instruction types, even in unreachable code. */
MO_MUST_USE bool mo_verify_instruction(struct mo_verify_function *v, uint32_t pc);
MO_MUST_USE const char *mo_verify_parameter_modes(const uint32_t *instruction);
/* Whether function type type is closure function's signature: the same parameter and result
 * types. Both indices must be inside their tables. The validator checks closure layouts and
 * literals with it, and the verifier make_closure. */
MO_MUST_USE bool mo_verify_closure_signature(const struct mo_bytecode *file, uint32_t type,
                                             uint32_t function);

struct mo_verification {
    const uint8_t *abilities;
    const struct layout_key *layouts;
};
/* Metadata survives in the image arena; whole-program scratch does not. */
MO_MUST_USE bool mo_bytecode_verify(const struct mo_bytecode *file, const struct mo_types *types,
                                    const uint32_t *code_offsets, size_t wire_size,
                                    struct mo_arena *arena, struct mo_verification *out,
                                    struct mo_bytecode_error_site *site,
                                    struct mo_load_error *error);

/* Replay only this function, reporting states before pc. pc must be an instruction boundary.
 * readable has register_count elements. work counts analysis steps for the bounded-work test. */
MO_MUST_USE bool mo_bytecode_readable(const struct mo_bytecode *file, const struct mo_types *types,
                                      const struct mo_verification *verified, uint32_t function,
                                      uint32_t base, uint32_t pc, bool *readable, uint64_t *work,
                                      struct mo_load_error *error);

/* Streaming replay for exhaustive checks without code-count separate analyses. The mask
 * changes only in slots the instruction/DFS undo log changes. Visitors run once per boundary,
 * in graph traversal order, and must not retain or change the mask. False stops the visit. */
typedef bool (*mo_readable_visitor)(uint32_t pc, const bool *readable, void *context);
MO_MUST_USE bool mo_bytecode_visit_readable(const struct mo_bytecode *file,
                                            const struct mo_types *types,
                                            const struct mo_verification *verified,
                                            uint32_t function, uint32_t base, bool *readable,
                                            mo_readable_visitor visit, void *context,
                                            struct mo_load_error *error);
MO_MUST_USE bool mo_image_visit_readable(const struct mo_image *image, uint32_t function,
                                         bool *readable, mo_readable_visitor visit, void *context,
                                         struct mo_load_error *error);

#endif
