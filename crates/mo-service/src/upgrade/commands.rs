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
use crate::listener::signals::install_signal_handlers;
use crate::protocol::{Pf, ProtocolFunctions, build_protocol_image};
use crate::store::history::read_program_from_file;

use super::bundle::bundle_from_path;
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

    // Load old program bundle (from data dir `service.sqlite` or `.mo` source path).
    let old_sqlite = opts.old_path.join("service.sqlite");
    let (old_bundle, old_ctx, old_recorded_prog) = if old_sqlite.is_file() {
        match read_program_from_file(&old_sqlite) {
            Ok(Some(p)) => (p.bundle.clone(), Some(p.context.clone()), Some(p)),
            _ => {
                report.refuse(
                    ErrorCode::UpgradeSourceMismatch as u16,
                    "source",
                    "old data folder has no valid recorded program",
                );
                return emit_report(opts.json, opts.migration, &report, output, errors);
            }
        }
    } else {
        let mut s = SourceMap::new();
        let mut d = DiagBag::new();
        let raw = opts.old_path.as_os_str().as_encoded_bytes();
        let Some((b, _)) = bundle_from_path(raw, opts.library, false, &mut d, &mut s) else {
            report.refuse(
                ErrorCode::UpgradeBundleFormat as u16,
                "source",
                "old source could not be bundled",
            );
            return emit_report(opts.json, opts.migration, &report, output, errors);
        };
        (b, None, None)
    };

    let mut cand_sources = SourceMap::new();
    let mut cand_diags = DiagBag::new();
    let Some((cand_bundle, cand_digest)) = bundle_from_path(
        opts.candidate_path,
        opts.library,
        true,
        &mut cand_diags,
        &mut cand_sources,
    ) else {
        let mut detail = String::new();
        for d in cand_diags.items() {
            detail.push_str(&format!("error E{:04}: {}\n", d.code as u16, d.message));
        }
        report.detail = detail;
        report.refuse(
            ErrorCode::UpgradeCompileDiagnostic as u16,
            "compile",
            "candidate source failed to load",
        );
        return emit_report(opts.json, opts.migration, &report, output, errors);
    };
    report.candidate = Some(cand_digest);

    let old_img = match old_recorded_prog {
        Some(p) => {
            report.base = Some(p.source);
            p
        }
        None => {
            let Some(img) = compile_image_sync(
                &old_bundle,
                None,
                None,
                old_ctx.as_deref(),
                opts.library,
                opts.build_digest,
                0,
                &mut report,
            ) else {
                return emit_report(opts.json, opts.migration, &report, output, errors);
            };
            report.base = Some(img.program.source);
            img.program
        }
    };

    let Some(next_img) = compile_image_sync(
        &cand_bundle,
        opts.migration.map(|_| old_bundle.as_slice()),
        opts.migration,
        old_ctx.as_deref(),
        opts.library,
        opts.build_digest,
        old_img.ordinal + 1,
        &mut report,
    ) else {
        return emit_report(opts.json, opts.migration, &report, output, errors);
    };

    let old_meta = program_metadata(&old_img);
    let next_meta = program_metadata(&next_img.program);
    if compare_programs(
        &old_img,
        old_meta.as_ref(),
        &next_img.program,
        next_meta.as_ref(),
        opts.migration,
        &mut report,
    ) {
        report.already_active = old_img.source == cand_digest;
        report.message = if report.already_active {
            "candidate matches the base source".to_string()
        } else {
            "candidate is compatible with the base program".to_string()
        };
    }

    emit_report(opts.json, opts.migration, &report, output, errors)
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
        let code = diags
            .items()
            .first()
            .map_or(ErrorCode::UpgradeBundleFormat as u16, |d| d.code as u16);
        let msg = diags.items().first().map_or_else(
            || "candidate source could not be bundled".to_string(),
            |d| d.message.to_string(),
        );
        report.refuse(code, "bundle", &msg);
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
        Err(msg) => {
            let _ = writeln!(errors, "mo push: {msg}");
            return 1;
        }
    };

    let mut client =
        match AdminRpcClient::new(opts.admin_socket, Duration::from_micros(MO_LIMIT_FETCH_US)) {
            Ok(c) => c,
            Err(msg) => {
                let _ = writeln!(errors, "mo push: {msg}");
                return 1;
            }
        };

    // Resolve base digest from --base or by querying mo-overview.
    let base_hex = match opts.base_hex {
        Some(b) => b.to_string(),
        None => loop {
            let wire = match client.call_tool("mo-overview", "{}") {
                Ok(w) => w,
                Err(msg) => {
                    let _ = writeln!(errors, "mo push: {msg}");
                    return 1;
                }
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
                Err(msg) => {
                    let _ = writeln!(errors, "mo push: {msg}");
                    return 1;
                }
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
            Err(msg) => {
                let _ = writeln!(errors, "mo push: {msg}");
                return 1;
            }
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
                Err(msg) => {
                    let _ = writeln!(errors, "mo push: {msg}");
                    return 1;
                }
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
            Err(msg) => {
                let _ = writeln!(errors, "mo push: {msg}");
                return 1;
            }
        };
    emit_push_reply(opts.json, &commit_reply, output, errors)
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
