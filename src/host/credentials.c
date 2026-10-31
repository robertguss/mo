#define _POSIX_C_SOURCE 200809L
#include "host/host.h"

#include <stdlib.h>
#include <string.h>

void mo_host_wipe(void *bytes, size_t length) {
    void *(*volatile wipe)(void *, int, size_t) = memset;
    (void)wipe(bytes, 0, length);
}

bool mo_bearer_read(struct mo_bearer *out, const char *environment) {
    MO_ASSERT(out != NULL && environment != NULL);
    mo_host_wipe(out, sizeof *out);
    const char *value = getenv(environment);
    if (value == NULL)
        return false;
    size_t length = strnlen(value, MO_LIMIT_MCP_TOKEN_BYTES + 1);
    if (length == 0 || length > MO_LIMIT_MCP_TOKEN_BYTES)
        return false;
    bool padding = false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c == '=') {
            if (i == 0)
                return false;
            padding = true;
        } else if (padding ||
                   !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                     c == '-' || c == '.' || c == '_' || c == '~' || c == '+' || c == '/'))
            return false;
    }
    memcpy(out->bytes, value, length);
    out->length = length;
    return true;
}

bool mo_bearer_check(const struct mo_bearer *bearer, const uint8_t *bytes, size_t length) {
    MO_ASSERT(bearer != NULL && (bytes != NULL || length == 0));
    uint8_t padded[MO_LIMIT_MCP_TOKEN_BYTES] = {0};
    if (length > 0 && length <= sizeof padded)
        memcpy(padded, bytes, length);
    size_t difference = bearer->length ^ length;
    difference |= bearer->length == 0;
    for (size_t i = 0; i < sizeof padded; i++)
        difference |= (size_t)(bearer->bytes[i] ^ padded[i]);
    mo_host_wipe(padded, sizeof padded);
    return difference == 0;
}
