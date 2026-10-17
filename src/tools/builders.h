/* Promise inputs of types exposed without their insides (architecture 10.1, "Hidden types"). Such
 * a value is built only through its module: an exposed constant, or a call to an exposed function
 * that returns it, then up to three more calls that take it and return it. The calls run in a
 * scratch process, so one that stops costs nothing but that step, and the value is then copied
 * into the trial's heap. These plug into mo_inputs_init as exact-type factories. */
#ifndef MO_BUILDERS_H
#define MO_BUILDERS_H

#include "check/check.h"
#include "tools/deadline.h"
#include "tools/inputs.h"
#include "vm/vm.h"

struct mo_hidden;

struct mo_builders {
    struct mo_arena arena;
    const struct mo_image *image;
    const struct mo_limits *limits;
    struct mo_deadline *deadline;
    struct mo_hidden *hidden;
    struct mo_input_factory *factories;
    uint32_t count;
};

/* One factory per hidden type in the image, over an initialized image. program names which
 * module constants are exposed. A hidden type with no way to start one gets a factory that
 * refuses, so it is never built from its insides. Calls poll deadline. Free after every init. */
MO_MUST_USE enum mo_input_result mo_builders_init(struct mo_builders *builders,
                                                  const struct mo_program *program,
                                                  const struct mo_image *image,
                                                  const struct mo_limits *limits,
                                                  struct mo_deadline *deadline);
void mo_builders_free(struct mo_builders *builders);
static inline MO_MUST_USE const struct mo_input_factory *
mo_builders_factories(const struct mo_builders *builders) {
    return builders->factories;
}
static inline MO_MUST_USE uint32_t mo_builders_count(const struct mo_builders *builders) {
    return builders->count;
}

#endif
