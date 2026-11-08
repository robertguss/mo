#include "base/upload.h"
#include "test.h"
#include <string.h>

#define PAGE MO_LIMIT_UPGRADE_PAGE_BYTES

static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static uint8_t source[MO_LIMIT_BUNDLE_BYTES], storage[MO_LIMIT_BUNDLE_BYTES],
    snapshot[MO_LIMIT_BUNDLE_BYTES], text[MO_LIMIT_UPGRADE_PAGE_TEXT + 16];

/* Independent RFC 4648 encoder, padded. */
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

static void fill(size_t total) {
    for (size_t i = 0; i < total; i++)
        source[i] = (uint8_t)(i * 131 + (i >> 8) * 7 + 1);
    memset(storage, 0xAA, sizeof storage);
}

/* Sends page index of source as the upload expects it. */
static enum mo_upload_status send(struct mo_upload *u, uint64_t index) {
    size_t offset = (size_t)index * PAGE;
    size_t n = u->total - offset < PAGE ? u->total - offset : PAGE;
    size_t length = encode(source + offset, n, text);
    return mo_upload_page(u, index, offset, u->total, text, length);
}

/* The page is refused with the expected status and nothing changes. */
static void refused(struct mo_upload *u, uint64_t index, uint64_t offset, uint64_t total,
                    const uint8_t *page, size_t length, enum mo_upload_status expected) {
    struct mo_upload before = *u;
    memcpy(snapshot, storage, sizeof storage);
    CHECK(mo_upload_page(u, index, offset, total, page, length) == expected);
    CHECK(u->bytes == before.bytes && u->total == before.total && u->received == before.received &&
          u->next == before.next);
    CHECK(memcmp(snapshot, storage, sizeof storage) == 0);
}
static void refused_text(struct mo_upload *u, uint64_t index, const char *page,
                         enum mo_upload_status expected) {
    refused(u, index, u->received, u->total, (const uint8_t *)page, strlen(page), expected);
}

static void whole(size_t total, uint64_t pages) {
    fill(total);
    struct mo_upload u;
    mo_upload_init(&u, storage, total);
    for (uint64_t i = 0; i + 1 < pages; i++)
        CHECK(send(&u, i) == MO_UPLOAD_MORE);
    CHECK(send(&u, pages - 1) == MO_UPLOAD_COMPLETE);
    CHECK(u.received == total && u.next == pages);
    CHECK(memcmp(storage, source, total) == 0);
    CHECK(storage[total - 1] == source[total - 1]);
    if (total < sizeof storage)
        CHECK(storage[total] == 0xAA);
}

static void base64_cases(void) {
    uint8_t out[16];
    size_t written = 99;
    CHECK(mo_upload_base64((const uint8_t *)"", 0, out, sizeof out, &written) && written == 0);
    CHECK(mo_upload_base64((const uint8_t *)"TWFu", 4, out, sizeof out, &written) && written == 3 &&
          memcmp(out, "Man", 3) == 0);
    CHECK(mo_upload_base64((const uint8_t *)"TWE=", 4, out, sizeof out, &written) && written == 2 &&
          memcmp(out, "Ma", 2) == 0);
    CHECK(mo_upload_base64((const uint8_t *)"TQ==", 4, out, sizeof out, &written) && written == 1 &&
          out[0] == 'M');
    CHECK(mo_upload_base64((const uint8_t *)"+/+/", 4, out, sizeof out, &written) && written == 3 &&
          out[0] == 0xFB && out[1] == 0xFF && out[2] == 0xBF);
    /* Never writes past capacity, even when the text is valid. */
    memset(out, 0x55, sizeof out);
    CHECK(!mo_upload_base64((const uint8_t *)"TWFuTWFu", 8, out, 5, &written));
    CHECK(out[5] == 0x55 && out[6] == 0x55);
    CHECK(mo_upload_base64((const uint8_t *)"TWFuTWFu", 8, out, 6, &written) && written == 6);
    const char *bad[] = {
        "T",      "TW",   "TWF",  "TWFuT", "TW-u",  "TW_u",  "TWF\n",    "TW u",
        "TWFu\n", "TQ",   "TWE",  "TQ=",   "TQ===", "TWE==", "TQ==TQ==", "TW=u",
        "T===",   "====", "=TWF", "TR==",  "TWF=",  "TWG=",  "TQ==\r\n", "TWFu===="};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(
            !mo_upload_base64((const uint8_t *)bad[i], strlen(bad[i]), out, sizeof out, &written));
    uint8_t high[4] = {'T', 'W', 'F', 0xC3};
    CHECK(!mo_upload_base64(high, 4, out, sizeof out, &written));
    /* Exactly one final character is canonical per decoded byte value. */
    for (size_t c = 0; c < 64; c++) {
        uint8_t one[4] = {'A', (uint8_t)alphabet[c], '=', '='};
        uint8_t two[4] = {'A', 'A', (uint8_t)alphabet[c], '='};
        CHECK(mo_upload_base64(one, 4, out, sizeof out, &written) == (c % 16 == 0));
        CHECK(mo_upload_base64(two, 4, out, sizeof out, &written) == (c % 4 == 0));
    }
    /* Every short byte string round-trips through the independent encoder. */
    uint8_t in[12], encoded[16];
    for (size_t n = 0; n <= sizeof in; n++)
        for (unsigned seed = 0; seed < 64; seed++) {
            for (size_t i = 0; i < n; i++)
                in[i] = (uint8_t)(seed * 37 + i * 101 + (seed >> 2) * i);
            size_t length = encode(in, n, encoded);
            CHECK(mo_upload_base64(encoded, length, out, sizeof out, &written) && written == n &&
                  memcmp(out, in, n) == 0);
        }
}

static void refusals(void) {
    size_t total = 2 * PAGE + 100;
    fill(total);
    struct mo_upload u;
    mo_upload_init(&u, storage, total);
    CHECK(u.bytes == storage && u.total == total && u.received == 0 && u.next == 0);
    CHECK(send(&u, 0) == MO_UPLOAD_MORE);
    CHECK(u.received == PAGE && u.next == 1);
    size_t full = encode(source + PAGE, PAGE, text);

    /* The slot's total is checked first, then index, offset, encoding and size. */
    refused(&u, 1, PAGE, total + 1, text, full, MO_UPLOAD_TOTAL);
    refused(&u, 1, PAGE, total - 1, text, full, MO_UPLOAD_TOTAL);
    refused(&u, 1, PAGE, UINT64_MAX, text, full, MO_UPLOAD_TOTAL);
    refused(&u, 7, 3, 0, (const uint8_t *)"T", 1, MO_UPLOAD_TOTAL);
    refused(&u, 2, PAGE, total, text, full, MO_UPLOAD_ORDER);
    refused(&u, 0, 0, total, text, full, MO_UPLOAD_ORDER);
    refused(&u, 0, PAGE, total, text, full, MO_UPLOAD_ORDER);
    refused(&u, UINT64_MAX, PAGE, total, text, full, MO_UPLOAD_ORDER);
    refused(&u, 1, 0, total, text, full, MO_UPLOAD_ORDER);
    refused(&u, 1, PAGE + 1, total, text, full, MO_UPLOAD_ORDER);
    refused(&u, 1, PAGE - 1, total, text, full, MO_UPLOAD_ORDER);
    refused(&u, 1, UINT64_MAX, total, text, full, MO_UPLOAD_ORDER);
    refused(&u, 2, 0, total, (const uint8_t *)"T", 1, MO_UPLOAD_ORDER);
    refused(&u, 1, PAGE, total, text, full - 1, MO_UPLOAD_ENCODING);
    refused_text(&u, 1, "TW-u", MO_UPLOAD_ENCODING);
    refused_text(&u, 1, "TWFu TWFu", MO_UPLOAD_ENCODING);
    refused_text(&u, 1, "TQ", MO_UPLOAD_ENCODING);
    refused_text(&u, 1, "TQ==TQ==", MO_UPLOAD_ENCODING);
    refused_text(&u, 1, "TR==", MO_UPLOAD_ENCODING);
    refused_text(&u, 1, "TWF=", MO_UPLOAD_ENCODING);
    refused_text(&u, 1, "TWFuT", MO_UPLOAD_ENCODING);
    /* An oversize page with a bad character is an encoding refusal: encoding before size. */
    size_t over = encode(source, PAGE + 3, text);
    text[over - 1] = '-';
    refused(&u, 1, PAGE, total, text, over, MO_UPLOAD_ENCODING);
    refused_text(&u, 1, "", MO_UPLOAD_SIZE);
    refused(&u, 1, PAGE, total, NULL, 0, MO_UPLOAD_SIZE);
    refused_text(&u, 1, "TWFu", MO_UPLOAD_SIZE);
    over = encode(source, PAGE + 1, text);
    refused(&u, 1, PAGE, total, text, over, MO_UPLOAD_SIZE);
    full = encode(source + PAGE, PAGE - 1, text);
    refused(&u, 1, PAGE, total, text, full, MO_UPLOAD_SIZE);
    CHECK(send(&u, 1) == MO_UPLOAD_MORE);

    /* The last page must end exactly at total. */
    size_t last = encode(source + 2 * PAGE, 101, text);
    refused(&u, 2, 2 * PAGE, total, text, last, MO_UPLOAD_SIZE);
    last = encode(source + 2 * PAGE, 99, text);
    refused(&u, 2, 2 * PAGE, total, text, last, MO_UPLOAD_SIZE);
    last = encode(source + 2 * PAGE, PAGE, text);
    refused(&u, 2, 2 * PAGE, total, text, last, MO_UPLOAD_SIZE);
    CHECK(send(&u, 2) == MO_UPLOAD_COMPLETE);
    CHECK(memcmp(storage, source, total) == 0);

    /* After completion every page is refused. */
    last = encode(source + 2 * PAGE, 100, text);
    refused(&u, 2, 2 * PAGE, total, text, last, MO_UPLOAD_ORDER);
    refused(&u, 3, 2 * PAGE, total, text, last, MO_UPLOAD_ORDER);
    refused_text(&u, 3, "TQ==", MO_UPLOAD_SIZE);
    refused_text(&u, 3, "", MO_UPLOAD_SIZE);

    /* A one-byte slot. */
    memset(storage, 0xAA, sizeof storage);
    mo_upload_init(&u, storage, 1);
    refused_text(&u, 0, "TWE=", MO_UPLOAD_SIZE);
    refused_text(&u, 0, "TR==", MO_UPLOAD_ENCODING);
    CHECK(mo_upload_page(&u, 0, 0, 1, (const uint8_t *)"TQ==", 4) == MO_UPLOAD_COMPLETE);
    CHECK(storage[0] == 'M' && storage[1] == 0xAA && u.received == 1 && u.next == 1);
    refused_text(&u, 0, "TQ==", MO_UPLOAD_ORDER);
    refused_text(&u, 1, "TQ==", MO_UPLOAD_SIZE);
}

int main(void) {
    base64_cases();
    whole(1, 1);
    whole(5, 1);
    whole(PAGE - 1, 1);
    whole(PAGE, 1);
    whole(PAGE + 1, 2);
    whole(3 * PAGE - 4000, 3);
    whole(MO_LIMIT_BUNDLE_BYTES - 1, MO_LIMIT_BUNDLE_BYTES / PAGE);
    whole(MO_LIMIT_BUNDLE_BYTES, MO_LIMIT_BUNDLE_BYTES / PAGE);
    CHECK(MO_LIMIT_BUNDLE_BYTES / PAGE == 128 && MO_LIMIT_UPGRADE_PAGE_TEXT == 10924);
    refusals();
    return mo_test_report("test_upload");
}
