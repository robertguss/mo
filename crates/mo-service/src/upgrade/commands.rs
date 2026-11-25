//! CLI upgrade commands: `mo check --upgrade-from` and `mo push`
//! (`src/tools/upgrade-commands.c` and `src/tools/push.c` parity).

use std::io::Write;
use std::path::Path;
use std::time::Duration;

use mo_base::{DiagBag, SourceMap};
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_LIMIT_BUNDLE_BYTES, MO_LIMIT_EXPORT_SCRATCH, MO_LIMIT_FETCH_US, MO_LIMIT_MCP_REQUEST_WORK,
    MO_LIMIT_UPGRADE_PAGE_BYTES,
};
use mo_host::HostContext;
use mo_vm::{Image, Limits, Outcome, Process};

use crate::binary::{hex_encode, push_json_string};
use crate::fetch::AdminRpcClient;
use crate::history::folder_error_message;
use crate::listener::signals::install_signal_handlers;
use crate::protocol::{Pf, ProtocolFunctions, build_protocol_image};
use crate::store::history::{FolderStatus, read_program_from_file};

use super::bundle::{bundle_from_path, bundle_from_path_opts};
use super::compile::compile_image_sync;
use super::{UpgradeReport, compare_programs, program_metadata};

/// Options for `mo check CANDIDATE --upgrade-from OLD [--migrate M] [--json]`.
pub struct UpgradeCheckOptions<'a> {
    /// Candidate entry `.mo` file path.
    pub candidate_path: &'a [u8],
    /// Old entry `.mo` file path or data folder containing `service.sqlite`.
    pub old_path: &'a Path,
    /// Optional migration function name.
    pub migration: Option<&'a str>,
    /// Optional explicit library directory.
    pub library: Option<&'a Path>,
    /// Emit JSON report on stdout.
    pub json: bool,
    /// 32-byte build runtime identity digest.
    pub build_digest: [u8; 32],
}

/// Executes `mo check CANDIDATE --upgrade-from OLD [--migrate M] [--json]`.
pub fn run_upgrade_check(
    opts: &UpgradeCheckOptions<'_>,
    output: &mut dyn Write,
    errors: &mut dyn Write,
) -> i32 {
    let mut report = UpgradeReport::default();

    // Distinguish data folder vs `.mo` source path using `is_dir()` (`src/tools/upgrade-commands.c:82`).
    let is_folder = std::fs::metadata(opts.old_path).is_ok_and(|m| m.is_dir());
    let (old_bundle, old_ctx) = if is_folder {
        let old_sqlite = opts.old_path.join("service.sqlite");
        let folder_str = opts.old_path.to_string_lossy();
        match read_program_from_file(&old_sqlite) {
            Ok(Some(p)) => (p.bundle, Some(p.context)),
            Ok(None) => {
                report.refuse(
                    ErrorCode::UpgradeSourceMismatch as u16,
                    "store",
                    "this folder has no recorded program",
                );
                return emit_report(opts.json, opts.migration, &report, output, errors);
            }
            Err(status) => {
                let code = if status == FolderStatus::Owned {
                    ErrorCode::UpgradeBusy as u16
                } else {
                    ErrorCode::UpgradeInvalidValue as u16
                };
                report.refuse(code, "store", folder_error_message(&folder_str, status, 0));
                return emit_report(opts.json, opts.migration, &report, output, errors);
            }
        }
    } else {
        let mut s = SourceMap::new();
        let mut d = DiagBag::new();
        let raw = opts.old_path.as_os_str().as_encoded_bytes();
        let Some((b, _)) = bundle_from_path_opts(raw, opts.library, false, false, &mut d, &mut s)
        else {
            refuse_from_bundle_diags(&d, "source bundling failed", &mut report);
            return emit_report(opts.json, opts.migration, &report, output, errors);
        };
        (b, None)
    };

    let mut cand_sources = SourceMap::new();
    let mut cand_diags = DiagBag::new();
    let Some((cand_bundle, cand_digest)) = bundle_from_path_opts(
        opts.candidate_path,
        opts.library,
        true,
        false,
        &mut cand_diags,
        &mut cand_sources,
    ) else {
        refuse_from_bundle_diags(&cand_diags, "source bundling failed", &mut report);
        return emit_report(opts.json, opts.migration, &report, output, errors);
    };

    report.base = Some(crate::binary::sha256_bytes(&old_bundle));
    report.candidate = Some(cand_digest);

    let Some(old_img) = compile_image_sync(
        &old_bundle,
        None,
        None,
        if is_folder { old_ctx.as_deref() } else { None },
        opts.library,
        opts.build_digest,
        0,
        &mut report,
    ) else {
        return emit_report(opts.json, opts.migration, &report, output, errors);
    };

    // Pass `&old_bundle` unless `folder && migration.is_none()` (`src/tools/upgrade-commands.c:105`)
    // so `use UpgradeOld` in candidate compiles cleanly and `compare_programs` emits `E1421` when
    // `--migrate` was omitted.
    let next_old = if is_folder && opts.migration.is_none() {
        None
    } else {
        Some(old_bundle.as_slice())
    };
    let Some(next_img) = compile_image_sync(
        &cand_bundle,
        next_old,
        opts.migration,
        if is_folder { old_ctx.as_deref() } else { None },
        opts.library,
        opts.build_digest,
        old_img.program.ordinal + 1,
        &mut report,
    ) else {
        return emit_report(opts.json, opts.migration, &report, output, errors);
    };

    let old_meta = program_metadata(&old_img.program);
    let next_meta = program_metadata(&next_img.program);
    if compare_programs(
        &old_img.program,
        old_meta.as_ref(),
        &next_img.program,
        next_meta.as_ref(),
        opts.migration,
        &mut report,
    ) {
        report.message = "candidate is compatible with the base program".to_string();
    }

    emit_report(opts.json, opts.migration, &report, output, errors)
}

fn refuse_from_bundle_diags(diags: &DiagBag, fallback: &str, report: &mut UpgradeReport) {
    let first = diags.items().first();
    let code = first
        .map(|d| d.code as u16)
        .filter(|&c| (1414..=1431).contains(&c))
        .unwrap_or(ErrorCode::UpgradeCompileDiagnostic as u16);
    let msg = first
        .map(|d| d.message.to_string())
        .unwrap_or_else(|| fallback.to_string());
    report.refuse(code, "compile", msg);
}

fn emit_report(
    json: bool,
    migration: Option<&str>,
    report: &UpgradeReport,
    output: &mut dyn Write,
    errors: &mut dyn Write,
) -> i32 {
    if json {
        let mut s = report.to_json(migration);
        s.push('\n');
        let _ = output.write_all(s.as_bytes());
        let _ = output.flush();
    } else if report.code == 0 {
        let _ = output.write_all(report.to_text().as_bytes());
        let _ = output.flush();
    } else {
        let _ = errors.write_all(report.to_text().as_bytes());
        let _ = errors.flush();
    }
    if report.code == 0 { 0 } else { 1 }
}

/// Options for `mo push FILE --admin-socket PATH [--base DIGEST] [--migrate M] [--json]`.
pub struct PushOptions<'a> {
    /// Candidate entry `.mo` file path.
    pub candidate_path: &'a [u8],
    /// Admin Unix domain socket path.
    pub admin_socket: &'a Path,
    /// Optional expected base 64-hex source digest.
    pub base_hex: Option<&'a str>,
    /// Optional migration function name.
    pub migration: Option<&'a str>,
    /// Optional explicit library directory.
    pub library: Option<&'a Path>,
    /// Emit JSON report on stdout.
    pub json: bool,
}

struct PushReply {
    failed: bool,
    rate_limited: bool,
    text: String,
    json: String,
    slot: String,
}

fn parse_upgrade_reply(
    proto: &Image,
    fns: &ProtocolFunctions,
    wire: &[u8],
) -> Result<PushReply, String> {
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
    let (outcome, _) = proc.call_with_budget(
        fns.get(Pf::FetchUpgrade),
        vec![arg],
        MO_LIMIT_MCP_REQUEST_WORK,
    );
    let list_val = match outcome {
        Outcome::Returned(v) => v,
        Outcome::Failed(d) | Outcome::Stopped(d) => {
            return Err(format!("McpFetch.upgrade failed: {}", d.reason));
        }
    };
    let items: Vec<String> = list_val
        .as_list()
        .iter()
        .map(|v| v.as_text().to_string())
        .collect();
    if items.len() == 1 && items[0] == "rate_limited" {
        return Ok(PushReply {
            failed: false,
            rate_limited: true,
            text: String::new(),
            json: String::new(),
            slot: String::new(),
        });
    }
    if items.len() != 4 {
        return Err("malformed upgrade RPC reply".to_string());
    }
    Ok(PushReply {
        failed: items[0] == "true",
        rate_limited: false,
        text: items[1].clone(),
        json: items[2].clone(),
        slot: items[3].clone(),
    })
}

fn parse_overview_source(
    proto: &Image,
    fns: &ProtocolFunctions,
    wire: &[u8],
) -> Result<Option<String>, String> {
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
    let (outcome, _) = proc.call_with_budget(
        fns.get(Pf::FetchOverviewSource),
        vec![arg],
        MO_LIMIT_MCP_REQUEST_WORK,
    );
    let val = match outcome {
        Outcome::Returned(v) => v,
        Outcome::Failed(d) | Outcome::Stopped(d) => {
            return Err(format!("McpFetch.overview_source failed: {}", d.reason));
        }
    };
    let s = val.as_text();
    if s == "rate_limited" {
        return Err("rate_limited".to_string());
    }
    if s == "none" {
        return Ok(None);
    }
    if s.len() == 64 {
        return Ok(Some(s.to_string()));
    }
    Err("invalid overview program_source".to_string())
}

/// Executes `mo push` against a running service's admin socket (`src/tools/push.c:mo_push`).
#[cfg(unix)]
pub fn run_push(opts: &PushOptions<'_>, output: &mut dyn Write, errors: &mut dyn Write) -> i32 {
    let _ = install_signal_handlers();

    if let Some(base) = opts.base_hex
        && (base.len() != 64
            || !base
                .bytes()
                .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)))
    {
        let _ = writeln!(errors, "mo push: --base must be 64 lowercase hex digits");
        let _ = errors.flush();
        return 2;
    }

    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();
    let Some((bundle, cand_digest)) = bundle_from_path(
        opts.candidate_path,
        opts.library,
        true,
        &mut diags,
        &mut sources,
    ) else {
        let mut report = UpgradeReport::default();
        refuse_from_bundle_diags(&diags, "candidate source could not be bundled", &mut report);
        return emit_report(opts.json, opts.migration, &report, output, errors);
    };

    if bundle.len() as u64 > MO_LIMIT_BUNDLE_BYTES {
        let mut report = UpgradeReport {
            candidate: Some(cand_digest),
            ..UpgradeReport::default()
        };
        report.refuse(
            ErrorCode::UpgradeBundleSize as u16,
            "bundle",
            "source bundle exceeds MO_LIMIT_BUNDLE_BYTES",
        );
        return emit_report(opts.json, opts.migration, &report, output, errors);
    }

    let (proto, fns) = match build_protocol_image() {
        Ok(v) => v,
        Err(msg) => return push_failure(opts.json, &msg, output, errors),
    };

    let mut client =
        match AdminRpcClient::new(opts.admin_socket, Duration::from_micros(MO_LIMIT_FETCH_US)) {
            Ok(c) => c,
            Err(msg) => return push_failure(opts.json, &msg, output, errors),
        };

    const BEFORE_COMMIT: &str =
        "the service did not answer or refused the request before the commit; nothing was changed";
    const AFTER_COMMIT: &str =
        "the service did not answer after the commit request; check mo-overview before retrying";

    // Resolve base digest from --base or by querying mo-overview.
    let base_hex = match opts.base_hex {
        Some(b) => b.to_string(),
        None => loop {
            let wire = match client.call_tool("mo-overview", "{}") {
                Ok(w) => w,
                Err(_) => return push_failure(opts.json, BEFORE_COMMIT, output, errors),
            };
            match parse_overview_source(&proto, &fns, &wire) {
                Ok(Some(src)) => break src,
                Ok(None) => {
                    let mut report = UpgradeReport {
                        candidate: Some(cand_digest),
                        ..UpgradeReport::default()
                    };
                    report.refuse(
                        ErrorCode::UpgradeSourceMismatch as u16,
                        "source",
                        "running service has no recorded base source",
                    );
                    return emit_report(opts.json, opts.migration, &report, output, errors);
                }
                Err(e) if e == "rate_limited" => {
                    std::thread::sleep(Duration::from_millis(120));
                    continue;
                }
                Err(_) => return push_failure(opts.json, BEFORE_COMMIT, output, errors),
            }
        },
    };

    let cand_hex = hex_encode(&cand_digest);
    let mut open_args = format!(
        "{{\"base\":\"{base_hex}\",\"candidate\":\"{cand_hex}\",\"total\":{}",
        bundle.len()
    );
    if let Some(m) = opts.migration.filter(|s| !s.is_empty()) {
        open_args.push_str(",\"migration\":");
        push_json_string(&mut open_args, m);
    }
    open_args.push('}');

    let open_reply =
        match call_upgrade_tool(&mut client, &proto, &fns, "mo-upgrade-open", &open_args) {
            Ok(r) => r,
            Err(_) => return push_failure(opts.json, BEFORE_COMMIT, output, errors),
        };
    if open_reply.failed {
        return emit_push_reply(opts.json, &open_reply, output, errors);
    }
    if open_reply.slot.is_empty() {
        // Already active!
        return emit_push_reply(opts.json, &open_reply, output, errors);
    }
    let slot = open_reply.slot;

    // Upload pages (base64 encoded, matching src/tools/push.c:mo_upload_text).
    let page_sz = MO_LIMIT_UPGRADE_PAGE_BYTES as usize;
    let total_len = bundle.len();
    for (idx, chunk) in bundle.chunks(page_sz).enumerate() {
        let offset = idx * page_sz;
        let b64_chunk = encode_base64(chunk);
        let page_args = format!(
            "{{\"slot\":\"{slot}\",\"index\":{idx},\"offset\":{offset},\"total\":{total_len},\"bytes\":\"{b64_chunk}\"}}"
        );
        let page_reply =
            match call_upgrade_tool(&mut client, &proto, &fns, "mo-upgrade-page", &page_args) {
                Ok(r) => r,
                Err(_) => return push_failure(opts.json, BEFORE_COMMIT, output, errors),
            };
        if page_reply.failed {
            return emit_push_reply(opts.json, &page_reply, output, errors);
        }
    }

    // Commit!
    let commit_args = format!("{{\"slot\":\"{slot}\",\"base\":\"{base_hex}\"}}");
    let commit_reply =
        match call_upgrade_tool(&mut client, &proto, &fns, "mo-upgrade-commit", &commit_args) {
            Ok(r) => r,
            Err(_) => return push_failure(opts.json, AFTER_COMMIT, output, errors),
        };
    emit_push_reply(opts.json, &commit_reply, output, errors)
}

fn push_failure(json: bool, message: &str, output: &mut dyn Write, errors: &mut dyn Write) -> i32 {
    if json {
        let mut line = String::from(
            "{\"version\":1,\"status\":\"refused\",\"code\":null,\"class\":\"client\",\"message\":",
        );
        push_json_string(&mut line, message);
        line.push_str("}\n");
        let _ = output.write_all(line.as_bytes());
        let _ = output.flush();
    } else {
        let _ = writeln!(errors, "mo push: {message}");
        let _ = errors.flush();
    }
    1
}

#[cfg(unix)]
fn call_upgrade_tool(
    client: &mut AdminRpcClient,
    proto: &Image,
    fns: &ProtocolFunctions,
    tool: &str,
    args_json: &str,
) -> Result<PushReply, String> {
    loop {
        let wire = client.call_tool(tool, args_json)?;
        let reply = parse_upgrade_reply(proto, fns, &wire)?;
        if reply.rate_limited {
            std::thread::sleep(Duration::from_millis(120));
            continue;
        }
        return Ok(reply);
    }
}

fn encode_base64(input: &[u8]) -> String {
    const TABLE: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity(input.len().div_ceil(3) * 4);
    for chunk in input.chunks(3) {
        let b0 = u32::from(chunk[0]);
        let b1 = chunk.get(1).copied().map_or(0u32, u32::from);
        let b2 = chunk.get(2).copied().map_or(0u32, u32::from);
        let triple = (b0 << 16) | (b1 << 8) | b2;
        out.push(TABLE[((triple >> 18) & 0x3f) as usize] as char);
        out.push(TABLE[((triple >> 12) & 0x3f) as usize] as char);
        if chunk.len() > 1 {
            out.push(TABLE[((triple >> 6) & 0x3f) as usize] as char);
        } else {
            out.push('=');
        }
        if chunk.len() > 2 {
            out.push(TABLE[(triple & 0x3f) as usize] as char);
        } else {
            out.push('=');
        }
    }
    out
}

fn emit_push_reply(
    json: bool,
    reply: &PushReply,
    output: &mut dyn Write,
    errors: &mut dyn Write,
) -> i32 {
    if json {
        let _ = writeln!(output, "{}", reply.json);
        let _ = output.flush();
    } else if reply.failed {
        let _ = writeln!(errors, "{}", reply.text);
        let _ = errors.flush();
    } else {
        let _ = writeln!(output, "{}", reply.text);
        let _ = output.flush();
    }
    if reply.failed { 1 } else { 0 }
}
