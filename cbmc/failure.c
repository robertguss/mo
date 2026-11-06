/* Proves the actual length-prefixed diagnostic slice arithmetic and the walk over a record's
 * arguments, including preservation on refusal. UTF-8 semantics and complete records are
 * fuzzed. */
#include "base/failure.c"
#include "base/binary.c"
#include <stdlib.h>
size_t nondet_size(void);
uint8_t nondet_byte(void);
bool nondet_bool(void);
uint32_t nondet_u32(void);
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
void harness_mo_failure_argument_next(void) {
    uint8_t bytes[MO_CBMC_INPUT];
    for (size_t i = 0; i < sizeof bytes; i++)
        bytes[i] = nondet_byte();
    size_t length = nondet_size(), position = nondet_size();
    uint32_t remaining = nondet_u32();
    __CPROVER_assume(length <= sizeof bytes && position <= length);
    struct mo_failure_arguments walk = {{bytes, length}, position, remaining};
    struct mo_failure_slice out = {NULL, 123};
    enum mo_failure_argument next = mo_failure_argument_next(&walk, &out);
    /* An argument is a 4-byte length and that many bytes of a MOSV value. */
    size_t rest = length - position, size = 0;
    bool fits = false;
    if (rest >= 4) {
        size = (size_t)bytes[position] | (size_t)bytes[position + 1] << 8 |
               (size_t)bytes[position + 2] << 16 | (size_t)bytes[position + 3] << 24;
        fits = size >= 9 && size <= rest - 4 && memcmp(bytes + position + 4, "MOSV\1", 5) == 0;
    }
    enum mo_failure_argument expected =
        remaining == 0 ? (rest == 0 ? MO_FAILURE_ARGUMENTS_END : MO_FAILURE_ARGUMENTS_DAMAGED)
        : fits         ? MO_FAILURE_ARGUMENT
                       : MO_FAILURE_ARGUMENTS_DAMAGED;
    __CPROVER_assert(next == expected, "the count and the bytes decide exactly");
    __CPROVER_assert(walk.bytes.bytes == bytes && walk.bytes.length == length,
                     "the walk keeps its bytes");
    if (next == MO_FAILURE_ARGUMENT) {
        __CPROVER_assert(out.bytes == bytes + position + 4 && out.length == size,
                         "the argument is the span after its length");
        __CPROVER_assert(walk.position == position + 4 + size && walk.remaining == remaining - 1,
                         "the walk advances past exactly one argument");
    } else
        __CPROVER_assert(out.bytes == NULL && out.length == 123 && walk.position == position &&
                             walk.remaining == remaining,
                         "the end and a refusal preserve the walk and the output");
    reached_end();
}
