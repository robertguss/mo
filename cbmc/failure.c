/* Proves the actual length-prefixed diagnostic slice arithmetic, including
 * preservation on refusal. UTF-8 semantics and complete records are fuzzed. */
#include "base/failure.c"
#include "base/binary.c"
#include <stdlib.h>
size_t nondet_size(void);
uint8_t nondet_byte(void);
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
void harness_slice(void) {
    uint8_t bytes[MO_CBMC_INPUT];
    for (size_t i = 0; i < sizeof bytes; i++)
        bytes[i] = nondet_byte();
    size_t length = nondet_size(), position = nondet_size(), maximum = nondet_size();
    __CPROVER_assume(length <= sizeof bytes && position <= length);
    struct mo_binary in = {bytes, length, position};
    struct mo_failure_slice out = {NULL, 123};
    bool ok = slice(&in, maximum, nondet_bool(), &out);
    if (ok) {
        __CPROVER_assert(out.length <= maximum, "slice observes caller limit");
        __CPROVER_assert(out.bytes >= bytes && out.bytes + out.length <= bytes + length,
                         "accepted slice inside complete input");
        __CPROVER_assert(in.position <= length, "reader never advances past input");
    } else
        __CPROVER_assert(out.bytes == NULL && out.length == 123, "refusal preserves output");
    reached_end();
}
