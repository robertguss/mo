use aes_gcm::Aes256Gcm;
use aes_gcm::aead::{AeadInOut, KeyInit, Nonce};
use hmac::{Hmac, Mac};
use mo_bytecode::types::TypeTable;
use mo_def::PrimId;
use mo_def::limits::{
    MO_LIMIT_CRYPTO_PASSWORD_BYTES, MO_LIMIT_CRYPTO_PHC_BYTES, MO_LIMIT_CRYPTO_SALT_MAX_BYTES,
    MO_LIMIT_CRYPTO_SALT_MIN_BYTES, MO_LIMIT_CRYPTO_SCRYPT_LN_DEFAULT,
    MO_LIMIT_CRYPTO_SCRYPT_LN_MAX, MO_LIMIT_CRYPTO_SCRYPT_MAXMEM, MO_LIMIT_CRYPTO_SCRYPT_P_DEFAULT,
    MO_LIMIT_CRYPTO_SCRYPT_P_MAX, MO_LIMIT_CRYPTO_SCRYPT_R_DEFAULT, MO_LIMIT_CRYPTO_SCRYPT_R_MAX,
    MO_LIMIT_VALUE_BYTES,
};
use mo_heap::{Heap, ScratchGuard, Value};
use ring::hmac as ring_hmac;
use ring::signature as ring_sig;
use sha2::{Digest, Sha256};
use subtle::ConstantTimeEq;
use zeroize::Zeroizing;

use crate::PrimError;

const GCM_KEY_BYTES: usize = 32;
const GCM_NONCE_BYTES: usize = 12;
const GCM_TAG_BYTES: usize = 16;
const SCRYPT_DK_BYTES: usize = 32;
const SCRYPT_DK_MAX_BYTES: usize = 64;

type HmacSha256 = Hmac<Sha256>;

fn ok_bytes(
    heap: &Heap,
    types: &TypeTable,
    result_ty: u32,
    bytes: Vec<u8>,
) -> Result<Value, PrimError> {
    let payload = heap.alloc_bytes(bytes)?;
    Ok(heap.make_result(types, result_ty, Ok(payload))?)
}

fn ok_text(
    heap: &Heap,
    types: &TypeTable,
    result_ty: u32,
    text: String,
) -> Result<Value, PrimError> {
    let payload = heap.alloc_text(text)?;
    Ok(heap.make_result(types, result_ty, Ok(payload))?)
}

fn ok_bool(
    heap: &Heap,
    types: &TypeTable,
    result_ty: u32,
    verified: bool,
) -> Result<Value, PrimError> {
    Ok(heap.make_result(types, result_ty, Ok(Value::Bool(verified)))?)
}

fn err_text(
    heap: &Heap,
    types: &TypeTable,
    result_ty: u32,
    text: String,
) -> Result<Value, PrimError> {
    let payload = heap.alloc_text(text)?;
    Ok(heap.make_result(types, result_ty, Err(payload))?)
}

fn err_length(
    heap: &Heap,
    types: &TypeTable,
    result_ty: u32,
    prefix: &str,
    got: usize,
) -> Result<Value, PrimError> {
    err_text(heap, types, result_ty, format!("{prefix}:{got}"))
}

fn crypto_sha256(heap: &Heap, data: &[u8]) -> Result<Value, PrimError> {
    let digest: Zeroizing<[u8; 32]> = Zeroizing::new(Sha256::digest(data).into());
    Ok(heap.alloc_bytes(digest.as_slice().to_vec())?)
}

fn crypto_hmac_sha256(heap: &Heap, key: &[u8], data: &[u8]) -> Result<Value, PrimError> {
    #[expect(clippy::expect_used, reason = "HMAC accepts any key length")]
    let mut mac = HmacSha256::new_from_slice(key).expect("HMAC accepts any key length");
    mac.update(data);
    let tag: Zeroizing<[u8; 32]> = Zeroizing::new(mac.finalize().into_bytes().into());
    Ok(heap.alloc_bytes(tag.as_slice().to_vec())?)
}

fn crypto_hmac_sha1(heap: &Heap, key: &[u8], data: &[u8]) -> Result<Value, PrimError> {
    let mac_key = ring_hmac::Key::new(ring_hmac::HMAC_SHA1_FOR_LEGACY_USE_ONLY, key);
    let sig = ring_hmac::sign(&mac_key, data);
    let tag: Zeroizing<Vec<u8>> = Zeroizing::new(sig.as_ref().to_vec());
    Ok(heap.alloc_bytes(tag.as_slice().to_vec())?)
}

fn crypto_verify_signature_raw(
    heap: &Heap,
    types: &TypeTable,
    result_ty: u32,
    alg: &str,
    public_key: &[u8],
    message: &[u8],
    signature: &[u8],
) -> Result<Value, PrimError> {
    let verifier: &'static dyn ring_sig::VerificationAlgorithm = match alg {
        "ed25519" => &ring_sig::ED25519,
        "es256_asn1" => &ring_sig::ECDSA_P256_SHA256_ASN1,
        "es256_fixed" => &ring_sig::ECDSA_P256_SHA256_FIXED,
        "rs256" => &ring_sig::RSA_PKCS1_2048_8192_SHA256,
        _ => {
            return err_text(
                heap,
                types,
                result_ty,
                format!("unsupported_algorithm:{alg}"),
            );
        }
    };
    let verified = ring_sig::UnparsedPublicKey::new(verifier, public_key)
        .verify(message, signature)
        .is_ok();
    ok_bool(heap, types, result_ty, verified)
}

fn crypto_constant_time_eq(a: &[u8], b: &[u8]) -> Value {
    if a.len() != b.len() {
        return Value::Bool(false);
    }
    if a.is_empty() {
        return Value::Bool(true);
    }
    Value::Bool(bool::from(a.ct_eq(b)))
}

fn crypto_seal_raw(
    heap: &Heap,
    types: &TypeTable,
    result_ty: u32,
    key: &[u8],
    nonce_bytes: &[u8],
    plaintext: &[u8],
    aad: &[u8],
) -> Result<Value, PrimError> {
    if key.len() != GCM_KEY_BYTES {
        return err_length(heap, types, result_ty, "invalid_key_length", key.len());
    }
    if nonce_bytes.len() != GCM_NONCE_BYTES {
        return err_length(
            heap,
            types,
            result_ty,
            "invalid_nonce_length",
            nonce_bytes.len(),
        );
    }
    if (plaintext.len() as u64) > MO_LIMIT_VALUE_BYTES - (GCM_TAG_BYTES as u64) {
        return Err(PrimError::OutOfMemory(Some(
            "result exceeds MO_LIMIT_VALUE_BYTES (u32 byte length)",
        )));
    }
    let total = plaintext.len() + GCM_TAG_BYTES;
    let mut scratch = ScratchGuard::new(heap.budget());
    scratch.charge(total)?;

    let Ok(cipher) = Aes256Gcm::new_from_slice(key) else {
        return err_length(heap, types, result_ty, "invalid_key_length", key.len());
    };
    let Ok(nonce) = Nonce::<Aes256Gcm>::try_from(nonce_bytes) else {
        return err_length(
            heap,
            types,
            result_ty,
            "invalid_nonce_length",
            nonce_bytes.len(),
        );
    };

    let mut sealed = Zeroizing::new(Vec::with_capacity(total));
    sealed.extend_from_slice(plaintext);
    let encrypted = cipher.encrypt_in_place(&nonce, aad, &mut *sealed);
    let res = match encrypted {
        Ok(()) => ok_bytes(heap, types, result_ty, sealed.as_slice().to_vec()),
        Err(_) => err_text(
            heap,
            types,
            result_ty,
            "encoding_failed:aes-256-gcm failed".to_string(),
        ),
    };
    drop(sealed);
    drop(scratch);
    res
}

fn crypto_open_raw(
    heap: &Heap,
    types: &TypeTable,
    result_ty: u32,
    key: &[u8],
    nonce_bytes: &[u8],
    ciphertext: &[u8],
    aad: &[u8],
) -> Result<Value, PrimError> {
    if key.len() != GCM_KEY_BYTES {
        return err_length(heap, types, result_ty, "invalid_key_length", key.len());
    }
    if nonce_bytes.len() != GCM_NONCE_BYTES {
        return err_length(
            heap,
            types,
            result_ty,
            "invalid_nonce_length",
            nonce_bytes.len(),
        );
    }
    if ciphertext.len() < GCM_TAG_BYTES {
        return err_text(heap, types, result_ty, "decryption_failed".to_string());
    }
    let pt_len = ciphertext.len() - GCM_TAG_BYTES;
    let mut scratch = ScratchGuard::new(heap.budget());
    scratch.charge(ciphertext.len().max(1))?;

    let Ok(cipher) = Aes256Gcm::new_from_slice(key) else {
        return err_length(heap, types, result_ty, "invalid_key_length", key.len());
    };
    let Ok(nonce) = Nonce::<Aes256Gcm>::try_from(nonce_bytes) else {
        return err_length(
            heap,
            types,
            result_ty,
            "invalid_nonce_length",
            nonce_bytes.len(),
        );
    };

    let mut buf = Zeroizing::new(ciphertext.to_vec());
    // `decrypt_in_place` verifies the 16-byte GCM tag before succeeding; on error
    // `Zeroizing` wipes `buf` on drop without releasing any plaintext byte.
    let decrypted = cipher.decrypt_in_place(&nonce, aad, &mut *buf);
    let res = match decrypted {
        Ok(()) => {
            debug_assert_eq!(buf.len(), pt_len);
            ok_bytes(heap, types, result_ty, buf.as_slice().to_vec())
        }
        Err(_) => err_text(heap, types, result_ty, "decryption_failed".to_string()),
    };
    drop(buf);
    drop(scratch);
    res
}

/// Standard unpadded Base64 (PHC string format B64: `[A-Za-z0-9+/]`, no `'='` padding).
fn phc_b64_encode(input: &[u8], out: &mut String) {
    const TABLE: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let full = input.len() / 3;
    let rem = input.len() % 3;
    for chunk in input.as_chunks::<3>().0 {
        let v = (u32::from(chunk[0]) << 16) | (u32::from(chunk[1]) << 8) | u32::from(chunk[2]);
        out.push(TABLE[((v >> 18) & 0x3f) as usize] as char);
        out.push(TABLE[((v >> 12) & 0x3f) as usize] as char);
        out.push(TABLE[((v >> 6) & 0x3f) as usize] as char);
        out.push(TABLE[(v & 0x3f) as usize] as char);
    }
    if rem == 1 {
        let b0 = u32::from(input[full * 3]);
        out.push(TABLE[(b0 >> 2) as usize] as char);
        out.push(TABLE[((b0 & 0x03) << 4) as usize] as char);
    } else if rem == 2 {
        let b0 = u32::from(input[full * 3]);
        let b1 = u32::from(input[full * 3 + 1]);
        out.push(TABLE[(b0 >> 2) as usize] as char);
        out.push(TABLE[(((b0 & 0x03) << 4) | (b1 >> 4)) as usize] as char);
        out.push(TABLE[((b1 & 0x0f) << 2) as usize] as char);
    }
}

fn phc_b64_value(ch: u8) -> Option<u32> {
    match ch {
        b'A'..=b'Z' => Some(u32::from(ch - b'A')),
        b'a'..=b'z' => Some(u32::from(ch - b'a') + 26),
        b'0'..=b'9' => Some(u32::from(ch - b'0') + 52),
        b'+' => Some(62),
        b'/' => Some(63),
        _ => None,
    }
}

fn phc_b64_decode(input: &[u8], out: &mut [u8]) -> Option<usize> {
    let rem = input.len() % 4;
    if rem == 1 {
        return None;
    }
    let full = input.len() / 4;
    let needed = full * 3 + if rem == 0 { 0 } else { rem - 1 };
    if needed > out.len() {
        return None;
    }
    let mut o = 0usize;
    for chunk in input.as_chunks::<4>().0 {
        let v0 = phc_b64_value(chunk[0])?;
        let v1 = phc_b64_value(chunk[1])?;
        let v2 = phc_b64_value(chunk[2])?;
        let v3 = phc_b64_value(chunk[3])?;
        let v = (v0 << 18) | (v1 << 12) | (v2 << 6) | v3;
        out[o] = ((v >> 16) & 0xff) as u8;
        out[o + 1] = ((v >> 8) & 0xff) as u8;
        out[o + 2] = (v & 0xff) as u8;
        o += 3;
    }
    if rem == 2 {
        let v0 = phc_b64_value(input[full * 4])?;
        let v1 = phc_b64_value(input[full * 4 + 1])?;
        if (v1 & 0x0f) != 0 {
            return None;
        }
        out[o] = ((v0 << 2) | (v1 >> 4)) as u8;
        o += 1;
    } else if rem == 3 {
        let v0 = phc_b64_value(input[full * 4])?;
        let v1 = phc_b64_value(input[full * 4 + 1])?;
        let v2 = phc_b64_value(input[full * 4 + 2])?;
        if (v2 & 0x03) != 0 {
            return None;
        }
        out[o] = ((v0 << 2) | (v1 >> 4)) as u8;
        out[o + 1] = (((v1 & 0x0f) << 4) | (v2 >> 2)) as u8;
        o += 2;
    }
    debug_assert_eq!(o, needed);
    Some(needed)
}

fn parse_phc_uint(bytes: &[u8], pos: &mut usize, min_val: u64, max_val: u64) -> Option<u64> {
    let first = *bytes.get(*pos)?;
    if !(b'1'..=b'9').contains(&first) {
        return None;
    }
    let mut value = 0u64;
    while let Some(&b) = bytes.get(*pos) {
        if !b.is_ascii_digit() {
            break;
        }
        let digit = u64::from(b - b'0');
        if value > (max_val - digit) / 10 {
            return None;
        }
        value = value * 10 + digit;
        *pos += 1;
    }
    if value < min_val || value > max_val {
        return None;
    }
    Some(value)
}

fn take_literal(bytes: &[u8], pos: &mut usize, literal: &[u8]) -> bool {
    if bytes.get(*pos..).is_some_and(|s| s.starts_with(literal)) {
        *pos += literal.len();
        true
    } else {
        false
    }
}

fn crypto_hash_password_raw(
    heap: &Heap,
    types: &TypeTable,
    result_ty: u32,
    password: &[u8],
    salt: &[u8],
) -> Result<Value, PrimError> {
    if (salt.len() as u64) < MO_LIMIT_CRYPTO_SALT_MIN_BYTES
        || (salt.len() as u64) > MO_LIMIT_CRYPTO_SALT_MAX_BYTES
    {
        return err_length(heap, types, result_ty, "invalid_salt_length", salt.len());
    }
    if (password.len() as u64) > MO_LIMIT_CRYPTO_PASSWORD_BYTES {
        return err_text(
            heap,
            types,
            result_ty,
            "encoding_failed:password exceeds 4096 bytes".to_string(),
        );
    }

    let ln = MO_LIMIT_CRYPTO_SCRYPT_LN_DEFAULT as u8;
    let r = MO_LIMIT_CRYPTO_SCRYPT_R_DEFAULT as u32;
    let p = MO_LIMIT_CRYPTO_SCRYPT_P_DEFAULT as u32;
    let params = match scrypt::Params::new(ln, r, p) {
        Ok(params) => params,
        Err(_) => {
            return err_text(
                heap,
                types,
                result_ty,
                "encoding_failed:scrypt failed".to_string(),
            );
        }
    };

    let mut dk = Zeroizing::new([0u8; SCRYPT_DK_BYTES]);
    if scrypt::scrypt(password, salt, &params, &mut *dk).is_err() {
        return err_text(
            heap,
            types,
            result_ty,
            "encoding_failed:scrypt failed".to_string(),
        );
    }

    let mut phc = Zeroizing::new(String::with_capacity(MO_LIMIT_CRYPTO_PHC_BYTES as usize));
    phc.push_str(&format!("$scrypt$ln={ln},r={r},p={p}$"));
    phc_b64_encode(salt, &mut phc);
    phc.push('$');
    phc_b64_encode(&*dk, &mut phc);
    debug_assert!((phc.len() as u64) <= MO_LIMIT_CRYPTO_PHC_BYTES);
    drop(dk);

    let out = phc.as_str().to_string();
    drop(phc);
    ok_text(heap, types, result_ty, out)
}

fn crypto_verify_password(password: &[u8], encoded: &[u8]) -> Value {
    if (password.len() as u64) > MO_LIMIT_CRYPTO_PASSWORD_BYTES
        || encoded.is_empty()
        || (encoded.len() as u64) > MO_LIMIT_CRYPTO_PHC_BYTES
    {
        return Value::Bool(false);
    }

    let mut pos = 0usize;
    if !take_literal(encoded, &mut pos, b"$scrypt$ln=") {
        return Value::Bool(false);
    }
    let Some(ln) = parse_phc_uint(encoded, &mut pos, 1, MO_LIMIT_CRYPTO_SCRYPT_LN_MAX) else {
        return Value::Bool(false);
    };
    if !take_literal(encoded, &mut pos, b",r=") {
        return Value::Bool(false);
    }
    let Some(r) = parse_phc_uint(encoded, &mut pos, 1, MO_LIMIT_CRYPTO_SCRYPT_R_MAX) else {
        return Value::Bool(false);
    };
    if !take_literal(encoded, &mut pos, b",p=") {
        return Value::Bool(false);
    }
    let Some(p) = parse_phc_uint(encoded, &mut pos, 1, MO_LIMIT_CRYPTO_SCRYPT_P_MAX) else {
        return Value::Bool(false);
    };
    if !take_literal(encoded, &mut pos, b"$") {
        return Value::Bool(false);
    }

    let n_cost = 1u64 << ln;
    if r > (MO_LIMIT_CRYPTO_SCRYPT_MAXMEM / 128) / n_cost || p > (1u64 << 30) / (128 * r) {
        return Value::Bool(false);
    }

    let salt_start = pos;
    while pos < encoded.len() && encoded[pos] != b'$' {
        pos += 1;
    }
    if pos >= encoded.len() {
        return Value::Bool(false);
    }
    let salt_b64 = &encoded[salt_start..pos];
    pos += 1; // skip '$'
    let hash_b64 = &encoded[pos..];
    if hash_b64.is_empty() {
        return Value::Bool(false);
    }

    let mut salt = Zeroizing::new([0u8; MO_LIMIT_CRYPTO_SALT_MAX_BYTES as usize]);
    let mut expected = Zeroizing::new([0u8; SCRYPT_DK_MAX_BYTES]);
    let Some(salt_len) = phc_b64_decode(salt_b64, &mut *salt) else {
        return Value::Bool(false);
    };
    let Some(expected_len) = phc_b64_decode(hash_b64, &mut *expected) else {
        return Value::Bool(false);
    };
    if expected_len == 0 {
        return Value::Bool(false);
    }

    let Ok(params) = scrypt::Params::new(ln as u8, r as u32, p as u32) else {
        return Value::Bool(false);
    };

    let mut derived = Zeroizing::new([0u8; SCRYPT_DK_MAX_BYTES]);
    let ok = scrypt::scrypt(
        password,
        &salt[..salt_len],
        &params,
        &mut derived[..expected_len],
    )
    .is_ok();
    let matched = ok && bool::from(derived[..expected_len].ct_eq(&expected[..expected_len]));
    Value::Bool(matched)
}

pub fn call(
    heap: &Heap,
    types: &TypeTable,
    prim: PrimId,
    result_ty: u32,
    args: &[Value],
) -> Result<Value, PrimError> {
    match prim {
        PrimId::CryptoSha256 => crypto_sha256(heap, args[0].as_bytes()),
        PrimId::CryptoHmacSha256 => {
            crypto_hmac_sha256(heap, args[0].as_bytes(), args[1].as_bytes())
        }
        PrimId::CryptoConstantTimeEq => Ok(crypto_constant_time_eq(
            args[0].as_bytes(),
            args[1].as_bytes(),
        )),
        PrimId::CryptoSealRaw => crypto_seal_raw(
            heap,
            types,
            result_ty,
            args[0].as_bytes(),
            args[1].as_bytes(),
            args[2].as_bytes(),
            args[3].as_bytes(),
        ),
        PrimId::CryptoOpenRaw => crypto_open_raw(
            heap,
            types,
            result_ty,
            args[0].as_bytes(),
            args[1].as_bytes(),
            args[2].as_bytes(),
            args[3].as_bytes(),
        ),
        PrimId::CryptoHashPasswordRaw => crypto_hash_password_raw(
            heap,
            types,
            result_ty,
            args[0].as_text().as_bytes(),
            args[1].as_bytes(),
        ),
        PrimId::CryptoVerifyPassword => Ok(crypto_verify_password(
            args[0].as_text().as_bytes(),
            args[1].as_text().as_bytes(),
        )),
        PrimId::CryptoHmacSha1 => crypto_hmac_sha1(heap, args[0].as_bytes(), args[1].as_bytes()),
        PrimId::CryptoVerifySignatureRaw => crypto_verify_signature_raw(
            heap,
            types,
            result_ty,
            args[0].as_text(),
            args[1].as_bytes(),
            args[2].as_bytes(),
            args[3].as_bytes(),
        ),
        _ => unreachable!("non-Crypto primitive dispatched to crypto::call"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use mo_bytecode::{NO_INDEX, TypeCase, TypeDef, TypeField, TypeKind};

    fn hex_decode(s: &str) -> Vec<u8> {
        let b = s.as_bytes();
        assert_eq!(b.len() % 2, 0);
        (0..b.len())
            .step_by(2)
            .map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap())
            .collect()
    }

    fn hex_encode(bytes: &[u8]) -> String {
        let mut out = String::with_capacity(bytes.len() * 2);
        for &b in bytes {
            out.push_str(&format!("{b:02x}"));
        }
        out
    }

    fn crypto_types() -> (TypeTable, u32, u32) {
        let base = |kind: TypeKind| TypeDef {
            kind,
            parameters: Vec::new(),
            result: NO_INDEX,
            function: NO_INDEX,
            capability: None,
            name_idx: None,
            name: None,
            fields: Vec::new(),
            cases: Vec::new(),
            is_reference: false,
            has_fields: false,
        };
        let bytes_ty = base(TypeKind::Bytes); // 0
        let text_ty = base(TypeKind::Text); // 1
        let make_res = |ok_ty: u32| TypeDef {
            kind: TypeKind::Choice,
            parameters: Vec::new(),
            result: NO_INDEX,
            function: NO_INDEX,
            capability: None,
            name_idx: None,
            name: Some("Result".into()),
            fields: Vec::new(),
            cases: vec![
                TypeCase {
                    name_idx: 0,
                    name: "Ok".into(),
                    fields: vec![TypeField {
                        name_idx: None,
                        name: None,
                        ty: ok_ty,
                    }],
                    unnamed: true,
                },
                TypeCase {
                    name_idx: 1,
                    name: "Error".into(),
                    fields: vec![TypeField {
                        name_idx: None,
                        name: None,
                        ty: 1,
                    }],
                    unnamed: true,
                },
            ],
            is_reference: false,
            has_fields: false,
        };
        let res_bytes = make_res(0); // 2
        let res_text = make_res(1); // 3
        let types =
            TypeTable::finish(vec![bytes_ty, text_ty, res_bytes, res_text], Vec::new()).unwrap();
        (types, 2, 3)
    }

    #[test]
    fn sha256_fips_180_4_and_hmac_rfc_4231() {
        let heap = Heap::new(4 * 1024 * 1024);

        for (input, want) in [
            (
                "",
                "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            ),
            (
                "abc",
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            ),
            (
                "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
            ),
        ] {
            let out = crypto_sha256(&heap, input.as_bytes()).unwrap();
            assert_eq!(hex_encode(out.as_bytes()), want);
        }

        let k1 = vec![0x0bu8; 20];
        assert_eq!(
            hex_encode(
                crypto_hmac_sha256(&heap, &k1, b"Hi There")
                    .unwrap()
                    .as_bytes()
            ),
            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"
        );
        assert_eq!(
            hex_encode(
                crypto_hmac_sha256(&heap, b"Jefe", b"what do ya want for nothing?")
                    .unwrap()
                    .as_bytes()
            ),
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"
        );
        let k6 = vec![0xaau8; 131];
        assert_eq!(
            hex_encode(
                crypto_hmac_sha256(
                    &heap,
                    &k6,
                    b"Test Using Larger Than Block-Size Key - Hash Key First"
                )
                .unwrap()
                .as_bytes()
            ),
            "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"
        );
        // Empty key and empty data.
        let empty_mac = crypto_hmac_sha256(&heap, b"", b"").unwrap();
        assert_eq!(
            hex_encode(empty_mac.as_bytes()),
            "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad"
        );
    }

    #[test]
    fn aes_256_gcm_nist_vectors_and_errors() {
        let heap = Heap::new(4 * 1024 * 1024);
        let (types, res_bytes, _) = crypto_types();

        let vectors = [
            (
                "0000000000000000000000000000000000000000000000000000000000000000",
                "000000000000000000000000",
                "",
                "",
                "530f8afbc74536b9a963b4f1c4cb738b",
            ),
            (
                "0000000000000000000000000000000000000000000000000000000000000000",
                "000000000000000000000000",
                "00000000000000000000000000000000",
                "",
                "cea7403d4d606b6e074ec5d3baf39d18d0d1c8a799996bf0265b98b5d48ab919",
            ),
            (
                "feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308",
                "cafebabefacedbaddecaf888",
                "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
                "feedfacedeadbeeffeedfacedeadbeefabaddad2",
                "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f66276fc6ece0f4e1768cddf8853bb2d551b",
            ),
        ];

        for (k_hex, n_hex, p_hex, a_hex, want_hex) in vectors {
            let k = hex_decode(k_hex);
            let n = hex_decode(n_hex);
            let p = hex_decode(p_hex);
            let a = hex_decode(a_hex);
            let sealed = crypto_seal_raw(&heap, &types, res_bytes, &k, &n, &p, &a).unwrap();
            assert_eq!(sealed.as_tag(), 0);
            let ct = heap.extract_case_field(&types, res_bytes, &sealed, 0, 0);
            assert_eq!(hex_encode(ct.as_bytes()), want_hex);

            let opened =
                crypto_open_raw(&heap, &types, res_bytes, &k, &n, ct.as_bytes(), &a).unwrap();
            assert_eq!(opened.as_tag(), 0);
            let pt = heap.extract_case_field(&types, res_bytes, &opened, 0, 0);
            assert_eq!(pt.as_bytes(), p.as_slice());

            let mut tampered = ct.as_bytes().to_vec();
            tampered[0] ^= 0x01;
            let bad = crypto_open_raw(&heap, &types, res_bytes, &k, &n, &tampered, &a).unwrap();
            assert_eq!(bad.as_tag(), 1);
            assert_eq!(
                heap.extract_case_field(&types, res_bytes, &bad, 1, 0)
                    .as_text(),
                "decryption_failed"
            );
        }

        // Exact error text format checks.
        let bad_key =
            crypto_seal_raw(&heap, &types, res_bytes, &[0u8; 5], &[0u8; 12], b"", b"").unwrap();
        assert_eq!(bad_key.as_tag(), 1);
        assert_eq!(
            heap.extract_case_field(&types, res_bytes, &bad_key, 1, 0)
                .as_text(),
            "invalid_key_length:5"
        );

        let bad_nonce = crypto_open_raw(
            &heap, &types, res_bytes, &[0u8; 32], &[0u8; 7], &[0u8; 16], b"",
        )
        .unwrap();
        assert_eq!(bad_nonce.as_tag(), 1);
        assert_eq!(
            heap.extract_case_field(&types, res_bytes, &bad_nonce, 1, 0)
                .as_text(),
            "invalid_nonce_length:7"
        );

        let short_ct = crypto_open_raw(
            &heap, &types, res_bytes, &[0u8; 32], &[0u8; 12], &[0u8; 15], b"",
        )
        .unwrap();
        assert_eq!(short_ct.as_tag(), 1);
        assert_eq!(
            heap.extract_case_field(&types, res_bytes, &short_ct, 1, 0)
                .as_text(),
            "decryption_failed"
        );
    }

    #[test]
    fn scrypt_rfc_7914_phc_and_constant_time_eq() {
        let heap = Heap::new(4 * 1024 * 1024);
        let (types, _, res_text) = crypto_types();

        let cases = [
            (
                "",
                "$scrypt$ln=4,r=1,p=1$$d9ZXYjhleyA7GcpCwYoEl/FrSETjB0ro39/6P+3iFEL80Aad7QlI+DJqdToPyB8X6NPg+y4NNijPNeIMONGJBg",
                true,
            ),
            (
                "password",
                "$scrypt$ln=10,r=8,p=16$TmFDbA$/bq+HJ00cgB4VucZDQHp/nxq18vII3gw53N2Y0s3MWIurzDZLiKjiG/xCSedmDDaxyevuUqD7m2DYMvfoswGQA",
                true,
            ),
            (
                "pleaseletmein",
                "$scrypt$ln=14,r=8,p=1$U29kaXVtQ2hsb3JpZGU$cCO9yzr9c0hGHAbNgf046/2o+7qQT44+qbVD9lRdofLVQylVYT8Pz2LUlwUkKpr55h6F3A1lHkDfzwF7RVdYhw",
                true,
            ),
            (
                "wrong",
                "$scrypt$ln=4,r=1,p=1$$d9ZXYjhleyA7GcpCwYoEl/FrSETjB0ro39/6P+3iFEL80Aad7QlI+DJqdToPyB8X6NPg+y4NNijPNeIMONGJBg",
                false,
            ),
            ("pw", "$scrypt$ln=20,r=8,p=1$AAAA$AAAA", false),
            ("pw", "$scrypt$ln=04,r=1,p=1$$AAAA", false),
            ("pw", "$scrypt$ln=4,r=1,p=1$AA==$AAAA", false),
            ("pw", "$scrypt$ln=4,r=1,p=1$$AAAA$extra", false),
        ];
        for (pw, phc, want) in cases {
            assert_eq!(
                crypto_verify_password(pw.as_bytes(), phc.as_bytes()).as_bool(),
                want,
                "verify failed for {phc}"
            );
        }

        // Salt & password limit checks.
        let bad_salt =
            crypto_hash_password_raw(&heap, &types, res_text, b"secret", &[1u8; 15]).unwrap();
        assert_eq!(bad_salt.as_tag(), 1);
        assert_eq!(
            heap.extract_case_field(&types, res_text, &bad_salt, 1, 0)
                .as_text(),
            "invalid_salt_length:15"
        );
        let long_pw = vec![b'a'; 4097];
        let bad_pw =
            crypto_hash_password_raw(&heap, &types, res_text, &long_pw, &[1u8; 16]).unwrap();
        assert_eq!(bad_pw.as_tag(), 1);
        assert_eq!(
            heap.extract_case_field(&types, res_text, &bad_pw, 1, 0)
                .as_text(),
            "encoding_failed:password exceeds 4096 bytes"
        );

        // Full round-trip with default ln=15, r=8, p=1.
        let salt: [u8; 16] = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16];
        let hashed =
            crypto_hash_password_raw(&heap, &types, res_text, b"arvo-secret", &salt).unwrap();
        assert_eq!(hashed.as_tag(), 0);
        let phc = heap.extract_case_field(&types, res_text, &hashed, 0, 0);
        assert!(phc.as_text().starts_with("$scrypt$ln=15,r=8,p=1$"));
        assert!(crypto_verify_password(b"arvo-secret", phc.as_text().as_bytes()).as_bool());
        assert!(!crypto_verify_password(b"wrong", phc.as_text().as_bytes()).as_bool());

        // Constant-time equality.
        assert!(crypto_constant_time_eq(b"", b"").as_bool());
        assert!(crypto_constant_time_eq(b"abc", b"abc").as_bool());
        assert!(!crypto_constant_time_eq(b"abc", b"abd").as_bool());
        assert!(!crypto_constant_time_eq(b"abc", b"ab").as_bool());
    }

    #[test]
    fn hmac_sha1_rfc_2202_and_signature_verification() {
        let heap = Heap::new(4 * 1024 * 1024);
        let (_, _, _) = crypto_types();

        // Build TypeTable including Result(Bool, Text) at index 4.
        let base = |kind: TypeKind| TypeDef {
            kind,
            parameters: Vec::new(),
            result: NO_INDEX,
            function: NO_INDEX,
            capability: None,
            name_idx: None,
            name: None,
            fields: Vec::new(),
            cases: Vec::new(),
            is_reference: false,
            has_fields: false,
        };
        let res_bool_def = TypeDef {
            kind: TypeKind::Choice,
            parameters: Vec::new(),
            result: NO_INDEX,
            function: NO_INDEX,
            capability: None,
            name_idx: None,
            name: Some("Result".into()),
            fields: Vec::new(),
            cases: vec![
                TypeCase {
                    name_idx: 0,
                    name: "Ok".into(),
                    fields: vec![TypeField {
                        name_idx: None,
                        name: None,
                        ty: 2,
                    }],
                    unnamed: true,
                },
                TypeCase {
                    name_idx: 1,
                    name: "Error".into(),
                    fields: vec![TypeField {
                        name_idx: None,
                        name: None,
                        ty: 1,
                    }],
                    unnamed: true,
                },
            ],
            is_reference: false,
            has_fields: false,
        };
        let sig_types = TypeTable::finish(
            vec![
                base(TypeKind::Bytes), // 0
                base(TypeKind::Text),  // 1
                base(TypeKind::Bool),  // 2
                res_bool_def,          // 3
            ],
            Vec::new(),
        )
        .unwrap();
        let res_bool = 3u32;

        // RFC 2202 HMAC-SHA1 test vectors.
        assert_eq!(
            hex_encode(
                crypto_hmac_sha1(&heap, &[0x0bu8; 20], b"Hi There")
                    .unwrap()
                    .as_bytes()
            ),
            "b617318655057264e28bc0b6fb378c8ef146be00"
        );
        assert_eq!(
            hex_encode(
                crypto_hmac_sha1(&heap, b"Jefe", b"what do ya want for nothing?")
                    .unwrap()
                    .as_bytes()
            ),
            "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"
        );
        assert_eq!(
            hex_encode(
                crypto_hmac_sha1(&heap, &[0xaau8; 20], &[0xddu8; 50])
                    .unwrap()
                    .as_bytes()
            ),
            "125d7342b9ac11cd91a39af48aa17b4f63f175d3"
        );
        assert_eq!(
            hex_encode(
                crypto_hmac_sha1(
                    &heap,
                    &[0xaau8; 80],
                    b"Test Using Larger Than Block-Size Key and Larger Than One Block-Size Data",
                )
                .unwrap()
                .as_bytes()
            ),
            "e8e99d0f45237d786d6bbaa7965c7808bbff1a91"
        );
        assert_eq!(
            hex_encode(crypto_hmac_sha1(&heap, b"", b"").unwrap().as_bytes()),
            "fbdb1d1b18aa6c08324b7d64b71fb76370690e1d"
        );

        let check_verify = |alg: &str, pk: &[u8], msg: &[u8], sig: &[u8]| -> bool {
            let v = crypto_verify_signature_raw(&heap, &sig_types, res_bool, alg, pk, msg, sig)
                .unwrap();
            assert_eq!(v.as_tag(), 0);
            heap.extract_case_field(&sig_types, res_bool, &v, 0, 0)
                .as_bool()
        };

        // Ed25519
        let ed_pub = hex_decode("f347ead52316f7f85f4b0448db694606dfcc1aeb420e13cfdb04933c73be350f");
        let ed_sig = hex_decode(
            "8a3fc479c36a70d56bb04dc4330886926451a1682f2b236f96c4f725acc98994b437e39ebe9bbf08722d6f0969c7ecc33d8cfe51b1405020052df63374e98a0d",
        );
        assert!(check_verify(
            "ed25519",
            &ed_pub,
            b"arvo-wave0-ed25519",
            &ed_sig
        ));
        let mut bad_ed = ed_sig.clone();
        bad_ed[0] ^= 0x01;
        assert!(!check_verify(
            "ed25519",
            &ed_pub,
            b"arvo-wave0-ed25519",
            &bad_ed
        ));
        assert!(!check_verify("ed25519", &ed_pub, b"tampered", &ed_sig));

        // ES256 ASN.1 DER and fixed R||S
        let ec_pub = hex_decode(
            "04fb4ddba3d0e5b6d1c65eaacd8bf5e13d0e5ab7011cb6591834939df20555dbf8b4a1f9a8726cd2ceff0a1b1cb88b22e637e4c67149b03b0892b7d6c80dfff4ea",
        );
        let ec_asn1 = hex_decode(
            "304502206a28481e84bc6cbfe1bc232b932f8fc125dca9f1be6e79cdd9b45acbfd08b782022100917dbd45a2dea75974f4aa5147e211dff3ac709da9b5fe9a54cc22ca801e57f1",
        );
        let ec_fixed = hex_decode(
            "6a28481e84bc6cbfe1bc232b932f8fc125dca9f1be6e79cdd9b45acbfd08b782917dbd45a2dea75974f4aa5147e211dff3ac709da9b5fe9a54cc22ca801e57f1",
        );
        assert!(check_verify(
            "es256_asn1",
            &ec_pub,
            b"arvo-wave0-es256",
            &ec_asn1
        ));
        assert!(!check_verify("es256_asn1", &ec_pub, b"tampered", &ec_asn1));
        assert!(check_verify(
            "es256_fixed",
            &ec_pub,
            b"arvo-wave0-es256",
            &ec_fixed
        ));
        let mut bad_ec = ec_fixed.clone();
        bad_ec[10] ^= 0x80;
        assert!(!check_verify(
            "es256_fixed",
            &ec_pub,
            b"arvo-wave0-es256",
            &bad_ec
        ));

        // RS256 PKCS#1 DER public key
        let rsa_pub = hex_decode(
            "3082010a0282010100ba7e0e9316a26bf1eba56b750ab96da09a40f275ca1b8c0d971b5bde63c844033fd43bf1f799a57607702c1edc1d9c9c12249f91004be6ee9abd02a8821fd650a2efe27d5f4729b4b341289a6f02075e96c194fbb58bc173cb087fcb5fc4b4aa3e3b0a405843b2acf6ff7797a8f28c9ce64b073c2f3a1216e8b014adbefd98cb1a34d2e02d18efd672c8fc33707abddeb1873ca97d85ceed61bb42acc2447d4e18f3b92d9e9015afc8ce7e2d311d2b3ddaab7597073116e61c176f1fa625a408d44cdcd5d0df7c976ef3f47606997dc4e8dfed3ac675a937dcaee4e717650c6633335a6404d0efeb6c341e7faf09b29c07cb16f5e281b8645f328f7c1f329ba70203010001",
        );
        let rsa_sig = hex_decode(
            "292dd9caafabdc25759f893c69e15f1f2b5f3b9524d88f4b3e7292d58ba951e778cd5db5b796e3319f43f332697ee3cf4a2a2e41420b4e63c5d22733afe110e27b7a57c93f743825b886007a4f5fa1b8b6487b8e871d0d04d320c3cbec19c38c1196ac16355c8076ba19c874629845b2dd334e22d1ce5d496a7a29f9165a83499cb390073d8eaf3237f3c8be9e657516a6f9749c8f6d5733af37cfb5b69453fc4cf5b09513e3e0676af377cf398b1b30f934d506612af4c27ae23c0a5577466442048751544726d23e9662d5446df495b5868b61b9a1e05f70e2a52b3bdba114ca3bf6a2fc30a82eea53a7c2e16921543aef9c0ab1aff790ba58f124d27c311a",
        );
        assert!(check_verify("rs256", &rsa_pub, b"arvo-wave0-rsa", &rsa_sig));
        assert!(!check_verify("rs256", &rsa_pub, b"tampered", &rsa_sig));
        assert!(!check_verify(
            "rs256",
            b"bad-der",
            b"arvo-wave0-rsa",
            &rsa_sig
        ));

        // Unsupported algorithm returns Error(text)
        let bad_alg = crypto_verify_signature_raw(
            &heap, &sig_types, res_bool, "hs256", &ed_pub, b"msg", &ed_sig,
        )
        .unwrap();
        assert_eq!(bad_alg.as_tag(), 1);
        assert_eq!(
            heap.extract_case_field(&sig_types, res_bool, &bad_alg, 1, 0)
                .as_text(),
            "unsupported_algorithm:hs256"
        );
    }
}
