//! Promise anchor checking (`src/tools/run.c:mo_program_has_anchors` & `mo_check_anchors` parity).

use mo_base::{DiagBag, Diagnostic, Label, SourceMap, Span};
use mo_bytecode::{AnchorExpectation, BytecodeFile, FunctionKind, NO_INDEX};
use mo_check::{CheckedProgram, CheckedSpec, CheckedSpecKind, ModuleId};
use mo_def::ErrorCode;
use mo_def::limits::{MO_CLI_ANCHOR_INSTRUCTIONS, MO_LIMIT_DEFECT_TEXT_SIZE};
use mo_host::HostContext;
use mo_vm::{Defect, Image, Outcome, Process, render_defect_raw};

use crate::cli::Cli;
use crate::render::runtime_span;

fn in_library(program: &CheckedProgram, module: ModuleId) -> bool {
    module.0 != 0 && program.modules.module(module).library
}

/// Returns whether the checked program has any non-library function or service promise anchors
/// (`src/tools/run.c:mo_program_has_anchors`).
#[must_use]
pub fn program_has_anchors(program: &CheckedProgram) -> bool {
    for spec in &program.specs {
        if spec.kind == CheckedSpecKind::Anchor && !in_library(program, spec.module) {
            return true;
        }
    }
    for service in &program.services {
        if in_library(program, service.module) {
            continue;
        }
        for promise in &service.promises {
            if !promise.anchors.is_empty() {
                return true;
            }
        }
    }
    false
}

/// Finds a compiled service promise or service anchor function by its lowered name
/// (`src/tools/run.c:mo_service_function`).
#[must_use]
pub fn service_function(
    file: &BytecodeFile,
    module_name: &str,
    promise_number: u32,
    anchor: Option<&CheckedSpec>,
) -> Option<u32> {
    let want = match anchor {
        None => format!("{module_name}.service.promise.{promise_number}"),
        Some(a) => {
            let dir = if a.allows { "allows" } else { "forbids" };
            format!(
                "{module_name}.service.promise.{promise_number}.{dir}.{}",
                a.number
            )
        }
    };
    for (idx, func) in file.functions.iter().enumerate() {
        if file.string(func.name) == want {
            return Some(idx as u32);
        }
    }
    None
}

fn anchor_error(
    file: &BytecodeFile,
    sources: &SourceMap,
    function: Option<u32>,
    message: &str,
    detail: &str,
    line: Option<Span>,
    diags: &mut DiagBag,
) -> bool {
    let primary_span = match function {
        Some(fn_idx) => {
            let dbg = &file.debug[fn_idx as usize];
            runtime_span(file, sources, dbg.file, dbg.start, dbg.end)
        }
        None => Span::NONE,
    };
    let mut diag =
        Diagnostic::new(ErrorCode::ToolAnchor, primary_span, message).with_primary_text(detail);
    if function.is_some()
        && let Some(cond_span) = line
    {
        diag = diag.with_secondary(Label::with_text(
            cond_span,
            "this promise condition was false",
        ));
    }
    diags.push(diag).is_ok()
}

fn anchor_stopped(
    file: &BytecodeFile,
    sources: &SourceMap,
    function: u32,
    defect: &Defect,
    diags: &mut DiagBag,
) -> bool {
    let raw = render_defect_raw(file, defect);
    let detail = if raw.len() <= MO_LIMIT_DEFECT_TEXT_SIZE as usize {
        raw.as_str()
    } else {
        "anchor execution could not complete; defect report unavailable"
    };
    anchor_error(
        file,
        sources,
        Some(function),
        "an anchor could not be checked",
        detail,
        None,
        diags,
    )
}

fn false_line(
    file: &BytecodeFile,
    sources: &SourceMap,
    function: u32,
    service_promise: Option<&CheckedSpec>,
    line: i64,
) -> Span {
    let idx = (line - 1) as usize;
    if let Some(svc_spec) = service_promise {
        return svc_spec
            .conditions
            .get(idx)
            .map_or(Span::NONE, |c| c.text.span);
    }
    let subj = file.debug[function as usize].subject;
    if subj == NO_INDEX {
        return Span::NONE;
    }
    file.debug[subj as usize]
        .lines
        .get(idx)
        .map_or(Span::NONE, |failed| {
            runtime_span(file, sources, failed.file, failed.start, failed.end)
        })
}

fn check_single_anchor(
    image: &Image,
    cli: &Cli,
    sources: &SourceMap,
    function: u32,
    allows: bool,
    service_promise: Option<&CheckedSpec>,
    diags: &mut DiagBag,
) -> bool {
    let file = image.file();
    let host = HostContext::stdio(Vec::new(), Vec::new());
    let mut process = Process::new(image, cli.limits, host);
    match process.call_bounded(function, Vec::new(), MO_CLI_ANCHOR_INSTRUCTIONS) {
        Outcome::Returned(val) => {
            let answer = val.as_int();
            if (answer <= 0) != allows {
                let cond_span = if answer > 0 {
                    Some(false_line(file, sources, function, service_promise, answer))
                } else {
                    None
                };
                let msg = if allows {
                    "an allows anchor was rejected"
                } else {
                    "a forbids anchor was accepted"
                };
                anchor_error(
                    file,
                    sources,
                    Some(function),
                    msg,
                    "allows must satisfy the promise; forbids must fail it (reference 12.2)",
                    cond_span,
                    diags,
                )
            } else {
                true
            }
        }
        Outcome::Failed(defect) | Outcome::Stopped(defect) => {
            anchor_stopped(file, sources, function, &defect, diags)
        }
    }
}

/// Checks every function and service promise anchor in `image`, recording `E1405`
/// diagnostics into `diags` (`src/tools/run.c:mo_check_anchors`).
#[must_use]
pub fn check_anchors(
    image: &Image,
    cli: &Cli,
    sources: &SourceMap,
    program: &CheckedProgram,
    diags: &mut DiagBag,
) -> bool {
    let file = image.file();
    for (idx, func) in file.functions.iter().enumerate() {
        if func.kind == FunctionKind::Anchor {
            let allows = file.debug[idx].expectation == AnchorExpectation::Allows;
            if !check_single_anchor(image, cli, sources, idx as u32, allows, None, diags) {
                return false;
            }
        }
    }
    for service in &program.services {
        for promise in &service.promises {
            let module = promise.spec.module;
            if in_library(program, module) {
                continue;
            }
            let mod_name = &program.modules.module(module).name;
            for anchor in &promise.anchors {
                if let Some(fn_idx) =
                    service_function(file, mod_name, promise.spec.number, Some(anchor))
                    && !check_single_anchor(
                        image,
                        cli,
                        sources,
                        fn_idx,
                        anchor.allows,
                        Some(&promise.spec),
                        diags,
                    )
                {
                    return false;
                }
            }
        }
    }
    !diags.has_any()
}
