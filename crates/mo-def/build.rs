#![forbid(unsafe_code)]

use std::collections::HashMap;
use std::env;
use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};

type BuildResult<T> = Result<T, String>;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    run().map_err(|msg| -> Box<dyn std::error::Error> { msg.into() })
}

fn run() -> BuildResult<()> {
    let manifest_dir = PathBuf::from(
        env::var("CARGO_MANIFEST_DIR").map_err(|e| format!("CARGO_MANIFEST_DIR: {e}"))?,
    );
    let defs_dir = manifest_dir.join("defs");

    let limits_path = defs_dir.join("limits.h");
    let opcodes_path = defs_dir.join("opcodes.def");
    let prims_path = defs_dir.join("prims.def");
    let hostops_path = defs_dir.join("hostops.def");
    let codes_path = defs_dir.join("codes.h");
    let core_h_path = defs_dir.join("core.h");
    let core_ops_path = defs_dir.join("ops.def");
    let version_path = defs_dir.join("version.def");

    for path in [
        &limits_path,
        &opcodes_path,
        &prims_path,
        &hostops_path,
        &codes_path,
        &core_h_path,
        &core_ops_path,
        &version_path,
    ] {
        println!("cargo:rerun-if-changed={}", path.display());
    }

    let limits_src = read_file(&limits_path)?;
    let opcodes_src = read_file(&opcodes_path)?;
    let prims_src = read_file(&prims_path)?;
    let hostops_src = read_file(&hostops_path)?;
    let codes_src = read_file(&codes_path)?;
    let core_h_src = read_file(&core_h_path)?;
    let core_ops_src = read_file(&core_ops_path)?;
    let version_src = read_file(&version_path)?;

    let limits = parse_limits(&limits_src)?;
    let opcodes = parse_opcodes(&opcodes_src, &limits)?;
    let prims = parse_prims(&prims_src)?;
    let (hostops, simops) = parse_hostops(&hostops_src)?;
    let error_codes = parse_error_codes(&codes_src)?;
    let core_text_version = parse_core_text_version(&core_h_src)?;
    let (typed_ops, core_op_families) = parse_core_ops(&core_ops_src)?;
    let mo_version = parse_mo_version(&version_src)?;

    let out_dir = PathBuf::from(env::var("OUT_DIR").map_err(|e| format!("OUT_DIR: {e}"))?);
    let mut out = String::new();
    let mo_version_banner = format!("mo {mo_version}");
    writeln!(&mut out, "pub const MO_VERSION: &str = {mo_version:?};").map_err(fmt_err)?;
    writeln!(
        &mut out,
        "pub const MO_VERSION_BANNER: &str = {mo_version_banner:?};\n"
    )
    .map_err(fmt_err)?;
    emit_limits(&mut out, &limits)?;
    emit_opcodes(&mut out, &opcodes)?;
    emit_prims(&mut out, &prims)?;
    emit_hostops(&mut out, &hostops)?;
    emit_simops(&mut out, &simops)?;
    emit_error_codes(&mut out, &error_codes)?;
    emit_core_defs(&mut out, &core_text_version, &typed_ops, &core_op_families)?;

    fs::write(out_dir.join("generated.rs"), out).map_err(|e| format!("write generated.rs: {e}"))?;
    Ok(())
}

fn read_file(path: &Path) -> BuildResult<String> {
    fs::read_to_string(path).map_err(|e| format!("read {}: {e}", path.display()))
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

fn parse_limits(src: &str) -> BuildResult<Vec<LimitEntry>> {
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
            .ok_or_else(|| format!("could not evaluate limit macro {name} = {body}"))?;
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
    Ok(entries)
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

fn extract_macro_calls(src: &str, macro_name: &str) -> BuildResult<Vec<Vec<String>>> {
    let cleaned = strip_c_comments(src);
    extract_macro_calls_in_body(&cleaned, macro_name, true)
}

fn extract_macro_calls_in_body(
    src: &str,
    macro_name: &str,
    skip_preprocessor: bool,
) -> BuildResult<Vec<Vec<String>>> {
    let bytes = src.as_bytes();
    let mut results = Vec::new();
    let mut i = 0;

    while i < bytes.len() {
        if skip_preprocessor {
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
                    let (args, next_pos) = parse_macro_args(src, k + 1)?;
                    results.push(args);
                    i = next_pos;
                    continue;
                }
            }
        }
        i += 1;
    }
    Ok(results)
}

fn parse_macro_args(src: &str, mut i: usize) -> BuildResult<(Vec<String>, usize)> {
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
                    return Ok((args, i + 1));
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
    Err("unterminated macro invocation".to_string())
}

fn unquote_c_string(s: &str) -> BuildResult<String> {
    let trimmed = s.trim();
    if !(trimmed.starts_with('"') && trimmed.ends_with('"') && trimmed.len() >= 2) {
        return Err(format!("expected C string literal, got {s}"));
    }
    let inner = &trimmed[1..trimmed.len() - 1];
    let mut out = String::with_capacity(inner.len());
    let mut chars = inner.chars();
    while let Some(c) = chars.next() {
        if c == '\\' {
            match chars
                .next()
                .ok_or_else(|| format!("dangling escape in {s}"))?
            {
                'n' => out.push('\n'),
                'r' => out.push('\r'),
                't' => out.push('\t'),
                '\\' => out.push('\\'),
                '"' => out.push('"'),
                other => return Err(format!("unsupported escape \\{other} in {s}")),
            }
        } else {
            out.push(c);
        }
    }
    Ok(out)
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

fn parse_opcodes(src: &str, limits: &[LimitEntry]) -> BuildResult<Vec<OpcodeEntry>> {
    let mut env: HashMap<String, i128> = limits.iter().map(|l| (l.name.clone(), l.value)).collect();
    env.insert("UINT16_MAX".to_string(), i128::from(u16::MAX));
    env.insert("UINT32_MAX".to_string(), i128::from(u32::MAX));

    extract_macro_calls(src, "OPCODE")?
        .into_iter()
        .map(|args| {
            if args.len() != 10 {
                return Err(format!("OPCODE expects 10 args: {args:?}"));
            }
            let id = args[0].clone();
            let variant = to_pascal_case(&id);
            let number = args[1]
                .parse::<u16>()
                .map_err(|e| format!("opcode number {}: {e}", args[1]))?;
            let name = unquote_c_string(&args[2])?;
            let fixed_count = args[3]
                .parse::<u8>()
                .map_err(|e| format!("fixed count {}: {e}", args[3]))?;
            let limit_val = eval_c_expr(&args[4], &env)
                .ok_or_else(|| format!("could not evaluate opcode limit {}", args[4]))?;
            let variable_limit = u16::try_from(limit_val)
                .map_err(|e| format!("variable_limit {limit_val} fits u16: {e}"))?;
            Ok(OpcodeEntry {
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
            })
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

fn parse_prims(src: &str) -> BuildResult<Vec<PrimEntry>> {
    extract_macro_calls(src, "PRIM")?
        .into_iter()
        .map(|args| {
            if args.len() != 6 {
                return Err(format!("PRIM expects 6 args: {args:?}"));
            }
            let id = args[0].clone();
            let variant = to_pascal_case(&id);
            Ok(PrimEntry {
                id,
                variant,
                module: unquote_c_string(&args[1])?,
                name: unquote_c_string(&args[2])?,
                signature: unquote_c_string(&args[3])?,
                params: unquote_c_string(&args[4])?,
                bugs: unquote_c_string(&args[5])?,
            })
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

fn parse_hostops(src: &str) -> BuildResult<(Vec<HostOpEntry>, Vec<SimOpEntry>)> {
    let hostops = extract_macro_calls(src, "HOSTOP")?
        .into_iter()
        .map(|args| {
            if args.len() != 7 {
                return Err(format!("HOSTOP expects 7 args: {args:?}"));
            }
            let id = args[0].clone();
            let variant = to_pascal_case(&id);
            let spec_only = match args[6].as_str() {
                "true" => true,
                "false" => false,
                other => return Err(format!("invalid spec_only bool: {other}")),
            };
            Ok(HostOpEntry {
                id,
                variant,
                module: unquote_c_string(&args[1])?,
                name: unquote_c_string(&args[2])?,
                signature: unquote_c_string(&args[3])?,
                params: unquote_c_string(&args[4])?,
                bugs: unquote_c_string(&args[5])?,
                spec_only,
            })
        })
        .collect::<BuildResult<Vec<_>>>()?;

    let simops = extract_macro_calls(src, "SIMOP")?
        .into_iter()
        .map(|args| {
            if args.len() != 4 {
                return Err(format!("SIMOP expects 4 args: {args:?}"));
            }
            let id = args[0].clone();
            let variant = to_pascal_case(&id);
            let spec_only = match args[3].as_str() {
                "true" => true,
                "false" => false,
                other => return Err(format!("invalid simop spec_only bool: {other}")),
            };
            Ok(SimOpEntry {
                id,
                variant,
                module: unquote_c_string(&args[1])?,
                name: unquote_c_string(&args[2])?,
                spec_only,
            })
        })
        .collect::<BuildResult<Vec<_>>>()?;

    Ok((hostops, simops))
}

struct ErrorCodeEntry {
    number: u16,
    id: String,
    variant: String,
    section: Option<String>,
}

fn find_define_body(src: &str, prefix: &str) -> BuildResult<String> {
    // C translation phase 2 splices `\<newline>` BEFORE phase 3 strips comments.
    for line in join_continuations(src) {
        let cleaned = strip_c_comments(&line);
        let trimmed = cleaned.trim();
        if let Some(rest) = trimmed.strip_prefix(prefix) {
            return Ok(rest.trim().to_string());
        }
    }
    Err(format!("missing macro definition starting with {prefix}"))
}

fn parse_error_codes(src: &str) -> BuildResult<Vec<ErrorCodeEntry>> {
    let body = find_define_body(src, "#define MO_CODES(X)")?;
    let calls = extract_macro_calls_in_body(&body, "X", false)?;
    if calls.is_empty() {
        return Err("MO_CODES(X) has no entries".to_string());
    }
    let mut entries = Vec::with_capacity(calls.len());
    for args in calls {
        if args.len() != 3 {
            return Err(format!("MO_CODES X expects 3 args: {args:?}"));
        }
        let number = args[0]
            .parse::<u16>()
            .map_err(|e| format!("error code number {}: {e}", args[0]))?;
        if !(1..=9999).contains(&number) {
            return Err(format!("error code number {number} out of 1..=9999"));
        }
        let id = args[1].clone();
        let variant = to_pascal_case(&id);
        let section = if args[2] == "NULL" {
            None
        } else {
            Some(unquote_c_string(&args[2])?)
        };
        entries.push(ErrorCodeEntry {
            number,
            id,
            variant,
            section,
        });
    }
    Ok(entries)
}

fn parse_core_text_version(src: &str) -> BuildResult<String> {
    let body = find_define_body(src, "#define MO_CORE_TEXT_VERSION")?;
    unquote_c_string(&body)
}

struct CoreOpEntry {
    id: String,
    variant: String,
    wire: String,
}

struct CoreOpFamily {
    enum_name: &'static str,
    entries: Vec<CoreOpEntry>,
}

fn parse_core_ops(src: &str) -> BuildResult<(Vec<CoreOpEntry>, Vec<CoreOpFamily>)> {
    let typed_body = find_define_body(src, "#define MO_CORE_TYPED_OPS(X)")?;
    let typed_calls = extract_macro_calls_in_body(&typed_body, "X", false)?;
    let mut typed_ops = Vec::with_capacity(typed_calls.len());
    for args in typed_calls {
        if args.len() != 3 {
            return Err(format!("MO_CORE_TYPED_OPS X expects 3 args: {args:?}"));
        }
        let id = args[0].clone();
        typed_ops.push(CoreOpEntry {
            variant: to_pascal_case(&id),
            id,
            wire: unquote_c_string(&args[2])?,
        });
    }

    let family_specs: [(&str, &'static str); 18] = [
        ("#define MO_CORE_INT_OPS(X)", "CoreIntOp"),
        ("#define MO_CORE_BIG_OPS(X)", "CoreBigOp"),
        ("#define MO_CORE_BYTE_OPS(X)", "CoreByteOp"),
        ("#define MO_CORE_DEC_OPS(X)", "CoreDecOp"),
        ("#define MO_CORE_FLOAT_OPS(X)", "CoreFloatOp"),
        ("#define MO_CORE_BASES(X)", "CoreBase"),
        ("#define MO_CORE_CMP_OPS(X)", "CoreCmpOp"),
        ("#define MO_CORE_CHAR_OPS(X)", "CoreCharOp"),
        ("#define MO_CORE_TEXT_OPS(X)", "CoreTextOp"),
        ("#define MO_CORE_BYTES_OPS(X)", "CoreBytesOp"),
        ("#define MO_CORE_UNICODE_OPS(X)", "CoreUnicodeOp"),
        ("#define MO_CORE_CRYPTO_OPS(X)", "CoreCryptoOp"),
        ("#define MO_CORE_RANGE_OPS(X)", "CoreRangeOp"),
        ("#define MO_CORE_TIME_OPS(X)", "CoreTimeOp"),
        ("#define MO_CORE_LIST_OPS(X)", "CoreListNullaryOp"),
        ("#define MO_CORE_MAP_OPS(X)", "CoreMapNullaryOp"),
        ("#define MO_CORE_SET_OPS(X)", "CoreSetOp"),
        ("#define MO_CORE_IO_OPS(X)", "CoreIoNullaryOp"),
    ];

    let mut families = Vec::with_capacity(family_specs.len());
    for (define_prefix, enum_name) in family_specs {
        let body = find_define_body(src, define_prefix)?;
        let calls = extract_macro_calls_in_body(&body, "X", false)?;
        let mut entries = Vec::with_capacity(calls.len());
        for args in calls {
            if args.len() != 2 {
                return Err(format!("{define_prefix} X expects 2 args: {args:?}"));
            }
            let id = args[0].clone();
            entries.push(CoreOpEntry {
                variant: to_pascal_case(&id),
                id,
                wire: unquote_c_string(&args[1])?,
            });
        }
        families.push(CoreOpFamily { enum_name, entries });
    }

    Ok((typed_ops, families))
}

fn desc_to_rust(desc: &str) -> BuildResult<&'static str> {
    match desc {
        "NONE" => Ok("OperandInfo::new(OperandKind::None, OperandAccess::None)"),
        "W" => Ok("OperandInfo::new(OperandKind::Register, OperandAccess::Write)"),
        "R" => Ok("OperandInfo::new(OperandKind::Register, OperandAccess::Read)"),
        "C" => Ok("OperandInfo::new(OperandKind::Register, OperandAccess::Consume)"),
        "M" => Ok("OperandInfo::new(OperandKind::Register, OperandAccess::Move)"),
        "CALL" => Ok("OperandInfo::new(OperandKind::Register, OperandAccess::CallParameter)"),
        "PRIM" => Ok("OperandInfo::new(OperandKind::Register, OperandAccess::PrimParameter)"),
        "HOST" => Ok("OperandInfo::new(OperandKind::Register, OperandAccess::HostParameter)"),
        "TW" => Ok("OperandInfo::new(OperandKind::Register, OperandAccess::TokenWrite)"),
        "TC" => Ok("OperandInfo::new(OperandKind::Register, OperandAccess::TokenConsume)"),
        "TYPE" => Ok("OperandInfo::new(OperandKind::Type, OperandAccess::None)"),
        "LAYOUT" => Ok("OperandInfo::new(OperandKind::Layout, OperandAccess::None)"),
        "CONSTANT" => Ok("OperandInfo::new(OperandKind::Constant, OperandAccess::None)"),
        "GLOBAL" => Ok("OperandInfo::new(OperandKind::Global, OperandAccess::None)"),
        "FUNCTION" => Ok("OperandInfo::new(OperandKind::Function, OperandAccess::None)"),
        "PRIMITIVE" => Ok("OperandInfo::new(OperandKind::Primitive, OperandAccess::None)"),
        "HOSTOP" => Ok("OperandInfo::new(OperandKind::Host, OperandAccess::None)"),
        "TARGET" => Ok("OperandInfo::new(OperandKind::Target, OperandAccess::None)"),
        "FIELD" => Ok("OperandInfo::new(OperandKind::Field, OperandAccess::None)"),
        "CASE" => Ok("OperandInfo::new(OperandKind::Case, OperandAccess::None)"),
        "INDEX" => Ok("OperandInfo::new(OperandKind::Index, OperandAccess::None)"),
        other => Err(format!("unknown opcode operand descriptor: {other}")),
    }
}

fn fmt_err(e: std::fmt::Error) -> String {
    format!("format generated.rs: {e}")
}

fn emit_limits(out: &mut String, limits: &[LimitEntry]) -> BuildResult<()> {
    writeln!(out, "pub mod limits {{").map_err(fmt_err)?;
    for entry in limits {
        if entry.signed {
            let v = i64::try_from(entry.value)
                .map_err(|e| format!("signed limit {} fits i64: {e}", entry.name))?;
            writeln!(out, "    pub const {}: i64 = {v};", entry.name).map_err(fmt_err)?;
        } else {
            let v = u64::try_from(entry.value)
                .map_err(|e| format!("unsigned limit {} fits u64: {e}", entry.name))?;
            writeln!(out, "    pub const {}: u64 = {v};", entry.name).map_err(fmt_err)?;
        }
    }
    writeln!(out, "}}\n").map_err(fmt_err)?;
    Ok(())
}

fn emit_opcodes(out: &mut String, opcodes: &[OpcodeEntry]) -> BuildResult<()> {
    writeln!(out, "pub const OPCODE_COUNT: usize = {};", opcodes.len()).map_err(fmt_err)?;
    writeln!(
        out,
        "#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]\n#[repr(u16)]\npub enum Opcode {{"
    )
    .map_err(fmt_err)?;
    for op in opcodes {
        writeln!(out, "    {} = {},", op.variant, op.number).map_err(fmt_err)?;
    }
    writeln!(out, "}}\n").map_err(fmt_err)?;

    writeln!(out, "impl Opcode {{").map_err(fmt_err)?;
    writeln!(out, "    pub const fn from_u16(n: u16) -> Option<Self> {{").map_err(fmt_err)?;
    writeln!(out, "        match n {{").map_err(fmt_err)?;
    for op in opcodes {
        writeln!(
            out,
            "            {} => Some(Self::{}),",
            op.number, op.variant
        )
        .map_err(fmt_err)?;
    }
    writeln!(out, "            _ => None,").map_err(fmt_err)?;
    writeln!(out, "        }}").map_err(fmt_err)?;
    writeln!(out, "    }}\n").map_err(fmt_err)?;

    writeln!(out, "    pub const fn number(self) -> u16 {{").map_err(fmt_err)?;
    writeln!(out, "        self as u16").map_err(fmt_err)?;
    writeln!(out, "    }}\n").map_err(fmt_err)?;

    writeln!(out, "    pub const fn info(self) -> &'static OpcodeInfo {{").map_err(fmt_err)?;
    writeln!(out, "        &OPCODES[(self as usize) - 1]").map_err(fmt_err)?;
    writeln!(out, "    }}").map_err(fmt_err)?;
    writeln!(out, "}}\n").map_err(fmt_err)?;

    writeln!(out, "pub static OPCODES: [OpcodeInfo; OPCODE_COUNT] = [").map_err(fmt_err)?;
    for op in opcodes {
        writeln!(out, "    OpcodeInfo {{").map_err(fmt_err)?;
        writeln!(out, "        opcode: Opcode::{},", op.variant).map_err(fmt_err)?;
        writeln!(out, "        id: {:?},", op.id).map_err(fmt_err)?;
        writeln!(out, "        number: {},", op.number).map_err(fmt_err)?;
        writeln!(out, "        name: {:?},", op.name).map_err(fmt_err)?;
        writeln!(out, "        fixed_count: {},", op.fixed_count).map_err(fmt_err)?;
        writeln!(out, "        variable_limit: {},", op.variable_limit).map_err(fmt_err)?;
        writeln!(out, "        variable: {},", desc_to_rust(&op.tail)?).map_err(fmt_err)?;
        writeln!(out, "        fixed: [").map_err(fmt_err)?;
        for f in &op.fixed {
            writeln!(out, "            {},", desc_to_rust(f)?).map_err(fmt_err)?;
        }
        writeln!(out, "        ],").map_err(fmt_err)?;
        writeln!(out, "    }},").map_err(fmt_err)?;
    }
    writeln!(out, "];\n").map_err(fmt_err)?;
    Ok(())
}

fn emit_prims(out: &mut String, prims: &[PrimEntry]) -> BuildResult<()> {
    writeln!(out, "pub const PRIM_COUNT: usize = {};", prims.len()).map_err(fmt_err)?;
    writeln!(
        out,
        "#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]\n#[repr(u32)]\npub enum PrimId {{"
    )
    .map_err(fmt_err)?;
    for (idx, p) in prims.iter().enumerate() {
        writeln!(out, "    {} = {idx},", p.variant).map_err(fmt_err)?;
    }
    writeln!(out, "}}\n").map_err(fmt_err)?;

    writeln!(out, "impl PrimId {{").map_err(fmt_err)?;
    writeln!(out, "    pub const fn from_u32(n: u32) -> Option<Self> {{").map_err(fmt_err)?;
    writeln!(out, "        match n {{").map_err(fmt_err)?;
    for (idx, p) in prims.iter().enumerate() {
        writeln!(out, "            {idx} => Some(Self::{}),", p.variant).map_err(fmt_err)?;
    }
    writeln!(out, "            _ => None,").map_err(fmt_err)?;
    writeln!(out, "        }}").map_err(fmt_err)?;
    writeln!(out, "    }}\n").map_err(fmt_err)?;

    writeln!(out, "    pub const fn info(self) -> &'static PrimInfo {{").map_err(fmt_err)?;
    writeln!(out, "        &PRIMS[self as usize]").map_err(fmt_err)?;
    writeln!(out, "    }}").map_err(fmt_err)?;
    writeln!(out, "}}\n").map_err(fmt_err)?;

    writeln!(out, "pub static PRIMS: [PrimInfo; PRIM_COUNT] = [").map_err(fmt_err)?;
    for p in prims {
        writeln!(out, "    PrimInfo {{").map_err(fmt_err)?;
        writeln!(out, "        prim: PrimId::{},", p.variant).map_err(fmt_err)?;
        writeln!(out, "        id: {:?},", p.id).map_err(fmt_err)?;
        writeln!(out, "        module: {:?},", p.module).map_err(fmt_err)?;
        writeln!(out, "        name: {:?},", p.name).map_err(fmt_err)?;
        writeln!(out, "        signature: {:?},", p.signature).map_err(fmt_err)?;
        writeln!(out, "        params: {:?},", p.params).map_err(fmt_err)?;
        writeln!(out, "        bugs: {:?},", p.bugs).map_err(fmt_err)?;
        writeln!(out, "    }},").map_err(fmt_err)?;
    }
    writeln!(out, "];\n").map_err(fmt_err)?;
    Ok(())
}

fn emit_hostops(out: &mut String, hostops: &[HostOpEntry]) -> BuildResult<()> {
    writeln!(out, "pub const HOSTOP_COUNT: usize = {};", hostops.len()).map_err(fmt_err)?;
    writeln!(
        out,
        "#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]\n#[repr(u32)]\npub enum HostOpId {{"
    )
    .map_err(fmt_err)?;
    for (idx, h) in hostops.iter().enumerate() {
        writeln!(out, "    {} = {idx},", h.variant).map_err(fmt_err)?;
    }
    writeln!(out, "}}\n").map_err(fmt_err)?;

    writeln!(out, "impl HostOpId {{").map_err(fmt_err)?;
    writeln!(out, "    pub const fn from_u32(n: u32) -> Option<Self> {{").map_err(fmt_err)?;
    writeln!(out, "        match n {{").map_err(fmt_err)?;
    for (idx, h) in hostops.iter().enumerate() {
        writeln!(out, "            {idx} => Some(Self::{}),", h.variant).map_err(fmt_err)?;
    }
    writeln!(out, "            _ => None,").map_err(fmt_err)?;
    writeln!(out, "        }}").map_err(fmt_err)?;
    writeln!(out, "    }}\n").map_err(fmt_err)?;

    writeln!(out, "    pub const fn info(self) -> &'static HostOpInfo {{").map_err(fmt_err)?;
    writeln!(out, "        &HOSTOPS[self as usize]").map_err(fmt_err)?;
    writeln!(out, "    }}").map_err(fmt_err)?;
    writeln!(out, "}}\n").map_err(fmt_err)?;

    writeln!(out, "pub static HOSTOPS: [HostOpInfo; HOSTOP_COUNT] = [").map_err(fmt_err)?;
    for h in hostops {
        writeln!(out, "    HostOpInfo {{").map_err(fmt_err)?;
        writeln!(out, "        op: HostOpId::{},", h.variant).map_err(fmt_err)?;
        writeln!(out, "        id: {:?},", h.id).map_err(fmt_err)?;
        writeln!(out, "        module: {:?},", h.module).map_err(fmt_err)?;
        writeln!(out, "        name: {:?},", h.name).map_err(fmt_err)?;
        writeln!(out, "        signature: {:?},", h.signature).map_err(fmt_err)?;
        writeln!(out, "        params: {:?},", h.params).map_err(fmt_err)?;
        writeln!(out, "        bugs: {:?},", h.bugs).map_err(fmt_err)?;
        writeln!(out, "        spec_only: {},", h.spec_only).map_err(fmt_err)?;
        writeln!(out, "    }},").map_err(fmt_err)?;
    }
    writeln!(out, "];\n").map_err(fmt_err)?;
    Ok(())
}

fn emit_simops(out: &mut String, simops: &[SimOpEntry]) -> BuildResult<()> {
    writeln!(out, "pub const SIMOP_COUNT: usize = {};", simops.len()).map_err(fmt_err)?;
    writeln!(
        out,
        "#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]\n#[repr(u32)]\npub enum SimOpId {{"
    )
    .map_err(fmt_err)?;
    for (idx, s) in simops.iter().enumerate() {
        writeln!(out, "    {} = {idx},", s.variant).map_err(fmt_err)?;
    }
    writeln!(out, "}}\n").map_err(fmt_err)?;

    writeln!(out, "impl SimOpId {{").map_err(fmt_err)?;
    writeln!(out, "    pub const fn from_u32(n: u32) -> Option<Self> {{").map_err(fmt_err)?;
    writeln!(out, "        match n {{").map_err(fmt_err)?;
    for (idx, s) in simops.iter().enumerate() {
        writeln!(out, "            {idx} => Some(Self::{}),", s.variant).map_err(fmt_err)?;
    }
    writeln!(out, "            _ => None,").map_err(fmt_err)?;
    writeln!(out, "        }}").map_err(fmt_err)?;
    writeln!(out, "    }}\n").map_err(fmt_err)?;

    writeln!(out, "    pub const fn info(self) -> &'static SimOpInfo {{").map_err(fmt_err)?;
    writeln!(out, "        &SIMOPS[self as usize]").map_err(fmt_err)?;
    writeln!(out, "    }}").map_err(fmt_err)?;
    writeln!(out, "}}\n").map_err(fmt_err)?;

    writeln!(out, "pub static SIMOPS: [SimOpInfo; SIMOP_COUNT] = [").map_err(fmt_err)?;
    for s in simops {
        writeln!(out, "    SimOpInfo {{").map_err(fmt_err)?;
        writeln!(out, "        op: SimOpId::{},", s.variant).map_err(fmt_err)?;
        writeln!(out, "        id: {:?},", s.id).map_err(fmt_err)?;
        writeln!(out, "        module: {:?},", s.module).map_err(fmt_err)?;
        writeln!(out, "        name: {:?},", s.name).map_err(fmt_err)?;
        writeln!(out, "        spec_only: {},", s.spec_only).map_err(fmt_err)?;
        writeln!(out, "    }},").map_err(fmt_err)?;
    }
    writeln!(out, "];\n").map_err(fmt_err)?;
    Ok(())
}

fn emit_error_codes(out: &mut String, codes: &[ErrorCodeEntry]) -> BuildResult<()> {
    writeln!(out, "pub const ERROR_CODE_COUNT: usize = {};", codes.len()).map_err(fmt_err)?;
    writeln!(
        out,
        "#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]\n#[repr(u16)]\npub enum ErrorCode {{"
    )
    .map_err(fmt_err)?;
    for c in codes {
        writeln!(out, "    {} = {},", c.variant, c.number).map_err(fmt_err)?;
    }
    writeln!(out, "}}\n").map_err(fmt_err)?;

    writeln!(out, "impl ErrorCode {{").map_err(fmt_err)?;
    writeln!(out, "    pub const fn from_u16(n: u16) -> Option<Self> {{").map_err(fmt_err)?;
    writeln!(out, "        match n {{").map_err(fmt_err)?;
    for c in codes {
        writeln!(
            out,
            "            {} => Some(Self::{}),",
            c.number, c.variant
        )
        .map_err(fmt_err)?;
    }
    writeln!(out, "            _ => None,").map_err(fmt_err)?;
    writeln!(out, "        }}").map_err(fmt_err)?;
    writeln!(out, "    }}\n").map_err(fmt_err)?;

    writeln!(out, "    pub const fn from_u32(n: u32) -> Option<Self> {{").map_err(fmt_err)?;
    writeln!(
        out,
        "        if n > u16::MAX as u32 {{ None }} else {{ Self::from_u16(n as u16) }}"
    )
    .map_err(fmt_err)?;
    writeln!(out, "    }}\n").map_err(fmt_err)?;

    writeln!(out, "    pub const fn number(self) -> u16 {{").map_err(fmt_err)?;
    writeln!(out, "        self as u16").map_err(fmt_err)?;
    writeln!(out, "    }}\n").map_err(fmt_err)?;

    writeln!(
        out,
        "    pub const fn info(self) -> &'static ErrorCodeInfo {{"
    )
    .map_err(fmt_err)?;
    writeln!(out, "        match self {{").map_err(fmt_err)?;
    for (idx, c) in codes.iter().enumerate() {
        writeln!(
            out,
            "            Self::{} => &ERROR_CODES[{idx}],",
            c.variant
        )
        .map_err(fmt_err)?;
    }
    writeln!(out, "        }}").map_err(fmt_err)?;
    writeln!(out, "    }}\n").map_err(fmt_err)?;

    writeln!(out, "    pub const fn id(self) -> &'static str {{").map_err(fmt_err)?;
    writeln!(out, "        self.info().id").map_err(fmt_err)?;
    writeln!(out, "    }}\n").map_err(fmt_err)?;

    writeln!(
        out,
        "    pub const fn section(self) -> Option<&'static str> {{"
    )
    .map_err(fmt_err)?;
    writeln!(out, "        self.info().section").map_err(fmt_err)?;
    writeln!(out, "    }}").map_err(fmt_err)?;
    writeln!(out, "}}\n").map_err(fmt_err)?;

    writeln!(
        out,
        "pub static ERROR_CODES: [ErrorCodeInfo; ERROR_CODE_COUNT] = ["
    )
    .map_err(fmt_err)?;
    for c in codes {
        writeln!(out, "    ErrorCodeInfo {{").map_err(fmt_err)?;
        writeln!(out, "        code: ErrorCode::{},", c.variant).map_err(fmt_err)?;
        writeln!(out, "        number: {},", c.number).map_err(fmt_err)?;
        writeln!(out, "        id: {:?},", c.id).map_err(fmt_err)?;
        match &c.section {
            Some(sec) => writeln!(out, "        section: Some({sec:?}),").map_err(fmt_err)?,
            None => writeln!(out, "        section: None,").map_err(fmt_err)?,
        }
        writeln!(out, "    }},").map_err(fmt_err)?;
    }
    writeln!(out, "];\n").map_err(fmt_err)?;
    Ok(())
}

fn emit_core_op_enum(
    out: &mut String,
    enum_name: &str,
    entries: &[CoreOpEntry],
) -> BuildResult<()> {
    writeln!(
        out,
        "    #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]\n    pub enum {enum_name} {{"
    )
    .map_err(fmt_err)?;
    for e in entries {
        writeln!(out, "        {},", e.variant).map_err(fmt_err)?;
    }
    writeln!(out, "    }}\n").map_err(fmt_err)?;

    writeln!(out, "    impl {enum_name} {{").map_err(fmt_err)?;
    writeln!(out, "        pub const ALL: &'static [Self] = &[").map_err(fmt_err)?;
    for e in entries {
        writeln!(out, "            Self::{},", e.variant).map_err(fmt_err)?;
    }
    writeln!(out, "        ];\n").map_err(fmt_err)?;

    writeln!(
        out,
        "        pub const fn wire_name(self) -> &'static str {{"
    )
    .map_err(fmt_err)?;
    writeln!(out, "            match self {{").map_err(fmt_err)?;
    for e in entries {
        writeln!(out, "                Self::{} => {:?},", e.variant, e.wire).map_err(fmt_err)?;
    }
    writeln!(out, "            }}").map_err(fmt_err)?;
    writeln!(out, "        }}\n").map_err(fmt_err)?;

    writeln!(out, "        pub const fn id(self) -> &'static str {{").map_err(fmt_err)?;
    writeln!(out, "            match self {{").map_err(fmt_err)?;
    for e in entries {
        writeln!(out, "                Self::{} => {:?},", e.variant, e.id).map_err(fmt_err)?;
    }
    writeln!(out, "            }}").map_err(fmt_err)?;
    writeln!(out, "        }}\n").map_err(fmt_err)?;

    writeln!(
        out,
        "        pub fn from_wire_name(s: &str) -> Option<Self> {{"
    )
    .map_err(fmt_err)?;
    writeln!(out, "            match s {{").map_err(fmt_err)?;
    for e in entries {
        writeln!(
            out,
            "                {:?} => Some(Self::{}),",
            e.wire, e.variant
        )
        .map_err(fmt_err)?;
    }
    writeln!(out, "                _ => None,").map_err(fmt_err)?;
    writeln!(out, "            }}").map_err(fmt_err)?;
    writeln!(out, "        }}").map_err(fmt_err)?;
    writeln!(out, "    }}\n").map_err(fmt_err)?;
    Ok(())
}

fn emit_core_defs(
    out: &mut String,
    core_text_version: &str,
    typed_ops: &[CoreOpEntry],
    families: &[CoreOpFamily],
) -> BuildResult<()> {
    writeln!(
        out,
        "pub const MO_CORE_TEXT_VERSION: &str = {core_text_version:?};\n"
    )
    .map_err(fmt_err)?;
    writeln!(out, "pub mod core_ops {{").map_err(fmt_err)?;
    emit_core_op_enum(out, "CoreTypedOp", typed_ops)?;
    for family in families {
        emit_core_op_enum(out, family.enum_name, &family.entries)?;
    }
    writeln!(out, "}}\n").map_err(fmt_err)?;
    Ok(())
}

fn parse_mo_version(src: &str) -> BuildResult<String> {
    for line in src.lines() {
        let trimmed = line.trim();
        if let Some(rest) = trimmed.strip_prefix("#define MO_VERSION") {
            let rest = rest.trim();
            if let Some(inner) = rest.strip_prefix('"').and_then(|s| s.strip_suffix('"')) {
                return Ok(inner.to_string());
            }
        }
    }
    Err("could not find #define MO_VERSION in crates/mo-def/defs/version.def".to_string())
}
