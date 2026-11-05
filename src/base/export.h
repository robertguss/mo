#ifndef MO_EXPORT_H
#define MO_EXPORT_H
#include "base/arena.h"
#include "base/limits.h"
#include <stdint.h>

/* MOEX v1 framing shared by the producer and later artifact consumers. No live values. */
struct mo_export_identity {
    uint8_t kind, id[16], build[32];
    const char *label;
    uint32_t service;
    uint64_t program, sequence, encoded_sequence;
    int64_t created;
};
struct mo_export_pin {
    struct mo_arena arena;
    uint8_t *bytes;
    size_t length;
    uint8_t id[16], digest[32];
    uint64_t sequence, encoded_sequence, touched;
};
/* Validates MOSV framing and hashes exactly its schema bytes, not its payload. */
MO_MUST_USE bool mo_export_schema(const uint8_t *saved, size_t length, uint8_t digest[32]);
/* Refusal preserves output arguments. Pages are fixed-size, aligned, nonempty. */
MO_MUST_USE bool mo_export_page_bounds(size_t total, size_t offset, size_t *length, size_t *count);
/* Caller supplies fresh OS randomness and the runtime identity. Replaces only on success.
 * The caller releases the old pin first when enforcing a one-pin-per-kind budget. */
MO_MUST_USE bool mo_export_create(struct mo_export_pin *pin,
                                  const struct mo_export_identity *identity, const uint8_t *saved,
                                  size_t length, uint64_t now);
void mo_export_release(struct mo_export_pin *pin);
MO_MUST_USE bool mo_export_expire(struct mo_export_pin *pin, uint64_t now);
#endif
