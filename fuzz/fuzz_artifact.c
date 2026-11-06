#include "base/export.h"
#include "base/failure.h"
#include "base/sha256.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static uint64_t le(const uint8_t *bytes, unsigned width) {
    uint64_t value = 0;
    for (unsigned i = 0; i < width; i++)
        value |= (uint64_t)bytes[i] << (8 * i);
    return value;
}

static bool same_walk(const struct mo_failure_arguments *a, const struct mo_failure_arguments *b) {
    return a->bytes.bytes == b->bytes.bytes && a->bytes.length == b->bytes.length &&
           a->position == b->position && a->remaining == b->remaining;
}

static bool same_identity(const struct mo_export_identity *a, const struct mo_export_identity *b) {
    return a->kind == b->kind && memcmp(a->id, b->id, 16) == 0 &&
           memcmp(a->build, b->build, 32) == 0 && a->label == b->label &&
           a->service == b->service && a->program == b->program && a->sequence == b->sequence &&
           a->encoded_sequence == b->encoded_sequence && a->failure_id == b->failure_id &&
           a->created == b->created;
}

static bool inside(struct mo_failure_slice span, const uint8_t *data, size_t size) {
    return span.bytes >= data && span.length <= size && span.bytes - data <= (ptrdiff_t)size &&
           (size_t)(span.bytes - data) + span.length <= size;
}

/* A walk over arbitrary bytes with an arbitrary count: it ends, stays inside its bytes, never
 * yields more than the count, and reports the end only when count and bytes run out together. */
static void walk(const uint8_t *data, size_t size, uint32_t count) {
    struct mo_failure_record record = {.arguments = {data, size}, .argument_count = count};
    struct mo_failure_arguments arguments = mo_failure_arguments(&record);
    struct mo_failure_slice out = {NULL, 0};
    uint32_t yielded = 0;
    for (;;) {
        struct mo_failure_arguments before = arguments;
        struct mo_failure_slice kept = out;
        enum mo_failure_argument next = mo_failure_argument_next(&arguments, &out);
        if (next != MO_FAILURE_ARGUMENT) {
            MO_ASSERT(same_walk(&before, &arguments));
            MO_ASSERT(kept.bytes == out.bytes && kept.length == out.length);
            MO_ASSERT((next == MO_FAILURE_ARGUMENTS_END) ==
                      (yielded == count && arguments.position == size));
            break;
        }
        yielded++;
        MO_ASSERT(yielded <= count && inside(out, data, size));
        MO_ASSERT(out.length >= 9 && memcmp(out.bytes, "MOSV\1", 5) == 0);
        MO_ASSERT(out.bytes == data + before.position + 4 &&
                  le(data + before.position, 4) == out.length);
        MO_ASSERT(arguments.position == before.position + 4 + out.length);
    }
}

/* Every field checked against the format's fixed offsets, not against the reader's own walk. */
static void accepted(const uint8_t *data, size_t size, const struct mo_export_artifact *a) {
    MO_ASSERT(size >= MO_EXPORT_FIXED_BYTES &&
              size <= MO_LIMIT_EXPORT_BYTES / MO_LIMIT_EXPORT_PINS);
    MO_ASSERT(memcmp(data, "MOEX\1", 5) == 0 && data[5] == a->identity.kind);
    uint8_t digest[32];
    struct mo_sha256 hash;
    mo_sha256_init(&hash);
    mo_sha256_update(&hash, data, size - 32);
    mo_sha256_finish(&hash, digest);
    MO_ASSERT(memcmp(digest, data + size - 32, 32) == 0);
    const struct mo_export_identity *id = &a->identity;
    MO_ASSERT(memcmp(id->id, data + 6, 16) == 0 && id->program == le(data + 22, 8));
    MO_ASSERT(memcmp(id->build, data + 30, 32) == 0 && id->label == NULL);
    size_t label = (size_t)le(data + 62, 4);
    /* An empty span may point anywhere; mo_binary_read gives the input's start. */
    MO_ASSERT(label <= MO_LIMIT_EXPORT_LABEL_BYTES && a->label.length == label &&
              (label == 0 || a->label.bytes == data + 66));
    const uint8_t *at = data + 66 + label;
    int64_t created;
    uint64_t created_bits = le(at + 20, 8);
    memcpy(&created, &created_bits, sizeof created);
    MO_ASSERT(id->service == le(at, 4) && id->sequence == le(at + 4, 8) &&
              id->encoded_sequence == le(at + 12, 8) && id->created == created);
    MO_ASSERT(memcmp(a->schema, at + 28, 32) == 0);
    struct mo_failure_slice body = {at + 64, (size_t)le(at + 60, 4)};
    MO_ASSERT(body.bytes + body.length == data + size - 32);
    uint8_t schema[32];
    MO_ASSERT(mo_export_schema(a->state.bytes, a->state.length, schema));
    MO_ASSERT(memcmp(schema, a->schema, 32) == 0);
    struct mo_export_identity verified;
    MO_ASSERT(mo_export_verify(data, size, &verified));
    MO_ASSERT(same_identity(&verified, id));
    if (id->kind == 1) {
        MO_ASSERT(a->state.bytes == body.bytes && a->state.length == body.length);
        MO_ASSERT(a->message.length == 0 && a->failure.id == 0 && id->failure_id == 0);
        return;
    }
    MO_ASSERT(id->kind == 2 && inside(a->state, body.bytes, body.length) &&
              inside(a->message, body.bytes, body.length));
    /* Each payload follows its frame's 4-byte length and 26-byte MOST header. */
    MO_ASSERT(a->state.bytes == body.bytes + 30 && le(body.bytes + 26, 4) == a->state.length);
    MO_ASSERT(a->message.bytes == a->state.bytes + a->state.length + 8 + 30);
    struct mo_failure_record record;
    MO_ASSERT(mo_failure_read(a->message.bytes, a->message.length, &record));
    MO_ASSERT(record.id == a->failure.id && record.argument_count == a->failure.argument_count &&
              record.arguments.bytes == a->failure.arguments.bytes &&
              record.arguments.length == a->failure.arguments.length);
    MO_ASSERT(id->failure_id == record.id && id->encoded_sequence == id->sequence &&
              record.sequence == id->sequence && memcmp(record.build, id->build, 32) == 0 &&
              memcmp(record.schema, a->schema, 32) == 0);
    struct mo_failure_arguments arguments = mo_failure_arguments(&record);
    struct mo_failure_slice argument;
    uint32_t count = 0;
    enum mo_failure_argument next;
    while ((next = mo_failure_argument_next(&arguments, &argument)) == MO_FAILURE_ARGUMENT) {
        MO_ASSERT(inside(argument, a->message.bytes, a->message.length));
        count++;
    }
    MO_ASSERT(next == MO_FAILURE_ARGUMENTS_END && count == record.argument_count);
}

static void read_artifact(const uint8_t *data, size_t size) {
    struct mo_export_artifact a, untouched;
    memset(&a, 0xa5, sizeof a);
    memcpy(&untouched, &a, sizeof a);
    bool ok = mo_export_read(data, size, &a);
    struct mo_export_identity identity;
    MO_ASSERT(mo_export_verify(data, size, &identity) == ok);
    if (!ok) {
        MO_ASSERT(memcmp(&a, &untouched, sizeof a) == 0);
        return;
    }
    accepted(data, size, &a);
    struct mo_export_artifact truncated;
    MO_ASSERT(!mo_export_read(data, size - 1, &truncated));
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    read_artifact(data, size);
    walk(data, size, size == 0 ? 0 : data[0] % 4);
    if (size < MO_EXPORT_FIXED_BYTES || size > MO_LIMIT_EXPORT_BYTES / MO_LIMIT_EXPORT_PINS)
        return 0;
    /* Sealed again, so a mutation reaches the checks behind the trailing digest. */
    uint8_t *sealed = malloc(size);
    MO_ASSERT(sealed != NULL);
    memcpy(sealed, data, size - 32);
    struct mo_sha256 hash;
    mo_sha256_init(&hash);
    mo_sha256_update(&hash, sealed, size - 32);
    mo_sha256_finish(&hash, sealed + size - 32);
    read_artifact(sealed, size);
    free(sealed);
    return 0;
}
