/* Actual arithmetic and MOSV framing reader. Hashing is modeled: the model checks the
 * selected span, while SHA-256's bytes are independently tested against hashlib. */
#include "base/export.c"
#include "base/binary.c"
#include <stdlib.h>

size_t nondet_size(void);
uint8_t nondet_byte(void);
void reached_end(void) {
}
_Noreturn void mo_assert_failed(const char *expression, const char *file, int line) {
    (void)expression;
    (void)file;
    (void)line;
    __CPROVER_assert(0, "MO_ASSERT holds");
    abort();
}
static const uint8_t *source;
static size_t source_length;
void model_digest(const uint8_t *bytes, size_t length, uint8_t out[32]) {
    __CPROVER_assert(bytes == source + 9, "schema starts after MOSV header");
    __CPROVER_assert(length > 0 && length <= source_length - 9, "schema span is within input");
    (void)out;
}
void harness_mo_export_page_bounds(void) {
    size_t total = nondet_size(), offset = nondet_size(), length = 77, count = 88;
    bool ok = mo_export_page_bounds(total, offset, &length, &count);
    __CPROVER_assert(ok == (total > 0 && total <= MO_LIMIT_EXPORT_BYTES / MO_LIMIT_EXPORT_PINS &&
                            offset < total && offset % MO_LIMIT_EXPORT_PAGE_BYTES == 0),
                     "acceptance exactly matches bounds");
    if (ok) {
        __CPROVER_assert(length > 0 && length <= MO_LIMIT_EXPORT_PAGE_BYTES &&
                             length <= total - offset,
                         "page span bounded");
        __CPROVER_assert(count > 0 && count <= 2176 &&
                             (count - 1) * MO_LIMIT_EXPORT_PAGE_BYTES < total &&
                             count * MO_LIMIT_EXPORT_PAGE_BYTES >= total,
                         "exact ceiling page count");
    } else
        __CPROVER_assert(length == 77 && count == 88, "refusal preserves outputs");
    reached_end();
}
void harness_mo_export_schema(void) {
    uint8_t bytes[MO_CBMC_INPUT], out[32];
    for (size_t i = 0; i < MO_CBMC_INPUT; i++)
        bytes[i] = nondet_byte();
    size_t length = nondet_size();
    __CPROVER_assume(length <= sizeof bytes);
    source = bytes;
    source_length = length;
    bool expected = length >= 9 && memcmp(bytes, "MOSV\1", 5) == 0;
    uint64_t shape = 0;
    if (expected) {
        shape = (uint64_t)bytes[5] | ((uint64_t)bytes[6] << 8) | ((uint64_t)bytes[7] << 16) |
                ((uint64_t)bytes[8] << 24);
        expected = shape > 0 && shape <= length - 9;
    }
    bool ok = mo_export_schema(bytes, length, out);
    __CPROVER_assert(ok == expected, "acceptance requires exact bounded schema span");
    reached_end();
}
