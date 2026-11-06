#include "base/runtime-identity.h"
#include "base/sha256.h"
#include "runtime_entry.h"
#include "test.h"
#include <string.h>

static void vector(const char *input, const char *expected) {
    struct mo_sha256 hash;
    uint8_t digest[32];
    char hex[65];
    const char digits[] = "0123456789abcdef";
    mo_sha256_init(&hash);
    for (size_t i = 0; i < strlen(input); i++)
        mo_sha256_update(&hash, input + i, 1);
    mo_sha256_finish(&hash, digest);
    for (size_t i = 0; i < 32; i++) {
        hex[2 * i] = digits[digest[i] >> 4];
        hex[2 * i + 1] = digits[digest[i] & 15];
    }
    hex[64] = 0;
    CHECK(strcmp(hex, expected) == 0);
}
int main(int argc, char **argv) {
    if (argc == 2) {
        uint8_t digest[32];
        if (strcmp(argv[1], "identity") == 0) {
            CHECK(!mo_runtime_identity(digest));
            char command[] = "mo", version[] = "--version";
            char *arguments[] = {command, version, NULL};
            CHECK(mo_test_runtime_main(2, arguments) == 0);
            CHECK(mo_runtime_identity(digest));
            mo_runtime_identity_init();
            CHECK(strstr(mo_runtime_build_label(), "mo ") != NULL);
            CHECK(strstr(mo_runtime_build_label(), MO_BUILD_CONFIGURATION) != NULL);
            CHECK(puts(mo_runtime_build_label()) >= 0);
        } else {
            struct mo_sha256 hash;
            mo_sha256_init(&hash);
            uint8_t bytes[137];
            size_t count;
            while ((count = fread(bytes, 1, sizeof bytes, stdin)) != 0)
                mo_sha256_update(&hash, bytes, count);
            CHECK(ferror(stdin) == 0);
            mo_sha256_finish(&hash, digest);
        }
        for (size_t i = 0; i < sizeof digest; i++)
            CHECK(printf("%02x", (unsigned)digest[i]) == 2);
        CHECK(putchar('\n') != EOF);
        return mo_test_failures != 0;
    }

    char command[] = "mo", check[] = "check", source[] = "examples/greet.mo";
    char *check_arguments[] = {command, check, source, NULL};
    CHECK(mo_test_runtime_main(3, check_arguments) == 0);
    uint8_t unused_digest[32];
    CHECK(!mo_runtime_identity(unused_digest));
    vector("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    vector("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    vector("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    return mo_test_report("sha256");
}
