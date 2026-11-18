#define CONFINED 0
#include "confine_control.h"
#include "confinement.h"

static bool answer;
bool mo_confine_control(const struct mo_compiler *compiler, const char *folder,
                        const char *const reads[3]) {
    (void)compiler;
    (void)folder;
    (void)reads;
    return answer;
}

#if defined(__linux__)
static void cases(void) {
    CHECK(mo_compile_can_confine());
    answer = true;
    struct outcome o = run_probe();
    CHECK(o.status == MO_COMPILE_OK);
    CHECK(o.needs == NEEDS && o.needed == NEEDS);
    CHECK(o.tries == ATTEMPTS && o.reported == ATTEMPTS && o.allowed == ATTEMPTS);
    CHECK(!o.outputs_kept);
    answer = false;
    o = run_probe();
    CHECK(o.status == MO_COMPILE_IO);
    CHECK(o.needs == 0 && o.tries == 0 && o.reported == 0);
    CHECK(o.outputs_kept);
}
#endif
