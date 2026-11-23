#![forbid(unsafe_code)]

use std::collections::HashMap;
use std::env;
use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};

fn main() {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    let repo_root = manifest_dir
        .parent()
        .and_then(Path::parent)
        .expect("crates/mo-def lives two levels under repo root");

    let limits_path = repo_root.join("src/base/limits.h");
    let opcodes_path = repo_root.join("src/bytecode/opcodes.def");
    let prims_path = repo_root.join("src/base/prims.def");
    let hostops_path = repo_root.join("src/base/hostops.def");

    for path in [&limits_path, &opcodes_path, &prims_path, &hostops_path] {
        println!("cargo:rerun-if-changed={}", path.display());
    }

    let limits_src = fs::read_to_string(&limits_path).expect("read src/base/limits.h");
    let opcodes_src = fs::read_to_string(&opcodes_path).expect("read src/bytecode/opcodes.def");
    let prims_src = fs::read_to_string(&prims_path).expect("read src/base/prims.def");
    let hostops_src = fs::read_to_string(&hostops_path).expect("read src/base/hostops.def");

    let limits = parse_limits(&limits_src);
    let opcodes = parse_opcodes(&opcodes_src, &limits);
    let prims = parse_prims(&prims_src);
    let (hostops, simops) = parse_hostops(&hostops_src);

    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR"));
    let mut out = String::new();
    emit_limits(&mut out, &limits);
    emit_opcodes(&mut out, &opcodes);
    emit_prims(&mut out, &prims);
    emit_hostops(&mut out, &hostops);
    emit_simops(&mut out, &simops);

    fs::write(out_dir.join("generated.rs"), out).expect("write generated.rs");
}

fn strip_c_comments(input: &str) -> String {
    let bytes = input.as_bytes();
    let mut out = String::with_capacity(input.len());
    let mut i = 0;
    let mut in_string = false;
    while i < bytes.len() {
        if in_string {
            let c = bytes[i];
            out.push(c as char);
            if c == b'\\' && i + 1 < bytes.len() {
                i += 1;
                out.push(bytes[i] as char);
            } else if c == b'"' {
                in_string = false;
            }
            i += 1;
        } else if bytes[i] == b'"' {
            in_string = true;
            out.push('"');
            i += 1;
        } else if bytes[i] == b'/' && i + 1 < bytes.len() && bytes[i + 1] == b'*' {
            i += 2;
            while i + 1 < bytes.len() && !(bytes[i] == b'*' && bytes[i + 1] == b'/') {
                if bytes[i] == b'\n' {
                    out.push('\n');
                }
                i += 1;
            }
            i += 2;
        } else if bytes[i] == b'/' && i + 1 < bytes.len() && bytes[i + 1] == b'/' {
            i += 2;
            while i < bytes.len() && bytes[i] != b'\n' {
                i += 1;
            }
        } else {
            out.push(bytes[i] as char);
            i += 1;
        }
    }
    out
}

fn join_continuations(input: &str) -> Vec<String> {
    let mut lines = Vec::new();
    let mut current = String::new();
    for raw in input.lines() {
        let trimmed_end = raw.trim_end_matches('\r');
        if let Some(prefix) = trimmed_end.strip_suffix('\\') {
            current.push_str(prefix);
            current.push(' ');
        } else {
            current.push_str(trimmed_end);
            lines.push(std::mem::take(&mut current));
        }
    }
    if !current.is_empty() {
        lines.push(current);
    }
    lines
}

#[derive(Debug, Clone)]
struct LimitEntry {
    name: String,
    value: i128,
    signed: bool,
}

fn parse_limits(src: &str) -> Vec<LimitEntry> {
    let cleaned = strip_c_comments(src);
    let lines = join_continuations(&cleaned);

    // Track conditional blocks: only enter #ifndef (not #if defined(__SANITIZE_ADDRESS__) / #elif).
    let mut active_stack: Vec<bool> = Vec::new();
    let mut raw_defs: Vec<(String, String)> = Vec::new();

    for line in lines {
        let t = line.trim();
        if t.starts_with("#ifdef") || t.starts_with("#if ") || t.starts_with("#if(") {
            let parent = active_stack.last().copied().unwrap_or(true);
            active_stack.push(parent && false);
            continue;
        }
        if t.starts_with("#ifndef") {
            let parent = active_stack.last().copied().unwrap_or(true);
            // Enter #ifndef MO_LIMITS_H and #ifndef MO_LIMIT_* default definitions.
            active_stack.push(parent);
            continue;
        }
        if t.starts_with("#elif") {
            if let Some(top) = active_stack.last_mut() {
                *top = false;
            }
            continue;
        }
        if t.starts_with("#else") {
            let parent = if active_stack.len() >= 2 {
                active_stack[active_stack.len() - 2]
            } else {
                true
            };
            if let Some(top) = active_stack.last_mut() {
                *top = parent && !*top;
            }
            continue;
        }
        if t.starts_with("#endif") {
            active_stack.pop();
            continue;
        }
        if !active_stack.last().copied().unwrap_or(true) {
            continue;
        }
        if let Some(rest) = t.strip_prefix("#define") {
            let rest = rest.trim_start();
            let mut parts = rest.splitn(2, |c: char| c.is_ascii_whitespace());
            let name = parts.next().unwrap_or("");
            let body = parts.next().unwrap_or("").trim();
            if name.is_empty() || body.is_empty() || name.contains('(') || !name.starts_with("MO_")
            {
                continue;
            }
            raw_defs.push((name.to_string(), body.to_string()));
        }
    }

    let mut env: HashMap<String, i128> = HashMap::new();
    env.insert("UINT16_MAX".to_string(), i128::from(u16::MAX));
    env.insert("UINT32_MAX".to_string(), i128::from(u32::MAX));
    env.insert("UINT64_MAX".to_string(), i128::from(u64::MAX));
    env.insert("INT64_MAX".to_string(), i128::from(i64::MAX));
    env.insert("INT64_MIN".to_string(), i128::from(i64::MIN));

    let mut resolved: HashMap<String, i128> = HashMap::new();
    for _pass in 0..16 {
        let mut progress = false;
        for (name, body) in &raw_defs {
            if resolved.contains_key(name) {
                continue;
            }
            if let Some(val) = eval_c_expr(body, &env) {
                env.insert(name.clone(), val);
                resolved.insert(name.clone(), val);
                progress = true;
            }
        }
        if !progress {
            break;
        }
    }

    let mut entries = Vec::new();
    for (name, body) in &raw_defs {
        let val = resolved
            .get(name)
            .copied()
            .unwrap_or_else(|| panic!("could not evaluate limit macro {name} = {body}"));
        let signed = val < 0
            || name == "MO_TIME_MIN_US"
            || name == "MO_TIME_MAX_US"
            || name == "MO_LIMIT_COMMAND_DURATION_US";
        entries.push(LimitEntry {
            name: name.clone(),
            value: val,
            signed,
        });
    }
    entries
}

struct ExprParser<'a> {
    src: &'a [u8],
    pos: usize,
    env: &'a HashMap<String, i128>,
}

impl<'a> ExprParser<'a> {
    fn new(src: &'a str, env: &'a HashMap<String, i128>) -> Self {
        Self {
            src: src.as_bytes(),
            pos: 0,
            env,
        }
    }

    fn skip_ws(&mut self) {
        while self.pos < self.src.len() && self.src[self.pos].is_ascii_whitespace() {
            self.pos += 1;
        }
    }

    fn peek(&mut self) -> Option<u8> {
        self.skip_ws();
        self.src.get(self.pos).copied()
    }

    fn parse_shift(&mut self) -> Option<i128> {
        let mut lhs = self.parse_add()?;
        loop {
            self.skip_ws();
            if self.pos + 1 < self.src.len()
                && self.src[self.pos] == b'<'
                && self.src[self.pos + 1] == b'<'
            {
                self.pos += 2;
                let rhs = self.parse_add()?;
                lhs <<= rhs;
            } else if self.pos + 1 < self.src.len()
                && self.src[self.pos] == b'>'
                && self.src[self.pos + 1] == b'>'
            {
                self.pos += 2;
                let rhs = self.parse_add()?;
                lhs >>= rhs;
            } else {
                break;
            }
        }
        Some(lhs)
    }

    fn parse_add(&mut self) -> Option<i128> {
        let mut lhs = self.parse_mul()?;
        loop {
            match self.peek() {
                Some(b'+') => {
                    self.pos += 1;
                    lhs += self.parse_mul()?;
                }
                Some(b'-') => {
                    self.pos += 1;
                    lhs -= self.parse_mul()?;
                }
                _ => break,
            }
        }
        Some(lhs)
    }

    fn parse_mul(&mut self) -> Option<i128> {
        let mut lhs = self.parse_unary()?;
        loop {
            match self.peek() {
                Some(b'*') => {
                    self.pos += 1;
                    lhs *= self.parse_unary()?;
                }
                Some(b'/') => {
                    self.pos += 1;
                    let rhs = self.parse_unary()?;
                    lhs /= rhs;
                }
                _ => break,
            }
        }
        Some(lhs)
    }

    fn parse_unary(&mut self) -> Option<i128> {
        match self.peek()? {
            b'+' => {
                self.pos += 1;
                self.parse_unary()
            }
            b'-' => {
                self.pos += 1;
                Some(-self.parse_unary()?)
            }
            b'(' => {
                // Could be a C cast like (size_t)256 or a parenthesized expression.
                let saved = self.pos;
                self.pos += 1;
                self.skip_ws();
                let start = self.pos;
                while self.pos < self.src.len()
                    && (self.src[self.pos].is_ascii_alphanumeric() || self.src[self.pos] == b'_')
                {
                    self.pos += 1;
                }
                let ident = std::str::from_utf8(&self.src[start..self.pos]).ok()?;
                self.skip_ws();
                if matches!(
                    ident,
                    "size_t" | "uint64_t" | "uint32_t" | "int64_t" | "unsigned"
                ) && self.pos < self.src.len()
                    && self.src[self.pos] == b')'
                {
                    self.pos += 1;
                    return self.parse_unary();
                }
                self.pos = saved + 1;
                let val = self.parse_shift()?;
                if self.peek() != Some(b')') {
                    return None;
                }
                self.pos += 1;
                Some(val)
            }
            c if c.is_ascii_digit() => self.parse_number(),
            c if c.is_ascii_alphabetic() || c == b'_' => self.parse_ident(),
            _ => None,
        }
    }

    fn parse_number(&mut self) -> Option<i128> {
        self.skip_ws();
        let start = self.pos;
        if self.pos + 1 < self.src.len()
            && self.src[self.pos] == b'0'
            && (self.src[self.pos + 1] == b'x' || self.src[self.pos + 1] == b'X')
        {
            self.pos += 2;
            let hex_start = self.pos;
            while self.pos < self.src.len() && self.src[self.pos].is_ascii_hexdigit() {
                self.pos += 1;
            }
            let val = i128::from_str_radix(
                std::str::from_utf8(&self.src[hex_start..self.pos]).ok()?,
                16,
            )
            .ok()?;
            while self.pos < self.src.len()
                && matches!(self.src[self.pos], b'u' | b'U' | b'l' | b'L')
            {
                self.pos += 1;
            }
            return Some(val);
        }
        while self.pos < self.src.len() && self.src[self.pos].is_ascii_digit() {
            self.pos += 1;
        }
        let val = std::str::from_utf8(&self.src[start..self.pos])
            .ok()?
            .parse::<i128>()
            .ok()?;
        while self.pos < self.src.len() && matches!(self.src[self.pos], b'u' | b'U' | b'l' | b'L') {
            self.pos += 1;
        }
        Some(val)
    }

    fn parse_ident(&mut self) -> Option<i128> {
        self.skip_ws();
        let start = self.pos;
        while self.pos < self.src.len()
            && (self.src[self.pos].is_ascii_alphanumeric() || self.src[self.pos] == b'_')
        {
            self.pos += 1;
        }
        let name = std::str::from_utf8(&self.src[start..self.pos]).ok()?;
        if matches!(name, "UINT64_C" | "INT64_C" | "UINT32_C" | "INT32_C") {
            if self.peek() != Some(b'(') {
                return None;
            }
            self.pos += 1;
            let val = self.parse_shift()?;
            if self.peek() != Some(b')') {
                return None;
            }
            self.pos += 1;
            return Some(val);
        }
        self.env.get(name).copied()
    }
}

fn eval_c_expr(expr: &str, env: &HashMap<String, i128>) -> Option<i128> {
    let mut p = ExprParser::new(expr, env);
    let val = p.parse_shift()?;
    p.skip_ws();
    if p.pos == p.src.len() {
        Some(val)
    } else {
        None
    }
}

fn extract_macro_calls(src: &str, macro_name: &str) -> Vec<Vec<String>> {
    let cleaned = strip_c_comments(src);
    let bytes = cleaned.as_bytes();
    let mut results = Vec::new();
    let mut i = 0;

    while i < bytes.len() {
        // Skip preprocessor lines starting with #
        let line_start = i == 0 || bytes[i - 1] == b'\n';
        if line_start {
            let mut j = i;
            while j < bytes.len() && (bytes[j] == b' ' || bytes[j] == b'\t') {
                j += 1;
            }
            if j < bytes.len() && bytes[j] == b'#' {
                while i < bytes.len() && bytes[i] != b'\n' {
                    i += 1;
                }
                i += 1;
                continue;
            }
        }
        if bytes[i..].starts_with(macro_name.as_bytes()) {
            let prev_ok = i == 0 || !(bytes[i - 1].is_ascii_alphanumeric() || bytes[i - 1] == b'_');
            let after = i + macro_name.len();
            if prev_ok && after < bytes.len() {
                let mut k = after;
                while k < bytes.len() && bytes[k].is_ascii_whitespace() {
                    k += 1;
                }
                if k < bytes.len() && bytes[k] == b'(' {
                    let (args, next_pos) = parse_macro_args(&cleaned, k + 1);
                    results.push(args);
                    i = next_pos;
                    continue;
                }
            }
        }
        i += 1;
    }
    results
}

fn parse_macro_args(src: &str, mut i: usize) -> (Vec<String>, usize) {
    let bytes = src.as_bytes();
    let mut args = Vec::new();
    let mut cur = String::new();
    let mut depth = 0usize;
    let mut in_string = false;

    while i < bytes.len() {
        let c = bytes[i];
        if in_string {
            cur.push(c as char);
            if c == b'\\' && i + 1 < bytes.len() {
                i += 1;
                cur.push(bytes[i] as char);
            } else if c == b'"' {
                in_string = false;
            }
            i += 1;
            continue;
        }
        match c {
            b'"' => {
                in_string = true;
                cur.push('"');
            }
            b'(' => {
                depth += 1;
                cur.push('(');
            }
            b')' => {
                if depth == 0 {
                    args.push(cur.trim().to_string());
                    return (args, i + 1);
                }
                depth -= 1;
                cur.push(')');
            }
            b',' if depth == 0 => {
                args.push(cur.trim().to_string());
                cur.clear();
            }
            _ => cur.push(c as char),
        }
        i += 1;
    }
    panic!("unterminated macro invocation");
}

fn unquote_c_string(s: &str) -> String {
    let trimmed = s.trim();
    assert!(
        trimmed.starts_with('"') && trimmed.ends_with('"') && trimmed.len() >= 2,
        "expected C string literal, got {s}"
    );
    let inner = &trimmed[1..trimmed.len() - 1];
    let mut out = String::with_capacity(inner.len());
    let mut chars = inner.chars();
    while let Some(c) = chars.next() {
        if c == '\\' {
            match chars.next().expect("escape char") {
                'n' => out.push('\n'),
                'r' => out.push('\r'),
                't' => out.push('\t'),
                '\\' => out.push('\\'),
                '"' => out.push('"'),
                other => panic!("unsupported escape \\{other} in {s}"),
            }
        } else {
            out.push(c);
        }
    }
    out
}

fn to_pascal_case(screaming: &str) -> String {
    screaming
        .split('_')
        .filter(|s| !s.is_empty())
        .map(|part| {
            let mut chars = part.chars();
            match chars.next() {
                None => String::new(),
                Some(first) => {
                    let mut s = String::new();
                    s.push(first.to_ascii_uppercase());
                    for c in chars {
                        s.push(c.to_ascii_lowercase());
                    }
                    s
                }
            }
        })
        .collect()
}

struct OpcodeEntry {
    id: String,
    variant: String,
    number: u16,
    name: String,
    fixed_count: u8,
    variable_limit: u16,
    tail: String,
    fixed: [String; 4],
}

fn parse_opcodes(src: &str, limits: &[LimitEntry]) -> Vec<OpcodeEntry> {
    let mut env: HashMap<String, i128> = limits.iter().map(|l| (l.name.clone(), l.value)).collect();
    env.insert("UINT16_MAX".to_string(), i128::from(u16::MAX));
    env.insert("UINT32_MAX".to_string(), i128::from(u32::MAX));

    extract_macro_calls(src, "OPCODE")
        .into_iter()
        .map(|args| {
            assert_eq!(args.len(), 10, "OPCODE expects 10 args: {args:?}");
            let id = args[0].clone();
            let variant = to_pascal_case(&id);
            let number = args[1].parse::<u16>().expect("opcode number");
            let name = unquote_c_string(&args[2]);
            let fixed_count = args[3].parse::<u8>().expect("fixed count");
            let limit_val = eval_c_expr(&args[4], &env)
                .unwrap_or_else(|| panic!("could not evaluate opcode limit {}", args[4]));
            let variable_limit = u16::try_from(limit_val).expect("variable_limit fits u16");
            OpcodeEntry {
                id,
                variant,
                number,
                name,
                fixed_count,
                variable_limit,
                tail: args[5].clone(),
                fixed: [
                    args[6].clone(),
                    args[7].clone(),
                    args[8].clone(),
                    args[9].clone(),
                ],
            }
        })
        .collect()
}

struct PrimEntry {
    id: String,
    variant: String,
    module: String,
    name: String,
    signature: String,
    params: String,
    bugs: String,
}

fn parse_prims(src: &str) -> Vec<PrimEntry> {
    extract_macro_calls(src, "PRIM")
        .into_iter()
        .map(|args| {
            assert_eq!(args.len(), 6, "PRIM expects 6 args: {args:?}");
            let id = args[0].clone();
            let variant = to_pascal_case(&id);
            PrimEntry {
                id,
                variant,
                module: unquote_c_string(&args[1]),
                name: unquote_c_string(&args[2]),
                signature: unquote_c_string(&args[3]),
                params: unquote_c_string(&args[4]),
                bugs: unquote_c_string(&args[5]),
            }
        })
        .collect()
}

struct HostOpEntry {
    id: String,
    variant: String,
    module: String,
    name: String,
    signature: String,
    params: String,
    bugs: String,
    spec_only: bool,
}

struct SimOpEntry {
    id: String,
    variant: String,
    module: String,
    name: String,
    spec_only: bool,
}

fn parse_hostops(src: &str) -> (Vec<HostOpEntry>, Vec<SimOpEntry>) {
    let hostops = extract_macro_calls(src, "HOSTOP")
        .into_iter()
        .map(|args| {
            assert_eq!(args.len(), 7, "HOSTOP expects 7 args: {args:?}");
            let id = args[0].clone();
            let variant = to_pascal_case(&id);
            let spec_only = match args[6].as_str() {
                "true" => true,
                "false" => false,
                other => panic!("invalid spec_only bool: {other}"),
            };
            HostOpEntry {
                id,
                variant,
                module: unquote_c_string(&args[1]),
                name: unquote_c_string(&args[2]),
                signature: unquote_c_string(&args[3]),
                params: unquote_c_string(&args[4]),
                bugs: unquote_c_string(&args[5]),
                spec_only,
            }
        })
        .collect();

    let simops = extract_macro_calls(src, "SIMOP")
        .into_iter()
        .map(|args| {
            assert_eq!(args.len(), 4, "SIMOP expects 4 args: {args:?}");
            let id = args[0].clone();
            let variant = to_pascal_case(&id);
            let spec_only = match args[3].as_str() {
                "true" => true,
                "false" => false,
                other => panic!("invalid simop spec_only bool: {other}"),
            };
            SimOpEntry {
                id,
                variant,
                module: unquote_c_string(&args[1]),
                name: unquote_c_string(&args[2]),
                spec_only,
            }
        })
        .collect();

    (hostops, simops)
}

fn desc_to_rust(desc: &str) -> &'static str {
    match desc {
        "NONE" => "OperandInfo::new(OperandKind::None, OperandAccess::None)",
        "W" => "OperandInfo::new(OperandKind::Register, OperandAccess::Write)",
        "R" => "OperandInfo::new(OperandKind::Register, OperandAccess::Read)",
        "C" => "OperandInfo::new(OperandKind::Register, OperandAccess::Consume)",
        "M" => "OperandInfo::new(OperandKind::Register, OperandAccess::Move)",
        "CALL" => "OperandInfo::new(OperandKind::Register, OperandAccess::CallParameter)",
        "PRIM" => "OperandInfo::new(OperandKind::Register, OperandAccess::PrimParameter)",
        "HOST" => "OperandInfo::new(OperandKind::Register, OperandAccess::HostParameter)",
        "TW" => "OperandInfo::new(OperandKind::Register, OperandAccess::TokenWrite)",
        "TC" => "OperandInfo::new(OperandKind::Register, OperandAccess::TokenConsume)",
        "TYPE" => "OperandInfo::new(OperandKind::Type, OperandAccess::None)",
        "LAYOUT" => "OperandInfo::new(OperandKind::Layout, OperandAccess::None)",
        "CONSTANT" => "OperandInfo::new(OperandKind::Constant, OperandAccess::None)",
        "GLOBAL" => "OperandInfo::new(OperandKind::Global, OperandAccess::None)",
        "FUNCTION" => "OperandInfo::new(OperandKind::Function, OperandAccess::None)",
        "PRIMITIVE" => "OperandInfo::new(OperandKind::Primitive, OperandAccess::None)",
        "HOSTOP" => "OperandInfo::new(OperandKind::Host, OperandAccess::None)",
        "TARGET" => "OperandInfo::new(OperandKind::Target, OperandAccess::None)",
        "FIELD" => "OperandInfo::new(OperandKind::Field, OperandAccess::None)",
        "CASE" => "OperandInfo::new(OperandKind::Case, OperandAccess::None)",
        "INDEX" => "OperandInfo::new(OperandKind::Index, OperandAccess::None)",
        other => panic!("unknown opcode operand descriptor: {other}"),
    }
}

fn emit_limits(out: &mut String, limits: &[LimitEntry]) {
    writeln!(out, "pub mod limits {{").unwrap();
    for entry in limits {
        if entry.signed {
            let v = i64::try_from(entry.value).expect("signed limit fits i64");
            writeln!(out, "    pub const {}: i64 = {v};", entry.name).unwrap();
        } else {
            let v = u64::try_from(entry.value).expect("unsigned limit fits u64");
            writeln!(out, "    pub const {}: u64 = {v};", entry.name).unwrap();
        }
    }
    writeln!(out, "}}\n").unwrap();
}

fn emit_opcodes(out: &mut String, opcodes: &[OpcodeEntry]) {
    writeln!(out, "pub const OPCODE_COUNT: usize = {};", opcodes.len()).unwrap();
    writeln!(
        out,
        "#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]\n#[repr(u16)]\npub enum Opcode {{"
    )
    .unwrap();
    for op in opcodes {
        writeln!(out, "    {} = {},", op.variant, op.number).unwrap();
    }
    writeln!(out, "}}\n").unwrap();

    writeln!(out, "impl Opcode {{").unwrap();
    writeln!(out, "    pub const fn from_u16(n: u16) -> Option<Self> {{").unwrap();
    writeln!(out, "        match n {{").unwrap();
    for op in opcodes {
        writeln!(
            out,
            "            {} => Some(Self::{}),",
            op.number, op.variant
        )
        .unwrap();
    }
    writeln!(out, "            _ => None,").unwrap();
    writeln!(out, "        }}").unwrap();
    writeln!(out, "    }}\n").unwrap();

    writeln!(out, "    pub const fn number(self) -> u16 {{").unwrap();
    writeln!(out, "        self as u16").unwrap();
    writeln!(out, "    }}\n").unwrap();

    writeln!(out, "    pub const fn info(self) -> &'static OpcodeInfo {{").unwrap();
    writeln!(out, "        &OPCODES[(self as usize) - 1]").unwrap();
    writeln!(out, "    }}").unwrap();
    writeln!(out, "}}\n").unwrap();

    writeln!(out, "pub static OPCODES: [OpcodeInfo; OPCODE_COUNT] = [").unwrap();
    for op in opcodes {
        writeln!(out, "    OpcodeInfo {{").unwrap();
        writeln!(out, "        opcode: Opcode::{},", op.variant).unwrap();
        writeln!(out, "        id: {:?},", op.id).unwrap();
        writeln!(out, "        number: {},", op.number).unwrap();
        writeln!(out, "        name: {:?},", op.name).unwrap();
        writeln!(out, "        fixed_count: {},", op.fixed_count).unwrap();
        writeln!(out, "        variable_limit: {},", op.variable_limit).unwrap();
        writeln!(out, "        variable: {},", desc_to_rust(&op.tail)).unwrap();
        writeln!(out, "        fixed: [").unwrap();
        for f in &op.fixed {
            writeln!(out, "            {},", desc_to_rust(f)).unwrap();
        }
        writeln!(out, "        ],").unwrap();
        writeln!(out, "    }},").unwrap();
    }
    writeln!(out, "];\n").unwrap();
}

fn emit_prims(out: &mut String, prims: &[PrimEntry]) {
    writeln!(out, "pub const PRIM_COUNT: usize = {};", prims.len()).unwrap();
    writeln!(
        out,
        "#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]\n#[repr(u32)]\npub enum PrimId {{"
    )
    .unwrap();
    for (idx, p) in prims.iter().enumerate() {
        writeln!(out, "    {} = {idx},", p.variant).unwrap();
    }
    writeln!(out, "}}\n").unwrap();

    writeln!(out, "impl PrimId {{").unwrap();
    writeln!(out, "    pub const fn from_u32(n: u32) -> Option<Self> {{").unwrap();
    writeln!(out, "        match n {{").unwrap();
    for (idx, p) in prims.iter().enumerate() {
        writeln!(out, "            {idx} => Some(Self::{}),", p.variant).unwrap();
    }
    writeln!(out, "            _ => None,").unwrap();
    writeln!(out, "        }}").unwrap();
    writeln!(out, "    }}\n").unwrap();

    writeln!(out, "    pub const fn info(self) -> &'static PrimInfo {{").unwrap();
    writeln!(out, "        &PRIMS[self as usize]").unwrap();
    writeln!(out, "    }}").unwrap();
    writeln!(out, "}}\n").unwrap();

    writeln!(out, "pub static PRIMS: [PrimInfo; PRIM_COUNT] = [").unwrap();
    for p in prims {
        writeln!(out, "    PrimInfo {{").unwrap();
        writeln!(out, "        prim: PrimId::{},", p.variant).unwrap();
        writeln!(out, "        id: {:?},", p.id).unwrap();
        writeln!(out, "        module: {:?},", p.module).unwrap();
        writeln!(out, "        name: {:?},", p.name).unwrap();
        writeln!(out, "        signature: {:?},", p.signature).unwrap();
        writeln!(out, "        params: {:?},", p.params).unwrap();
        writeln!(out, "        bugs: {:?},", p.bugs).unwrap();
        writeln!(out, "    }},").unwrap();
    }
    writeln!(out, "];\n").unwrap();
}

fn emit_hostops(out: &mut String, hostops: &[HostOpEntry]) {
    writeln!(out, "pub const HOSTOP_COUNT: usize = {};", hostops.len()).unwrap();
    writeln!(
        out,
        "#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]\n#[repr(u32)]\npub enum HostOpId {{"
    )
    .unwrap();
    for (idx, h) in hostops.iter().enumerate() {
        writeln!(out, "    {} = {idx},", h.variant).unwrap();
    }
    writeln!(out, "}}\n").unwrap();

    writeln!(out, "impl HostOpId {{").unwrap();
    writeln!(out, "    pub const fn from_u32(n: u32) -> Option<Self> {{").unwrap();
    writeln!(out, "        match n {{").unwrap();
    for (idx, h) in hostops.iter().enumerate() {
        writeln!(out, "            {idx} => Some(Self::{}),", h.variant).unwrap();
    }
    writeln!(out, "            _ => None,").unwrap();
    writeln!(out, "        }}").unwrap();
    writeln!(out, "    }}\n").unwrap();

    writeln!(out, "    pub const fn info(self) -> &'static HostOpInfo {{").unwrap();
    writeln!(out, "        &HOSTOPS[self as usize]").unwrap();
    writeln!(out, "    }}").unwrap();
    writeln!(out, "}}\n").unwrap();

    writeln!(out, "pub static HOSTOPS: [HostOpInfo; HOSTOP_COUNT] = [").unwrap();
    for h in hostops {
        writeln!(out, "    HostOpInfo {{").unwrap();
        writeln!(out, "        op: HostOpId::{},", h.variant).unwrap();
        writeln!(out, "        id: {:?},", h.id).unwrap();
        writeln!(out, "        module: {:?},", h.module).unwrap();
        writeln!(out, "        name: {:?},", h.name).unwrap();
        writeln!(out, "        signature: {:?},", h.signature).unwrap();
        writeln!(out, "        params: {:?},", h.params).unwrap();
        writeln!(out, "        bugs: {:?},", h.bugs).unwrap();
        writeln!(out, "        spec_only: {},", h.spec_only).unwrap();
        writeln!(out, "    }},").unwrap();
    }
    writeln!(out, "];\n").unwrap();
}

fn emit_simops(out: &mut String, simops: &[SimOpEntry]) {
    writeln!(out, "pub const SIMOP_COUNT: usize = {};", simops.len()).unwrap();
    writeln!(
        out,
        "#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]\n#[repr(u32)]\npub enum SimOpId {{"
    )
    .unwrap();
    for (idx, s) in simops.iter().enumerate() {
        writeln!(out, "    {} = {idx},", s.variant).unwrap();
    }
    writeln!(out, "}}\n").unwrap();

    writeln!(out, "impl SimOpId {{").unwrap();
    writeln!(out, "    pub const fn from_u32(n: u32) -> Option<Self> {{").unwrap();
    writeln!(out, "        match n {{").unwrap();
    for (idx, s) in simops.iter().enumerate() {
        writeln!(out, "            {idx} => Some(Self::{}),", s.variant).unwrap();
    }
    writeln!(out, "            _ => None,").unwrap();
    writeln!(out, "        }}").unwrap();
    writeln!(out, "    }}\n").unwrap();

    writeln!(out, "    pub const fn info(self) -> &'static SimOpInfo {{").unwrap();
    writeln!(out, "        &SIMOPS[self as usize]").unwrap();
    writeln!(out, "    }}").unwrap();
    writeln!(out, "}}\n").unwrap();

    writeln!(out, "pub static SIMOPS: [SimOpInfo; SIMOP_COUNT] = [").unwrap();
    for s in simops {
        writeln!(out, "    SimOpInfo {{").unwrap();
        writeln!(out, "        op: SimOpId::{},", s.variant).unwrap();
        writeln!(out, "        id: {:?},", s.id).unwrap();
        writeln!(out, "        module: {:?},", s.module).unwrap();
        writeln!(out, "        name: {:?},", s.name).unwrap();
        writeln!(out, "        spec_only: {},", s.spec_only).unwrap();
        writeln!(out, "    }},").unwrap();
    }
    writeln!(out, "];\n").unwrap();
}
