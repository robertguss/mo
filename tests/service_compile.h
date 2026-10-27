#ifndef MO_TEST_SERVICE_COMPILE_H
#define MO_TEST_SERVICE_COMPILE_H

#include "codegen/codegen.h"
#include "lower/lower.h"
#include "program_fixture.h"
#include "vm/vm.h"

static struct mo_image *compile(const char *source) {
    struct fixture f;
    init(&f, source);
    MO_ASSERT(f.program != NULL && !mo_diags_any(&f.diags));
    const char *modules[] = {"Main"};
    f.set.names = modules;
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_LOWER_WORK);
    struct mo_services services;
    struct mo_core *core =
        mo_lower_services(&arena, f.program, &f.sources, &f.diags, NULL, &services);
    MO_ASSERT(core != NULL && !mo_diags_any(&f.diags));
    struct mo_buffer wire;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    MO_ASSERT(mo_compile_services(core, &services, &wire, &f.diags));
    struct mo_load_error error;
    struct mo_image *image = mo_load(wire.bytes, wire.length, &error);
    MO_ASSERT(image != NULL);
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    MO_ASSERT(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    mo_buffer_free(&wire);
    mo_arena_free(&arena);
    free_fixture(&f);
    return image;
}

#endif
