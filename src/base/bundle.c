#include "base/bundle.h"
#include "base/binary.h"
#include "base/utf8.h"
#include <string.h>

static bool bundle_span(struct mo_binary *in, size_t limit, struct mo_bundle_span *out) {
    uint64_t size;
    const uint8_t *bytes;
    if (!mo_binary_uint(in, 4, &size) || size > limit ||
        !mo_binary_read(in, (size_t)size, &bytes) || !mo_utf8_valid(bytes, (size_t)size))
        return false;
    *out = (struct mo_bundle_span){bytes, (size_t)size};
    return true;
}
static bool component_is(const uint8_t *bytes, size_t length, const char *word) {
    return length == strlen(word) && memcmp(bytes, word, length) == 0;
}
static bool bundle_path(struct mo_binary *in, struct mo_bundle_span *out) {
    if (!bundle_span(in, MO_LIMIT_BUNDLE_PATH_BYTES, out) || out->length == 0)
        return false;
    size_t start = 0;
    for (size_t i = 0; i <= out->length; i++) {
        if (i < out->length && (out->bytes[i] == 0 || out->bytes[i] == '\\'))
            return false;
        if (i < out->length && out->bytes[i] != '/')
            continue;
        size_t n = i - start;
        const uint8_t *part = out->bytes + start;
        if (n == 0 || component_is(part, n, ".") || component_is(part, n, "..") ||
            (start == 0 && (component_is(part, n, "lib") || component_is(part, n, "UpgradeOld") ||
                            component_is(part, n, "UpgradeOld.mo"))))
            return false;
        start = i + 1;
    }
    return true;
}
static int compare(struct mo_bundle_span a, struct mo_bundle_span b) {
    size_t n = a.length < b.length ? a.length : b.length;
    int order = memcmp(a.bytes, b.bytes, n);
    return order != 0 ? order : (a.length > b.length) - (a.length < b.length);
}
bool mo_bundle_read(const uint8_t *bytes, size_t length, struct mo_bundle *out) {
    if (length < 13 || length > MO_LIMIT_BUNDLE_BYTES || memcmp(bytes, "MOSB\1", 5) != 0)
        return false;
    struct mo_binary in = {bytes, length, 5};
    struct mo_bundle next = {0};
    uint64_t count;
    if (!bundle_path(&in, &next.entry) || !mo_binary_uint(&in, 4, &count) || count == 0 ||
        count > MO_LIMIT_BUNDLE_FILES)
        return false;
    next.count = (uint32_t)count;
    bool found = false;
    for (uint32_t i = 0; i < next.count; i++) {
        struct mo_bundle_file *file = &next.files[i];
        if (!bundle_path(&in, &file->path) ||
            !bundle_span(&in, MO_LIMIT_BUNDLE_BYTES, &file->source) ||
            (i != 0 && compare(next.files[i - 1].path, file->path) >= 0))
            return false;
        found = found || compare(next.entry, file->path) == 0;
    }
    if (!found || in.position != length)
        return false;
    *out = next;
    return true;
}
