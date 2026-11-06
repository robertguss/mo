/* The actual MOEX header walk of the artifact reader. Hashing, UTF-8, the MOSV schema span and
 * the kind-2 body are modeled: each model checks the span it is given and returns an
 * unconstrained result, which the oracle then reads. Their bytes are proved or tested elsewhere.
 * An empty span may point anywhere (mo_binary_read gives the input's start), so only nonempty
 * spans are located. */
#include "base/binary.c"
#include "base/export.c"
#include <stdlib.h>

size_t nondet_size(void);
uint8_t nondet_byte(void);
uint64_t nondet_u64(void);
bool nondet_bool(void);
void reached_end(void) {
}
_Noreturn void mo_assert_failed(const char *expression, const char *file, int line) {
    (void)expression;
    (void)file;
    (void)line;
    __CPROVER_assert(0, "MO_ASSERT holds");
    abort();
}

static const uint8_t *source;
static size_t source_length;
/* What each model returned, for the oracle. */
static uint8_t hashed[32], body_schema[32], failure_build[32];
static bool utf8_valid, body_valid;
static uint64_t failure_id, failure_sequence, failure_program;

static void nondet_bytes(uint8_t out[32]) {
    for (unsigned i = 0; i < 32; i++)
        out[i] = nondet_byte();
}
void model_digest(const uint8_t *bytes, size_t length, uint8_t out[32]) {
    __CPROVER_assert(bytes == source && length == source_length - 32,
                     "the digest covers every byte before it");
    nondet_bytes(hashed);
    memcpy(out, hashed, 32);
}
bool model_mo_utf8_valid(const uint8_t *bytes, size_t length) {
    __CPROVER_assert((length == 0 || bytes == source + 66) &&
                         length <= MO_LIMIT_EXPORT_LABEL_BYTES && 66 + length <= source_length - 32,
                     "the label span follows its length and fits before the digest");
    utf8_valid = nondet_bool();
    return utf8_valid;
}
static void body_span(const uint8_t *body, size_t length) {
    __CPROVER_assert(length == 0 || (body >= source + MO_EXPORT_FIXED_BYTES - 32 &&
                                     body + length == source + source_length - 32),
                     "the body ends where the digest starts");
}
bool model_mo_export_schema(const uint8_t *saved, size_t length, uint8_t out[32]) {
    body_span(saved, length);
    body_valid = nondet_bool();
    nondet_bytes(body_schema);
    if (body_valid)
        memcpy(out, body_schema, 32);
    return body_valid;
}
bool model_failure_body(const uint8_t *body, size_t length, uint8_t schema[32],
                        struct mo_export_artifact *out) {
    body_span(body, length);
    body_valid = nondet_bool();
    nondet_bytes(body_schema);
    nondet_bytes(failure_build);
    failure_id = nondet_u64();
    failure_sequence = nondet_u64();
    failure_program = nondet_u64();
    if (body_valid) {
        memcpy(schema, body_schema, 32);
        out->state = (struct mo_failure_slice){body, 0};
        out->message = (struct mo_failure_slice){body, length};
        out->failure = (struct mo_failure_record){
            .id = failure_id, .sequence = failure_sequence, .program = failure_program};
        memcpy(out->failure.build, failure_build, 32);
    }
    return body_valid;
}

static uint64_t le(const uint8_t *bytes, unsigned width) {
    uint64_t value = 0;
    for (unsigned i = 0; i < width; i++)
        value |= (uint64_t)bytes[i] << (8 * i);
    return value;
}

static uint64_t created_bits(int64_t created) {
    uint64_t bits;
    memcpy(&bits, &created, sizeof bits);
    return bits;
}

void harness_mo_export_read(void) {
    uint8_t bytes[MO_CBMC_INPUT];
    for (size_t i = 0; i < MO_CBMC_INPUT; i++)
        bytes[i] = nondet_byte();
    size_t length = nondet_size();
    __CPROVER_assume(length <= sizeof bytes);
    source = bytes;
    source_length = length;
    struct mo_export_artifact out = {.identity.kind = 77, .state.length = 123};
    bool ok = mo_export_read(bytes, length, &out);

    /* The format's fixed offsets: a 66-byte prefix, the label, 64 bytes of fields and the
     * body length, the body, the 32-byte digest. */
    bool expected = length >= MO_EXPORT_FIXED_BYTES && memcmp(bytes, "MOEX\1", 5) == 0 &&
                    (bytes[5] == 1 || bytes[5] == 2) &&
                    memcmp(hashed, bytes + length - 32, 32) == 0;
    size_t label = 0, at = 0, body = 0;
    if (expected) {
        label = (size_t)le(bytes + 62, 4);
        expected = label <= MO_LIMIT_EXPORT_LABEL_BYTES && 66 + label + 64 <= length - 32;
    }
    if (expected) {
        at = 66 + label;
        body = (size_t)le(bytes + at + 60, 4);
        expected = utf8_valid && at + 64 + body == length - 32 && body_valid &&
                   memcmp(body_schema, bytes + at + 28, 32) == 0;
    }
    uint64_t program = 0, sequence = 0, service = 0;
    if (expected) {
        program = le(bytes + 22, 8);
        service = le(bytes + at, 4);
        sequence = le(bytes + at + 4, 8);
    }
    if (expected && bytes[5] == 2)
        expected = failure_program == (program ^ (service << 32)) && failure_sequence == sequence &&
                   le(bytes + at + 12, 8) == sequence && memcmp(failure_build, bytes + 30, 32) == 0;
    __CPROVER_assert(ok == expected, "acceptance is exactly the format's conditions");
    if (ok) {
        const struct mo_export_identity *id = &out.identity;
        __CPROVER_assert(id->kind == bytes[5] && memcmp(id->id, bytes + 6, 16) == 0 &&
                             id->program == program && memcmp(id->build, bytes + 30, 32) == 0 &&
                             id->label == NULL,
                         "identity prefix read from its offsets");
        __CPROVER_assert(id->service == service && id->sequence == sequence &&
                             id->encoded_sequence == le(bytes + at + 12, 8) &&
                             created_bits(id->created) == le(bytes + at + 20, 8),
                         "identity fields read from their offsets");
        __CPROVER_assert(out.label.length == label && (label == 0 || out.label.bytes == bytes + 66),
                         "label span");
        __CPROVER_assert(memcmp(out.schema, bytes + at + 28, 32) == 0, "header schema digest");
        if (bytes[5] == 1)
            __CPROVER_assert((body == 0 || out.state.bytes == bytes + at + 64) &&
                                 out.state.length == body && out.message.length == 0 &&
                                 out.failure.id == 0 && id->failure_id == 0,
                             "kind 1: the state is the whole body");
        else
            __CPROVER_assert(id->failure_id == failure_id && out.failure.id == failure_id,
                             "kind 2: the record's id");
    } else
        __CPROVER_assert(out.identity.kind == 77 && out.state.length == 123,
                         "refusal preserves output");
    reached_end();
}
