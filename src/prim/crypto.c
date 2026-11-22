/* Thin OpenSSL 3 libcrypto wrappers for Mo's pure Crypto primitives (reference 14).
 * No cryptographic algorithms are implemented in C or Mo. Temporary key, password,
 * plaintext and tag buffers are wiped with OPENSSL_cleanse, and OpenSSL's error queue
 * is cleared on every entry and exit. */
#include "prim/internal.h"

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

enum {
    MO_CRYPTO_SHA256_BYTES = 32,
    MO_CRYPTO_GCM_KEY_BYTES = 32,
    MO_CRYPTO_GCM_NONCE_BYTES = 12,
    MO_CRYPTO_GCM_TAG_BYTES = 16,
    MO_CRYPTO_SCRYPT_DK_BYTES = 32,
    MO_CRYPTO_SCRYPT_DK_MAX_BYTES = 64,
    MO_CRYPTO_EVP_CHUNK = 1 << 28,
};

struct bytes_view {
    const uint8_t *bytes;
    size_t length;
};

static MO_MUST_USE struct bytes_view view(struct mo_cell value) {
    struct bytes_view v;
    v.bytes = mo_value_data(value, &v.length);
    return v;
}

static MO_MUST_USE enum mo_prim_status ok_bytes(struct mo_prim_call *c, const uint8_t *bytes,
                                                size_t length, struct mo_cell *out) {
    struct mo_cell payload;
    if (!mo_value_bytes(c->heap, bytes, length, &payload))
        return MO_PRIM_OUT_OF_MEMORY;
    enum mo_prim_status status = mo_prim_result(c, true, payload, out);
    mo_value_drop(c->heap, c->types->items[c->result].cases[0].fields[0].type, payload);
    return status;
}

static MO_MUST_USE enum mo_prim_status ok_text(struct mo_prim_call *c, const char *bytes,
                                               size_t length, struct mo_cell *out) {
    struct mo_cell payload;
    if (!mo_value_text(c->heap, bytes, length, &payload))
        return MO_PRIM_OUT_OF_MEMORY;
    enum mo_prim_status status = mo_prim_result(c, true, payload, out);
    mo_value_drop(c->heap, c->types->items[c->result].cases[0].fields[0].type, payload);
    return status;
}

static MO_MUST_USE enum mo_prim_status err_text(struct mo_prim_call *c, const char *text,
                                                struct mo_cell *out) {
    struct mo_cell payload;
    if (!mo_value_text(c->heap, text, strlen(text), &payload))
        return MO_PRIM_OUT_OF_MEMORY;
    enum mo_prim_status status = mo_prim_result(c, false, payload, out);
    mo_value_drop(c->heap, c->types->items[c->result].cases[1].fields[0].type, payload);
    return status;
}

static MO_MUST_USE enum mo_prim_status err_length(struct mo_prim_call *c, const char *prefix,
                                                  size_t got, struct mo_cell *out) {
    char message[96];
    int n = snprintf(message, sizeof message, "%s:%zu", prefix, got);
    MO_ASSERT(n > 0 && (size_t)n < sizeof message);
    return err_text(c, message, out);
}

static MO_MUST_USE enum mo_prim_status crypto_sha256(struct mo_prim_call *c, struct mo_cell *out) {
    struct bytes_view data = view(c->args[0]);
    const uint8_t *ptr = data.length == 0 ? (const uint8_t *)"" : data.bytes;
    uint8_t digest[MO_CRYPTO_SHA256_BYTES];
    unsigned digest_len = 0;
    ERR_clear_error();
    int ok = EVP_Digest(ptr, data.length, digest, &digest_len, EVP_sha256(), NULL);
    ERR_clear_error();
    MO_ASSERT(ok == 1 && digest_len == MO_CRYPTO_SHA256_BYTES);
    bool made = mo_value_bytes(c->heap, digest, sizeof digest, out);
    OPENSSL_cleanse(digest, sizeof digest);
    return made ? MO_PRIM_RETURNED : MO_PRIM_OUT_OF_MEMORY;
}

static MO_MUST_USE enum mo_prim_status crypto_hmac_sha256(struct mo_prim_call *c,
                                                          struct mo_cell *out) {
    struct bytes_view key = view(c->args[0]);
    struct bytes_view data = view(c->args[1]);
    const uint8_t *key_ptr = key.length == 0 ? (const uint8_t *)"" : key.bytes;
    const uint8_t *data_ptr = data.length == 0 ? (const uint8_t *)"" : data.bytes;
    uint8_t mac[MO_CRYPTO_SHA256_BYTES];
    size_t mac_len = 0;
    ERR_clear_error();
    unsigned char *res = EVP_Q_mac(NULL, "HMAC", NULL, "SHA256", NULL, key_ptr, key.length,
                                   data_ptr, data.length, mac, sizeof mac, &mac_len);
    ERR_clear_error();
    MO_ASSERT(res != NULL && mac_len == MO_CRYPTO_SHA256_BYTES);
    bool made = mo_value_bytes(c->heap, mac, sizeof mac, out);
    OPENSSL_cleanse(mac, sizeof mac);
    return made ? MO_PRIM_RETURNED : MO_PRIM_OUT_OF_MEMORY;
}

static MO_MUST_USE enum mo_prim_status crypto_constant_time_eq(struct mo_prim_call *c,
                                                               struct mo_cell *out) {
    struct bytes_view a = view(c->args[0]);
    struct bytes_view b = view(c->args[1]);
    if (a.length != b.length) {
        *out = mo_cell_bool(false);
        return MO_PRIM_RETURNED;
    }
    if (a.length == 0) {
        *out = mo_cell_bool(true);
        return MO_PRIM_RETURNED;
    }
    *out = mo_cell_bool(CRYPTO_memcmp(a.bytes, b.bytes, a.length) == 0);
    return MO_PRIM_RETURNED;
}

static MO_MUST_USE bool feed_aad(EVP_CIPHER_CTX *ctx, bool encrypt, struct bytes_view aad) {
    size_t pos = 0;
    while (pos < aad.length) {
        size_t step = aad.length - pos;
        if (step > MO_CRYPTO_EVP_CHUNK)
            step = MO_CRYPTO_EVP_CHUNK;
        int out_len = 0;
        int ok = encrypt ? EVP_EncryptUpdate(ctx, NULL, &out_len, aad.bytes + pos, (int)step)
                         : EVP_DecryptUpdate(ctx, NULL, &out_len, aad.bytes + pos, (int)step);
        if (ok != 1)
            return false;
        pos += step;
    }
    return true;
}

static MO_MUST_USE enum mo_prim_status crypto_seal_raw(struct mo_prim_call *c,
                                                       struct mo_cell *out) {
    struct bytes_view key = view(c->args[0]);
    struct bytes_view nonce = view(c->args[1]);
    struct bytes_view plaintext = view(c->args[2]);
    struct bytes_view aad = view(c->args[3]);
    if (key.length != MO_CRYPTO_GCM_KEY_BYTES)
        return err_length(c, "invalid_key_length", key.length, out);
    if (nonce.length != MO_CRYPTO_GCM_NONCE_BYTES)
        return err_length(c, "invalid_nonce_length", nonce.length, out);
    if (plaintext.length > MO_LIMIT_VALUE_BYTES - MO_CRYPTO_GCM_TAG_BYTES) {
        c->reason = "result exceeds MO_LIMIT_VALUE_BYTES (u32 byte length)";
        return MO_PRIM_OUT_OF_MEMORY;
    }
    size_t total = plaintext.length + MO_CRYPTO_GCM_TAG_BYTES;
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, c->heap);
    uint8_t *sealed = mo_scratch_resize(&scratch, NULL, 0, total);
    if (sealed == NULL) {
        mo_scratch_free(&scratch);
        return MO_PRIM_OUT_OF_MEMORY;
    }
    ERR_clear_error();
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (ctx == NULL) {
        ERR_clear_error();
        mo_scratch_free(&scratch);
        return MO_PRIM_OUT_OF_MEMORY;
    }
    bool ok =
        EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, MO_CRYPTO_GCM_NONCE_BYTES, NULL) == 1 &&
        EVP_EncryptInit_ex(ctx, NULL, NULL, key.bytes, nonce.bytes) == 1 &&
        feed_aad(ctx, true, aad);
    size_t out_pos = 0;
    size_t in_pos = 0;
    while (ok && in_pos < plaintext.length) {
        size_t step = plaintext.length - in_pos;
        if (step > MO_CRYPTO_EVP_CHUNK)
            step = MO_CRYPTO_EVP_CHUNK;
        int produced = 0;
        if (EVP_EncryptUpdate(ctx, sealed + out_pos, &produced, plaintext.bytes + in_pos,
                              (int)step) != 1 ||
            produced < 0) {
            ok = false;
            break;
        }
        out_pos += (size_t)produced;
        in_pos += step;
    }
    int final_len = 0;
    if (ok && (EVP_EncryptFinal_ex(ctx, sealed + out_pos, &final_len) != 1 || final_len < 0 ||
               out_pos + (size_t)final_len != plaintext.length ||
               EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, MO_CRYPTO_GCM_TAG_BYTES,
                                   sealed + plaintext.length) != 1))
        ok = false;
    EVP_CIPHER_CTX_free(ctx);
    ERR_clear_error();
    enum mo_prim_status status = ok ? ok_bytes(c, sealed, total, out)
                                    : err_text(c, "encoding_failed:aes-256-gcm failed", out);
    OPENSSL_cleanse(sealed, total);
    mo_scratch_free(&scratch);
    return status;
}

static MO_MUST_USE enum mo_prim_status crypto_open_raw(struct mo_prim_call *c,
                                                       struct mo_cell *out) {
    struct bytes_view key = view(c->args[0]);
    struct bytes_view nonce = view(c->args[1]);
    struct bytes_view ciphertext = view(c->args[2]);
    struct bytes_view aad = view(c->args[3]);
    if (key.length != MO_CRYPTO_GCM_KEY_BYTES)
        return err_length(c, "invalid_key_length", key.length, out);
    if (nonce.length != MO_CRYPTO_GCM_NONCE_BYTES)
        return err_length(c, "invalid_nonce_length", nonce.length, out);
    if (ciphertext.length < MO_CRYPTO_GCM_TAG_BYTES)
        return err_text(c, "decryption_failed", out);
    size_t pt_len = ciphertext.length - MO_CRYPTO_GCM_TAG_BYTES;
    uint8_t tag[MO_CRYPTO_GCM_TAG_BYTES];
    memcpy(tag, ciphertext.bytes + pt_len, sizeof tag);
    size_t buf_len = pt_len == 0 ? 1U : pt_len;
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, c->heap);
    uint8_t *plain = mo_scratch_resize(&scratch, NULL, 0, buf_len);
    if (plain == NULL) {
        OPENSSL_cleanse(tag, sizeof tag);
        mo_scratch_free(&scratch);
        return MO_PRIM_OUT_OF_MEMORY;
    }
    ERR_clear_error();
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (ctx == NULL) {
        ERR_clear_error();
        OPENSSL_cleanse(tag, sizeof tag);
        OPENSSL_cleanse(plain, buf_len);
        mo_scratch_free(&scratch);
        return MO_PRIM_OUT_OF_MEMORY;
    }
    bool ok =
        EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, MO_CRYPTO_GCM_NONCE_BYTES, NULL) == 1 &&
        EVP_DecryptInit_ex(ctx, NULL, NULL, key.bytes, nonce.bytes) == 1 &&
        feed_aad(ctx, false, aad);
    size_t out_pos = 0;
    size_t in_pos = 0;
    while (ok && in_pos < pt_len) {
        size_t step = pt_len - in_pos;
        if (step > MO_CRYPTO_EVP_CHUNK)
            step = MO_CRYPTO_EVP_CHUNK;
        int produced = 0;
        if (EVP_DecryptUpdate(ctx, plain + out_pos, &produced, ciphertext.bytes + in_pos,
                              (int)step) != 1 ||
            produced < 0) {
            ok = false;
            break;
        }
        out_pos += (size_t)produced;
        in_pos += step;
    }
    int final_len = 0;
    /* Verify the 16-byte GCM authentication tag BEFORE releasing any plaintext byte. */
    if (ok && (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, MO_CRYPTO_GCM_TAG_BYTES, tag) != 1 ||
               EVP_DecryptFinal_ex(ctx, plain + out_pos, &final_len) != 1 || final_len < 0 ||
               out_pos + (size_t)final_len != pt_len))
        ok = false;
    EVP_CIPHER_CTX_free(ctx);
    ERR_clear_error();
    enum mo_prim_status status =
        ok ? ok_bytes(c, plain, pt_len, out) : err_text(c, "decryption_failed", out);
    OPENSSL_cleanse(plain, buf_len);
    OPENSSL_cleanse(tag, sizeof tag);
    mo_scratch_free(&scratch);
    return status;
}

/* Standard unpadded Base64 (PHC string format B64: [A-Za-z0-9+/], no '=' padding). */
static MO_MUST_USE size_t phc_b64_encode(const uint8_t *in, size_t len, char *out,
                                         size_t capacity) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t full = len / 3;
    size_t rem = len % 3;
    size_t needed = full * 4 + (rem == 0 ? 0U : rem + 1U);
    MO_ASSERT(needed <= capacity);
    size_t o = 0;
    for (size_t i = 0; i < full * 3; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | (uint32_t)in[i + 2];
        out[o++] = table[(v >> 18) & 0x3fU];
        out[o++] = table[(v >> 12) & 0x3fU];
        out[o++] = table[(v >> 6) & 0x3fU];
        out[o++] = table[v & 0x3fU];
    }
    if (rem == 1) {
        uint32_t b0 = in[full * 3];
        out[o++] = table[b0 >> 2];
        out[o++] = table[(b0 & 0x03U) << 4];
    } else if (rem == 2) {
        uint32_t b0 = in[full * 3];
        uint32_t b1 = in[full * 3 + 1];
        out[o++] = table[b0 >> 2];
        out[o++] = table[((b0 & 0x03U) << 4) | (b1 >> 4)];
        out[o++] = table[(b1 & 0x0fU) << 2];
    }
    MO_ASSERT(o == needed);
    return needed;
}

static MO_MUST_USE int phc_b64_value(uint8_t ch) {
    if (ch >= 'A' && ch <= 'Z')
        return (int)(ch - 'A');
    if (ch >= 'a' && ch <= 'z')
        return (int)(ch - 'a') + 26;
    if (ch >= '0' && ch <= '9')
        return (int)(ch - '0') + 52;
    if (ch == '+')
        return 62;
    if (ch == '/')
        return 63;
    return -1;
}

static MO_MUST_USE bool phc_b64_decode(const uint8_t *in, size_t len, uint8_t *out, size_t capacity,
                                       size_t *out_len) {
    size_t rem = len % 4;
    if (rem == 1)
        return false;
    size_t full = len / 4;
    size_t needed = full * 3 + (rem == 0 ? 0U : rem - 1U);
    if (needed > capacity)
        return false;
    size_t o = 0;
    for (size_t i = 0; i < full * 4; i += 4) {
        int v0 = phc_b64_value(in[i]);
        int v1 = phc_b64_value(in[i + 1]);
        int v2 = phc_b64_value(in[i + 2]);
        int v3 = phc_b64_value(in[i + 3]);
        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0)
            return false;
        uint32_t v =
            ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12) | ((uint32_t)v2 << 6) | (uint32_t)v3;
        out[o++] = (uint8_t)((v >> 16) & 0xffU);
        out[o++] = (uint8_t)((v >> 8) & 0xffU);
        out[o++] = (uint8_t)(v & 0xffU);
    }
    if (rem == 2) {
        int v0 = phc_b64_value(in[full * 4]);
        int v1 = phc_b64_value(in[full * 4 + 1]);
        if (v0 < 0 || v1 < 0 || (v1 & 0x0f) != 0)
            return false;
        out[o++] = (uint8_t)(((uint32_t)v0 << 2) | ((uint32_t)v1 >> 4));
    } else if (rem == 3) {
        int v0 = phc_b64_value(in[full * 4]);
        int v1 = phc_b64_value(in[full * 4 + 1]);
        int v2 = phc_b64_value(in[full * 4 + 2]);
        if (v0 < 0 || v1 < 0 || v2 < 0 || (v2 & 0x03) != 0)
            return false;
        out[o++] = (uint8_t)(((uint32_t)v0 << 2) | ((uint32_t)v1 >> 4));
        out[o++] = (uint8_t)((((uint32_t)v1 & 0x0fU) << 4) | ((uint32_t)v2 >> 2));
    }
    MO_ASSERT(o == needed);
    *out_len = needed;
    return true;
}

static MO_MUST_USE bool parse_phc_uint(const uint8_t *bytes, size_t length, size_t *pos,
                                       uint64_t min_val, uint64_t max_val, uint64_t *out) {
    if (*pos >= length)
        return false;
    uint8_t first = bytes[*pos];
    if (first < '1' || first > '9')
        return false;
    uint64_t value = 0;
    while (*pos < length && bytes[*pos] >= '0' && bytes[*pos] <= '9') {
        uint64_t digit = (uint64_t)(bytes[*pos] - '0');
        if (value > (max_val - digit) / 10U)
            return false;
        value = value * 10U + digit;
        (*pos)++;
    }
    if (value < min_val || value > max_val)
        return false;
    *out = value;
    return true;
}

static MO_MUST_USE bool take_literal(const uint8_t *bytes, size_t length, size_t *pos,
                                     const char *literal) {
    size_t n = strlen(literal);
    if (n > length - *pos || memcmp(bytes + *pos, literal, n) != 0)
        return false;
    *pos += n;
    return true;
}

static MO_MUST_USE enum mo_prim_status crypto_hash_password_raw(struct mo_prim_call *c,
                                                                struct mo_cell *out) {
    struct bytes_view password = view(c->args[0]);
    struct bytes_view salt = view(c->args[1]);
    if (salt.length < MO_LIMIT_CRYPTO_SALT_MIN_BYTES ||
        salt.length > MO_LIMIT_CRYPTO_SALT_MAX_BYTES)
        return err_length(c, "invalid_salt_length", salt.length, out);
    if (password.length > MO_LIMIT_CRYPTO_PASSWORD_BYTES)
        return err_text(c, "encoding_failed:password exceeds 4096 bytes", out);

    char pass_buf[MO_LIMIT_CRYPTO_PASSWORD_BYTES + 1];
    if (password.length > 0)
        memcpy(pass_buf, password.bytes, password.length);
    pass_buf[password.length] = '\0';

    uint8_t dk[MO_CRYPTO_SCRYPT_DK_BYTES];
    uint64_t n_cost = UINT64_C(1) << MO_LIMIT_CRYPTO_SCRYPT_LN_DEFAULT;
    ERR_clear_error();
    int ok = EVP_PBE_scrypt(pass_buf, password.length, salt.bytes, salt.length, n_cost,
                            MO_LIMIT_CRYPTO_SCRYPT_R_DEFAULT, MO_LIMIT_CRYPTO_SCRYPT_P_DEFAULT,
                            MO_LIMIT_CRYPTO_SCRYPT_MAXMEM, dk, sizeof dk);
    ERR_clear_error();
    OPENSSL_cleanse(pass_buf, sizeof pass_buf);
    if (ok != 1) {
        OPENSSL_cleanse(dk, sizeof dk);
        return err_text(c, "encoding_failed:scrypt failed", out);
    }

    char phc[MO_LIMIT_CRYPTO_PHC_BYTES];
    int prefix_len =
        snprintf(phc, sizeof phc, "$scrypt$ln=%d,r=%d,p=%d$", MO_LIMIT_CRYPTO_SCRYPT_LN_DEFAULT,
                 MO_LIMIT_CRYPTO_SCRYPT_R_DEFAULT, MO_LIMIT_CRYPTO_SCRYPT_P_DEFAULT);
    MO_ASSERT(prefix_len > 0 && (size_t)prefix_len < sizeof phc);
    size_t pos = (size_t)prefix_len;
    pos += phc_b64_encode(salt.bytes, salt.length, phc + pos, sizeof phc - pos);
    MO_ASSERT(pos + 1 < sizeof phc);
    phc[pos++] = '$';
    pos += phc_b64_encode(dk, sizeof dk, phc + pos, sizeof phc - pos);
    MO_ASSERT(pos <= MO_LIMIT_CRYPTO_PHC_BYTES);
    OPENSSL_cleanse(dk, sizeof dk);

    enum mo_prim_status status = ok_text(c, phc, pos, out);
    OPENSSL_cleanse(phc, sizeof phc);
    return status;
}

static MO_MUST_USE enum mo_prim_status crypto_verify_password(struct mo_prim_call *c,
                                                              struct mo_cell *out) {
    struct bytes_view password = view(c->args[0]);
    struct bytes_view encoded = view(c->args[1]);
    *out = mo_cell_bool(false);
    if (password.length > MO_LIMIT_CRYPTO_PASSWORD_BYTES || encoded.length == 0 ||
        encoded.length > MO_LIMIT_CRYPTO_PHC_BYTES)
        return MO_PRIM_RETURNED;

    size_t pos = 0;
    uint64_t ln = 0, r = 0, p = 0;
    if (!take_literal(encoded.bytes, encoded.length, &pos, "$scrypt$ln=") ||
        !parse_phc_uint(encoded.bytes, encoded.length, &pos, 1, MO_LIMIT_CRYPTO_SCRYPT_LN_MAX,
                        &ln) ||
        !take_literal(encoded.bytes, encoded.length, &pos, ",r=") ||
        !parse_phc_uint(encoded.bytes, encoded.length, &pos, 1, MO_LIMIT_CRYPTO_SCRYPT_R_MAX, &r) ||
        !take_literal(encoded.bytes, encoded.length, &pos, ",p=") ||
        !parse_phc_uint(encoded.bytes, encoded.length, &pos, 1, MO_LIMIT_CRYPTO_SCRYPT_P_MAX, &p) ||
        !take_literal(encoded.bytes, encoded.length, &pos, "$"))
        return MO_PRIM_RETURNED;

    uint64_t n_cost = UINT64_C(1) << ln;
    if (r > (MO_LIMIT_CRYPTO_SCRYPT_MAXMEM / 128U) / n_cost || p > (UINT64_C(1) << 30) / (128U * r))
        return MO_PRIM_RETURNED;

    size_t salt_start = pos;
    while (pos < encoded.length && encoded.bytes[pos] != '$')
        pos++;
    if (pos >= encoded.length)
        return MO_PRIM_RETURNED;
    size_t salt_b64_len = pos - salt_start;
    pos++;
    size_t hash_start = pos;
    size_t hash_b64_len = encoded.length - hash_start;
    if (hash_b64_len == 0)
        return MO_PRIM_RETURNED;

    uint8_t salt[MO_LIMIT_CRYPTO_SALT_MAX_BYTES];
    uint8_t expected[MO_CRYPTO_SCRYPT_DK_MAX_BYTES];
    size_t salt_len = 0, expected_len = 0;
    if (!phc_b64_decode(encoded.bytes + salt_start, salt_b64_len, salt, sizeof salt, &salt_len) ||
        !phc_b64_decode(encoded.bytes + hash_start, hash_b64_len, expected, sizeof expected,
                        &expected_len) ||
        expected_len == 0) {
        OPENSSL_cleanse(salt, sizeof salt);
        OPENSSL_cleanse(expected, sizeof expected);
        return MO_PRIM_RETURNED;
    }

    char pass_buf[MO_LIMIT_CRYPTO_PASSWORD_BYTES + 1];
    if (password.length > 0)
        memcpy(pass_buf, password.bytes, password.length);
    pass_buf[password.length] = '\0';

    uint8_t derived[MO_CRYPTO_SCRYPT_DK_MAX_BYTES];
    const uint8_t *salt_ptr = salt_len == 0 ? (const uint8_t *)"" : salt;
    ERR_clear_error();
    int ok = EVP_PBE_scrypt(pass_buf, password.length, salt_ptr, salt_len, n_cost, r, p,
                            MO_LIMIT_CRYPTO_SCRYPT_MAXMEM, derived, expected_len);
    ERR_clear_error();
    bool match = ok == 1 && CRYPTO_memcmp(derived, expected, expected_len) == 0;
    OPENSSL_cleanse(pass_buf, sizeof pass_buf);
    OPENSSL_cleanse(salt, sizeof salt);
    OPENSSL_cleanse(expected, sizeof expected);
    OPENSSL_cleanse(derived, sizeof derived);
    *out = mo_cell_bool(match);
    return MO_PRIM_RETURNED;
}

enum mo_prim_status mo_prim_crypto(struct mo_prim_call *c, struct mo_cell *out) {
    if (c->id == MO_PRIM_CRYPTO_SHA256)
        return crypto_sha256(c, out);
    if (c->id == MO_PRIM_CRYPTO_HMAC_SHA256)
        return crypto_hmac_sha256(c, out);
    if (c->id == MO_PRIM_CRYPTO_CONSTANT_TIME_EQ)
        return crypto_constant_time_eq(c, out);
    if (c->id == MO_PRIM_CRYPTO_SEAL_RAW)
        return crypto_seal_raw(c, out);
    if (c->id == MO_PRIM_CRYPTO_OPEN_RAW)
        return crypto_open_raw(c, out);
    if (c->id == MO_PRIM_CRYPTO_HASH_PASSWORD_RAW)
        return crypto_hash_password_raw(c, out);
    if (c->id == MO_PRIM_CRYPTO_VERIFY_PASSWORD)
        return crypto_verify_password(c, out);
    MO_UNREACHABLE();
}
