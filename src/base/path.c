#include "base/path.h"

static MO_MUST_USE bool capital(uint8_t c) {
    return c >= 'A' && c <= 'Z';
}

static MO_MUST_USE bool lower(uint8_t c) {
    return c >= 'a' && c <= 'z';
}

static MO_MUST_USE bool digit(uint8_t c) {
    return c >= '0' && c <= '9';
}

bool mo_module_path(const uint8_t *name, size_t length, char out[MO_LIMIT_PATH_SIZE + 1]) {
    MO_ASSERT(name != NULL && out != NULL);
    if (length == 0 || length > MO_LIMIT_PATH_SIZE)
        return false;
    size_t n = 0;
    bool start = true;
    for (size_t i = 0; i < length; i++) {
        uint8_t c = name[i];
        if (c == '.') {
            if (start || i + 1 == length || n == MO_LIMIT_PATH_SIZE)
                return false;
            out[n++] = '/';
            start = true;
            continue;
        }
        if ((start && !capital(c)) || (!capital(c) && !lower(c) && !digit(c)))
            return false;
        bool word =
            !start && capital(c) &&
            (lower(name[i - 1]) || digit(name[i - 1]) || (i + 1 < length && lower(name[i + 1])));
        if (word) {
            if (n == MO_LIMIT_PATH_SIZE)
                return false;
            out[n++] = '-';
        }
        if (n == MO_LIMIT_PATH_SIZE)
            return false;
        out[n++] = (char)(capital(c) ? c + ('a' - 'A') : c);
        start = false;
    }
    if (n > MO_LIMIT_PATH_SIZE - 3)
        return false;
    out[n++] = '.';
    out[n++] = 'm';
    out[n++] = 'o';
    out[n] = '\0';
    return true;
}
