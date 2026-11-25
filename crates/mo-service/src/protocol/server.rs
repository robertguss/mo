//! Dual-listener HTTP/1.1 + MCP + SSE + operator views + live upgrade server
//! (`src/tools/protocol-server.c`, `protocol-request.c`, `protocol-call.c`,
//! `protocol-views.c`, `protocol-export.c`, `protocol-upgrade.c` parity).

use std::collections::VecDeque;
use std::path::PathBuf;
use std::rc::Rc;

use mo_bytecode::{BytecodeFile, FNV_OFFSET, NO_INDEX, TypeKind, fnv1a};
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_LIMIT_BUNDLE_BYTES, MO_LIMIT_EXPORT_PAGE_BYTES, MO_LIMIT_EXPORT_SCRATCH,
    MO_LIMIT_MCP_BIGINT_DIGITS, MO_LIMIT_MCP_BODY_BYTES, MO_LIMIT_MCP_BODY_US, MO_LIMIT_MCP_BURST,
    MO_LIMIT_MCP_CATALOG_MEMORY, MO_LIMIT_MCP_CHUNKS, MO_LIMIT_MCP_CONNECTIONS,
    MO_LIMIT_MCP_ENVELOPE_NODES, MO_LIMIT_MCP_HEADER_BYTES, MO_LIMIT_MCP_HEADER_FIELDS,
    MO_LIMIT_MCP_HEADER_US, MO_LIMIT_MCP_HEADER_WORK, MO_LIMIT_MCP_INGRESS_BYTES,
    MO_LIMIT_MCP_LINE_BYTES, MO_LIMIT_MCP_PAYLOAD_NODES, MO_LIMIT_MCP_RATE,
    MO_LIMIT_MCP_REQUEST_MEMORY, MO_LIMIT_MCP_REQUEST_WORK, MO_LIMIT_MCP_RESPONSE_BYTES,
    MO_LIMIT_MCP_RESPONSE_NODES, MO_LIMIT_MCP_SESSION_ID_BYTES, MO_LIMIT_MCP_SESSIONS,
    MO_LIMIT_MCP_STREAM_BYTES, MO_LIMIT_MCP_TRAILER_BYTES, MO_LIMIT_MCP_TRAILER_FIELDS,
    MO_LIMIT_MCP_VALUE_DEPTH, MO_LIMIT_MCP_VIEW_BYTES, MO_LIMIT_MCP_VIEW_MEMORY,
    MO_LIMIT_MCP_VIEW_NAME_BYTES, MO_LIMIT_MCP_VIEW_TIMERS, MO_LIMIT_MCP_WRITE_US,
    MO_LIMIT_SERVICE_FAILURE_WINDOW_US, MO_LIMIT_SERVICE_RESTART_WINDOW_US,
    MO_LIMIT_UPGRADE_SLOT_IDLE_US,
};
use mo_heap::saved::decode_value;
use mo_heap::{Heap, Value};
use mo_host::HostContext;
use mo_vm::{Image, Limits, Outcome, Process};

use crate::binary::export::{
    ExportIdentity, ExportPin, create_export_pin, create_failure_export_pin, export_page_bounds,
};
use crate::binary::failure::read_failure_record;
use crate::binary::upload::{UploadPageStatus, UploadStage};
use crate::binary::{hex_decode_exact, hex_encode, sha256_bytes};
use crate::listener::{
    ConnectionKey, EVENT_ERROR, EVENT_READABLE, EVENT_WRITABLE, Interest, Listener, ListenerStatus,
    ListenerTime, WaitRequest, wait_pair,
};
use crate::service_loop::{ServiceEvent, ServiceEventSummary, ServiceLoop, ServiceStatus};
use crate::store::programs::{ActivationStatus, StoreActivation, StoredProgram};
use crate::store::{StoreRecord, StoreRecordKind};
use crate::upgrade::compile::{CompileJob, CompilePoll};
use crate::upgrade::{
    UpgradeChildren, UpgradeReport, compare_programs, program_metadata, run_migration_state,
};

use super::descriptor::build_descriptor;
use super::value::{ValueBridge, ValueBridgeStatus};
use super::{Pf, ProtocolFunctions, body_marks, build_protocol_image, choice_field};

const PROTOCOL_VERSION_TEXT: &str = "0.1.0";

/// Optional admin listener & upgrade compiler attachment.
pub struct AdminConfig {
    /// Unix domain socket listener for operator views, exports, and live upgrades.
    pub listener: Listener,
    /// Executable path to spawn for confined child compilation.
    pub executable: PathBuf,
    /// Optional explicit library directory.
    pub library: Option<PathBuf>,
    /// Confinement policy for compile children.
    pub children: UpgradeChildren,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ConnPhase {
    Headers,
    Continue,
    Body,
    Write,
    Stream,
    UpgradeWait,
}

struct SessionSlot {
    id: String,
    admin: bool,
    live: bool,
    stream_conn: Option<usize>,
}

struct Connection {
    key: ConnectionKey,
    admin: bool,
    phase: ConnPhase,
    deadline_us: u64,
    authenticated: bool,
    input: Vec<u8>,
    body: Vec<u8>,
    output: Vec<u8>,
    write_offset: usize,
    http_version_header: Option<String>,
    body_fixed_remaining: Option<usize>,
    body_chunked: bool,
    chunk_buf: Vec<u8>,
    request_id_json: Option<String>,
    session_idx: Option<usize>,
    stream_queue: Vec<u8>,
}

struct UpgradeState {
    active_program: Option<StoredProgram>,
    slot: Option<String>,
    base: [u8; 32],
    candidate: [u8; 32],
    migration: Option<String>,
    stage: Option<UploadStage>,
    touched_us: u64,
    job: Option<CompileJob>,
    compiled_result: Option<Result<crate::upgrade::UpgradeImage, (u16, &'static str, String)>>,
    waiting_conn: Option<usize>,
    waiting_req_id_json: Option<String>,
    executable: PathBuf,
    library: Option<PathBuf>,
    children: UpgradeChildren,
}

/// Complete live protocol server driving a [`ServiceLoop`] over primary TCP and optional admin
/// Unix domain socket listeners.
pub struct ProtocolServer {
    protocol: Rc<Image>,
    fns: ProtocolFunctions,
    service: ServiceLoop,
    listener: Listener,
    admin_listener: Option<Listener>,
    authorities: Vec<String>,
    admin_authorities: Vec<String>,
    catalog_json_primary: Value,
    catalog_json_admin: Value,
    descriptor_val: Value,
    descriptor_ty: u32,
    _catalog_heap: Heap,
    connections: Vec<Option<Connection>>,
    sessions: Vec<SessionSlot>,
    state_export: Option<ExportPin>,
    failure_export: Option<ExportPin>,
    rate_at: [u64; 2],
    rate_credit: [u64; 2],
    started_us: u64,
    now: ListenerTime,
    view_scratch_peak: usize,
    upgrade: Option<UpgradeState>,
    closing: bool,
    event_sink: Box<dyn FnMut(&ServiceEvent)>,
}

impl ProtocolServer {
    /// Creates a new [`ProtocolServer`] bound to `service`, primary `listener`, and allowed
    /// `Host` authorities (`mo_protocol_server_new`).
    pub fn new(
        service: ServiceLoop,
        listener: Listener,
        authorities: Vec<String>,
        admin: Option<AdminConfig>,
        event_sink: Box<dyn FnMut(&ServiceEvent)>,
    ) -> Result<Self, String> {
        let (protocol, fns) = build_protocol_image()?;
        {
            let mut auth_proc = Process::new(
                &protocol,
                Limits::default(),
                HostContext::stdio(Vec::new(), Vec::new()),
            );
            let mut list_items = VecDeque::with_capacity(authorities.len());
            for a in &authorities {
                let tv = auth_proc
                    .heap()
                    .alloc_text(a.clone())
                    .map_err(|_| "out of memory".to_string())?;
                list_items.push_back(tv);
            }
            let auth_list = auth_proc
                .heap()
                .alloc_list(list_items)
                .map_err(|_| "out of memory".to_string())?;
            let (out, _) = auth_proc.call_with_budget(
                fns.get(Pf::Authority),
                vec![auth_list],
                MO_LIMIT_MCP_REQUEST_WORK,
            );
            match out {
                Outcome::Returned(v) if v.as_bool() => {}
                _ => return Err("the server configuration was refused".to_string()),
            }
        }
        let now = listener.now();
        let (catalog_heap, descriptor_ty, descriptor_val, catalog_primary, catalog_admin) =
            prepare_catalogs(&protocol, &fns, service.image(), service.service_index())?;

        let (admin_listener, upgrade) = match admin {
            Some(cfg) => {
                let active_program = service.store().read_program().ok().flatten();
                let up = UpgradeState {
                    active_program,
                    slot: None,
                    base: [0u8; 32],
                    candidate: [0u8; 32],
                    migration: None,
                    stage: None,
                    touched_us: now.monotonic_us,
                    job: None,
                    compiled_result: None,
                    waiting_conn: None,
                    waiting_req_id_json: None,
                    executable: cfg.executable,
                    library: cfg.library,
                    children: cfg.children,
                };
                (Some(cfg.listener), Some(up))
            }
            None => (None, None),
        };

        let admin_authorities = vec!["localhost".to_string(), "127.0.0.1".to_string()];
        let cap = (2 * MO_LIMIT_MCP_CONNECTIONS) as usize;
        let mut connections = Vec::with_capacity(cap);
        for _ in 0..cap {
            connections.push(None);
        }

        let full_burst = MO_LIMIT_MCP_BURST * 1_000_000;
        Ok(Self {
            protocol: Rc::new(protocol),
            fns,
            service,
            listener,
            admin_listener,
            authorities,
            admin_authorities,
            catalog_json_primary: catalog_primary,
            catalog_json_admin: catalog_admin,
            descriptor_val,
            descriptor_ty,
            _catalog_heap: catalog_heap,
            connections,
            sessions: Vec::new(),
            state_export: None,
            failure_export: None,
            rate_at: [now.monotonic_us, now.monotonic_us],
            rate_credit: [full_burst, full_burst],
            started_us: now.monotonic_us,
            now,
            view_scratch_peak: 0,
            upgrade,
            closing: false,
            event_sink,
        })
    }

    /// Stops accepting new connections while allowing active requests to drain.
    pub fn stop_accepting(&mut self) {
        self.closing = true;
        self.listener.stop_accepting();
        if let Some(ref mut adm) = self.admin_listener {
            adm.stop_accepting();
        }
        // Close idle SSE streams so graceful shutdown finishes promptly.
        for slot in &mut self.connections {
            if let Some(c) = slot
                && c.phase == ConnPhase::Stream
                && c.stream_queue.is_empty()
            {
                let key = c.key;
                let is_admin = c.admin;
                *slot = None;
                if is_admin {
                    if let Some(ref mut adm) = self.admin_listener {
                        adm.close_connection(key);
                    }
                } else {
                    self.listener.close_connection(key);
                }
            }
        }
    }

    /// Returns `true` if any active connection remains open.
    #[must_use]
    pub fn has_connections(&self) -> bool {
        self.connections.iter().any(Option::is_some)
    }

    /// Returns `true` if the underlying service supervisor permanently stopped the service.
    #[must_use]
    pub fn service_stopped(&self) -> bool {
        self.service.is_stopped()
    }

    /// Returns `true` if the backing store entered an unsafe state.
    #[must_use]
    pub fn store_unsafe(&self) -> bool {
        self.service.store().is_unsafe()
    }

    /// Returns the current monotonic timestamp in microseconds from the server's clock.
    #[must_use]
    pub fn now_monotonic_us(&self) -> u64 {
        self.listener.now().monotonic_us
    }

    /// Runs one poll iteration up to `until_us` (`mo_protocol_server_poll`).
    #[cfg(unix)]
    pub fn poll_once(&mut self, until_us: u64) -> Result<(), String> {
        self.now = self.listener.now();

        // Advance timers before sleeping.
        if let Ok(events) = self
            .service
            .tick_timers(self.now.monotonic_us, self.now.wall_us)
        {
            for ev in &events {
                (self.event_sink)(ev);
            }
        }

        // Advance any active live upgrade compilation / slot expiry.
        self.poll_upgrade();

        // Expire idle export pins.
        if let Some(ref pin) = self.state_export
            && pin.is_expired(self.now.monotonic_us)
        {
            self.state_export = None;
        }
        if let Some(ref pin) = self.failure_export
            && pin.is_expired(self.now.monotonic_us)
        {
            self.failure_export = None;
        }

        // Compute wakeup deadline.
        let mut wake = until_us;
        if let Some(t) = self.service.next_timer_deadline_us() {
            wake = wake.min(t);
        }
        if self.upgrade.as_ref().is_some_and(|u| u.job.is_some()) {
            wake = wake.min(self.now.monotonic_us.saturating_add(10_000));
        }

        let mut primary_interests = Vec::new();
        let mut primary_indices = Vec::new();
        let mut admin_interests = Vec::new();
        let mut admin_indices = Vec::new();

        for (idx, slot) in self.connections.iter().enumerate() {
            let Some(c) = slot else {
                continue;
            };
            let events = match c.phase {
                ConnPhase::Headers | ConnPhase::Body => EVENT_READABLE,
                ConnPhase::Continue | ConnPhase::Write => EVENT_WRITABLE,
                ConnPhase::Stream => {
                    if c.stream_queue.is_empty() {
                        EVENT_READABLE
                    } else {
                        EVENT_READABLE | EVENT_WRITABLE
                    }
                }
                ConnPhase::UpgradeWait => EVENT_READABLE,
            };
            if c.admin {
                admin_interests.push(Interest { key: c.key, events });
                admin_indices.push(idx);
            } else {
                primary_interests.push(Interest { key: c.key, events });
                primary_indices.push(idx);
            }
        }

        let primary_req = WaitRequest {
            until_us: wake,
            accept: !self.closing && self.has_free_conn_slot(false),
            interests: primary_interests,
        };
        let admin_req = WaitRequest {
            until_us: wake,
            accept: !self.closing && self.has_free_conn_slot(true),
            interests: admin_interests,
        };

        let (p_ready, a_ready) = match &self.admin_listener {
            Some(adm) => wait_pair(&self.listener, &primary_req, Some((adm, &admin_req)))
                .map_err(|e| format!("poll failed: {e:?}"))?,
            None => wait_pair(&self.listener, &primary_req, None)
                .map_err(|e| format!("poll failed: {e:?}"))?,
        };
        self.now = p_ready.now;

        if p_ready.accept {
            self.accept_ready(false);
        }
        if a_ready.as_ref().is_some_and(|r| r.accept) {
            self.accept_ready(true);
        }

        for (i, &ev) in p_ready.events.iter().enumerate() {
            if ev != 0 {
                self.drive_connection(primary_indices[i], ev);
            }
        }
        if let Some(ar) = a_ready {
            for (i, &ev) in ar.events.iter().enumerate() {
                if ev != 0 {
                    self.drive_connection(admin_indices[i], ev);
                }
            }
        }

        // Expire timed-out connections.
        let now_us = self.now.monotonic_us;
        for idx in 0..self.connections.len() {
            let expired = self.connections[idx]
                .as_ref()
                .is_some_and(|c| c.phase != ConnPhase::Stream && now_us >= c.deadline_us);
            if expired {
                self.close_conn(idx);
            }
        }

        self.poll_upgrade();
        Ok(())
    }

    fn has_free_conn_slot(&self, admin: bool) -> bool {
        let half = MO_LIMIT_MCP_CONNECTIONS as usize;
        let range = if admin { half..2 * half } else { 0..half };
        self.connections[range].iter().any(Option::is_none)
    }

    fn accept_ready(&mut self, admin: bool) {
        let half = MO_LIMIT_MCP_CONNECTIONS as usize;
        let range = if admin { half..2 * half } else { 0..half };
        while let Some(slot_rel) = self.connections[range.clone()]
            .iter()
            .position(Option::is_none)
        {
            let slot_idx = range.start + slot_rel;
            let key = if admin {
                let Some(ref mut adm) = self.admin_listener else {
                    break;
                };
                match adm.accept() {
                    Ok(k) => k,
                    Err(ListenerStatus::Again | ListenerStatus::Full) => break,
                    Err(_) => break,
                }
            } else {
                match self.listener.accept() {
                    Ok(k) => k,
                    Err(ListenerStatus::Again | ListenerStatus::Full) => break,
                    Err(_) => break,
                }
            };
            self.connections[slot_idx] = Some(Connection {
                key,
                admin,
                phase: ConnPhase::Headers,
                deadline_us: self.now.monotonic_us.saturating_add(
                    MO_LIMIT_MCP_HEADER_US + MO_LIMIT_MCP_BODY_US + MO_LIMIT_MCP_WRITE_US,
                ),
                authenticated: false,
                input: Vec::new(),
                body: Vec::new(),
                output: Vec::new(),
                write_offset: 0,
                http_version_header: None,
                body_fixed_remaining: None,
                body_chunked: false,
                chunk_buf: Vec::new(),
                request_id_json: None,
                session_idx: None,
                stream_queue: Vec::new(),
            });
        }
    }

    fn close_conn(&mut self, idx: usize) {
        if let Some(c) = self.connections[idx].take() {
            if let Some(sidx) = c.session_idx
                && let Some(sess) = self.sessions.get_mut(sidx)
                && sess.stream_conn == Some(idx)
            {
                sess.stream_conn = None;
            }
            if let Some(ref mut up) = self.upgrade
                && up.waiting_conn == Some(idx)
            {
                up.waiting_conn = None;
            }
            if c.admin {
                if let Some(ref mut adm) = self.admin_listener {
                    adm.close_connection(c.key);
                }
            } else {
                self.listener.close_connection(c.key);
            }
        }
    }

    fn drive_connection(&mut self, idx: usize, ev: u32) {
        let Some(phase) = self.connections[idx].as_ref().map(|c| c.phase) else {
            return;
        };
        if ev & EVENT_ERROR != 0 && ev & (EVENT_READABLE | EVENT_WRITABLE) == 0 {
            self.close_conn(idx);
            return;
        }
        match phase {
            ConnPhase::Headers => self.read_headers(idx),
            ConnPhase::Continue => self.write_continue(idx),
            ConnPhase::Body => self.read_body(idx),
            ConnPhase::Write => self.write_response(idx),
            ConnPhase::Stream => self.drive_stream(idx, ev),
            ConnPhase::UpgradeWait => {
                // Check for peer hangup while waiting for compile child.
                if ev & EVENT_READABLE != 0 {
                    let mut probe = [0u8; 1];
                    let res = self.conn_read(idx, &mut probe);
                    if matches!(res, Err(ListenerStatus::Eof | ListenerStatus::Io)) {
                        self.close_conn(idx);
                    }
                }
            }
        }
    }

    fn conn_read(&mut self, idx: usize, buf: &mut [u8]) -> Result<usize, ListenerStatus> {
        let Some(c) = self.connections[idx].as_ref() else {
            return Err(ListenerStatus::Stale);
        };
        let (key, admin) = (c.key, c.admin);
        if admin {
            self.admin_listener
                .as_mut()
                .ok_or(ListenerStatus::Stale)?
                .read(key, buf)
        } else {
            self.listener.read(key, buf)
        }
    }

    fn conn_write(&mut self, idx: usize, buf: &[u8]) -> Result<usize, ListenerStatus> {
        let Some(c) = self.connections[idx].as_ref() else {
            return Err(ListenerStatus::Stale);
        };
        let (key, admin) = (c.key, c.admin);
        if admin {
            self.admin_listener
                .as_mut()
                .ok_or(ListenerStatus::Stale)?
                .write(key, buf)
        } else {
            self.listener.write(key, buf)
        }
    }

    fn read_headers(&mut self, idx: usize) {
        let mut buf = [0u8; 4096];
        loop {
            match self.conn_read(idx, &mut buf) {
                Ok(n) => {
                    let Some(c) = self.connections[idx].as_mut() else {
                        return;
                    };
                    if c.input.len() + n > MO_LIMIT_MCP_HEADER_BYTES as usize + 65_536 {
                        self.fail_http(idx, 431);
                        return;
                    }
                    c.input.extend_from_slice(&buf[..n]);
                    if let Some(hdr_end) = find_header_end(&c.input) {
                        if hdr_end + 4 > MO_LIMIT_MCP_HEADER_BYTES as usize {
                            self.fail_http(idx, 431);
                            return;
                        }
                        self.process_headers(idx, hdr_end + 4);
                        return;
                    }
                    if c.input.len() >= MO_LIMIT_MCP_HEADER_BYTES as usize {
                        self.fail_http(idx, 431);
                        return;
                    }
                }
                Err(ListenerStatus::Again) => return,
                Err(_) => {
                    self.close_conn(idx);
                    return;
                }
            }
        }
    }

    fn process_headers(&mut self, idx: usize, header_len: usize) {
        let Some(c) = self.connections[idx].as_ref() else {
            return;
        };
        let admin = c.admin;
        let head_bytes = c.input[..header_len].to_vec();
        let leftover = c.input[header_len..].to_vec();

        // Run HttpServer.parse_headers + HttpPolicy.inspect inside protocol VM.
        let mut proc = Process::new(
            &self.protocol,
            Limits {
                heap_size: MO_LIMIT_MCP_REQUEST_MEMORY as usize,
                ..Limits::default()
            },
            HostContext::stdio(Vec::new(), Vec::new()),
        );
        let Ok(byte_list) = alloc_byte_list(proc.heap(), &head_bytes) else {
            self.fail_http(idx, 413);
            return;
        };
        let ptypes = &self.protocol.file().types;
        let hdr_fn = &self.protocol.file().functions[self.fns.get(Pf::Headers) as usize];
        let limits_ty = hdr_fn.parameters[1].ty;
        let Ok(limits_val) = proc.heap().make_case(
            ptypes,
            limits_ty,
            0,
            vec![
                Value::Int(MO_LIMIT_MCP_HEADER_BYTES as i64),
                Value::Int(MO_LIMIT_MCP_LINE_BYTES as i64),
                Value::Int(MO_LIMIT_MCP_HEADER_FIELDS as i64),
            ],
        ) else {
            self.fail_http(idx, 500);
            return;
        };
        let (hdr_out, _) = proc.call_with_budget(
            self.fns.get(Pf::Headers),
            vec![byte_list.clone(), limits_val],
            MO_LIMIT_MCP_HEADER_WORK,
        );
        let head_val = match hdr_out {
            Outcome::Returned(v) if v.as_tag() == 0 => {
                let Some(inner) = choice_field(self.protocol.file(), hdr_fn.result_type, &v, 0)
                else {
                    self.fail_http(idx, 400);
                    return;
                };
                inner
            }
            Outcome::Returned(v) => {
                let prob = choice_field(self.protocol.file(), hdr_fn.result_type, &v, 0);
                let code = if prob.is_some_and(|p| p.as_tag() == 2) {
                    431
                } else {
                    400
                };
                self.fail_http(idx, code);
                return;
            }
            _ => {
                self.fail_http(idx, 400);
                return;
            }
        };

        // HttpPolicy.inspect(bytes, head, authorities)
        let allowed = if admin {
            &self.admin_authorities
        } else {
            &self.authorities
        };
        let Ok(auth_list) = alloc_text_list(proc.heap(), allowed) else {
            self.fail_http(idx, 500);
            return;
        };
        let inspect_fn = &self.protocol.file().functions[self.fns.get(Pf::Inspect) as usize];
        let (insp_out, _) = proc.call_with_budget(
            self.fns.get(Pf::Inspect),
            vec![byte_list.clone(), head_val.clone(), auth_list],
            MO_LIMIT_MCP_HEADER_WORK,
        );
        let access_val = match insp_out {
            Outcome::Returned(v) if v.as_tag() == 0 => {
                let Some(a) = choice_field(self.protocol.file(), inspect_fn.result_type, &v, 0)
                else {
                    self.fail_http(idx, 400);
                    return;
                };
                a
            }
            Outcome::Returned(v) => {
                let code = choice_field(self.protocol.file(), inspect_fn.result_type, &v, 0)
                    .map_or(400, |c| c.as_int() as i32);
                self.fail_http(idx, code);
                return;
            }
            _ => {
                self.fail_http(idx, 400);
                return;
            }
        };

        let access_ty = ptypes.get(inspect_fn.result_type).cases[0].fields[0].ty;
        let bearer_opt = choice_field(self.protocol.file(), access_ty, &access_val, 0);
        let session_opt = choice_field(self.protocol.file(), access_ty, &access_val, 1);
        let is_stream = choice_field(self.protocol.file(), access_ty, &access_val, 2)
            .is_some_and(|v| v.as_bool());

        // Authenticate bearer token (admin Unix socket is implicitly authenticated).
        let bearer_ty = ptypes.get(access_ty).cases[0].fields[0].ty;
        let bearer_slice: &[u8] = match bearer_opt {
            Some(ref opt) if opt.as_tag() == 1 => {
                extract_span_bytes(self.protocol.file(), bearer_ty, opt, &head_bytes).unwrap_or(&[])
            }
            _ => &[],
        };
        let authed = if admin {
            true
        } else {
            self.listener.authenticate(bearer_slice)
        };
        if !authed {
            self.fail_http(idx, 401);
            return;
        }

        // Check optional MCP-Session-Id header.
        let session_ty = ptypes.get(access_ty).cases[0].fields[1].ty;
        let mut session_idx = None;
        if let Some(ref opt) = session_opt
            && opt.as_tag() == 1
        {
            let Some(sid_bytes) =
                extract_span_bytes(self.protocol.file(), session_ty, opt, &head_bytes)
            else {
                self.fail_http(idx, 400);
                return;
            };
            let found = self
                .sessions
                .iter()
                .position(|s| s.live && s.admin == admin && s.id.as_bytes() == sid_bytes);
            let Some(sidx) = found else {
                self.fail_http(idx, 404);
                return;
            };
            session_idx = Some(sidx);
        }

        if let Some(c) = self.connections[idx].as_mut() {
            c.authenticated = true;
            c.session_idx = session_idx;
        }

        if is_stream {
            // GET /mcp SSE stream
            let (sp_out, _) = proc.call_with_budget(
                self.fns.get(Pf::StreamPlan),
                vec![byte_list, head_val],
                MO_LIMIT_MCP_REQUEST_WORK,
            );
            let refused = match sp_out {
                Outcome::Returned(v) => v.as_int() as i32,
                _ => 400,
            };
            if refused != 0 {
                self.fail_http(idx, refused);
                return;
            }
            let Some(sidx) = session_idx else {
                self.fail_http(idx, 400);
                return;
            };
            self.open_stream(idx, sidx);
            return;
        }

        // POST body plan: HttpPolicy.body_plan(bytes, head, MO_LIMIT_MCP_BODY_BYTES)
        let plan_fn = &self.protocol.file().functions[self.fns.get(Pf::Plan) as usize];
        let (plan_out, _) = proc.call_with_budget(
            self.fns.get(Pf::Plan),
            vec![
                byte_list,
                head_val,
                Value::Int(MO_LIMIT_MCP_BODY_BYTES as i64),
            ],
            MO_LIMIT_MCP_REQUEST_WORK,
        );
        let plan_val = match plan_out {
            Outcome::Returned(v) => v,
            _ => {
                self.fail_http(idx, 400);
                return;
            }
        };
        let ptag = plan_val.as_tag();
        if ptag == 0 {
            let code = choice_field(self.protocol.file(), plan_fn.result_type, &plan_val, 0)
                .map_or(400, |v| v.as_int() as i32);
            self.fail_http(idx, code);
            return;
        }

        let (fixed_len, send_continue, ver_field_idx) = if ptag == 1 {
            let len = choice_field(self.protocol.file(), plan_fn.result_type, &plan_val, 0)
                .map_or(0, |v| v.as_int().max(0) as usize);
            let sc = choice_field(self.protocol.file(), plan_fn.result_type, &plan_val, 1)
                .is_some_and(|v| v.as_bool());
            (Some(len), sc, 2usize)
        } else {
            let sc = choice_field(self.protocol.file(), plan_fn.result_type, &plan_val, 0)
                .is_some_and(|v| v.as_bool());
            (None, sc, 1usize)
        };
        let ver_opt_ty =
            ptypes.get(plan_fn.result_type).cases[ptag as usize].fields[ver_field_idx].ty;
        let ver_str = choice_field(
            self.protocol.file(),
            plan_fn.result_type,
            &plan_val,
            ver_field_idx,
        )
        .and_then(|opt| {
            if opt.as_tag() == 1 {
                choice_field(self.protocol.file(), ver_opt_ty, &opt, 0)
                    .map(|t| t.as_text().to_string())
            } else {
                None
            }
        });

        if let Some(c) = self.connections[idx].as_mut() {
            c.http_version_header = ver_str;
            c.body_fixed_remaining = fixed_len;
            c.body_chunked = fixed_len.is_none();
            c.input.clear();
            if send_continue {
                c.output = b"HTTP/1.1 100 Continue\r\n\r\n".to_vec();
                c.write_offset = 0;
                c.phase = ConnPhase::Continue;
                if c.body_chunked {
                    c.chunk_buf = leftover;
                } else {
                    c.body = leftover;
                }
                return;
            }
            c.phase = ConnPhase::Body;
        }

        if !leftover.is_empty() {
            self.feed_body_bytes(idx, &leftover);
        } else if fixed_len == Some(0) {
            self.receive_body(idx);
        }
    }

    fn write_continue(&mut self, idx: usize) {
        let Some(c) = self.connections[idx].as_ref() else {
            return;
        };
        let rem = c.output[c.write_offset..].to_vec();
        match self.conn_write(idx, &rem) {
            Ok(n) => {
                let done = {
                    let Some(c) = self.connections[idx].as_mut() else {
                        return;
                    };
                    c.write_offset += n;
                    c.write_offset >= c.output.len()
                };
                if done {
                    let buffered = {
                        let Some(c) = self.connections[idx].as_mut() else {
                            return;
                        };
                        c.output.clear();
                        c.write_offset = 0;
                        c.phase = ConnPhase::Body;
                        if c.body_chunked {
                            std::mem::take(&mut c.chunk_buf)
                        } else {
                            std::mem::take(&mut c.body)
                        }
                    };
                    if !buffered.is_empty() {
                        self.feed_body_bytes(idx, &buffered);
                    } else if self.connections[idx]
                        .as_ref()
                        .is_some_and(|c| c.body_fixed_remaining == Some(0))
                    {
                        self.receive_body(idx);
                    }
                }
            }
            Err(ListenerStatus::Again) => {}
            Err(_) => self.close_conn(idx),
        }
    }

    fn read_body(&mut self, idx: usize) {
        let mut buf = [0u8; 8192];
        loop {
            match self.conn_read(idx, &mut buf) {
                Ok(n) => {
                    self.feed_body_bytes(idx, &buf[..n]);
                    if self.connections[idx]
                        .as_ref()
                        .is_none_or(|c| c.phase != ConnPhase::Body)
                    {
                        return;
                    }
                }
                Err(ListenerStatus::Again) => return,
                Err(_) => {
                    self.close_conn(idx);
                    return;
                }
            }
        }
    }

    fn feed_body_bytes(&mut self, idx: usize, bytes: &[u8]) {
        let complete = {
            let Some(c) = self.connections[idx].as_mut() else {
                return;
            };
            if let Some(rem) = c.body_fixed_remaining {
                let take = bytes.len().min(rem);
                if c.body.len() + take > MO_LIMIT_MCP_BODY_BYTES as usize {
                    self.fail_http(idx, 413);
                    return;
                }
                c.body.extend_from_slice(&bytes[..take]);
                c.body_fixed_remaining = Some(rem - take);
                rem - take == 0
            } else {
                c.chunk_buf.extend_from_slice(bytes);
                match decode_chunked_incremental(&mut c.chunk_buf, &mut c.body) {
                    Ok(done) => done,
                    Err(code) => {
                        self.fail_http(idx, code);
                        return;
                    }
                }
            }
        };
        if complete {
            self.receive_body(idx);
        }
    }

    fn receive_body(&mut self, idx: usize) {
        let (body_bytes, version_opt, admin) = {
            let Some(c) = self.connections[idx].as_ref() else {
                return;
            };
            (c.body.clone(), c.http_version_header.clone(), c.admin)
        };

        let Some(marks) = body_marks(&body_bytes) else {
            self.fail_http(idx, 413);
            return;
        };
        if marks >= (MO_LIMIT_MCP_PAYLOAD_NODES + MO_LIMIT_MCP_ENVELOPE_NODES) as usize {
            self.fail_http(idx, 413);
            return;
        }

        let body_text = match std::str::from_utf8(&body_bytes) {
            Ok(s) => s.to_string(),
            Err(_) => "{".to_string(),
        };

        let protocol = Rc::clone(&self.protocol);
        let mut proc = Process::new(
            &protocol,
            Limits {
                heap_size: MO_LIMIT_MCP_VIEW_MEMORY as usize,
                ..Limits::default()
            },
            HostContext::stdio(Vec::new(), Vec::new()),
        );
        let ptypes = &protocol.file().types;
        let recv_fn = &protocol.file().functions[self.fns.get(Pf::Receive) as usize];
        let ver_opt_ty = recv_fn.parameters[1].ty;
        let ver_val = match version_opt {
            Some(v) => {
                let Ok(tv) = proc.heap().alloc_text(v) else {
                    self.fail_http(idx, 413);
                    return;
                };
                let Ok(opt) = proc.heap().make_case(ptypes, ver_opt_ty, 1, vec![tv]) else {
                    self.fail_http(idx, 413);
                    return;
                };
                opt
            }
            None => {
                let Ok(opt) = proc.heap().make_case(ptypes, ver_opt_ty, 0, Vec::new()) else {
                    self.fail_http(idx, 413);
                    return;
                };
                opt
            }
        };
        let Ok(body_val) = proc.heap().alloc_text(body_text) else {
            self.fail_http(idx, 413);
            return;
        };
        let Ok(mo_ver_val) = proc.heap().alloc_text(PROTOCOL_VERSION_TEXT.to_string()) else {
            self.fail_http(idx, 413);
            return;
        };
        let catalog_val = if admin {
            self.catalog_json_admin.clone()
        } else {
            self.catalog_json_primary.clone()
        };

        let (recv_out, _) = proc.call_with_budget(
            self.fns.get(Pf::Receive),
            vec![
                body_val,
                ver_val,
                self.descriptor_val.clone(),
                catalog_val,
                mo_ver_val,
                Value::Bool(admin),
            ],
            MO_LIMIT_MCP_REQUEST_WORK,
        );
        let action = match recv_out {
            Outcome::Returned(v) => v,
            _ => {
                self.fail_http(idx, 413);
                return;
            }
        };

        let action_ty = recv_fn.result_type;
        match action.as_tag() {
            0 => {
                // Action.Reply(status, body)
                let status = choice_field(self.protocol.file(), action_ty, &action, 0)
                    .map_or(200, |v| v.as_int() as i32);
                let body = choice_field(self.protocol.file(), action_ty, &action, 1)
                    .map_or_else(String::new, |v| v.as_text().to_string());
                self.respond_http_text(idx, &mut proc, status, &body);
            }
            3 => {
                // Action.Session(body)
                let body = choice_field(self.protocol.file(), action_ty, &action, 0)
                    .map_or_else(String::new, |v| v.as_text().to_string());
                let Some(sess_id) = self.open_session(admin) else {
                    self.fail_http(idx, 503);
                    return;
                };
                let (Ok(bv), Ok(sv)) = (
                    proc.heap().alloc_text(body),
                    proc.heap().alloc_text(sess_id),
                ) else {
                    self.fail_http(idx, 413);
                    return;
                };
                let (out, _) = proc.call_with_budget(
                    self.fns.get(Pf::HttpSession),
                    vec![bv, sv],
                    MO_LIMIT_MCP_REQUEST_WORK,
                );
                match out {
                    Outcome::Returned(v) => self.start_write(idx, v.as_text().as_bytes().to_vec()),
                    _ => self.fail_http(idx, 413),
                }
            }
            1 => {
                // Action.Call(request, tool, arguments)
                let Some(request) = choice_field(self.protocol.file(), action_ty, &action, 0)
                else {
                    self.fail_http(idx, 500);
                    return;
                };
                let tool_idx = choice_field(self.protocol.file(), action_ty, &action, 1)
                    .map_or(0, |v| v.as_int() as u32);
                let Some(args_json) = choice_field(self.protocol.file(), action_ty, &action, 2)
                else {
                    self.fail_http(idx, 500);
                    return;
                };
                let req_ty = ptypes.get(action_ty).cases[1].fields[0].ty;
                let id_json = choice_field(self.protocol.file(), req_ty, &request, 0);
                let id_text = choice_field(self.protocol.file(), req_ty, &request, 1)
                    .map(|v| v.as_text().to_string());
                if let Some(c) = self.connections[idx].as_mut() {
                    c.request_id_json = id_text;
                }
                if !self.take_rate_token(admin) {
                    self.send_fallback(idx, "rate_limited", "none");
                    return;
                }
                let Some(id_val) = id_json else {
                    self.fail_http(idx, 500);
                    return;
                };
                self.dispatch_service_tool(idx, &mut proc, id_val, tool_idx, args_json);
            }
            2 => {
                // Action.Operator(request, view, arguments, offset)
                let Some(request) = choice_field(self.protocol.file(), action_ty, &action, 0)
                else {
                    self.fail_http(idx, 500);
                    return;
                };
                let view_id = choice_field(self.protocol.file(), action_ty, &action, 1)
                    .map_or(0, |v| v.as_int());
                let Some(args_json) = choice_field(self.protocol.file(), action_ty, &action, 2)
                else {
                    self.fail_http(idx, 500);
                    return;
                };
                let offset = choice_field(self.protocol.file(), action_ty, &action, 3)
                    .map_or(0, |v| v.as_int());
                let req_ty = ptypes.get(action_ty).cases[2].fields[0].ty;
                let id_json = choice_field(self.protocol.file(), req_ty, &request, 0);
                let id_text = choice_field(self.protocol.file(), req_ty, &request, 1)
                    .map(|v| v.as_text().to_string());
                if let Some(c) = self.connections[idx].as_mut() {
                    c.request_id_json = id_text;
                }
                if !self.take_rate_token(admin) {
                    self.send_fallback(idx, "rate_limited", "none");
                    return;
                }
                let Some(id_val) = id_json else {
                    self.fail_http(idx, 500);
                    return;
                };

                // Check if this is an upgrade tool (mo-upgrade-open/page/commit).
                let (uk_out, _) = proc.call_with_budget(
                    self.fns.get(Pf::UpgradeKind),
                    vec![Value::Int(view_id)],
                    MO_LIMIT_MCP_REQUEST_WORK,
                );
                let upgrade_kind = match uk_out {
                    Outcome::Returned(v) => v.as_int(),
                    _ => 0,
                };
                if upgrade_kind != 0 {
                    self.dispatch_upgrade_tool(
                        idx,
                        &mut proc,
                        id_val,
                        upgrade_kind as u32,
                        args_json,
                    );
                    return;
                }
                self.dispatch_operator_view(idx, &mut proc, id_val, view_id, args_json, offset);
            }
            _ => self.fail_http(idx, 500),
        }
    }

    fn dispatch_service_tool(
        &mut self,
        idx: usize,
        proc: &mut Process<'_>,
        id_val: Value,
        tool_idx: u32,
        args_json: Value,
    ) {
        let pfile = self.protocol.file();
        let desc_kinds = choice_field(pfile, self.descriptor_ty, &self.descriptor_val, 0);
        let desc_tools = choice_field(pfile, self.descriptor_ty, &self.descriptor_val, 1);
        let (Some(kinds_val), Some(tools_val)) = (desc_kinds, desc_tools) else {
            self.send_fallback(idx, "internal", "none");
            return;
        };
        let Some(tool_desc) = tools_val.as_list().get(tool_idx as usize).cloned() else {
            self.send_fallback(idx, "invalid_arguments", "none");
            return;
        };
        let tool_desc_ty = pfile
            .types
            .get(pfile.types.get(self.descriptor_ty).cases[0].fields[1].ty)
            .parameters[0];
        let Some(tool_fields_desc) = choice_field(pfile, tool_desc_ty, &tool_desc, 3) else {
            self.send_fallback(idx, "internal", "none");
            return;
        };

        let arg_fn = &pfile.functions[self.fns.get(Pf::Arguments) as usize];
        let (dec_out, _) = proc.call_with_budget(
            self.fns.get(Pf::Arguments),
            vec![
                kinds_val.clone(),
                tool_fields_desc,
                args_json,
                Value::Int(MO_LIMIT_MCP_VALUE_DEPTH as i64),
                Value::Int(MO_LIMIT_MCP_BIGINT_DIGITS as i64),
            ],
            MO_LIMIT_MCP_REQUEST_WORK,
        );
        let decoded_list = match dec_out {
            Outcome::Returned(v) if v.as_tag() == 0 => {
                let Some(list) = choice_field(pfile, arg_fn.result_type, &v, 0) else {
                    self.send_fallback(idx, "invalid_arguments", "none");
                    return;
                };
                list
            }
            Outcome::Returned(_) => {
                self.send_fallback(idx, "invalid_arguments", "none");
                return;
            }
            _ => {
                self.send_fallback(idx, "work_limit", "none");
                return;
            }
        };

        let list_ty = pfile.types.get(arg_fn.result_type).cases[0].fields[0].ty;
        let value_ty = pfile.types.get(list_ty).parameters[0];

        // Unpack each protocol Value into service heap and encode to MOSV\x01.
        let svc_file = self.service.file();
        let svc_def = self.service.service_def();
        let Some(tool_def) = svc_def.tools.get(tool_idx as usize) else {
            self.send_fallback(idx, "invalid_arguments", "none");
            return;
        };
        let arg_items: Vec<Value> = decoded_list.as_list().iter().cloned().collect();
        if arg_items.len() != tool_def.argument_types.len() {
            self.send_fallback(idx, "invalid_arguments", "none");
            return;
        }
        let temp_svc_heap = Heap::new(self.service.limits().heap_size);
        let mut work_left = MO_LIMIT_MCP_REQUEST_WORK;
        let mut encoded_args = Vec::with_capacity(arg_items.len());
        {
            let mut bridge = ValueBridge {
                protocol: &pfile.types,
                service: &svc_file.types,
                descriptor: svc_def,
                value_ty,
                work: Some(&mut work_left),
                node_limit: 0,
            };
            for (val, &aty) in arg_items.iter().zip(&tool_def.argument_types) {
                match bridge.unpack(&temp_svc_heap, aty, val, MO_LIMIT_MCP_BODY_BYTES as usize) {
                    Ok(sv) => {
                        let mut enc = Vec::new();
                        if mo_heap::saved::encode_value(&svc_file.types, aty, &sv, &mut enc)
                            .is_err()
                        {
                            self.send_fallback(idx, "memory_limit", "none");
                            return;
                        }
                        encoded_args.push(enc);
                    }
                    Err(ValueBridgeStatus::Invalid) => {
                        self.send_fallback(idx, "invalid_arguments", "none");
                        return;
                    }
                    Err(ValueBridgeStatus::Memory) => {
                        self.send_fallback(idx, "memory_limit", "none");
                        return;
                    }
                    Err(ValueBridgeStatus::Limit | ValueBridgeStatus::Ok) => {
                        self.send_fallback(idx, "work_limit", "none");
                        return;
                    }
                }
            }
        }

        // Invoke service tool synchronously at the service boundary.
        let res = self.service.call_tool(
            tool_idx,
            encoded_args,
            self.now.wall_us,
            self.now.monotonic_us,
        );
        for ev in &res.events {
            (self.event_sink)(ev);
        }

        let effect = if res.updated { "applied" } else { "none" };
        if res.status != ServiceStatus::Ok {
            let err_code = match res.status {
                ServiceStatus::Stopped => "stopped",
                ServiceStatus::StoreIo => "store_failed",
                ServiceStatus::Memory => "memory_limit",
                ServiceStatus::Full => "busy",
                ServiceStatus::Invalid => "invalid_arguments",
                _ => "handler_failed",
            };
            let eff = if res.status == ServiceStatus::StoreIo {
                "unknown"
            } else {
                effect
            };
            self.send_fallback(idx, err_code, eff);
            return;
        }

        let Some(reply_enc) = res.reply_encoded else {
            self.send_fallback(idx, "internal", effect);
            return;
        };

        // Decode reply into temp heap, unwrap Result / (State, Reply), pack to protocol Value, encode JSON!
        let svc_file = self.service.file();
        let svc_def = self.service.service_def();
        let tool_def = &svc_def.tools[tool_idx as usize];
        let mut cur_ty = tool_def.result_type;
        let Ok(mut cur_val) = decode_value(&temp_svc_heap, &svc_file.types, cur_ty, &reply_enc)
        else {
            self.send_fallback(idx, "response_limit", effect);
            return;
        };

        let is_result = svc_file.types.get(cur_ty).kind == TypeKind::Choice
            && svc_file
                .types
                .get(cur_ty)
                .name
                .as_deref()
                .is_some_and(|n| n.starts_with("Result("));
        let failed = is_result && cur_val.as_tag() == 1;
        if is_result {
            let tag = cur_val.as_tag();
            let Some(inner) = choice_field(svc_file, cur_ty, &cur_val, 0) else {
                self.send_fallback(idx, "internal", effect);
                return;
            };
            cur_ty = svc_file.types.get(cur_ty).cases[tag as usize].fields[0].ty;
            cur_val = inner;
        }
        if !failed && !tool_def.read_only {
            let mo_heap::HeapObj::Tuple { fields, .. } = cur_val.as_heap_ref().obj() else {
                self.send_fallback(idx, "internal", effect);
                return;
            };
            cur_val = fields[1].clone();
            cur_ty = svc_file.types.get(cur_ty).parameters[1];
        }

        let declared_tid = if failed {
            let err_opt_ty = pfile.types.get(tool_desc_ty).cases[0].fields[5].ty;
            choice_field(pfile, tool_desc_ty, &tool_desc, 5)
                .and_then(|opt| choice_field(pfile, err_opt_ty, &opt, 0))
                .unwrap_or(Value::Int(0))
        } else {
            choice_field(pfile, tool_desc_ty, &tool_desc, 4).unwrap_or(Value::Int(0))
        };

        let mut work_pack = MO_LIMIT_MCP_REQUEST_WORK;
        let packed = {
            let mut bridge = ValueBridge {
                protocol: &pfile.types,
                service: &svc_file.types,
                descriptor: svc_def,
                value_ty,
                work: Some(&mut work_pack),
                node_limit: MO_LIMIT_MCP_RESPONSE_NODES as usize,
            };
            match bridge.pack(
                proc.heap(),
                cur_ty,
                &cur_val,
                MO_LIMIT_MCP_RESPONSE_BYTES as usize,
            ) {
                Ok(v) => v,
                Err(_) => {
                    self.send_fallback(idx, "response_limit", effect);
                    return;
                }
            }
        };

        let enc_fn = &pfile.functions[self.fns.get(Pf::Encode) as usize];
        let (enc_out, _) = proc.call_with_budget(
            self.fns.get(Pf::Encode),
            vec![
                kinds_val,
                declared_tid,
                packed,
                Value::Int(MO_LIMIT_MCP_VALUE_DEPTH as i64),
            ],
            MO_LIMIT_MCP_REQUEST_WORK,
        );
        let json_val = match enc_out {
            Outcome::Returned(v) if v.as_tag() == 0 => {
                let Some(j) = choice_field(pfile, enc_fn.result_type, &v, 0) else {
                    self.send_fallback(idx, "response_limit", effect);
                    return;
                };
                j
            }
            _ => {
                self.send_fallback(idx, "response_limit", effect);
                return;
            }
        };

        let tr_fn = &pfile.functions[self.fns.get(Pf::ToolReply) as usize];
        let (tr_out, _) = proc.call_with_budget(
            self.fns.get(Pf::ToolReply),
            vec![
                id_val,
                json_val,
                Value::Bool(failed),
                Value::Int(MO_LIMIT_MCP_RESPONSE_BYTES as i64),
            ],
            MO_LIMIT_MCP_REQUEST_WORK,
        );
        let body_text = match tr_out {
            Outcome::Returned(v) if v.as_tag() == 1 => {
                let Some(b) = choice_field(pfile, tr_fn.result_type, &v, 0) else {
                    self.send_fallback(idx, "response_limit", effect);
                    return;
                };
                b.as_text().to_string()
            }
            _ => {
                self.send_fallback(idx, "response_limit", effect);
                return;
            }
        };
        self.respond_http_text(idx, proc, 200, &body_text);
    }

    fn dispatch_operator_view(
        &mut self,
        idx: usize,
        proc: &mut Process<'_>,
        id_val: Value,
        view_id: i64,
        args_json: Value,
        offset: i64,
    ) {
        let mut refused = false;
        let json_opt = if view_id == 7 {
            self.build_export_view(proc, args_json, offset, &mut refused)
        } else if view_id == 8 {
            self.build_failures_view(proc, args_json)
        } else {
            self.build_observation_view(proc, view_id, args_json, offset)
        };
        let Some(json_val) = json_opt else {
            self.send_fallback(idx, "memory_limit", "none");
            return;
        };

        let pfile = self.protocol.file();
        let op_fn = &pfile.functions[self.fns.get(Pf::OperatorReply) as usize];
        let (rep_out, _) = proc.call_with_budget(
            self.fns.get(Pf::OperatorReply),
            vec![
                id_val,
                json_val,
                Value::Bool(refused),
                Value::Int(MO_LIMIT_MCP_RESPONSE_BYTES as i64),
            ],
            MO_LIMIT_MCP_REQUEST_WORK,
        );
        let scratch_peak = proc.heap().budget().scratch_peak();
        if scratch_peak > self.view_scratch_peak {
            self.view_scratch_peak = scratch_peak;
        }
        let body_text = match rep_out {
            Outcome::Returned(v) if v.as_tag() == 1 => {
                let Some(b) = choice_field(pfile, op_fn.result_type, &v, 0) else {
                    self.send_fallback(idx, "response_limit", "none");
                    return;
                };
                b.as_text().to_string()
            }
            _ => {
                self.send_fallback(idx, "response_limit", "none");
                return;
            }
        };
        self.respond_http_text(idx, proc, 200, &body_text);
    }

    fn build_observation_view(
        &mut self,
        proc: &mut Process<'_>,
        view_id: i64,
        args_json: Value,
        offset: i64,
    ) -> Option<Value> {
        let pfile = self.protocol.file();
        let ptypes = &pfile.types;
        let view_fn = &pfile.functions[self.fns.get(Pf::View) as usize];
        let obs_ty = view_fn.parameters[2].ty;
        let stats_ty = ptypes.get(obs_ty).cases[0].fields[4].ty;
        let msg_list_ty = ptypes.get(obs_ty).cases[0].fields[5].ty;
        let msg_ty = ptypes.get(msg_list_ty).parameters[0];
        let timer_list_ty = ptypes.get(obs_ty).cases[0].fields[6].ty;
        let timer_ty = ptypes.get(timer_list_ty).parameters[0];
        let ev_list_ty = ptypes.get(obs_ty).cases[0].fields[7].ty;
        let ev_ty = ptypes.get(ev_list_ty).parameters[0];

        let svc_file = self.service.file();
        let svc_def = self.service.service_def();
        let mod_name = svc_file.string(svc_file.modules[svc_def.module as usize].name);
        let svc_name = bounded_view_name(mod_name);
        let program_num = svc_file.program_checksum.to_string();
        let build_hex = hex_encode(&self.service.build_digest());
        let build_label = self.service.build_label().to_string();

        let (retained_failures, evicted_kinds) = match self.service.store().failure_stats() {
            Ok((c, e)) => (i64::from(c), e),
            Err(_) => (-1i64, 0u64),
        };
        let pinned_export_bytes = self
            .state_export
            .as_ref()
            .map_or(0, |p| p.bytes.len().saturating_add(64))
            + self
                .failure_export
                .as_ref()
                .map_or(0, |p| p.bytes.len().saturating_add(64));

        let elapsed_us = self.now.monotonic_us.saturating_sub(self.started_us);
        let diagnostic_scratch = self
            .view_scratch_peak
            .max(self.service.metrics().diagnostic_scratch_peak)
            .max(self.service.encoded_state().len())
            .max(64);
        let (state_heap, _) = self.service.state_value();
        let heap_used = state_heap.budget().used();
        let heap_peak = state_heap
            .budget()
            .peak()
            .max(self.service.metrics().handler_heap_peak);
        let heap_limit = self.service.limits().heap_size;
        let stack_limit = self.service.limits().stack_size;

        let h = proc.heap();
        let stats_val = h
            .make_case(
                ptypes,
                stats_ty,
                0,
                vec![
                    Value::Int(self.service.sequence() as i64),
                    Value::Int(self.service.snapshot_sequence() as i64),
                    Value::Int(self.service.encoded_sequence() as i64),
                    h.alloc_text(elapsed_us.to_string()).ok()?,
                    Value::Int(heap_used as i64),
                    Value::Int(heap_peak as i64),
                    Value::Int(heap_limit as i64),
                    Value::Int(stack_limit as i64),
                    Value::Int(self.service.encoded_state().len() as i64),
                    Value::Int(diagnostic_scratch as i64),
                    Value::Int(self.service.supervisor().failures.len() as i64),
                    Value::Int(self.service.supervisor().restarts.len() as i64),
                    Value::Bool(!self.service.is_stopped()),
                    Value::Bool(true),
                    Value::Int(MO_LIMIT_SERVICE_FAILURE_WINDOW_US as i64),
                    Value::Int(MO_LIMIT_SERVICE_RESTART_WINDOW_US as i64),
                    Value::Int(self.service.timers().len() as i64),
                    Value::Int(stack_limit as i64),
                    Value::Int(pinned_export_bytes as i64),
                    Value::Int(retained_failures),
                    h.alloc_text(evicted_kinds.to_string()).ok()?,
                ],
            )
            .ok()?;

        // Mailbox (always empty at boundary since we execute synchronously).
        let mailbox_val = h.alloc_list(VecDeque::new()).ok()?;

        // Timers page.
        let timer_count = self.service.timers().len();
        let first = (offset.max(0) as usize).min(timer_count);
        let last = timer_count.min(first + MO_LIMIT_MCP_VIEW_TIMERS as usize);
        let mut timer_vals = VecDeque::with_capacity(last - first);
        for i in first..last {
            let slot = self.service.timers()[i];
            let tname = bounded_view_name(svc_file.function_name(svc_def.timers[i].function));
            let tv = h
                .make_case(
                    ptypes,
                    timer_ty,
                    0,
                    vec![
                        h.alloc_text(tname).ok()?,
                        h.alloc_text(slot.interval_us.to_string()).ok()?,
                        h.alloc_text(slot.next_due_us.to_string()).ok()?,
                        Value::Bool(false),
                    ],
                )
                .ok()?;
            timer_vals.push_back(tv);
        }
        let timers_val = h.alloc_list(timer_vals).ok()?;

        // Events.
        let mut ev_vals = VecDeque::with_capacity(self.service.event_log().len());
        for ev in self.service.event_log() {
            let ev_val = h
                .make_case(
                    ptypes,
                    ev_ty,
                    0,
                    vec![
                        h.alloc_text(ev.number.to_string()).ok()?,
                        h.alloc_text(ev.kind.to_string()).ok()?,
                        h.alloc_text(ev.reason.clone()).ok()?,
                        h.alloc_text(bounded_view_name(&ev.handler_name)).ok()?,
                        Value::Int(ev.wall_us),
                        h.alloc_text(ev.elapsed_us.to_string()).ok()?,
                        h.alloc_text(ev.record_id.to_string()).ok()?,
                        h.alloc_text(ev.record_unavailable.clone()).ok()?,
                    ],
                )
                .ok()?;
            ev_vals.push_back(ev_val);
        }
        let events_val = h.alloc_list(ev_vals).ok()?;

        // Recent log.
        let mut log_vals = VecDeque::with_capacity(self.service.recent_log().len());
        for entry in self.service.recent_log() {
            let arg_total: usize = entry.argument_bytes.iter().map(|&n| n as usize).sum();
            let lv = h
                .make_case(
                    ptypes,
                    msg_ty,
                    0,
                    vec![
                        h.alloc_text(bounded_view_name(&entry.name)).ok()?,
                        Value::Bool(entry.timer),
                        Value::Int(entry.arrival),
                        h.alloc_text(entry.sequence.to_string()).ok()?,
                        Value::Int(arg_total as i64),
                        Value::Bool(true),
                    ],
                )
                .ok()?;
            log_vals.push_back(lv);
        }
        let log_val = h.alloc_list(log_vals).ok()?;

        let source_hex = self
            .upgrade
            .as_ref()
            .and_then(|u| u.active_program.as_ref())
            .map(|p| hex_encode(&p.source))
            .unwrap_or_default();

        let obs_val = h
            .make_case(
                ptypes,
                obs_ty,
                0,
                vec![
                    h.alloc_text(svc_name).ok()?,
                    h.alloc_text(program_num).ok()?,
                    h.alloc_text(build_hex).ok()?,
                    h.alloc_text(build_label).ok()?,
                    stats_val,
                    mailbox_val,
                    timers_val,
                    events_val,
                    log_val,
                    h.alloc_text(elapsed_us.to_string()).ok()?,
                    h.alloc_text(source_hex).ok()?,
                ],
            )
            .ok()?;

        let state_bytes = if view_id == 1 {
            self.service.encoded_state().to_vec()
        } else {
            Vec::new()
        };
        let saved_val = h.alloc_bytes(state_bytes).ok()?;
        let (out, _) = proc.call_with_budget(
            self.fns.get(Pf::View),
            vec![
                Value::Int(view_id),
                args_json,
                obs_val,
                saved_val,
                Value::Int(MO_LIMIT_MCP_VIEW_BYTES as i64),
            ],
            MO_LIMIT_MCP_REQUEST_WORK,
        );
        match out {
            Outcome::Returned(v) => Some(v),
            _ => None,
        }
    }

    fn build_export_view(
        &mut self,
        proc: &mut Process<'_>,
        args_json: Value,
        offset: i64,
        refused: &mut bool,
    ) -> Option<Value> {
        let (id_out, _) = proc.call_with_budget(
            self.fns.get(Pf::ExportId),
            vec![args_json.clone()],
            MO_LIMIT_MCP_REQUEST_WORK,
        );
        let requested_id = match id_out {
            Outcome::Returned(v) => v.as_text().to_string(),
            _ => return None,
        };
        let (fid_out, _) = proc.call_with_budget(
            self.fns.get(Pf::FailureId),
            vec![args_json],
            MO_LIMIT_MCP_REQUEST_WORK,
        );
        let failure_id = match fid_out {
            Outcome::Returned(v) => v.as_int().max(0) as u64,
            _ => return None,
        };

        let now_us = self.now.monotonic_us;
        let wall_us = self.now.wall_us;
        let mut error = "";

        if requested_id.is_empty() {
            let mut rnd = [0u8; 16];
            if getrandom::fill(&mut rnd).is_err() {
                error = "identity_unavailable";
            } else {
                let id = ExportIdentity {
                    kind: if failure_id == 0 { 1 } else { 2 },
                    id: rnd,
                    build: self.service.build_digest(),
                    label: self.service.build_label().to_string(),
                    service: self.service.service_index(),
                    program: self.service.file().program_checksum,
                    sequence: self.service.sequence(),
                    encoded_sequence: self.service.encoded_sequence(),
                    failure_id,
                    created: wall_us,
                };
                if failure_id != 0 {
                    match self.service.store().failure_get(failure_id) {
                        Ok(Some(f)) => {
                            let mut fid_ident = id;
                            fid_ident.sequence = f.state.sequence;
                            fid_ident.encoded_sequence = f.state.sequence;
                            if let Some(pin) = create_failure_export_pin(
                                &fid_ident,
                                &f.state.bytes,
                                &f.message.bytes,
                                now_us,
                            ) {
                                self.failure_export = Some(pin);
                            } else {
                                error = "failure_unavailable";
                            }
                        }
                        _ => error = "failure_unavailable",
                    }
                } else {
                    let saved = self.service.encoded_state();
                    if let Some(pin) = create_export_pin(&id, saved, now_us) {
                        self.state_export = Some(pin);
                    } else {
                        error = "export_limit";
                    }
                }
            }
        } else {
            let pin_ref = if failure_id == 0 {
                self.state_export.as_ref()
            } else {
                self.failure_export.as_ref()
            };
            match pin_ref {
                None => error = "export_released",
                Some(pin) => {
                    if hex_encode(&pin.id) != requested_id || pin.failure_id != failure_id {
                        error = "export_replaced";
                    }
                }
            }
        }

        let mut page_bytes = Vec::new();
        let mut meta_fields: Vec<String> = Vec::new();
        let mut complete = false;

        if error.is_empty() {
            let pin_opt = if failure_id == 0 {
                self.state_export.as_mut()
            } else {
                self.failure_export.as_mut()
            };
            let Some(pin) = pin_opt else {
                error = "export_released";
                *refused = true;
                return self.invoke_export_reply(proc, error, &meta_fields, &page_bytes);
            };
            if offset < 0 {
                error = "invalid_offset";
            } else if let Some((len, count)) = export_page_bounds(pin.bytes.len(), offset as usize)
            {
                let off = offset as usize;
                let slice = &pin.bytes[off..off + len];
                let hash = fnv1a(FNV_OFFSET, slice);
                let csum_hex = hex_encode(&hash.to_be_bytes());
                complete = off + len == pin.bytes.len();
                meta_fields = vec![
                    hex_encode(&pin.id),
                    pin.sequence.to_string(),
                    pin.encoded_sequence.to_string(),
                    off.to_string(),
                    pin.bytes.len().to_string(),
                    count.to_string(),
                    csum_hex,
                    if complete {
                        hex_encode(&pin.digest)
                    } else {
                        String::new()
                    },
                    if failure_id == 0 {
                        "state".to_string()
                    } else {
                        "failure".to_string()
                    },
                    failure_id.to_string(),
                ];
                page_bytes = slice.to_vec();
                pin.touched = now_us;
                let _ = MO_LIMIT_EXPORT_PAGE_BYTES;
            } else {
                error = "invalid_offset";
            }
        }

        *refused = !error.is_empty();
        let res = self.invoke_export_reply(proc, error, &meta_fields, &page_bytes);
        if res.is_some() && error.is_empty() && complete {
            if failure_id == 0 {
                self.state_export = None;
            } else {
                self.failure_export = None;
            }
        }
        res
    }

    fn invoke_export_reply(
        &self,
        proc: &mut Process<'_>,
        error: &str,
        meta_fields: &[String],
        page_bytes: &[u8],
    ) -> Option<Value> {
        let h = proc.heap();
        let err_val = h.alloc_text(error.to_string()).ok()?;
        let list_val = alloc_text_list(h, meta_fields).ok()?;
        let bytes_val = h.alloc_bytes(page_bytes.to_vec()).ok()?;
        let (out, _) = proc.call_with_budget(
            self.fns.get(Pf::Export),
            vec![err_val, list_val, bytes_val],
            MO_LIMIT_MCP_REQUEST_WORK,
        );
        match out {
            Outcome::Returned(v) => Some(v),
            _ => None,
        }
    }

    fn build_failures_view(&self, proc: &mut Process<'_>, args_json: Value) -> Option<Value> {
        let (after_out, _) = proc.call_with_budget(
            self.fns.get(Pf::FailureAfter),
            vec![args_json.clone()],
            MO_LIMIT_MCP_REQUEST_WORK,
        );
        let after = match after_out {
            Outcome::Returned(v) => v.as_int().max(0) as u64,
            _ => return None,
        };

        let (entries, omitted) = self.service.store().failure_catalog(after).ok()?;
        let mut flat: Vec<String> = Vec::new();
        let classes = ["defect", "work", "memory", "state_size"];

        for f in &entries {
            if let Some(rec) = read_failure_record(&f.message.bytes).filter(|r| r.id == f.id) {
                flat.push(f.id.to_string());
                flat.push(f.kind.clone());
                flat.push(rec.name);
                flat.push(rec.arrival.to_string());
                flat.push(f.state_bytes.to_string());
                flat.push(f.message.bytes.len().to_string());
                flat.push(rec.signature_version.to_string());
                flat.push(
                    classes
                        .get(rec.class as usize)
                        .copied()
                        .unwrap_or("defect")
                        .to_string(),
                );
                flat.push(rec.reason);
                flat.push(rec.limit);
                flat.push(rec.limit_value.to_string());
                flat.push(rec.function.to_string());
                flat.push(rec.file.to_string());
                flat.push(rec.start.to_string());
                flat.push(rec.end.to_string());
            } else {
                flat.push("0".to_string());
                flat.push(format!("record {} unreadable", f.id));
                flat.push(String::new());
                flat.push(String::new());
                flat.push("0".to_string());
                flat.push("0".to_string());
                for _ in 6..15 {
                    flat.push(String::new());
                }
            }
        }
        if omitted != 0 {
            flat.push("-1".to_string());
            flat.push(omitted.to_string());
            for _ in 2..15 {
                flat.push(String::new());
            }
        }

        let list_val = alloc_text_list(proc.heap(), &flat).ok()?;
        let (out, _) = proc.call_with_budget(
            self.fns.get(Pf::FailureView),
            vec![args_json, list_val],
            MO_LIMIT_MCP_REQUEST_WORK,
        );
        match out {
            Outcome::Returned(v) => Some(v),
            _ => None,
        }
    }

    fn dispatch_upgrade_tool(
        &mut self,
        idx: usize,
        proc: &mut Process<'_>,
        id_val: Value,
        kind: u32,
        args_json: Value,
    ) {
        // Parse 8 normalized fields via McpViews.upgrade_arguments(args_json).
        let (ua_out, _) = proc.call_with_budget(
            self.fns.get(Pf::UpgradeArguments),
            vec![args_json],
            MO_LIMIT_MCP_REQUEST_WORK,
        );
        let fields: Vec<String> = match ua_out {
            Outcome::Returned(v) => v
                .as_list()
                .iter()
                .map(|x| x.as_text().to_string())
                .collect(),
            _ => {
                self.send_fallback(idx, "invalid_arguments", "none");
                return;
            }
        };
        if fields.len() != 8 {
            self.send_fallback(idx, "invalid_arguments", "none");
            return;
        }
        let (base_s, cand_s, total_s, mig_s, slot_s, _index_s, offset_s, bytes_s) = (
            &fields[0], &fields[1], &fields[2], &fields[3], &fields[4], &fields[5], &fields[6],
            &fields[7],
        );

        let now_us = self.now.monotonic_us;
        let Some(ref mut up) = self.upgrade else {
            let mut r = UpgradeReport::default();
            r.refuse(
                ErrorCode::UpgradeSourceMismatch as u16,
                "upgrade",
                "live upgrades are not enabled on this server",
            );
            self.send_upgrade_report(idx, proc, id_val, &r, None, true);
            return;
        };

        let mut report = UpgradeReport::default();
        if let Some(ref active) = up.active_program {
            report.base = Some(active.source);
            report.ordinal = active.ordinal;
        }
        report.head = self.service.sequence();

        match kind {
            1 => {
                // mo-upgrade-open
                if !report.check_children(up.children) {
                    self.send_upgrade_report(idx, proc, id_val, &report, None, true);
                    return;
                }
                if up.job.is_some() {
                    report.refuse(
                        ErrorCode::UpgradeBusy as u16,
                        "busy",
                        "another upgrade commit is already in progress",
                    );
                    self.send_upgrade_report(idx, proc, id_val, &report, None, true);
                    return;
                }
                let (Some(base), Some(cand), Ok(total)) = (
                    hex_decode_exact::<32>(base_s),
                    hex_decode_exact::<32>(cand_s),
                    total_s.parse::<usize>(),
                ) else {
                    report.refuse(
                        ErrorCode::UpgradeInvalidValue as u16,
                        "arguments",
                        "invalid upgrade open arguments",
                    );
                    self.send_upgrade_report(idx, proc, id_val, &report, None, true);
                    return;
                };
                report.candidate = Some(cand);
                if total == 0 || total as u64 > MO_LIMIT_BUNDLE_BYTES {
                    report.refuse(
                        ErrorCode::UpgradeBundleSize as u16,
                        "bundle",
                        "candidate bundle size exceeds MO_LIMIT_BUNDLE_BYTES",
                    );
                    self.send_upgrade_report(idx, proc, id_val, &report, None, true);
                    return;
                }
                let Some(ref active) = up.active_program else {
                    report.refuse(
                        ErrorCode::UpgradeSourceMismatch as u16,
                        "source",
                        "service has no recorded program; upgrade offline first",
                    );
                    self.send_upgrade_report(idx, proc, id_val, &report, None, true);
                    return;
                };
                // Check `already_active` BEFORE stale-base so retrying an already-landed push
                // with its original `--base` returns `already_active` (`check_upgrade_live.py:456`).
                if active.source == cand {
                    report.already_active = true;
                    report.activated = true;
                    report.class = "compatible".to_string();
                    report.message = "candidate source is already active".to_string();
                    self.send_upgrade_report(idx, proc, id_val, &report, None, false);
                    return;
                }
                if active.source != base {
                    report.refuse(
                        ErrorCode::UpgradeStaleBase as u16,
                        "stale_base",
                        format!(
                            "base is not the active program {}",
                            hex_encode(&active.source)
                        ),
                    );
                    self.send_upgrade_report(idx, proc, id_val, &report, None, true);
                    return;
                }
                let Some(stage) = UploadStage::open(total) else {
                    report.refuse(
                        ErrorCode::UpgradeBundleSize as u16,
                        "bundle",
                        "candidate bundle size is invalid",
                    );
                    self.send_upgrade_report(idx, proc, id_val, &report, None, true);
                    return;
                };
                let mut slot_bytes = [0u8; 16];
                let _ = getrandom::fill(&mut slot_bytes);
                let slot_hex = hex_encode(&slot_bytes);
                let mig = (!mig_s.is_empty()).then(|| mig_s.clone());
                up.slot = Some(slot_hex.clone());
                up.base = base;
                up.candidate = cand;
                up.migration = mig.clone();
                up.stage = Some(stage);
                up.compiled_result = None;
                up.touched_us = now_us;

                let json = format!(
                    "{{\"version\":1,\"status\":\"open\",\"slot\":\"{slot_hex}\",\"base_source\":\"{}\",\"candidate_source\":\"{}\",\"total\":\"{total}\"}}",
                    hex_encode(&base),
                    hex_encode(&cand)
                );
                let text = format!("upgrade upload slot {slot_hex} opened ({total} bytes)");
                self.send_upgrade_raw(idx, proc, id_val, &json, &text, false);
            }
            2 => {
                // mo-upgrade-page
                if up.slot.as_deref() != Some(slot_s.as_str()) {
                    report.refuse(
                        ErrorCode::UpgradeInvalidValue as u16,
                        "slot",
                        "unknown or expired upgrade upload slot",
                    );
                    self.send_upgrade_report(idx, proc, id_val, &report, None, true);
                    return;
                }
                let (Ok(offset), Some(data)) =
                    (offset_s.parse::<usize>(), decode_base64_vec(bytes_s))
                else {
                    report.refuse(
                        ErrorCode::UpgradeInvalidValue as u16,
                        "page",
                        "invalid upgrade page offset or bytes",
                    );
                    self.send_upgrade_report(idx, proc, id_val, &report, None, true);
                    return;
                };
                let Some(ref mut stage) = up.stage else {
                    report.refuse(
                        ErrorCode::UpgradeInvalidValue as u16,
                        "slot",
                        "upload slot has no active stage",
                    );
                    self.send_upgrade_report(idx, proc, id_val, &report, None, true);
                    return;
                };
                match stage.push_page(offset, &data) {
                    UploadPageStatus::Accepted | UploadPageStatus::Duplicate => {
                        up.touched_us = now_us;
                        let recvd = stage.received();
                        let total = stage.expected_total();
                        let complete = stage.is_complete();
                        if complete && up.job.is_none() && up.compiled_result.is_none() {
                            let bundle = stage.bytes().to_vec();
                            if sha256_bytes(&bundle) == up.candidate
                                && let Some(ref active) = up.active_program
                            {
                                let old_bundle =
                                    up.migration.as_ref().map(|_| active.bundle.as_slice());
                                if let Ok(job) = CompileJob::start(
                                    &up.executable,
                                    up.library.as_deref(),
                                    bundle,
                                    old_bundle,
                                    up.migration.as_deref(),
                                    Some(active.context.as_slice()),
                                    true,
                                    self.service.build_digest(),
                                    active.ordinal + 1,
                                ) {
                                    up.job = Some(job);
                                }
                            }
                        }
                        let json = format!(
                            "{{\"version\":1,\"status\":\"accepted\",\"slot\":\"{slot_s}\",\"received\":\"{recvd}\",\"total\":\"{total}\",\"complete\":{complete}}}"
                        );
                        let text = format!("received {recvd}/{total} bytes");
                        self.send_upgrade_raw(idx, proc, id_val, &json, &text, false);
                    }
                    UploadPageStatus::Invalid => {
                        report.refuse(
                            ErrorCode::UpgradeInvalidValue as u16,
                            "page",
                            "upgrade page offset, size, or ordering is invalid",
                        );
                        self.send_upgrade_report(idx, proc, id_val, &report, None, true);
                    }
                }
            }
            3 => {
                // mo-upgrade-commit
                if up.slot.as_deref() != Some(slot_s.as_str()) {
                    report.refuse(
                        ErrorCode::UpgradeInvalidValue as u16,
                        "slot",
                        "unknown or expired upgrade upload slot",
                    );
                    self.send_upgrade_report(idx, proc, id_val, &report, None, true);
                    return;
                }
                if up.waiting_conn.is_some() {
                    report.refuse(
                        ErrorCode::UpgradeBusy as u16,
                        "busy",
                        "upgrade commit is already running",
                    );
                    self.send_upgrade_report(idx, proc, id_val, &report, None, true);
                    return;
                }
                let Some(ref stage) = up.stage else {
                    report.refuse(
                        ErrorCode::UpgradeInvalidValue as u16,
                        "slot",
                        "upload slot is empty",
                    );
                    self.send_upgrade_report(idx, proc, id_val, &report, None, true);
                    return;
                };
                if !stage.is_complete() {
                    up.slot = None;
                    up.stage = None;
                    up.migration = None;
                    up.compiled_result = None;
                    report.refuse(
                        ErrorCode::UpgradeInvalidValue as u16,
                        "bundle",
                        "uploaded candidate bundle is incomplete",
                    );
                    self.send_upgrade_report(idx, proc, id_val, &report, None, true);
                    return;
                }
                let bundle = stage.bytes().to_vec();
                let mig_copy = up.migration.clone();
                let actual_digest = sha256_bytes(&bundle);
                report.candidate = Some(actual_digest);
                if actual_digest != up.candidate {
                    up.slot = None;
                    up.stage = None;
                    up.migration = None;
                    up.compiled_result = None;
                    report.refuse(
                        ErrorCode::UpgradeInvalidValue as u16,
                        "bundle",
                        "uploaded bundle digest does not match declared candidate digest",
                    );
                    self.send_upgrade_report(idx, proc, id_val, &report, mig_copy.as_deref(), true);
                    return;
                }
                let Some(ref active) = up.active_program else {
                    up.slot = None;
                    up.stage = None;
                    up.migration = None;
                    up.compiled_result = None;
                    report.refuse(
                        ErrorCode::UpgradeSourceMismatch as u16,
                        "source",
                        "no active program recorded",
                    );
                    self.send_upgrade_report(idx, proc, id_val, &report, mig_copy.as_deref(), true);
                    return;
                };
                if active.source != up.base {
                    up.slot = None;
                    up.stage = None;
                    up.migration = None;
                    up.compiled_result = None;
                    report.refuse(
                        ErrorCode::UpgradeStaleBase as u16,
                        "stale_base",
                        format!(
                            "base is not the active program {}",
                            hex_encode(&active.source)
                        ),
                    );
                    self.send_upgrade_report(idx, proc, id_val, &report, mig_copy.as_deref(), true);
                    return;
                }

                // Start compile job if not already started on last page (or consume ready result).
                if up.job.is_none() && up.compiled_result.is_none() {
                    let old_bundle = up.migration.as_ref().map(|_| active.bundle.as_slice());
                    let job = match CompileJob::start(
                        &up.executable,
                        up.library.as_deref(),
                        bundle,
                        old_bundle,
                        up.migration.as_deref(),
                        Some(active.context.as_slice()),
                        true,
                        self.service.build_digest(),
                        active.ordinal + 1,
                    ) {
                        Ok(j) => j,
                        Err(msg) => {
                            up.slot = None;
                            up.stage = None;
                            up.migration = None;
                            report.refuse(
                                ErrorCode::UpgradeCompileDiagnostic as u16,
                                "compile",
                                msg,
                            );
                            self.send_upgrade_report(
                                idx,
                                proc,
                                id_val,
                                &report,
                                mig_copy.as_deref(),
                                true,
                            );
                            return;
                        }
                    };
                    up.job = Some(job);
                }

                up.waiting_conn = Some(idx);
                up.waiting_req_id_json = self.connections[idx]
                    .as_ref()
                    .and_then(|c| c.request_id_json.clone());
                if let Some(c) = self.connections[idx].as_mut() {
                    c.phase = ConnPhase::UpgradeWait;
                    c.deadline_us = now_us.saturating_add(120_000_000);
                }
                if up.compiled_result.is_some() {
                    self.poll_upgrade();
                }
            }
            _ => self.send_fallback(idx, "invalid_arguments", "none"),
        }
    }

    fn poll_upgrade(&mut self) {
        let now_us = self.now.monotonic_us;
        let poll_res = {
            let Some(ref mut up) = self.upgrade else {
                return;
            };
            if up.job.is_none()
                && up.compiled_result.is_none()
                && up.slot.is_some()
                && now_us.saturating_sub(up.touched_us) >= MO_LIMIT_UPGRADE_SLOT_IDLE_US
            {
                up.slot = None;
                up.stage = None;
                up.migration = None;
            }
            if let Some(ref mut job) = up.job {
                match job.poll() {
                    CompilePoll::Running => return,
                    CompilePoll::Finished(res) => {
                        up.job = None;
                        up.compiled_result = Some(*res);
                    }
                }
            }
            // Wait until `mo-upgrade-commit` has attached a waiting connection before activating!
            if up.waiting_conn.is_none() {
                return;
            }
            let Some(res) = up.compiled_result.take() else {
                return;
            };
            Box::new(res)
        };

        // Take ownership of the finished job and waiting connection.
        let (wait_conn, req_id_str, migration, active_prog, candidate_digest, unconfined) = {
            let Some(ref mut up) = self.upgrade else {
                return;
            };
            up.job = None;
            let wc = up.waiting_conn.take();
            let rid = up.waiting_req_id_json.take();
            let mig = up.migration.take();
            let act = up.active_program.clone();
            let cand = up.candidate;
            let unc = up.children == UpgradeChildren::Unconfined;
            up.slot = None;
            up.stage = None;
            (wc, rid, mig, act, cand, unc)
        };

        let mut report = UpgradeReport {
            unconfined,
            candidate: Some(candidate_digest),
            head: self.service.sequence(),
            ..UpgradeReport::default()
        };
        if let Some(ref act) = active_prog {
            report.base = Some(act.source);
            report.ordinal = act.ordinal;
        }

        let mut activated_prog: Option<StoredProgram> = None;

        match *poll_res {
            Err((code, class, msg)) => {
                report.refuse(code, class, msg);
            }
            Ok(cand_img) => {
                let Some(active) = active_prog else {
                    report.refuse(
                        ErrorCode::UpgradeSourceMismatch as u16,
                        "source",
                        "no active program recorded",
                    );
                    self.finish_upgrade_wait(
                        wait_conn,
                        req_id_str.as_deref(),
                        &report,
                        migration.as_deref(),
                    );
                    return;
                };
                let old_meta = program_metadata(&active);
                let next_meta = program_metadata(&cand_img.program);
                let old_state_type_name = self
                    .service
                    .file()
                    .types
                    .get(self.service.service_def().state_type)
                    .name
                    .as_deref();
                if compare_programs(
                    &active,
                    old_meta.as_ref(),
                    &cand_img.program,
                    next_meta.as_ref(),
                    migration.as_deref(),
                    &mut report,
                ) && let Some(migrated_bytes) = run_migration_state(
                    &cand_img.image,
                    cand_img.service,
                    migration.as_deref(),
                    old_state_type_name,
                    self.service.encoded_state(),
                    true,
                    cand_img.schema_synthesized,
                    &mut report,
                ) {
                    let head = self.service.sequence();
                    let prev_snap = StoreRecord {
                        kind: StoreRecordKind::State,
                        sequence: head,
                        image: self.service.image_key(),
                        bytes: self.service.encoded_state().to_vec(),
                    };
                    let next_snap = StoreRecord {
                        kind: StoreRecordKind::State,
                        sequence: head,
                        image: cand_img.program.image,
                        bytes: migrated_bytes.clone(),
                    };
                    report.activated = true;
                    report.ordinal = cand_img.program.ordinal;
                    report.head = head;
                    report.message = format!(
                        "activated ordinal {} at sequence {}",
                        cand_img.program.ordinal, head
                    );
                    let report_json = report.to_json(migration.as_deref());
                    let activation = StoreActivation {
                        previous: active.clone(),
                        candidate: cand_img.program.clone(),
                        previous_state: prev_snap,
                        candidate_state: next_snap,
                        migration: migration.clone().unwrap_or_default(),
                        report_json,
                    };

                    match self.service.prepare_swap(
                        cand_img.image,
                        cand_img.service,
                        migrated_bytes,
                        now_us,
                    ) {
                        Err(_) => {
                            report.activated = false;
                            report.refuse(
                                ErrorCode::UpgradeInvalidValue as u16,
                                "swap",
                                "failed to prepare candidate service state",
                            );
                        }
                        Ok(prepared) => match self.service.store_mut().activate(&activation) {
                            ActivationStatus::Activated => {
                                self.service.finish_swap(prepared);
                                // Clear pinned exports across image swap (`protocol-upgrade.c:584`).
                                self.state_export = None;
                                self.failure_export = None;
                                // Rebuild cached MCP catalogs for the new program!
                                if let Ok((ch, dty, dval, cp, ca)) = prepare_catalogs(
                                    &self.protocol,
                                    &self.fns,
                                    self.service.image(),
                                    self.service.service_index(),
                                ) {
                                    self._catalog_heap = ch;
                                    self.descriptor_ty = dty;
                                    self.descriptor_val = dval;
                                    self.catalog_json_primary = cp;
                                    self.catalog_json_admin = ca;
                                }
                                activated_prog = Some(cand_img.program);
                                let elapsed = now_us.saturating_sub(self.started_us);
                                self.service.push_event_summary(ServiceEventSummary {
                                    number: 0,
                                    kind: "upgrade",
                                    reason: format!(
                                        "accepted ordinal {} from {} to {}; answered 0; dropped 0",
                                        report.ordinal,
                                        hex_encode(&active.source),
                                        hex_encode(&candidate_digest)
                                    ),
                                    handler_name: String::new(),
                                    wall_us: self.now.wall_us,
                                    elapsed_us: elapsed,
                                    record_id: 0,
                                    record_unavailable: String::new(),
                                });
                                self.broadcast_list_changed();
                            }
                            ActivationStatus::Refused => {
                                report.activated = false;
                                report.refuse(
                                    ErrorCode::UpgradeSourceMismatch as u16,
                                    "store",
                                    "store refused activation transaction",
                                );
                            }
                            ActivationStatus::Unsafe => {
                                report.activated = false;
                                report.refuse(
                                    ErrorCode::UpgradeSourceMismatch as u16,
                                    "store",
                                    "store activation commit outcome is uncertain",
                                );
                            }
                        },
                    }
                }
            }
        }

        if let Some(new_prog) = activated_prog
            && let Some(ref mut up) = self.upgrade
        {
            up.active_program = Some(new_prog);
        }

        self.finish_upgrade_wait(
            wait_conn,
            req_id_str.as_deref(),
            &report,
            migration.as_deref(),
        );
    }

    fn finish_upgrade_wait(
        &mut self,
        wait_conn: Option<usize>,
        req_id_str: Option<&str>,
        report: &UpgradeReport,
        migration: Option<&str>,
    ) {
        let Some(idx) = wait_conn else {
            return;
        };
        if self.connections[idx].is_none() {
            return;
        }
        let json = report.to_json(migration);
        let text = report.to_text();
        let refused = report.code != 0;
        let id_raw = req_id_str.unwrap_or("1");
        let mut rpc = String::with_capacity(json.len() + text.len() + 256);
        rpc.push_str("{\"jsonrpc\":\"2.0\",\"id\":");
        rpc.push_str(id_raw);
        rpc.push_str(",\"result\":{\"content\":[{\"type\":\"text\",\"text\":");
        crate::binary::push_json_string(&mut rpc, text.trim_end_matches('\n'));
        rpc.push_str("}],\"structuredContent\":");
        rpc.push_str(&json);
        rpc.push_str(",\"isError\":");
        rpc.push_str(if refused { "true" } else { "false" });
        rpc.push_str("}}");
        let http = format!(
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: {}\r\n\r\n{}",
            rpc.len(),
            rpc
        );
        self.start_write(idx, http.into_bytes());
    }

    fn send_upgrade_report(
        &mut self,
        idx: usize,
        proc: &mut Process<'_>,
        id_val: Value,
        report: &UpgradeReport,
        migration: Option<&str>,
        refused: bool,
    ) {
        let json = report.to_json(migration);
        let text = report.to_text();
        self.send_upgrade_raw(
            idx,
            proc,
            id_val,
            &json,
            text.trim_end_matches('\n'),
            refused,
        );
    }

    fn send_upgrade_raw(
        &mut self,
        idx: usize,
        proc: &mut Process<'_>,
        id_val: Value,
        json: &str,
        text: &str,
        refused: bool,
    ) {
        let pfile = self.protocol.file();
        let ur_fn = &pfile.functions[self.fns.get(Pf::UpgradeReply) as usize];
        let (Ok(jv), Ok(tv)) = (
            proc.heap().alloc_text(json.to_string()),
            proc.heap().alloc_text(text.to_string()),
        ) else {
            self.send_fallback(idx, "response_limit", "none");
            return;
        };
        let (out, _) = proc.call_with_budget(
            self.fns.get(Pf::UpgradeReply),
            vec![
                id_val,
                jv,
                tv,
                Value::Bool(refused),
                Value::Int(MO_LIMIT_MCP_RESPONSE_BYTES as i64),
            ],
            MO_LIMIT_MCP_REQUEST_WORK,
        );
        match out {
            Outcome::Returned(v) if v.as_tag() == 1 => {
                if let Some(body) = choice_field(pfile, ur_fn.result_type, &v, 0) {
                    let s = body.as_text().to_string();
                    self.respond_http_text(idx, proc, 200, &s);
                } else {
                    self.send_fallback(idx, "response_limit", "none");
                }
            }
            _ => self.send_fallback(idx, "response_limit", "none"),
        }
    }

    fn open_session(&mut self, admin: bool) -> Option<String> {
        if self.sessions.iter().filter(|s| s.live).count() >= MO_LIMIT_MCP_SESSIONS as usize {
            return None;
        }
        let mut rnd = [0u8; (MO_LIMIT_MCP_SESSION_ID_BYTES / 2) as usize];
        let _ = getrandom::fill(&mut rnd);
        let id = hex_encode(&rnd);
        self.sessions.push(SessionSlot {
            id: id.clone(),
            admin,
            live: true,
            stream_conn: None,
        });
        Some(id)
    }

    fn open_stream(&mut self, idx: usize, sidx: usize) {
        let protocol = Rc::clone(&self.protocol);
        let mut proc = Process::new(
            &protocol,
            Limits::default(),
            HostContext::stdio(Vec::new(), Vec::new()),
        );
        let (out, _) = proc.call_with_budget(
            self.fns.get(Pf::StreamHead),
            Vec::new(),
            MO_LIMIT_MCP_REQUEST_WORK,
        );
        let head = match out {
            Outcome::Returned(v) => v.as_text().as_bytes().to_vec(),
            _ => b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n".to_vec(),
        };
        if let Some(sess) = self.sessions.get_mut(sidx)
            && let Some(old_idx) = sess.stream_conn.replace(idx)
        {
            self.close_conn(old_idx);
        }
        if let Some(c) = self.connections[idx].as_mut() {
            c.phase = ConnPhase::Stream;
            c.stream_queue = head;
        }
    }

    fn broadcast_list_changed(&mut self) {
        let protocol = Rc::clone(&self.protocol);
        let mut proc = Process::new(
            &protocol,
            Limits::default(),
            HostContext::stdio(Vec::new(), Vec::new()),
        );
        let (out, _) = proc.call_with_budget(
            self.fns.get(Pf::ListChanged),
            Vec::new(),
            MO_LIMIT_MCP_REQUEST_WORK,
        );
        let event_bytes = match out {
            Outcome::Returned(v) => v.as_text().as_bytes().to_vec(),
            _ => b"data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/list_changed\"}\n\n"
                .to_vec(),
        };
        for slot in &mut self.connections {
            if let Some(c) = slot
                && c.phase == ConnPhase::Stream
                && c.stream_queue.len() + event_bytes.len() <= MO_LIMIT_MCP_STREAM_BYTES as usize
            {
                c.stream_queue.extend_from_slice(&event_bytes);
            }
        }
    }

    fn drive_stream(&mut self, idx: usize, ev: u32) {
        if ev & EVENT_WRITABLE != 0 {
            let chunk = self.connections[idx]
                .as_ref()
                .map(|c| c.stream_queue.clone())
                .unwrap_or_default();
            if !chunk.is_empty() {
                match self.conn_write(idx, &chunk) {
                    Ok(n) => {
                        if let Some(c) = self.connections[idx].as_mut() {
                            c.stream_queue.drain(..n);
                        }
                    }
                    Err(ListenerStatus::Again) => {}
                    Err(_) => {
                        self.close_conn(idx);
                        return;
                    }
                }
            }
        }
        if ev & EVENT_READABLE != 0 {
            let mut buf = [0u8; 64];
            match self.conn_read(idx, &mut buf) {
                Ok(_) | Err(ListenerStatus::Again) => {}
                Err(_) => self.close_conn(idx),
            }
        }
    }

    fn respond_http_text(&mut self, idx: usize, proc: &mut Process<'_>, status: i32, body: &str) {
        if body.len() as u64 > MO_LIMIT_MCP_RESPONSE_BYTES {
            self.fail_http(idx, 413);
            return;
        }
        let Ok(bv) = proc.heap().alloc_text(body.to_string()) else {
            self.fail_http(idx, 500);
            return;
        };
        let (out, _) = proc.call_with_budget(
            self.fns.get(Pf::Http),
            vec![Value::Int(i64::from(status)), bv],
            MO_LIMIT_MCP_REQUEST_WORK,
        );
        match out {
            Outcome::Returned(v) => self.start_write(idx, v.as_text().as_bytes().to_vec()),
            _ => self.fail_http(idx, 500),
        }
    }

    fn fail_http(&mut self, idx: usize, status: i32) {
        let protocol = Rc::clone(&self.protocol);
        let mut proc = Process::new(
            &protocol,
            Limits::default(),
            HostContext::stdio(Vec::new(), Vec::new()),
        );
        let Ok(empty) = proc.heap().alloc_text(String::new()) else {
            self.close_conn(idx);
            return;
        };
        let (out, _) = proc.call_with_budget(
            self.fns.get(Pf::Http),
            vec![Value::Int(i64::from(status)), empty],
            MO_LIMIT_MCP_REQUEST_WORK,
        );
        let bytes = match out {
            Outcome::Returned(v) => v.as_text().as_bytes().to_vec(),
            _ => {
                format!("HTTP/1.1 {status} Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
                    .into_bytes()
            }
        };
        self.start_write(idx, bytes);
    }

    fn send_fallback(&mut self, idx: usize, error_code: &str, effect: &str) {
        let id_str = self.connections[idx]
            .as_ref()
            .and_then(|c| c.request_id_json.clone())
            .unwrap_or_else(|| "null".to_string());
        let protocol = Rc::clone(&self.protocol);
        let mut proc = Process::new(
            &protocol,
            Limits::default(),
            HostContext::stdio(Vec::new(), Vec::new()),
        );
        let (Ok(ev), Ok(efv)) = (
            proc.heap().alloc_text(error_code.to_string()),
            proc.heap().alloc_text(effect.to_string()),
        ) else {
            self.fail_http(idx, 500);
            return;
        };
        let (out, _) = proc.call_with_budget(
            self.fns.get(Pf::Fallback),
            vec![ev, efv],
            MO_LIMIT_MCP_REQUEST_WORK,
        );
        let body = match out {
            Outcome::Returned(v) => match v.as_heap_ref().obj() {
                mo_heap::HeapObj::Tuple { fields, .. } if fields.len() == 2 => {
                    format!("{}{}{}", fields[0].as_text(), id_str, fields[1].as_text())
                }
                _ => format!(
                    "{{\"jsonrpc\":\"2.0\",\"id\":{id_str},\"result\":{{\"isError\":true,\"content\":[{{\"type\":\"text\",\"text\":\"{error_code}\"}}],\"structuredContent\":{{\"error\":{{\"kind\":\"runtime\",\"code\":\"{error_code}\",\"effect\":\"{effect}\"}}}}}}}}"
                ),
            },
            _ => format!(
                "{{\"jsonrpc\":\"2.0\",\"id\":{id_str},\"result\":{{\"isError\":true,\"content\":[{{\"type\":\"text\",\"text\":\"{error_code}\"}}],\"structuredContent\":{{\"error\":{{\"kind\":\"runtime\",\"code\":\"{error_code}\",\"effect\":\"{effect}\"}}}}}}}}"
            ),
        };
        self.respond_http_text(idx, &mut proc, 200, &body);
    }

    fn start_write(&mut self, idx: usize, bytes: Vec<u8>) {
        if let Some(c) = self.connections[idx].as_mut() {
            c.output = bytes;
            c.write_offset = 0;
            c.phase = ConnPhase::Write;
        }
        self.write_response(idx);
    }

    fn write_response(&mut self, idx: usize) {
        let Some(c) = self.connections[idx].as_ref() else {
            return;
        };
        let rem = c.output[c.write_offset..].to_vec();
        match self.conn_write(idx, &rem) {
            Ok(n) => {
                let done = {
                    let Some(c) = self.connections[idx].as_mut() else {
                        return;
                    };
                    c.write_offset += n;
                    c.write_offset >= c.output.len()
                };
                if done {
                    let Some(c) = self.connections[idx].as_ref() else {
                        return;
                    };
                    let (key, admin) = (c.key, c.admin);
                    if admin {
                        if let Some(ref mut adm) = self.admin_listener {
                            let _ = adm.half_close(key);
                        }
                    } else {
                        let _ = self.listener.half_close(key);
                    }
                    self.close_conn(idx);
                }
            }
            Err(ListenerStatus::Again) => {}
            Err(_) => self.close_conn(idx),
        }
    }

    fn take_rate_token(&mut self, admin: bool) -> bool {
        let b = usize::from(admin);
        let now = self.now.monotonic_us;
        let elapsed = now.saturating_sub(self.rate_at[b]);
        let full = MO_LIMIT_MCP_BURST * 1_000_000;
        self.rate_at[b] = now;
        let added = elapsed.saturating_mul(MO_LIMIT_MCP_RATE);
        self.rate_credit[b] = self.rate_credit[b].saturating_add(added).min(full);
        if self.rate_credit[b] < 1_000_000 {
            return false;
        }
        self.rate_credit[b] -= 1_000_000;
        true
    }
}

fn prepare_catalogs(
    protocol: &Image,
    fns: &ProtocolFunctions,
    service_image: &Image,
    service_idx: u32,
) -> Result<(Heap, u32, Value, Value, Value), String> {
    let mut proc = Process::new(
        protocol,
        Limits {
            heap_size: MO_LIMIT_MCP_CATALOG_MEMORY as usize,
            ..Limits::default()
        },
        HostContext::stdio(Vec::new(), Vec::new()),
    );
    let desc = build_descriptor(
        protocol.file(),
        fns,
        proc.heap(),
        service_image.file(),
        service_idx,
    )
    .ok_or_else(|| "failed to build protocol service descriptor".to_string())?;
    let key = service_image.file().program_checksum ^ (u64::from(service_idx) << 32);
    let key_hex = format!("{key:016x}");
    let key_val = proc
        .heap()
        .alloc_text(key_hex)
        .map_err(|_| "out of memory preparing catalog".to_string())?;

    let prep_fn = &protocol.file().functions[fns.get(Pf::Prepare) as usize];
    let (p_out, _) = proc.call_with_budget(
        fns.get(Pf::Prepare),
        vec![
            desc.value.clone(),
            key_val.clone(),
            Value::Int(MO_LIMIT_MCP_RESPONSE_BYTES as i64),
            Value::Bool(false),
        ],
        MO_LIMIT_MCP_REQUEST_WORK * 4,
    );
    let primary_cat = match p_out {
        Outcome::Returned(v) if v.as_tag() == 1 => {
            choice_field(protocol.file(), prep_fn.result_type, &v, 0)
                .ok_or_else(|| "missing primary catalog".to_string())?
        }
        _ => return Err("Mcp.prepare(false) failed".to_string()),
    };

    let (a_out, _) = proc.call_with_budget(
        fns.get(Pf::Prepare),
        vec![
            desc.value.clone(),
            key_val,
            Value::Int(MO_LIMIT_MCP_RESPONSE_BYTES as i64),
            Value::Bool(true),
        ],
        MO_LIMIT_MCP_REQUEST_WORK * 4,
    );
    let admin_cat = match a_out {
        Outcome::Returned(v) if v.as_tag() == 1 => {
            choice_field(protocol.file(), prep_fn.result_type, &v, 0)
                .ok_or_else(|| "missing admin catalog".to_string())?
        }
        _ => return Err("Mcp.prepare(true) failed".to_string()),
    };

    proc.heap().make_immortal(&desc.value);
    proc.heap().make_immortal(&primary_cat);
    proc.heap().make_immortal(&admin_cat);

    // Keep heap alive via a fresh handle (immortal nodes stay alive as long as Value clones live).
    Ok((Heap::new(64), desc.ty, desc.value, primary_cat, admin_cat))
}

fn find_header_end(buf: &[u8]) -> Option<usize> {
    buf.windows(4).position(|w| w == b"\r\n\r\n")
}

fn alloc_byte_list(heap: &Heap, bytes: &[u8]) -> Result<Value, mo_heap::HeapError> {
    let items: VecDeque<Value> = bytes.iter().copied().map(Value::Byte).collect();
    heap.alloc_list(items)
}

fn alloc_text_list(heap: &Heap, strings: &[String]) -> Result<Value, mo_heap::HeapError> {
    let mut items = VecDeque::with_capacity(strings.len());
    for s in strings {
        items.push_back(heap.alloc_text(s.clone())?);
    }
    heap.alloc_list(items)
}

fn extract_span_bytes<'a>(
    file: &BytecodeFile,
    opt_ty: u32,
    opt_val: &Value,
    buf: &'a [u8],
) -> Option<&'a [u8]> {
    let span = choice_field(file, opt_ty, opt_val, 0)?;
    let span_ty = file.types.get(opt_ty).cases[1].fields[0].ty;
    let start = choice_field(file, span_ty, &span, 0)?.as_int();
    let size = choice_field(file, span_ty, &span, 1)?.as_int();
    if start < 0 || size < 0 {
        return None;
    }
    let s = start as usize;
    let n = size as usize;
    buf.get(s..s.checked_add(n)?)
}

fn bounded_view_name(name: &str) -> String {
    if name.len() as u64 > MO_LIMIT_MCP_VIEW_NAME_BYTES {
        "<name unavailable: exceeds inspection byte limit>".to_string()
    } else {
        name.to_string()
    }
}

fn decode_base64_vec(text: &str) -> Option<Vec<u8>> {
    let bytes = text.as_bytes();
    if !bytes.len().is_multiple_of(4) {
        return None;
    }
    let val = |c: u8| -> Option<u8> {
        match c {
            b'A'..=b'Z' => Some(c - b'A'),
            b'a'..=b'z' => Some(c - b'a' + 26),
            b'0'..=b'9' => Some(c - b'0' + 52),
            b'+' => Some(62),
            b'/' => Some(63),
            _ => None,
        }
    };
    let mut out = Vec::with_capacity((bytes.len() / 4) * 3);
    for (ci, chunk) in bytes.chunks_exact(4).enumerate() {
        let is_last = (ci + 1) * 4 == bytes.len();
        let a = val(chunk[0])?;
        let b = val(chunk[1])?;
        if chunk[2] == b'=' {
            if !is_last || chunk[3] != b'=' || (b & 0x0f) != 0 {
                return None;
            }
            out.push((a << 2) | (b >> 4));
        } else if chunk[3] == b'=' {
            if !is_last {
                return None;
            }
            let c = val(chunk[2])?;
            if (c & 0x03) != 0 {
                return None;
            }
            out.push((a << 2) | (b >> 4));
            out.push((b << 4) | (c >> 2));
        } else {
            let c = val(chunk[2])?;
            let d = val(chunk[3])?;
            out.push((a << 2) | (b >> 4));
            out.push((b << 4) | (c >> 2));
            out.push((c << 6) | d);
        }
    }
    Some(out)
}

fn decode_chunked_incremental(buf: &mut Vec<u8>, out: &mut Vec<u8>) -> Result<bool, i32> {
    let mut chunks = 0u64;
    let mut pos = 0usize;
    loop {
        let Some(rel_crlf) = buf[pos..].windows(2).position(|w| w == b"\r\n") else {
            if buf.len() - pos > MO_LIMIT_MCP_LINE_BYTES as usize {
                return Err(400);
            }
            if pos > 0 {
                buf.drain(..pos);
            }
            return Ok(false);
        };
        let line = &buf[pos..pos + rel_crlf];
        if line.len() > MO_LIMIT_MCP_LINE_BYTES as usize {
            return Err(400);
        }
        let size_part = line.split(|&b| b == b';').next().unwrap_or(&[]);
        let size_str = std::str::from_utf8(size_part).map_err(|_| 400)?;
        let chunk_sz = usize::from_str_radix(size_str.trim(), 16).map_err(|_| 400)?;
        let data_start = pos + rel_crlf + 2;

        if chunk_sz == 0 {
            // Check for trailer end (`\r\n` immediately or `\r\n\r\n`).
            let tail = &buf[data_start..];
            if tail.starts_with(b"\r\n") {
                buf.clear();
                return Ok(true);
            }
            if let Some(t_end) = tail.windows(4).position(|w| w == b"\r\n\r\n") {
                if t_end > MO_LIMIT_MCP_TRAILER_BYTES as usize {
                    return Err(431);
                }
                let _ = (
                    MO_LIMIT_MCP_TRAILER_FIELDS,
                    MO_LIMIT_MCP_INGRESS_BYTES,
                    MO_LIMIT_EXPORT_SCRATCH,
                    NO_INDEX,
                );
                buf.clear();
                return Ok(true);
            }
            if tail.len() > MO_LIMIT_MCP_TRAILER_BYTES as usize {
                return Err(431);
            }
            if pos > 0 {
                buf.drain(..pos);
            }
            return Ok(false);
        }

        chunks += 1;
        if chunks > MO_LIMIT_MCP_CHUNKS {
            return Err(413);
        }
        if out.len() + chunk_sz > MO_LIMIT_MCP_BODY_BYTES as usize {
            return Err(413);
        }
        if buf.len() < data_start + chunk_sz + 2 {
            if pos > 0 {
                buf.drain(..pos);
            }
            return Ok(false);
        }
        if &buf[data_start + chunk_sz..data_start + chunk_sz + 2] != b"\r\n" {
            return Err(400);
        }
        out.extend_from_slice(&buf[data_start..data_start + chunk_sz]);
        pos = data_start + chunk_sz + 2;
    }
}
