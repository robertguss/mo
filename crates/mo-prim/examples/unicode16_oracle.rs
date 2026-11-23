#![forbid(unsafe_code)]

//! Oracle driver compatible with `ci/check_unicode16.py` for verifying the
//! pure-Rust `mo-prim` Unicode 16.0.0 implementation against all pinned UCD
//! 16.0.0 conformance files (`GraphemeBreakTest.txt`, `NormalizationTest.txt`,
//! `SpecialCasing.txt`, `EastAsianWidth.txt`, `UnicodeData.txt`, etc.).

use std::env;
use std::io::{self, BufRead, BufWriter, Write};

use mo_bytecode::types::TypeTable;
use mo_bytecode::{NO_INDEX, TypeDef, TypeKind};
use mo_def::PrimId;
use mo_heap::{Heap, Value};

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
    fn new(heap_limit: usize) -> Self {
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

        Self {
            heap: Heap::new(heap_limit),
            types: TypeTable::finish(items, Vec::new()).unwrap(),
            sig_graphemes,
            sig_width,
            sig_nfc,
            sig_lower,
            sig_upper,
            sig_char_bool,
        }
    }
}

fn write_hex(out: &mut impl Write, bytes: &[u8]) {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut buf = Vec::with_capacity(bytes.len() * 2);
    for &b in bytes {
        buf.push(HEX[(b >> 4) as usize]);
        buf.push(HEX[(b & 0xf) as usize]);
    }
    out.write_all(&buf).unwrap();
}

fn run_categories() {
    let fx = Fixture::new(64 * 1024 * 1024);
    let mut out = BufWriter::new(io::stdout().lock());
    for cp in 0u32..=0x10_FFFF {
        let Some(c) = char::from_u32(cp) else {
            continue;
        };
        let ch = Value::Char(c);
        let is_letter = mo_prim::call(
            &fx.heap,
            &fx.types,
            PrimId::Unicode16Letter,
            fx.sig_char_bool,
            &mut [ch.clone()],
        )
        .unwrap()
        .as_bool();
        let is_digit = mo_prim::call(
            &fx.heap,
            &fx.types,
            PrimId::Unicode16Digit,
            fx.sig_char_bool,
            &mut [ch],
        )
        .unwrap()
        .as_bool();
        let bits = u8::from(is_letter) | (u8::from(is_digit) << 1);
        out.write_all(&[bits]).unwrap();
    }
    out.flush().unwrap();
}

fn run_oracle() {
    let fx = Fixture::new(256 * 1024 * 1024);
    let stdin = io::stdin().lock();
    let mut out = BufWriter::new(io::stdout().lock());
    for line in stdin.lines() {
        let line = line.unwrap();
        let bytes = line.as_bytes();
        assert!(bytes.len() >= 2 && bytes[1] == b' ');
        let op = bytes[0] as char;
        let hex = &bytes[2..];
        assert!(hex.len().is_multiple_of(2));
        let mut raw = Vec::with_capacity(hex.len() / 2);
        for pair in hex.chunks_exact(2) {
            let hi = (pair[0] as char).to_digit(16).unwrap() as u8;
            let lo = (pair[1] as char).to_digit(16).unwrap() as u8;
            raw.push((hi << 4) | lo);
        }
        let text = String::from_utf8(raw).unwrap();
        let input = fx.heap.alloc_text(text).unwrap();
        let (prim, sig) = match op {
            'g' => (PrimId::Unicode16Graphemes, fx.sig_graphemes),
            'w' => (PrimId::Unicode16DisplayWidth, fx.sig_width),
            'n' => (PrimId::Unicode16NormalizeNfc, fx.sig_nfc),
            'l' => (PrimId::Unicode16Lowercase, fx.sig_lower),
            'u' => (PrimId::Unicode16Uppercase, fx.sig_upper),
            other => panic!("unexpected op: {other}"),
        };
        let res = mo_prim::call(&fx.heap, &fx.types, prim, sig, &mut [input]).unwrap();
        match op {
            'g' => {
                let items = res.as_list();
                for (i, item) in items.iter().enumerate() {
                    if i != 0 {
                        out.write_all(b":").unwrap();
                    }
                    write_hex(&mut out, item.as_text().as_bytes());
                }
            }
            'w' => {
                write!(out, "{}", res.as_int()).unwrap();
            }
            _ => {
                write_hex(&mut out, res.as_text().as_bytes());
            }
        }
        out.write_all(b"\n").unwrap();
    }
    out.flush().unwrap();
}

fn run_work() {
    let fx = Fixture::new(256 * 1024 * 1024);
    let count = 131_072usize;

    let ri = "\u{1F1E6}".repeat(count);
    let ri_val = fx.heap.alloc_text(ri).unwrap();
    let w = mo_prim::call(
        &fx.heap,
        &fx.types,
        PrimId::Unicode16DisplayWidth,
        fx.sig_width,
        &mut [ri_val.clone()],
    )
    .unwrap()
    .as_int();
    assert_eq!(w, count as i64);
    let g = mo_prim::call(
        &fx.heap,
        &fx.types,
        PrimId::Unicode16Graphemes,
        fx.sig_graphemes,
        &mut [ri_val],
    )
    .unwrap();
    assert_eq!(g.as_list().len(), count / 2);

    let mut comb = String::with_capacity(1 + count * 4);
    comb.push('a');
    for _ in 0..count {
        comb.push('\u{0315}');
    }
    for _ in 0..count {
        comb.push('\u{0300}');
    }
    let comb_val = fx.heap.alloc_text(comb).unwrap();
    assert_eq!(
        mo_prim::call(
            &fx.heap,
            &fx.types,
            PrimId::Unicode16DisplayWidth,
            fx.sig_width,
            &mut [comb_val.clone()],
        )
        .unwrap()
        .as_int(),
        1
    );
    let nfc = mo_prim::call(
        &fx.heap,
        &fx.types,
        PrimId::Unicode16NormalizeNfc,
        fx.sig_nfc,
        &mut [comb_val],
    )
    .unwrap();
    let nfc_bytes = nfc.as_text().as_bytes();
    assert_eq!(nfc_bytes.len(), count * 4);
    assert_eq!(&nfc_bytes[..2], &[0xc3, 0xa0]);

    let mut sig_text = String::with_capacity(3 + count * 4);
    sig_text.push('A');
    for _ in 0..count {
        sig_text.push('\u{0345}');
    }
    sig_text.push('\u{03A3}');
    for _ in 0..count {
        sig_text.push('\u{0307}');
    }
    let sig_val = fx.heap.alloc_text(sig_text).unwrap();
    let lowered = mo_prim::call(
        &fx.heap,
        &fx.types,
        PrimId::Unicode16Lowercase,
        fx.sig_lower,
        &mut [sig_val],
    )
    .unwrap();
    let lb = lowered.as_text().as_bytes();
    assert_eq!(lb[0], b'a');
    assert_eq!(&lb[count * 2 + 1..count * 2 + 3], &[0xcf, 0x82]);
}

fn main() {
    let arg = env::args().nth(1).unwrap_or_default();
    match arg.as_str() {
        "--categories" => run_categories(),
        "--oracle" => run_oracle(),
        "--work" => run_work(),
        _ => {
            eprintln!("usage: unicode16_oracle --categories | --oracle | --work");
            std::process::exit(2);
        }
    }
}
