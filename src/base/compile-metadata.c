#include "base/compile-metadata.h"
#include "base/binary.h"
#include "base/utf8.h"
#include <string.h>
bool mo_compile_text_read(struct mo_binary *in, size_t maximum, struct mo_bundle_span *out) {
    uint64_t n;
    const uint8_t *bytes;
    if (!mo_binary_uint(in, 4, &n) || n > maximum || !mo_binary_read(in, (size_t)n, &bytes) ||
        !mo_utf8_valid(bytes, (size_t)n) || memchr(bytes, 0, (size_t)n) != NULL)
        return false;
    *out = (struct mo_bundle_span){bytes, (size_t)n};
    return true;
}
static int span_order(struct mo_bundle_span a, struct mo_bundle_span b) {
    size_t n = a.length < b.length ? a.length : b.length;
    int order = memcmp(a.bytes, b.bytes, n);
    return order != 0 ? order : (a.length > b.length) - (a.length < b.length);
}
bool mo_compile_files_read(struct mo_binary *in, uint32_t count, struct mo_bundle_file *files) {
    if (count > MO_LIMIT_BUNDLE_FILES)
        return false;
    for (uint32_t i = 0; i < count; i++) {
        struct mo_bundle_file *f = &files[i];
        if (!mo_compile_text_read(in, MO_LIMIT_BUNDLE_PATH_BYTES, &f->path) ||
            f->path.length < 13 || memcmp(f->path.bytes, "UpgradeOld.", 11) != 0 ||
            memcmp(f->path.bytes + f->path.length - 3, ".mo", 3) != 0 ||
            !mo_compile_text_read(in, MO_LIMIT_BUNDLE_BYTES, &f->source) ||
            (i != 0 && span_order(files[i - 1].path, f->path) >= 0))
            return false;
        for (size_t j = 0; j < f->path.length; j++) {
            uint8_t c = f->path.bytes[j];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '.') ||
                (j != 0 && c == '.' && f->path.bytes[j - 1] == '.'))
                return false;
        }
    }
    return true;
}
bool mo_compile_handler_read(struct mo_binary *input, bool timer, struct mo_compile_handler *out) {
    struct mo_binary in = *input;
    struct mo_compile_handler next = {0};
    const uint8_t *digest;
    if (!mo_compile_text_read(&in, MO_LIMIT_BUNDLE_BYTES, &next.name) || next.name.length == 0)
        return false;
    for (size_t i = 0; i < next.name.length; i++)
        if (next.name.bytes[i] < 32 || next.name.bytes[i] == '"' || next.name.bytes[i] == '\\')
            return false;
    if (timer) {
        if (!mo_binary_uint(&in, 8, &next.interval) || next.interval == 0 ||
            next.interval > INT64_MAX)
            return false;
    } else {
        if (!mo_binary_read(&in, 32, &digest))
            return false;
        memcpy(next.digest, digest, 32);
    }
    *input = in;
    *out = next;
    return true;
}
bool mo_compile_metadata_read(const uint8_t *bytes, size_t length,
                              struct mo_compile_metadata *out) {
    if (length < 25 || length > MO_LIMIT_BUNDLE_BYTES || memcmp(bytes, "MOCS\1", 5) != 0)
        return false;
    struct mo_binary in = {bytes, length, 5};
    struct mo_compile_metadata next = {0};
    uint64_t n;
    const uint8_t *schema;
    if (!mo_compile_text_read(&in, MO_LIMIT_BUNDLE_BYTES, &next.token) || next.token.length == 0 ||
        !mo_binary_uint(&in, 4, &n) || n == 0 || !mo_binary_read(&in, (size_t)n, &schema))
        return false;
    next.schema = (struct mo_bundle_span){schema, (size_t)n};
    if (!mo_binary_uint(&in, 4, &n) || n > MO_LIMIT_BUNDLE_FILES)
        return false;
    next.type_count = (uint32_t)n;
    if (!mo_compile_files_read(&in, next.type_count, next.types))
        return false;
    for (unsigned timer = 0; timer < 2; timer++) {
        if (!mo_binary_uint(&in, 4, &n) || n > MO_LIMIT_SERVICE_HANDLERS)
            return false;
        size_t start = in.position;
        struct mo_compile_handler previous = {0};
        for (uint32_t i = 0; i < (uint32_t)n; i++) {
            struct mo_compile_handler handler;
            if (!mo_compile_handler_read(&in, timer != 0, &handler) ||
                (i != 0 && span_order(previous.name, handler.name) >= 0))
                return false;
            previous = handler;
        }
        struct mo_bundle_span section = {bytes + start, in.position - start};
        if (timer) {
            next.timer_count = (uint32_t)n;
            next.timers = section;
        } else {
            next.tool_count = (uint32_t)n;
            next.tools = section;
        }
    }
    if (in.position != length)
        return false;
    *out = next;
    return true;
}
