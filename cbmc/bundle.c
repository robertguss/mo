/* Complete framing reader, bounded to MO_CBMC_INPUT bytes. UTF-8 is modeled as
 * an unconstrained predicate after checking its span; its codec has separate proofs. */
#include "base/bundle.c"
#include "base/binary.c"
#include <stdlib.h>
size_t nondet_size(void);
uint8_t nondet_byte(void);
bool nondet_bool(void);
int nondet_int(void);
/* Model the content comparison only. These proofs check the reader's bounds;
 * libc's comparison implementation and ordering semantics are assumptions. */
bool model_component_is(const uint8_t *bytes, size_t length, const char *word) {
    __CPROVER_assert(__CPROVER_r_ok(bytes, length), "component comparison is readable");
    (void)word;
    return nondet_bool();
}
int model_compare(struct mo_bundle_span a, struct mo_bundle_span b) {
    __CPROVER_assert(__CPROVER_r_ok(a.bytes, a.length), "left comparison is readable");
    __CPROVER_assert(__CPROVER_r_ok(b.bytes, b.length), "right comparison is readable");
    return nondet_int();
}
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
bool model_mo_utf8_valid(const uint8_t *bytes, size_t length) {
    __CPROVER_assert(length == 0 || (bytes >= source && bytes + length <= source + source_length),
                     "UTF-8 span is inside input");
    return nondet_bool();
}
/* The path policy has its own proof. Here retain its bounded framing and model
 * only acceptance of the byte content. */
bool model_bundle_path(struct mo_binary *in, struct mo_bundle_span *out) {
    struct mo_bundle_span next;
    if (!bundle_span(in, MO_LIMIT_BUNDLE_PATH_BYTES, &next) || next.length == 0 || !nondet_bool())
        return false;
    *out = next;
    return true;
}
void harness_bundle_path(void) {
    uint8_t bytes[MO_CBMC_INPUT];
    for (size_t i = 0; i < sizeof bytes; i++)
        bytes[i] = nondet_byte();
    size_t length = nondet_size();
    __CPROVER_assume(length <= sizeof bytes);
    source = bytes;
    source_length = length;
    struct mo_binary in = {bytes, length, 0};
    struct mo_bundle_span out;
    if (bundle_path(&in, &out)) {
        __CPROVER_assert(out.length > 0 && out.length <= MO_LIMIT_BUNDLE_PATH_BYTES,
                         "path length bounded");
        __CPROVER_assert(out.bytes == bytes + 4 && out.length + 4 == in.position &&
                             in.position <= length,
                         "path span inside input");
    }
    reached_end();
}
void harness_mo_bundle_read(void) {
    uint8_t bytes[MO_CBMC_INPUT];
    for (size_t i = 0; i < sizeof bytes; i++)
        bytes[i] = nondet_byte();
    size_t length = nondet_size();
    __CPROVER_assume(length <= sizeof bytes);
    source = bytes;
    source_length = length;
    struct mo_bundle out = {.count = 99};
    bool ok = mo_bundle_read(bytes, length, &out);
    if (ok) {
        __CPROVER_assert(out.count > 0 && out.count <= MO_LIMIT_BUNDLE_FILES,
                         "accepted file count is bounded");
        __CPROVER_assert(out.entry.length > 0 && out.entry.length <= MO_LIMIT_BUNDLE_PATH_BYTES,
                         "accepted entry is bounded");
        for (uint32_t i = 0; i < out.count; i++) {
            struct mo_bundle_file f = out.files[i];
            __CPROVER_assert(f.path.length > 0 && f.path.bytes >= bytes &&
                                 f.path.bytes + f.path.length <= bytes + length,
                             "accepted path borrows input");
            __CPROVER_assert(
                f.source.length == 0 ||
                    (f.source.bytes >= bytes && f.source.bytes + f.source.length <= bytes + length),
                "accepted source borrows input");
        }
    } else
        __CPROVER_assert(out.count == 99, "refusal preserves output");
    reached_end();
}
