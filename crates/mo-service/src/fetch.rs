//! Offline `mo fetch` client (`src/tools/fetch.c` parity).
//!
//! Connects directly to an operator Unix domain socket (never honoring `HTTP_PROXY`),
//! calls `mo-export` via MCP JSON-RPC, parses replies through embedded `McpFetch.read`,
//! verifies pages and final `MOEX\x01` artifact via `ExportConsumer`, and writes the
//! output atomically with mode `0600` (`O_CREAT | O_EXCL | O_NOFOLLOW` + `sync_all` +
//! `hard_link` + `remove_file`).

use std::fs::{self, OpenOptions};
use std::io::{Read, Write};
#[cfg(unix)]
use std::os::unix::fs::OpenOptionsExt;
#[cfg(unix)]
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use mo_def::limits::{
    MO_LIMIT_EXPORT_PAGE_BYTES, MO_LIMIT_EXPORT_SCRATCH, MO_LIMIT_FETCH_RECEIVE_BYTES,
    MO_LIMIT_FETCH_US, MO_LIMIT_MCP_RATE, MO_LIMIT_MCP_REQUEST_WORK,
};
use mo_host::HostContext;
use mo_vm::{Image, Limits, Outcome, Process};

use crate::binary::export::{ExportConsumer, ExportPage};
use crate::binary::{hex_decode_exact, push_json_string};
use crate::listener::signals::{install_signal_handlers, is_signalled};
use crate::protocol::{Pf, ProtocolFunctions, build_protocol_image};

/// Shared Unix-socket admin RPC helper used by both `mo fetch` and `mo push`.
pub struct AdminRpcClient {
    socket_path: PathBuf,
    deadline: Instant,
    last_sent: Option<Instant>,
}

impl AdminRpcClient {
    /// Creates a new client targeting `socket_path` with a total wall-clock budget.
    pub fn new(socket_path: &Path, total_timeout: Duration) -> Result<Self, String> {
        #[cfg(target_os = "linux")]
        const UNIX_PATH_LIMIT: usize = 108;
        #[cfg(not(target_os = "linux"))]
        const UNIX_PATH_LIMIT: usize = 104;
        let raw = socket_path.as_os_str();
        if raw.is_empty() || raw.len() >= UNIX_PATH_LIMIT {
            return Err("admin socket path is invalid or too long".to_string());
        }
        Ok(Self {
            socket_path: socket_path.to_path_buf(),
            deadline: Instant::now() + total_timeout,
            last_sent: None,
        })
    }

    /// Returns `true` if deadline has expired or a stop signal arrived.
    #[must_use]
    pub fn expired_or_cancelled(&self) -> bool {
        is_signalled() || Instant::now() >= self.deadline
    }

    /// Sends one MCP `tools/call` over the Unix socket and returns the raw HTTP response bytes.
    #[cfg(unix)]
    pub fn call_tool(&mut self, tool: &str, arguments_json: &str) -> Result<Vec<u8>, String> {
        let min_interval = Duration::from_micros(1_000_000 / MO_LIMIT_MCP_RATE + 5_000);
        if let Some(prev) = self.last_sent {
            let elapsed = prev.elapsed();
            if elapsed < min_interval {
                std::thread::sleep(min_interval - elapsed);
            }
        }
        if self.expired_or_cancelled() {
            return Err("timed out or cancelled".to_string());
        }

        let mut body = String::with_capacity(96 + tool.len() + arguments_json.len());
        body.push_str(
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":",
        );
        push_json_string(&mut body, tool);
        body.push_str(",\"arguments\":");
        body.push_str(arguments_json);
        body.push_str("}}");

        let req = format!(
            "POST /mcp HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n\
             MCP-Protocol-Version: 2025-11-25\r\nConnection: close\r\nContent-Length: {}\r\n\r\n{}",
            body.len(),
            body
        );

        let mut stream = UnixStream::connect(&self.socket_path)
            .map_err(|e| format!("could not connect to admin socket: {e}"))?;
        let rem = self.deadline.saturating_duration_since(Instant::now());
        if rem.is_zero() {
            return Err("timed out".to_string());
        }
        let _ = stream.set_read_timeout(Some(rem.min(Duration::from_secs(30))));
        let _ = stream.set_write_timeout(Some(rem.min(Duration::from_secs(30))));

        stream
            .write_all(req.as_bytes())
            .map_err(|e| format!("failed to send request: {e}"))?;
        let _ = stream.flush();
        self.last_sent = Some(Instant::now());

        let mut wire = Vec::new();
        let mut buf = [0u8; 8192];
        loop {
            if self.expired_or_cancelled() {
                return Err("timed out or cancelled".to_string());
            }
            match stream.read(&mut buf) {
                Ok(0) => break,
                Ok(n) => {
                    if wire.len() + n > MO_LIMIT_FETCH_RECEIVE_BYTES as usize {
                        return Err("response exceeded receive limit".to_string());
                    }
                    wire.extend_from_slice(&buf[..n]);
                }
                Err(e)
                    if matches!(
                        e.kind(),
                        std::io::ErrorKind::Interrupted | std::io::ErrorKind::WouldBlock
                    ) =>
                {
                    continue;
                }
                Err(e) if e.kind() == std::io::ErrorKind::ConnectionReset => break,
                Err(e) => return Err(format!("failed to read response: {e}")),
            }
        }
        Ok(wire)
    }
}

/// Calls `McpFetch.read(wire)` in the embedded protocol VM to parse one `mo-export` HTTP response.
enum FetchPageReply {
    RateLimited,
    Page(ExportPage),
    Error(String),
}

fn parse_fetch_page(
    proto: &Image,
    fns: &ProtocolFunctions,
    wire: &[u8],
) -> Result<FetchPageReply, String> {
    let wire_str = std::str::from_utf8(wire).map_err(|_| "non-UTF-8 HTTP response".to_string())?;
    let limits = Limits {
        heap_size: MO_LIMIT_EXPORT_SCRATCH as usize,
        ..Limits::default()
    };
    let mut proc = Process::new(proto, limits, HostContext::stdio(Vec::new(), Vec::new()));
    let arg = proc
        .heap()
        .alloc_text(wire_str.to_string())
        .map_err(|_| "out of memory".to_string())?;
    let (outcome, _) =
        proc.call_with_budget(fns.get(Pf::FetchRead), vec![arg], MO_LIMIT_MCP_REQUEST_WORK);
    let list_val = match outcome {
        Outcome::Returned(v) => v,
        Outcome::Failed(d) | Outcome::Stopped(d) => {
            return Err(format!("McpFetch.read failed: {}", d.reason));
        }
    };
    let items: Vec<String> = list_val
        .as_list()
        .iter()
        .map(|v| v.as_text().to_string())
        .collect();
    if items.is_empty() {
        return Err("invalid export reply".to_string());
    }
    if items.len() == 1 {
        if items[0] == "rate_limited" {
            return Ok(FetchPageReply::RateLimited);
        }
        return Ok(FetchPageReply::Error(items[0].clone()));
    }
    if items.len() < 10 {
        return Err("incomplete export page fields".to_string());
    }
    // Exact 10 fields returned by runtime/mcp-fetch.mo:read:
    // [0]=export(32hex), [1]=sequence, [2]=encoded_sequence, [3]=offset,
    // [4]=length, [5]=total, [6]=pages, [7]=checksum(16hex BE),
    // [8]=digest(64hex or ""), [9]=base64
    let id = hex_decode_exact::<16>(&items[0]).ok_or_else(|| "invalid export id".to_string())?;
    let sequence: u64 = items[1]
        .parse()
        .map_err(|_| "invalid sequence".to_string())?;
    let encoded_sequence: u64 = items[2]
        .parse()
        .map_err(|_| "invalid encoded_sequence".to_string())?;
    let offset: usize = items[3].parse().map_err(|_| "invalid offset".to_string())?;
    let length: usize = items[4].parse().map_err(|_| "invalid length".to_string())?;
    let total: usize = items[5].parse().map_err(|_| "invalid total".to_string())?;
    let count: usize = items[6].parse().map_err(|_| "invalid count".to_string())?;
    let csum_bytes =
        hex_decode_exact::<8>(&items[7]).ok_or_else(|| "invalid checksum".to_string())?;
    let checksum = u64::from_be_bytes(csum_bytes);
    let is_final = offset.checked_add(length) == Some(total);
    let digest = if is_final {
        hex_decode_exact::<32>(&items[8]).ok_or_else(|| "invalid digest".to_string())?
    } else {
        [0u8; 32]
    };
    let bytes = decode_base64(&items[9]).ok_or_else(|| "invalid page base64".to_string())?;
    if bytes.len() != length {
        return Err("page byte length mismatch".to_string());
    }
    Ok(FetchPageReply::Page(ExportPage {
        id,
        digest,
        sequence,
        encoded_sequence,
        checksum,
        offset,
        total,
        count,
        is_final,
        bytes,
    }))
}

fn decode_base64(input: &str) -> Option<Vec<u8>> {
    let b = input.as_bytes();
    if !b.len().is_multiple_of(4) {
        return None;
    }
    let mut out = Vec::with_capacity((b.len() / 4) * 3);
    for (idx, chunk) in b.as_chunks::<4>().0.iter().enumerate() {
        let is_last = idx + 1 == b.len() / 4;
        let v0 = b64_val(chunk[0])?;
        let v1 = b64_val(chunk[1])?;
        out.push((v0 << 2) | (v1 >> 4));
        if chunk[2] == b'=' {
            if !is_last || chunk[3] != b'=' || (v1 & 0x0f) != 0 {
                return None;
            }
        } else {
            let v2 = b64_val(chunk[2])?;
            out.push((v1 << 4) | (v2 >> 2));
            if chunk[3] == b'=' {
                if !is_last || (v2 & 0x03) != 0 {
                    return None;
                }
            } else {
                let v3 = b64_val(chunk[3])?;
                out.push((v2 << 6) | v3);
            }
        }
    }
    Some(out)
}

fn b64_val(c: u8) -> Option<u8> {
    match c {
        b'A'..=b'Z' => Some(c - b'A'),
        b'a'..=b'z' => Some(c - b'a' + 26),
        b'0'..=b'9' => Some(c - b'0' + 52),
        b'+' => Some(62),
        b'/' => Some(63),
        _ => None,
    }
}

/// Configuration for `mo fetch`.
pub struct FetchOptions<'a> {
    /// Admin Unix domain socket path.
    pub admin_socket: &'a Path,
    /// Output `.moex` file path (must not already exist).
    pub output_path: &'a Path,
    /// Optional failure record id (`None` or `0` fetches current live state).
    pub failure_id: Option<u64>,
    /// Whether to emit JSON output on stdout.
    pub json: bool,
}

/// Fetches a live state or failure `MOEX\x01` export artifact into memory from an admin Unix domain
/// socket (`src/tools/fetch.c:mo_fetch_live_export` parity, used by `mo query --admin-socket`).
#[cfg(unix)]
pub fn fetch_live_export(admin_socket: &Path, failure_id: Option<u64>) -> Result<Vec<u8>, String> {
    let _ = install_signal_handlers();
    let (proto, fns) = build_protocol_image()?;
    let mut client = AdminRpcClient::new(admin_socket, Duration::from_micros(MO_LIMIT_FETCH_US))?;
    let mut consumer = ExportConsumer::default();
    let mut export_id_hex = String::new();
    let mut offset = 0usize;
    let page_sz = MO_LIMIT_EXPORT_PAGE_BYTES as usize;

    loop {
        let args_json = match (export_id_hex.is_empty(), failure_id.filter(|&id| id > 0)) {
            (true, None) => "{\"kind\":\"state\",\"offset\":0}".to_string(),
            (true, Some(fid)) => {
                format!("{{\"kind\":\"failure\",\"failure\":{fid},\"offset\":0}}")
            }
            (false, None) => {
                format!("{{\"kind\":\"state\",\"export\":\"{export_id_hex}\",\"offset\":{offset}}}")
            }
            (false, Some(fid)) => {
                format!(
                    "{{\"kind\":\"failure\",\"failure\":{fid},\"export\":\"{export_id_hex}\",\"offset\":{offset}}}"
                )
            }
        };
        let wire = client.call_tool("mo-export", &args_json)?;
        match parse_fetch_page(&proto, &fns, &wire)? {
            FetchPageReply::RateLimited => {
                std::thread::sleep(Duration::from_millis(120));
            }
            FetchPageReply::Error(reason) => {
                let msg = match reason.as_str() {
                    "export_replaced" => "export replaced by a newer export",
                    "export_released" => "export was released or expired before completion",
                    other => other,
                };
                return Err(msg.to_string());
            }
            FetchPageReply::Page(page) => {
                if export_id_hex.is_empty() {
                    export_id_hex = crate::binary::hex_encode(&page.id);
                }
                let is_final = page.is_final;
                if !consumer.accept(&page) {
                    return Err("export page verification failed".to_string());
                }
                if is_final {
                    break;
                }
                offset += page_sz;
            }
        }
    }
    if !consumer.complete {
        return Err("incomplete export".to_string());
    }
    Ok(consumer.bytes)
}

/// Executes `mo fetch` (`src/tools/fetch.c:mo_fetch`).
#[cfg(unix)]
pub fn run_fetch(opts: &FetchOptions<'_>, output: &mut dyn Write, errors: &mut dyn Write) -> i32 {
    let _ = install_signal_handlers();

    // Verify output path does not already exist (refuse overwrite).
    if fs::symlink_metadata(opts.output_path).is_ok() {
        let _ = writeln!(
            errors,
            "mo fetch: output file already exists: {}",
            opts.output_path.display()
        );
        return 1;
    }

    let (proto, fns) = match build_protocol_image() {
        Ok(v) => v,
        Err(msg) => {
            let _ = writeln!(errors, "mo fetch: {msg}");
            return 1;
        }
    };

    let mut client =
        match AdminRpcClient::new(opts.admin_socket, Duration::from_micros(MO_LIMIT_FETCH_US)) {
            Ok(c) => c,
            Err(msg) => {
                let _ = writeln!(errors, "mo fetch: {msg}");
                return 1;
            }
        };

    let mut consumer = ExportConsumer::default();
    let mut export_id_hex = String::new();
    let mut offset = 0usize;
    let page_sz = MO_LIMIT_EXPORT_PAGE_BYTES as usize;

    loop {
        let args_json = match (
            export_id_hex.is_empty(),
            opts.failure_id.filter(|&id| id > 0),
        ) {
            (true, None) => "{\"kind\":\"state\",\"offset\":0}".to_string(),
            (true, Some(fid)) => {
                format!("{{\"kind\":\"failure\",\"failure\":{fid},\"offset\":0}}")
            }
            (false, None) => {
                format!("{{\"kind\":\"state\",\"export\":\"{export_id_hex}\",\"offset\":{offset}}}")
            }
            (false, Some(fid)) => {
                format!(
                    "{{\"kind\":\"failure\",\"failure\":{fid},\"export\":\"{export_id_hex}\",\"offset\":{offset}}}"
                )
            }
        };

        let wire = match client.call_tool("mo-export", &args_json) {
            Ok(w) => w,
            Err(msg) => {
                let _ = writeln!(errors, "mo fetch: {msg}");
                return 1;
            }
        };

        match parse_fetch_page(&proto, &fns, &wire) {
            Ok(FetchPageReply::RateLimited) => {
                std::thread::sleep(Duration::from_millis(120));
                continue;
            }
            Ok(FetchPageReply::Error(reason)) => {
                let msg = match reason.as_str() {
                    "export_replaced" => "export replaced by a newer export",
                    "export_released" => "export was released or expired before completion",
                    other => other,
                };
                let _ = writeln!(errors, "mo fetch: {msg}");
                return 1;
            }
            Ok(FetchPageReply::Page(page)) => {
                if export_id_hex.is_empty() {
                    export_id_hex = crate::binary::hex_encode(&page.id);
                }
                let is_final = page.is_final;
                if !consumer.accept(&page) {
                    let _ = writeln!(errors, "mo fetch: export page verification failed");
                    return 1;
                }
                if is_final {
                    break;
                }
                offset += page_sz;
            }
            Err(msg) => {
                let _ = writeln!(errors, "mo fetch: {msg}");
                return 1;
            }
        }
    }

    if !consumer.complete {
        let _ = writeln!(errors, "mo fetch: incomplete export");
        return 1;
    }

    if let Err(msg) = write_export_atomic_0600(opts.output_path, &consumer.bytes) {
        let _ = writeln!(errors, "mo fetch: {msg}");
        return 1;
    }

    if opts.json {
        let kind_str = if consumer.kind == 2 {
            "failure"
        } else {
            "state"
        };
        let digest_hex = crate::binary::hex_encode(&consumer.bytes[consumer.total - 32..]);
        let mut line = String::new();
        line.push_str("{\"record\":\"fetch\",\"kind\":\"");
        line.push_str(kind_str);
        line.push_str("\",\"id\":\"");
        line.push_str(&export_id_hex);
        line.push_str("\",\"sequence\":\"");
        line.push_str(&consumer.sequence.to_string());
        line.push_str("\",\"encoded_sequence\":\"");
        line.push_str(&consumer.encoded_sequence.to_string());
        line.push_str("\",\"bytes\":\"");
        line.push_str(&consumer.total.to_string());
        line.push_str("\",\"sha256\":\"");
        line.push_str(&digest_hex);
        if consumer.kind == 2 {
            line.push_str("\",\"failure\":\"");
            line.push_str(&consumer.failure_id.to_string());
        }
        line.push_str("\",\"output\":");
        push_json_string(&mut line, &opts.output_path.to_string_lossy());
        line.push_str("}\n");
        let _ = output.write_all(line.as_bytes());
        let _ = output.flush();
    }
    0
}

#[cfg(unix)]
fn write_export_atomic_0600(dest: &Path, bytes: &[u8]) -> Result<(), String> {
    let parent = dest
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    let mut rnd = [0u8; 8];
    let _ = getrandom::fill(&mut rnd);
    let tmp_name = format!(".mo-fetch-{:016x}.tmp", u64::from_le_bytes(rnd));
    let tmp_path = parent.join(tmp_name);

    let prev_mask = rustix::process::umask(rustix::fs::Mode::from_bits_truncate(0o077));
    let file_res = OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .custom_flags(rustix::fs::OFlags::NOFOLLOW.bits() as i32)
        .open(&tmp_path);
    let _ = rustix::process::umask(prev_mask);

    let mut file = file_res.map_err(|e| format!("could not create temporary file: {e}"))?;
    let _ = rustix::fs::fchmod(&file, rustix::fs::Mode::from_bits_truncate(0o600));
    if let Err(e) = file.write_all(bytes).and_then(|()| file.sync_all()) {
        drop(file);
        let _ = fs::remove_file(&tmp_path);
        return Err(format!("could not write export file: {e}"));
    }
    drop(file);

    // Atomic non-overwriting publish via hard_link + unlink.
    let link_res = fs::hard_link(&tmp_path, dest);
    let _ = fs::remove_file(&tmp_path);
    link_res.map_err(|e| format!("could not publish export file: {e}"))?;
    Ok(())
}
