#include "base/export.h"
#include "base/binary.h"
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

bool mo_export_create(struct mo_export_pin *pin, const struct mo_export_identity *id,
                      const uint8_t *saved, size_t length, uint64_t now) {
    uint8_t schema[32];
    size_t label = strlen(id->label);
    if (id->kind != 1 || label > MO_LIMIT_EXPORT_LABEL_BYTES ||
        !mo_utf8_valid((const uint8_t *)id->label, label) ||
        !mo_export_schema(saved, length, schema))
        return false;
    size_t size = MO_EXPORT_FIXED_BYTES + label + length;
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
    integer(bytes, &at, length, 4);
    memcpy(bytes + at, saved, length);
    at += length;
    MO_ASSERT(at + 32 == size);
    digest(bytes, at, next.digest);
    memcpy(bytes + at, next.digest, 32);
    memcpy(next.id, id->id, 16);
    mo_export_release(pin);
    *pin = next;
    return true;
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
