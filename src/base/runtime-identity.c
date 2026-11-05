#define _POSIX_C_SOURCE 200809L
#include "base/runtime-identity.h"
#include "base/limits.h"
#include "base/sha256.h"
#include <stdio.h>
#include <string.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifndef MO_VERSION
#define MO_VERSION "0.0.0"
#endif
static bool initialized, available;
static uint8_t executable_digest[32];
void mo_runtime_identity_init(void) {
    if (initialized)
        return;
    initialized = true;
    const char *path = NULL;
#ifdef __linux__
    path = "/proc/self/exe";
#elif defined(__APPLE__)
    char executable[MO_LIMIT_PATH_SIZE];
    uint32_t size = sizeof executable;
    if (_NSGetExecutablePath(executable, &size) == 0)
        path = executable;
#endif
    if (path == NULL)
        return;
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return;
    struct mo_sha256 hash;
    mo_sha256_init(&hash);
    uint8_t bytes[8192];
    size_t count;
    while ((count = fread(bytes, 1, sizeof bytes, file)) != 0)
        mo_sha256_update(&hash, bytes, count);
    bool ok = ferror(file) == 0;
    if (fclose(file) != 0)
        ok = false;
    if (ok) {
        mo_sha256_finish(&hash, executable_digest);
        available = true;
    }
}
bool mo_runtime_identity(uint8_t digest[32]) {
    MO_ASSERT(digest != NULL);
    if (!available)
        return false;
    memcpy(digest, executable_digest, sizeof executable_digest);
    return true;
}
const char *mo_runtime_build_label(void) {
#ifdef __clang__
    return "mo " MO_VERSION "; clang " __clang_version__ "; " MO_BUILD_CONFIGURATION;
#else
    return "mo " MO_VERSION "; gcc " __VERSION__ "; " MO_BUILD_CONFIGURATION;
#endif
}
