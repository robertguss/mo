#define CONFINED 1
#include "confinement.h"

#if defined(__linux__)
static void cases(void) {
    CHECK(mo_compile_can_confine());
    struct outcome o = run_probe();
    CHECK(o.status == MO_COMPILE_OK);
    CHECK(o.needs == NEEDS && o.needed == NEEDS);
    CHECK(o.tries == ATTEMPTS && o.reported == ATTEMPTS && o.refused == ATTEMPTS);
    CHECK(o.outputs_kept);
}
#endif
