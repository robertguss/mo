//! Service promise and recovery combined execution (`src/tools/service-tests.{h,c}`).

use std::io::Write;
use std::time::Instant;

use mo_base::{DiagBag, Diagnostic, SourceMap, Span, TerminalEncoding};
use mo_bytecode::{BytecodeFile, CaseShape, FunctionKind, TypeKind};
use mo_check::{CheckedProgram, CheckedSpec, ServiceReply};
use mo_def::ErrorCode;
use mo_def::limits::{MO_CLI_RECOVERY_STEPS, MO_LIMIT_SERVICE_WORK};
use mo_heap::Value;
use mo_heap::saved::{copy_saved, decode_value};
use mo_host::HostContext;
use mo_vm::simulation::SimulationSent;
use mo_vm::{CallObserver, ControlledOutcome, Defect, Image, Limits, Process};

use crate::promises::{PromiseVacuity, promise_vacuity_plan, vacuity_missing_line};
use crate::recovery::{
    Recovery, RecoveryCounts, RecoveryHooks, RecoveryObservation, RecoveryOutcome,
    RecoveryStepKind, RecoveryVerdict,
};
use crate::render::{
    Observation, RecoveryReport, TestOutcome, TestResult, TestTotals, VacuityKind,
    detect_terminal_encoding, render_test_result,
};

struct Judge {
    item: usize,
    function: u32,
    tool: Option<u32>,
    vacuity: PromiseVacuity,
    seen_mask: u64,
    active: bool,
}

/// Finds a compiled service promise or anchor function by naming convention (`mo_service_function`).
#[must_use]
pub fn service_function(
    file: &BytecodeFile,
    module_name: &str,
    promise_number: u32,
    anchor: Option<&CheckedSpec>,
) -> Option<u32> {
    let want = match anchor {
        None => format!("{module_name}.service.promise.{promise_number}"),
        Some(a) => format!(
            "{module_name}.service.promise.{promise_number}.{}.{}",
            if a.allows { "allows" } else { "forbids" },
            a.number
        ),
    };
    file.functions
        .iter()
        .position(|f| f.kind == FunctionKind::Function && file.string(f.name) == want)
        .map(|i| i as u32)
}

fn report_error(diags: &mut DiagBag, message: &str, span: Span, detail: Option<&str>) {
    let mut diag = Diagnostic::new(ErrorCode::ToolPipeline, span, message);
    if let Some(d) = detail {
        diag = diag.with_primary_text(d);
    }
    let _ = diags.push(diag);
}

struct JudgeContext<'a> {
    image: &'a Image,
    limits: Limits,
    service: u32,
    recovery_heap: &'a mo_heap::Heap,
    process: Process<'a>,
    judges: &'a mut [Judge],
    problem: Option<&'static str>,
    last: bool,
    replies: &'a mut [Option<(u32, Value)>],
    stopped_defect: Option<Defect>,
    check_enabled: bool,
    observe_enabled: bool,
}

impl<'a> JudgeContext<'a> {
    fn copy_in(&self, ty: u32, val: &Value) -> Option<Value> {
        copy_saved(&self.image.file().types, self.process.heap(), ty, val)
            .ok()
            .map(|(v, _)| v)
    }

    fn reply_value(&self, obs: &RecoveryObservation<'_>, expected_ty: u32) -> Option<Value> {
        let file = self.image.file();
        let types = &file.types;
        let reply = obs.reply?;
        let (shown, value) = extract_reply_from(
            file,
            self.recovery_heap,
            self.service,
            obs.step.tool,
            obs.step.sent,
            reply,
        );
        if shown == expected_ty {
            return self.copy_in(expected_ty, &value);
        }
        let result_def = types.get(expected_ty);
        let tag = u32::from(obs.step.sent == SimulationSent::Rejected);
        let (inner_shown, inner_val) = if tag == 1 {
            let v = self
                .recovery_heap
                .extract_case_field(types, shown, &value, 1, 0);
            let s = types.get(shown).cases[1].fields[0].ty;
            (s, v)
        } else {
            (shown, value)
        };
        debug_assert!(
            result_def.kind == TypeKind::Choice
                && result_def.cases[tag as usize].fields[0].ty == inner_shown
        );
        let payload = self.copy_in(inner_shown, &inner_val)?;
        self.process
            .heap()
            .make_case(types, expected_ty, tag, vec![payload])
            .ok()
    }

    fn build_arguments(&self, judge: &Judge, obs: &RecoveryObservation<'_>) -> Option<Vec<Value>> {
        let file = self.image.file();
        let f = &file.functions[judge.function as usize];
        let state_ty = file.services[self.service as usize].state_type;
        let before = decode_value(self.process.heap(), &file.types, state_ty, obs.before?).ok()?;
        let after = decode_value(self.process.heap(), &file.types, state_ty, obs.after?).ok()?;
        let mut out = vec![before, after, Value::Time(obs.now)];
        if let Some(tool_idx) = judge.tool {
            let tool = &file.services[self.service as usize].tools[tool_idx as usize];
            for (a, &arg_ty) in tool.argument_types.iter().enumerate() {
                out.push(self.copy_in(arg_ty, &obs.step.arguments[a])?);
            }
            let reply_ty = f.parameters[f.parameters.len() - 1].ty;
            out.push(self.reply_value(obs, reply_ty)?);
        }
        Some(out)
    }

    fn eval_judge(&mut self, judge_idx: usize, obs: &RecoveryObservation<'_>) -> (bool, u32) {
        let (fn_idx, args) = {
            let j = &self.judges[judge_idx];
            let Some(args) = self.build_arguments(j, obs) else {
                self.problem =
                    Some("a service promise's values could not be copied into its process");
                return (true, 0);
            };
            (j.function, args)
        };
        let mut remaining = MO_LIMIT_SERVICE_WORK + 1;
        let mut keep = || {
            if remaining == 0 {
                return false;
            }
            remaining -= 1;
            true
        };
        let outcome = self
            .process
            .call_controlled(fn_idx, args, Some(&mut keep), None);
        match outcome {
            ControlledOutcome::Returned(val) => {
                let answer = val.as_int();
                if answer > 0 {
                    (true, answer as u32)
                } else {
                    self.judges[judge_idx].seen_mask |= answer.wrapping_neg() as u64;
                    (true, 0)
                }
            }
            ControlledOutcome::Cancelled => {
                if self.last {
                    self.stopped_defect = Some(Defect {
                        reason: "running past a service promise's work limit".to_string(),
                        limit_name: Some("MO_LIMIT_SERVICE_WORK"),
                        limit: MO_LIMIT_SERVICE_WORK as usize,
                        detail: None,
                        frames: Vec::new(),
                        variables: Vec::new(),
                        variables_unavailable: false,
                    });
                }
                self.process = Process::new(
                    self.image,
                    self.limits,
                    HostContext::stdio(Vec::new(), Vec::new()),
                );
                (false, 0)
            }
            ControlledOutcome::Failed(defect) | ControlledOutcome::Stopped(defect) => {
                if self.last {
                    self.stopped_defect = Some(defect);
                }
                self.process = Process::new(
                    self.image,
                    self.limits,
                    HostContext::stdio(Vec::new(), Vec::new()),
                );
                (false, 0)
            }
        }
    }
}

impl RecoveryHooks for JudgeContext<'_> {
    fn observe(&mut self, obs: &RecoveryObservation<'_>) {
        if !self.observe_enabled {
            return;
        }
        let Some(reply) = obs.reply else {
            return;
        };
        let (ty, val) = extract_reply_from(
            self.image.file(),
            self.recovery_heap,
            self.service,
            obs.step.tool,
            obs.step.sent,
            reply,
        );
        if let Some(slot) = self.replies.get_mut(obs.index as usize) {
            *slot = Some((ty, val));
        }
    }

    fn has_check(&self) -> bool {
        self.check_enabled
    }

    fn check(&mut self, obs: &RecoveryObservation<'_>) -> RecoveryVerdict {
        if self.problem.is_some() || obs.before.is_none() || obs.after.is_none() {
            return RecoveryVerdict::default();
        }
        for i in 0..self.judges.len() {
            let matches = {
                let j = &self.judges[i];
                j.active
                    && match j.tool {
                        None => true,
                        Some(t) => {
                            obs.step.kind == RecoveryStepKind::Send
                                && obs.step.tool == t
                                && obs.reply.is_some()
                        }
                    }
            };
            if !matches {
                continue;
            }
            let (returned, line) = self.eval_judge(i, obs);
            if self.problem.is_some() {
                return RecoveryVerdict::default();
            }
            if !returned || line != 0 {
                return RecoveryVerdict {
                    check: i as u32,
                    line,
                };
            }
        }
        RecoveryVerdict::default()
    }
}

fn extract_reply_from(
    file: &BytecodeFile,
    heap: &mo_heap::Heap,
    service: u32,
    tool: u32,
    sent: SimulationSent,
    reply: &mo_vm::simulation::SimulationReply,
) -> (u32, Value) {
    let mut ty = reply.ty;
    let mut value = reply.reply.clone().unwrap_or(Value::Unit);
    let svc_tool = &file.services[service as usize].tools[tool as usize];
    if sent == SimulationSent::Replied && !svc_tool.read_only {
        let t = file.types.get(ty);
        if t.kind == TypeKind::Choice {
            value = heap.extract_case_field(&file.types, ty, &value, 0, 0);
            ty = t.cases[0].fields[0].ty;
        }
        let tup = file.types.get(ty);
        value = heap.extract_field(&value, 1);
        ty = tup.parameters[1];
    }
    (ty, value)
}

fn harvest_tool_examples(
    program: &CheckedProgram,
    image: &Image,
    limits: Limits,
    service: u32,
    items: &[Observation],
    judge: &mut Judge,
) {
    let file = image.file();
    let item = &items[judge.item];
    if let (Some(svc_idx), Some(prom_idx)) = (item.service_index, item.service_promise_index) {
        let checked_svc = &program.services[svc_idx];
        let promise = &checked_svc.promises[prom_idx];
        let mod_name = &program.modules.module(promise.spec.module).name;
        for anchor in &promise.anchors {
            if (judge.seen_mask & judge.vacuity.required_mask) == judge.vacuity.required_mask {
                break;
            }
            if !anchor.allows {
                continue;
            }
            let Some(fn_idx) = service_function(file, mod_name, promise.spec.number, Some(anchor))
            else {
                continue;
            };
            let mut proc = Process::new(image, limits, HostContext::stdio(Vec::new(), Vec::new()));
            let mut budget = MO_LIMIT_SERVICE_WORK + 1;
            let mut keep = || {
                if budget == 0 {
                    return false;
                }
                budget -= 1;
                true
            };
            if let ControlledOutcome::Returned(ans) =
                proc.call_controlled(fn_idx, Vec::new(), Some(&mut keep), None)
            {
                let cond = ans.as_int();
                if cond <= 0 {
                    judge.seen_mask |= cond.wrapping_neg() as u64;
                }
            }
        }
    }
    let Some(tool_idx) = judge.tool else {
        return;
    };
    if (judge.seen_mask & judge.vacuity.required_mask) == judge.vacuity.required_mask {
        return;
    }
    let btool = &file.services[service as usize].tools[tool_idx as usize];
    let handler_fn = btool.function;
    let svc_idx = item.service_index.unwrap_or(0);
    let reply_kind = program.services[svc_idx].tools[tool_idx as usize].reply;
    let pfn = &file.functions[judge.function as usize];
    let hname = file.function_name(handler_fn);

    for pass in 0..2 {
        if (judge.seen_mask & judge.vacuity.required_mask) == judge.vacuity.required_mask {
            break;
        }
        for ex in 0..file.functions.len() {
            if (judge.seen_mask & judge.vacuity.required_mask) == judge.vacuity.required_mask {
                break;
            }
            let fk = file.functions[ex].kind;
            if !matches!(fk, FunctionKind::Example | FunctionKind::Test) {
                continue;
            }
            let ex_name = file.function_name(ex as u32);
            let direct = fk == FunctionKind::Example
                && ex_name
                    .strip_prefix(hname)
                    .is_some_and(|rest| rest.starts_with(".example."));
            if direct != (pass == 0) {
                continue;
            }
            let mut proc = Process::new(image, limits, HostContext::stdio(Vec::new(), Vec::new()));
            let mut observer = CallObserver::new(handler_fn);
            let mut budget = MO_LIMIT_SERVICE_WORK + 1;
            let mut keep = || {
                if budget == 0 {
                    return false;
                }
                budget -= 1;
                true
            };
            let ex_out =
                proc.call_controlled(ex as u32, Vec::new(), Some(&mut keep), Some(&mut observer));
            if let ControlledOutcome::Returned(_) = ex_out {
                let reply_ty = pfn.parameters[pfn.parameters.len() - 1].ty;
                let hres_ty = file.functions[handler_fn as usize].result_type;
                for call in &observer.calls {
                    if (judge.seen_mask & judge.vacuity.required_mask)
                        == judge.vacuity.required_mask
                    {
                        break;
                    }
                    let before = call.arguments[0].clone();
                    let mut after = before.clone();
                    let now = call.arguments[1].clone();
                    let reply_cell = match reply_kind {
                        ServiceReply::ReadOnly => call.result.clone(),
                        ServiceReply::Update => {
                            after = proc.heap().extract_field(&call.result, 0);
                            proc.heap().extract_field(&call.result, 1)
                        }
                        ServiceReply::ResultUpdate => {
                            let tag = call.result.as_tag();
                            let inner = match file.types.case_shape(hres_ty, tag) {
                                CaseShape::Empty => Value::Unit,
                                CaseShape::Inline | CaseShape::Ref | CaseShape::Boxed => proc
                                    .heap()
                                    .extract_case_field(&file.types, hres_ty, &call.result, tag, 0),
                            };
                            let payload_val = if tag == 0 {
                                after = proc.heap().extract_field(&inner, 0);
                                proc.heap().extract_field(&inner, 1)
                            } else {
                                inner
                            };
                            let Ok(constructed) = proc.heap().make_case(
                                &file.types,
                                reply_ty,
                                tag,
                                vec![payload_val],
                            ) else {
                                break;
                            };
                            constructed
                        }
                    };
                    let mut call_args = Vec::with_capacity(pfn.parameters.len());
                    call_args.push(before);
                    call_args.push(after);
                    call_args.push(now);
                    for a in 0..btool.argument_types.len() {
                        call_args.push(call.arguments[2 + a].clone());
                    }
                    call_args.push(reply_cell);
                    let mut pbudget = MO_LIMIT_SERVICE_WORK + 1;
                    let mut pkeep = || {
                        if pbudget == 0 {
                            return false;
                        }
                        pbudget -= 1;
                        true
                    };
                    let pout =
                        proc.call_controlled(judge.function, call_args, Some(&mut pkeep), None);
                    let ControlledOutcome::Returned(pres) = pout else {
                        break;
                    };
                    let ans = pres.as_int();
                    if ans <= 0 {
                        judge.seen_mask |= ans.wrapping_neg() as u64;
                    }
                }
            }
        }
    }
}

/// Runs recovery and service promise checks for one service (`src/tools/service-tests.c:mo_test_service`).
#[expect(
    clippy::too_many_arguments,
    reason = "matches C mo_test_service signature directly"
)]
pub fn run_service_tests(
    json: bool,
    sequences: u64,
    first_failure: bool,
    limits: Limits,
    program: &CheckedProgram,
    image: &Image,
    sources: &SourceMap,
    items: &[Observation],
    service: u32,
    output: &mut dyn Write,
    diags: &mut DiagBag,
    totals: &mut TestTotals,
) -> bool {
    let start_instant = Instant::now();
    let count = items.len();
    let mut decided = vec![false; count];
    let mut records: Vec<Option<Vec<u8>>> = vec![None; count];
    let encoding = if json {
        TerminalEncoding::Utf8
    } else {
        detect_terminal_encoding()
    };

    let mut judges = Vec::new();
    let mut recovery_item: Option<usize> = None;
    let mut problem: Option<&'static str> = None;

    let checked_svc_idx = items[0].service_index.unwrap_or(0);
    let checked_svc = &program.services[checked_svc_idx];
    let mod_name = &program.modules.module(checked_svc.module).name;
    let file = image.file();

    for (i, item) in items.iter().enumerate() {
        let Some(prom_idx) = item.service_promise_index else {
            recovery_item = Some(i);
            continue;
        };
        let promise = &checked_svc.promises[prom_idx];
        let Some(fn_idx) = service_function(file, mod_name, promise.spec.number, None) else {
            problem = Some("service promises could not be bound to compiled functions");
            break;
        };
        judges.push(Judge {
            item: i,
            function: fn_idx,
            tool: promise.tool,
            vacuity: promise_vacuity_plan(program, &promise.spec),
            seen_mask: 0,
            active: true,
        });
    }

    let mut recovery_opt = if problem.is_none() {
        match Recovery::new(image, service, limits) {
            Ok(r) => Some(r),
            Err(_) => {
                problem = Some("recovery check could not be prepared");
                None
            }
        }
    } else {
        None
    };

    let decide = |item_idx: usize,
                  err_problem: Option<&str>,
                  err_detail: Option<&str>,
                  result: Option<&TestResult<'_>>,
                  decided: &mut [bool],
                  records: &mut [Option<Vec<u8>>],
                  diags: &mut DiagBag,
                  totals: &mut TestTotals|
     -> bool {
        debug_assert!(!decided[item_idx]);
        decided[item_idx] = true;
        let obs = &items[item_idx];
        if let Some(p) = err_problem {
            report_error(diags, p, obs.span, Some(err_detail.unwrap_or(&obs.name)));
            return true;
        }
        let Some(res) = result else {
            return true;
        };
        match res.outcome {
            TestOutcome::Passed => totals.passed += 1,
            TestOutcome::Failed => {
                totals.failed += 1;
                if obs.service_promise_index.is_none() {
                    totals.failed_tests += 1;
                }
            }
            TestOutcome::Stopped => {
                totals.stopped += 1;
                if obs.service_promise_index.is_none() {
                    totals.stopped_tests += 1;
                }
            }
        }
        match render_test_result(sources, res, json, encoding) {
            Ok(buf) => {
                records[item_idx] = Some(buf);
                true
            }
            Err(_) => {
                report_error(
                    diags,
                    "test result could not be rendered or written completely",
                    obs.span,
                    Some(&obs.name),
                );
                false
            }
        }
    };

    let mut written = true;
    if let Some(p) = problem {
        for i in 0..count {
            written = decide(
                i,
                Some(p),
                None,
                None,
                &mut decided,
                &mut records,
                diags,
                totals,
            ) && written;
        }
    } else if let Some(ref mut recovery) = recovery_opt {
        let mut base = RecoveryCounts::default();
        let mut first = 0u64;
        loop {
            let comparing = recovery_item.is_some_and(|ri| !decided[ri]);
            recovery.set_compare_restarts(comparing);
            for j in &mut judges {
                j.active = !decided[j.item];
            }
            let mut dummy_replies: Vec<Option<(u32, Value)>> = Vec::new();
            let (found, search_problem) = {
                let rec_heap_ptr: *const mo_heap::Heap = recovery.heap();
                // Recovery::heap() lives inside Recovery::process whose address stays stable;
                // use a separate Process heap for judge execution and pass replies cleanly.
                let _ = rec_heap_ptr;
                if judges.is_empty() {
                    (recovery.search(first, sequences, None), None)
                } else {
                    // Build a temporary JudgeContext referencing a fresh heap copy of the recovery heap?
                    // Wait: we cannot borrow recovery.heap() while borrowing recovery mutably!
                    // Instead, extract replies using a temporary heap when copying into JudgeContext.
                    let scratch_heap = mo_heap::Heap::new(limits.heap_size);
                    let mut jctx = JudgeContext {
                        image,
                        limits,
                        service,
                        recovery_heap: &scratch_heap,
                        process: Process::new(
                            image,
                            limits,
                            HostContext::stdio(Vec::new(), Vec::new()),
                        ),
                        judges: &mut judges,
                        problem: None,
                        last: false,
                        replies: &mut dummy_replies,
                        stopped_defect: None,
                        check_enabled: true,
                        observe_enabled: false,
                    };
                    let res = recovery.search(first, sequences, Some(&mut jctx));
                    (res, jctx.problem)
                }
            };

            let counts = RecoveryCounts {
                sequences: base.sequences + found.counts.sequences,
                restarts: base.restarts + found.counts.restarts,
                commits: base.commits + found.counts.commits,
                replayed: base.replayed + found.counts.replayed,
                snapshots: base.snapshots + found.counts.snapshots,
            };

            let mut run_problem: Option<&str> =
                search_problem.map(|_| "service promises could not run");
            let mut run_detail: Option<&str> = search_problem;
            let mut every = TestOutcome::Passed;
            let mut more = false;

            match found.outcome {
                RecoveryOutcome::Passed => {}
                RecoveryOutcome::Stopped => every = TestOutcome::Stopped,
                RecoveryOutcome::Unavailable => {
                    run_problem = Some("recovery check could not run");
                    run_detail = found.problem;
                }
                RecoveryOutcome::Mismatch | RecoveryOutcome::Refused | RecoveryOutcome::Check => {
                    if run_problem.is_none() {
                        let is_check = found.outcome == RecoveryOutcome::Check;
                        let item_idx = if is_check {
                            judges[found.verdict.check as usize].item
                        } else {
                            recovery_item.unwrap_or(0)
                        };
                        let mut seq = found.sequence.clone();
                        recovery.set_compare_restarts(!is_check);
                        if is_check {
                            for (ji, j) in judges.iter_mut().enumerate() {
                                j.active = !decided[j.item] && ji as u32 == found.verdict.check;
                            }
                        }
                        let scratch_heap = mo_heap::Heap::new(limits.heap_size);
                        if !first_failure {
                            if is_check {
                                let mut jctx = JudgeContext {
                                    image,
                                    limits,
                                    service,
                                    recovery_heap: &scratch_heap,
                                    process: Process::new(
                                        image,
                                        limits,
                                        HostContext::stdio(Vec::new(), Vec::new()),
                                    ),
                                    judges: &mut judges,
                                    problem: None,
                                    last: false,
                                    replies: &mut dummy_replies,
                                    stopped_defect: None,
                                    check_enabled: true,
                                    observe_enabled: false,
                                };
                                let _ = recovery.shrink(
                                    &mut seq,
                                    found.outcome,
                                    Some(&mut jctx),
                                    found.verdict,
                                );
                            } else {
                                let _ =
                                    recovery.shrink(&mut seq, found.outcome, None, found.verdict);
                            }
                        }
                        let mut kept_replies: Vec<Option<(u32, Value)>> =
                            vec![None; MO_CLI_RECOVERY_STEPS as usize];
                        let (rerun, replay_problem, stopped_defect) = {
                            let mut jctx = JudgeContext {
                                image,
                                limits,
                                service,
                                recovery_heap: &scratch_heap,
                                process: Process::new(
                                    image,
                                    limits,
                                    HostContext::stdio(Vec::new(), Vec::new()),
                                ),
                                judges: &mut judges,
                                problem: None,
                                last: true,
                                replies: &mut kept_replies,
                                stopped_defect: None,
                                check_enabled: is_check,
                                observe_enabled: true,
                            };
                            let rr = recovery.replay(&mut seq, Some(&mut jctx));
                            (rr, jctx.problem, jctx.stopped_defect)
                        };
                        let same = rerun.outcome == found.outcome && rerun.verdict == found.verdict;
                        let mut final_counts = counts;
                        final_counts.sequences += 1;
                        let report = RecoveryReport {
                            heap: recovery.heap(),
                            service,
                            counts: final_counts,
                            sequence: Some(&seq),
                            result: &rerun,
                            replies: &kept_replies,
                        };
                        let (outcome, line) = if is_check && rerun.verdict.line == 0 {
                            (TestOutcome::Stopped, Span::NONE)
                        } else if is_check {
                            let prom_idx = items[item_idx].service_promise_index.unwrap_or(0);
                            let cond_span = checked_svc.promises[prom_idx].spec.conditions
                                [(rerun.verdict.line - 1) as usize]
                                .text
                                .span;
                            (TestOutcome::Failed, cond_span)
                        } else {
                            (TestOutcome::Failed, Span::NONE)
                        };
                        let test_res = TestResult {
                            observation: &items[item_idx],
                            file,
                            outcome,
                            vacuity: VacuityKind::None,
                            defect: stopped_defect.as_ref(),
                            line,
                            microseconds: start_instant.elapsed().as_micros() as u64,
                            promise: None,
                            recovery: Some(&report),
                        };
                        let fail_prob = if replay_problem.is_some() {
                            Some("service promises could not run")
                        } else if same {
                            None
                        } else {
                            Some(
                                "the shrunk recovery sequence did not fail the \
                                 same way when it ran again",
                            )
                        };
                        written = decide(
                            item_idx,
                            fail_prob,
                            replay_problem,
                            Some(&test_res),
                            &mut decided,
                            &mut records,
                            diags,
                            totals,
                        );
                        more = written;
                    }
                }
            }

            if written && !more {
                let empty_replies: [Option<(u32, Value)>; 0] = [];
                let report = RecoveryReport {
                    heap: recovery.heap(),
                    service,
                    counts,
                    sequence: None,
                    result: &found,
                    replies: &empty_replies,
                };
                for i in 0..count {
                    if !written {
                        break;
                    }
                    if decided[i] {
                        continue;
                    }
                    let mut outcome = every;
                    let mut vacuity = VacuityKind::None;
                    let mut line = Span::NONE;
                    if every == TestOutcome::Passed
                        && run_problem.is_none()
                        && let Some(prom_idx) = items[i].service_promise_index
                        && let Some(j) = judges.iter_mut().find(|j| j.item == i)
                    {
                        if j.vacuity.tautology_line != 0 {
                            outcome = TestOutcome::Failed;
                            vacuity = VacuityKind::Tautology;
                            line = checked_svc.promises[prom_idx].spec.conditions
                                [(j.vacuity.tautology_line - 1) as usize]
                                .text
                                .span;
                        } else if (j.seen_mask & j.vacuity.required_mask) != j.vacuity.required_mask
                        {
                            harvest_tool_examples(program, image, limits, service, items, j);
                            if (j.seen_mask & j.vacuity.required_mask) != j.vacuity.required_mask {
                                let missing =
                                    vacuity_missing_line(j.vacuity.required_mask, j.seen_mask);
                                outcome = TestOutcome::Failed;
                                vacuity = VacuityKind::UnreachedImplies;
                                line = checked_svc.promises[prom_idx].spec.conditions
                                    [(missing - 1) as usize]
                                    .text
                                    .span;
                            }
                        }
                    }
                    let test_res = TestResult {
                        observation: &items[i],
                        file,
                        outcome,
                        vacuity,
                        defect: None,
                        line,
                        microseconds: start_instant.elapsed().as_micros() as u64,
                        promise: None,
                        recovery: Some(&report),
                    };
                    written = decide(
                        i,
                        run_problem,
                        run_detail,
                        Some(&test_res),
                        &mut decided,
                        &mut records,
                        diags,
                        totals,
                    );
                }
            }

            base.sequences += found.resume.sequences;
            base.restarts += found.resume.restarts;
            base.commits += found.resume.commits;
            base.replayed += found.resume.replayed;
            base.snapshots += found.resume.snapshots;
            first = found.ordinal;
            if !more || first_failure {
                break;
            }
        }
    }

    for (i, rec) in records.iter().enumerate() {
        if !written {
            break;
        }
        if let Some(buf) = rec
            && !buf.is_empty()
            && (output.write_all(buf).is_err() || output.flush().is_err())
        {
            report_error(
                diags,
                "test result could not be rendered or written completely",
                items[i].span,
                Some(&items[i].name),
            );
            written = false;
        }
    }
    written
}
