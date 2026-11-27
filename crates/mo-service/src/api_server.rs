//! Multi-worker HTTP/1.1 server for `api` blocks (`src/tools/api-server.c` parity).
//!
//! Handles `mo serve FILE` when the entry module declares an `api` block:
//! - HTTP/1.1 keep-alive, `Content-Length` & `Transfer-Encoding: chunked` body parsing
//! - Confined static asset mounts (`ETag`, `If-None-Match` 304, `Cache-Control`, MIME types)
//! - CORS headers, worker pool, request/job logging, background scheduled jobs, `--watch` reload
//! - Safe per-worker `mo_vm::Image` execution (plus serialized live actor handling when declared)

use std::collections::VecDeque;
use std::io::{Read, Write};
use std::net::{IpAddr, SocketAddr, TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use mo_base::{
    DiagBag, Diagnostic, DisplayMetrics, RenderFormat, SourceMap, Span, TerminalEncoding,
    render_diagnostic_default,
};
use mo_bytecode::{Capability, load as decode_file};
use mo_check::{
    LibraryMode, LoadOptions, check_program, default_embedded_library, load_program_bytes,
};
use mo_codegen::compile_services;
use mo_core::check_program_services;
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_CLI_CHECK_SIZE, MO_LIMIT_API_BODY_BYTES, MO_LIMIT_API_CHUNK_OVERHEAD_BYTES,
    MO_LIMIT_API_HEADER_BYTES, MO_LIMIT_API_HEADERS, MO_LIMIT_API_JOB_POLL_US,
    MO_LIMIT_API_JOB_WORK, MO_LIMIT_API_KEEPALIVE_IDLE_US, MO_LIMIT_API_KEEPALIVE_REQUESTS,
    MO_LIMIT_API_QUEUE, MO_LIMIT_API_READ_US, MO_LIMIT_API_REQUEST_HEAP, MO_LIMIT_API_REQUEST_WORK,
    MO_LIMIT_API_SSE_PUBLISH_PER_RESPONSE, MO_LIMIT_API_WATCH_POLL_US, MO_LIMIT_API_WORKERS,
    MO_LIMIT_API_WORKERS_MAX, MO_LIMIT_API_WRITE_US, MO_LIMIT_HOST_PATH_SIZE,
    MO_LIMIT_STATIC_FILE_BYTES, MO_SOURCE_STACK_SIZE,
};
use mo_host::HostContext;
use mo_lower::lower_services;
use mo_syntax::{SyntaxKind, parse_module};
use mo_vm::{Image, Limits, Outcome, Process};

use crate::listener::signals::{install_signal_handlers, is_signalled, reset_signal};
use crate::sse_broker::{
    SSE_PUBLISH_HEADER, SSE_SUBSCRIBE_HEADER, SseBroker, SsePublishSpec, SseSubscribeSpec,
    encode_http_chunk, parse_publish_header, parse_subscribe_header,
};
use crate::sse_relay::{SSE_RELAY_HEADER, SseRelayPool, SseRelaySpec, parse_relay_header};

const UNICODE_METRICS: DisplayMetrics = DisplayMetrics {
    next_grapheme: mo_prim::unicode16::next_grapheme_bytes,
    display_width: mo_prim::unicode16::display_width_bytes,
};

/// Options for running `mo serve` on an `api` block.
pub struct ApiServeOptions<'a> {
    /// Entry `.mo` file path as raw OS bytes.
    pub entry_file: &'a [u8],
    /// Optional data / root folder (`--data`, defaults to `.`).
    pub data_dir: Option<&'a Path>,
    /// Optional explicit port (`--port`).
    pub port: Option<u16>,
    /// Optional bind IPv4 address (`--bind`, defaults to `127.0.0.1`).
    pub bind_host: Option<&'a str>,
    /// Optional worker count (`--workers`, defaults to `MO_LIMIT_API_WORKERS`).
    pub workers: Option<u32>,
    /// Whether `--watch` hot-reload is enabled.
    pub watch: bool,
    /// Optional explicit library directory (`--lib`).
    pub library: Option<&'a Path>,
    /// Optional path where readiness JSON is written atomically once listening (`--ready-file`).
    pub ready_file: Option<&'a Path>,
    /// VM limits.
    pub limits: Limits,
}

/// Checks whether the entry module at `entry_path` declares a top-level `api` block
/// (`src/tools/api-server.c:probe_entry_api`).
#[must_use]
pub fn probe_entry_api(entry_path: &[u8]) -> bool {
    let path_str = String::from_utf8_lossy(entry_path).into_owned();
    let mut sources = SourceMap::new();
    let Ok(file_id) = sources.add_file(Path::new(&path_str)) else {
        return false;
    };
    let mut diags = DiagBag::new();
    let Some(parsed) = parse_module(&sources, file_id, &mut diags) else {
        return false;
    };
    let root = parsed.tree.node(parsed.root);
    let Some(items_id) = root.children.get(1).copied().flatten() else {
        return false;
    };
    let items = parsed.tree.node(items_id);
    items
        .children
        .iter()
        .flatten()
        .any(|&child| parsed.tree.node(child).kind == SyntaxKind::Api)
}

#[derive(Clone)]
struct ApiStaticMount {
    prefix: String,
    dir: String,
}

#[derive(Clone)]
struct ApiJobMeta {
    name: String,
    has_schedule: bool,
    interval_us: u64,
}

#[derive(Clone)]
struct ApiGeneration {
    id: u64,
    wire: Arc<Vec<u8>>,
    wire_fn: u32,
    job_fn: u32,
    default_port: u16,
    cors: Option<String>,
    statics: Vec<ApiStaticMount>,
    jobs: Vec<ApiJobMeta>,
    actor_services: Vec<u32>,
}

#[derive(Clone)]
struct WatchedFile {
    path: PathBuf,
    exists: bool,
    size: u64,
    mtime: Option<SystemTime>,
}

impl WatchedFile {
    fn snapshot(path: PathBuf) -> Self {
        match std::fs::metadata(&path) {
            Ok(meta) => Self {
                path,
                exists: true,
                size: meta.len(),
                mtime: meta.modified().ok(),
            },
            Err(_) => Self {
                path,
                exists: false,
                size: 0,
                mtime: None,
            },
        }
    }

    fn changed(&self) -> bool {
        let cur = Self::snapshot(self.path.clone());
        cur.exists != self.exists || cur.size != self.size || cur.mtime != self.mtime
    }
}

fn write_diags_text(sources: &SourceMap, diags: &mut DiagBag, errors: &mut dyn Write) -> i32 {
    diags.sort();
    for d in diags.items() {
        if let Ok(buf) = render_diagnostic_default(
            sources,
            d,
            RenderFormat::Text,
            TerminalEncoding::Utf8,
            UNICODE_METRICS,
        ) {
            let _ = errors.write_all(&buf);
        }
    }
    let _ = errors.flush();
    1
}

fn report_start_error(errors: &mut dyn Write, msg: &str) -> i32 {
    let sources = SourceMap::new();
    let mut diags = DiagBag::new();
    let _ = diags.push(Diagnostic::new(
        ErrorCode::ToolServiceStart,
        Span::NONE,
        msg,
    ));
    write_diags_text(&sources, &mut diags, errors)
}

type ApiCompileResult = Result<(ApiGeneration, Vec<WatchedFile>), (i32, Vec<u8>, Vec<WatchedFile>)>;

fn compile_api_generation(
    entry_file: &[u8],
    library: Option<&Path>,
    limits: Limits,
    gen_id: u64,
    collect_watch: bool,
) -> ApiCompileResult {
    let entry_owned = entry_file.to_vec();
    let lib_owned = library.map(Path::to_path_buf);

    let builder = std::thread::Builder::new()
        .name("mo-api-compile".to_string())
        .stack_size(MO_SOURCE_STACK_SIZE as usize);
    let handle = builder
        .spawn(move || {
            compile_api_inner(
                &entry_owned,
                lib_owned.as_deref(),
                limits,
                gen_id,
                collect_watch,
            )
        })
        .map_err(|e| {
            (
                1,
                format!("error E1409: could not prepare the source pipeline stack: {e}\n")
                    .into_bytes(),
                Vec::new(),
            )
        })?;
    handle.join().unwrap_or_else(|_| {
        Err((
            1,
            b"error E1409: api compilation panicked\n".to_vec(),
            Vec::new(),
        ))
    })
}

fn compile_api_inner(
    entry_file: &[u8],
    library: Option<&Path>,
    limits: Limits,
    gen_id: u64,
    collect_watch: bool,
) -> ApiCompileResult {
    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();
    let load_opts = LoadOptions {
        library: match library {
            Some(dir) => LibraryMode::Directory(dir.to_path_buf()),
            None => LibraryMode::Embedded {
                library: default_embedded_library(),
                entry_root: mo_check::default_library_root(),
            },
        },
        upgrade: false,
        syntax_arena_limit: MO_CLI_CHECK_SIZE as usize,
    };

    let modules = load_program_bytes(&mut sources, entry_file, &load_opts, &mut diags);

    let mut watched = Vec::new();
    if collect_watch {
        let mut seen = Vec::<PathBuf>::new();
        let entry_p = PathBuf::from(String::from_utf8_lossy(entry_file).into_owned());
        seen.push(entry_p.clone());
        watched.push(WatchedFile::snapshot(entry_p));
        for idx in 0..sources.len() {
            if let Some(f) = sources.get(idx) {
                let p = PathBuf::from(f.path_lossy().as_ref());
                if p.exists() && !seen.contains(&p) {
                    seen.push(p.clone());
                    watched.push(WatchedFile::snapshot(p));
                }
            }
        }
    }

    let Some(modules) = modules else {
        let mut buf = Vec::new();
        let st = write_diags_text(&sources, &mut diags, &mut buf);
        return Err((st, buf, watched));
    };
    if diags.has_any() {
        let mut buf = Vec::new();
        let st = write_diags_text(&sources, &mut diags, &mut buf);
        return Err((st, buf, watched));
    }

    let Some(program) = check_program(Some(&sources), modules, &mut diags) else {
        let mut buf = Vec::new();
        let st = write_diags_text(&sources, &mut diags, &mut buf);
        return Err((st, buf, watched));
    };
    if diags.has_any() {
        let mut buf = Vec::new();
        let st = write_diags_text(&sources, &mut diags, &mut buf);
        return Err((st, buf, watched));
    }

    let Some(entry_api) = program.apis.iter().find(|a| a.module.0 == 0) else {
        let _ = diags.push(
            Diagnostic::new(
                ErrorCode::ApiBlock,
                Span::NONE,
                "the entry module declares no api block",
            )
            .with_reason("Serve an entry module that declares an api block (reference 11.6)."),
        );
        let mut buf = Vec::new();
        let st = write_diags_text(&sources, &mut diags, &mut buf);
        return Err((st, buf, watched));
    };

    let Some(lowered) = lower_services(&program, Some(&sources), &mut diags) else {
        let mut buf = Vec::new();
        let st = write_diags_text(&sources, &mut diags, &mut buf);
        return Err((st, buf, watched));
    };
    if diags.has_any() {
        let mut buf = Vec::new();
        let st = write_diags_text(&sources, &mut diags, &mut buf);
        return Err((st, buf, watched));
    }

    let services_opt = (!lowered.services.items.is_empty()).then_some(&lowered.services);
    if !check_program_services(&lowered.arena, &lowered.program, services_opt, &mut diags) {
        let mut buf = Vec::new();
        let st = write_diags_text(&sources, &mut diags, &mut buf);
        return Err((st, buf, watched));
    }
    let Some(wire) = compile_services(&lowered.arena, &lowered.program, services_opt, &mut diags)
    else {
        let mut buf = Vec::new();
        let st = write_diags_text(&sources, &mut diags, &mut buf);
        return Err((st, buf, watched));
    };

    let file = match decode_file(&wire) {
        Ok(f) => f,
        Err(e) => {
            let _ = diags.push(
                Diagnostic::new(
                    ErrorCode::ToolPipeline,
                    Span::NONE,
                    "compiled api bytecode failed to load",
                )
                .with_primary_text(e.reason),
            );
            let mut buf = Vec::new();
            let st = write_diags_text(&sources, &mut diags, &mut buf);
            return Err((st, buf, watched));
        }
    };

    // Validate Image::init before accepting the generation.
    let img = match Image::init(file, limits) {
        Ok(i) => i,
        Err(boxed) => {
            let msg = format!("bug: {}\n", boxed.1.reason).into_bytes();
            return Err((70, msg, watched));
        }
    };

    let mod_name = &program.modules.modules[0].name;
    let wire_name = format!("{mod_name}.%api.wire");
    let job_name = format!("{mod_name}.%api.job");
    let mut wire_fn = None;
    let mut job_fn = None;
    for (idx, f) in img.file().functions.iter().enumerate() {
        let nm = img.file().string(f.name);
        if nm == wire_name {
            wire_fn = Some(idx as u32);
        }
        if nm == job_name {
            job_fn = Some(idx as u32);
        }
    }
    let (Some(wire_fn), Some(job_fn)) = (wire_fn, job_fn) else {
        return Err((
            1,
            b"error E1409: compiled api bytecode missing wire/job dispatchers\n".to_vec(),
            watched,
        ));
    };

    let generation = ApiGeneration {
        id: gen_id,
        wire: Arc::new(wire),
        wire_fn,
        job_fn,
        default_port: entry_api.port,
        cors: entry_api.cors.clone(),
        statics: entry_api
            .statics
            .iter()
            .map(|s| ApiStaticMount {
                prefix: s.prefix.clone(),
                dir: s.dir.clone(),
            })
            .collect(),
        jobs: entry_api
            .jobs
            .iter()
            .map(|j| ApiJobMeta {
                name: j.name.clone(),
                has_schedule: j.has_schedule,
                interval_us: j.interval_us,
            })
            .collect(),
        actor_services: entry_api.actors.iter().map(|a| a.service_index.0).collect(),
    };
    Ok((generation, watched))
}

struct ParsedHttpRequest {
    method: String,
    target: String,
    http_1_1: bool,
    headers: Vec<(String, String)>,
    body: Vec<u8>,
}

fn status_reason_phrase(code: i64) -> &'static str {
    match code {
        200 => "OK",
        201 => "Created",
        202 => "Accepted",
        204 => "No Content",
        301 => "Moved Permanently",
        302 => "Found",
        303 => "See Other",
        304 => "Not Modified",
        307 => "Temporary Redirect",
        308 => "Permanent Redirect",
        400 => "Bad Request",
        401 => "Unauthorized",
        403 => "Forbidden",
        404 => "Not Found",
        405 => "Method Not Allowed",
        408 => "Request Timeout",
        409 => "Conflict",
        413 => "Content Too Large",
        422 => "Unprocessable Content",
        429 => "Too Many Requests",
        431 => "Request Header Fields Too Large",
        500 => "Internal Server Error",
        502 => "Bad Gateway",
        503 => "Service Unavailable",
        _ => "Response",
    }
}

fn has_ctl_bytes(s: &[u8], allow_tab: bool) -> bool {
    s.iter()
        .any(|&c| c == 0x7f || (c < 0x20 && !(allow_tab && c == b'\t')))
}

fn send_json_status(
    stream: &mut TcpStream,
    code: i64,
    err_code: &str,
    cors: Option<&str>,
    keep_alive: bool,
    is_head: bool,
) -> bool {
    let payload = format!("{{\"error\":\"{err_code}\"}}");
    let mut head = format!(
        "HTTP/1.1 {code} {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\n",
        status_reason_phrase(code),
        payload.len()
    );
    if let Some(c) = cors
        && !c.is_empty()
    {
        head.push_str("Access-Control-Allow-Origin: ");
        head.push_str(c);
        head.push_str("\r\n");
    }
    head.push_str(if keep_alive {
        "Connection: keep-alive\r\n\r\n"
    } else {
        "Connection: close\r\n\r\n"
    });
    if stream.write_all(head.as_bytes()).is_err() {
        return false;
    }
    if !is_head && stream.write_all(payload.as_bytes()).is_err() {
        return false;
    }
    stream.flush().is_ok()
}

fn read_http_request(
    stream: &mut TcpStream,
    buf: &mut Vec<u8>,
    deadline: Instant,
) -> Result<ParsedHttpRequest, i32> {
    let header_limit = MO_LIMIT_API_HEADER_BYTES as usize;
    let body_limit = MO_LIMIT_API_BODY_BYTES as usize;
    let mut tmp = [0u8; 8192];

    let header_end = loop {
        if let Some(pos) = buf.windows(4).position(|w| w == b"\r\n\r\n") {
            break pos;
        }
        if buf.len() >= header_limit {
            return Err(431);
        }
        let rem = deadline.saturating_duration_since(Instant::now());
        if rem.is_zero() {
            return Err(408);
        }
        let _ = stream.set_read_timeout(Some(rem.min(Duration::from_millis(200))));
        match stream.read(&mut tmp) {
            Ok(0) => return Err(if buf.is_empty() { -1 } else { 400 }),
            Ok(n) => buf.extend_from_slice(&tmp[..n]),
            Err(e)
                if matches!(
                    e.kind(),
                    std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
                ) =>
            {
                if Instant::now() >= deadline {
                    return Err(408);
                }
            }
            Err(_) => return Err(400),
        }
    };

    let header_bytes = &buf[..header_end];
    let first_crlf = header_bytes
        .windows(2)
        .position(|w| w == b"\r\n")
        .unwrap_or(header_bytes.len());
    let req_line = &header_bytes[..first_crlf];
    let sp1 = req_line.iter().position(|&b| b == b' ').ok_or(400)?;
    if sp1 == 0 {
        return Err(400);
    }
    let after_m = &req_line[sp1 + 1..];
    let sp2 = after_m.iter().position(|&b| b == b' ').ok_or(400)?;
    if sp2 == 0 {
        return Err(400);
    }
    let method_b = &req_line[..sp1];
    let target_b = &after_m[..sp2];
    let ver_b = &after_m[sp2 + 1..];
    let http_1_1 = if ver_b == b"HTTP/1.1" {
        true
    } else if ver_b == b"HTTP/1.0" {
        false
    } else {
        return Err(400);
    };
    if method_b.iter().any(|&c| c <= 0x20 || c >= 0x7f) || has_ctl_bytes(target_b, false) {
        return Err(400);
    }
    let method = std::str::from_utf8(method_b).map_err(|_| 400)?.to_string();
    let target = std::str::from_utf8(target_b).map_err(|_| 400)?.to_string();

    let mut headers = Vec::new();
    let mut has_cl = false;
    let mut is_chunked = false;
    let mut content_length = 0usize;

    let mut pos = first_crlf + 2;
    while pos < header_bytes.len() {
        let eol = header_bytes[pos..]
            .windows(2)
            .position(|w| w == b"\r\n")
            .map_or(header_bytes.len(), |off| pos + off);
        let line = &header_bytes[pos..eol];
        let colon = line.iter().position(|&b| b == b':').ok_or(400)?;
        if colon == 0 {
            return Err(400);
        }
        let name_b = &line[..colon];
        if name_b.iter().any(|&c| c <= 0x20 || c >= 0x7f) {
            return Err(400);
        }
        let mut val_b = &line[colon + 1..];
        while let Some(&b) = val_b.first() {
            if b == b' ' || b == b'\t' {
                val_b = &val_b[1..];
            } else {
                break;
            }
        }
        while let Some(&b) = val_b.last() {
            if b == b' ' || b == b'\t' {
                val_b = &val_b[..val_b.len() - 1];
            } else {
                break;
            }
        }
        if has_ctl_bytes(val_b, true) {
            return Err(400);
        }
        let name = std::str::from_utf8(name_b).map_err(|_| 400)?.to_string();
        let value = std::str::from_utf8(val_b).map_err(|_| 400)?.to_string();
        if headers.len() >= MO_LIMIT_API_HEADERS as usize {
            return Err(431);
        }
        if name.eq_ignore_ascii_case("content-length") {
            if is_chunked || value.is_empty() {
                return Err(400);
            }
            let parsed = value.parse::<usize>().map_err(|_| 400)?;
            if parsed > body_limit {
                return Err(413);
            }
            if has_cl && parsed != content_length {
                return Err(400);
            }
            has_cl = true;
            content_length = parsed;
        } else if name.eq_ignore_ascii_case("transfer-encoding") {
            if has_cl || is_chunked || !value.eq_ignore_ascii_case("chunked") {
                return Err(400);
            }
            is_chunked = true;
        }
        headers.push((name, value));
        pos = eol + 2;
    }

    // Consume header block + CRLFCRLF.
    buf.drain(..header_end + 4);

    let mut body = Vec::new();
    if is_chunked {
        let mut overhead = 0usize;
        loop {
            let line_end = loop {
                if let Some(p) = buf.windows(2).position(|w| w == b"\r\n") {
                    break p;
                }
                if buf.len() > 1024 {
                    return Err(400);
                }
                let rem = deadline.saturating_duration_since(Instant::now());
                if rem.is_zero() {
                    return Err(408);
                }
                let _ = stream.set_read_timeout(Some(rem.min(Duration::from_millis(200))));
                match stream.read(&mut tmp) {
                    Ok(0) => return Err(400),
                    Ok(n) => buf.extend_from_slice(&tmp[..n]),
                    Err(e)
                        if matches!(
                            e.kind(),
                            std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
                        ) =>
                    {
                        if Instant::now() >= deadline {
                            return Err(408);
                        }
                    }
                    Err(_) => return Err(400),
                }
            };
            overhead = overhead.saturating_add(line_end + 4);
            if overhead > MO_LIMIT_API_CHUNK_OVERHEAD_BYTES as usize {
                return Err(413);
            }
            let size_line = &buf[..line_end];
            let hex_part = match size_line.iter().position(|&b| b == b';') {
                Some(sc) => &size_line[..sc],
                None => size_line,
            };
            let hex_str = std::str::from_utf8(hex_part).map_err(|_| 400)?;
            if hex_str.is_empty() {
                return Err(400);
            }
            let chunk_sz = usize::from_str_radix(hex_str, 16).map_err(|_| 400)?;
            buf.drain(..line_end + 2);
            if chunk_sz == 0 {
                // Consume trailer lines up to empty CRLF.
                let mut trailer_bytes = 0usize;
                loop {
                    let teol = loop {
                        if let Some(p) = buf.windows(2).position(|w| w == b"\r\n") {
                            break p;
                        }
                        if trailer_bytes + buf.len() > header_limit {
                            return Err(431);
                        }
                        let rem = deadline.saturating_duration_since(Instant::now());
                        if rem.is_zero() {
                            return Err(408);
                        }
                        let _ = stream.set_read_timeout(Some(rem.min(Duration::from_millis(200))));
                        match stream.read(&mut tmp) {
                            Ok(0) => return Err(400),
                            Ok(n) => buf.extend_from_slice(&tmp[..n]),
                            Err(e)
                                if matches!(
                                    e.kind(),
                                    std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
                                ) =>
                            {
                                if Instant::now() >= deadline {
                                    return Err(408);
                                }
                            }
                            Err(_) => return Err(400),
                        }
                    };
                    if teol == 0 {
                        buf.drain(..2);
                        break;
                    }
                    let tline = &buf[..teol];
                    if !tline.contains(&b':') || tline[0] == b':' || has_ctl_bytes(tline, true) {
                        return Err(400);
                    }
                    trailer_bytes += teol + 2;
                    if trailer_bytes > header_limit {
                        return Err(431);
                    }
                    buf.drain(..teol + 2);
                }
                break;
            }
            if body.len().saturating_add(chunk_sz) > body_limit {
                return Err(413);
            }
            while buf.len() < chunk_sz + 2 {
                let rem = deadline.saturating_duration_since(Instant::now());
                if rem.is_zero() {
                    return Err(408);
                }
                let _ = stream.set_read_timeout(Some(rem.min(Duration::from_millis(200))));
                match stream.read(&mut tmp) {
                    Ok(0) => return Err(400),
                    Ok(n) => buf.extend_from_slice(&tmp[..n]),
                    Err(e)
                        if matches!(
                            e.kind(),
                            std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
                        ) =>
                    {
                        if Instant::now() >= deadline {
                            return Err(408);
                        }
                    }
                    Err(_) => return Err(400),
                }
            }
            if &buf[chunk_sz..chunk_sz + 2] != b"\r\n" {
                return Err(400);
            }
            body.extend_from_slice(&buf[..chunk_sz]);
            buf.drain(..chunk_sz + 2);
        }
    } else if content_length > 0 {
        while buf.len() < content_length {
            let rem = deadline.saturating_duration_since(Instant::now());
            if rem.is_zero() {
                return Err(408);
            }
            let _ = stream.set_read_timeout(Some(rem.min(Duration::from_millis(200))));
            match stream.read(&mut tmp) {
                Ok(0) => return Err(400),
                Ok(n) => buf.extend_from_slice(&tmp[..n]),
                Err(e)
                    if matches!(
                        e.kind(),
                        std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
                    ) =>
                {
                    if Instant::now() >= deadline {
                        return Err(408);
                    }
                }
                Err(_) => return Err(400),
            }
        }
        body.extend_from_slice(&buf[..content_length]);
        buf.drain(..content_length);
    }

    Ok(ParsedHttpRequest {
        method,
        target,
        http_1_1,
        headers,
        body,
    })
}

fn static_mime_type(path: &str) -> &'static str {
    let lower = path.to_ascii_lowercase();
    if lower.ends_with(".webmanifest") {
        "application/manifest+json"
    } else if lower.ends_with(".mjs") || lower.ends_with(".js") {
        "text/javascript; charset=utf-8"
    } else if lower.ends_with(".css") {
        "text/css; charset=utf-8"
    } else if lower.ends_with(".html") {
        "text/html; charset=utf-8"
    } else if lower.ends_with(".json") || lower.ends_with(".map") {
        "application/json"
    } else if lower.ends_with(".svg") {
        "image/svg+xml"
    } else if lower.ends_with(".png") {
        "image/png"
    } else if lower.ends_with(".jpeg") || lower.ends_with(".jpg") {
        "image/jpeg"
    } else if lower.ends_with(".webp") {
        "image/webp"
    } else if lower.ends_with(".ico") {
        "image/x-icon"
    } else if lower.ends_with(".woff2") {
        "font/woff2"
    } else if lower.ends_with(".woff") {
        "font/woff"
    } else if lower.ends_with(".txt") {
        "text/plain; charset=utf-8"
    } else {
        "application/octet-stream"
    }
}

fn is_fingerprint_token(s: &str) -> bool {
    if !(8..=64).contains(&s.len()) {
        return false;
    }
    let mut has_digit = false;
    let mut has_alpha = false;
    for b in s.bytes() {
        if b.is_ascii_digit() {
            has_digit = true;
        } else if b.is_ascii_alphabetic() {
            has_alpha = true;
        } else if b != b'_' {
            return false;
        }
    }
    has_digit && has_alpha
}

fn is_fingerprinted_asset(path: &str) -> bool {
    let leaf = path.rsplit('/').next().unwrap_or(path);
    let Some(last_dot) = leaf.rfind('.') else {
        return false;
    };
    if last_dot == 0 {
        return false;
    }
    let stem = &leaf[..last_dot];
    if let Some(prev_dot) = stem.rfind('.')
        && is_fingerprint_token(&stem[prev_dot + 1..])
    {
        return true;
    }
    if let Some(last_dash) = stem.rfind('-')
        && is_fingerprint_token(&stem[last_dash + 1..])
    {
        return true;
    }
    false
}

fn decode_static_subpath(raw: &str) -> Option<String> {
    if raw.is_empty() || raw.starts_with('/') || raw.ends_with('/') {
        return None;
    }
    let bytes = raw.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if out.len() + 1 >= MO_LIMIT_HOST_PATH_SIZE as usize {
            return None;
        }
        if bytes[i] == b'%' {
            if i + 2 >= bytes.len() {
                return None;
            }
            let hi = (bytes[i + 1] as char).to_digit(16)?;
            let lo = (bytes[i + 2] as char).to_digit(16)?;
            let byte = ((hi << 4) | lo) as u8;
            if byte == 0 {
                return None;
            }
            out.push(byte);
            i += 3;
        } else {
            if bytes[i] == 0 {
                return None;
            }
            out.push(bytes[i]);
            i += 1;
        }
    }
    let s = String::from_utf8(out).ok()?;
    if s.is_empty() || s.starts_with('/') || s.ends_with('/') || s.contains('\\') {
        return None;
    }
    for seg in s.split('/') {
        if seg.is_empty() || seg == "." || seg == ".." {
            return None;
        }
    }
    Some(s)
}

fn etag_matches(req: &ParsedHttpRequest, etag: &str) -> bool {
    for (k, v) in &req.headers {
        if !k.eq_ignore_ascii_case("if-none-match") {
            continue;
        }
        for part in v.split(',') {
            let trimmed = part.trim();
            if trimmed == "*" {
                return true;
            }
            let stripped = trimmed.strip_prefix("W/").unwrap_or(trimmed);
            if stripped == etag {
                return true;
            }
        }
    }
    false
}

#[cfg(unix)]
fn try_serve_static_asset(
    generation: &ApiGeneration,
    data_dir: &Path,
    req: &ParsedHttpRequest,
    req_path: &str,
    keep_alive: bool,
    stream: &mut TcpStream,
) -> Option<(i32, bool)> {
    if generation.statics.is_empty() {
        return None;
    }
    if generation.cors.is_some() && req.method.eq_ignore_ascii_case("OPTIONS") {
        return None;
    }
    let mut matched: Option<(&ApiStaticMount, &str)> = None;
    for m in &generation.statics {
        if req_path == m.prefix {
            matched = Some((m, ""));
            break;
        }
        if m.prefix == "/" && req_path.starts_with('/') {
            matched = Some((m, &req_path[1..]));
            break;
        }
        if let Some(rest) = req_path.strip_prefix(&m.prefix)
            && let Some(sub) = rest.strip_prefix('/')
        {
            matched = Some((m, sub));
            break;
        }
    }
    let (mount, raw_sub) = matched?;
    let is_get = req.method.eq_ignore_ascii_case("GET");
    let is_head = req.method.eq_ignore_ascii_case("HEAD");
    if !is_get && !is_head {
        let ok = send_json_status(
            stream,
            405,
            "method_not_allowed",
            generation.cors.as_deref(),
            keep_alive,
            false,
        );
        return Some((405, ok));
    }
    let Some(subpath) = decode_static_subpath(raw_sub) else {
        let ok = send_json_status(
            stream,
            404,
            "not_found",
            generation.cors.as_deref(),
            keep_alive,
            is_head,
        );
        return Some((404, ok));
    };
    let Some((mut file, file_size, mtime_sec, mtime_nsec)) =
        mo_host::open_confined_file(data_dir, &mount.dir, &subpath)
    else {
        let ok = send_json_status(
            stream,
            404,
            "not_found",
            generation.cors.as_deref(),
            keep_alive,
            is_head,
        );
        return Some((404, ok));
    };
    if file_size > MO_LIMIT_STATIC_FILE_BYTES {
        let ok = send_json_status(
            stream,
            404,
            "not_found",
            generation.cors.as_deref(),
            keep_alive,
            is_head,
        );
        return Some((404, ok));
    }
    let etag = format!(
        "\"{:x}-{:x}-{:x}\"",
        file_size, mtime_sec as u64, mtime_nsec as u64
    );
    let cache_ctl = if is_fingerprinted_asset(&subpath) {
        "public, max-age=31536000, immutable"
    } else {
        "public, max-age=0, must-revalidate"
    };
    let not_modified = etag_matches(req, &etag);
    let code = if not_modified { 304 } else { 200 };
    let mut head = format!("HTTP/1.1 {code} {}\r\n", status_reason_phrase(code));
    if !not_modified {
        head.push_str("Content-Type: ");
        head.push_str(static_mime_type(&subpath));
        head.push_str("\r\n");
    }
    head.push_str("ETag: ");
    head.push_str(&etag);
    head.push_str("\r\nCache-Control: ");
    head.push_str(cache_ctl);
    head.push_str("\r\nX-Content-Type-Options: nosniff\r\n");
    if let Some(ref c) = generation.cors
        && !c.is_empty()
    {
        head.push_str("Access-Control-Allow-Origin: ");
        head.push_str(c);
        head.push_str("\r\n");
    }
    if !not_modified {
        head.push_str(&format!("Content-Length: {file_size}\r\n"));
    }
    head.push_str(if keep_alive {
        "Connection: keep-alive\r\n\r\n"
    } else {
        "Connection: close\r\n\r\n"
    });
    let mut sent = stream.write_all(head.as_bytes()).is_ok();
    if sent && !not_modified && !is_head && file_size > 0 {
        let mut buf = [0u8; 16384];
        let mut rem = file_size;
        while rem > 0 {
            let want = (rem as usize).min(buf.len());
            match file.read(&mut buf[..want]) {
                Ok(0) => {
                    sent = false;
                    break;
                }
                Ok(n) => {
                    if stream.write_all(&buf[..n]).is_err() {
                        sent = false;
                        break;
                    }
                    rem -= n as u64;
                }
                Err(_) => {
                    sent = false;
                    break;
                }
            }
        }
    }
    if sent {
        sent = stream.flush().is_ok();
    }
    Some((code as i32, sent))
}

fn sanitize_header(bytes: &[u8], is_name: bool) -> String {
    let mut out = String::with_capacity(bytes.len());
    for &b in bytes {
        if b == b'\r' || b == b'\n' || b == 0 {
            continue;
        }
        if is_name && (b <= 0x20 || b >= 0x7f || b == b':') {
            continue;
        }
        out.push(b as char);
    }
    out
}

fn format_utc_now() -> String {
    let secs = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs();
    // Civil date conversion (Howard Hinnant's algorithm).
    let days = (secs / 86_400) as i64;
    let rem = secs % 86_400;
    let hour = rem / 3600;
    let min = (rem % 3600) / 60;
    let sec = rem % 60;
    let z = days + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = (z - era * 146_097) as u64;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146_096) / 365;
    let y = yoe as i64 + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = if m <= 2 { y + 1 } else { y };
    format!("{year:04}-{m:02}-{d:02}T{hour:02}:{min:02}:{sec:02}Z")
}

#[cfg(unix)]
struct WorkerPool {
    active_gen: Mutex<Arc<ApiGeneration>>,
    queue: Mutex<(VecDeque<TcpStream>, bool)>,
    queue_cv: Condvar,
    job_cv: Condvar,
    sse_broker: SseBroker,
    sse_relay: SseRelayPool,
    stopping: AtomicBool,
    data_dir: PathBuf,
    bound_bind: String,
    bound_port: u16,
    limits: Limits,
    log_mutex: Mutex<()>,
}

#[cfg(unix)]
fn worker_env_vars(pool: &WorkerPool) -> Vec<(String, String)> {
    let mut env_vars: Vec<(String, String)> = std::env::vars().collect();
    if !env_vars.iter().any(|(k, _)| k == "MO_SERVE_BIND") {
        env_vars.push(("MO_SERVE_BIND".to_string(), pool.bound_bind.clone()));
    }
    if !env_vars.iter().any(|(k, _)| k == "MO_SERVE_PORT") {
        env_vars.push(("MO_SERVE_PORT".to_string(), pool.bound_port.to_string()));
    }
    env_vars
}

#[cfg(unix)]
impl WorkerPool {
    fn is_stopping(&self) -> bool {
        self.stopping.load(Ordering::Relaxed) || is_signalled()
    }

    fn current_gen(&self) -> Arc<ApiGeneration> {
        self.active_gen
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .clone()
    }
}

#[cfg(unix)]
enum DispatchOutcome {
    Completed { status: i32, keep_alive: bool },
    UpgradedSse { status: i32 },
}

#[cfg(unix)]
fn send_error_status_opt(stream_opt: &mut Option<TcpStream>, code: i64, err_code: &str) {
    if let Some(stream) = stream_opt.as_mut() {
        let _ = send_json_status(stream, code, err_code, None, false, false);
    }
}

struct WorkerCache {
    gen_id: u64,
    image: Image,
}

fn ensure_worker_image<'a>(
    cache: &'a mut Option<WorkerCache>,
    generation: &ApiGeneration,
    limits: Limits,
) -> Option<&'a Image> {
    let need_load = cache.as_ref().is_none_or(|c| c.gen_id != generation.id);
    if need_load {
        let file = decode_file(&generation.wire).ok()?;
        let image = Image::init(file, limits).ok()?;
        *cache = Some(WorkerCache {
            gen_id: generation.id,
            image,
        });
    }
    cache.as_ref().map(|c| &c.image)
}

#[cfg(unix)]
fn dispatch_vm_request(
    pool: &WorkerPool,
    generation: &ApiGeneration,
    image: &Image,
    req: &ParsedHttpRequest,
    req_path: &str,
    keep_alive: bool,
    stream_opt: &mut Option<TcpStream>,
) -> DispatchOutcome {
    let env_vars = worker_env_vars(pool);
    let host = match HostContext::stdio_with_root(Vec::new(), env_vars, Some(&pool.data_dir)) {
        Ok(h) => h,
        Err(_) => {
            send_error_status_opt(stream_opt, 500, "internal_server_error");
            return DispatchOutcome::Completed {
                status: 500,
                keep_alive: false,
            };
        }
    };
    let mut proc = Process::new(image, pool.limits, host);
    let fn_def = &image.file().functions[generation.wire_fn as usize];
    let pcount = fn_def.parameters.len();

    let mut args = Vec::with_capacity(pcount);
    let Ok(m_val) = proc.heap().alloc_text(req.method.clone()) else {
        send_error_status_opt(stream_opt, 500, "internal_server_error");
        return DispatchOutcome::Completed {
            status: 500,
            keep_alive: false,
        };
    };
    args.push(m_val);
    let Ok(t_val) = proc.heap().alloc_text(req.target.clone()) else {
        send_error_status_opt(stream_opt, 500, "internal_server_error");
        return DispatchOutcome::Completed {
            status: 500,
            keep_alive: false,
        };
    };
    args.push(t_val);

    let list_ty = fn_def.parameters[2].ty;
    let pair_ty = image.file().types.get(list_ty).parameters[0];
    let Some(pair_layout) = image.file().types.find_tuple_layout(pair_ty) else {
        send_error_status_opt(stream_opt, 500, "internal_server_error");
        return DispatchOutcome::Completed {
            status: 500,
            keep_alive: false,
        };
    };
    let mut hdr_items = VecDeque::with_capacity(req.headers.len());
    for (k, v) in &req.headers {
        let Ok(kv) = proc.heap().alloc_text(k.clone()) else {
            send_error_status_opt(stream_opt, 500, "internal_server_error");
            return DispatchOutcome::Completed {
                status: 500,
                keep_alive: false,
            };
        };
        let Ok(vv) = proc.heap().alloc_text(v.clone()) else {
            send_error_status_opt(stream_opt, 500, "internal_server_error");
            return DispatchOutcome::Completed {
                status: 500,
                keep_alive: false,
            };
        };
        let Ok(pair) = proc
            .heap()
            .construct(&image.file().types, pair_layout, vec![kv, vv])
        else {
            send_error_status_opt(stream_opt, 500, "internal_server_error");
            return DispatchOutcome::Completed {
                status: 500,
                keep_alive: false,
            };
        };
        hdr_items.push_back(pair);
    }
    let Ok(hdr_list) = proc.heap().alloc_list(hdr_items) else {
        send_error_status_opt(stream_opt, 500, "internal_server_error");
        return DispatchOutcome::Completed {
            status: 500,
            keep_alive: false,
        };
    };
    args.push(hdr_list);

    let Ok(body_val) = proc.heap().alloc_bytes(req.body.clone()) else {
        send_error_status_opt(stream_opt, 500, "internal_server_error");
        return DispatchOutcome::Completed {
            status: 500,
            keep_alive: false,
        };
    };
    args.push(body_val);

    let actor_base = pcount.saturating_sub(generation.actor_services.len());
    for (i, p) in fn_def.parameters.iter().enumerate().skip(4) {
        let cap_kind = image.file().types.get(p.ty).capability;
        if cap_kind == Some(Capability::Simulation) {
            let svc_idx = generation.actor_services[i - actor_base];
            match proc.make_live_actor(p.ty, svc_idx) {
                Ok(v) => args.push(v),
                Err(_) => {
                    send_error_status_opt(stream_opt, 500, "internal_server_error");
                    return DispatchOutcome::Completed {
                        status: 500,
                        keep_alive: false,
                    };
                }
            }
        } else {
            match proc.make_real_capability(p.ty) {
                Ok(v) => args.push(v),
                Err(_) => {
                    send_error_status_opt(stream_opt, 500, "internal_server_error");
                    return DispatchOutcome::Completed {
                        status: 500,
                        keep_alive: false,
                    };
                }
            }
        }
    }

    let (outcome, _) = crate::call_with_budget_isolated(
        &mut proc,
        generation.wire_fn,
        args,
        MO_LIMIT_API_REQUEST_WORK,
    );
    let result = match outcome {
        Outcome::Returned(v) => v,
        Outcome::Failed(d) | Outcome::Stopped(d) => {
            let _guard = pool
                .log_mutex
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            eprintln!(
                "mo serve: failure: {} {}\nbug: {}",
                req.method, req_path, d.reason
            );
            send_error_status_opt(stream_opt, 500, "internal_server_error");
            return DispatchOutcome::Completed {
                status: 500,
                keep_alive: false,
            };
        }
    };

    let mut status_code = proc.heap().extract_field(&result, 0).as_int();
    if !(100..=599).contains(&status_code) {
        status_code = 500;
    }
    let res_headers_val = proc.heap().extract_field(&result, 1);
    let res_body_val = proc.heap().extract_field(&result, 2);
    let res_headers = res_headers_val.as_list();
    let res_body = res_body_val.as_bytes();
    let is_head = req.method.eq_ignore_ascii_case("HEAD");
    let no_body = (100..200).contains(&status_code) || status_code == 204 || status_code == 304;
    let effective_ka = keep_alive && status_code < 500;

    let mut subscribe_spec: Option<SseSubscribeSpec> = None;
    let mut relay_spec: Option<SseRelaySpec> = None;
    let mut publishes: Vec<SsePublishSpec> = Vec::new();
    let mut has_content_type = false;
    let mut has_cache_control = false;
    let mut has_accel_buffering = false;

    let mut head = format!(
        "HTTP/1.1 {status_code} {}\r\n",
        status_reason_phrase(status_code)
    );
    for pair in res_headers {
        let name_val = proc.heap().extract_field(pair, 0);
        let val_val = proc.heap().extract_field(pair, 1);
        let name = name_val.as_text();
        let val = val_val.as_text();
        if name.eq_ignore_ascii_case(SSE_SUBSCRIBE_HEADER) {
            if let Some(spec) = parse_subscribe_header(val) {
                subscribe_spec = Some(spec);
            }
            continue;
        }
        if name.eq_ignore_ascii_case(SSE_RELAY_HEADER) {
            if let Some(spec) = parse_relay_header(val) {
                relay_spec = Some(spec);
            }
            continue;
        }
        if name.eq_ignore_ascii_case(SSE_PUBLISH_HEADER) {
            if publishes.len() < MO_LIMIT_API_SSE_PUBLISH_PER_RESPONSE as usize
                && let Some(ev) = parse_publish_header(val)
            {
                publishes.push(ev);
            }
            continue;
        }
        if name.eq_ignore_ascii_case("content-length")
            || name.eq_ignore_ascii_case("transfer-encoding")
            || name.eq_ignore_ascii_case("connection")
        {
            continue;
        }
        if name.eq_ignore_ascii_case("content-type") {
            has_content_type = true;
        } else if name.eq_ignore_ascii_case("cache-control") {
            has_cache_control = true;
        } else if name.eq_ignore_ascii_case("x-accel-buffering") {
            has_accel_buffering = true;
        }
        let sn = sanitize_header(name.as_bytes(), true);
        if sn.is_empty() {
            continue;
        }
        let sv = sanitize_header(val.as_bytes(), false);
        head.push_str(&sn);
        head.push_str(": ");
        head.push_str(&sv);
        head.push_str("\r\n");
    }

    if status_code == 200
        && !is_head
        && let Some(sub_spec) = subscribe_spec
    {
        if !has_content_type {
            head.push_str("Content-Type: text/event-stream; charset=utf-8\r\n");
        }
        if !has_cache_control {
            head.push_str("Cache-Control: no-cache, no-transform\r\n");
        }
        if !has_accel_buffering {
            head.push_str("X-Accel-Buffering: no\r\n");
        }
        head.push_str("Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
        let mut initial_wire = head.into_bytes();
        if !res_body.is_empty() {
            initial_wire.extend_from_slice(&encode_http_chunk(res_body));
        }
        if let Some(stream) = stream_opt.take() {
            match pool
                .sse_broker
                .register_client(stream, initial_wire, sub_spec)
            {
                Ok(()) => {
                    if !publishes.is_empty() {
                        pool.sse_broker.publish(&publishes);
                    }
                    return DispatchOutcome::UpgradedSse { status: 200 };
                }
                Err(mut returned) => {
                    let _ = send_json_status(
                        &mut returned,
                        503,
                        "service_unavailable",
                        generation.cors.as_deref(),
                        false,
                        false,
                    );
                    return DispatchOutcome::Completed {
                        status: 503,
                        keep_alive: false,
                    };
                }
            }
        }
        return DispatchOutcome::Completed {
            status: 500,
            keep_alive: false,
        };
    }

    if status_code == 200
        && !is_head
        && let Some(r_spec) = relay_spec
    {
        if !has_content_type {
            head.push_str("Content-Type: text/event-stream; charset=utf-8\r\n");
        }
        if !has_cache_control {
            head.push_str("Cache-Control: no-cache, no-transform\r\n");
        }
        if !has_accel_buffering {
            head.push_str("X-Accel-Buffering: no\r\n");
        }
        head.push_str("Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
        let mut initial_wire = head.into_bytes();
        if !res_body.is_empty() {
            initial_wire.extend_from_slice(&encode_http_chunk(res_body));
        }
        if let Some(stream) = stream_opt.take() {
            match pool.sse_relay.spawn(stream, initial_wire, r_spec) {
                Ok(()) => {
                    if !publishes.is_empty() {
                        pool.sse_broker.publish(&publishes);
                    }
                    return DispatchOutcome::UpgradedSse { status: 200 };
                }
                Err(Some(mut returned)) => {
                    let _ = send_json_status(
                        &mut returned,
                        503,
                        "service_unavailable",
                        generation.cors.as_deref(),
                        false,
                        false,
                    );
                    return DispatchOutcome::Completed {
                        status: 503,
                        keep_alive: false,
                    };
                }
                Err(None) => {
                    return DispatchOutcome::Completed {
                        status: 503,
                        keep_alive: false,
                    };
                }
            }
        }
        return DispatchOutcome::Completed {
            status: 500,
            keep_alive: false,
        };
    }

    if (200..300).contains(&status_code) && !publishes.is_empty() {
        pool.sse_broker.publish(&publishes);
    }

    let conn_val = if effective_ka { "keep-alive" } else { "close" };
    if no_body {
        head.push_str(&format!("Connection: {conn_val}\r\n\r\n"));
    } else {
        head.push_str(&format!(
            "Content-Length: {}\r\nConnection: {conn_val}\r\n\r\n",
            res_body.len()
        ));
    }

    let Some(stream) = stream_opt.as_mut() else {
        return DispatchOutcome::Completed {
            status: 500,
            keep_alive: false,
        };
    };
    let mut sent = stream.write_all(head.as_bytes()).is_ok();
    if sent && !no_body && !is_head && !res_body.is_empty() {
        sent = stream.write_all(res_body).is_ok();
    }
    if sent {
        sent = stream.flush().is_ok();
    }
    DispatchOutcome::Completed {
        status: status_code as i32,
        keep_alive: sent && effective_ka,
    }
}

#[cfg(unix)]
fn serve_connection(pool: &WorkerPool, cache: &mut Option<WorkerCache>, stream: TcpStream) {
    let _ = stream.set_write_timeout(Some(Duration::from_micros(MO_LIMIT_API_WRITE_US)));
    let peer_ip = stream.peer_addr().ok().map(|addr| addr.ip().to_string());
    let mut stream_opt = Some(stream);
    let mut buf = Vec::new();
    for req_idx in 0..MO_LIMIT_API_KEEPALIVE_REQUESTS {
        if pool.is_stopping() {
            break;
        }
        let timeout_us = if req_idx == 0 {
            MO_LIMIT_API_READ_US
        } else {
            MO_LIMIT_API_KEEPALIVE_IDLE_US
        };
        let deadline = Instant::now() + Duration::from_micros(timeout_us);
        let req = {
            let Some(stream) = stream_opt.as_mut() else {
                break;
            };
            match read_http_request(stream, &mut buf, deadline) {
                Ok(mut r) => {
                    // Strip any client-supplied X-Mo-Peer-Ip so proxy/rate-limit logic sees only the socket peer.
                    r.headers
                        .retain(|(k, _)| !k.eq_ignore_ascii_case("x-mo-peer-ip"));
                    if let Some(ip) = &peer_ip {
                        r.headers.push(("X-Mo-Peer-Ip".to_string(), ip.clone()));
                    }
                    r
                }
                Err(code) => {
                    if code > 0 {
                        let err_str = match code {
                            408 => "request_timeout",
                            413 => "payload_too_large",
                            431 => "headers_too_large",
                            _ => "bad_request",
                        };
                        let _ =
                            send_json_status(stream, i64::from(code), err_str, None, false, false);
                    }
                    break;
                }
            }
        };

        let start = Instant::now();
        let req_path = req.target.split('?').next().unwrap_or(&req.target);
        let mut has_close = false;
        let mut has_ka = false;
        for (k, v) in &req.headers {
            if k.eq_ignore_ascii_case("connection") {
                for tok in v.split(',') {
                    let t = tok.trim();
                    if t.eq_ignore_ascii_case("close") {
                        has_close = true;
                    } else if t.eq_ignore_ascii_case("keep-alive") {
                        has_ka = true;
                    }
                }
            }
        }
        let keep_alive = !has_close
            && (req.http_1_1 || has_ka)
            && req_idx + 1 < MO_LIMIT_API_KEEPALIVE_REQUESTS
            && !pool.is_stopping();

        let generation = pool.current_gen();
        let outcome = {
            let static_res = stream_opt.as_mut().and_then(|stream| {
                try_serve_static_asset(
                    &generation,
                    &pool.data_dir,
                    &req,
                    req_path,
                    keep_alive,
                    stream,
                )
            });
            if let Some((st, io_ok)) = static_res {
                DispatchOutcome::Completed {
                    status: st,
                    keep_alive: io_ok && keep_alive,
                }
            } else if let Some(img) = ensure_worker_image(cache, &generation, pool.limits) {
                dispatch_vm_request(
                    pool,
                    &generation,
                    img,
                    &req,
                    req_path,
                    keep_alive,
                    &mut stream_opt,
                )
            } else {
                send_error_status_opt(&mut stream_opt, 500, "internal_server_error");
                DispatchOutcome::Completed {
                    status: 500,
                    keep_alive: false,
                }
            }
        };

        let (status, cont) = match outcome {
            DispatchOutcome::Completed { status, keep_alive } => (status, keep_alive),
            DispatchOutcome::UpgradedSse { status } => (status, false),
        };

        let dur_ms = start.elapsed().as_secs_f64() * 1000.0;
        {
            let _guard = pool
                .log_mutex
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            eprintln!(
                "mo serve: request: {} {} {} {} {:.2}ms",
                format_utc_now(),
                req.method,
                req_path,
                status,
                dur_ms
            );
        }
        if !cont {
            break;
        }
    }
}

#[cfg(unix)]
fn job_worker_main(pool: Arc<WorkerPool>) {
    let mut cache: Option<WorkerCache> = None;
    let mut seen_gen_id = 0u64;
    let mut schedules: Vec<(String, u64, Instant)> = Vec::new();

    while !pool.is_stopping() {
        let generation = pool.current_gen();
        if generation.id != seen_gen_id {
            let now = Instant::now();
            let prev = std::mem::take(&mut schedules);
            for jm in &generation.jobs {
                if jm.has_schedule {
                    let due = prev
                        .iter()
                        .find(|(n, iv, _)| n == &jm.name && *iv == jm.interval_us)
                        .map_or_else(
                            || now + Duration::from_micros(jm.interval_us),
                            |(_, _, d)| *d,
                        );
                    schedules.push((jm.name.clone(), jm.interval_us, due));
                }
            }
            seen_gen_id = generation.id;
        }

        if let Some(img) = ensure_worker_image(&mut cache, &generation, pool.limits) {
            for (name, interval_us, next_due) in &mut schedules {
                if pool.is_stopping() {
                    break;
                }
                if Instant::now() >= *next_due {
                    let start = Instant::now();
                    let env_vars = worker_env_vars(&pool);
                    let outcome_str = if let Ok(host) =
                        HostContext::stdio_with_root(Vec::new(), env_vars, Some(&pool.data_dir))
                    {
                        let mut proc = Process::new(img, pool.limits, host);
                        let fn_def = &img.file().functions[generation.job_fn as usize];
                        let mut args = Vec::with_capacity(fn_def.parameters.len());
                        let mut ok = true;
                        if let Ok(a0) = proc.heap().alloc_text(name.clone())
                            && let Ok(a1) = proc.heap().alloc_text("null".to_string())
                        {
                            args.push(a0);
                            args.push(a1);
                        } else {
                            ok = false;
                        }
                        if ok {
                            for p in fn_def.parameters.iter().skip(2) {
                                match proc.make_real_capability(p.ty) {
                                    Ok(v) => args.push(v),
                                    Err(_) => {
                                        ok = false;
                                        break;
                                    }
                                }
                            }
                        }
                        if ok {
                            match crate::call_with_budget_isolated(
                                &mut proc,
                                generation.job_fn,
                                args,
                                MO_LIMIT_API_JOB_WORK,
                            )
                            .0
                            {
                                Outcome::Returned(v) => {
                                    if v.as_tag() == 0 {
                                        "ok"
                                    } else {
                                        "error"
                                    }
                                }
                                Outcome::Failed(_) | Outcome::Stopped(_) => "defect",
                            }
                        } else {
                            "defect"
                        }
                    } else {
                        "defect"
                    };
                    let dur_ms = start.elapsed().as_secs_f64() * 1000.0;
                    {
                        let _g = pool
                            .log_mutex
                            .lock()
                            .unwrap_or_else(std::sync::PoisonError::into_inner);
                        eprintln!(
                            "mo serve: job: {} {} {} {:.2}ms",
                            format_utc_now(),
                            name,
                            outcome_str,
                            dur_ms
                        );
                    }
                    *next_due = Instant::now() + Duration::from_micros(*interval_us);
                }
            }
        }

        let mut wait = Duration::from_micros(MO_LIMIT_API_JOB_POLL_US);
        let now = Instant::now();
        for (_, _, due) in &schedules {
            let rem = due.saturating_duration_since(now);
            if rem < wait {
                wait = rem;
            }
        }
        let guard = pool
            .queue
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        if !guard.1 && !pool.is_stopping() {
            let _ = pool.job_cv.wait_timeout(guard, wait);
        }
    }
}

#[cfg(unix)]
fn shutdown_pool_and_threads(
    pool: &WorkerPool,
    worker_handles: Vec<std::thread::JoinHandle<()>>,
    job_handle: Option<std::thread::JoinHandle<()>>,
) {
    pool.stopping.store(true, Ordering::Relaxed);
    {
        let mut g = pool
            .queue
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        g.1 = true;
        pool.queue_cv.notify_all();
        pool.job_cv.notify_all();
    }
    pool.sse_broker.shutdown();
    pool.sse_relay.shutdown();
    for h in worker_handles {
        let _ = h.join();
    }
    if let Some(jh) = job_handle {
        let _ = jh.join();
    }
}

/// Runs an HTTP/1.1 `api` server (`src/tools/api-server.c:mo_serve_maybe_api`).
#[cfg(unix)]
pub fn run_serve_api(
    opts: &ApiServeOptions<'_>,
    output: &mut dyn Write,
    errors: &mut dyn Write,
) -> i32 {
    reset_signal();
    if let Err(e) = install_signal_handlers() {
        return report_start_error(
            errors,
            &format!("mo serve could not watch for stop signals: {e}"),
        );
    }

    let data_dir = opts.data_dir.unwrap_or_else(|| Path::new("."));
    match std::fs::metadata(data_dir) {
        Ok(m) if m.is_dir() => {}
        Ok(_) => {
            return report_start_error(
                errors,
                &format!(
                    "mo serve can't use the data folder {}: Not a directory",
                    data_dir.display()
                ),
            );
        }
        Err(e) => {
            return report_start_error(
                errors,
                &format!(
                    "mo serve can't use the data folder {}: {e}",
                    data_dir.display()
                ),
            );
        }
    }

    let mut gen_counter = 1u64;
    let (initial_gen, mut watched) = match compile_api_generation(
        opts.entry_file,
        opts.library,
        opts.limits,
        gen_counter,
        opts.watch,
    ) {
        Ok(v) => v,
        Err((st, msg, _)) => {
            let _ = errors.write_all(&msg);
            let _ = errors.flush();
            return st;
        }
    };

    let bind_host = opts.bind_host.unwrap_or("127.0.0.1");
    let ip: IpAddr = match bind_host.parse() {
        Ok(ip) => ip,
        Err(_) => {
            return report_start_error(
                errors,
                &format!("mo serve could not listen on {bind_host}: invalid address"),
            );
        }
    };
    let requested_port = opts.port.unwrap_or(initial_gen.default_port);
    let listener = match TcpListener::bind(SocketAddr::new(ip, requested_port)) {
        Ok(l) => l,
        Err(e) => {
            return report_start_error(
                errors,
                &format!("mo serve could not listen on {bind_host}:{requested_port}: {e}"),
            );
        }
    };
    if let Some(ready_path) = opts.ready_file {
        crate::clear_ready_file(ready_path);
    }
    let actual_port = match listener.local_addr() {
        Ok(a) => a.port(),
        Err(e) => {
            return report_start_error(errors, &format!("mo serve could not get port: {e}"));
        }
    };
    let _ = listener.set_nonblocking(true);

    let mut req_limits = opts.limits;
    req_limits.heap_size = MO_LIMIT_API_REQUEST_HEAP as usize;

    let has_jobs = !initial_gen.jobs.is_empty();
    let has_actors = !initial_gen.actor_services.is_empty();
    let sse_broker = match SseBroker::default_broker() {
        Ok(b) => b,
        Err(e) => {
            return report_start_error(
                errors,
                &format!("mo serve could not start SSE broker: {e}"),
            );
        }
    };
    let pool = Arc::new(WorkerPool {
        active_gen: Mutex::new(Arc::new(initial_gen)),
        queue: Mutex::new((VecDeque::new(), false)),
        queue_cv: Condvar::new(),
        job_cv: Condvar::new(),
        sse_broker,
        sse_relay: SseRelayPool::default_pool(),
        stopping: AtomicBool::new(false),
        data_dir: data_dir.to_path_buf(),
        bound_bind: bind_host.to_string(),
        bound_port: actual_port,
        limits: req_limits,
        log_mutex: Mutex::new(()),
    });

    // If actors are declared, serialize requests onto a single worker so live actor state stays coherent.
    let target_workers = if has_actors {
        1
    } else {
        opts.workers
            .unwrap_or(MO_LIMIT_API_WORKERS as u32)
            .clamp(1, MO_LIMIT_API_WORKERS_MAX as u32)
    };

    let mut worker_handles = Vec::with_capacity(target_workers as usize);
    for _ in 0..target_workers {
        let p = Arc::clone(&pool);
        if let Ok(h) = std::thread::Builder::new()
            .name("mo-api-worker".to_string())
            .stack_size(2 << 20)
            .spawn(move || {
                let mut cache: Option<WorkerCache> = None;
                loop {
                    let stream = {
                        let mut guard = p
                            .queue
                            .lock()
                            .unwrap_or_else(std::sync::PoisonError::into_inner);
                        while guard.0.is_empty() && !guard.1 {
                            guard = p
                                .queue_cv
                                .wait(guard)
                                .unwrap_or_else(std::sync::PoisonError::into_inner);
                        }
                        if let Some(s) = guard.0.pop_front() {
                            s
                        } else {
                            break;
                        }
                    };
                    serve_connection(&p, &mut cache, stream);
                }
            })
        {
            worker_handles.push(h);
        }
    }

    let mut job_handle = if has_jobs {
        let p = Arc::clone(&pool);
        std::thread::Builder::new()
            .name("mo-api-jobs".to_string())
            .stack_size(2 << 20)
            .spawn(move || job_worker_main(p))
            .ok()
    } else {
        None
    };

    let listen_url = format!("http://{bind_host}:{actual_port}");
    if let Some(ready_path) = opts.ready_file
        && let Err(err) = crate::write_ready_file(ready_path, &listen_url, actual_port, data_dir)
    {
        shutdown_pool_and_threads(&pool, worker_handles, job_handle);
        return report_start_error(
            errors,
            &format!("mo serve could not write ready file: {err}"),
        );
    }
    let _ready_guard = crate::ReadyFileGuard(opts.ready_file);

    if writeln!(output, "mo serve: listening on {listen_url}").is_err() || output.flush().is_err() {
        shutdown_pool_and_threads(&pool, worker_handles, job_handle);
        return report_start_error(errors, "mo serve could not write its listening line");
    }

    let mut next_watch = Instant::now() + Duration::from_micros(MO_LIMIT_API_WATCH_POLL_US);
    while !is_signalled() {
        if opts.watch && Instant::now() >= next_watch {
            if watched.iter().any(WatchedFile::changed) {
                gen_counter += 1;
                match compile_api_generation(
                    opts.entry_file,
                    opts.library,
                    opts.limits,
                    gen_counter,
                    true,
                ) {
                    Ok((next_gen, next_watch_files)) => {
                        let start_jobs = !next_gen.jobs.is_empty();
                        {
                            let mut ag = pool
                                .active_gen
                                .lock()
                                .unwrap_or_else(std::sync::PoisonError::into_inner);
                            *ag = Arc::new(next_gen);
                        }
                        watched = next_watch_files;
                        if start_jobs && job_handle.is_none() {
                            let p = Arc::clone(&pool);
                            job_handle = std::thread::Builder::new()
                                .name("mo-api-jobs".to_string())
                                .stack_size(2 << 20)
                                .spawn(move || job_worker_main(p))
                                .ok();
                        }
                        let _g = pool
                            .log_mutex
                            .lock()
                            .unwrap_or_else(std::sync::PoisonError::into_inner);
                        let _ = writeln!(
                            errors,
                            "mo serve: reload: {}",
                            String::from_utf8_lossy(opts.entry_file)
                        );
                        let _ = errors.flush();
                    }
                    Err((_, diag_bytes, next_files)) => {
                        let _g = pool
                            .log_mutex
                            .lock()
                            .unwrap_or_else(std::sync::PoisonError::into_inner);
                        let _ = errors.write_all(&diag_bytes);
                        let _ = errors.flush();
                        for w in &mut watched {
                            *w = WatchedFile::snapshot(w.path.clone());
                        }
                        for nf in next_files {
                            if !watched.iter().any(|w| w.path == nf.path) {
                                watched.push(nf);
                            }
                        }
                    }
                }
            }
            next_watch = Instant::now() + Duration::from_micros(MO_LIMIT_API_WATCH_POLL_US);
        }

        match listener.accept() {
            Ok((stream, _)) => {
                let mut g = pool
                    .queue
                    .lock()
                    .unwrap_or_else(std::sync::PoisonError::into_inner);
                if g.0.len() < MO_LIMIT_API_QUEUE as usize {
                    g.0.push_back(stream);
                    pool.queue_cv.notify_one();
                } else {
                    drop(g);
                    let mut s = stream;
                    let _ =
                        send_json_status(&mut s, 503, "service_unavailable", None, false, false);
                }
            }
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                std::thread::sleep(Duration::from_millis(20));
            }
            Err(_) => {
                std::thread::sleep(Duration::from_millis(20));
            }
        }
    }

    drop(listener);
    shutdown_pool_and_threads(&pool, worker_handles, job_handle);

    let _ = writeln!(errors, "mo serve: stop: a signal asked the server to stop");
    let _ = errors.flush();
    0
}
