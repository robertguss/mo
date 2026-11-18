#ifndef MO_TEST_CONFINE_CONTROL_H
#define MO_TEST_CONFINE_CONTROL_H
#include <stdbool.h>
struct mo_compiler;
bool mo_confine_control(const struct mo_compiler *compiler, const char *folder,
                        const char *const reads[3]);
#endif
