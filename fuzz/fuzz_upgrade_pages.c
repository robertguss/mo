/* One live-upgrade upload driven by a sequence of page commands. Input: a 4-byte little-endian
 * slot total, then commands of mode (1 byte), index, offset and total (8 bytes each), a 2-byte
 * length and that many payload bytes. Mode bits 1, 2 and 4 replace index, offset and total by
 * the values the upload expects; bit 8 sends the base64 encoding of the payload instead of the
 * payload itself; bit 16 gives the decoder one byte less than it needs. */
#include "base/upload.h"
#include <string.h>

#define PAYLOAD_MAX 65535
#define ENCODED_MAX (4 * ((PAYLOAD_MAX + 2) / 3))

static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static uint8_t storage[MO_LIMIT_BUNDLE_BYTES + 1], shadow[MO_LIMIT_BUNDLE_BYTES + 1];
static uint8_t text[ENCODED_MAX], again[ENCODED_MAX], decoded[PAYLOAD_MAX + 1],
    direct[PAYLOAD_MAX + 1];

static uint64_t little(const uint8_t *bytes, unsigned width) {
    uint64_t value = 0;
    for (unsigned i = 0; i < width; i++)
        value |= (uint64_t)bytes[i] << (8 * i);
    return value;
}

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

/* A bit-stream decode that is canonical exactly when encode() gives the text back, so the
 * verdict rests on the encoder alone. */
static bool reference(const uint8_t *in, size_t length, uint8_t *out, size_t *count) {
    if (length % 4 != 0)
        return false;
    size_t pad = 0;
    if (length >= 4 && in[length - 1] == '=')
        pad = in[length - 2] == '=' ? 2 : 1;
    uint32_t acc = 0;
    unsigned bits = 0;
    size_t at = 0;
    for (size_t i = 0; i < length - pad; i++) {
        const char *p = memchr(alphabet, in[i], 64);
        if (p == NULL)
            return false;
        acc = ((acc << 6) | (uint32_t)(p - alphabet)) & 0xFFFFFF;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[at++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    *count = at;
    return encode(out, at, again) == length && memcmp(again, in, length) == 0;
}

static enum mo_upload_status expect(const struct mo_upload *u, uint64_t index, uint64_t offset,
                                    uint64_t total, bool canonical, size_t n) {
    if (total != u->total)
        return MO_UPLOAD_TOTAL;
    if (index != u->next || offset != u->received)
        return MO_UPLOAD_ORDER;
    if (!canonical)
        return MO_UPLOAD_ENCODING;
    size_t remaining = u->total - u->received;
    if (n == 0 || n > MO_LIMIT_UPGRADE_PAGE_BYTES || n > remaining ||
        (n < MO_LIMIT_UPGRADE_PAGE_BYTES && n != remaining))
        return MO_UPLOAD_SIZE;
    return n == remaining ? MO_UPLOAD_COMPLETE : MO_UPLOAD_MORE;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 4)
        return 0;
    uint64_t declared = little(data, 4);
    if (declared == 0 || declared > MO_LIMIT_BUNDLE_BYTES)
        return 0;
    size_t total = (size_t)declared;
    memset(storage, 0x5A, total + 1);
    memset(shadow, 0x5A, total + 1);
    struct mo_upload u;
    mo_upload_init(&u, storage, total);
    size_t at = 4;
    while (size - at >= 27) {
        uint8_t mode = data[at];
        uint64_t index = little(data + at + 1, 8), offset = little(data + at + 9, 8),
                 page_total = little(data + at + 17, 8);
        size_t length = (size_t)little(data + at + 25, 2);
        at += 27;
        if (length > size - at)
            length = size - at;
        const uint8_t *page = data + at, *payload = data + at;
        at += length;
        if (mode & 1)
            index = u.next;
        if (mode & 2)
            offset = u.received;
        if (mode & 4)
            page_total = u.total;
        if (mode & 8) {
            length = encode(payload, length, text);
            page = text;
        }
        size_t n = 0;
        bool canonical = reference(page, length, decoded, &n);
        struct mo_upload before = u;
        enum mo_upload_status wanted = expect(&u, index, offset, page_total, canonical, n);
        enum mo_upload_status status = mo_upload_page(&u, index, offset, page_total, page, length);
        MO_ASSERT(status == wanted);
        MO_ASSERT(u.bytes == storage && u.total == total && u.received <= u.total);
        if (status == MO_UPLOAD_MORE || status == MO_UPLOAD_COMPLETE) {
            MO_ASSERT(u.next == before.next + 1 && u.received == before.received + n);
            MO_ASSERT((status == MO_UPLOAD_COMPLETE) == (u.received == u.total));
            memcpy(shadow + before.received, decoded, n);
        } else
            MO_ASSERT(u.next == before.next && u.received == before.received);
        /* The accepted bytes are the reference decode; nothing else, past total included, moved. */
        MO_ASSERT(memcmp(storage, shadow, total + 1) == 0);

        size_t written = 0;
        if (canonical) {
            size_t capacity = (mode & 16) && n > 0 ? n - 1 : n;
            direct[capacity] = 0xC3;
            bool ok = mo_upload_base64(page, length, direct, capacity, &written);
            MO_ASSERT(ok == (capacity == n) && direct[capacity] == 0xC3);
            MO_ASSERT(!ok || (written == n && memcmp(direct, decoded, n) == 0));
        } else
            MO_ASSERT(!mo_upload_base64(page, length, direct, sizeof direct, &written));
    }
    return 0;
}
