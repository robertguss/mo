#include "base/limits.h"
#include "test.h"

#include <stdint.h>

static void test_the_defaults(void) {
    struct mo_limits limits = mo_limits_default();
    CHECK(limits.stack_size == MO_DEFAULT_STACK_SIZE);
    CHECK(limits.call_depth == MO_DEFAULT_CALL_DEPTH);
    CHECK(limits.heap_size == MO_DEFAULT_HEAP_SIZE);
    /* The stack holds a frame of the most registers a function may have, 16 bytes each. */
    CHECK(limits.stack_size >= (size_t)MO_LIMIT_FUNCTION_REGISTERS * 16);
    CHECK(limits.call_depth > 0 && limits.heap_size > 0);
}

static void test_a_run_sets_its_own(void) {
    struct mo_limits limits = mo_limits_default();
    limits.call_depth = 3;
    limits.heap_size = 4096;
    CHECK(limits.call_depth == 3 && limits.heap_size == 4096);
    CHECK(limits.stack_size == MO_DEFAULT_STACK_SIZE);
}

static void test_counts_fit_their_fields(void) {
    /* Spans, line tables and the sources hold offsets and counts in 32 bits, and diagnostics
     * hold their count in 32 bits too. */
    CHECK((uint64_t)MO_LIMIT_PROGRAM_SIZE <= UINT32_MAX);
    CHECK((uint64_t)MO_LIMIT_FILES <= UINT32_MAX);
    CHECK((uint64_t)MO_LIMIT_DIAGNOSTICS <= UINT32_MAX);
    CHECK(MO_LIMIT_BYTECODE_TEXT_SIZE == UINT64_C(8) * MO_LIMIT_BYTECODE_SIZE);
    CHECK(MO_LIMIT_BYTECODE_TEXT_SIZE <= UINT32_MAX);
    CHECK(MO_LIMIT_PARAMETERS == 255);
    CHECK(MO_LIMIT_BYTECODE_PARAMETERS == 256);
    CHECK(MO_LIMIT_BYTECODE_PARAMETERS <= MO_LIMIT_OPERANDS);
    CHECK(MO_LIMIT_BYTECODE_PARAMETERS <= MO_LIMIT_FUNCTION_REGISTERS);
}

int main(void) {
    test_the_defaults();
    test_a_run_sets_its_own();
    test_counts_fit_their_fields();
    return mo_test_report("test_limits");
}
