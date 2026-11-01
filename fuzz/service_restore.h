#ifndef MO_FUZZ_SERVICE_RESTORE_H
#define MO_FUZZ_SERVICE_RESTORE_H

#include "../tests/program_fixture.h"
#include "codegen/codegen.h"
#include "lower/lower.h"
#include "vm/vm.h"

static struct mo_service_time restore_clock(void *context) {
    (void)context;
    return (struct mo_service_time){0};
}

static void restore_reply(const struct mo_service_response *reply, void *context) {
    (void)reply;
    (void)context;
    MO_UNREACHABLE();
}

static void restore_event(const struct mo_service_event *event, void *context) {
    (void)event;
    (void)context;
}

/* Built once per fuzzing process: compiling per input left only the compiler
 * to fuzz. The image is reachable until exit. */
static struct mo_image *restore_image(void) {
    static struct mo_image *image;
    if (image != NULL)
        return image;
    struct fixture f;
    init(&f, "module Main\nservice\nstate Int = 7\ntools add\n"
             "token from env \"TOKEN\"\nend\n"
             "fn add(state: Int, _now: Time, n: Int) : (Int, Int)\n"
             "(state + n, state + n)\nend\n");
    MO_ASSERT(f.program != NULL && !mo_diags_any(&f.diags));
    const char *modules[] = {"Main"};
    f.set.names = modules;
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_LOWER_WORK);
    struct mo_services services;
    struct mo_core *core =
        mo_lower_services(&arena, f.program, &f.sources, &f.diags, NULL, &services);
    MO_ASSERT(core != NULL && !mo_diags_any(&f.diags));
    struct mo_buffer bytes;
    mo_buffer_init(&bytes, MO_LIMIT_BYTECODE_SIZE);
    MO_ASSERT(mo_compile_services(core, &services, &bytes, &f.diags));
    struct mo_load_error error;
    image = mo_load(bytes.bytes, bytes.length, &error);
    MO_ASSERT(image != NULL);
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    MO_ASSERT(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    mo_buffer_free(&bytes);
    mo_arena_free(&arena);
    free_fixture(&f);
    return image;
}

static void fuzz_restore(enum mo_store_record_kind kind, const uint8_t *data, size_t size) {
    if (size > 65536)
        return;
    struct mo_image *image = restore_image();
    struct mo_store *store;
    MO_ASSERT(mo_store_open(NULL, &store) == MO_STORE_OK);
    struct mo_service_hooks hooks = {restore_clock, restore_reply, restore_event, NULL};
    struct mo_service_loop *loop = NULL;
    MO_ASSERT(mo_service_open(image, 0, &hooks, store, &loop) == MO_SERVICE_OK);
    mo_service_free(loop);
    struct mo_store_record record = {kind, kind == MO_STORE_STATE ? 0 : 1, mo_image_key(image, 0),
                                     data, size};
    /* Use the real writer to produce a valid frame checksum. This drives the
     * VM message decoder, not just the host envelope rejection path. */
    MO_ASSERT(mo_store_call(store, kind == MO_STORE_STATE ? MO_STORE_SNAPSHOT : MO_STORE_APPEND,
                            &record, NULL, NULL) == MO_STORE_OK);
    loop = NULL;
    enum mo_service_status status = mo_service_open(image, 0, &hooks, store, &loop);
    MO_ASSERT(status == MO_SERVICE_OK || status == MO_SERVICE_SAVED_INVALID);
    MO_ASSERT((loop != NULL) == (status == MO_SERVICE_OK));
    mo_service_free(loop);
    mo_store_close(store);
}

#endif
