/* The live-upgrade upload reader, with two limits shrunk so that every page shape fits one
 * bounded proof: a page decodes to MO_LIMIT_UPGRADE_PAGE_BYTES = 4 bytes (8 text bytes) and a
 * slot holds at most MO_LIMIT_BUNDLE_BYTES = 10 bytes (two full pages and a short third). The
 * code is the real reader; nothing is modeled. Text is up to MO_CBMC_UPLOAD_TEXT = 12 bytes,
 * one quad longer than a page, so oversize pages are covered. Both proofs check acceptance
 * against an independent encoder in both directions: an accepted text is exactly the encoding
 * of the bytes written, and the encoding of any short byte string is accepted. */
#include "base/limits.h"
#undef MO_LIMIT_UPGRADE_PAGE_BYTES
#define MO_LIMIT_UPGRADE_PAGE_BYTES 4
#undef MO_LIMIT_BUNDLE_BYTES
#define MO_LIMIT_BUNDLE_BYTES 10
#include "base/upload.c"
#include <stdlib.h>

#define MO_CBMC_UPLOAD_TEXT 12
/* Bytes whose encoding fits the text bound. */
#define MO_CBMC_UPLOAD_PLAIN 9

size_t nondet_size(void);
uint64_t nondet_u64(void);
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

static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
/* RFC 4648 padded encoding, written independently of the decoder. */
static size_t encode(const uint8_t *in, size_t n, uint8_t *out) {
    size_t at = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t word = (uint32_t)in[i] << 16;
        if (i + 1 < n)
            word |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n)
            word |= in[i + 2];
        out[at++] = (uint8_t)alphabet[(word >> 18) & 63];
        out[at++] = (uint8_t)alphabet[(word >> 12) & 63];
        out[at++] = i + 1 < n ? (uint8_t)alphabet[(word >> 6) & 63] : '=';
        out[at++] = i + 2 < n ? (uint8_t)alphabet[word & 63] : '=';
    }
    return at;
}
static bool same(const uint8_t *a, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i])
            return false;
    return true;
}
/* Nondeterministic text, and a nondeterministic short byte string with its encoding. */
struct sample {
    uint8_t text[MO_CBMC_UPLOAD_TEXT], plain[MO_CBMC_UPLOAD_PLAIN], encoded[MO_CBMC_UPLOAD_TEXT];
    size_t length, plain_length, encoded_length;
    bool encodes; /* text is exactly the encoding of plain */
};
static void sample(struct sample *s) {
    for (size_t i = 0; i < MO_CBMC_UPLOAD_TEXT; i++)
        s->text[i] = nondet_byte();
    for (size_t i = 0; i < MO_CBMC_UPLOAD_PLAIN; i++)
        s->plain[i] = nondet_byte();
    s->length = nondet_size();
    __CPROVER_assume(s->length <= MO_CBMC_UPLOAD_TEXT);
    s->plain_length = nondet_size();
    __CPROVER_assume(s->plain_length <= MO_CBMC_UPLOAD_PLAIN);
    s->encoded_length = encode(s->plain, s->plain_length, s->encoded);
    s->encodes = s->encoded_length == s->length && same(s->text, s->encoded, s->length);
}

void harness_mo_upload_base64(void) {
    struct sample s;
    sample(&s);
    size_t capacity = nondet_size();
    __CPROVER_assume(capacity <= MO_CBMC_UPLOAD_TEXT);
    /* An object of exactly capacity bytes, so any write past it fails the pointer check. */
    uint8_t *out = malloc(capacity);
    __CPROVER_assume(out != NULL);
    size_t written = 99;
    bool ok = mo_upload_base64(s.text, s.length, out, capacity, &written);
    if (ok) {
        uint8_t again[MO_CBMC_UPLOAD_TEXT];
        __CPROVER_assert(s.length % 4 == 0 && written <= capacity && written <= s.length / 4 * 3,
                         "accepted output is bounded");
        __CPROVER_assert(encode(out, written, again) == s.length && same(again, s.text, s.length),
                         "accepted text is exactly the canonical encoding of the output");
    }
    if (s.encodes && s.plain_length <= capacity)
        __CPROVER_assert(ok && written == s.plain_length && same(out, s.plain, written),
                         "every canonical encoding that fits is accepted and decodes back");
    if (s.encodes && s.plain_length > capacity)
        __CPROVER_assert(!ok, "output past capacity is refused");
    free(out);
    reached_end();
}

void harness_mo_upload_page(void) {
    size_t total = nondet_size();
    __CPROVER_assume(total > 0 && total <= MO_LIMIT_BUNDLE_BYTES);
    uint8_t *bytes = malloc(total);
    __CPROVER_assume(bytes != NULL);
    uint8_t before[MO_LIMIT_BUNDLE_BYTES];
    for (size_t i = 0; i < total; i++)
        before[i] = bytes[i] = nondet_byte();
    struct mo_upload u;
    mo_upload_init(&u, bytes, total);
    __CPROVER_assert(u.bytes == bytes && u.total == total && u.received == 0 && u.next == 0,
                     "a slot opens empty");
    /* Any reachable state: some pages accepted, every one full until the last. */
    size_t pages = (total - 1) / MO_LIMIT_UPGRADE_PAGE_BYTES + 1;
    uint64_t done = nondet_u64();
    __CPROVER_assume(done <= pages);
    u.next = done;
    u.received = done == pages ? total : (size_t)done * MO_LIMIT_UPGRADE_PAGE_BYTES;
    struct mo_upload saved = u;

    struct sample s;
    sample(&s);
    uint64_t index = nondet_u64(), offset = nondet_u64(), page_total = nondet_u64();
    enum mo_upload_status status = mo_upload_page(&u, index, offset, page_total, s.text, s.length);

    bool matches = page_total == total && index == saved.next && offset == saved.received;
    size_t remaining = total - saved.received;
    if (status == MO_UPLOAD_MORE || status == MO_UPLOAD_COMPLETE) {
        __CPROVER_assert(matches, "an accepted page carries the slot's total, index and offset");
        __CPROVER_assert(u.bytes == bytes && u.total == total && u.next == saved.next + 1,
                         "acceptance advances the index by one");
        __CPROVER_assert(u.received > saved.received && u.received <= total &&
                             u.received - saved.received <= MO_LIMIT_UPGRADE_PAGE_BYTES,
                         "acceptance advances by one nonempty page within total");
        size_t n = u.received - saved.received;
        __CPROVER_assert(n == MO_LIMIT_UPGRADE_PAGE_BYTES || u.received == total,
                         "only the last page is short");
        __CPROVER_assert((status == MO_UPLOAD_COMPLETE) == (u.received == total),
                         "complete exactly when every byte arrived");
        uint8_t again[MO_CBMC_UPLOAD_TEXT];
        __CPROVER_assert(encode(bytes + saved.received, n, again) == s.length &&
                             same(again, s.text, s.length),
                         "the stored page is exactly what the text encodes");
        __CPROVER_assert(same(bytes, before, saved.received) &&
                             same(bytes + u.received, before + u.received, total - u.received),
                         "acceptance writes only the page's range");
    } else {
        __CPROVER_assert(u.bytes == saved.bytes && u.total == saved.total &&
                             u.received == saved.received && u.next == saved.next,
                         "refusal preserves the slot");
        __CPROVER_assert(same(bytes, before, total), "refusal preserves the bytes");
        if (status == MO_UPLOAD_TOTAL)
            __CPROVER_assert(page_total != total, "total refusal names a different total");
        if (status == MO_UPLOAD_ORDER)
            __CPROVER_assert(page_total == total && !matches, "order refusal is out of range");
        if (status == MO_UPLOAD_ENCODING || status == MO_UPLOAD_SIZE)
            __CPROVER_assert(matches, "content is checked only for the next exact range");
    }
    if (matches && s.encodes) {
        bool fits = s.plain_length > 0 && s.plain_length <= MO_LIMIT_UPGRADE_PAGE_BYTES &&
                    s.plain_length <= remaining &&
                    (s.plain_length == MO_LIMIT_UPGRADE_PAGE_BYTES || s.plain_length == remaining);
        __CPROVER_assert(fits ? status == MO_UPLOAD_MORE || status == MO_UPLOAD_COMPLETE
                              : status == MO_UPLOAD_SIZE,
                         "a canonical page is accepted exactly when its size fits");
        if (fits)
            __CPROVER_assert(same(bytes + saved.received, s.plain, s.plain_length),
                             "the accepted page holds the encoded bytes");
    }
    free(bytes);
    reached_end();
}
