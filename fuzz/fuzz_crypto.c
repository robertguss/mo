/* Fuzz Crypto primitives (GCM open/seal, PHC scrypt parser/verifier, SHA-256, HMAC,
 * constant_time_eq) and Bytes hex/base64url decoders on arbitrary hostile inputs. */
#include "../tests/prim_fixture.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static bool is_valid_utf8_ascii(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i] >= 0x80)
            return false;
    return true;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2 || size > 4096)
        return 0;
    uint8_t mode = data[0] % 6;
    const uint8_t *payload = data + 1;
    size_t len = size - 1;

    struct prim_fixture f;
    prim_fixture_init(&f, 4 << 20);
    const char *reason = NULL;

    if (mode == 0) {
        /* AES-256-GCM open_raw on hostile ciphertext/tag/key/nonce/aad slices. */
        size_t klen = len >= 32 ? 32 : len;
        size_t rem = len - klen;
        size_t nlen = rem >= 12 ? 12 : rem;
        rem -= nlen;
        size_t alen = rem / 3;
        size_t clen = rem - alen;
        struct mo_cell args[4];
        if (mo_value_bytes(f.heap, payload, klen, &args[0]) &&
            mo_value_bytes(f.heap, payload + klen, nlen, &args[1]) &&
            mo_value_bytes(f.heap, payload + klen + nlen + alen, clen, &args[2]) &&
            mo_value_bytes(f.heap, payload + klen + nlen, alen, &args[3])) {
            struct mo_cell out;
            uint32_t res_ty = f.types.items[f.signature[MO_PRIM_CRYPTO_OPEN_RAW]].result;
            enum mo_prim_status st =
                mo_prim_call(f.heap, &f.types, MO_PRIM_CRYPTO_OPEN_RAW,
                             f.signature[MO_PRIM_CRYPTO_OPEN_RAW], args, &out, &reason);
            if (st == MO_PRIM_RETURNED)
                mo_value_drop(f.heap, res_ty, out);
            for (int i = 0; i < 4; i++)
                mo_value_drop(f.heap, f.builtin[MO_TYPE_BYTES], args[i]);
        }
    } else if (mode == 1) {
        /* AES-256-GCM seal_raw -> open_raw round-trip invariant. */
        uint8_t key[32] = {0}, nonce[12] = {0};
        size_t copy_k = len < 32 ? len : 32;
        memcpy(key, payload, copy_k);
        size_t copy_n = len > 32 ? (len - 32 < 12 ? len - 32 : 12) : 0;
        if (copy_n != 0)
            memcpy(nonce, payload + 32, copy_n);
        size_t pt_off = len > 44 ? 44 : len;
        size_t pt_len = len - pt_off;
        struct mo_cell args[4];
        if (mo_value_bytes(f.heap, key, 32, &args[0]) &&
            mo_value_bytes(f.heap, nonce, 12, &args[1]) &&
            mo_value_bytes(f.heap, payload + pt_off, pt_len, &args[2]) &&
            mo_value_bytes(f.heap, payload, len < 16 ? len : 16, &args[3])) {
            struct mo_cell sealed;
            uint32_t res_ty = f.types.items[f.signature[MO_PRIM_CRYPTO_SEAL_RAW]].result;
            if (mo_prim_call(f.heap, &f.types, MO_PRIM_CRYPTO_SEAL_RAW,
                             f.signature[MO_PRIM_CRYPTO_SEAL_RAW], args, &sealed,
                             &reason) == MO_PRIM_RETURNED) {
                MO_ASSERT(mo_cell_get_tag(sealed) == 0);
                struct mo_cell ct = mo_cell_get_payload(sealed);
                if (mo_case_shape(&f.types, res_ty, 0) == MO_CASE_BOXED)
                    ct = mo_value_field(ct, 0);
                struct mo_cell oargs[4] = {args[0], args[1], ct, args[3]};
                struct mo_cell opened;
                MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_CRYPTO_OPEN_RAW,
                                       f.signature[MO_PRIM_CRYPTO_OPEN_RAW], oargs, &opened,
                                       &reason) == MO_PRIM_RETURNED);
                MO_ASSERT(mo_cell_get_tag(opened) == 0);
                struct mo_cell dec = mo_cell_get_payload(opened);
                if (mo_case_shape(&f.types, res_ty, 0) == MO_CASE_BOXED)
                    dec = mo_value_field(dec, 0);
                size_t dlen;
                const uint8_t *dp = mo_value_data(dec, &dlen);
                MO_ASSERT(dlen == pt_len &&
                          (pt_len == 0 || memcmp(dp, payload + pt_off, pt_len) == 0));
                mo_value_drop(f.heap, res_ty, opened);
                mo_value_drop(f.heap, res_ty, sealed);
            }
            for (int i = 0; i < 4; i++)
                mo_value_drop(f.heap, f.builtin[MO_TYPE_BYTES], args[i]);
        }
    } else if (mode == 2) {
        /* PHC scrypt verify_password on hostile encoded strings. */
        size_t split = payload[0] % (len + 1);
        size_t pw_len = split > 1 ? split - 1 : 0;
        const uint8_t *pw_ptr = payload + 1;
        size_t enc_off = 1 + pw_len;
        size_t enc_len = len > enc_off ? len - enc_off : 0;
        const uint8_t *enc_ptr = payload + enc_off;
        /* Keep inputs valid UTF-8 (ASCII) as required by Mo Text contract. */
        char pw_buf[256], enc_buf[600];
        if (pw_len > sizeof pw_buf)
            pw_len = sizeof pw_buf;
        if (enc_len > sizeof enc_buf)
            enc_len = sizeof enc_buf;
        for (size_t i = 0; i < pw_len; i++)
            pw_buf[i] = (char)(pw_ptr[i] & 0x7f);
        for (size_t i = 0; i < enc_len; i++)
            enc_buf[i] = (char)(enc_ptr[i] & 0x7f);
        struct mo_cell args[2];
        if (mo_value_text(f.heap, pw_buf, pw_len, &args[0]) &&
            mo_value_text(f.heap, enc_buf, enc_len, &args[1])) {
            struct mo_cell out;
            enum mo_prim_status st =
                mo_prim_call(f.heap, &f.types, MO_PRIM_CRYPTO_VERIFY_PASSWORD,
                             f.signature[MO_PRIM_CRYPTO_VERIFY_PASSWORD], args, &out, &reason);
            MO_ASSERT(st == MO_PRIM_RETURNED);
            mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], args[0]);
            mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], args[1]);
        }
    } else if (mode == 3) {
        /* Bytes.from_hex + round-trip Bytes.to_hex. */
        if (is_valid_utf8_ascii(payload, len)) {
            struct mo_cell t;
            if (mo_value_text(f.heap, (const char *)payload, len, &t)) {
                struct mo_cell opt;
                uint32_t opt_ty = f.types.items[f.signature[MO_PRIM_BYTES_FROM_HEX]].result;
                if (mo_prim_call(f.heap, &f.types, MO_PRIM_BYTES_FROM_HEX,
                                 f.signature[MO_PRIM_BYTES_FROM_HEX], &t, &opt,
                                 &reason) == MO_PRIM_RETURNED) {
                    if (mo_cell_get_tag(opt) == 1) {
                        struct mo_cell bs = mo_cell_get_payload(opt);
                        if (mo_case_shape(&f.types, opt_ty, 1) == MO_CASE_BOXED)
                            bs = mo_value_field(bs, 0);
                        struct mo_cell back;
                        MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_BYTES_TO_HEX,
                                               f.signature[MO_PRIM_BYTES_TO_HEX], &bs, &back,
                                               &reason) == MO_PRIM_RETURNED);
                        mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], back);
                    }
                    mo_value_drop(f.heap, opt_ty, opt);
                }
                mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], t);
            }
        }
    } else if (mode == 4) {
        /* Bytes.from_base64url + strict canonical re-encoding check! */
        if (is_valid_utf8_ascii(payload, len)) {
            struct mo_cell t;
            if (mo_value_text(f.heap, (const char *)payload, len, &t)) {
                struct mo_cell opt;
                uint32_t opt_ty = f.types.items[f.signature[MO_PRIM_BYTES_FROM_BASE64URL]].result;
                if (mo_prim_call(f.heap, &f.types, MO_PRIM_BYTES_FROM_BASE64URL,
                                 f.signature[MO_PRIM_BYTES_FROM_BASE64URL], &t, &opt,
                                 &reason) == MO_PRIM_RETURNED) {
                    if (mo_cell_get_tag(opt) == 1) {
                        struct mo_cell bs = mo_cell_get_payload(opt);
                        if (mo_case_shape(&f.types, opt_ty, 1) == MO_CASE_BOXED)
                            bs = mo_value_field(bs, 0);
                        struct mo_cell back;
                        MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_BYTES_TO_BASE64URL,
                                               f.signature[MO_PRIM_BYTES_TO_BASE64URL], &bs, &back,
                                               &reason) == MO_PRIM_RETURNED);
                        /* Strict canonicality: decoded base64url MUST re-encode identically! */
                        size_t blen;
                        const uint8_t *bp = mo_value_data(back, &blen);
                        MO_ASSERT(blen == len && (len == 0 || memcmp(bp, payload, len) == 0));
                        mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], back);
                    }
                    mo_value_drop(f.heap, opt_ty, opt);
                }
                mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], t);
            }
        }
    } else {
        /* SHA-256, HMAC-SHA256, constant_time_eq? */
        size_t mid = len / 2;
        struct mo_cell a, b;
        if (mo_value_bytes(f.heap, payload, mid, &a) &&
            mo_value_bytes(f.heap, payload + mid, len - mid, &b)) {
            struct mo_cell h, mac, eq;
            struct mo_cell pair[2] = {a, b};
            if (mo_prim_call(f.heap, &f.types, MO_PRIM_CRYPTO_SHA256,
                             f.signature[MO_PRIM_CRYPTO_SHA256], &a, &h,
                             &reason) == MO_PRIM_RETURNED)
                mo_value_drop(f.heap, f.builtin[MO_TYPE_BYTES], h);
            if (mo_prim_call(f.heap, &f.types, MO_PRIM_CRYPTO_HMAC_SHA256,
                             f.signature[MO_PRIM_CRYPTO_HMAC_SHA256], pair, &mac,
                             &reason) == MO_PRIM_RETURNED)
                mo_value_drop(f.heap, f.builtin[MO_TYPE_BYTES], mac);
            MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_CRYPTO_CONSTANT_TIME_EQ,
                                   f.signature[MO_PRIM_CRYPTO_CONSTANT_TIME_EQ], pair, &eq,
                                   &reason) == MO_PRIM_RETURNED);
            bool expect_eq =
                (mid == len - mid) && (mid == 0 || memcmp(payload, payload + mid, mid) == 0);
            MO_ASSERT(mo_cell_get_bool(eq) == expect_eq);
            mo_value_drop(f.heap, f.builtin[MO_TYPE_BYTES], a);
            mo_value_drop(f.heap, f.builtin[MO_TYPE_BYTES], b);
        }
    }

    MO_ASSERT(mo_heap_objects(f.heap) == 0);
    prim_fixture_free(&f);
    return 0;
}
