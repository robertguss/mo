#include "base/export.h"
#include "base/failure.h"
#include "base/runtime-identity.h"
#include "base/sha256.h"
#include "service_compile.h"
#include "test.h"
#include "tools/tools.h"

#include <string.h>

/* A whole program from its entry file, with the toolchain's lib/ folder, as mo test compiles one.
 */
static struct mo_image *compile_path(const char *path) {
    struct mo_arena arena, lowered;
    mo_arena_init(&arena, MO_CLI_CHECK_SIZE);
    mo_arena_init(&lowered, MO_LIMIT_LOWER_WORK);
    struct mo_sources sources;
    struct mo_diags diags;
    MO_ASSERT(mo_sources_init(&sources) && mo_diags_init(&diags));
    struct mo_module_set *set = mo_load_program(&arena, &sources, path, "lib", &diags);
    MO_ASSERT(set != NULL && !mo_diags_any(&diags));
    struct mo_program *program = mo_check(&arena, set, &diags);
    MO_ASSERT(program != NULL && !mo_diags_any(&diags));
    struct mo_services services;
    struct mo_core *core = mo_lower_services(&lowered, program, &sources, &diags, NULL, &services);
    MO_ASSERT(core != NULL && !mo_diags_any(&diags));
    struct mo_buffer wire;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    MO_ASSERT(mo_compile_services(core, &services, &wire, &diags));
    struct mo_load_error error;
    struct mo_image *image = mo_load(wire.bytes, wire.length, &error);
    MO_ASSERT(image != NULL);
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    MO_ASSERT(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    mo_buffer_free(&wire);
    mo_arena_free(&lowered);
    mo_arena_free(&arena);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
    return image;
}

/* 2026-01-01T00:00:00Z. */
static struct mo_service_time clock_now(void *context) {
    (void)context;
    return (struct mo_service_time){.now = INT64_C(1767225600000000), .elapsed = 5};
}
static void ignore_reply(const struct mo_service_response *r, void *context) {
    (void)r;
    (void)context;
}
static void ignore_event(const struct mo_service_event *e, void *context) {
    (void)e;
    (void)context;
}

struct running {
    struct mo_image *image;
    struct mo_store *store;
    struct mo_service_loop *service;
    struct mo_heap *heap;
    uint32_t state_type;
};
static void start(struct running *r, struct mo_image *image) {
    r->image = image;
    MO_ASSERT(mo_store_open(NULL, &r->store) == MO_STORE_OK);
    struct mo_service_hooks hooks = {clock_now, ignore_reply, ignore_event, NULL};
    MO_ASSERT(mo_service_open(image, 0, &hooks, r->store, &r->service) == MO_SERVICE_OK);
    r->heap = mo_service_heap(r->service);
    r->state_type = mo_image_bytecode(image)->services[0].state_type;
}
static void stop(struct running *r) {
    mo_service_free(r->service);
    mo_store_close(r->store);
    mo_image_free(r->image);
}

static void seal(uint8_t *bytes, size_t length) {
    struct mo_sha256 hash;
    mo_sha256_init(&hash);
    mo_sha256_update(&hash, bytes, length - 32);
    mo_sha256_finish(&hash, bytes + length - 32);
}

static bool same_identity(const struct mo_export_identity *a, const struct mo_export_identity *b) {
    return a->kind == b->kind && memcmp(a->id, b->id, 16) == 0 &&
           memcmp(a->build, b->build, 32) == 0 && a->label == b->label &&
           a->service == b->service && a->program == b->program && a->sequence == b->sequence &&
           a->encoded_sequence == b->encoded_sequence && a->failure_id == b->failure_id &&
           a->created == b->created;
}

static bool same_walk(const struct mo_failure_arguments *a, const struct mo_failure_arguments *b) {
    return a->bytes.bytes == b->bytes.bytes && a->bytes.length == b->bytes.length &&
           a->position == b->position && a->remaining == b->remaining;
}

static bool same_bytes(struct mo_failure_slice a, const uint8_t *bytes, size_t length) {
    return a.length == length && (length == 0 || memcmp(a.bytes, bytes, length) == 0);
}

/* Every strict prefix and every single flipped bit is refused, and refusal leaves out alone. */
static void refuses_damage(uint8_t *bytes, size_t length) {
    struct mo_export_artifact out, untouched;
    memset(&out, 0xa5, sizeof out);
    memcpy(&untouched, &out, sizeof out);
    for (size_t n = 0; n < length; n++)
        CHECK(!mo_export_read(bytes, n, &out));
    for (size_t i = 0; i < length; i++) {
        bytes[i] ^= 1;
        CHECK(!mo_export_read(bytes, length, &out));
        bytes[i] ^= 1;
    }
    CHECK(memcmp(&out, &untouched, sizeof out) == 0);
}

/* A byte changed and the artifact resealed, so its own check, not the digest, refuses it. */
static void refuses_resealed(uint8_t *bytes, size_t length, size_t at) {
    struct mo_export_artifact out;
    MO_ASSERT(at < length - 32);
    bytes[at] ^= 0x40;
    seal(bytes, length);
    CHECK(!mo_export_read(bytes, length, &out));
    bytes[at] ^= 0x40;
    seal(bytes, length);
    CHECK(mo_export_read(bytes, length, &out));
}

/* One byte more between the body and the digest, sealed again: the body must end the artifact. */
static void refuses_trailing(const uint8_t *bytes, size_t length) {
    struct mo_buffer longer;
    mo_buffer_init(&longer, length + 1);
    MO_ASSERT(mo_buffer_append(&longer, bytes, length - 32) && mo_buffer_append_byte(&longer, 0) &&
              mo_buffer_append(&longer, bytes + length - 32, 32));
    seal(longer.bytes, longer.length);
    struct mo_export_artifact out;
    CHECK(!mo_export_read(longer.bytes, longer.length, &out));
    mo_buffer_free(&longer);
}

static bool has_text(struct mo_cell value, const char *expected) {
    size_t length = 0;
    const uint8_t *bytes = mo_value_data(value, &length);
    return bytes != NULL && length == strlen(expected) && memcmp(bytes, expected, length) == 0;
}

struct stored {
    struct mo_buffer state, message;
    unsigned count;
};
static bool keep_record(const struct mo_store_failure *f, void *context) {
    struct stored *s = context;
    s->count++;
    return mo_buffer_append(&s->state, f->state.bytes, f->state.length) &&
           mo_buffer_append(&s->message, f->message.bytes, f->message.length);
}

/* A handler that stops on its two arguments: the record holds the pre-call state and both. */
static void artifacts(void) {
    mo_runtime_identity_init();
    struct running r;
    start(&r, compile("module Main\nstruct Counter\ncount: Int\nname: Text\nend\n"
                      "service\nstate Counter = Counter(count: 7, name: \"seven\")\n"
                      "tools broken\ntoken from env \"TOKEN\"\nend\n"
                      "fn broken(_state: Counter, _now: Time, _n: Int, _label: Text) : Bool\n"
                      "stop \"broken\"\nend\n"));
    const struct mo_bytecode_service *descriptor = &mo_image_bytecode(r.image)->services[0];
    MO_ASSERT(descriptor->tools[0].argument_count == 2);
    uint32_t int_type = descriptor->tools[0].argument_types[0];
    uint32_t text_type = descriptor->tools[0].argument_types[1];
    struct mo_cell arguments[2] = {mo_cell_int(5), mo_cell_int(0)};
    MO_ASSERT(mo_value_text(r.heap, "five", 4, &arguments[1]));
    CHECK(mo_service_send(r.service, 0, arguments, 2, 1) == MO_SERVICE_OK);
    mo_value_drop(r.heap, text_type, arguments[1]);
    CHECK(mo_service_step(r.service));
    struct stored stored = {.count = 0};
    mo_buffer_init(&stored.state, MO_LIMIT_SAVED_BYTES);
    mo_buffer_init(&stored.message, MO_LIMIT_FAILURE_MESSAGE_BYTES);
    CHECK(mo_service_failures(r.service, keep_record, &stored) == MO_STORE_OK);
    CHECK(stored.count == 1);
    uint8_t type_digest[32];
    CHECK(mo_type_schema_digest(r.heap, r.state_type, type_digest));

    /* Kind 2, pinned exactly as mo-export pins a failure. */
    struct mo_export_identity identity = {.kind = 2, .label = "test", .id = {9}, .created = 77};
    CHECK(mo_runtime_identity(identity.build));
    struct mo_export_pin pin = {0};
    CHECK(mo_service_pin_failure(r.service, 1, &pin, &identity, 0));
    MO_ASSERT(pin.bytes != NULL);
    struct mo_export_identity verified;
    struct mo_export_artifact a;
    CHECK(mo_export_verify(pin.bytes, pin.length, &verified));
    CHECK(mo_export_read(pin.bytes, pin.length, &a));
    CHECK(same_identity(&a.identity, &verified) && a.identity.kind == 2);
    CHECK(a.identity.failure_id == 1 && a.identity.label == NULL);
    /* A failure pin takes its label from the record, which holds the recording build's. */
    const char *build_label = mo_runtime_build_label();
    CHECK(same_bytes(a.label, (const uint8_t *)build_label, strlen(build_label)));
    CHECK(memcmp(a.schema, type_digest, 32) == 0);
    CHECK(same_bytes(a.state, stored.state.bytes, stored.state.length));
    CHECK(same_bytes(a.message, stored.message.bytes, stored.message.length));
    CHECK(a.failure.id == 1 && a.failure.argument_count == 2);
    CHECK(memcmp(a.failure.schema, type_digest, 32) == 0);
    CHECK(a.failure.name.length == 6 && memcmp(a.failure.name.bytes, "broken", 6) == 0);
    struct mo_cell state;
    CHECK(mo_value_decode(r.heap, r.state_type, a.state.bytes, a.state.length, &state));
    bool equal = false;
    CHECK(mo_value_equal(r.heap, r.state_type, state, mo_service_state(r.service), &equal) &&
          equal);
    mo_value_drop(r.heap, r.state_type, state);

    /* The arguments, in order, then the end. */
    struct mo_failure_arguments walk = mo_failure_arguments(&a.failure);
    struct mo_failure_slice argument = {0};
    struct mo_cell value = mo_cell_int(-1);
    CHECK(mo_failure_argument_next(&walk, &argument) == MO_FAILURE_ARGUMENT);
    CHECK(mo_value_decode(r.heap, int_type, argument.bytes, argument.length, &value));
    CHECK(mo_cell_get_int(value) == 5);
    CHECK(mo_failure_argument_next(&walk, &argument) == MO_FAILURE_ARGUMENT);
    CHECK(mo_value_decode(r.heap, text_type, argument.bytes, argument.length, &value));
    CHECK(has_text(value, "five"));
    mo_value_drop(r.heap, text_type, value);
    struct mo_failure_arguments ended = walk;
    struct mo_failure_slice kept = argument;
    CHECK(mo_failure_argument_next(&walk, &argument) == MO_FAILURE_ARGUMENTS_END);
    CHECK(same_walk(&walk, &ended) && argument.bytes == kept.bytes &&
          argument.length == kept.length);

    /* A count that disagrees with the bytes, either way, and bytes cut short. */
    struct mo_failure_record fewer = a.failure, more = a.failure, short_bytes = a.failure;
    fewer.argument_count = 1;
    more.argument_count = 3;
    short_bytes.arguments.length--;
    const struct mo_failure_record *wrong[] = {&fewer, &more, &short_bytes};
    const unsigned yielded[] = {1, 2, 1};
    for (unsigned i = 0; i < 3; i++) {
        walk = mo_failure_arguments(wrong[i]);
        for (unsigned n = 0; n < yielded[i]; n++)
            CHECK(mo_failure_argument_next(&walk, &argument) == MO_FAILURE_ARGUMENT);
        struct mo_failure_arguments before = walk;
        kept = argument;
        CHECK(mo_failure_argument_next(&walk, &argument) == MO_FAILURE_ARGUMENTS_DAMAGED);
        CHECK(same_walk(&walk, &before));
        CHECK(argument.bytes == kept.bytes && argument.length == kept.length);
    }

    /* Damage: each frame's payload and checksum is checked even under a valid outer digest. */
    size_t state_at = (size_t)(a.state.bytes - pin.bytes);
    size_t message_at = (size_t)(a.message.bytes - pin.bytes);
    refuses_resealed(pin.bytes, pin.length, state_at);
    refuses_resealed(pin.bytes, pin.length, state_at + a.state.length);
    refuses_resealed(pin.bytes, pin.length, message_at + a.message.length / 2);
    refuses_resealed(pin.bytes, pin.length, message_at + a.message.length + 7);
    refuses_trailing(pin.bytes, pin.length);
    refuses_damage(pin.bytes, pin.length);
    mo_export_release(&pin);

    /* Kind 1 over the same service's encoded state, as mo-export pins a state. */
    const uint8_t *saved = NULL;
    size_t saved_length = 0;
    CHECK(mo_service_encoded_state(r.service, &saved, &saved_length));
    identity.kind = 1;
    identity.sequence = 4;
    identity.encoded_sequence = 3;
    identity.service = 0;
    identity.program = mo_image_key(r.image, 0);
    CHECK(mo_export_create(&pin, &identity, saved, saved_length, 0));
    MO_ASSERT(pin.bytes != NULL);
    CHECK(mo_export_verify(pin.bytes, pin.length, &verified));
    CHECK(mo_export_read(pin.bytes, pin.length, &a));
    CHECK(same_identity(&a.identity, &verified) && a.identity.kind == 1);
    CHECK(a.identity.sequence == 4 && a.identity.encoded_sequence == 3);
    CHECK(same_bytes(a.label, (const uint8_t *)"test", 4));
    CHECK(same_bytes(a.state, saved, saved_length) && a.message.length == 0);
    CHECK(memcmp(a.schema, type_digest, 32) == 0);
    CHECK(a.failure.id == 0 && a.failure.argument_count == 0 && a.failure.arguments.bytes == NULL &&
          a.failure.name.bytes == NULL && a.identity.failure_id == 0);
    CHECK(mo_value_decode(r.heap, r.state_type, a.state.bytes, a.state.length, &state));
    CHECK(mo_value_equal(r.heap, r.state_type, state, mo_service_state(r.service), &equal) &&
          equal);
    mo_value_drop(r.heap, r.state_type, state);
    /* The schema length's first byte: the header's schema digest no longer matches. */
    refuses_resealed(pin.bytes, pin.length, (size_t)(a.state.bytes - pin.bytes) + 9);
    refuses_trailing(pin.bytes, pin.length);
    refuses_damage(pin.bytes, pin.length);
    mo_export_release(&pin);

    mo_buffer_free(&stored.state);
    mo_buffer_free(&stored.message);
    stop(&r);
}

/* The state type's digest without a value, and the digest of its encoded state. */
static void state_digests(struct mo_image *image, uint8_t type_digest[32]) {
    struct running r;
    start(&r, image);
    const uint8_t *saved = NULL;
    size_t length = 0;
    uint8_t encoded[32];
    CHECK(mo_service_encoded_state(r.service, &saved, &length));
    CHECK(mo_export_schema(saved, length, encoded));
    struct mo_buffer schema;
    mo_buffer_init(&schema, MO_LIMIT_SAVED_BYTES);
    CHECK(mo_buffer_append(&schema, "x", 1));
    CHECK(mo_type_schema(r.heap, r.state_type, &schema));
    size_t written = schema.length - 1;
    CHECK(length > 9 + written && saved[5] == (uint8_t)written);
    CHECK(memcmp(schema.bytes + 1, saved + 9, written) == 0);
    mo_buffer_free(&schema);
    memset(type_digest, 0, 32);
    CHECK(mo_type_schema_digest(r.heap, r.state_type, type_digest));
    CHECK(memcmp(type_digest, encoded, 32) == 0);
    stop(&r);
}

static const char *const variants[] = {
    /* The original. */
    "module Main\nstruct State\ncount: Int\nmode: Mode\nend\nenum Mode\nOn\nOff\nend\n"
    "service\nstate State = State(count: 1, mode: On)\ntools read\n"
    "token from env \"TOKEN\"\nend\nfn read(state: State, _now: Time) : Int\nstate.count\nend\n",
    /* A renamed field. */
    "module Main\nstruct State\ntotal: Int\nmode: Mode\nend\nenum Mode\nOn\nOff\nend\n"
    "service\nstate State = State(total: 1, mode: On)\ntools read\n"
    "token from env \"TOKEN\"\nend\nfn read(state: State, _now: Time) : Int\nstate.total\nend\n",
    /* An added field. */
    "module Main\nstruct State\ncount: Int\nmode: Mode\nseen: Bool\nend\nenum Mode\nOn\nOff\nend\n"
    "service\nstate State = State(count: 1, mode: On, seen: false)\ntools read\n"
    "token from env \"TOKEN\"\nend\nfn read(state: State, _now: Time) : Int\nstate.count\nend\n",
    /* A reordered choice. */
    "module Main\nstruct State\ncount: Int\nmode: Mode\nend\nenum Mode\nOff\nOn\nend\n"
    "service\nstate State = State(count: 1, mode: On)\ntools read\n"
    "token from env \"TOKEN\"\nend\nfn read(state: State, _now: Time) : Int\nstate.count\nend\n",
};

static void schema_digests(void) {
    uint8_t queue[32], digests[4][32];
    state_digests(compile_path("services/task-queue/main.mo"), queue);
    for (unsigned i = 0; i < 4; i++)
        state_digests(compile(variants[i]), digests[i]);
    for (unsigned i = 1; i < 4; i++)
        CHECK(memcmp(digests[i], digests[0], 32) != 0);
    CHECK(memcmp(queue, digests[0], 32) != 0);
}

int main(void) {
    artifacts();
    schema_digests();
    return mo_test_report("artifact");
}
