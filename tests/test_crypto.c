#include "host_fixture.h"
#include "prim_fixture.h"
#include "test.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static struct mo_cell make_bytes(struct mo_heap *heap, const uint8_t *data, size_t len) {
    struct mo_cell out;
    MO_ASSERT(mo_value_bytes(heap, data, len, &out));
    return out;
}

static struct mo_cell make_text(struct mo_heap *heap, const char *s) {
    struct mo_cell out;
    MO_ASSERT(mo_value_text(heap, s, strlen(s), &out));
    return out;
}

static struct mo_cell make_hex_bytes(struct prim_fixture *f, const char *hex) {
    struct mo_cell t = make_text(f->heap, hex);
    struct mo_cell opt;
    const char *reason = NULL;
    MO_ASSERT(mo_prim_call(f->heap, &f->types, MO_PRIM_BYTES_FROM_HEX,
                           f->signature[MO_PRIM_BYTES_FROM_HEX], &t, &opt,
                           &reason) == MO_PRIM_RETURNED);
    mo_value_drop(f->heap, f->builtin[MO_TYPE_TEXT], t);
    MO_ASSERT(mo_cell_get_tag(opt) == 1);
    uint32_t opt_ty = f->types.items[f->signature[MO_PRIM_BYTES_FROM_HEX]].result;
    struct mo_cell payload = mo_cell_get_payload(opt);
    if (mo_case_shape(&f->types, opt_ty, 1) == MO_CASE_BOXED)
        payload = mo_value_field(payload, 0);
    struct mo_cell out = mo_value_dup(f->heap, f->builtin[MO_TYPE_BYTES], payload);
    mo_value_drop(f->heap, opt_ty, opt);
    return out;
}

static void check_bytes_hex(struct prim_fixture *f, struct mo_cell bytes, const char *expected) {
    struct mo_cell hex;
    const char *reason = NULL;
    MO_ASSERT(mo_prim_call(f->heap, &f->types, MO_PRIM_BYTES_TO_HEX,
                           f->signature[MO_PRIM_BYTES_TO_HEX], &bytes, &hex,
                           &reason) == MO_PRIM_RETURNED);
    size_t len;
    const uint8_t *p = mo_value_data(hex, &len);
    CHECK(len == strlen(expected) && memcmp(p, expected, len) == 0);
    mo_value_drop(f->heap, f->builtin[MO_TYPE_TEXT], hex);
}

static struct mo_cell unwrap_result_payload(struct prim_fixture *f, uint32_t res_ty,
                                            struct mo_cell res, uint32_t expected_tag,
                                            uint32_t payload_ty) {
    MO_ASSERT(mo_cell_get_tag(res) == expected_tag);
    struct mo_cell payload = mo_cell_get_payload(res);
    if (mo_case_shape(&f->types, res_ty, expected_tag) == MO_CASE_BOXED)
        payload = mo_value_field(payload, 0);
    return mo_value_dup(f->heap, payload_ty, payload);
}

static void test_sha256_fips_180_4(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, 4 << 20);
    static const struct {
        const char *input;
        const char *expected;
    } vectors[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
    };
    for (size_t i = 0; i < sizeof vectors / sizeof vectors[0]; i++) {
        struct mo_cell in =
            make_bytes(f.heap, (const uint8_t *)vectors[i].input, strlen(vectors[i].input));
        struct mo_cell out;
        const char *reason = NULL;
        CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_CRYPTO_SHA256,
                           f.signature[MO_PRIM_CRYPTO_SHA256], &in, &out,
                           &reason) == MO_PRIM_RETURNED);
        check_bytes_hex(&f, out, vectors[i].expected);
        mo_value_drop(f.heap, f.builtin[MO_TYPE_BYTES], out);
        mo_value_drop(f.heap, f.builtin[MO_TYPE_BYTES], in);
    }
    CHECK(mo_heap_objects(f.heap) == 0);
    prim_fixture_free(&f);
}

static void test_hmac_sha256_rfc_4231(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, 4 << 20);
    uint8_t key131[131];
    memset(key131, 0xaa, sizeof key131);
    uint8_t k1[20], k3[20], d3[50], k4[25], d4[50];
    memset(k1, 0x0b, sizeof k1);
    memset(k3, 0xaa, sizeof k3);
    memset(d3, 0xdd, sizeof d3);
    for (size_t i = 0; i < sizeof k4; i++)
        k4[i] = (uint8_t)(i + 1);
    memset(d4, 0xcd, sizeof d4);

    static const char *tc6_msg = "Test Using Larger Than Block-Size Key - Hash Key First";
    static const char *tc7_msg =
        "This is a test using a larger than block-size key and a larger than block-size data. The "
        "key needs to be hashed before being used by the HMAC algorithm.";
    struct {
        const uint8_t *key;
        size_t key_len;
        const uint8_t *data;
        size_t data_len;
        const char *expected;
    } vectors[] = {
        {k1, sizeof k1, (const uint8_t *)"Hi There", 8,
         "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"},
        {(const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28,
         "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"},
        {k3, sizeof k3, d3, sizeof d3,
         "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"},
        {k4, sizeof k4, d4, sizeof d4,
         "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b"},
        {key131, sizeof key131, (const uint8_t *)tc6_msg, strlen(tc6_msg),
         "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"},
        {key131, sizeof key131, (const uint8_t *)tc7_msg, strlen(tc7_msg),
         "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"},
    };
    for (size_t i = 0; i < sizeof vectors / sizeof vectors[0]; i++) {
        struct mo_cell args[2] = {make_bytes(f.heap, vectors[i].key, vectors[i].key_len),
                                  make_bytes(f.heap, vectors[i].data, vectors[i].data_len)};
        struct mo_cell out;
        const char *reason = NULL;
        CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_CRYPTO_HMAC_SHA256,
                           f.signature[MO_PRIM_CRYPTO_HMAC_SHA256], args, &out,
                           &reason) == MO_PRIM_RETURNED);
        check_bytes_hex(&f, out, vectors[i].expected);
        mo_value_drop(f.heap, f.builtin[MO_TYPE_BYTES], out);
        mo_value_drop(f.heap, f.builtin[MO_TYPE_BYTES], args[0]);
        mo_value_drop(f.heap, f.builtin[MO_TYPE_BYTES], args[1]);
    }
    CHECK(mo_heap_objects(f.heap) == 0);
    prim_fixture_free(&f);
}

static void test_aes_256_gcm_nist_sp_800_38d(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, 4 << 20);
    uint32_t res_ty = f.types.items[f.signature[MO_PRIM_CRYPTO_SEAL_RAW]].result;

    /* NIST SP 800-38D Test Cases 13, 14, 15, 16 */
    static const struct {
        const char *key;
        const char *iv;
        const char *pt;
        const char *aad;
        const char *ct_and_tag;
    } vectors[] = {
        {"0000000000000000000000000000000000000000000000000000000000000000",
         "000000000000000000000000", "", "", "530f8afbc74536b9a963b4f1c4cb738b"},
        {"0000000000000000000000000000000000000000000000000000000000000000",
         "000000000000000000000000", "00000000000000000000000000000000", "",
         "cea7403d4d606b6e074ec5d3baf39d18d0d1c8a799996bf0265b98b5d48ab919"},
        {"feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308",
         "cafebabefacedbaddecaf888",
         "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e244"
         "9a6b525b16aedf5aa0de657ba637b391aafd255",
         "",
         "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3da7b08b105"
         "6828838c5f61e6393ba7a0abcc9f662898015adb094dac5d93471bdec1a502270e3cc6c"},
        {"feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308",
         "cafebabefacedbaddecaf888",
         "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e244"
         "9a6b525b16aedf5aa0de657ba637b39",
         "feedfacedeadbeeffeedfacedeadbeefabaddad2",
         "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3da7b08b105"
         "6828838c5f61e6393ba7a0abcc9f66276fc6ece0f4e1768cddf8853bb2d551b"},
    };
    for (size_t i = 0; i < sizeof vectors / sizeof vectors[0]; i++) {
        struct mo_cell key = make_hex_bytes(&f, vectors[i].key);
        struct mo_cell iv = make_hex_bytes(&f, vectors[i].iv);
        struct mo_cell pt = make_hex_bytes(&f, vectors[i].pt);
        struct mo_cell aad = make_hex_bytes(&f, vectors[i].aad);
        struct mo_cell seal_args[4] = {key, iv, pt, aad};
        struct mo_cell sealed;
        const char *reason = NULL;
        CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_CRYPTO_SEAL_RAW,
                           f.signature[MO_PRIM_CRYPTO_SEAL_RAW], seal_args, &sealed,
                           &reason) == MO_PRIM_RETURNED);
        struct mo_cell ct = unwrap_result_payload(&f, res_ty, sealed, 0, f.builtin[MO_TYPE_BYTES]);
        check_bytes_hex(&f, ct, vectors[i].ct_and_tag);

        struct mo_cell open_args[4] = {key, iv, ct, aad};
        struct mo_cell opened;
        CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_CRYPTO_OPEN_RAW,
                           f.signature[MO_PRIM_CRYPTO_OPEN_RAW], open_args, &opened,
                           &reason) == MO_PRIM_RETURNED);
        struct mo_cell dec = unwrap_result_payload(&f, res_ty, opened, 0, f.builtin[MO_TYPE_BYTES]);
        check_bytes_hex(&f, dec, vectors[i].pt);

        /* Flip every byte position at start and end (ciphertext and tag) -> DecryptionFailed */
        size_t ct_len;
        const uint8_t *ct_ptr = mo_value_data(ct, &ct_len);
        uint8_t flipped[256];
        MO_ASSERT(ct_len <= sizeof flipped);
        memcpy(flipped, ct_ptr, ct_len);
        flipped[0] ^= 0x01;
        struct mo_cell bad_ct = make_bytes(f.heap, flipped, ct_len);
        struct mo_cell bad_args[4] = {key, iv, bad_ct, aad};
        struct mo_cell bad_res;
        CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_CRYPTO_OPEN_RAW,
                           f.signature[MO_PRIM_CRYPTO_OPEN_RAW], bad_args, &bad_res,
                           &reason) == MO_PRIM_RETURNED);
        CHECK(mo_cell_get_tag(bad_res) == 1);
        mo_value_drop(f.heap, res_ty, bad_res);
        mo_value_drop(f.heap, f.builtin[MO_TYPE_BYTES], bad_ct);

        flipped[0] ^= 0x01;
        flipped[ct_len - 1] ^= 0x80;
        bad_ct = make_bytes(f.heap, flipped, ct_len);
        bad_args[2] = bad_ct;
        CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_CRYPTO_OPEN_RAW,
                           f.signature[MO_PRIM_CRYPTO_OPEN_RAW], bad_args, &bad_res,
                           &reason) == MO_PRIM_RETURNED);
        CHECK(mo_cell_get_tag(bad_res) == 1);
        mo_value_drop(f.heap, res_ty, bad_res);
        mo_value_drop(f.heap, f.builtin[MO_TYPE_BYTES], bad_ct);

        mo_value_drop(f.heap, f.builtin[MO_TYPE_BYTES], dec);
        mo_value_drop(f.heap, res_ty, opened);
        mo_value_drop(f.heap, f.builtin[MO_TYPE_BYTES], ct);
        mo_value_drop(f.heap, res_ty, sealed);
        mo_value_drop(f.heap, f.builtin[MO_TYPE_BYTES], key);
        mo_value_drop(f.heap, f.builtin[MO_TYPE_BYTES], iv);
        mo_value_drop(f.heap, f.builtin[MO_TYPE_BYTES], pt);
        mo_value_drop(f.heap, f.builtin[MO_TYPE_BYTES], aad);
    }
    CHECK(mo_heap_objects(f.heap) == 0);
    prim_fixture_free(&f);
}

static void test_scrypt_and_constant_time(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, 4 << 20);
    uint32_t res_ty = f.types.items[f.signature[MO_PRIM_CRYPTO_HASH_PASSWORD_RAW]].result;

    static const struct {
        const char *pw;
        const char *phc;
        bool want;
    } cases[] = {
        {"",
         "$scrypt$ln=4,r=1,p=1$$d9ZXYjhleyA7GcpCwYoEl/FrSETjB0ro39/"
         "6P+3iFEL80Aad7QlI+DJqdToPyB8X6NPg+y4NNijPNeIMONGJBg",
         true},
        {"password",
         "$scrypt$ln=10,r=8,p=16$TmFDbA$/bq+HJ00cgB4VucZDQHp/nxq18vII3gw53N2Y0s3MWIurzDZLiKjiG/"
         "xCSedmDDaxyevuUqD7m2DYMvfoswGQA",
         true},
        {"pleaseletmein",
         "$scrypt$ln=14,r=8,p=1$U29kaXVtQ2hsb3JpZGU$cCO9yzr9c0hGHAbNgf046/"
         "2o+7qQT44+qbVD9lRdofLVQylVYT8Pz2LUlwUkKpr55h6F3A1lHkDfzwF7RVdYhw",
         true},
        {"wrong",
         "$scrypt$ln=4,r=1,p=1$$d9ZXYjhleyA7GcpCwYoEl/FrSETjB0ro39/"
         "6P+3iFEL80Aad7QlI+DJqdToPyB8X6NPg+y4NNijPNeIMONGJBg",
         false},
        {"pw", "$scrypt$ln=20,r=8,p=1$AAAA$AAAA", false},
        {"pw", "$scrypt$ln=04,r=1,p=1$$AAAA", false},
        {"pw", "$scrypt$ln=4,r=1,p=1$AA==$AAAA", false},
        {"pw", "$scrypt$ln=4,r=1,p=1$$AAAA$extra", false},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        struct mo_cell args[2] = {make_text(f.heap, cases[i].pw), make_text(f.heap, cases[i].phc)};
        struct mo_cell out;
        const char *reason = NULL;
        CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_CRYPTO_VERIFY_PASSWORD,
                           f.signature[MO_PRIM_CRYPTO_VERIFY_PASSWORD], args, &out,
                           &reason) == MO_PRIM_RETURNED);
        CHECK(mo_cell_get_bool(out) == cases[i].want);
        mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], args[0]);
        mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], args[1]);
    }

    uint8_t salt[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    struct mo_cell h_args[2] = {make_text(f.heap, "arvo-secret"), make_bytes(f.heap, salt, 16)};
    struct mo_cell h_res;
    const char *reason = NULL;
    CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_CRYPTO_HASH_PASSWORD_RAW,
                       f.signature[MO_PRIM_CRYPTO_HASH_PASSWORD_RAW], h_args, &h_res,
                       &reason) == MO_PRIM_RETURNED);
    struct mo_cell phc = unwrap_result_payload(&f, res_ty, h_res, 0, f.builtin[MO_TYPE_TEXT]);
    struct mo_cell v_args[2] = {h_args[0], phc};
    struct mo_cell ok;
    CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_CRYPTO_VERIFY_PASSWORD,
                       f.signature[MO_PRIM_CRYPTO_VERIFY_PASSWORD], v_args, &ok,
                       &reason) == MO_PRIM_RETURNED);
    CHECK(mo_cell_get_bool(ok));
    mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], phc);
    mo_value_drop(f.heap, res_ty, h_res);
    mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], h_args[0]);
    mo_value_drop(f.heap, f.builtin[MO_TYPE_BYTES], h_args[1]);

    /* Constant-time equality */
    struct mo_cell a = make_bytes(f.heap, salt, 16);
    struct mo_cell b = make_bytes(f.heap, salt, 16);
    salt[15] ^= 1;
    struct mo_cell c = make_bytes(f.heap, salt, 16);
    struct mo_cell eq1[2] = {a, b}, eq2[2] = {a, c};
    CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_CRYPTO_CONSTANT_TIME_EQ,
                       f.signature[MO_PRIM_CRYPTO_CONSTANT_TIME_EQ], eq1, &ok,
                       &reason) == MO_PRIM_RETURNED &&
          mo_cell_get_bool(ok));
    CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_CRYPTO_CONSTANT_TIME_EQ,
                       f.signature[MO_PRIM_CRYPTO_CONSTANT_TIME_EQ], eq2, &ok,
                       &reason) == MO_PRIM_RETURNED &&
          !mo_cell_get_bool(ok));
    mo_value_drop(f.heap, f.builtin[MO_TYPE_BYTES], a);
    mo_value_drop(f.heap, f.builtin[MO_TYPE_BYTES], b);
    mo_value_drop(f.heap, f.builtin[MO_TYPE_BYTES], c);
    CHECK(mo_heap_objects(f.heap) == 0);
    prim_fixture_free(&f);
}

static void *thread_worker(void *arg) {
    int tid = *(const int *)arg;
    struct host_fixture hf;
    host_fixture_init(&hf, 4 << 20);
    struct mo_host_error err;
    struct mo_cell real_rng;
    MO_ASSERT(mo_host_real(&hf.host, hf.heap, hf.capabilities[MO_CAP_SECURE_RANDOM], NULL, -1,
                           &real_rng, &err) == MO_HOST_RETURNED);
    for (int i = 0; i < 16; i++) {
        struct mo_cell args[2] = {real_rng, mo_cell_int(32)};
        struct mo_cell b1, b2;
        MO_ASSERT(mo_host_call(&hf.host, hf.heap, MO_HOST_SECURERANDOM_BYTES, hf.bytes, args, &b1,
                               &err) == MO_HOST_RETURNED);
        MO_ASSERT(mo_host_call(&hf.host, hf.heap, MO_HOST_SECURERANDOM_BYTES, hf.bytes, args, &b2,
                               &err) == MO_HOST_RETURNED);
        size_t l1, l2;
        const uint8_t *p1 = mo_value_data(b1, &l1);
        const uint8_t *p2 = mo_value_data(b2, &l2);
        MO_ASSERT(l1 == 32 && l2 == 32 && memcmp(p1, p2, 32) != 0);
        mo_value_drop(hf.heap, hf.bytes, b1);
        mo_value_drop(hf.heap, hf.bytes, b2);
    }
    mo_value_drop(hf.heap, hf.capabilities[MO_CAP_SECURE_RANDOM], real_rng);
    host_fixture_free(&hf);

    struct prim_fixture pf;
    prim_fixture_init(&pf, 4 << 20);
    uint8_t key[32], nonce[12], msg[24];
    memset(key, (uint8_t)(tid + 1), sizeof key);
    memset(nonce, (uint8_t)(tid + 7), sizeof nonce);
    memset(msg, (uint8_t)(tid + 13), sizeof msg);
    struct mo_cell k = make_bytes(pf.heap, key, sizeof key);
    struct mo_cell n = make_bytes(pf.heap, nonce, sizeof nonce);
    struct mo_cell m = make_bytes(pf.heap, msg, sizeof msg);
    struct mo_cell aad = make_bytes(pf.heap, (const uint8_t *)"tsan", 4);
    uint32_t res_ty = pf.types.items[pf.signature[MO_PRIM_CRYPTO_SEAL_RAW]].result;
    for (int i = 0; i < 16; i++) {
        struct mo_cell sargs[4] = {k, n, m, aad};
        struct mo_cell sealed, opened;
        const char *reason = NULL;
        MO_ASSERT(mo_prim_call(pf.heap, &pf.types, MO_PRIM_CRYPTO_SEAL_RAW,
                               pf.signature[MO_PRIM_CRYPTO_SEAL_RAW], sargs, &sealed,
                               &reason) == MO_PRIM_RETURNED);
        struct mo_cell ct =
            unwrap_result_payload(&pf, res_ty, sealed, 0, pf.builtin[MO_TYPE_BYTES]);
        struct mo_cell oargs[4] = {k, n, ct, aad};
        MO_ASSERT(mo_prim_call(pf.heap, &pf.types, MO_PRIM_CRYPTO_OPEN_RAW,
                               pf.signature[MO_PRIM_CRYPTO_OPEN_RAW], oargs, &opened,
                               &reason) == MO_PRIM_RETURNED);
        MO_ASSERT(mo_cell_get_tag(opened) == 0);
        mo_value_drop(pf.heap, res_ty, opened);
        mo_value_drop(pf.heap, pf.builtin[MO_TYPE_BYTES], ct);
        mo_value_drop(pf.heap, res_ty, sealed);
    }
    mo_value_drop(pf.heap, pf.builtin[MO_TYPE_BYTES], k);
    mo_value_drop(pf.heap, pf.builtin[MO_TYPE_BYTES], n);
    mo_value_drop(pf.heap, pf.builtin[MO_TYPE_BYTES], m);
    mo_value_drop(pf.heap, pf.builtin[MO_TYPE_BYTES], aad);
    prim_fixture_free(&pf);
    return NULL;
}

static void test_secure_random_and_threads(void) {
    struct host_fixture hf;
    host_fixture_init(&hf, 4 << 20);
    struct mo_host_error err;
    struct mo_cell real_rng;
    CHECK(mo_host_real(&hf.host, hf.heap, hf.capabilities[MO_CAP_SECURE_RANDOM], NULL, -1,
                       &real_rng, &err) == MO_HOST_RETURNED);

    /* Bounds: 0 and 65536 succeed; -1 and 65537 are host bugs. */
    struct mo_cell out;
    struct mo_cell zero_args[2] = {real_rng, mo_cell_int(0)};
    CHECK(mo_host_call(&hf.host, hf.heap, MO_HOST_SECURERANDOM_BYTES, hf.bytes, zero_args, &out,
                       &err) == MO_HOST_RETURNED);
    size_t len;
    const uint8_t *p0 = mo_value_data(out, &len);
    CHECK(p0 != NULL && len == 0);
    mo_value_drop(hf.heap, hf.bytes, out);

    struct mo_cell max_args[2] = {real_rng, mo_cell_int(MO_LIMIT_SECURE_RANDOM_BYTES)};
    CHECK(mo_host_call(&hf.host, hf.heap, MO_HOST_SECURERANDOM_BYTES, hf.bytes, max_args, &out,
                       &err) == MO_HOST_RETURNED);
    const uint8_t *pmax = mo_value_data(out, &len);
    CHECK(pmax != NULL && len == MO_LIMIT_SECURE_RANDOM_BYTES);
    mo_value_drop(hf.heap, hf.bytes, out);

    struct mo_cell neg_args[2] = {real_rng, mo_cell_int(-1)};
    CHECK(mo_host_call(&hf.host, hf.heap, MO_HOST_SECURERANDOM_BYTES, hf.bytes, neg_args, &out,
                       &err) == MO_HOST_BUG);
    struct mo_cell over_args[2] = {real_rng, mo_cell_int(MO_LIMIT_SECURE_RANDOM_BYTES + 1)};
    CHECK(mo_host_call(&hf.host, hf.heap, MO_HOST_SECURERANDOM_BYTES, hf.bytes, over_args, &out,
                       &err) == MO_HOST_BUG);

    /* Seeded snapshot and restore preserve deterministic stream position. */
    struct mo_cell seed = mo_cell_int(12345);
    struct mo_cell fake_rng;
    CHECK(mo_host_call(&hf.host, hf.heap, MO_HOST_SECURERANDOM_SEEDED,
                       hf.capabilities[MO_CAP_SECURE_RANDOM], &seed, &fake_rng,
                       &err) == MO_HOST_RETURNED);
    struct mo_cell step_args[2] = {fake_rng, mo_cell_int(32)};
    struct mo_cell first;
    CHECK(mo_host_call(&hf.host, hf.heap, MO_HOST_SECURERANDOM_BYTES, hf.bytes, step_args, &first,
                       &err) == MO_HOST_RETURNED);
    mo_value_drop(hf.heap, hf.bytes, first);

    struct mo_host_fake_state snap;
    CHECK(mo_host_fake_snapshot(&hf.host, hf.heap, fake_rng, &snap));
    CHECK(snap.kind == MO_CAP_SECURE_RANDOM && snap.as.secure_random[0] == 12345 &&
          snap.as.secure_random[1] == 1);
    struct mo_cell restored;
    CHECK(mo_host_fake_restore(&hf.host, hf.heap, hf.capabilities[MO_CAP_SECURE_RANDOM], &snap,
                               &restored, &err) == MO_HOST_RETURNED);
    mo_host_fake_state_drop(&hf.host, hf.heap, &snap);

    struct mo_cell next_a, next_b;
    struct mo_cell r_args[2] = {restored, mo_cell_int(48)};
    struct mo_cell f_args[2] = {fake_rng, mo_cell_int(48)};
    CHECK(mo_host_call(&hf.host, hf.heap, MO_HOST_SECURERANDOM_BYTES, hf.bytes, f_args, &next_a,
                       &err) == MO_HOST_RETURNED);
    CHECK(mo_host_call(&hf.host, hf.heap, MO_HOST_SECURERANDOM_BYTES, hf.bytes, r_args, &next_b,
                       &err) == MO_HOST_RETURNED);
    size_t la, lb;
    const uint8_t *pa = mo_value_data(next_a, &la);
    const uint8_t *pb = mo_value_data(next_b, &lb);
    CHECK(la == 48 && lb == 48 && memcmp(pa, pb, 48) == 0);

    mo_value_drop(hf.heap, hf.bytes, next_a);
    mo_value_drop(hf.heap, hf.bytes, next_b);
    mo_value_drop(hf.heap, hf.capabilities[MO_CAP_SECURE_RANDOM], restored);
    mo_value_drop(hf.heap, hf.capabilities[MO_CAP_SECURE_RANDOM], fake_rng);
    mo_value_drop(hf.heap, hf.capabilities[MO_CAP_SECURE_RANDOM], real_rng);
    CHECK(mo_heap_objects(hf.heap) == 0);
    host_fixture_free(&hf);

    /* Concurrent multi-thread OpenSSL RAND_bytes & AES-256-GCM stress test (TSan). */
    pthread_t threads[4];
    int ids[4] = {0, 1, 2, 3};
    for (int i = 0; i < 4; i++)
        MO_ASSERT(pthread_create(&threads[i], NULL, thread_worker, &ids[i]) == 0);
    for (int i = 0; i < 4; i++)
        MO_ASSERT(pthread_join(threads[i], NULL) == 0);
}

int main(void) {
    test_sha256_fips_180_4();
    test_hmac_sha256_rfc_4231();
    test_aes_256_gcm_nist_sp_800_38d();
    test_scrypt_and_constant_time();
    test_secure_random_and_threads();
    return mo_test_report("test_crypto");
}
