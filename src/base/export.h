#ifndef MO_EXPORT_H
#define MO_EXPORT_H
#include "base/arena.h"
#include "base/failure.h"
#include "base/limits.h"
#include <stdint.h>

/* MOEX v1 framing shared by the producer and later artifact consumers. No live values. */
struct mo_export_identity {
    uint8_t kind, id[16], build[32];
    const char *label;
    uint32_t service;
    uint64_t program, sequence, encoded_sequence, failure_id;
    int64_t created;
};
struct mo_export_pin {
    struct mo_arena arena;
    uint8_t *bytes;
    size_t length;
    uint8_t id[16], digest[32];
    uint64_t sequence, encoded_sequence, touched, failure_id;
};
struct mo_export_page {
    uint8_t id[16], digest[32], bytes[MO_LIMIT_EXPORT_PAGE_BYTES];
    uint64_t sequence, encoded_sequence, checksum;
    size_t offset, length, total, count;
    bool final;
};
struct mo_export_consumer {
    uint8_t *bytes, id[16];
    uint64_t sequence, encoded_sequence, failure_id;
    uint8_t kind;
    size_t offset, total;
    bool complete;
};
/* Refusal ends this attempt; caller releases its confidential arena. */
MO_MUST_USE bool mo_export_accept(struct mo_arena *arena, struct mo_export_consumer *consumer,
                                  const struct mo_export_page *page);
/* Validates MOSV framing and hashes exactly its schema bytes, not its payload. */
MO_MUST_USE bool mo_export_schema(const uint8_t *saved, size_t length, uint8_t digest[32]);
/* Refusal preserves output arguments. Pages are fixed-size, aligned, nonempty. */
MO_MUST_USE bool mo_export_page_bounds(size_t total, size_t offset, size_t *length, size_t *count);
/* Caller supplies fresh OS randomness and the runtime identity. Replaces only on success.
 * The caller releases the old pin first when enforcing a one-pin-per-kind budget. */
MO_MUST_USE bool mo_export_create(struct mo_export_pin *pin,
                                  const struct mo_export_identity *identity, const uint8_t *saved,
                                  size_t length, uint64_t now);
MO_MUST_USE bool mo_export_create_failure(struct mo_export_pin *pin,
                                          const struct mo_export_identity *identity,
                                          const uint8_t *state, size_t state_length,
                                          const uint8_t *message, size_t message_length,
                                          uint64_t now);
/* One artifact read from outside input. Every span borrows the input. identity.label stays
 * NULL, since the label is not NUL-terminated; label holds it. schema is the header's state
 * schema digest. Kind 1: state is the MOSV body, message is empty and failure is zero.
 * Kind 2: state and message are the payloads of the body's two MOST frames, and failure is
 * the message parsed by mo_failure_read. */
struct mo_export_artifact {
    struct mo_export_identity identity;
    struct mo_failure_slice label, state, message;
    uint8_t schema[32];
    struct mo_failure_record failure;
};
/* Validates the format, kind, every frame, the schema identity and the trailing digest before
 * publishing anything. Refusal leaves out unchanged. */
MO_MUST_USE bool mo_export_read(const uint8_t *bytes, size_t length,
                                struct mo_export_artifact *out);
/* mo_export_read, keeping only the identity. Label is not borrowed. */
MO_MUST_USE bool mo_export_verify(const uint8_t *bytes, size_t length,
                                  struct mo_export_identity *identity);
void mo_export_release(struct mo_export_pin *pin);
MO_MUST_USE bool mo_export_expire(struct mo_export_pin *pin, uint64_t now);
#endif
