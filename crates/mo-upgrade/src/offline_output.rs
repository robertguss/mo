//! Verdict classification and offline NDJSON/text formatting (`src/base/verdict.c` and
//! `src/tools/offline-output.c` parity).

use std::fmt::Write as _;

use mo_service::binary::failure::{FailureContextStatus, FailureRecord};
use mo_service::binary::{push_json_string, sanitize_terminal_text};
use mo_service::history::FailureSignature;

/// Outcome status of one offline handler execution (`enum mo_outcome_status`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OutcomeStatus {
    /// Handler returned cleanly and any updated state saved.
    Succeeded,
    /// Handler failed with a runtime defect.
    Defect,
    /// Handler exceeded `MO_LIMIT_SERVICE_WORK`.
    Work,
    /// Handler exceeded memory/stack/depth limits.
    Memory,
    /// Handler returned an updated state that exceeded `MO_LIMIT_SAVED_BYTES`.
    StateSize,
    /// Run could not start (e.g. OOM or undecodable inputs).
    Refused,
}

impl OutcomeStatus {
    /// Canonical JSON string (`outcome_status` in `src/tools/offline.c`).
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Succeeded => "succeeded",
            Self::Defect => "defect",
            Self::Work => "work",
            Self::Memory => "memory",
            Self::StateSize => "state_size",
            Self::Refused => "refused",
        }
    }

    /// Returns `true` when this outcome represents one of the 4 failure classes.
    #[must_use]
    pub const fn is_failure(self) -> bool {
        matches!(
            self,
            Self::Defect | Self::Work | Self::Memory | Self::StateSize
        )
    }
}

/// Reply kind when a handler returns (`enum mo_reply_kind`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ReplyKind {
    /// Plain return or `Result.Ok`.
    Ok,
    /// Tool returned `Result.Error`.
    Error,
}

/// Replay classification comparing recorded signature against offline run (`enum mo_replay_class`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ReplayClass {
    /// Offline run reproduced the exact recorded failure signature.
    Reproduced,
    /// Offline run failed, but with a different signature.
    Different,
    /// Offline run succeeded instead of failing.
    NotReproduced,
    /// Offline run was refused before executing.
    Refused,
}

impl ReplayClass {
    /// Canonical JSON/text name (`replay_class` in `src/tools/offline.c`).
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Reproduced => "reproduced",
            Self::Different => "different",
            Self::NotReproduced => "not_reproduced",
            Self::Refused => "refused",
        }
    }
}

/// Fix verification verdict (`enum mo_verdict`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Verdict {
    /// Original reproduced AND revised succeeded cleanly.
    Verified,
    /// Original reproduced, revised still failed.
    NotFixed,
    /// Original did not reproduce, so fix could not be verified.
    Unverified,
    /// Either original or revised was refused.
    Refused,
}

impl Verdict {
    /// Canonical JSON/text name (`verdict_name` in `src/tools/offline.c`).
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Verified => "verified",
            Self::NotFixed => "not_fixed",
            Self::Unverified => "unverified",
            Self::Refused => "refused",
        }
    }
}

/// Structured outcome of one offline handler call (`struct mo_run_outcome`).
#[derive(Debug, Clone)]
pub struct RunOutcome {
    /// Outcome status.
    pub status: OutcomeStatus,
    /// Failure signature (meaningful when `status.is_failure()`).
    pub signature: Option<FailureSignature>,
    /// Reply kind (meaningful when `status == Succeeded || status == StateSize`).
    pub reply: ReplyKind,
    /// Whether the state bytes changed (meaningful when `status == Succeeded || status == StateSize`).
    pub state_changed: bool,
}

/// Full result of [`crate::offline::offline_call`] (`struct mo_offline_run`).
#[derive(Debug, Clone)]
pub struct OfflineRun {
    /// Structured outcome.
    pub outcome: RunOutcome,
    /// Stack/variable context completeness.
    pub context_status: FailureContextStatus,
    /// Formatted stack/variable context text.
    pub context: String,
    /// Refusal message when `outcome.status == OutcomeStatus::Refused`.
    pub refusal: Option<String>,
}

/// Builds a [`FailureSignature`] from a persisted [`FailureRecord`].
#[must_use]
pub fn signature_from_record(rec: &FailureRecord) -> FailureSignature {
    FailureSignature {
        class: rec.class,
        reason: rec.reason.clone(),
        limit: rec.limit.clone(),
        limit_value: rec.limit_value,
        function: rec.function,
        file: rec.file,
        start: rec.start,
        end: rec.end,
    }
}

/// Compares two [`FailureSignature`]s using the exact equivalence rules of `src/base/verdict.c`.
#[must_use]
pub fn signature_match(a: &FailureSignature, b: &FailureSignature) -> bool {
    if a.class != b.class {
        return false;
    }
    match a.class {
        0 => {
            a.reason == b.reason
                && a.function == b.function
                && a.file == b.file
                && a.start == b.start
                && a.end == b.end
        }
        1 => a.limit_value == b.limit_value,
        2 | 3 => a.limit == b.limit && a.limit_value == b.limit_value,
        _ => false,
    }
}

/// Classifies an offline replay outcome against the recorded failure signature (`mo_replay_classify`).
#[must_use]
pub fn classify_replay(recorded: &FailureSignature, offline: &RunOutcome) -> ReplayClass {
    match offline.status {
        OutcomeStatus::Refused => ReplayClass::Refused,
        OutcomeStatus::Succeeded => ReplayClass::NotReproduced,
        _ => match &offline.signature {
            Some(sig) if signature_match(recorded, sig) => ReplayClass::Reproduced,
            _ => ReplayClass::Different,
        },
    }
}

/// Computes the `--fix` verification verdict (`mo_verdict`).
#[must_use]
pub fn compute_verdict(original: ReplayClass, revised: &RunOutcome) -> Verdict {
    if original == ReplayClass::Refused || revised.status == OutcomeStatus::Refused {
        return Verdict::Refused;
    }
    if original != ReplayClass::Reproduced {
        return Verdict::Unverified;
    }
    if revised.status == OutcomeStatus::Succeeded {
        Verdict::Verified
    } else {
        Verdict::NotFixed
    }
}

/// One structured offline refusal (`struct refusal` in `src/tools/offline.c`).
#[derive(Debug, Clone)]
pub struct OfflineRefusal {
    /// Compatibility check name (e.g. `"format"`, `"kind"`, `"build"`, `"program"`).
    pub check: &'static str,
    /// Human-readable message.
    pub message: String,
    /// Recorded value (if any).
    pub recorded: Option<String>,
    /// Found value (if any).
    pub found: Option<String>,
    /// Custom text label for `found` (defaults to `"found"`).
    pub found_label: &'static str,
}

impl OfflineRefusal {
    /// Creates a new refusal with default `"found"` label.
    #[must_use]
    pub fn new(
        check: &'static str,
        message: impl Into<String>,
        recorded: Option<String>,
        found: Option<String>,
    ) -> Self {
        Self {
            check,
            message: message.into(),
            recorded,
            found,
            found_label: "found",
        }
    }

    /// Formats the single-line stderr text (`write_refusal_text` in `src/tools/offline.c`).
    #[must_use]
    pub fn to_text_line(&self, command: &str) -> String {
        let mut out = format!("{command}: refused: {}: {}", self.check, self.message);
        if let Some(ref rec) = self.recorded
            && !rec.is_empty()
        {
            let _ = write!(out, "; recorded {rec}");
        }
        if let Some(ref fnd) = self.found
            && !fnd.is_empty()
        {
            let _ = write!(out, "; {} {fnd}", self.found_label);
        }
        out.push('\n');
        out
    }

    /// Appends the JSON refusal object (`refusal_json` in `src/tools/offline.c`).
    pub fn push_json(&self, out: &mut String) {
        out.push_str("{\"check\":\"");
        out.push_str(self.check);
        out.push_str("\",\"message\":");
        push_json_string(out, &self.message);
        out.push_str(",\"recorded\":");
        match self.recorded.as_deref().filter(|s| !s.is_empty()) {
            Some(rec) => push_json_string(out, rec),
            None => out.push_str("null"),
        }
        out.push_str(",\"found\":");
        match self.found.as_deref().filter(|s| !s.is_empty()) {
            Some(fnd) => push_json_string(out, fnd),
            None => out.push_str("null"),
        }
        out.push('}');
    }
}

/// Formats `context_status` string (`complete`, `truncated`, `unavailable`).
#[must_use]
pub const fn context_status_str(status: FailureContextStatus) -> &'static str {
    match status {
        FailureContextStatus::Complete => "complete",
        FailureContextStatus::Truncated => "truncated",
        FailureContextStatus::Unavailable => "unavailable",
    }
}

/// Describes a service handler in text (`describe` in `src/tools/offline.c`).
#[must_use]
pub fn describe_handler(
    name: &str,
    timer: bool,
    read_only: bool,
    index: u32,
    arguments: u32,
) -> String {
    let kind = if timer {
        "timer"
    } else if read_only {
        "read-only tool"
    } else {
        "updating tool"
    };
    let noun = if arguments == 1 {
        "argument"
    } else {
        "arguments"
    };
    format!(
        "{} ({kind} {index}, {arguments} {noun})",
        sanitize_terminal_text(name)
    )
}

/// Appends indented context lines (`context_text` in `src/tools/offline.c`).
pub fn push_context_text(out: &mut String, label: &str, status: FailureContextStatus, text: &str) {
    let _ = writeln!(out, "{label} ({}):", context_status_str(status));
    for line in text.split('\n') {
        if line.is_empty() && text.ends_with('\n') {
            continue;
        }
        out.push_str("  ");
        out.push_str(&sanitize_terminal_text(line));
        out.push('\n');
    }
}

/// Appends the JSON representation of an [`OfflineRun`] (`run_json` in `src/tools/offline.c`).
pub fn push_run_json(out: &mut String, run: &OfflineRun) {
    let x = &run.outcome;
    let failed = x.status.is_failure();
    let returned = matches!(
        x.status,
        OutcomeStatus::Succeeded | OutcomeStatus::StateSize
    );
    out.push_str("{\"status\":\"");
    out.push_str(x.status.as_str());
    out.push_str("\",\"signature\":");
    if failed && let Some(ref sig) = x.signature {
        sig.push_json(out, 1);
    } else {
        out.push_str("null");
    }
    out.push_str(",\"reply\":");
    if !returned {
        out.push_str("null");
    } else if x.reply == ReplyKind::Error {
        out.push_str("\"error\"");
    } else {
        out.push_str("\"ok\"");
    }
    out.push_str(",\"state_changed\":");
    if !returned {
        out.push_str("null");
    } else if x.state_changed {
        out.push_str("true");
    } else {
        out.push_str("false");
    }
    out.push_str(",\"context_status\":");
    if failed {
        let _ = write!(
            out,
            "\"{}\",\"context\":",
            context_status_str(run.context_status)
        );
        push_json_string(out, &run.context);
    } else {
        out.push_str("null,\"context\":null");
    }
    if x.status == OutcomeStatus::Refused {
        out.push_str(",\"refusal\":");
        push_json_string(out, run.refusal.as_deref().unwrap_or("refused"));
    }
    out.push('}');
}

/// Appends the terminal text representation of an [`OfflineRun`] (`run_text` in `src/tools/offline.c`).
pub fn push_run_text(out: &mut String, label: &str, run: &OfflineRun) {
    let x = &run.outcome;
    let _ = write!(out, "{label}: ");
    match x.status {
        OutcomeStatus::Succeeded => {
            let reply = if x.reply == ReplyKind::Error {
                "error"
            } else {
                "ok"
            };
            let changed = if x.state_changed {
                "state changed"
            } else {
                "state unchanged"
            };
            let _ = writeln!(out, "succeeded, reply {reply}, {changed}");
        }
        OutcomeStatus::Refused => {
            let _ = writeln!(
                out,
                "refused before it started: {}",
                run.refusal.as_deref().unwrap_or("refused")
            );
        }
        _ => {
            if let Some(ref sig) = x.signature {
                let _ = writeln!(out, "{}", sig.to_text());
            } else {
                let _ = writeln!(out, "{}", x.status.as_str());
            }
            push_context_text(out, "stack", run.context_status, &run.context);
        }
    }
}
