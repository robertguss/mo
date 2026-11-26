#![forbid(unsafe_code)]

//! Oracle driver compatible with `ci/check_unicode16.py` for verifying the
//! pure-Rust `mo-prim` Unicode 16.0.0 implementation against all pinned UCD
//! 16.0.0 conformance files (`GraphemeBreakTest.txt`, `NormalizationTest.txt`,
//! `SpecialCasing.txt`, `EastAsianWidth.txt`, `UnicodeData.txt`, etc.).

use std::env;
use std::error::Error;
use std::io::{self, BufRead, BufWriter, Write};

use mo_bytecode::types::TypeTable;
use mo_bytecode::{NO_INDEX, TypeDef, TypeKind};
use mo_def::PrimId;
use mo_heap::{Heap, Value};

type OracleResult<T = ()> = Result<T, Box<dyn Error>>;

fn scalar_def(kind: TypeKind, is_reference: bool) -> TypeDef {
    TypeDef {
        kind,
        parameters: Vec::new(),
        result: NO_INDEX,
        function: NO_INDEX,
        capability: None,
        name_idx: None,
        name: None,
        fields: Vec::new(),
        cases: Vec::new(),
        is_reference,
        has_fields: false,
    }
}

struct Fixture {
    heap: Heap,
    types: TypeTable,
    sig_graphemes: u32,
    sig_width: u32,
    sig_nfc: u32,
    sig_lower: u32,
    sig_upper: u32,
    sig_char_bool: u32,
}

impl Fixture {
    fn new(heap_limit: usize) -> OracleResult<Self> {
        let mut items = Vec::new();
        let mut push = |def: TypeDef| -> u32 {
            let idx = items.len() as u32;
            items.push(def);
            idx
        };
        let t_text = push(scalar_def(TypeKind::Text, true));
        let t_int = push(scalar_def(TypeKind::Int, false));
        let t_char = push(scalar_def(TypeKind::Char, false));
        let t_bool = push(scalar_def(TypeKind::Bool, false));
        let mut list_text = scalar_def(TypeKind::List, true);
        list_text.parameters = vec![t_text];
        let t_list_text = push(list_text);

        let mut make_sig = |param: u32, ret: u32| -> u32 {
            let mut e = scalar_def(TypeKind::Function, false);
            e.parameters = vec![param];
            e.result = ret;
            push(e)
        };
        let sig_graphemes = make_sig(t_text, t_list_text);
        let sig_width = make_sig(t_text, t_int);
        let sig_nfc = make_sig(t_text, t_text);
        let sig_lower = make_sig(t_text, t_text);
        let sig_upper = make_sig(t_text, t_text);
        let sig_char_bool = make_sig(t_char, t_bool);

        Ok(Self {
            heap: Heap::new(heap_limit),
            types: TypeTable::finish(items, Vec::new()).map_err(|e| format!("{e:?}"))?,
            sig_graphemes,
            sig_width,
            sig_nfc,
            sig_lower,
            sig_upper,
            sig_char_bool,
        })
    }
}

fn write_hex(out: &mut impl Write, bytes: &[u8]) -> io::Result<()> {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut buf = Vec::with_capacity(bytes.len() * 2);
    for &b in bytes {
        buf.push(HEX[(b >> 4) as usize]);
        buf.push(HEX[(b & 0xf) as usize]);
    }
    out.write_all(&buf)
}

fn call_prim(fx: &Fixture, prim: PrimId, sig: u32, args: &mut [Value]) -> OracleResult<Value> {
    mo_prim::call(&fx.heap, &fx.types, prim, sig, args).map_err(|e| format!("{e:?}").into())
}

fn run_categories() -> OracleResult {
    let fx = Fixture::new(64 * 1024 * 1024)?;
    let mut out = BufWriter::new(io::stdout().lock());
    for cp in 0u32..=0x10_FFFF {
        let Some(c) = char::from_u32(cp) else {
            continue;
        };
        let ch = Value::Char(c);
        let is_letter = call_prim(
            &fx,
            PrimId::Unicode16Letter,
            fx.sig_char_bool,
            &mut [ch.clone()],
        )?
        .as_bool();
        let is_digit =
            call_prim(&fx, PrimId::Unicode16Digit, fx.sig_char_bool, &mut [ch])?.as_bool();
        let bits = u8::from(is_letter) | (u8::from(is_digit) << 1);
        out.write_all(&[bits])?;
    }
    out.flush()?;
    Ok(())
}

fn run_oracle() -> OracleResult {
    let fx = Fixture::new(256 * 1024 * 1024)?;
    let stdin = io::stdin().lock();
    let mut out = BufWriter::new(io::stdout().lock());
    for line in stdin.lines() {
        let line = line?;
        let bytes = line.as_bytes();
        if bytes.len() < 2 || bytes[1] != b' ' {
            return Err(format!("malformed oracle line: {line:?}").into());
        }
        let op = bytes[0] as char;
        let hex = &bytes[2..];
        if !hex.len().is_multiple_of(2) {
            return Err(format!("odd hex length in oracle line: {line:?}").into());
        }
        let mut raw = Vec::with_capacity(hex.len() / 2);
        for pair in hex.as_chunks::<2>().0 {
            let hi = (pair[0] as char)
                .to_digit(16)
                .ok_or_else(|| format!("invalid hex byte: {}", pair[0]))?
                as u8;
            let lo = (pair[1] as char)
                .to_digit(16)
                .ok_or_else(|| format!("invalid hex byte: {}", pair[1]))?
                as u8;
            raw.push((hi << 4) | lo);
        }
        let text = String::from_utf8(raw)?;
        let input = fx.heap.alloc_text(text).map_err(|e| format!("{e:?}"))?;
        let (prim, sig) = match op {
            'g' => (PrimId::Unicode16Graphemes, fx.sig_graphemes),
            'w' => (PrimId::Unicode16DisplayWidth, fx.sig_width),
            'n' => (PrimId::Unicode16NormalizeNfc, fx.sig_nfc),
            'l' => (PrimId::Unicode16Lowercase, fx.sig_lower),
            'u' => (PrimId::Unicode16Uppercase, fx.sig_upper),
            other => return Err(format!("unexpected op: {other}").into()),
        };
        let res = call_prim(&fx, prim, sig, &mut [input])?;
        match op {
            'g' => {
                let items = res.as_list();
                for (i, item) in items.iter().enumerate() {
                    if i != 0 {
                        out.write_all(b":")?;
                    }
                    write_hex(&mut out, item.as_text().as_bytes())?;
                }
            }
            'w' => {
                write!(out, "{}", res.as_int())?;
            }
            _ => {
                write_hex(&mut out, res.as_text().as_bytes())?;
            }
        }
        out.write_all(b"\n")?;
    }
    out.flush()?;
    Ok(())
}

fn check(cond: bool, msg: &str) -> OracleResult {
    if cond { Ok(()) } else { Err(msg.into()) }
}

fn run_work() -> OracleResult {
    let fx = Fixture::new(256 * 1024 * 1024)?;
    let count = 131_072usize;

    let ri = "\u{1F1E6}".repeat(count);
    let ri_val = fx.heap.alloc_text(ri).map_err(|e| format!("{e:?}"))?;
    let w = call_prim(
        &fx,
        PrimId::Unicode16DisplayWidth,
        fx.sig_width,
        &mut [ri_val.clone()],
    )?
    .as_int();
    check(
        w == count as i64,
        "regional indicator display width mismatch",
    )?;
    let g = call_prim(
        &fx,
        PrimId::Unicode16Graphemes,
        fx.sig_graphemes,
        &mut [ri_val],
    )?;
    check(
        g.as_list().len() == count / 2,
        "regional indicator grapheme count mismatch",
    )?;

    let mut comb = String::with_capacity(1 + count * 4);
    comb.push('a');
    for _ in 0..count {
        comb.push('\u{0315}');
    }
    for _ in 0..count {
        comb.push('\u{0300}');
    }
    let comb_val = fx.heap.alloc_text(comb).map_err(|e| format!("{e:?}"))?;
    let comb_w = call_prim(
        &fx,
        PrimId::Unicode16DisplayWidth,
        fx.sig_width,
        &mut [comb_val.clone()],
    )?
    .as_int();
    check(comb_w == 1, "combining marks display width mismatch")?;
    let nfc = call_prim(
        &fx,
        PrimId::Unicode16NormalizeNfc,
        fx.sig_nfc,
        &mut [comb_val],
    )?;
    let nfc_bytes = nfc.as_text().as_bytes();
    check(
        nfc_bytes.len() == count * 4,
        "combining marks NFC length mismatch",
    )?;
    check(
        nfc_bytes.starts_with(&[0xc3, 0xa0]),
        "combining marks NFC prefix mismatch",
    )?;

    let mut sig_text = String::with_capacity(3 + count * 4);
    sig_text.push('A');
    for _ in 0..count {
        sig_text.push('\u{0345}');
    }
    sig_text.push('\u{03A3}');
    for _ in 0..count {
        sig_text.push('\u{0307}');
    }
    let sig_val = fx.heap.alloc_text(sig_text).map_err(|e| format!("{e:?}"))?;
    let lowered = call_prim(
        &fx,
        PrimId::Unicode16Lowercase,
        fx.sig_lower,
        &mut [sig_val],
    )?;
    let lb = lowered.as_text().as_bytes();
    check(lb[0] == b'a', "final sigma lowercase prefix mismatch")?;
    check(
        lb[count * 2 + 1..count * 2 + 3] == [0xcf, 0x82],
        "final sigma lowercase mismatch",
    )?;
    Ok(())
}

fn main() {
    let arg = env::args().nth(1).unwrap_or_default();
    let res = match arg.as_str() {
        "--categories" => run_categories(),
        "--oracle" => run_oracle(),
        "--work" => run_work(),
        _ => {
            eprintln!("usage: unicode16_oracle --categories | --oracle | --work");
            std::process::exit(2);
        }
    };
    if let Err(err) = res {
        eprintln!("unicode16_oracle error: {err}");
        std::process::exit(1);
    }
}
