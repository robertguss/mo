//! Service program bundling, `UpgradeOld` type synthesis, child compilation, program
//! comparison, migration execution, and `mo push` / `mo check --upgrade-from` commands
//! (`src/tools/upgrade*.c` parity).

pub mod bundle;
pub mod commands;
pub mod compile;
pub mod schema;
pub mod types;

use std::fmt::Write as _;

use mo_bytecode::{BytecodeFile, load as load_bytecode};
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_LIMIT_SAVED_BYTES, MO_LIMIT_SAVED_NODES, MO_LIMIT_UPGRADE_LIVE_HEAP,
    MO_LIMIT_UPGRADE_LIVE_WORK, MO_LIMIT_UPGRADE_REPORT_BYTES, MO_LIMIT_UPGRADE_RESTART_HEAP,
    MO_LIMIT_UPGRADE_RESTART_WORK,
};
use mo_heap::Heap;
use mo_heap::saved::{decode_value_budget, encode_value_budget};
use mo_host::HostContext;
use mo_vm::{Image, Limits, Outcome, Process};

use crate::binary::compile_context::{
    CompileMetadata, read_compile_context, read_compile_metadata,
};
use crate::binary::{
    hex_decode_exact, hex_encode, push_json_string, sanitize_terminal_text, sha256_bytes,
};
use crate::store::programs::StoredProgram;

/// Confinement mode for upgrade compile children (`enum mo_upgrade_children`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UpgradeChildren {
    /// Host supports Landlock confinement; compile children run confined.
    Confined,
    /// Host cannot confine, and `--unconfined` was explicitly passed.
    Unconfined,
    /// Host cannot confine and `--unconfined` was not passed; refuse upgrades.
    Refused,
}

impl UpgradeChildren {
    /// Determines child confinement mode based on host capability and `--unconfined`.
    #[must_use]
    pub fn detect(unconfined: bool) -> Self {
        if compile::can_confine() {
            Self::Confined
        } else if unconfined {
            Self::Unconfined
        } else {
            Self::Refused
        }
    }
}

/// Compiled service image paired with its canonical `StoredProgram` metadata (`struct upgrade_image`).
pub struct UpgradeImage {
    /// Initialized VM image.
    pub image: Image,
    /// Index of the primary non-actor service in module 0.
    pub service: u32,
    /// Canonical program row metadata (source/build/schema/bundle/context).
    pub program: StoredProgram,
    /// Whether `UpgradeOld` types were synthesized from binary schema fallback.
    pub schema_synthesized: bool,
}

/// Structured upgrade acceptance/refusal report (`struct upgrade_report`).
#[derive(Debug, Clone)]
pub struct UpgradeReport {
    /// Error code (`0` = accepted, non-zero = refused `E<code>`).
    pub code: u16,
    /// Short classification string (`"accepted"`, `"source"`, `"compile"`, `"migration"`, etc.).
    pub class: String,
    /// Human-readable message.
    pub message: String,
    /// Optional detailed diagnostic output (e.g. compiler diagnostics).
    pub detail: String,
    /// Base program source digest.
    pub base: Option<[u8; 32]>,
    /// Candidate program source digest.
    pub candidate: Option<[u8; 32]>,
    /// Whether candidate was activated.
    pub activated: bool,
    /// Whether candidate was already active (`base == candidate`).
    pub already_active: bool,
    /// Active ordinal after upgrade.
    pub ordinal: u64,
    /// Service head sequence at upgrade.
    pub head: u64,
    /// VM instructions spent in migration.
    pub vm_work: u64,
    /// Native work units spent in decode/encode.
    pub native_work: u64,
    /// Peak heap usage in migration.
    pub heap_peak: usize,
    /// Encoded migrated state byte length.
    pub encoded_bytes: usize,
    /// Whether old and new descriptors were compared.
    pub compared: bool,
    /// Old state schema SHA-256 digest.
    pub old_schema: [u8; 32],
    /// New state schema SHA-256 digest.
    pub new_schema: [u8; 32],
    /// Whether implementation bytecode changed.
    pub implementation_changed: bool,
    /// JSON array of descriptor differences (`"[]"` when identical).
    pub differences_json: String,
    /// Whether this was a rehearsal.
    pub rehearsal: bool,
    /// Whether state validated during rehearsal.
    pub state_valid: bool,
    /// Whether compile children ran unconfined.
    pub unconfined: bool,
}

impl Default for UpgradeReport {
    fn default() -> Self {
        Self {
            code: 0,
            class: "accepted".to_string(),
            message: String::new(),
            detail: String::new(),
            base: None,
            candidate: None,
            activated: false,
            already_active: false,
            ordinal: 0,
            head: 0,
            vm_work: 0,
            native_work: 0,
            heap_peak: 0,
            encoded_bytes: 0,
            compared: false,
            old_schema: [0u8; 32],
            new_schema: [0u8; 32],
            implementation_changed: false,
            differences_json: "[]".to_string(),
            rehearsal: false,
            state_valid: false,
            unconfined: false,
        }
    }
}

impl UpgradeReport {
    /// Marks this report as refused with `code`, `class`, and `message`.
    pub fn refuse(&mut self, code: u16, class: &str, message: impl Into<String>) {
        self.code = code;
        self.class = class.to_string();
        let mut msg = message.into();
        if msg.len() > MO_LIMIT_UPGRADE_REPORT_BYTES as usize {
            let mut cut = MO_LIMIT_UPGRADE_REPORT_BYTES as usize;
            while cut > 0 && !msg.is_char_boundary(cut) {
                cut -= 1;
            }
            msg.truncate(cut);
        }
        self.message = msg;
    }

    /// Checks whether `children` confinement mode permits upgrades.
    pub fn check_children(&mut self, children: UpgradeChildren) -> bool {
        self.unconfined = children == UpgradeChildren::Unconfined;
        if children == UpgradeChildren::Refused {
            self.refuse(
                ErrorCode::UpgradeUnconfined as u16,
                "confinement",
                "this host cannot confine the compile children, so mo serve accepts no upgrade; start it with --unconfined to accept upgrades unconfined",
            );
            return false;
        }
        true
    }

    /// Serializes this report to exact `mo_upgrade_report_json` format.
    #[must_use]
    pub fn to_json(&self, migration: Option<&str>) -> String {
        let mut out = String::with_capacity(512);
        let status = if self.code != 0 {
            "refused"
        } else if self.already_active {
            "already_active"
        } else {
            "accepted"
        };
        let _ = write!(out, "{{\"version\":1,\"status\":\"{status}\",\"code\":");
        if self.code == 0 {
            out.push_str("null");
        } else {
            let _ = write!(out, "\"E{:04}\"", self.code);
        }
        out.push_str(",\"class\":");
        push_json_string(&mut out, &self.class);
        out.push_str(",\"message\":");
        push_json_string(&mut out, &self.message);
        out.push_str(",\"base_source\":");
        append_digest_json(&mut out, self.base);
        out.push_str(",\"candidate_source\":");
        append_digest_json(&mut out, self.candidate);
        out.push_str(",\"active_source\":");
        let active = if self.activated {
            self.candidate
        } else {
            self.base
        };
        append_digest_json(&mut out, active);
        out.push_str(",\"migration\":");
        match migration {
            Some(m) if !m.is_empty() => push_json_string(&mut out, m),
            _ => out.push_str("null"),
        }
        if self.code == 0 {
            if self.rehearsal {
                let _ = write!(out, ",\"head\":\"{}\"", self.head);
            } else {
                let _ = write!(
                    out,
                    ",\"ordinal\":\"{}\",\"head\":\"{}\"",
                    self.ordinal, self.head
                );
            }
        }
        if self.compared {
            out.push_str(",\"old_schema\":");
            append_digest_json(&mut out, Some(self.old_schema));
            out.push_str(",\"new_schema\":");
            append_digest_json(&mut out, Some(self.new_schema));
            let _ = write!(
                out,
                ",\"implementation_changed\":{},\"differences\":{}",
                self.implementation_changed, self.differences_json
            );
        }
        let _ = write!(
            out,
            ",\"vm_work\":\"{}\",\"native_work\":\"{}\",\"heap_peak\":\"{}\",\"encoded_bytes\":\"{}\"",
            self.vm_work, self.native_work, self.heap_peak, self.encoded_bytes
        );
        if self.rehearsal {
            let _ = write!(
                out,
                ",\"rehearsal\":true,\"accepted\":false,\"state_valid\":{}",
                self.state_valid
            );
        }
        if self.unconfined {
            out.push_str(",\"confined\":false");
        }
        out.push('}');
        out
    }

    /// Renders this report to human-readable terminal text (`mo_upgrade_report_text`).
    #[must_use]
    pub fn to_text(&self) -> String {
        let mut out = String::new();
        if self.code != 0 {
            let _ = write!(out, "error E{:04}: ", self.code);
        } else if self.rehearsal {
            out.push_str("rehearsal would be accepted: ");
        } else {
            out.push_str("upgrade accepted: ");
        }
        out.push_str(&sanitize_terminal_text(&self.message));
        if self.compared {
            let _ = write!(
                out,
                "\n  descriptor differences: {}\n  implementation {}",
                self.differences_json,
                if self.implementation_changed {
                    "changed"
                } else {
                    "unchanged"
                }
            );
        }
        if self.rehearsal {
            out.push_str("\n  rehearsal is not upgrade acceptance");
        }
        if self.unconfined {
            out.push_str("\n  the compile children were not confined (--unconfined)");
        }
        out.push('\n');
        out
    }
}

fn append_digest_json(out: &mut String, digest: Option<[u8; 32]>) {
    match digest {
        Some(d) => {
            out.push('"');
            out.push_str(&hex_encode(&d));
            out.push('"');
        }
        None => out.push_str("null"),
    }
}

/// Extracts `MOCS\x01` compile metadata embedded in a [`StoredProgram`]'s compile context.
#[must_use]
pub fn program_metadata(program: &StoredProgram) -> Option<CompileMetadata> {
    let ctx = read_compile_context(&program.context)?;
    let raw = ctx.metadata.as_deref()?;
    read_compile_metadata(raw)
}

/// Finds the primary (non-actor) service in module 0 of `file`.
#[must_use]
pub fn find_entry_service(file: &BytecodeFile) -> Option<u32> {
    for (i, svc) in file.services.iter().enumerate() {
        let tok = file.string(svc.token_env);
        if svc.module == 0 && tok != "_MO_ACTOR" {
            return Some(i as u32);
        }
    }
    None
}

/// Compares old and candidate programs for upgrade compatibility (`mo_upgrade_compare_programs`).
///
/// Enforces:
/// - token env variable matches (`E1426` if changed),
/// - state schema matches unless a migration function is given (`E1420` if schema changed without
///   migration; `E1421` if migration given when schema unchanged — wait: check C rule!),
/// - records descriptor differences JSON array.
pub fn compare_programs(
    old: &StoredProgram,
    old_meta: Option<&CompileMetadata>,
    next: &StoredProgram,
    next_meta: Option<&CompileMetadata>,
    migration: Option<&str>,
    report: &mut UpgradeReport,
) -> bool {
    report.base = Some(old.source);
    report.candidate = Some(next.source);
    report.compared = true;
    report.old_schema = old.schema;
    report.new_schema = next.schema;
    report.implementation_changed = old.image != next.image;

    let Some(om) = old_meta else {
        report.refuse(
            ErrorCode::UpgradeInvalidValue as u16,
            "metadata",
            "recorded program metadata is missing",
        );
        return false;
    };
    let Some(nm) = next_meta else {
        report.refuse(
            ErrorCode::UpgradeInvalidValue as u16,
            "metadata",
            "candidate program metadata is missing",
        );
        return false;
    };

    // Emit descriptor differences in sorted merge order matching `src/tools/upgrade.c:378-433`:
    // tools first, then timers. Token and state schema changes are NOT placed in `differences`.
    let mut diffs: Vec<String> = Vec::new();
    let mut old_tools: Vec<_> = om.tools.iter().collect();
    let mut new_tools: Vec<_> = nm.tools.iter().collect();
    old_tools.sort_by(|a, b| a.name.cmp(&b.name));
    new_tools.sort_by(|a, b| a.name.cmp(&b.name));
    let (mut oi, mut ni) = (0usize, 0usize);
    while oi < old_tools.len() || ni < new_tools.len() {
        let cmp = match (old_tools.get(oi), new_tools.get(ni)) {
            (Some(o), Some(n)) => o.name.cmp(&n.name),
            (Some(_), None) => std::cmp::Ordering::Less,
            (None, Some(_)) => std::cmp::Ordering::Greater,
            (None, None) => break,
        };
        match cmp {
            std::cmp::Ordering::Less => {
                diffs.push(format!(
                    "{{\"kind\":\"tool\",\"name\":{},\"change\":\"removed\"}}",
                    json_quoted(&old_tools[oi].name)
                ));
                oi += 1;
            }
            std::cmp::Ordering::Greater => {
                diffs.push(format!(
                    "{{\"kind\":\"tool\",\"name\":{},\"change\":\"added\"}}",
                    json_quoted(&new_tools[ni].name)
                ));
                ni += 1;
            }
            std::cmp::Ordering::Equal => {
                if old_tools[oi].digest != new_tools[ni].digest {
                    diffs.push(format!(
                        "{{\"kind\":\"tool\",\"name\":{},\"change\":\"schema_or_description_changed\"}}",
                        json_quoted(&old_tools[oi].name)
                    ));
                }
                oi += 1;
                ni += 1;
            }
        }
    }

    let mut old_timers: Vec<_> = om.timers.iter().collect();
    let mut new_timers: Vec<_> = nm.timers.iter().collect();
    old_timers.sort_by(|a, b| a.name.cmp(&b.name));
    new_timers.sort_by(|a, b| a.name.cmp(&b.name));
    let (mut oi, mut ni) = (0usize, 0usize);
    while oi < old_timers.len() || ni < new_timers.len() {
        let cmp = match (old_timers.get(oi), new_timers.get(ni)) {
            (Some(o), Some(n)) => o.name.cmp(&n.name),
            (Some(_), None) => std::cmp::Ordering::Less,
            (None, Some(_)) => std::cmp::Ordering::Greater,
            (None, None) => break,
        };
        match cmp {
            std::cmp::Ordering::Less => {
                diffs.push(format!(
                    "{{\"kind\":\"timer\",\"name\":{},\"change\":\"removed\"}}",
                    json_quoted(&old_timers[oi].name)
                ));
                oi += 1;
            }
            std::cmp::Ordering::Greater => {
                diffs.push(format!(
                    "{{\"kind\":\"timer\",\"name\":{},\"change\":\"added\"}}",
                    json_quoted(&new_timers[ni].name)
                ));
                ni += 1;
            }
            std::cmp::Ordering::Equal => {
                if old_timers[oi].interval != new_timers[ni].interval {
                    diffs.push(format!(
                        "{{\"kind\":\"timer\",\"name\":{},\"change\":\"interval_changed\"}}",
                        json_quoted(&old_timers[oi].name)
                    ));
                }
                oi += 1;
                ni += 1;
            }
        }
    }
    report.differences_json = format!("[{}]", diffs.join(","));

    if om.token != nm.token {
        report.refuse(
            ErrorCode::UpgradeChangedAccess as u16,
            "access",
            "service bearer token environment variable changed; access cannot change in a live upgrade",
        );
        return false;
    }

    let has_migration = migration.is_some_and(|m| !m.is_empty());
    if old.schema != next.schema && !has_migration {
        report.refuse(
            ErrorCode::UpgradeMissingMigration as u16,
            "schema",
            format!(
                "state type changed from {} to {}; supply --migrate with a pure migration function",
                hex_encode(&old.schema),
                hex_encode(&next.schema)
            ),
        );
        return false;
    }
    true
}

fn json_quoted(s: &str) -> String {
    let mut out = String::new();
    push_json_string(&mut out, s);
    out
}

/// Runs a candidate migration function (or verifies state compatibility when `migration` is `None`)
/// against saved state bytes (`src/tools/upgrade.c:mo_upgrade_migration_state`).
///
/// `live_or_rehearsal` selects the tighter live/rehearsal budget (`MO_LIMIT_UPGRADE_LIVE_HEAP` /
/// `MO_LIMIT_UPGRADE_LIVE_WORK`) vs checked restart budget (`MO_LIMIT_UPGRADE_RESTART_HEAP` /
/// `MO_LIMIT_UPGRADE_RESTART_WORK`).
#[expect(
    clippy::too_many_arguments,
    reason = "migration execution needs candidate image, service, migration name, old state metadata, live flag, and report"
)]
pub fn run_migration_state(
    next_image: &Image,
    next_service_idx: u32,
    migration: Option<&str>,
    old_state_type_name: Option<&str>,
    old_state_bytes: &[u8],
    live_or_rehearsal: bool,
    schema_synthesized: bool,
    report: &mut UpgradeReport,
) -> Option<Vec<u8>> {
    let file = next_image.file();
    let svc = file.services.get(next_service_idx as usize)?;
    let new_state_ty = svc.state_type;

    let (heap_limit, work_budget) = if live_or_rehearsal {
        (
            MO_LIMIT_UPGRADE_LIVE_HEAP as usize,
            MO_LIMIT_UPGRADE_LIVE_WORK,
        )
    } else {
        (
            MO_LIMIT_UPGRADE_RESTART_HEAP as usize,
            MO_LIMIT_UPGRADE_RESTART_WORK,
        )
    };
    let mut remaining = work_budget;

    let Some(mig_name) = migration.filter(|m| !m.is_empty()) else {
        // No migration: decode directly under candidate state type and check timer intervals.
        let heap = Heap::new(heap_limit);
        let max_nodes = remaining.min(MO_LIMIT_SAVED_NODES) as u32;
        match decode_value_budget(
            &heap,
            &file.types,
            new_state_ty,
            old_state_bytes,
            None,
            MO_LIMIT_SAVED_BYTES as usize,
            max_nodes,
        ) {
            Ok((_v, nodes)) => {
                report.native_work = u64::from(nodes);
                remaining = remaining.saturating_sub(u64::from(nodes));
                report.heap_peak = heap.budget().peak();
                report.encoded_bytes = old_state_bytes.len();
            }
            Err(_) => {
                report.refuse(
                    ErrorCode::UpgradeInvalidValue as u16,
                    "state",
                    "saved state does not match the candidate state schema",
                );
                return None;
            }
        }
        if !validate_timer_intervals(
            next_image,
            next_service_idx,
            heap_limit,
            &mut remaining,
            report,
        ) {
            return None;
        }
        return Some(old_state_bytes.to_vec());
    };

    let inferred_root;
    let effective_root = match old_state_type_name {
        Some(r) => Some(r),
        None => {
            inferred_root = schema::schema_root_name(old_state_bytes);
            inferred_root.as_deref()
        }
    };
    let owner_prefix: Option<&str> = effective_root.and_then(|root| {
        let head = root.split('(').next().unwrap_or(root);
        head.rfind('.').map(|idx| &head[..idx])
    });

    // Rewrite `UpgradeOld.` nominal type names back to the original persisted `MOSV\x01` schema
    // names (exact parity with `src/tools/upgrade.c:550-595`, including `Saved_<hex>` un-hexing).
    let stripped_names: Vec<Option<String>> = file
        .types
        .items
        .iter()
        .map(|t| {
            t.name
                .as_deref()
                .filter(|n| n.contains("UpgradeOld."))
                .map(|n| rewrite_upgrade_old_name(n, owner_prefix, schema_synthesized))
        })
        .collect();
    let overrides: Vec<Option<&str>> = stripped_names.iter().map(|o| o.as_deref()).collect();

    let limits = Limits {
        heap_size: heap_limit,
        ..Limits::default()
    };
    let mut proc = Process::new(
        next_image,
        limits,
        HostContext::stdio(Vec::new(), Vec::new()),
    );

    // Locate the matching 1-argument migration function (`src/tools/upgrade.c:599-622`).
    let qualified = mig_name.contains('.');
    let mut found_sig = false;
    let mut selected: Option<(u32, mo_heap::Value, u32)> = None;
    let max_decode_nodes = remaining.min(MO_LIMIT_SAVED_NODES) as u32;
    for (idx, func) in file.functions.iter().enumerate() {
        let full = file.string(func.name);
        let head = full.split('@').next().unwrap_or(full);
        let candidate_name = if qualified {
            head
        } else {
            head.rsplit('.').next().unwrap_or(head)
        };
        if func.parameters.len() != 1 || candidate_name != mig_name {
            continue;
        }
        if func.result_type != new_state_ty {
            continue;
        }
        found_sig = true;
        if let Ok((val, nodes)) = decode_value_budget(
            proc.heap(),
            &file.types,
            func.parameters[0].ty,
            old_state_bytes,
            Some(&overrides),
            MO_LIMIT_SAVED_BYTES as usize,
            max_decode_nodes,
        ) {
            selected = Some((idx as u32, val, nodes));
            break;
        }
    }

    let Some((mig_fn_idx, old_val, decode_nodes)) = selected else {
        if !found_sig {
            report.refuse(
                ErrorCode::UpgradeMigrationSignature as u16,
                "migration",
                format!(
                    "migration function `{mig_name}` was not found or has an incompatible signature"
                ),
            );
        } else {
            report.refuse(
                ErrorCode::UpgradeInvalidValue as u16,
                "state",
                "saved state could not be decoded for the migration function",
            );
        }
        return None;
    };

    report.native_work = u64::from(decode_nodes);
    remaining = remaining.saturating_sub(u64::from(decode_nodes));
    if remaining == 0 {
        report.heap_peak = proc.memory_peak();
        report.refuse(
            ErrorCode::UpgradeMigrationBudget as u16,
            "budget",
            "migration exceeded its work budget",
        );
        return None;
    }

    let (outcome, work) = proc.call_with_budget(mig_fn_idx, vec![old_val], remaining);
    report.vm_work = work;
    remaining = remaining.saturating_sub(work);
    report.heap_peak = proc.memory_peak();

    let new_val = match outcome {
        Outcome::Returned(v) => v,
        Outcome::Failed(d) => {
            report.refuse(
                ErrorCode::UpgradeMigrationTrap as u16,
                "migration",
                format!("migration failed: {}", d.reason),
            );
            return None;
        }
        Outcome::Stopped(d) => {
            let is_budget = remaining == 0
                || d.reason.contains("instruction budget")
                || d.reason.contains("out of work")
                || d.limit_name.is_some()
                || d.reason.contains("out of memory");
            let (code, class) = if is_budget {
                (ErrorCode::UpgradeMigrationBudget as u16, "budget")
            } else {
                (ErrorCode::UpgradeMigrationTrap as u16, "migration")
            };
            report.refuse(code, class, format!("migration stopped: {}", d.reason));
            return None;
        }
    };

    let max_encode_nodes = remaining.min(MO_LIMIT_SAVED_NODES) as u32;
    let mut out = Vec::new();
    match encode_value_budget(
        &file.types,
        new_state_ty,
        &new_val,
        MO_LIMIT_SAVED_BYTES as usize,
        max_encode_nodes,
        &mut out,
    ) {
        Ok(enc_nodes) => {
            report.native_work = report.native_work.saturating_add(u64::from(enc_nodes));
            remaining = remaining.saturating_sub(u64::from(enc_nodes));
            report.encoded_bytes = out.len();
        }
        Err(_) => {
            report.refuse(
                ErrorCode::UpgradeMigrationBudget as u16,
                "budget",
                "migrated state exceeded MO_LIMIT_SAVED_BYTES or migration work budget",
            );
            return None;
        }
    }

    // Validate the encoded migrated state round-trips cleanly within the remaining work budget.
    let val_heap = Heap::new(heap_limit);
    let max_val_nodes = remaining.min(MO_LIMIT_SAVED_NODES) as u32;
    match decode_value_budget(
        &val_heap,
        &file.types,
        new_state_ty,
        &out,
        None,
        MO_LIMIT_SAVED_BYTES as usize,
        max_val_nodes,
    ) {
        Ok((_v, val_nodes)) => {
            report.native_work = report.native_work.saturating_add(u64::from(val_nodes));
            remaining = remaining.saturating_sub(u64::from(val_nodes));
            report.heap_peak = report.heap_peak.max(val_heap.budget().peak());
        }
        Err(_) => {
            report.refuse(
                ErrorCode::UpgradeMigrationBudget as u16,
                "budget",
                "migrated state validation failed or exceeded its work budget",
            );
            return None;
        }
    }

    if !validate_timer_intervals(
        next_image,
        next_service_idx,
        heap_limit,
        &mut remaining,
        report,
    ) {
        return None;
    }

    Some(out)
}

fn validate_timer_intervals(
    next_image: &Image,
    next_service_idx: u32,
    heap_limit: usize,
    remaining: &mut u64,
    report: &mut UpgradeReport,
) -> bool {
    let svc = &next_image.file().services[next_service_idx as usize];
    for t in &svc.timers {
        if *remaining == 0 {
            report.refuse(
                ErrorCode::UpgradeMigrationBudget as u16,
                "budget",
                "candidate timer interval check exceeded the migration work budget",
            );
            return false;
        }
        let limits = Limits {
            heap_size: heap_limit,
            ..Limits::default()
        };
        let mut proc = Process::new(
            next_image,
            limits,
            HostContext::stdio(Vec::new(), Vec::new()),
        );
        let (outcome, work) = proc.call_with_budget(t.interval, Vec::new(), *remaining);
        report.vm_work = report.vm_work.saturating_add(work);
        *remaining = remaining.saturating_sub(work);
        report.heap_peak = report.heap_peak.max(proc.memory_peak());
        match outcome {
            Outcome::Returned(v) if v.as_duration() > 0 => {}
            Outcome::Returned(_) => {
                report.refuse(
                    ErrorCode::UpgradeTimerInterval as u16,
                    "timer",
                    "candidate timer interval must be positive",
                );
                return false;
            }
            Outcome::Failed(d) | Outcome::Stopped(d) => {
                if *remaining == 0 {
                    report.refuse(
                        ErrorCode::UpgradeMigrationBudget as u16,
                        "budget",
                        format!("timer interval exceeded budget: {}", d.reason),
                    );
                } else {
                    report.refuse(
                        ErrorCode::UpgradeTimerInterval as u16,
                        "timer",
                        format!("candidate timer interval failed: {}", d.reason),
                    );
                }
                return false;
            }
        }
    }
    true
}

/// Rewrites `UpgradeOld.` module prefixes in synthesized nominal type names back to the
/// original persisted `MOSV\x01` schema names (`src/tools/upgrade.c:556-588`).
///
/// - `UpgradeOld.State` (no dot after `UpgradeOld.`) -> `<owner_prefix>.State`
/// - `UpgradeOld.Sub.Item` (has dot after `UpgradeOld.`) -> `Sub.Item`
/// - When `schema_synthesized` is true, `Saved_<hex>` tokens are decoded back to UTF-8.
fn rewrite_upgrade_old_name(
    name: &str,
    owner_prefix: Option<&str>,
    schema_synthesized: bool,
) -> String {
    let mut out = String::with_capacity(name.len() + 16);
    let bytes = name.as_bytes();
    let mut at = 0usize;
    while at < bytes.len() {
        let b = bytes[at];
        if matches!(b, b'(' | b')' | b',' | b' ') {
            out.push(b as char);
            at += 1;
            continue;
        }
        let start = at;
        while at < bytes.len() && !matches!(bytes[at], b'(' | b')' | b',' | b' ') {
            at += 1;
        }
        let token = &name[start..at];
        if let Some(after) = token.strip_prefix("UpgradeOld.") {
            if schema_synthesized
                && let Some(hex_part) = after
                    .rsplit('.')
                    .next()
                    .and_then(|s| s.strip_prefix("Saved_"))
                && let Some(decoded) = decode_hex_utf8(hex_part)
            {
                out.push_str(&decoded);
                continue;
            }
            if !after.contains('.')
                && let Some(owner) = owner_prefix
                && !owner.is_empty()
            {
                out.push_str(owner);
                out.push('.');
            }
            out.push_str(after);
        } else {
            out.push_str(token);
        }
    }
    out
}

fn decode_hex_utf8(hex: &str) -> Option<String> {
    if hex.is_empty() || !hex.len().is_multiple_of(2) {
        return None;
    }
    let mut bytes = Vec::with_capacity(hex.len() / 2);
    for pair in hex.as_bytes().chunks_exact(2) {
        let s = std::str::from_utf8(pair).ok()?;
        let one = hex_decode_exact::<1>(s)?;
        bytes.push(one[0]);
    }
    String::from_utf8(bytes).ok()
}

/// Loads bytecode bytes and initializes an [`Image`] bounded by restart budgets.
pub fn load_service_image(wire: &[u8], limits: Limits) -> Result<(Image, u32), String> {
    let bc = load_bytecode(wire).map_err(|e| format!("bytecode load failed: {e}"))?;
    let service = find_entry_service(&bc)
        .ok_or_else(|| "the entry module declares no service".to_string())?;
    let image = Image::init(bc, limits)
        .map_err(|b| format!("service initialization failed: {}", b.1.reason))?;
    Ok((image, service))
}

/// Computes the 32-byte SHA-256 digest of `data` (convenience wrapper).
#[must_use]
pub fn digest_bytes(data: &[u8]) -> [u8; 32] {
    sha256_bytes(data)
}
