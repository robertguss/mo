#include "base/stack.h"
#include "test.h"

#include <errno.h>
#include <stdint.h>

static void large_frame(void *argument) {
    volatile uint8_t bytes[1 << 20];
    unsigned *value = argument;
    bytes[0] = (uint8_t)*value;
    bytes[sizeof(bytes) - 1] = 19;
    *value = (unsigned)bytes[0] + bytes[sizeof(bytes) - 1];
}

static void small_caller(void *argument) {
    CHECK(mo_with_stack(4 << 20, large_frame, argument) == 0);
}

int main(void) {
    unsigned value = 17;
    CHECK(mo_with_stack(512 << 10, small_caller, &value) == 0);
    CHECK(value == 36);
    CHECK(mo_with_stack(1, large_frame, &value) == EINVAL);
    CHECK(value == 36);
    return mo_test_report("stack");
}
