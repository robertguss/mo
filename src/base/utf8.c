#include "base/utf8.h"

bool mo_utf8_decode(const uint8_t *bytes, size_t length, size_t *pos, uint32_t *out) {
    MO_ASSERT((bytes != NULL || length == 0) && pos != NULL && out != NULL);
    if (*pos >= length) {
        return false;
    }
    size_t at = *pos;
    uint32_t scalar = bytes[at++];
    unsigned extra = 0;
    uint32_t minimum = 0;
    if (scalar < 0x80) {
        *pos = at;
        *out = scalar;
        return true;
    }
    if (scalar >= 0xc2 && scalar <= 0xdf) {
        extra = 1;
        minimum = 0x80;
        scalar &= 0x1f;
    } else if (scalar >= 0xe0 && scalar <= 0xef) {
        extra = 2;
        minimum = 0x800;
        scalar &= 0x0f;
    } else if (scalar >= 0xf0 && scalar <= 0xf4) {
        extra = 3;
        minimum = 0x10000;
        scalar &= 0x07;
    } else {
        return false;
    }
    if (extra > length - at) {
        return false;
    }
    for (unsigned i = 0; i < extra; i++) {
        uint32_t byte = bytes[at++];
        if ((byte & 0xc0) != 0x80) {
            return false;
        }
        scalar = (scalar << 6) | (byte & 0x3f);
    }
    if (scalar < minimum || scalar >= 0x110000 || (scalar >= 0xd800 && scalar <= 0xdfff)) {
        return false;
    }
    *pos = at;
    *out = scalar;
    return true;
}

bool mo_utf8_valid(const uint8_t *bytes, size_t length) {
    MO_ASSERT(bytes != NULL || length == 0);
    size_t pos = 0;
    uint32_t scalar;
    while (pos < length) {
        if (!mo_utf8_decode(bytes, length, &pos, &scalar)) {
            return false;
        }
    }
    return true;
}
