#include "base/upload.h"
#include <string.h>

static int sextet(uint8_t c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

/* Validates all of text and counts every decoded byte, but writes only the first capacity, so
 * the caller can tell bad encoding from too much data. */
static bool decode(const uint8_t *text, size_t length, uint8_t *out, size_t capacity,
                   size_t *count) {
    if (length % 4 != 0)
        return false;
    size_t at = 0;
    for (size_t i = 0; i < length; i += 4) {
        size_t pad = 0;
        if (length - i == 4 && text[i + 3] == '=')
            pad = text[i + 2] == '=' ? 2 : 1;
        uint32_t word = 0;
        for (size_t k = 0; k < 4 - pad; k++) {
            int value = sextet(text[i + k]);
            if (value < 0)
                return false;
            word |= (uint32_t)value << (18 - 6 * k);
        }
        /* Canonical form: the bits past the last decoded byte are zero. */
        if (pad == 1 && (word & 0xFF) != 0)
            return false;
        if (pad == 2 && (word & 0xFFFF) != 0)
            return false;
        for (size_t k = 0; k < 3 - pad; k++, at++)
            if (at < capacity)
                out[at] = (uint8_t)((word >> (16 - 8 * k)) & 0xFF);
    }
    MO_ASSERT(at <= length);
    *count = at;
    return true;
}

bool mo_upload_base64(const uint8_t *text, size_t length, uint8_t *out, size_t capacity,
                      size_t *written) {
    MO_ASSERT(text != NULL || length == 0);
    MO_ASSERT(out != NULL || capacity == 0);
    MO_ASSERT(written != NULL);
    size_t count;
    if (!decode(text, length, out, capacity, &count) || count > capacity)
        return false;
    *written = count;
    return true;
}

void mo_upload_init(struct mo_upload *upload, uint8_t *bytes, size_t total) {
    MO_ASSERT(upload != NULL && bytes != NULL);
    MO_ASSERT(total > 0 && total <= MO_LIMIT_BUNDLE_BYTES);
    *upload = (struct mo_upload){.bytes = bytes, .total = total};
}

enum mo_upload_status mo_upload_page(struct mo_upload *upload, uint64_t index, uint64_t offset,
                                     uint64_t total, const uint8_t *text, size_t length) {
    MO_ASSERT(upload != NULL && upload->bytes != NULL);
    MO_ASSERT(upload->total > 0 && upload->total <= MO_LIMIT_BUNDLE_BYTES);
    MO_ASSERT(upload->received <= upload->total);
    MO_ASSERT(upload->next <= MO_LIMIT_BUNDLE_BYTES / MO_LIMIT_UPGRADE_PAGE_BYTES + 1);
    MO_ASSERT(upload->received == upload->total ||
              upload->received == upload->next * MO_LIMIT_UPGRADE_PAGE_BYTES);
    MO_ASSERT(text != NULL || length == 0);
    if (total != upload->total)
        return MO_UPLOAD_TOTAL;
    if (index != upload->next)
        return MO_UPLOAD_ORDER;
    if (offset != upload->received)
        return MO_UPLOAD_ORDER;
    /* Decode into a local page so a refusal leaves the upload's bytes untouched. */
    uint8_t page[MO_LIMIT_UPGRADE_PAGE_BYTES];
    size_t count;
    if (!decode(text, length, page, sizeof page, &count))
        return MO_UPLOAD_ENCODING;
    size_t remaining = upload->total - upload->received;
    if (count == 0)
        return MO_UPLOAD_SIZE;
    if (count > sizeof page)
        return MO_UPLOAD_SIZE;
    if (count > remaining)
        return MO_UPLOAD_SIZE;
    if (count < sizeof page && count != remaining)
        return MO_UPLOAD_SIZE;
    memcpy(upload->bytes + upload->received, page, count);
    upload->received += count;
    upload->next++;
    if (upload->received == upload->total)
        return MO_UPLOAD_COMPLETE;
    return MO_UPLOAD_MORE;
}
