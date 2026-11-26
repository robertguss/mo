#![forbid(unsafe_code)]

//! Bounded outbound HTTP/HTTPS-to-SSE streaming relay for `mo serve` (`api` blocks).
//!
//! Design invariants:
//! - Route handlers stay bounded and deterministic inside `MO_LIMIT_API_REQUEST_WORK`: they
//!   build an `Api.SseRelay` directive header (`X-Mo-Sse-Relay`) and return initial SSE frames
//!   (such as retrieved workspace citations).
//! - `mo serve` strips `X-Mo-Sse-Relay` on every response, flushes initial SSE chunks to the
//!   client immediately, and hands off upstream streaming to a bounded `mo-api-sse-relay` worker
//!   off `MO_LIMIT_API_WORKERS` using [`mo_host::OutboundStream`] with per-chunk idle timeouts.
//! - Supports `raw`, `openai_sse`, and `anthropic_sse` upstream SSE transforms, enforcing header,
//!   line, and body size caps without ever leaking directive headers or credentials on the wire.

use std::io::Write as _;
use std::mem;
use std::net::TcpStream;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant};

use mo_def::limits::{
    MO_LIMIT_API_SSE_RELAY_BODY_BYTES, MO_LIMIT_API_SSE_RELAY_CONNECT_US,
    MO_LIMIT_API_SSE_RELAY_HEADER_BYTES, MO_LIMIT_API_SSE_RELAY_IDLE_US,
    MO_LIMIT_API_SSE_RELAY_LINE_BYTES, MO_LIMIT_API_SSE_RELAY_TOTAL_US, MO_LIMIT_API_SSE_RELAYS,
    MO_LIMIT_API_WRITE_US, MO_LIMIT_HOST_NAME,
};
use mo_host::{OutboundError, OutboundStream};

use crate::binary::push_json_string;
use crate::sse_broker::{
    JsonValue, encode_http_chunk, find_json_field, format_sse_event, parse_json,
    sanitize_single_line,
};

/// Internal response header stripped by `mo serve` to stream an upstream HTTP/SSE response
/// directly to the caller as chunked `text/event-stream`.
pub(crate) const SSE_RELAY_HEADER: &str = "x-mo-sse-relay";

const MAX_RELAY_HEADERS: usize = 64;
const MAX_METHOD_BYTES: usize = 16;
const MAX_EVENT_NAME_BYTES: usize = 256;
const MAX_ERROR_DETAIL_BYTES: usize = 256;
const MIN_IDLE_TIMEOUT_MS: u64 = 100;
const CHUNK_SIZE_LINE_LIMIT: usize = 1024;

/// Supported upstream-to-downstream SSE transform modes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum SseRelayMode {
    Raw,
    OpenAiSse,
    AnthropicSse,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct ParsedUrl {
    pub secure: bool,
    pub authority: String,
    pub host: String,
    pub port: u16,
    pub path: String,
}

/// Validated `X-Mo-Sse-Relay` directive extracted from an `api` route handler response.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct SseRelaySpec {
    pub method: String,
    pub target: ParsedUrl,
    pub headers: Vec<(String, String)>,
    pub body: String,
    pub mode: SseRelayMode,
    pub idle_timeout: Duration,
    pub token_event: String,
    pub done_event: Option<String>,
    pub done_data: Option<String>,
    pub error_event: String,
}

/// Bounded coordinator tracking in-flight outbound SSE relay threads for graceful shutdown.
pub(crate) struct SseRelayPool {
    stopping: Arc<AtomicBool>,
    state: Arc<(Mutex<usize>, Condvar)>,
    max_relays: usize,
    total_timeout: Duration,
}

impl SseRelayPool {
    /// Creates a relay pool configured with `limits.h` defaults.
    pub(crate) fn default_pool() -> Self {
        Self::with_limits(
            MO_LIMIT_API_SSE_RELAYS as usize,
            Duration::from_micros(MO_LIMIT_API_SSE_RELAY_TOTAL_US),
        )
    }

    pub(crate) fn with_limits(max_relays: usize, total_timeout: Duration) -> Self {
        Self {
            stopping: Arc::new(AtomicBool::new(false)),
            state: Arc::new((Mutex::new(0), Condvar::new())),
            max_relays: max_relays.max(1),
            total_timeout,
        }
    }

    /// Spawns a dedicated relay thread for `client` if capacity allows, flushing `initial_wire`
    /// first and closing gracefully on upstream completion, error, client disconnect, or shutdown.
    pub(crate) fn spawn(
        &self,
        client: TcpStream,
        initial_wire: Vec<u8>,
        spec: SseRelaySpec,
    ) -> Result<(), Option<TcpStream>> {
        if self.stopping.load(Ordering::Relaxed) {
            return Err(Some(client));
        }
        {
            let mut active = self
                .state
                .0
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            if *active >= self.max_relays || self.stopping.load(Ordering::Relaxed) {
                return Err(Some(client));
            }
            *active += 1;
        }

        let stopping = Arc::clone(&self.stopping);
        let state = Arc::clone(&self.state);
        let total_timeout = self.total_timeout;
        let spawn_res = std::thread::Builder::new()
            .name("mo-api-sse-relay".to_string())
            .stack_size(2 << 20)
            .spawn(move || {
                struct ActiveGuard(Arc<(Mutex<usize>, Condvar)>);
                impl Drop for ActiveGuard {
                    fn drop(&mut self) {
                        let mut active = self
                            .0
                            .0
                            .lock()
                            .unwrap_or_else(std::sync::PoisonError::into_inner);
                        *active = active.saturating_sub(1);
                        self.0.1.notify_all();
                    }
                }
                let _guard = ActiveGuard(state);
                run_relay(client, initial_wire, spec, stopping, total_timeout);
            });

        if spawn_res.is_err() {
            let mut active = self
                .state
                .0
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            *active = active.saturating_sub(1);
            self.state.1.notify_all();
            return Err(None);
        }
        Ok(())
    }

    /// Signals all relays to stop and waits until active relay threads finish exiting.
    pub(crate) fn shutdown(&self) {
        self.stopping.store(true, Ordering::Relaxed);
        let deadline = Instant::now() + Duration::from_secs(2);
        let mut active = self
            .state
            .0
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        while *active > 0 {
            let rem = deadline.saturating_duration_since(Instant::now());
            if rem.is_zero() {
                break;
            }
            let (guard, _) = self
                .state
                .1
                .wait_timeout(active, rem)
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            active = guard;
        }
    }
}

fn plain_ascii(s: &str, lowest: u8) -> bool {
    s.bytes().all(|b| b >= lowest && b != 0x7f)
}

fn parse_port_str(raw: &str) -> Option<u16> {
    if raw.is_empty() || raw.len() > 5 || !raw.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    let port = raw.parse::<u32>().ok()?;
    if (1..=65535).contains(&port) {
        Some(port as u16)
    } else {
        None
    }
}

/// Validates and decomposes an `http://` or `https://` URL without fragment/control chars.
pub(crate) fn parse_outbound_url(raw: &str) -> Option<ParsedUrl> {
    if !plain_ascii(raw, 33) || raw.contains('#') {
        return None;
    }
    let (secure, rest) = if let Some(r) = raw.strip_prefix("http://") {
        (false, r)
    } else {
        let r = raw.strip_prefix("https://")?;
        (true, r)
    };

    let split_at = rest.find(['/', '?']).unwrap_or(rest.len());
    let authority = &rest[..split_at];
    let tail = &rest[split_at..];
    if authority.is_empty() || authority.contains('@') {
        return None;
    }

    let (host, port) = if let Some(after_open) = authority.strip_prefix('[') {
        let close = after_open.find(']')?;
        let inner = &after_open[..close];
        if inner.is_empty() || inner.contains('[') || !inner.contains(':') {
            return None;
        }
        let suffix = &after_open[close + 1..];
        let port = if suffix.is_empty() {
            if secure { 443 } else { 80 }
        } else {
            let p_str = suffix.strip_prefix(':')?;
            parse_port_str(p_str)?
        };
        (inner.to_string(), port)
    } else {
        let mut colon_iter = authority.match_indices(':');
        match (colon_iter.next(), colon_iter.next()) {
            (None, _) => (authority.to_string(), if secure { 443 } else { 80 }),
            (Some((idx, _)), None) => {
                let h = &authority[..idx];
                if h.is_empty() {
                    return None;
                }
                let p = parse_port_str(&authority[idx + 1..])?;
                (h.to_string(), p)
            }
            (Some(_), Some(_)) => return None,
        }
    };

    if host.is_empty() || host.len() > MO_LIMIT_HOST_NAME as usize {
        return None;
    }

    let path = if tail.is_empty() {
        "/".to_string()
    } else if tail.starts_with('/') {
        tail.to_string()
    } else {
        format!("/{tail}")
    };

    Some(ParsedUrl {
        secure,
        authority: authority.to_string(),
        host,
        port,
        path,
    })
}

fn is_valid_header_pair(name: &str, value: &str) -> bool {
    !name.is_empty()
        && plain_ascii(name, 33)
        && !name.contains(':')
        && value.bytes().all(|b| (b >= 32 || b == b'\t') && b != 0x7f)
}

fn is_hop_by_hop_or_framing_header(name: &str) -> bool {
    name.eq_ignore_ascii_case("host")
        || name.eq_ignore_ascii_case("connection")
        || name.eq_ignore_ascii_case("content-length")
        || name.eq_ignore_ascii_case("transfer-encoding")
}

fn parse_optional_event_name(obj: &[(String, JsonValue)], key: &str) -> Option<Option<String>> {
    match find_json_field(obj, key) {
        None | Some(JsonValue::Null) => Some(None),
        Some(JsonValue::String(s)) => {
            let clean = sanitize_single_line(s).trim().to_string();
            if clean.is_empty() {
                Some(None)
            } else if clean.len() > MAX_EVENT_NAME_BYTES {
                None
            } else {
                Some(Some(clean))
            }
        }
        Some(_) => None,
    }
}

/// Parses and validates `X-Mo-Sse-Relay` JSON directive emitted by `Api.with_sse_relay` / `Api.sse_relay_stream`.
pub(crate) fn parse_relay_header(raw: &str) -> Option<SseRelaySpec> {
    let max_raw = (MO_LIMIT_API_SSE_RELAY_BODY_BYTES as usize).saturating_add(65536);
    if raw.len() > max_raw {
        return None;
    }
    let JsonValue::Object(obj) = parse_json(raw)? else {
        return None;
    };

    let Some(JsonValue::String(method_raw)) = find_json_field(&obj, "method") else {
        return None;
    };
    let method = method_raw.trim().to_string();
    if method.is_empty()
        || method.len() > MAX_METHOD_BYTES
        || !method.bytes().all(|b| b.is_ascii_alphabetic())
    {
        return None;
    }

    let Some(JsonValue::String(url_raw)) = find_json_field(&obj, "url") else {
        return None;
    };
    let target = parse_outbound_url(url_raw)?;

    let mut headers = Vec::new();
    if let Some(h_val) = find_json_field(&obj, "headers") {
        let JsonValue::Array(arr) = h_val else {
            return None;
        };
        if arr.len() > MAX_RELAY_HEADERS {
            return None;
        }
        for pair in arr {
            let JsonValue::Array(items) = pair else {
                return None;
            };
            if items.len() != 2 {
                return None;
            }
            let (JsonValue::String(name), JsonValue::String(val)) = (&items[0], &items[1]) else {
                return None;
            };
            if !is_valid_header_pair(name, val) {
                return None;
            }
            if !is_hop_by_hop_or_framing_header(name) {
                headers.push((name.clone(), val.clone()));
            }
        }
    }

    let body = match find_json_field(&obj, "body") {
        None | Some(JsonValue::Null) => String::new(),
        Some(JsonValue::String(s)) => {
            if s.len() > MO_LIMIT_API_SSE_RELAY_BODY_BYTES as usize {
                return None;
            }
            s.clone()
        }
        Some(_) => return None,
    };

    let mode = match find_json_field(&obj, "mode") {
        Some(JsonValue::String(m)) => match m.as_str() {
            "raw" => SseRelayMode::Raw,
            "openai_sse" => SseRelayMode::OpenAiSse,
            "anthropic_sse" => SseRelayMode::AnthropicSse,
            _ => return None,
        },
        _ => return None,
    };

    let max_idle_ms = MO_LIMIT_API_SSE_RELAY_IDLE_US / 1000;
    let idle_ms = match find_json_field(&obj, "idle_timeout_ms") {
        None | Some(JsonValue::Null) => max_idle_ms,
        Some(JsonValue::Number(num_str)) => {
            let parsed = num_str.trim().parse::<u64>().ok()?;
            parsed.clamp(MIN_IDLE_TIMEOUT_MS, max_idle_ms)
        }
        Some(_) => return None,
    };

    let token_event =
        parse_optional_event_name(&obj, "token_event")?.unwrap_or_else(|| "ai.token".to_string());
    let done_event = parse_optional_event_name(&obj, "done_event")?;
    let done_data = match find_json_field(&obj, "done_data") {
        None | Some(JsonValue::Null) => None,
        Some(JsonValue::String(s)) => {
            if s.len() > MO_LIMIT_API_SSE_RELAY_LINE_BYTES as usize {
                return None;
            }
            Some(s.clone())
        }
        Some(_) => return None,
    };
    let error_event =
        parse_optional_event_name(&obj, "error_event")?.unwrap_or_else(|| "ai.error".to_string());

    Some(SseRelaySpec {
        method,
        target,
        headers,
        body,
        mode,
        idle_timeout: Duration::from_millis(idle_ms),
        token_event,
        done_event,
        done_data,
        error_event,
    })
}

fn build_outbound_request(spec: &SseRelaySpec) -> Vec<u8> {
    let mut sorted = spec.headers.clone();
    sorted.sort_by_key(|a| a.0.to_ascii_lowercase());
    let mut head = format!(
        "{} {} HTTP/1.1\r\nHost: {}\r\nConnection: close\r\n",
        spec.method, spec.target.path, spec.target.authority
    );
    let body_bytes = spec.body.as_bytes();
    let sized = !body_bytes.is_empty() || !matches!(spec.method.as_str(), "GET" | "HEAD");
    if sized {
        head.push_str(&format!("Content-Length: {}\r\n", body_bytes.len()));
    }
    for (k, v) in &sorted {
        head.push_str(k);
        head.push_str(": ");
        head.push_str(v);
        head.push_str("\r\n");
    }
    head.push_str("\r\n");
    let mut wire = head.into_bytes();
    wire.extend_from_slice(body_bytes);
    wire
}

fn parse_status_code(line: &str) -> Option<u16> {
    let mut parts = line.split(' ');
    let version = parts.next()?;
    if !version.starts_with("HTTP/1.") {
        return None;
    }
    let digits = parts.next()?;
    if digits.len() != 3 || !digits.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    digits.parse::<u16>().ok()
}

fn parse_content_length_headers(headers: &[(String, String)]) -> Result<Option<usize>, ()> {
    let mut agreed: Option<usize> = None;
    let mut saw_cl = false;
    for (name, val) in headers {
        if !name.eq_ignore_ascii_case("content-length") {
            continue;
        }
        saw_cl = true;
        for member in val.split(',') {
            let digits = member.trim();
            if digits.is_empty() {
                continue;
            }
            if !digits.bytes().all(|b| b.is_ascii_digit()) {
                return Err(());
            }
            let parsed = digits.parse::<usize>().map_err(|_| ())?;
            if let Some(prev) = agreed
                && prev != parsed
            {
                return Err(());
            }
            agreed = Some(parsed);
        }
    }
    if saw_cl && agreed.is_none() {
        return Err(());
    }
    Ok(agreed)
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct RawSseFrame {
    event: Option<String>,
    id: Option<String>,
    data: String,
}

struct SseLineParser {
    line_buf: Vec<u8>,
    last_was_cr: bool,
    cur_event: Option<String>,
    cur_id: Option<String>,
    cur_data: Vec<String>,
    has_data: bool,
    cur_data_bytes: usize,
    saw_first_byte: bool,
}

impl SseLineParser {
    fn new() -> Self {
        Self {
            line_buf: Vec::new(),
            last_was_cr: false,
            cur_event: None,
            cur_id: None,
            cur_data: Vec::new(),
            has_data: false,
            cur_data_bytes: 0,
            saw_first_byte: false,
        }
    }

    fn dispatch_line(&mut self, frames: &mut Vec<RawSseFrame>) -> Result<(), &'static str> {
        let raw_line = String::from_utf8_lossy(&self.line_buf).into_owned();
        self.line_buf.clear();
        let line = if !self.saw_first_byte {
            self.saw_first_byte = true;
            raw_line.strip_prefix('\u{feff}').unwrap_or(&raw_line)
        } else {
            &raw_line
        };

        if line.is_empty() {
            if self.has_data || self.cur_event.is_some() || self.cur_id.is_some() {
                let data = self.cur_data.join("\n");
                self.cur_data.clear();
                self.cur_data_bytes = 0;
                self.has_data = false;
                let event = self.cur_event.take();
                let id = self.cur_id.take();
                if data.is_empty() && event.is_none() && id.is_none() {
                    return Ok(());
                }
                frames.push(RawSseFrame { event, id, data });
            }
            return Ok(());
        }

        if line.starts_with(':') {
            return Ok(());
        }

        let (field, value) = match line.find(':') {
            Some(idx) => {
                let raw_val = &line[idx + 1..];
                let trimmed_val = raw_val.strip_prefix(' ').unwrap_or(raw_val);
                (&line[..idx], trimmed_val)
            }
            None => (line, ""),
        };

        match field {
            "event" => {
                let clean = sanitize_single_line(value);
                if clean.is_empty() {
                    self.cur_event = None;
                } else if clean.len() <= MAX_EVENT_NAME_BYTES {
                    self.cur_event = Some(clean);
                }
            }
            "data" => {
                self.cur_data_bytes = self.cur_data_bytes.saturating_add(value.len() + 1);
                if self.cur_data_bytes > MO_LIMIT_API_SSE_RELAY_BODY_BYTES as usize {
                    return Err("upstream_stream_error");
                }
                self.cur_data.push(value.to_string());
                self.has_data = true;
            }
            "id" if !value.contains('\0') => {
                let clean = sanitize_single_line(value);
                if !clean.is_empty() && clean.len() <= MAX_EVENT_NAME_BYTES {
                    self.cur_id = Some(clean);
                }
            }
            _ => {}
        }
        Ok(())
    }

    fn feed(&mut self, bytes: &[u8], frames: &mut Vec<RawSseFrame>) -> Result<(), &'static str> {
        let line_limit = MO_LIMIT_API_SSE_RELAY_LINE_BYTES as usize;
        for &b in bytes {
            if self.last_was_cr {
                self.last_was_cr = false;
                if b == b'\n' {
                    continue;
                }
            }
            if b == b'\r' {
                self.last_was_cr = true;
                self.dispatch_line(frames)?;
                continue;
            }
            if b == b'\n' {
                self.dispatch_line(frames)?;
                continue;
            }
            if self.line_buf.len() >= line_limit {
                return Err("upstream_stream_error");
            }
            self.line_buf.push(b);
        }
        Ok(())
    }

    fn finish(&mut self, frames: &mut Vec<RawSseFrame>) -> Result<(), &'static str> {
        if !self.line_buf.is_empty() {
            self.dispatch_line(frames)?;
        }
        if self.has_data {
            self.line_buf.clear();
            self.dispatch_line(frames)?;
        }
        Ok(())
    }
}

enum ChunkState {
    ReadingSizeLine,
    ReadingData { rem: usize },
    ReadingDataCrlf,
    ReadingTrailers,
    Done,
}

struct ChunkedBodyDecoder {
    state: ChunkState,
    buf: Vec<u8>,
    total_decoded: usize,
}

impl ChunkedBodyDecoder {
    fn new() -> Self {
        Self {
            state: ChunkState::ReadingSizeLine,
            buf: Vec::new(),
            total_decoded: 0,
        }
    }

    fn feed(
        &mut self,
        input: &[u8],
        sse: &mut SseLineParser,
        frames: &mut Vec<RawSseFrame>,
    ) -> Result<bool, &'static str> {
        if !input.is_empty() {
            self.buf.extend_from_slice(input);
        }
        loop {
            match self.state {
                ChunkState::Done => return Ok(true),
                ChunkState::ReadingSizeLine => {
                    let Some(pos) = self.buf.windows(2).position(|w| w == b"\r\n") else {
                        if self.buf.len() > CHUNK_SIZE_LINE_LIMIT {
                            return Err("upstream_stream_error");
                        }
                        return Ok(false);
                    };
                    let size_line = &self.buf[..pos];
                    let hex_bytes = match size_line.iter().position(|&b| b == b';') {
                        Some(sc) => &size_line[..sc],
                        None => size_line,
                    };
                    let hex_str = std::str::from_utf8(hex_bytes)
                        .map_err(|_| "upstream_stream_error")?
                        .trim_end_matches([' ', '\t']);
                    if hex_str.is_empty()
                        || hex_str.len() > 8
                        || !hex_str.bytes().all(|b| b.is_ascii_hexdigit())
                    {
                        return Err("upstream_stream_error");
                    }
                    let chunk_sz =
                        usize::from_str_radix(hex_str, 16).map_err(|_| "upstream_stream_error")?;
                    self.buf.drain(..pos + 2);
                    if chunk_sz == 0 {
                        self.state = ChunkState::ReadingTrailers;
                    } else {
                        let new_total = self.total_decoded.saturating_add(chunk_sz);
                        if new_total > MO_LIMIT_API_SSE_RELAY_BODY_BYTES as usize {
                            return Err("upstream_stream_error");
                        }
                        self.total_decoded = new_total;
                        self.state = ChunkState::ReadingData { rem: chunk_sz };
                    }
                }
                ChunkState::ReadingData { rem } => {
                    if self.buf.is_empty() {
                        return Ok(false);
                    }
                    let take = self.buf.len().min(rem);
                    sse.feed(&self.buf[..take], frames)?;
                    self.buf.drain(..take);
                    if take == rem {
                        self.state = ChunkState::ReadingDataCrlf;
                    } else {
                        self.state = ChunkState::ReadingData { rem: rem - take };
                    }
                }
                ChunkState::ReadingDataCrlf => {
                    if self.buf.len() < 2 {
                        return Ok(false);
                    }
                    if &self.buf[..2] != b"\r\n" {
                        return Err("upstream_stream_error");
                    }
                    self.buf.drain(..2);
                    self.state = ChunkState::ReadingSizeLine;
                }
                ChunkState::ReadingTrailers => {
                    let Some(pos) = self.buf.windows(2).position(|w| w == b"\r\n") else {
                        if self.buf.len() > MO_LIMIT_API_SSE_RELAY_HEADER_BYTES as usize {
                            return Err("upstream_stream_error");
                        }
                        return Ok(false);
                    };
                    if pos == 0 {
                        self.buf.drain(..2);
                        self.state = ChunkState::Done;
                        return Ok(true);
                    }
                    self.buf.drain(..pos + 2);
                }
            }
        }
    }
}

enum UpstreamFraming {
    Chunked(ChunkedBodyDecoder),
    ContentLength { remaining: usize },
    CloseDelimited { total: usize },
}

enum FrameAction {
    Continue,
    EmitBytes(Vec<u8>),
    Done,
    ProviderError(Option<String>),
}

fn truncate_sanitized_detail(raw: &str) -> String {
    let clean = sanitize_single_line(raw);
    let trimmed = clean.trim();
    if trimmed.len() <= MAX_ERROR_DETAIL_BYTES {
        return trimmed.to_string();
    }
    let mut end = MAX_ERROR_DETAIL_BYTES;
    while end > 0 && !trimmed.is_char_boundary(end) {
        end -= 1;
    }
    trimmed[..end].to_string()
}

fn format_raw_sse_frame(frame: &RawSseFrame) -> Vec<u8> {
    use std::fmt::Write as _;

    let mut out = String::new();
    if let Some(id) = &frame.id {
        let clean = sanitize_single_line(id);
        if !clean.is_empty() {
            let _ = writeln!(&mut out, "id: {clean}");
        }
    }
    if let Some(ev) = &frame.event {
        let clean = sanitize_single_line(ev);
        if !clean.is_empty() {
            let _ = writeln!(&mut out, "event: {clean}");
        }
    }
    let normalized = frame.data.replace("\r\n", "\n").replace('\r', "\n");
    for line in normalized.split('\n') {
        out.push_str("data: ");
        out.push_str(line);
        out.push('\n');
    }
    out.push('\n');
    out.into_bytes()
}

fn extract_json_error_detail(obj: &[(String, JsonValue)]) -> Option<String> {
    match find_json_field(obj, "error")? {
        JsonValue::Object(err_obj) => match find_json_field(err_obj, "message") {
            Some(JsonValue::String(msg)) => Some(truncate_sanitized_detail(msg)),
            _ => None,
        },
        JsonValue::String(msg) => Some(truncate_sanitized_detail(msg)),
        _ => None,
    }
}

fn transform_sse_frame(spec: &SseRelaySpec, frame: RawSseFrame) -> FrameAction {
    match spec.mode {
        SseRelayMode::Raw => FrameAction::EmitBytes(format_raw_sse_frame(&frame)),
        SseRelayMode::OpenAiSse => {
            let trimmed = frame.data.trim();
            if trimmed == "[DONE]" {
                return FrameAction::Done;
            }
            if trimmed.is_empty() {
                return FrameAction::Continue;
            }
            let Some(JsonValue::Object(obj)) = parse_json(trimmed) else {
                return FrameAction::Continue;
            };
            if let Some(err) = find_json_field(&obj, "error")
                && !matches!(err, JsonValue::Null)
            {
                return FrameAction::ProviderError(extract_json_error_detail(&obj));
            }
            let Some(JsonValue::Array(choices)) = find_json_field(&obj, "choices") else {
                return FrameAction::Continue;
            };
            let Some(JsonValue::Object(first_choice)) = choices.first() else {
                return FrameAction::Continue;
            };
            let Some(JsonValue::Object(delta_obj)) = find_json_field(first_choice, "delta") else {
                return FrameAction::Continue;
            };
            let Some(JsonValue::String(content)) = find_json_field(delta_obj, "content") else {
                return FrameAction::Continue;
            };
            if content.is_empty() {
                return FrameAction::Continue;
            }
            let mut payload = String::with_capacity(content.len() + 16);
            payload.push_str("{\"delta\":");
            push_json_string(&mut payload, content);
            payload.push('}');
            FrameAction::EmitBytes(format_sse_event(None, Some(&spec.token_event), &payload))
        }
        SseRelayMode::AnthropicSse => {
            if frame.event.as_deref() == Some("message_stop") {
                return FrameAction::Done;
            }
            let trimmed = frame.data.trim();
            if frame.event.as_deref() == Some("error") {
                let detail = parse_json(trimmed).and_then(|v| {
                    if let JsonValue::Object(obj) = v {
                        extract_json_error_detail(&obj)
                    } else {
                        None
                    }
                });
                return FrameAction::ProviderError(detail);
            }
            if trimmed.is_empty() {
                return FrameAction::Continue;
            }
            let Some(JsonValue::Object(obj)) = parse_json(trimmed) else {
                return FrameAction::Continue;
            };
            match find_json_field(&obj, "type") {
                Some(JsonValue::String(kind)) if kind == "error" => {
                    return FrameAction::ProviderError(extract_json_error_detail(&obj));
                }
                Some(JsonValue::String(kind)) if kind == "message_stop" => {
                    return FrameAction::Done;
                }
                Some(JsonValue::String(kind)) if kind == "content_block_delta" => {}
                _ => return FrameAction::Continue,
            }
            let Some(JsonValue::Object(delta_obj)) = find_json_field(&obj, "delta") else {
                return FrameAction::Continue;
            };
            let Some(JsonValue::String(delta_type)) = find_json_field(delta_obj, "type") else {
                return FrameAction::Continue;
            };
            if delta_type != "text_delta" {
                return FrameAction::Continue;
            }
            let Some(JsonValue::String(text)) = find_json_field(delta_obj, "text") else {
                return FrameAction::Continue;
            };
            if text.is_empty() {
                return FrameAction::Continue;
            }
            let mut payload = String::with_capacity(text.len() + 16);
            payload.push_str("{\"delta\":");
            push_json_string(&mut payload, text);
            payload.push('}');
            FrameAction::EmitBytes(format_sse_event(None, Some(&spec.token_event), &payload))
        }
    }
}

fn write_chunk_and_flush(client: &mut TcpStream, sse_bytes: &[u8]) -> bool {
    if sse_bytes.is_empty() {
        return true;
    }
    let chunk = encode_http_chunk(sse_bytes);
    client.write_all(&chunk).is_ok() && client.flush().is_ok()
}

fn finish_chunked_stream(client: &mut TcpStream) {
    let _ = client.write_all(b"0\r\n\r\n");
    let _ = client.flush();
}

fn emit_relay_error(
    client: &mut TcpStream,
    error_event: &str,
    code: &str,
    status: Option<u16>,
    detail: Option<&str>,
) {
    let mut json = String::with_capacity(64);
    json.push_str("{\"error\":");
    push_json_string(&mut json, code);
    if let Some(st) = status {
        json.push_str(&format!(",\"status\":{st}"));
    }
    if let Some(msg) = detail
        && !msg.is_empty()
    {
        json.push_str(",\"detail\":");
        push_json_string(&mut json, msg);
    }
    json.push('}');
    let sse = format_sse_event(None, Some(error_event), &json);
    let _ = write_chunk_and_flush(client, &sse);
    finish_chunked_stream(client);
}

fn emit_done_if_configured(client: &mut TcpStream, spec: &SseRelaySpec) {
    if let Some(done_ev) = &spec.done_event {
        let data = spec.done_data.as_deref().unwrap_or("{}");
        let sse = format_sse_event(None, Some(done_ev), data);
        let _ = write_chunk_and_flush(client, &sse);
    }
    finish_chunked_stream(client);
}

fn map_outbound_error_code(err: &OutboundError) -> &'static str {
    match err {
        OutboundError::TimedOut => "upstream_timeout",
        OutboundError::Closed | OutboundError::Broken(_) => "upstream_stream_error",
        OutboundError::Refused | OutboundError::UnknownHost | OutboundError::Certificate(_) => {
            "upstream_connect_failed"
        }
    }
}

struct ResponseHead {
    status: u16,
    headers: Vec<(String, String)>,
    trailing: Vec<u8>,
}

fn read_response_head(
    upstream: &mut OutboundStream,
    stopping: &AtomicBool,
) -> Result<ResponseHead, &'static str> {
    let mut head_buf = Vec::new();
    let mut tmp = [0u8; 8192];
    let head_limit = MO_LIMIT_API_SSE_RELAY_HEADER_BYTES as usize;
    let cut = loop {
        if let Some(pos) = head_buf.windows(4).position(|w| w == b"\r\n\r\n") {
            break pos;
        }
        if head_buf.len() >= head_limit {
            return Err("upstream_stream_error");
        }
        match upstream.recv_some(&mut tmp, stopping) {
            Ok(0) => return Err("upstream_stream_error"),
            Ok(n) => head_buf.extend_from_slice(&tmp[..n]),
            Err(e) => return Err(map_outbound_error_code(&e)),
        }
    };

    if cut > head_limit {
        return Err("upstream_stream_error");
    }
    let head_str = std::str::from_utf8(&head_buf[..cut]).map_err(|_| "upstream_stream_error")?;
    let mut lines = head_str.split("\r\n");
    let status_line = lines.next().ok_or("upstream_stream_error")?;
    if status_line
        .bytes()
        .any(|b| b == 0x7f || (b < 0x20 && b != b'\t'))
    {
        return Err("upstream_stream_error");
    }
    let status = parse_status_code(status_line).ok_or("upstream_stream_error")?;

    let mut headers = Vec::new();
    for line in lines {
        if line.bytes().any(|b| b == 0x7f || (b < 0x20 && b != b'\t')) {
            return Err("upstream_stream_error");
        }
        let colon = line.find(':').ok_or("upstream_stream_error")?;
        let name = &line[..colon];
        if name.is_empty() || !plain_ascii(name, 33) {
            return Err("upstream_stream_error");
        }
        let val = line[colon + 1..].trim();
        headers.push((name.to_string(), val.to_string()));
    }

    let trailing = head_buf[cut + 4..].to_vec();
    Ok(ResponseHead {
        status,
        headers,
        trailing,
    })
}

fn process_frames(
    client: &mut TcpStream,
    spec: &SseRelaySpec,
    frames: &mut Vec<RawSseFrame>,
) -> Result<bool, Option<String>> {
    for frame in mem::take(frames) {
        match transform_sse_frame(spec, frame) {
            FrameAction::Continue => {}
            FrameAction::EmitBytes(bytes) => {
                if !write_chunk_and_flush(client, &bytes) {
                    return Err(None);
                }
            }
            FrameAction::Done => return Ok(true),
            FrameAction::ProviderError(detail) => return Err(Some(detail.unwrap_or_default())),
        }
    }
    Ok(false)
}

fn run_relay(
    mut client: TcpStream,
    initial_wire: Vec<u8>,
    spec: SseRelaySpec,
    stopping: Arc<AtomicBool>,
    total_timeout: Duration,
) {
    let _ = client.set_nodelay(true);
    let _ = client.set_write_timeout(Some(Duration::from_micros(MO_LIMIT_API_WRITE_US)));

    // 1. Flush HTTP 200 headers and initial chunks (e.g. ai.citation) to the browser immediately.
    if client.write_all(&initial_wire).is_err() || client.flush().is_err() {
        return;
    }
    if stopping.load(Ordering::Relaxed) {
        finish_chunked_stream(&mut client);
        return;
    }

    // 2. Connect upstream with per-chunk idle timeout and bounded total lifetime.
    let connect_timeout = Duration::from_micros(MO_LIMIT_API_SSE_RELAY_CONNECT_US);
    let mut upstream = match OutboundStream::connect(
        &spec.target.host,
        spec.target.port,
        spec.target.secure,
        connect_timeout,
        spec.idle_timeout,
        total_timeout,
    ) {
        Ok(s) => s,
        Err(err) => {
            emit_relay_error(
                &mut client,
                &spec.error_event,
                map_outbound_error_code(&err),
                None,
                None,
            );
            return;
        }
    };

    // 3. Send signed HTTP request upstream.
    let req_wire = build_outbound_request(&spec);
    if let Err(err) = upstream.send_all(&req_wire, &stopping) {
        emit_relay_error(
            &mut client,
            &spec.error_event,
            map_outbound_error_code(&err),
            None,
            None,
        );
        return;
    }

    // 4. Read and validate upstream response head.
    let ResponseHead {
        status,
        headers,
        trailing: initial_rest,
    } = match read_response_head(&mut upstream, &stopping) {
        Ok(head) => head,
        Err(code) => {
            emit_relay_error(&mut client, &spec.error_event, code, None, None);
            return;
        }
    };
    if !(200..300).contains(&status) {
        emit_relay_error(
            &mut client,
            &spec.error_event,
            "upstream_http_status",
            Some(status),
            None,
        );
        return;
    }
    if status == 204 || status == 304 || spec.method == "HEAD" {
        emit_done_if_configured(&mut client, &spec);
        return;
    }

    let te_values: Vec<&str> = headers
        .iter()
        .filter(|(k, _)| k.eq_ignore_ascii_case("transfer-encoding"))
        .map(|(_, v)| v.as_str())
        .collect();
    let mut framing = if !te_values.is_empty() {
        let joined = te_values.join(", ");
        if !joined.eq_ignore_ascii_case("chunked") {
            emit_relay_error(
                &mut client,
                &spec.error_event,
                "upstream_stream_error",
                None,
                None,
            );
            return;
        }
        UpstreamFraming::Chunked(ChunkedBodyDecoder::new())
    } else {
        match parse_content_length_headers(&headers) {
            Ok(Some(len)) => {
                if len > MO_LIMIT_API_SSE_RELAY_BODY_BYTES as usize {
                    emit_relay_error(
                        &mut client,
                        &spec.error_event,
                        "upstream_stream_error",
                        None,
                        None,
                    );
                    return;
                }
                UpstreamFraming::ContentLength { remaining: len }
            }
            Ok(None) => UpstreamFraming::CloseDelimited { total: 0 },
            Err(()) => {
                emit_relay_error(
                    &mut client,
                    &spec.error_event,
                    "upstream_stream_error",
                    None,
                    None,
                );
                return;
            }
        }
    };

    let mut sse = SseLineParser::new();
    let mut frames = Vec::new();
    let mut pending_slice = initial_rest;
    let mut read_buf = [0u8; 8192];

    loop {
        if stopping.load(Ordering::Relaxed) {
            emit_relay_error(
                &mut client,
                &spec.error_event,
                "upstream_stream_error",
                None,
                None,
            );
            return;
        }

        let stream_finished = if !pending_slice.is_empty() {
            match &mut framing {
                UpstreamFraming::Chunked(dec) => {
                    match dec.feed(&pending_slice, &mut sse, &mut frames) {
                        Ok(done) => done,
                        Err(code) => {
                            emit_relay_error(&mut client, &spec.error_event, code, None, None);
                            return;
                        }
                    }
                }
                UpstreamFraming::ContentLength { remaining } => {
                    let take = pending_slice.len().min(*remaining);
                    if let Err(code) = sse.feed(&pending_slice[..take], &mut frames) {
                        emit_relay_error(&mut client, &spec.error_event, code, None, None);
                        return;
                    }
                    *remaining -= take;
                    *remaining == 0
                }
                UpstreamFraming::CloseDelimited { total } => {
                    let next_total = total.saturating_add(pending_slice.len());
                    if next_total > MO_LIMIT_API_SSE_RELAY_BODY_BYTES as usize {
                        emit_relay_error(
                            &mut client,
                            &spec.error_event,
                            "upstream_stream_error",
                            None,
                            None,
                        );
                        return;
                    }
                    *total = next_total;
                    if let Err(code) = sse.feed(&pending_slice, &mut frames) {
                        emit_relay_error(&mut client, &spec.error_event, code, None, None);
                        return;
                    }
                    false
                }
            }
        } else {
            matches!(&framing, UpstreamFraming::ContentLength { remaining: 0 })
        };
        pending_slice.clear();

        match process_frames(&mut client, &spec, &mut frames) {
            Ok(true) => {
                emit_done_if_configured(&mut client, &spec);
                return;
            }
            Ok(false) => {}
            Err(None) => return,
            Err(Some(detail)) => {
                emit_relay_error(
                    &mut client,
                    &spec.error_event,
                    "upstream_provider_error",
                    None,
                    (!detail.is_empty()).then_some(&detail),
                );
                return;
            }
        }

        if stream_finished {
            if let Err(code) = sse.finish(&mut frames) {
                emit_relay_error(&mut client, &spec.error_event, code, None, None);
                return;
            }
            match process_frames(&mut client, &spec, &mut frames) {
                Ok(_) => emit_done_if_configured(&mut client, &spec),
                Err(None) => {}
                Err(Some(detail)) => emit_relay_error(
                    &mut client,
                    &spec.error_event,
                    "upstream_provider_error",
                    None,
                    (!detail.is_empty()).then_some(&detail),
                ),
            }
            return;
        }

        match upstream.recv_some(&mut read_buf, &stopping) {
            Ok(0) => {
                let valid_eof = match &framing {
                    UpstreamFraming::CloseDelimited { .. } => true,
                    UpstreamFraming::ContentLength { remaining } => *remaining == 0,
                    UpstreamFraming::Chunked(dec) => matches!(dec.state, ChunkState::Done),
                };
                if !valid_eof {
                    emit_relay_error(
                        &mut client,
                        &spec.error_event,
                        "upstream_stream_error",
                        None,
                        None,
                    );
                    return;
                }
                if let Err(code) = sse.finish(&mut frames) {
                    emit_relay_error(&mut client, &spec.error_event, code, None, None);
                    return;
                }
                match process_frames(&mut client, &spec, &mut frames) {
                    Ok(_) => emit_done_if_configured(&mut client, &spec),
                    Err(None) => {}
                    Err(Some(detail)) => emit_relay_error(
                        &mut client,
                        &spec.error_event,
                        "upstream_provider_error",
                        None,
                        (!detail.is_empty()).then_some(&detail),
                    ),
                }
                return;
            }
            Ok(n) => {
                pending_slice.extend_from_slice(&read_buf[..n]);
            }
            Err(err) => {
                emit_relay_error(
                    &mut client,
                    &spec.error_event,
                    map_outbound_error_code(&err),
                    None,
                    None,
                );
                return;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Read as _;
    use std::net::TcpListener;

    #[test]
    fn relay_header_parser_validates_fields_and_strips_framing_headers() {
        let raw = r#"{
            "method":"POST",
            "url":"https://api.openai.com/v1/chat/completions",
            "headers":[
                ["Authorization","Bearer secret-key"],
                ["Content-Type","application/json"],
                ["Host","evil.example"],
                ["Transfer-Encoding","chunked"]
            ],
            "body":"{\"model\":\"gpt-4o-mini\",\"stream\":true}",
            "mode":"openai_sse",
            "idle_timeout_ms":5000,
            "token_event":"ai.token",
            "done_event":"ai.done",
            "done_data":"{\"model\":\"gpt-4o-mini\"}",
            "error_event":"ai.error"
        }"#;
        let spec = parse_relay_header(raw).expect("valid relay directive");
        assert_eq!(spec.method, "POST");
        assert_eq!(spec.target.host, "api.openai.com");
        assert_eq!(spec.target.port, 443);
        assert!(spec.target.secure);
        assert_eq!(spec.target.path, "/v1/chat/completions");
        assert_eq!(
            spec.headers,
            vec![
                ("Authorization".to_string(), "Bearer secret-key".to_string()),
                ("Content-Type".to_string(), "application/json".to_string()),
            ]
        );
        assert_eq!(spec.mode, SseRelayMode::OpenAiSse);
        assert_eq!(spec.idle_timeout, Duration::from_millis(5000));
        assert_eq!(spec.done_event.as_deref(), Some("ai.done"));

        assert_eq!(
            parse_relay_header(r#"{"method":"GET","url":"ftp://x/a","mode":"raw"}"#),
            None
        );
        assert_eq!(
            parse_relay_header(
                r#"{"method":"GET","url":"http://example.com/","headers":[["Bad\nName","v"]],"mode":"raw"}"#
            ),
            None
        );
        assert_eq!(
            parse_relay_header(r#"{"method":"GET","url":"http://example.com/","mode":"unknown"}"#),
            None
        );
    }

    #[test]
    fn transforms_openai_anthropic_and_raw_sse_frames() {
        let base_target = parse_outbound_url("http://127.0.0.1:8080/v1").unwrap();
        let make_spec = |mode| SseRelaySpec {
            method: "POST".into(),
            target: base_target.clone(),
            headers: Vec::new(),
            body: String::new(),
            mode,
            idle_timeout: Duration::from_secs(5),
            token_event: "ai.token".into(),
            done_event: Some("ai.done".into()),
            done_data: Some("{\"model\":\"m\"}".into()),
            error_event: "ai.error".into(),
        };

        let oa_spec = make_spec(SseRelayMode::OpenAiSse);
        let oa_frame = RawSseFrame {
            event: None,
            id: None,
            data: r#"{"choices":[{"delta":{"content":"Hi\nthere"}}]}"#.into(),
        };
        let oa_out = match transform_sse_frame(&oa_spec, oa_frame) {
            FrameAction::EmitBytes(bytes) => Some(bytes),
            _ => None,
        };
        assert_eq!(
            oa_out.as_deref().and_then(|b| std::str::from_utf8(b).ok()),
            Some("event: ai.token\ndata: {\"delta\":\"Hi\\nthere\"}\n\n")
        );
        assert!(matches!(
            transform_sse_frame(
                &oa_spec,
                RawSseFrame {
                    event: None,
                    id: None,
                    data: "[DONE]".into()
                }
            ),
            FrameAction::Done
        ));

        let ant_spec = make_spec(SseRelayMode::AnthropicSse);
        let ant_frame = RawSseFrame {
            event: Some("content_block_delta".into()),
            id: None,
            data: r#"{"type":"content_block_delta","delta":{"type":"text_delta","text":"Claude"}}"#
                .into(),
        };
        let ant_out = match transform_sse_frame(&ant_spec, ant_frame) {
            FrameAction::EmitBytes(bytes) => Some(bytes),
            _ => None,
        };
        assert_eq!(
            ant_out.as_deref().and_then(|b| std::str::from_utf8(b).ok()),
            Some("event: ai.token\ndata: {\"delta\":\"Claude\"}\n\n")
        );
        assert!(matches!(
            transform_sse_frame(
                &ant_spec,
                RawSseFrame {
                    event: Some("message_stop".into()),
                    id: None,
                    data: r#"{"type":"message_stop"}"#.into()
                }
            ),
            FrameAction::Done
        ));

        let raw_spec = make_spec(SseRelayMode::Raw);
        let raw_frame = RawSseFrame {
            event: Some("custom".into()),
            id: Some("7".into()),
            data: "line1\nline2".into(),
        };
        let raw_out = match transform_sse_frame(&raw_spec, raw_frame) {
            FrameAction::EmitBytes(bytes) => Some(bytes),
            _ => None,
        };
        assert_eq!(
            raw_out.as_deref().and_then(|b| std::str::from_utf8(b).ok()),
            Some("id: 7\nevent: custom\ndata: line1\ndata: line2\n\n")
        );
    }

    #[test]
    fn relay_streams_chunked_openai_sse_and_emits_timeout_error_cleanly()
    -> Result<(), Box<dyn std::error::Error>> {
        let upstream = TcpListener::bind("127.0.0.1:0")?;
        let up_addr = upstream.local_addr()?;

        let up_handle = std::thread::spawn(move || {
            // Connection 1: healthy chunked OpenAI SSE response split across packets
            let (mut s1, _) = upstream.accept().unwrap();
            let mut req_buf = [0u8; 512];
            let _ = s1.read(&mut req_buf);
            s1.write_all(
                b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n",
            )
            .unwrap();
            let c1 = b"data: {\"choices\":[{\"delta\":{\"content\":\"Hello \"}}]}\n\n";
            s1.write_all(format!("{:x}\r\n", c1.len()).as_bytes())
                .unwrap();
            s1.write_all(c1).unwrap();
            s1.write_all(b"\r\n").unwrap();
            s1.flush().unwrap();
            std::thread::sleep(Duration::from_millis(80));
            let c2 =
                b"data: {\"choices\":[{\"delta\":{\"content\":\"world\"}}]}\n\ndata: [DONE]\n\n";
            s1.write_all(format!("{:x}\r\n", c2.len()).as_bytes())
                .unwrap();
            s1.write_all(c2).unwrap();
            s1.write_all(b"\r\n0\r\n\r\n").unwrap();
            s1.flush().unwrap();

            // Connection 2: stalls after initial header -> triggers idle timeout
            let (mut s2, _) = upstream.accept().unwrap();
            let _ = s2.read(&mut req_buf);
            s2.write_all(
                b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n",
            )
            .unwrap();
            s2.flush().unwrap();
            std::thread::sleep(Duration::from_millis(350));
        });

        let pool = SseRelayPool::with_limits(4, Duration::from_secs(10));

        // Test 1: Healthy relay
        let client_listener = TcpListener::bind("127.0.0.1:0")?;
        let cl_addr = client_listener.local_addr()?;
        let mut client_rx = TcpStream::connect(cl_addr)?;
        let (srv_stream, _) = client_listener.accept()?;
        let spec_ok = SseRelaySpec {
            method: "POST".into(),
            target: parse_outbound_url(&format!("http://127.0.0.1:{}/v1/chat", up_addr.port()))
                .unwrap(),
            headers: vec![("Authorization".into(), "Bearer super-secret-key".into())],
            body: "{}".into(),
            mode: SseRelayMode::OpenAiSse,
            idle_timeout: Duration::from_secs(2),
            token_event: "ai.token".into(),
            done_event: Some("ai.done".into()),
            done_data: Some("{\"model\":\"m\"}".into()),
            error_event: "ai.error".into(),
        };
        let init = b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1d\r\nevent: ai.citation\ndata: {}\n\n\r\n".to_vec();
        pool.spawn(srv_stream, init, spec_ok)
            .map_err(|_| "spawn ok")?;
        let mut out1 = String::new();
        client_rx.read_to_string(&mut out1)?;
        assert!(out1.contains("event: ai.citation"));
        assert!(out1.contains("{\"delta\":\"Hello \"}"));
        assert!(out1.contains("{\"delta\":\"world\"}"));
        assert!(out1.contains("event: ai.done\ndata: {\"model\":\"m\"}"));
        assert!(!out1.contains("super-secret-key"));

        // Test 2: Stalled upstream triggers idle timeout ai.error
        let mut client_rx2 = TcpStream::connect(cl_addr)?;
        let (srv_stream2, _) = client_listener.accept()?;
        let spec_timeout = SseRelaySpec {
            method: "POST".into(),
            target: parse_outbound_url(&format!("http://127.0.0.1:{}/v1/chat", up_addr.port()))
                .unwrap(),
            headers: vec![("Authorization".into(), "Bearer super-secret-key".into())],
            body: "{}".into(),
            mode: SseRelayMode::OpenAiSse,
            idle_timeout: Duration::from_millis(120),
            token_event: "ai.token".into(),
            done_event: Some("ai.done".into()),
            done_data: None,
            error_event: "ai.error".into(),
        };
        pool.spawn(
            srv_stream2,
            b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n".to_vec(),
            spec_timeout,
        )
        .map_err(|_| "spawn timeout")?;
        let mut out2 = String::new();
        client_rx2.read_to_string(&mut out2)?;
        assert!(out2.contains("event: ai.error\ndata: {\"error\":\"upstream_timeout\"}"));
        assert!(!out2.contains("event: ai.done"));
        assert!(!out2.contains("super-secret-key"));

        pool.shutdown();
        up_handle.join().unwrap();
        Ok(())
    }
}
