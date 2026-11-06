/* Real generation/coverage admission, with unconstrained metadata and byte identities. */
#include "base/export.c"
#include <stdlib.h>

size_t nondet_size(void);
uint64_t nondet_u64(void);
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
void harness_export_page_matches(void) {
    struct mo_export_consumer c = {.bytes = nondet_bool() ? NULL : (uint8_t *)1,
                                   .offset = nondet_size(),
                                   .total = nondet_size(),
                                   .sequence = nondet_u64(),
                                   .encoded_sequence = nondet_u64(),
                                   .complete = nondet_bool()};
    struct mo_export_page p = {.offset = nondet_size(),
                               .total = nondet_size(),
                               .length = nondet_size(),
                               .count = nondet_size(),
                               .sequence = nondet_u64(),
                               .encoded_sequence = nondet_u64(),
                               .final = nondet_bool()};
    for (size_t i = 0; i < 16; i++) {
        c.id[i] = nondet_byte();
        p.id[i] = nondet_byte();
    }
    bool ok = export_page_matches(&c, &p);
    if (ok) {
        __CPROVER_assert(!c.complete && p.total >= MO_EXPORT_FIXED_BYTES &&
                             p.total <= MO_LIMIT_EXPORT_BYTES / MO_LIMIT_EXPORT_PINS &&
                             p.offset < p.total,
                         "accepted page has bounded nonempty total and valid offset");
        __CPROVER_assert(p.length > 0 && p.length <= MO_LIMIT_EXPORT_PAGE_BYTES &&
                             p.length <= p.total - p.offset && p.offset + p.length <= p.total,
                         "coverage advance cannot overflow or exceed the artifact");
        __CPROVER_assert(p.count > 0 && (p.count - 1) * MO_LIMIT_EXPORT_PAGE_BYTES < p.total &&
                             p.count * MO_LIMIT_EXPORT_PAGE_BYTES >= p.total,
                         "page count is the exact ceiling");
        __CPROVER_assert(p.final == (p.offset + p.length == p.total),
                         "completion requires exact coverage");
        if (c.bytes == NULL)
            __CPROVER_assert(p.offset == 0, "creation begins at offset zero");
        else {
            __CPROVER_assert(
                p.total == c.total && p.offset == c.offset && p.sequence == c.sequence &&
                    p.encoded_sequence == c.encoded_sequence && memcmp(p.id, c.id, 16) == 0,
                "continuation preserves generation, both sequences and coverage");
        }
    }
    reached_end();
}
