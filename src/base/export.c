#include "base/export.h"
#include "base/binary.h"
#include "base/failure.h"
#include "base/sha256.h"
#include "base/utf8.h"
#include <string.h>

static void digest(const uint8_t *bytes, size_t length, uint8_t out[32]) {
    struct mo_sha256 hash;
    mo_sha256_init(&hash);
    mo_sha256_update(&hash, bytes, length);
    mo_sha256_finish(&hash, out);
}

bool mo_export_schema(const uint8_t *saved, size_t length, uint8_t out[32]) {
    if (length < 9 || length > MO_LIMIT_SAVED_BYTES || memcmp(saved, "MOSV\1", 5) != 0)
        return false;
    struct mo_binary input = {saved, length, 5};
    uint64_t size;
    const uint8_t *schema;
    if (!mo_binary_uint(&input, 4, &size) || size == 0 ||
        !mo_binary_read(&input, (size_t)size, &schema))
        return false;
    digest(schema, (size_t)size, out);
    return true;
}

bool mo_export_page_bounds(size_t total, size_t offset, size_t *length, size_t *count) {
    if (total == 0 || total > MO_LIMIT_EXPORT_BYTES / MO_LIMIT_EXPORT_PINS || offset >= total ||
        offset % MO_LIMIT_EXPORT_PAGE_BYTES != 0)
        return false;
    size_t remaining = total - offset;
    *length = remaining < MO_LIMIT_EXPORT_PAGE_BYTES ? remaining : MO_LIMIT_EXPORT_PAGE_BYTES;
    *count = 1 + (total - 1) / MO_LIMIT_EXPORT_PAGE_BYTES;
    return true;
}

static void integer(uint8_t *bytes, size_t *at, uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; i++) {
        bytes[(*at)++] = (uint8_t)value;
        value >>= 8;
    }
}

/* Failure bodies carry two length-prefixed MOST frames. Validate every byte
 * before using the state's schema; these artifacts are outside input. */
static bool failure_frame(struct mo_binary *in, uint8_t kind, const uint8_t **payload, size_t *size,
                          uint64_t *sequence, uint64_t *program) {
    uint64_t length, n, checksum;
    const uint8_t *bytes;
    if (!mo_binary_uint(in, 4, &length) || length < 34 || length > MO_LIMIT_SAVED_BYTES + 34 ||
        !mo_binary_read(in, (size_t)length, &bytes) || memcmp(bytes, "MOST\1", 5) != 0 ||
        bytes[5] != kind)
        return false;
    struct mo_binary frame = {bytes, (size_t)length, 6};
    if (!mo_binary_uint(&frame, 8, sequence) || !mo_binary_uint(&frame, 8, program) ||
        !mo_binary_uint(&frame, 4, &n) || !mo_binary_read(&frame, (size_t)n, payload) ||
        !mo_binary_uint(&frame, 8, &checksum) || frame.position != frame.length ||
        checksum != mo_binary_checksum(bytes, (size_t)length - 8))
        return false;
    *size = (size_t)n;
    return true;
}
static bool export_schema(uint8_t kind, const uint8_t *body, size_t length, uint8_t schema[32],
                          struct mo_failure_record *failure) {
    if (kind == 1)
        return mo_export_schema(body, length, schema);
    if (kind != 2)
        return false;
    struct mo_binary in = {body, length, 0};
    const uint8_t *state, *message;
    size_t state_size, message_size;
    uint64_t seq, seq2, program, program2;
    struct mo_failure_record record;
    bool ok = failure_frame(&in, 3, &state, &state_size, &seq, &program) &&
              failure_frame(&in, 4, &message, &message_size, &seq2, &program2) &&
              in.position == length && seq == seq2 && program == program2 &&
              mo_failure_read(message, message_size, &record) && record.sequence == seq &&
              record.program == program && mo_export_schema(state, state_size, schema) &&
              memcmp(schema, record.schema, 32) == 0;
    if (ok && failure != NULL)
        *failure = record;
    return ok;
}

static bool create(struct mo_export_pin *pin, const struct mo_export_identity *id,
                   const uint8_t *saved, size_t length, const uint8_t *message,
                   size_t message_length, uint64_t now) {
    uint8_t schema[32];
    size_t label = strlen(id->label);
    if ((id->kind != 1 && id->kind != 2) || label > MO_LIMIT_EXPORT_LABEL_BYTES ||
        !mo_utf8_valid((const uint8_t *)id->label, label) ||
        !(message == NULL ? export_schema(id->kind, saved, length, schema, NULL)
                          : mo_export_schema(saved, length, schema)))
        return false;
    if (message != NULL && (id->kind != 2 || message_length > MO_LIMIT_FAILURE_MESSAGE_BYTES))
        return false;
    size_t body_length = length + (message == NULL ? 0 : message_length + 76);
    size_t size = MO_EXPORT_FIXED_BYTES + label + body_length;
    struct mo_export_pin next = {.length = size,
                                 .sequence = id->sequence,
                                 .encoded_sequence = id->encoded_sequence,
                                 .touched = now};
    mo_arena_init_confidential(&next.arena, MO_LIMIT_EXPORT_BYTES / MO_LIMIT_EXPORT_PINS);
    next.bytes = mo_arena_allocate(&next.arena, size, 1);
    if (next.bytes == NULL)
        return false;
    uint8_t *bytes = next.bytes;
    size_t at = 0;
    memcpy(bytes, "MOEX\1", 5);
    at += 5;
    bytes[at++] = id->kind;
    memcpy(bytes + at, id->id, 16);
    at += 16;
    integer(bytes, &at, id->program, 8);
    memcpy(bytes + at, id->build, 32);
    at += 32;
    integer(bytes, &at, label, 4);
    memcpy(bytes + at, id->label, label);
    at += label;
    integer(bytes, &at, id->service, 4);
    integer(bytes, &at, id->sequence, 8);
    integer(bytes, &at, id->encoded_sequence, 8);
    integer(bytes, &at, (uint64_t)id->created, 8);
    memcpy(bytes + at, schema, 32);
    at += 32;
    integer(bytes, &at, body_length, 4);
    if (message == NULL) {
        memcpy(bytes + at, saved, length);
        at += length;
    } else {
        const uint8_t *parts[] = {saved, message};
        size_t sizes[] = {length, message_length};
        for (unsigned i = 0; i < 2; i++) {
            integer(bytes, &at, sizes[i] + 34, 4);
            size_t start = at;
            memcpy(bytes + at, "MOST\1", 5);
            at += 5;
            integer(bytes, &at, 3 + i, 1);
            integer(bytes, &at, id->sequence, 8);
            integer(bytes, &at, id->program ^ ((uint64_t)id->service << 32), 8);
            integer(bytes, &at, sizes[i], 4);
            memcpy(bytes + at, parts[i], sizes[i]);
            at += sizes[i];
            integer(bytes, &at, mo_binary_checksum(bytes + start, at - start), 8);
        }
    }
    MO_ASSERT(at + 32 == size);
    digest(bytes, at, next.digest);
    memcpy(bytes + at, next.digest, 32);
    memcpy(next.id, id->id, 16);
    mo_export_release(pin);
    *pin = next;
    return true;
}

bool mo_export_create(struct mo_export_pin *pin, const struct mo_export_identity *id,
                      const uint8_t *saved, size_t length, uint64_t now) {
    return create(pin, id, saved, length, NULL, 0, now);
}
bool mo_export_create_failure(struct mo_export_pin *pin, const struct mo_export_identity *id,
                              const uint8_t *state, size_t state_length, const uint8_t *message,
                              size_t message_length, uint64_t now) {
    struct mo_failure_record record;
    if (!mo_failure_read(message, message_length, &record) || record.sequence != id->sequence ||
        record.program != (id->program ^ ((uint64_t)id->service << 32)) ||
        memcmp(record.build, id->build, 32) != 0)
        return false;
    bool ok = create(pin, id, state, state_length, message, message_length, now);
    if (ok)
        pin->failure_id = record.id;
    return ok;
}

void mo_export_release(struct mo_export_pin *pin) {
    mo_arena_free(&pin->arena);
    *pin = (struct mo_export_pin){0};
}

bool mo_export_expire(struct mo_export_pin *pin, uint64_t now) {
    if (pin->bytes == NULL || now < pin->touched || now - pin->touched < MO_LIMIT_EXPORT_IDLE_US)
        return false;
    mo_export_release(pin);
    return true;
}

bool mo_export_verify(const uint8_t *bytes, size_t length, struct mo_export_identity *out) {
    if (length < MO_EXPORT_FIXED_BYTES || length > MO_LIMIT_EXPORT_BYTES / MO_LIMIT_EXPORT_PINS ||
        (memcmp(bytes, "MOEX\1", 5) != 0 || (bytes[5] != 1 && bytes[5] != 2)))
        return false;
    uint8_t actual[32];
    digest(bytes, length - 32, actual);
    if (memcmp(actual, bytes + length - 32, 32) != 0)
        return false;
    struct mo_binary in = {bytes, length - 32, 6};
    struct mo_export_identity id = {.kind = bytes[5]};
    struct mo_failure_record failure = {0};
    const uint8_t *value, *schema, *saved;
    uint64_t label, service, created, size;
    if (!mo_binary_read(&in, 16, &value))
        return false;
    memcpy(id.id, value, 16);
    if (!mo_binary_uint(&in, 8, &id.program) || !mo_binary_read(&in, 32, &value))
        return false;
    memcpy(id.build, value, 32);
    if (!mo_binary_uint(&in, 4, &label) || label > MO_LIMIT_EXPORT_LABEL_BYTES ||
        !mo_binary_read(&in, (size_t)label, &value) || !mo_utf8_valid(value, (size_t)label) ||
        !mo_binary_uint(&in, 4, &service) || !mo_binary_uint(&in, 8, &id.sequence) ||
        !mo_binary_uint(&in, 8, &id.encoded_sequence) || !mo_binary_uint(&in, 8, &created) ||
        !mo_binary_read(&in, 32, &schema) || !mo_binary_uint(&in, 4, &size) ||
        !mo_binary_read(&in, (size_t)size, &saved) || in.position != in.length ||
        !export_schema(id.kind, saved, (size_t)size, actual, &failure) ||
        memcmp(actual, schema, 32) != 0)
        return false;
    id.service = (uint32_t)service;
    if (id.kind == 2) {
        if (failure.program != (id.program ^ ((uint64_t)id.service << 32)) ||
            failure.sequence != id.sequence || id.encoded_sequence != id.sequence ||
            memcmp(failure.build, id.build, 32) != 0)
            return false;
        id.failure_id = failure.id;
    }
    /* created is a signed wire timestamp; preserve its bit representation. */
    memcpy(&id.created, &created, sizeof created);
    *out = id;
    return true;
}

static bool export_page_matches(const struct mo_export_consumer *consumer,
                                const struct mo_export_page *page) {
    size_t length, count;
    if (consumer->complete || page->total < MO_EXPORT_FIXED_BYTES ||
        !mo_export_page_bounds(page->total, page->offset, &length, &count) ||
        page->length != length || page->count != count ||
        page->final != (length == page->total - page->offset))
        return false;
    if (consumer->bytes == NULL)
        return page->offset == 0;
    return page->offset == consumer->offset && page->total == consumer->total &&
           page->sequence == consumer->sequence &&
           page->encoded_sequence == consumer->encoded_sequence &&
           memcmp(page->id, consumer->id, 16) == 0;
}

bool mo_export_accept(struct mo_arena *arena, struct mo_export_consumer *consumer,
                      const struct mo_export_page *page) {
    if (!export_page_matches(consumer, page) ||
        mo_binary_checksum(page->bytes, page->length) != page->checksum)
        return false;
    if (consumer->bytes == NULL) {
        uint8_t *bytes = mo_arena_allocate(arena, page->total, 1);
        if (bytes == NULL)
            return false;
        *consumer = (struct mo_export_consumer){.bytes = bytes,
                                                .sequence = page->sequence,
                                                .encoded_sequence = page->encoded_sequence,
                                                .total = page->total};
        memcpy(consumer->id, page->id, 16);
    }
    memcpy(consumer->bytes + consumer->offset, page->bytes, page->length);
    consumer->offset += page->length;
    if (page->final) {
        struct mo_export_identity identity;
        if (memcmp(page->digest, consumer->bytes + consumer->total - 32, 32) != 0 ||
            !mo_export_verify(consumer->bytes, consumer->total, &identity) ||
            memcmp(identity.id, consumer->id, 16) != 0 || identity.sequence != consumer->sequence ||
            identity.encoded_sequence != consumer->encoded_sequence)
            return false;
        consumer->kind = identity.kind;
        consumer->failure_id = identity.failure_id;
        consumer->complete = true;
    }
    return true;
}
