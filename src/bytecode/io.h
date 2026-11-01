/* Private bytecode I/O. Decoding is structural, not permission to execute. */
#ifndef MO_BYTECODE_IO_H
#define MO_BYTECODE_IO_H

#include "base/arena.h"
#include "bytecode/bytecode.h"
#include "heap/types.h"

#include <string.h>

_Static_assert(sizeof(struct mo_type) <= sizeof(struct mo_bytecode) &&
                   sizeof(struct mo_bytecode_function) <= sizeof(struct mo_bytecode) &&
                   sizeof(struct mo_bytecode_debug) <= sizeof(struct mo_bytecode) &&
                   sizeof(struct mo_bytecode_constant) <= sizeof(struct mo_bytecode),
               "the decoded file header covers native table-entry sizes");

/* Every native entry is charged to at least one wire byte. Allow one largest entry plus
 * alignment/header per byte, and double for chunk slack. This bounds working storage without
 * imposing another file limit. It is a ceiling, not an allocation or a reservation. */
static inline MO_MUST_USE size_t mo_bytecode_work_budget(size_t wire_size) {
    const size_t per_byte =
        2 * (sizeof(struct mo_bytecode) + MO_ARENA_CHUNK_HEADER + MO_ARENA_ALIGNMENT);
    MO_ASSERT(wire_size <= MO_LIMIT_BYTECODE_SIZE && wire_size < SIZE_MAX / per_byte);
    return (wire_size + 1) * per_byte;
}

static inline void mo_bytecode_header_bytes(uint8_t header[MO_BYTECODE_HEADER_SIZE]) {
    memset(header, 0, MO_BYTECODE_HEADER_SIZE);
    memcpy(header, MO_BYTECODE_MAGIC, 4);
    header[4] = (uint8_t)MO_BYTECODE_MAJOR;
    header[5] = (uint8_t)((unsigned)MO_BYTECODE_MAJOR >> 8);
    header[6] = (uint8_t)MO_BYTECODE_MINOR;
    header[7] = (uint8_t)((unsigned)MO_BYTECODE_MINOR >> 8);
    header[8] = (uint8_t)MO_BYTECODE_FLAGS;
    header[9] = (uint8_t)(MO_BYTECODE_FLAGS >> 8);
    header[10] = (uint8_t)(MO_BYTECODE_FLAGS >> 16);
    header[11] = (uint8_t)(MO_BYTECODE_FLAGS >> 24);
}

struct mo_bytecode_names;
/* Extra source-map context for relational errors whose public wire offset is 20.
 * COUNT means no site; NO_INDEX means the section count rather than an entry. */
struct mo_bytecode_error_site {
    enum mo_bytecode_section section;
    uint32_t entry;
};
struct mo_bytecode_debug_positions {
    uint32_t spans; /* count word; entries start four bytes later */
    uint32_t names;
    uint32_t metadata; /* text word */
    uint32_t lines;
};
struct mo_bytecode_read_info {
    struct mo_types types; /* read-only view for the shared case-shape rules */
    const struct mo_bytecode_names *names;
    const uint32_t *code; /* first code word, by function */
    const struct mo_bytecode_debug_positions *debug;
    struct mo_bytecode_error_site *site;
    size_t wire_size;
};

/* Copies all data into arena, retaining file indices. NULL on structural refusal or allocation
 * failure; error is required and has static reason text. Failure may use arena storage, which
 * the caller frees. No pointer into bytes survives. The verifier must run before publication. */
MO_MUST_USE struct mo_bytecode *mo_bytecode_decode(struct mo_arena *arena, const uint8_t *bytes,
                                                   size_t length, struct mo_load_error *error);

/* As decode, retaining first-code-word wire positions in the caller's arena for verification.
 * program_checksum, when non-NULL, receives FNV-1a of what the header checksum covers, less the
 * files section, so source paths do not reach it. */
MO_MUST_USE struct mo_bytecode *
mo_bytecode_decode_info(struct mo_arena *arena, const uint8_t *bytes, size_t length,
                        const uint32_t **code_offsets, struct mo_types *types_out,
                        uint64_t *program_checksum, struct mo_bytecode_error_site *site,
                        struct mo_load_error *error);

MO_MUST_USE struct mo_image *mo_bytecode_load_site(const uint8_t *bytes, size_t length,
                                                   struct mo_bytecode_error_site *site,
                                                   struct mo_load_error *error);

/* Structural-only writer for decoder tests and indexed I/O internals; never execute its output. */
MO_MUST_USE bool mo_bytecode_encode(const struct mo_bytecode *file, struct mo_buffer *out,
                                    struct mo_write_error *error);

/* For the decoder's complete, bounded tables only, not arbitrary caller-built structures.
 * Checks section 3, without ownership dataflow or execution. Relational refusals point to the
 * start of the payload; local wire/instruction/debug errors point to the offending field/item. */
MO_MUST_USE bool mo_bytecode_validate(const struct mo_bytecode *file,
                                      const struct mo_bytecode_read_info *info,
                                      struct mo_load_error *error);

/* The binary and text readers accept the same Unicode scalar encodings. */
MO_MUST_USE bool mo_bytecode_utf8(const uint8_t *bytes, uint32_t length);

#endif
